// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation
import Network
import SwiftUI
import UIKit

/// How Henge gets a debugger attached. See docs/06-built-in-jit.md.
enum JITMethod: String, CaseIterable, Identifiable {
    /// TrollStore or jailbreak first, then a paired built-in helper or StikDebug.
    case automatic
    case stikDebug
    case trollStore
    case jailbreak
    case builtIn

    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .stikDebug: return "StikDebug"
        case .trollStore: return "TrollStore"
        case .jailbreak: return "Jailbreak (Dopamine)"
        case .builtIn: return "Built-in StikJIT"
        }
    }
}

/// Where Built-in StikJIT's pairing file came from.
enum JITPairingSource: String {
    /// Made by Henge on this device (iOS 27, OnDevicePairing).
    case onDevice
    /// Imported from a file made on a computer.
    case imported
}

/// The device credential for Built-in StikJIT. Keep it out of Files, Finder and backups.
enum JITPairingFileStore {
    private static var directory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
    }
    private static var url: URL { directory.appendingPathComponent("pairingFile.plist") }
    private static var legacyURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
            .appendingPathComponent("pairingFile.plist")
    }
    private static let sourceKey = "henge.jitPairingSource"

    static var source: JITPairingSource? {
        try? migrate()
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        return UserDefaults.standard.string(forKey: sourceKey).flatMap(JITPairingSource.init(rawValue:)) ?? .imported
    }

    static func data() throws -> Data {
        try migrate()
        return try Data(contentsOf: url)
    }

    static func importFile(from source: URL) throws {
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        try store(try Data(contentsOf: source), source: .imported)
    }

    /// Validates an RPPairing plist and makes it the pairing file.
    static func store(_ data: Data, source: JITPairingSource) throws {
        try writePrivate(data)
        UserDefaults.standard.set(source.rawValue, forKey: sourceKey)
        removeLegacy()
    }

    private static func migrate() throws {
        if FileManager.default.fileExists(atPath: url.path) {
            removeLegacy() // Retry if a prior launch stopped after writing the private copy.
            return
        }
        let old = legacyURL
        guard FileManager.default.fileExists(atPath: old.path) else { return }
        try writePrivate(readLegacy(old))
        removeLegacy()
    }

    private static let maximumPairingBytes = 1024 * 1024

    private static func readLegacy(_ old: URL) throws -> Data {
        let parent = try old.deletingLastPathComponent().resourceValues(forKeys: [.isSymbolicLinkKey])
        let file = try old.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey])
        guard parent.isSymbolicLink != true, file.isRegularFile == true, file.isSymbolicLink != true,
              let size = file.fileSize, size > 0, size <= maximumPairingBytes else {
            throw CocoaError(.fileReadNoPermission)
        }
        return try Data(contentsOf: old)
    }

    private static func removeLegacy() {
        let old = legacyURL
        guard let data = try? readLegacy(old), isPairingFile(data) else { return }
        // This may only unlink a file, even if the legacy name changes during cleanup.
        _ = unlink(old.path)
    }

    private static func isPairingFile(_ data: Data) -> Bool {
        guard !data.isEmpty, data.count <= maximumPairingBytes,
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
              let dictionary = plist as? [String: Any],
              let publicKey = dictionary["public_key"] as? Data, publicKey.count == 32,
              let privateKey = dictionary["private_key"] as? Data, privateKey.count == 32,
              let identifier = dictionary["identifier"] as? String, !identifier.isEmpty else { return false }
        return true
    }

    private static func writePrivate(_ data: Data) throws {
        guard isPairingFile(data) else {
            throw NSError(domain: "HengeJIT", code: 10,
                          userInfo: [NSLocalizedDescriptionKey:
                            "That is not a remote pairing file. Make one with the StikDebug pairing-file guide and try again."])
        }
        var folder = directory
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try folder.setResourceValues(values)
        try data.write(to: url, options: [.atomic, .completeFileProtection])
    }
}

@MainActor
final class JITCoordinator: ObservableObject {
    static let shared = JITCoordinator()

