import Foundation

/// Stands in for MacPresentation: a session identified by its number.
struct Session: Identifiable {
    let id: Int
}

@main
struct ExternalScreenPlanTests {
    static func main() {
        var plan = ExternalScreenPlan<Session>()
        let monitor = UUID(), second = UUID(), ghost = UUID()
        let names = [monitor: "monitor", second: "second", ghost: "ghost"]
        /// Changes as readable text: "show monitor 1" is the Mac of session 1 going to the monitor.
        func text(_ changes: [ExternalScreenPlan<Session>.Change]) -> [String] {
            changes.map {
                switch $0 {
                case .showMac(let surface, let session): "show \(names[surface]!) \(session.id)"
                case .stopMac(let surface, let session): "stop \(names[surface]!) \(session.id)"
                }
            }
        }

        // A monitor plugged in before any session shows nothing of ours: iOS mirrors the phone.
        precondition(text(plan.connect(monitor)) == [])
        // The session begins: the waiting monitor gets the Mac.
        precondition(text(plan.begin(Session(id: 1))) == ["show monitor 1"])

        // Unplugged while showing the Mac: stop, so the phone gets it back.
        precondition(text(plan.disconnect(monitor)) == ["stop monitor 1"])
        // A screen the plan never saw changes nothing.
        precondition(text(plan.disconnect(ghost)) == [])

        // Plugged in while the session runs: the Mac goes there at once.
        precondition(text(plan.connect(monitor)) == ["show monitor 1"])

        // A stale session ending (an old SessionView going away late) leaves the running one alone.
        precondition(text(plan.end(Session(id: 2))) == [])
        // The session ends: every screen showing the Mac goes back to mirroring...
        precondition(text(plan.end(Session(id: 1))) == ["stop monitor 1"])
        // ...and stays that way for screens plugged in afterwards.
        precondition(text(plan.connect(second)) == [])

        // Two screens showing the Mac: the session ending stops each exactly once.
        do {
            var plan = ExternalScreenPlan<Session>()
            _ = plan.connect(monitor); _ = plan.connect(second)
            _ = plan.begin(Session(id: 1))
            precondition(text(plan.end(Session(id: 1))) == ["stop monitor 1", "stop second 1"])
            precondition(text(plan.end(Session(id: 1))) == [])
        }

        // A second session beginning mid-session re-shows every screen with the new one.
        do {
            var plan = ExternalScreenPlan<Session>()
            _ = plan.connect(monitor); _ = plan.connect(second)
            _ = plan.begin(Session(id: 1))
            precondition(text(plan.begin(Session(id: 2))) == ["show monitor 2", "show second 2"])
            // The first session's late end is stale now; the second still ends normally.
            precondition(text(plan.end(Session(id: 1))) == [])
            precondition(text(plan.end(Session(id: 2))) == ["stop monitor 2", "stop second 2"])
        }

        // With no session there is nothing to show or stop: ending or unplugging changes nothing.
        do {
            var plan = ExternalScreenPlan<Session>()
            precondition(text(plan.end(Session(id: 1))) == [])
            _ = plan.connect(monitor)
            precondition(text(plan.disconnect(monitor)) == [])
            precondition(text(plan.end(Session(id: 1))) == [])
        }

        // Unplugged after the session ended: it was already stopped, so nothing more happens.
        do {
            var plan = ExternalScreenPlan<Session>()
            _ = plan.connect(monitor)
            _ = plan.begin(Session(id: 1))
            _ = plan.end(Session(id: 1))
            precondition(text(plan.disconnect(monitor)) == [])
        }

        print("External screen plan: all checks passed")
    }
}
