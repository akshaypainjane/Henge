import Foundation

// Before iOS 27 the external display scene reaches the app only when Info.plist's scene manifest
// declares UIWindowSceneSessionRoleExternalDisplayNonInteractive with a delegate class that
// resolves at runtime; a SwiftUI-lifecycle app's delegate is never asked for that role, with or
// without the entry (measured on iOS 18.5 and 26.5 simulators, see the external-display-diag
// commit). Without the entry the display mirrors the phone and nothing is logged, which is the
// bug seen on an iPhone 17 Pro on iOS 26.6.1. This pins the entry and the class it names.
//
// Usage: test_scene_manifest [Info.plist] [ExternalDisplaySceneDelegate.swift]
@main
struct SceneManifestTests {
    static var failures = 0

    static func check(_ ok: Bool, _ what: String) {
        print("\(ok ? "ok  " : "FAIL") \(what)")
        if !ok { failures += 1 }
    }

    static func main() {
        let args = CommandLine.arguments
        let plistPath = args.count > 1 ? args[1] : "ios/Henge/Info.plist"
        let sourcePath = args.count > 2 ? args[2] : "ios/Henge/ExternalDisplaySceneDelegate.swift"
        let role = "UIWindowSceneSessionRoleExternalDisplayNonInteractive"
        let className = "$(PRODUCT_MODULE_NAME).ExternalDisplaySceneDelegate"

        let plist = (try? Data(contentsOf: URL(fileURLWithPath: plistPath)))
            .flatMap { try? PropertyListSerialization.propertyList(from: $0, format: nil) } as? [String: Any]
        check(plist != nil, "\(plistPath) parses")

        let manifest = plist?["UIApplicationSceneManifest"] as? [String: Any]
        check(manifest != nil, "Info.plist has a UIApplicationSceneManifest")

        let configs = manifest?["UISceneConfigurations"] as? [String: Any]
        let external = configs?[role] as? [[String: Any]]
        check(external != nil, "manifest declares \(role)")
        check((external?.count ?? 0) == 1, "the role has exactly one configuration (an empty array opts out)")

        let delegate = external?.first?["UISceneDelegateClassName"] as? String
        check(delegate == className, "the configuration names \(className) (a name-only entry gets no scene)")
        check((external?.first?["UISceneConfigurationName"] as? String)?.isEmpty == false, "the configuration has a name")

        // Xcode expands $(PRODUCT_MODULE_NAME) to the Swift module; the class must exist there under
        // that exact name, as a UIWindowSceneDelegate, for UIKit's NSClassFromString to find it.
        let source = (try? String(contentsOfFile: sourcePath, encoding: .utf8)) ?? ""
        check(source.contains("class ExternalDisplaySceneDelegate"), "\(sourcePath) declares ExternalDisplaySceneDelegate")
        check(source.contains("UIWindowSceneDelegate"), "ExternalDisplaySceneDelegate is a UIWindowSceneDelegate")
        check(!source.contains("@objc(") , "the class keeps its Swift name (no @objc rename, which the plist does not use)")

        print(failures == 0 ? "scene manifest: all checks passed" : "scene manifest: \(failures) check(s) failed")
        exit(failures == 0 ? 0 : 1)
    }
}