    @Published var method: JITMethod {
        didSet { UserDefaults.standard.set(method.rawValue, forKey: "henge.jitMethod") }
    }
    @Published var showSetup = false
    @Published private(set) var connectionProblem: JITConnectionProblem?
    /// What to tell the user, and which buttons to offer, for the last connection problem.
    @Published private(set) var advice: JITAdvice?
    @Published private(set) var busy = false
    @Published private(set) var status: String?
    @Published private(set) var error: String?
    @Published private(set) var txmPresent: Bool?
    /// The setup check passed for the current pairing file.
    @Published private(set) var prepared = false
    @Published private(set) var pairingSource = JITPairingFileStore.source
    /// Bumped each time the built-in helper attaches. Henge stays in the
    /// foreground for that, so nothing else tells ContentView to claim the region.
    @Published private(set) var attachGeneration = 0

    var hasPairing: Bool { pairingSource != nil }

    /// The network path right now. The helper reaches this device through LocalDevVPN, but iOS may send that
    /// connection over cellular instead; knowing which network is in use decides the advice and the retries.
    private let pathMonitor = NWPathMonitor()

    var networkSituation: NetworkSituation {
        let path = pathMonitor.currentPath
        return NetworkSituation(NetworkPathSnapshot(
            isSatisfied: path.status == .satisfied,
            usesWiFi: path.usesInterfaceType(.wifi),
            usesCellular: path.usesInterfaceType(.cellular),
            usesWired: path.usesInterfaceType(.wiredEthernet)))
    }

    private init() {
        method = UserDefaults.standard.string(forKey: "henge.jitMethod")
            .flatMap(JITMethod.init(rawValue:)) ?? .automatic
        pathMonitor.start(queue: DispatchQueue(label: "com.henge.app.jit-path", qos: .utility))
    }

    var resolvedMethod: JITMethod {
        guard method == .automatic else { return method }
        if JITBootstrap.isTrollStoreInstalled { return .trollStore }
        if JITBootstrap.isJailbroken { return .jailbreak }
        if hasPairing && HengeBuiltInJIT.isAvailable { return .builtIn }
        if JITBootstrap.isStikDebugInstalled { return .stikDebug }
        return .builtIn
    }

    var automaticDescription: String {
        switch resolvedMethod {
        case .stikDebug: return "StikDebug is installed, so Henge will open it."
        case .trollStore: return "TrollStore is installed, so Henge will ask it to enable JIT."
        case .jailbreak: return "This device is jailbroken, so JIT comes from the jailbreak's Allow JIT in Apps setting."
        default: return hasPairing
            ? "Henge will use its paired built-in helper."
            : "StikDebug, TrollStore and a jailbreak were not found, so Henge will use its built-in helper."
        }
    }

    func refreshPairingStatus() {
        pairingSource = JITPairingFileStore.source
    }

    private func log(_ line: String) { HengeLog.log("jit", line) }

    /// Get a debugger attached with whichever method applies, or open the setup
    /// walkthrough when that method is not set up yet.
    /// "Turn on JIT at launch": when Henge opens without JIT, ask the built-in StikJIT for it straight away, once per launch.
    /// Only when StikJIT can do it unattended -- it is available here and already paired -- so opening Henge never lands in
    /// the setup walkthrough by surprise.
    static let autoEnableKey = "henge.jit.autoEnable"
    private var autoTried = false

    /// A Henge TrollStore installed asks TrollStore for JIT each time it opens without it (#135), unless turned off in Settings:
    /// TrollStore attaches to the running Henge for a moment, with nothing to tap.
    static let trollStoreAutoKey = "henge.jit.trollStoreAuto"
    static var trollStoreAuto: Bool { UserDefaults.standard.object(forKey: trollStoreAutoKey) as? Bool ?? true }

    /// JIT at launch opened LocalDevVPN to connect it; the next return to the foreground goes on with the built-in helper.
    private var awaitingVPN = false

