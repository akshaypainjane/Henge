import Foundation
import QuartzCore

/// What the engine was last told to draw into.
nonisolated(unsafe) var engineLayer: UnsafeMutableRawPointer?

@_cdecl("henge_vm_set_display_layer")
func recordDisplayLayer(_ layer: UnsafeMutableRawPointer?) { engineLayer = layer }

@main
struct DisplayRouterTests {
    @MainActor static func main() {
        let phoneLayer = CAMetalLayer(), monitorLayer = CAMetalLayer()
        let phone = UUID(), monitor = UUID()
        let router = DisplayRouter()
        func engineShows(_ layer: CAMetalLayer) -> Bool { engineLayer == Unmanaged.passUnretained(layer).toOpaque() }

        router.connect(phone, layer: phoneLayer, role: .device)
        router.connect(monitor, layer: monitorLayer, role: .external)
        precondition(engineShows(monitorLayer) && router.isExternal)

        // Two monitors (USB-C and AirPlay): the later one shows the Mac. Disconnecting one
        // monitor's surface leaves the other showing it, not the phone.
        let airPlayLayer = CAMetalLayer(), airPlay = UUID()
        router.connect(airPlay, layer: airPlayLayer, role: .external)
        precondition(engineShows(airPlayLayer))
        router.disconnect(airPlay)
        precondition(engineShows(monitorLayer) && router.isExternal)

        // The last monitor going gives the Mac back to the phone, and a late repeat changes nothing.
        router.disconnect(monitor)
        precondition(engineShows(phoneLayer) && !router.isExternal)
        router.disconnect(monitor)
        precondition(engineShows(phoneLayer) && !router.isExternal)
        print("Display router: all checks passed")
    }
}
