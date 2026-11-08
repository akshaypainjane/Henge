#if DEBUG
import Foundation

/// A debug launch with HENGE_AUTOPILOT=1 may read one plan from
/// Library/Caches/henge-autopilot.txt. Place the complete plan before launching.
enum Autopilot {
    private static let maxBytes = 64 * 1024

    enum Command {
        case start(String)
        case wait(Duration)
        case type(String)
        case key(UInt16)
        case chord(Character)
        case pointer
        case move(x: Int32, y: Int32, click: Bool)
        case snap(String)
        case mark
        case stop
        case exit
    }

    struct InvalidPlan: LocalizedError {
        let line: Int
        var errorDescription: String? { "Invalid automation command on line \(line)." }
    }

    enum ExecutionError: LocalizedError {
        case pointerUnavailable
        case targetUnreached(Int32, Int32)
        case noSnapshotGuest

        var errorDescription: String? {
            switch self {
            case .pointerUnavailable: "The guest pointer is unavailable."
            case .targetUnreached(let x, let y): "The guest pointer did not reach (\(x), \(y))."
            case .noSnapshotGuest: "Start a guest with mac before taking a snapshot."
            }
        }
    }

    static func parse(_ text: String) throws -> [Command] {
        var commands: [Command] = []
        for (index, raw) in text.split(omittingEmptySubsequences: false, whereSeparator: \.isNewline).enumerated() {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let parts = line.split(maxSplits: 1, whereSeparator: \.isWhitespace)
            guard let verb = parts.first else { continue }
            let argument = parts.count == 2 ? String(parts[1]) : ""
            let invalid = InvalidPlan(line: index + 1)
            guard commands.count < 256 else { throw invalid }
            switch verb {
            case "mac": commands.append(.start(argument))
            case "wait":
                guard let seconds = Double(argument), seconds.isFinite, (0...300).contains(seconds) else { throw invalid }
                commands.append(.wait(.seconds(seconds)))
            case "type" where !argument.isEmpty: commands.append(.type(argument))
            case "key":
                guard let key = UInt16(argument) else { throw invalid }
                commands.append(.key(key))
            case "cmd":
                let key = argument == "space" ? " " : argument
                guard key.count == 1, let character = key.first else { throw invalid }
                commands.append(.chord(character))
            case "pointer" where argument.isEmpty: commands.append(.pointer)
            case "move", "click":
                let xy = argument.split(whereSeparator: \.isWhitespace)
                guard xy.count == 2, let x = UInt16(xy[0]), let y = UInt16(xy[1]) else { throw invalid }
                commands.append(.move(x: Int32(x), y: Int32(y), click: verb == "click"))
            case "snap":
                let name = argument.isEmpty ? "snap" : argument
                guard name.utf8.count <= 64, name.allSatisfy({
                    $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_" || $0 == "-")
                }) else { throw invalid }
                commands.append(.snap(name))
            case "log" where argument.isEmpty: commands.append(.mark)
            case "stop" where argument.isEmpty: commands.append(.stop)
            case "exit" where argument.isEmpty: commands.append(.exit)
            default: throw invalid
            }
        }
        return commands
    }

    @MainActor static func aim(x: Int32, y: Int32, click: Bool) async throws {
        for _ in 0..<1500 {
            try Task.checkCancellation()
            let step = henge_vm_pointer_step(x, y)
            if step < 0 { throw ExecutionError.pointerUnavailable }
            if step > 0 {
                if click {
                    henge_vm_mouse_button(0, 1)
                    defer { henge_vm_mouse_button(0, 0) }
                    try await Task.sleep(for: .milliseconds(120))
                }
                return
            }
            try await Task.sleep(for: .milliseconds(30))
        }
        throw ExecutionError.targetUnreached(x, y)
    }

    @MainActor static func run(start: (VirtualMac) -> Void) async {
        guard ProcessInfo.processInfo.environment["HENGE_AUTOPILOT"] == "1" else { return }
        let path = URL.cachesDirectory.appendingPathComponent("henge-autopilot.txt")
        do {
            let commands = try await Task.detached(priority: .utility) {
                let claimed = path.deletingLastPathComponent().appendingPathComponent(UUID().uuidString)
                try FileManager.default.moveItem(at: path, to: claimed)
                defer { try? FileManager.default.removeItem(at: claimed) }
                let file = try FileHandle(forReadingFrom: claimed)
                defer { try? file.close() }
                guard let bytes = try file.read(upToCount: maxBytes + 1),
                      bytes.count <= maxBytes, let text = String(data: bytes, encoding: .utf8) else {
                    throw CocoaError(.fileReadCorruptFile)
                }
                return try parse(text)
            }.value
            var selectedMac: VirtualMac?
            for command in commands {
                try Task.checkCancellation()
                switch command {
                case .start(let name):
                    for _ in 0..<240 where !JITBootstrap.ready { try await Task.sleep(for: .milliseconds(500)) }
                    let all: [VirtualMac] = Machines.shared.all
                    let mac = name.isEmpty
                        ? all.max { ($0.lastUsed ?? $0.added) < ($1.lastUsed ?? $1.added) }
                        : all.first { $0.name.caseInsensitiveCompare(name) == .orderedSame }
                    guard JITBootstrap.ready, let mac else {
                        HengeLog.log("autopilot", "Guest or JIT unavailable. Stopping plan.")
                        return
                    }
                    selectedMac = mac
                    start(mac)
                case .wait(let duration): try await Task.sleep(for: duration)
                case .type(let value): await Keys.typeSlowly(value)
                case .key(let key): Keys.press(key)
                case .chord(let key): Keys.chord(125, key)
                case .pointer:
                    var x: Int32 = 0, y: Int32 = 0
                    guard henge_vm_pointer(&x, &y) == 0 else { throw ExecutionError.pointerUnavailable }
                    HengeLog.log("autopilot", "Pointer at (\(x), \(y))")
                case .move(let x, let y, let click):
                    try await aim(x: x, y: y, click: click)
                    HengeLog.log("autopilot", "\(click ? "Clicked" : "Moved") at (\(x), \(y))")
                case .snap(let name):
                    guard let mac = selectedMac else { throw ExecutionError.noSnapshotGuest }
                    let url = try await MacPreview.saveAutomationSnapshot(named: name, for: mac)
                    HengeLog.log("autopilot", "Saved \(url.lastPathComponent)")
                case .mark: HengeLog.log("autopilot", "Marker at \(Date().timeIntervalSince1970)")
                case .stop: henge_vm_stop()
                case .exit: exit(0)
                }
            }
        } catch is CancellationError {
            return
        } catch {
            HengeLog.log("autopilot", "Plan stopped. \(error.localizedDescription)")
        }
    }
}
#endif