    func enableAtLaunchIfAsked() {
        if awaitingVPN {
            awaitingVPN = false
            guard !JITBootstrap.ready else { return }
            log("back from LocalDevVPN (\(LocalDevVPN.isConnected ? "connected" : "not connected")): turning JIT on with built-in StikJIT")
            method = .builtIn
            enable()
            return
        }
        if !autoTried, JITBootstrap.isInstalledWithTrollStore, Self.trollStoreAuto {
            autoTried = true
            guard !JITBootstrap.ready else { return }
            log("installed by TrollStore: asking it for JIT at launch")
            method = .trollStore
            enable()
            return
        }
        guard !autoTried, UserDefaults.standard.bool(forKey: Self.autoEnableKey) else { return }
        autoTried = true
        guard !JITBootstrap.ready else { return }
        guard HengeBuiltInJIT.isAvailable, hasPairing else {
            log("JIT at launch is on, but built-in StikJIT is not available or not paired; skipped")
            return
        }
        // The helper reaches this device through LocalDevVPN. When it is not connected, connect it first: LocalDevVPN
        // connects and comes back (henge://), and the return to the foreground goes on above.
        if LocalDevVPN.isInstalled, !LocalDevVPN.isConnected {
            log("JIT at launch: LocalDevVPN is not connected; opening it to connect")
            awaitingVPN = true
            LocalDevVPN.open()
            return
        }
        log("turning JIT on at launch with built-in StikJIT")
        method = .builtIn
        enable()
    }

    func enable() {
        guard !JITBootstrap.ready else { return }
        error = nil
        connectionProblem = nil
        advice = nil
        switch resolvedMethod {
        case .automatic:
            assertionFailure("Automatic must resolve to a concrete JIT method")
        case .stikDebug:
            // requestAttach() calls open() on stikjit:// and cannot report failure
            // synchronously: an unclaimed scheme fails open() silently. Wait for
            // CS_DEBUGGED so a missing StikDebug still ends in a clear message.
            if JITBootstrap.requestAttach() {
                awaitStikDebug()
            } else if JITBootstrap.requestTrollStoreAttach() {
                awaitTrollStore()
            } else {
                error = "StikDebug is not installed. Install it, or set up Built-in StikJIT."
                showSetup = true
            }
        case .trollStore:
            if JITBootstrap.requestTrollStoreAttach() {
                awaitTrollStore()
            } else {
                error = "TrollStore could not be opened. Install Henge through TrollStore, or choose another method."
                showSetup = true
            }
        case .jailbreak:
            // Nothing to ask: a jailbreak marks an app as debugged as it opens, if it has been told to. Say what to turn on.
            error = "Turn on Allow JIT in Apps in Dopamine's settings, then open Henge again."
            showSetup = true
        case .builtIn:
            guard HengeBuiltInJIT.isAvailable, hasPairing else {
                log("built-in JIT is not set up; opening the walkthrough")
                showSetup = true
                return
            }
            enableBuiltIn()
        }
    }

    /// After TrollStore is asked: it opens Henge again and attaches to it for a moment, which marks the process as debugged. Watch for
    /// that, so the Games tab and the Library notice without waiting for another trip to the foreground, and say what to check if
    /// it never comes -- TrollStore only answers enable-jit once its URL Scheme setting is on.
    private func awaitTrollStore() {
        busy = true
        status = "Waiting for TrollStore to enable JIT…"
        log("waiting for TrollStore's attach")
        _ = Self.waitForDebugger(timeout: 60) { [weak self] attached in
            guard let self else { return }
            busy = false
            if attached {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("TrollStore enabled JIT")
            } else {
                status = nil
                error = "TrollStore did not enable JIT. In TrollStore's Settings turn on URL Scheme, and make sure Henge was "
                      + "installed through TrollStore (or is signed with get-task-allow)."
                log("TrollStore did not enable JIT within a minute")
                showSetup = true
            }
        }
    }

    /// After StikDebug is asked to attach: open() cannot say whether it found StikDebug,
    /// so watch CS_DEBUGGED the same way awaitTrollStore does.
    private func awaitStikDebug() {
        busy = true
        status = "Waiting for StikDebug to attach…"
        log("waiting for StikDebug's attach")
        _ = Self.waitForDebugger(timeout: 60) { [weak self] attached in
            guard let self else { return }
            busy = false
            if attached {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("StikDebug attached")
            } else {
                status = nil
                error = "StikDebug did not attach within a minute. Make sure it's installed and try again."
                log("StikDebug did not attach within a minute")
                showSetup = true
            }
        }
    }

