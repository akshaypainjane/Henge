import AVFoundation
import SwiftUI

/// Permission and sharing are per VM session. Only the user can turn the camera on.
@MainActor
final class GuestCamera: ObservableObject {
    @Published private(set) var status = "Off"
    @Published private(set) var busy = false
    @Published private(set) var sharing = false
    private var request = 0
    private let capture = CameraCapture()

    func start() {
        guard !busy, !sharing, henge_vm_camera_available() == 1 else { return }
        request += 1
        let thisRequest = request
        busy = true
        status = "Requesting camera access"
        Task { [weak self] in
            let authorization = AVCaptureDevice.authorizationStatus(for: .video)
            let allowed: Bool
            if authorization == .notDetermined {
                allowed = await AVCaptureDevice.requestAccess(for: .video)
                // The permission alert can keep the scene inactive until it finishes dismissing.
                try? await Task.sleep(nanoseconds: 450_000_000)
            } else {
                allowed = authorization == .authorized
            }
            guard let self, request == thisRequest else { return }
            guard allowed else {
                stop("Camera access is off in iPad Settings.")
                return
            }
            guard UIApplication.shared.applicationState == .active, henge_vm_camera_available() == 1 else {
                stop("Return to Henge and turn on sharing again.")
                return
            }
            // From this point a guest window can show the feed or a captured picture.
            MacPreview.disableSavingForSession()
            sharing = true
            status = "Starting camera"
            guard henge_vm_camera_share(1) == 0 else {
                stop("The virtual Mac’s camera is unavailable.")
                return
            }
            capture.start { [weak self] error in
                Task { @MainActor in
                    guard let self, self.request == thisRequest else { return }
                    if let error { self.stop(error) }
                    else {
                        self.busy = false
                        self.status = "Sharing front camera"
                    }
                }
            }
        }
    }

    func sceneChanged(_ phase: ScenePhase) {
        if phase == .background || (phase == .inactive && sharing) { stop() }
    }

    func stop(_ message: String = "Off") {
        guard busy || sharing else { return }
        request += 1
        // Stop guest access immediately, before queuing the potentially blocking AVFoundation stop.
        henge_vm_camera_share(0)
        capture.stop()
        busy = false
        sharing = false
        status = message
    }
}

private final class CameraCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    private let queue = DispatchQueue(label: "henge.camera")
    private var session: AVCaptureSession?
    private var observers: [NSObjectProtocol] = []
    private var rotation: AVCaptureDevice.RotationCoordinator?
    private var rotationObservation: NSKeyValueObservation?
    private var lastFrame = CMTime.invalid
    private let frameRate = henge_vm_camera_frame_rate()

    func start(report: @escaping @Sendable (String?) -> Void) {
        queue.async { [self] in
            guard henge_vm_camera_sharing() == 1 else { return }
            let session = AVCaptureSession()
            guard let front = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front),
                  let input = try? AVCaptureDeviceInput(device: front),
                  session.canSetSessionPreset(.vga640x480), session.canAddInput(input) else {
                report("The iPad’s front camera is unavailable.")
                return
            }
            let output = AVCaptureVideoDataOutput()
            session.beginConfiguration()
            session.sessionPreset = .vga640x480
            session.addInput(input)
            guard session.canAddOutput(output) else {
                session.commitConfiguration()
                report("The camera couldn’t provide the VM’s video format.")
                return
            }
            session.addOutput(output)
            guard output.availableVideoPixelFormatTypes.contains(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) else {
                session.commitConfiguration()
                report("The camera couldn’t provide the VM’s video format.")
                return
            }
            output.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
            ]
            output.alwaysDiscardsLateVideoFrames = true
            output.setSampleBufferDelegate(self, queue: self.queue)
            session.commitConfiguration()

            let fps = Double(self.frameRate)
            let interval = CMTime(value: 1, timescale: self.frameRate)
            if front.activeFormat.videoSupportedFrameRateRanges.contains(where: { $0.minFrameRate <= fps && $0.maxFrameRate >= fps }),
               (try? front.lockForConfiguration()) != nil {
                front.activeVideoMinFrameDuration = interval
                front.activeVideoMaxFrameDuration = interval
                front.unlockForConfiguration()
            }
            self.lastFrame = .invalid
            self.session = session
            if let connection = output.connection(with: .video) {
                let rotation = AVCaptureDevice.RotationCoordinator(device: front, previewLayer: nil)
                self.rotation = rotation
                self.rotateLandscape(connection, angle: rotation.videoRotationAngleForHorizonLevelCapture)
                self.rotationObservation = rotation.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) {
                    [weak self, weak connection, weak session] _, change in
                    guard let self, let connection, let session, let angle = change.newValue else { return }
                    self.queue.async {
                        guard self.session === session, henge_vm_camera_sharing() == 1 else { return }
                        self.rotateLandscape(connection, angle: angle)
                    }
                }
            }
            let notifications = NotificationCenter.default
            self.observers = [AVCaptureSession.wasInterruptedNotification, AVCaptureSession.runtimeErrorNotification].map { name in
                notifications.addObserver(forName: name, object: session, queue: nil) { _ in
                    report("The iPad camera was interrupted. Turn on sharing again to reconnect.")
                }
            }
            guard henge_vm_camera_sharing() == 1 else { return }
            session.startRunning()
            report(session.isRunning ? nil : "The iPad camera couldn’t start.")
        }
    }

    private func rotateLandscape(_ connection: AVCaptureConnection, angle: CGFloat) {
        // The virtual device has a fixed 640 × 480 landscape mode. A half-turn
        // keeps those dimensions and lets AVFoundation rotate both NV12 planes.
        let landscapeAngle: CGFloat = angle == 180 ? 180 : 0
        guard connection.isVideoRotationAngleSupported(landscapeAngle) else { return }
        connection.videoRotationAngle = landscapeAngle
        HengeLog.log("camera", "capture rotation \(Int(landscapeAngle)) degrees")
    }

    func stop() {
        queue.async {
            self.rotationObservation?.invalidate()
            self.rotationObservation = nil
            self.rotation = nil
            self.observers.forEach { NotificationCenter.default.removeObserver($0) }
            self.observers.removeAll()
            self.session?.stopRunning()
            self.session = nil
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        let time = sampleBuffer.presentationTimeStamp
        guard !lastFrame.isValid || CMTimeCompare(time - lastFrame, CMTime(value: 19, timescale: 20 * frameRate)) >= 0,
              let image = sampleBuffer.imageBuffer,
              CVPixelBufferGetPixelFormatType(image) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
              CVPixelBufferGetWidth(image) == 640, CVPixelBufferGetHeight(image) == 480,
              CVPixelBufferGetPlaneCount(image) == 2,
              CVPixelBufferLockBaseAddress(image, .readOnly) == kCVReturnSuccess else { return }
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        guard let y = CVPixelBufferGetBaseAddressOfPlane(image, 0),
              let uv = CVPixelBufferGetBaseAddressOfPlane(image, 1) else { return }
        lastFrame = time
        henge_vm_camera_frame(y.assumingMemoryBound(to: UInt8.self), CVPixelBufferGetBytesPerRowOfPlane(image, 0),
                              uv.assumingMemoryBound(to: UInt8.self), CVPixelBufferGetBytesPerRowOfPlane(image, 1))
    }
}
