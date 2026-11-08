import Foundation

// The host tests the real parser without booting or sending input to a guest.
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

@main struct AutopilotTests {
    static func main() throws {
        let valid = try Autopilot.parse("# comment\r\nmac Test VM\nwait 0.5\ncmd space\nkey 28\nstop\n")
        guard valid.count == 5, case .start("Test VM") = valid[0],
              case .wait(.milliseconds(500)) = valid[1], case .chord(" ") = valid[2] else {
            fatalError("valid plan was not parsed")
        }
        for invalid in ["wait -1", "wait NaN", "wait inf", "wait 301", "wait 1e100",
                        "key 65536", "cmd xx", "stop extra", "knob HV_FLUSH 1"] {
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
        print("Autopilot plan checks passed")
    }
}