    func importPairingFile(_ source: URL) {
        do {
            try JITPairingFileStore.importFile(from: source)
            OnDevicePairing.shared.cancel()
            refreshPairingStatus()
            method = .builtIn
            prepared = false
            status = "Pairing file imported."
            error = nil
            log("pairing file imported")
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// OnDevicePairing finished: its file becomes the pairing file and Built-in StikJIT the method.
    func storeOnDevicePairing(_ data: Data) throws {
        try JITPairingFileStore.store(data, source: .onDevice)
        refreshPairingStatus()
        method = .builtIn
        prepared = false
        status = "Paired on this device."
        error = nil
    }

    /// Check LocalDevVPN and the Developer Disk Image, mounting it when needed. `attempt` counts tries; see JITRetryPolicy.
    func prepareBuiltIn(attempt: Int = 1) {
        guard let pairing = builtInPairing() else { return }
        busy = true
        error = nil
        connectionProblem = nil
        advice = nil
        if attempt == 1 { status = "Checking LocalDevVPN and the Developer Disk Image…" }
        RemotePairingPortDiscovery.resolve { port in
            HengeBuiltInJIT.send(.prepare(pairingData: pairing, rsdPort: port)) { [weak self] result in
                guard let self else { return }
                switch result {
                case .success(let response):
                    log("setup check \(response.success ? "passed" : "failed"): \(response.message)")
                    if !response.success,
                       retryIfAdvised(helperMessage: response.message, attempt: attempt, retry: { [weak self] in self?.prepareBuiltIn(attempt: $0) }) {
                        return
                    }
                    busy = false
                    txmPresent = response.txmPresent
                    prepared = response.success
                    status = response.success ? response.message : nil
                    error = response.success ? nil : helperFailure(response.message)
                case .failure(let failure):
                    busy = false
                    prepared = false
                    status = nil
                    error = failure.localizedDescription
                }
            }
        }
    }

    func enableBuiltIn(attempt: Int = 1) {
        guard let pairing = builtInPairing() else { return }
        guard let script = JITBootstrap.scriptBase64 else {
            error = "Henge's JIT script is missing from this installation. Reinstall Henge."
            return
        }

        busy = true
        error = nil
        connectionProblem = nil
        advice = nil
        if attempt == 1 { status = "Starting Henge's JIT helper…" }
        log("enabling JIT with the built-in helper" + (attempt > 1 ? " (attempt \(attempt))" : ""))
        var readiness: Timer?
        var finished = false
        let finish: (String?) -> Void = { [weak self] failure in
            guard let self, !finished else { return }
            finished = true
            readiness?.invalidate()
            busy = false
            if let failure {
                status = nil
                error = failure
                log("built-in JIT failed: \(failure)")
                showSetup = true
            } else {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("built-in helper attached")
            }
        }

        RemotePairingPortDiscovery.resolve { port in
            HengeBuiltInJIT.send(
                .enable(targetPID: getpid(), pairingData: pairing, scriptBase64: script, rsdPort: port),
                started: { [weak self] in
                    if attempt == 1 { self?.status = "Waiting for the helper to attach…" }
                    readiness = Self.waitForDebugger { attached in
                        finish(attached ? nil : "The helper did not attach within 90 seconds. "
                                              + "Check that LocalDevVPN is connected, then try again.")
                    }
                },
                completion: { [weak self] result in
                    // The helper replies once Henge detaches, or when attaching failed.
                    switch result {
                    case .success(let response):
                        self?.txmPresent = response.txmPresent
                        HengeLog.log("jit", "helper: \(response.message)")
                        if !response.success {
                            guard let self else { return }
                            // Not finish(): a retry keeps the screen busy and the next attempt starts its own watch.
                            if !finished,
                               retryIfAdvised(helperMessage: response.message, attempt: attempt,
                                              retry: { [weak self] in self?.enableBuiltIn(attempt: $0) }) {
                                finished = true
                                readiness?.invalidate()
                                return
                            }
                            finish(helperFailure(response.message))
                        }
                    case .failure(let failure):
                        finish(failure.localizedDescription)
                    }
                })
        }
    }

    func resetDDI() {
        busy = true
        error = nil
        status = "Resetting the Developer Disk Image cache…"
        HengeBuiltInJIT.send(.resetDDI) { [weak self] result in
            guard let self else { return }
            busy = false
            prepared = false
            switch result {
            case .success(let response):
                status = response.success ? response.message : nil
                error = response.success ? nil : response.message
            case .failure(let failure):
                status = nil
                error = failure.localizedDescription
            }
        }
    }

    private func builtInPairing() -> Data? {
        if let reason = HengeBuiltInJIT.unavailableReason {
            error = reason
            return nil
        }
        guard let pairing = try? JITPairingFileStore.data() else {
            error = "Pair this device or import its pairing file first."
            return nil
        }
        return pairing
    }

    /// The helper's failure as shown: a connection problem gets its plain
    /// explanation for the network in use (the helper's own message is already in the log).
    private func helperFailure(_ message: String) -> String {
        connectionProblem = JITConnectionProblem(helperMessage: message)
        advice = JITNetworkAdvice.advice(forHelperMessage: message, situation: networkSituation)
        return advice?.message ?? message
    }

    /// When the policy says the failure is worth another try, say so, wait, and call `retry` with the next attempt number.
    /// Returns false when it gives up, and the caller shows the advice instead.
    private func retryIfAdvised(helperMessage: String, attempt: Int, retry: @escaping (Int) -> Void) -> Bool {
        guard let problem = JITConnectionProblem(helperMessage: helperMessage),
              case .retry(let wait) = JITRetryPolicy.decision(situation: networkSituation, problem: problem, attempt: attempt)
        else { return false }
        status = JITRetryPolicy.statusText(nextAttempt: attempt + 1)
        log("helper could not connect (attempt \(attempt)); retrying in \(Int(wait))s")
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(wait * 1_000_000_000))
            guard self != nil else { return }
            retry(attempt + 1)
        }
        return true
    }

