import Foundation
import QuartzCore

/// What the engine was last told to draw into.
nonisolated(unsafe) var engineLayer: UnsafeMutableRawPointer?

@_cdecl("henge_vm_set_display_layer")
func recordDisplayLayer(_ layer: UnsafeMutableRawPointer?) { engineLayer = layer }

/// An external display scene as the app has it: records what it was told. Showing the Mac
/// attaches a layer the way the real screen view does; stopping it never detaches, because
/// iOS may never tear the view down once the monitor is gone.
@MainActor
final class FakeScreen: ExternalScreen {
    let surface = UUID()
    let layer = CAMetalLayer()
    var told: [String] = []

    func showMac(_ presentation: MacPresentation) {
        told.append("show")
        presentation.display.connect(surface, layer: layer, role: .external)
    }

    func stopMac() { told.append("stop") }
}

@main
struct LegacyExternalDisplayTests {
    @MainActor static func main() {
        let phoneLayer = CAMetalLayer()
        let router = DisplayRouter()
        func engineShows(_ layer: CAMetalLayer) -> Bool { engineLayer == Unmanaged.passUnretained(layer).toOpaque() }

        // Two external displays (USB-C and AirPlay) plugged in, then the session begins.
        router.connect(UUID(), layer: phoneLayer, role: .device)
        let a = FakeScreen(), b = FakeScreen()
        let legacy = LegacyExternalDisplay()
        legacy.connect(a)
        legacy.connect(b)
        legacy.begin(MacPresentation(display: router, aspect: 16.0 / 9.0))
        precondition(a.told == ["show"] && b.told == ["show"])
        precondition(engineShows(b.layer))

        // Unplugging A leaves B alone and still showing the Mac, not the phone.
        legacy.disconnect(a)
        precondition(a.told == ["show", "stop"])
        precondition(b.told == ["show"])
        precondition(engineShows(b.layer))

        // The session ending stops only the display still showing the Mac, and the phone gets it back.
        legacy.end(MacPresentation(display: router, aspect: 16.0 / 9.0))
        precondition(a.told == ["show", "stop"])
        precondition(b.told == ["show", "stop"])
        precondition(engineShows(phoneLayer) && !router.isExternal)
        print("Legacy external display: all checks passed")
    }
}
