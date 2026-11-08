import Foundation

/// The Shared folder in Files. A small guest helper mounts it inside macOS.
enum Sharing {
    static let enabledKey = "henge.shareFiles"
    static var folder: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Shared", isDirectory: true)
    }

    static let setUpCommand = "curl -s http://10.0.2.4/.henge/install | sh"

    /// Before a Mac starts.
    static func prepare(enabled: Bool) -> Bool {
        // Clear the guest route even when a previous VM in this app process used it.
        henge_vm_share(nil, nil, nil)
        guard enabled else { return false }
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let properties = try folder.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard properties.isDirectory == true, properties.isSymbolicLink != true else { return false }
        } catch { return false }
        // A Unix socket's path can't be long, so the short temporary folder.
        let socket = (NSTemporaryDirectory() as NSString).appendingPathComponent("s.sock")
        return henge_vm_share(folder.path, socket, Bundle.main.path(forResource: "henge-agent", ofType: "gz")) == 0
    }

    /// Types the set up command at the Mac's cursor, and stops there: the person using the Mac presses Return.
    /// Henge can't see what has the keyboard, so it must not submit anything by itself. Opening Terminal for them
    /// through Spotlight was tried: when Spotlight was slow the text went to a web search, and it could as easily
    /// have gone to a chat.
    static func typeSetUpCommand() async {
        await Keys.typeSlowly(setUpCommand)
    }
}