    /// Polls CS_DEBUGGED until it is set or `timeout` passes.
    private static func waitForDebugger(timeout: TimeInterval = 90,
                                        completion: @escaping (Bool) -> Void) -> Timer {
        let deadline = Date().addingTimeInterval(timeout)
        let timer = Timer(timeInterval: 0.5, repeats: true) { timer in
            if JITBootstrap.debuggedFlag {
                timer.invalidate()
                completion(true)
            } else if Date() >= deadline {
                timer.invalidate()
                completion(false)
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }
}

/// LocalDevVPN, which Built-in StikJIT and StikDebug reach the device through:
/// open it to connect when it is installed, otherwise its App Store page.
enum LocalDevVPN {
    static let appStore = URL(string: "https://apps.apple.com/us/app/localdevvpn/id6755608044")!
    /// `enable` connects the VPN; `scheme` has LocalDevVPN return to Henge afterwards.
    static let connect = URL(string: "localdevvpn://enable?scheme=henge")!

    static var isInstalled: Bool { UIApplication.shared.canOpenURL(URL(string: "localdevvpn://")!) }

    /// Whether its tunnel is up: a tunnel interface that is up with an address in 10.7.0.0/16 (LocalDevVPN's own is
    /// 10.7.1.1), the network it routes to reach this device at 10.7.0.1, StikJIT's default device address.
    static var isConnected: Bool {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return false }
        defer { freeifaddrs(list) }
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = p.pointee
            guard (ifa.ifa_flags & UInt32(IFF_UP)) != 0, let sa = ifa.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            let ip = sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { UInt32(bigEndian: $0.pointee.sin_addr.s_addr) }
            if ip >> 16 == 0x0a07, String(cString: ifa.ifa_name).hasPrefix("utun") {
                HengeLog.log("jit", "LocalDevVPN tunnel on \(String(cString: ifa.ifa_name))")
                return true
            }
        }
        return false
    }
    static var actionTitle: String { isInstalled ? "Connect LocalDevVPN" : "Get LocalDevVPN" }

