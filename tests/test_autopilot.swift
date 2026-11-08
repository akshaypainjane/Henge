import Foundation

// Exercise the real parser and click sequencing without a guest.
struct VirtualMac {
    var name = ""
    var added = Date()
    var lastUsed: Date?
}

enum Machines {
    struct Store { let all: [VirtualMac] = [] }
    static let shared = Store()
}

enum JITBootstrap { static let ready = false }

enum Keys {
    static func typeSlowly(_ value: String) async {}
    static func press(_ key: UInt16) {}
    static func chord(_ modifier: UInt16, _ key: Character) {}
}

enum HengeLog { static func log(_ category: String, _ message: String) {} }
func henge_vm_stop() {}
func henge_vm_pointer(_ x: inout Int32, _ y: inout Int32) -> Int32 { -1 }

@MainActor enum PointerFixture {
    static var steps: [Int32] = []
    static var events: [String] = []
}

@MainActor func henge_vm_pointer_step(_ x: Int32, _ y: Int32) -> Int32 {
    precondition(x == 40 && y == 60)
    PointerFixture.events.append("step")
    return PointerFixture.steps.removeFirst()
}

@MainActor func henge_vm_mouse_button(_ button: Int32, _ down: Int32) {
    precondition(button == 0)
    PointerFixture.events.append(down == 1 ? "down" : "up")
}

enum MacPreview {
    static func saveAutomationSnapshot(named name: String, for mac: VirtualMac) async throws -> URL {
        fatalError("Snapshot saves are covered by the iOS app")
    }
}

@main struct AutopilotTests {
    @MainActor static func main() async throws {
        let valid = try Autopilot.parse("# comment\r\nmac Test VM\nwait 0.5\ncmd space\nkey 28\nstop\n")
        guard valid.count == 5, case .start("Test VM") = valid[0],
              case .wait(.milliseconds(500)) = valid[1], case .chord(" ") = valid[2] else {
            fatalError("valid plan was not parsed")
        }
        let input = try Autopilot.parse("pointer\nmove 40 60\nclick 40 60\nsnap\nsnap TextEdit_2-verify\n")
        guard input.count == 5, case .pointer = input[0], case .move(40, 60, false) = input[1],
              case .move(40, 60, true) = input[2], case .snap("snap") = input[3],
              case .snap("TextEdit_2-verify") = input[4] else {
            fatalError("valid pointer and snapshot commands were not parsed")
        }
        for invalid in ["wait -1", "wait NaN", "wait inf", "wait 301", "wait 1e100",
                        "key 65536", "cmd xx", "stop extra", "knob HV_FLUSH 1", "pointer extra",
                        "click 40 bad 60", "move -1 60", "click 65536 60", "move 40",
                        "snap ../escape", "snap /absolute", "snap multiple words",
                        "snap " + String(repeating: "a", count: 65)] {
            do {
                _ = try Autopilot.parse("key 28\n" + invalid)
                fatalError("invalid plan was accepted")
            } catch let error as Autopilot.InvalidPlan {
                precondition(error.line == 2)
            }
        }
        let maximum = String(repeating: "key 28\n", count: 256)
        let commands = try Autopilot.parse(maximum)
        precondition(commands.count == 256)
        do {
            _ = try Autopilot.parse(maximum + "exit")
            fatalError("overlong plan was accepted")
        } catch let error as Autopilot.InvalidPlan {
            precondition(error.line == 257)
        }
        PointerFixture.steps = [0, -1]
        do {
            try await Autopilot.aim(x: 40, y: 60, click: true)
            fatalError("click reported success without a pointer")
        } catch Autopilot.ExecutionError.pointerUnavailable {
            precondition(PointerFixture.events == ["step", "step"])
        }

        PointerFixture.events = []
        PointerFixture.steps = [0, 1]
        try await Autopilot.aim(x: 40, y: 60, click: true)
        precondition(PointerFixture.events == ["step", "step", "down", "up"])

        PointerFixture.events = []
        PointerFixture.steps = [1]
        try await Autopilot.aim(x: 40, y: 60, click: false)
        precondition(PointerFixture.events == ["step"])

        PointerFixture.events = []
        PointerFixture.steps = [1]
        let click = Task { try await Autopilot.aim(x: 40, y: 60, click: true) }
        while !PointerFixture.events.contains("down") { await Task.yield() }
        click.cancel()
        do {
            try await click.value
            fatalError("cancelled click completed")
        } catch is CancellationError {
            precondition(PointerFixture.events == ["step", "down", "up"])
        }
        print("Autopilot plan and click checks passed")
    }
}
