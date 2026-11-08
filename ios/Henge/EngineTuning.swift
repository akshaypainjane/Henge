import UIKit

/// Read the app's preferences once per VM, before the engine starts.
enum EngineTuning {
    static let refreshKey = "henge.perf.hz"
    static let superblocksKey = "henge.perf.superblocks"
    static let wfiSpinKey = "henge.perf.wfiSpin"
    static let fastFlushKey = "henge.perf.fastFlush"

    @MainActor static func snapshot(forInstall: Bool = false) -> HvVmTuning {
        let defaults = UserDefaults.standard
        let useHighRefresh = !forInstall && defaults.integer(forKey: refreshKey) == 120 &&
                             UIScreen.main.maximumFramesPerSecond >= 120
        var statsInterval: Int32 = 0
        #if DEBUG
        if let value = ProcessInfo.processInfo.environment["HV_STATS"].flatMap(Int32.init),
           (0...3600).contains(value) {
            statsInterval = value
        }
        #endif
        return HvVmTuning(refresh_hz: useHighRefresh ? 120 : 60, stats_interval: statsInterval,
                          fast_disk_flush: !forInstall && defaults.bool(forKey: fastFlushKey),
                          uncached_disk_writes: forInstall,
                          idle_spin: !forInstall && defaults.bool(forKey: wfiSpinKey),
                          superblocks: !forInstall && defaults.bool(forKey: superblocksKey))
    }
}