    @MainActor static func open() {
        let installed = isInstalled
        HengeLog.log("jit", "LocalDevVPN \(installed ? "connect" : "app-store")")
        UIApplication.shared.open(installed ? connect : appStore)
    }
}

/// The device advertises RemotePairing on a port it picks at boot, while
/// StikJIT defaults to the old fixed 49152. Find the advertised port so
/// Built-in StikJIT keeps working after the device restarts.
enum RemotePairingPortDiscovery {
    private static let serviceType = "_remotepairing._tcp"
    private static let queue = DispatchQueue(label: "com.henge.app.remotepairing-discovery", qos: .utility)
    private static let lock = NSLock()
    private static var cachedPort: UInt16?
    private static var lastFailure: Date?

    /// Calls back on the main thread. nil means the helper should use StikJIT's
    /// default.
    static func resolve(timeout: TimeInterval = 3, completion: @escaping (UInt16?) -> Void) {
        lock.lock()
        if let cachedPort {
            lock.unlock()
            DispatchQueue.main.async { completion(cachedPort) }
            return
        }
        if let lastFailure, Date().timeIntervalSince(lastFailure) < 30 {
            lock.unlock()
            DispatchQueue.main.async { completion(nil) }
            return
        }
        lock.unlock()

        queue.async {
            let port = discover(timeout: timeout)
            lock.lock()
            if let port {
                cachedPort = port
                lastFailure = nil
            } else {
                lastFailure = Date()
            }
            lock.unlock()
            HengeLog.log("jit", "RemotePairing port "
                        + (port.map { String($0) } ?? "not found; using StikJIT's default"))
            DispatchQueue.main.async { completion(port) }
        }
    }

    private static func discover(timeout: TimeInterval) -> UInt16? {
        let parameters = NWParameters.tcp
        parameters.includePeerToPeer = true
        let browser = NWBrowser(for: .bonjour(type: serviceType, domain: nil), using: parameters)

        let semaphore = DispatchSemaphore(value: 0)
        let resultLock = NSLock()
        var finished = false
        var resolvedPort: UInt16?
        var pendingConnection: NWConnection?

        func finish(_ port: UInt16?) {
            resultLock.lock()
            if !finished {
                finished = true
                resolvedPort = port
                semaphore.signal()
            }
            resultLock.unlock()
        }

        func hasFinished() -> Bool {
            resultLock.lock()
            defer { resultLock.unlock() }
            return finished
        }

        browser.browseResultsChangedHandler = { results, _ in
            guard !hasFinished() else { return }

            let preferred = results.first { result in
                result.interfaces.contains { $0.name == "lo0" }
            } ?? results.first
            guard let result = preferred, case .service = result.endpoint else { return }

            resultLock.lock()
            let alreadyConnecting = pendingConnection != nil
            if !alreadyConnecting {
                pendingConnection = NWConnection(to: result.endpoint, using: parameters)
            }
            let connection = pendingConnection
            resultLock.unlock()

            guard !alreadyConnecting, let connection else { return }

            connection.stateUpdateHandler = { state in
                switch state {
                case .ready:
                    if let remoteEndpoint = connection.currentPath?.remoteEndpoint,
                       case .hostPort(_, let port) = remoteEndpoint {
                        finish(port.rawValue)
                    } else {
                        finish(nil)
                    }
                case .failed, .cancelled:
                    finish(nil)
                default:
                    break
                }
            }
            connection.start(queue: queue)
        }

        browser.stateUpdateHandler = { state in
            switch state {
            case .failed, .cancelled:
                finish(nil)
            default:
                break
            }
        }

        browser.start(queue: queue)
        _ = semaphore.wait(timeout: .now() + timeout)
        browser.cancel()

        resultLock.lock()
        let connectionToCancel = pendingConnection
        resultLock.unlock()
        connectionToCancel?.cancel()

        resultLock.lock()
        defer { resultLock.unlock() }
        return resolvedPort
    }
}

