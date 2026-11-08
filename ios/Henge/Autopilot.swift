import Foundation

/// Testing a build on a phone nobody is holding. Henge follows Documents/henge-autopilot.txt when it is
/// launched with HENGE_AUTOPILOT set, as in
///   xcrun devicectl device process launch -e '{"HENGE_AUTOPILOT":"1"}' --device ID BUNDLE
/// or, for a launch by hand (a locked phone driven through iPhone Mirroring), once: at launch
/// Documents/henge-autopilot-once.txt is renamed to henge-autopilot.txt and followed, so the next
/// launch runs nothing. Put the files there with devicectl device copy to. One command per line,
/// picking up lines added later while it runs:
///   mac [NAME]         start that Mac (the one used last without a name) once JIT is on
///   wait SECONDS
///   type TEXT          typed into the Mac (the log shows only how long it is)
///   key CODE           a Linux key code (28 is Return, 1 is Escape)
///   cmd KEY            KEY (a character, or space) with Command held
///   knob NAME VALUE    a translation knob (core/machine.h)
///   log TEXT           a marker in henge.log
///   stop               stop the Mac
///   exit               quit Henge
/// Each command is written to henge.log as it runs, so the log has the timeline.
enum Autopilot {
    private static let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]

    /// Launched for testing: by environment, or by a one-time file (used up here).
    static let enabled: Bool = {
        if ProcessInfo.processInfo.environment["HENGE_AUTOPILOT"] != nil { return true }
        let once = docs.appendingPathComponent("henge-autopilot-once.txt"), plan = docs.appendingPathComponent("henge-autopilot.txt")
        guard FileManager.default.fileExists(atPath: once.path) else { return false }
        try? FileManager.default.removeItem(at: plan)
        return (try? FileManager.default.moveItem(at: once, to: plan)) != nil
    }()

    @MainActor static func run(start: @escaping (VirtualMac) -> Void) async {
        guard enabled else { return }
        let url = docs.appendingPathComponent("henge-autopilot.txt")
        HengeLog.log("autopilot", "following \(url.lastPathComponent)")
        var done = 0 // lines already run
        while !Task.isCancelled {
            let text = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
            var lines = text.components(separatedBy: "\n")
            lines.removeLast() // only whole lines: the last piece has no newline yet
            if lines.count < done { done = 0 } // a new file
            while done < lines.count, !Task.isCancelled {
                let line = lines[done].trimmingCharacters(in: .whitespaces)
                done += 1
                if line.isEmpty || line.hasPrefix("#") { continue }
                /* What is typed may be a password: the log gets its length only. */
                HengeLog.log("autopilot", line.hasPrefix("type ") ? "type (\(line.count - 5) characters)" : line)
                await perform(line, start: start)
            }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
    }

    @MainActor private static func perform(_ line: String, start: (VirtualMac) -> Void) async {
        let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
        let arg = parts.count > 1 ? parts[1] : ""
        switch parts[0] {
        case "mac":
            for _ in 0..<240 where !JITBootstrap.ready { try? await Task.sleep(nanoseconds: 500_000_000) }
            guard JITBootstrap.ready else { HengeLog.log("autopilot", "JIT is not on; not starting a Mac"); return }
            let all: [VirtualMac] = Machines.shared.all
            var mac: VirtualMac?
            if arg.isEmpty {
                mac = all.max { a, b in (a.lastUsed ?? a.added) < (b.lastUsed ?? b.added) }
            } else {
                mac = all.first { $0.name.lowercased() == arg.lowercased() }
            }
            guard let mac else {
                HengeLog.log("autopilot", "no Mac called \(arg); there are \(all.map(\.name))")
                return
            }
            start(mac)
        case "wait":
            try? await Task.sleep(nanoseconds: UInt64((Double(arg) ?? 0) * 1e9))
        case "type":
            await Keys.typeSlowly(arg)
        case "key":
            if let code = UInt16(arg) { Keys.press(code) }
        case "cmd": // "cmd space", "cmd q": the key with Command held
            if let c = arg == "space" ? " " : arg.first { Keys.chord(125, c) }
        case "knob":
            let kv = arg.split(separator: " ")
            if kv.count == 2, let value = Int32(kv[1]) {
                let found = henge_vm_knob(String(kv[0]), value)
                HengeLog.log("autopilot", found != 0 ? "knob \(kv[0]) = \(value)" : "no knob \(kv[0]) (or no Mac running)")
            }
        case "log": // already in the log; the wall clock too, to line it up with things measured elsewhere
            HengeLog.log("autopilot", String(format: "at %.3f", Date().timeIntervalSince1970))
        case "stop":
            henge_vm_stop()
        case "exit":
            HengeLog.log("autopilot", "quitting")
            try? await Task.sleep(nanoseconds: 300_000_000)
            exit(0)
        default:
            HengeLog.log("autopilot", "unknown command")
        }
    }
}
