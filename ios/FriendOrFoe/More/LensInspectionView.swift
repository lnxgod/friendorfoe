import AVFoundation
import CoreImage
import SwiftUI

struct LensInspectionView: View {
    @StateObject private var camera = InspectionCamera()
    @Environment(\.scenePhase) private var scene
    @Environment(\.verticalSizeClass) private var verticalSize

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                Text("Move the phone slowly and look for small reflections. Glass, mirrors and polished surfaces can all reflect light.")
                if camera.phase == .live, let device = camera.device {
                    InspectionPreview(frames: camera.frames, device: device)
                        .aspectRatio(verticalSize == .compact ? 4.0 / 3 : 3.0 / 4, contentMode: .fit)
                        .frame(maxWidth: verticalSize == .compact ? 360 : .infinity)
                        .clipShape(RoundedRectangle(cornerRadius: 16))
                        .accessibilityLabel("Live camera preview")
                    Toggle("Highlight bright areas", isOn: $camera.highlight)
                    if camera.hasTorch {
                        Toggle("Flashlight", isOn: Binding(get: { camera.torchOn }, set: { camera.setTorch($0) }))
                    }
                }
                if camera.phase == .idle {
                    Label("On-device camera", systemImage: "camera.viewfinder").font(.headline)
                    Button("Start inspection", action: camera.start).buttonStyle(.borderedProminent)
                } else {
                    if camera.phase == .starting { ProgressView("Starting camera…") }
                    Button("Stop inspection", action: camera.stop)
                }
                if let message = camera.message { Text(message).foregroundStyle(.secondary) }
                Text("Frames stay on this phone and are discarded after display. Reflections alone cannot identify a hidden camera. Some lenses produce no visible reflection.")
                    .font(.footnote).foregroundStyle(.secondary)
            }.padding()
        }
        .navigationTitle("Lens inspection")
        .onDisappear { camera.stop() }
        .onChange(of: scene) { _, phase in if phase == .background { camera.stop() } }
    }
}

// Mask and preview share one image. They always come from the same instant and receive the same crop and rotation.
@MainActor private final class InspectionFrames: ObservableObject {
    @Published var image: CGImage?
}

private struct InspectionPreview: UIViewRepresentable {
    @ObservedObject var frames: InspectionFrames
    let device: AVCaptureDevice

    func makeUIView(context: Context) -> InspectionSurface { InspectionSurface(device: device) }
    func updateUIView(_ view: InspectionSurface, context: Context) { view.frameLayer.contents = frames.image }
}

private final class InspectionSurface: UIView {
    let frameLayer = CALayer()
    private var coordinator: AVCaptureDevice.RotationCoordinator?
    private var rotationObservation: NSKeyValueObservation?
    private var angle = CGFloat.zero

    init(device: AVCaptureDevice) {
        super.init(frame: .zero)
        backgroundColor = .black
        clipsToBounds = true
        layer.addSublayer(frameLayer)
        frameLayer.contentsGravity = .resizeAspectFill
        frameLayer.masksToBounds = true
        frameLayer.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull(), "transform": NSNull()]
        let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: layer)
        self.coordinator = coordinator
        // AVFoundation delivers preview-angle observations on the main queue.
        rotationObservation = coordinator.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.initial, .new]) { [weak self] _, change in
            guard let degrees = change.newValue else { return }
            MainActor.assumeIsolated {
                self?.angle = degrees * .pi / 180
                self?.setNeedsLayout()
            }
        }
    }

    required init?(coder: NSCoder) { nil }

    override func layoutSubviews() {
        super.layoutSubviews()
        // Find the unrotated rectangle that covers the view. This swaps width and height at quarter turns.
        let width = abs(bounds.width * cos(angle)) + abs(bounds.height * sin(angle))
        let height = abs(bounds.width * sin(angle)) + abs(bounds.height * cos(angle))
        frameLayer.bounds = CGRect(x: 0, y: 0, width: width, height: height)
        frameLayer.position = CGPoint(x: bounds.midX, y: bounds.midY)
        frameLayer.setAffineTransform(CGAffineTransform(rotationAngle: angle))
    }
}

@MainActor private final class InspectionCamera: ObservableObject {
    enum Phase { case idle, starting, live, interrupted }
    @Published private(set) var phase = Phase.idle
    @Published private(set) var message: String?
    @Published private(set) var hasTorch = false
    @Published private(set) var torchOn = false
    @Published var highlight = false { didSet { capture?.highlight(highlight) } }
    let frames = InspectionFrames()
    private(set) var device: AVCaptureDevice?
    private var capture: InspectionCapture?
    private var generation = 0

    func start() {
        guard phase == .idle else { return }
        message = nil
        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else {
            message = "A rear camera is required. Try this tool on an iPhone."
            return
        }
        generation += 1
        let request = generation
        phase = .starting
        Task {
            let granted = await AVCaptureDevice.requestAccess(for: .video)
            guard generation == request else { return }
            guard granted else {
                phase = .idle
                message = "Allow camera access in iPhone Settings to start inspection."
                return
            }
            self.device = device
            let capture = InspectionCapture(device: device) { [weak self] event in
                guard let self, self.generation == request else { return }
                switch event {
                case .live(let hasTorch):
                    self.hasTorch = hasTorch
                    self.message = nil
                    self.phase = .live
                case .frame(let image):
                    if self.phase == .live { self.frames.image = image }
                case .torch(let enabled, let available):
                    self.torchOn = enabled
                    self.hasTorch = available
                case .interrupted:
                    self.phase = .interrupted
                    self.frames.image = nil
                    self.torchOn = false
                    self.message = "Camera interrupted. Inspection will resume when the camera is available while this screen is open."
                case .failure(let reason):
                    self.stop()
                    self.message = reason
                }
            }
            self.capture = capture
            capture.start()
        }
    }

    func stop() {
        generation += 1
        capture?.stop()
        capture = nil
        device = nil
        frames.image = nil
        highlight = false
        hasTorch = false
        torchOn = false
        message = nil
        phase = .idle
    }

    func setTorch(_ enabled: Bool) {
        if phase == .live { capture?.setTorch(enabled) }
    }
}

// All AV capture mutations, notifications and frame callbacks run on the same serial queue.
// The queue is shared across runs so a quick restart releases the previous session before opening another.
private final class InspectionCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    enum Event: Sendable { case live(Bool), frame(CGImage), torch(Bool, Bool), interrupted, failure(String) }
    private static let queue = DispatchQueue(label: "org.gamechangersai.lens-capture", qos: .userInitiated)
    private let session = AVCaptureSession()
    private let device: AVCaptureDevice
    private let receive: @MainActor @Sendable (Event) -> Void
    private let context = CIContext(options: [.cacheIntermediates: false])
    private let frameSlot = DispatchSemaphore(value: 1)
    private var observations: [NSObjectProtocol] = []
    private var highlighting = false
    private var stopped = false
    private var lastFrame = CMTime.invalid

    init(device: AVCaptureDevice, receive: @escaping @MainActor @Sendable (Event) -> Void) {
        self.device = device
        self.receive = receive
    }

    private func send(_ event: Event) { DispatchQueue.main.async { self.receive(event) } }

    func start() {
        Self.queue.async {
            guard self.configure() else {
                self.send(.failure("The camera is unavailable. Close other camera tools and try again."))
                return
            }
            self.observeSession()
            self.resume()
        }
    }

    private func configure() -> Bool {
        guard let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) else { return false }
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        if session.canSetSessionPreset(.vga640x480) { session.sessionPreset = .vga640x480 }
        session.addInput(input)
        let output = AVCaptureVideoDataOutput()
        output.alwaysDiscardsLateVideoFrames = true
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
        guard session.canAddOutput(output) else { return false }
        session.addOutput(output)
        // Render in native sensor coordinates. The preview layer applies a single display rotation.
        guard let connection = output.connection(with: .video), connection.isVideoRotationAngleSupported(0) else { return false }
        connection.videoRotationAngle = 0
        output.setSampleBufferDelegate(self, queue: Self.queue)
        return true
    }

    private func observeSession() {
        let center = NotificationCenter.default
        observations = [
            center.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: session, queue: nil) { [weak self] _ in
                Self.queue.async { [weak self] in
                    guard let self, !self.stopped, self.session.isInterrupted else { return }
                    self.applyTorch(false)
                    self.send(.interrupted)
                }
            },
            center.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: session, queue: nil) { [weak self] _ in
                Self.queue.async { [weak self] in self?.resume() }
            },
            center.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: session, queue: nil) { [weak self] _ in
                Self.queue.async { [weak self] in
                    guard let self, !self.stopped else { return }
                    self.send(.failure("The camera stopped. Start inspection again when it is available."))
                }
            }
        ]
    }

    private func resume() {
        guard !stopped else { return }
        if !session.isInterrupted && !session.isRunning { session.startRunning() }
        if session.isInterrupted { send(.interrupted) }
        else if session.isRunning {
            applyTorch(false)
            send(.live(device.hasTorch && device.isTorchAvailable))
        }
        else { send(.failure("The camera could not start. Try again when it is available.")) }
    }

    func stop() {
        Self.queue.async {
            self.stopped = true
            self.observations.forEach(NotificationCenter.default.removeObserver)
            self.observations.removeAll()
            self.applyTorch(false)
            // stopRunning also withdraws a start request during an interruption.
            self.session.stopRunning()
        }
    }

    func highlight(_ enabled: Bool) { Self.queue.async { self.highlighting = enabled } }
    func setTorch(_ enabled: Bool) {
        Self.queue.async {
            guard !self.stopped, self.session.isRunning, !self.session.isInterrupted else { return }
            self.applyTorch(enabled)
            self.send(.torch(self.device.torchMode == .on, self.device.isTorchAvailable))
        }
    }

    private func applyTorch(_ enabled: Bool) {
        let mode: AVCaptureDevice.TorchMode = enabled ? .on : .off
        guard device.hasTorch, device.torchMode != mode, device.isTorchModeSupported(mode),
              !enabled || device.isTorchAvailable, (try? device.lockForConfiguration()) != nil else { return }
        device.torchMode = mode
        device.unlockForConfiguration()
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        let time = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        guard !stopped, !session.isInterrupted,
              !lastFrame.isValid || time < lastFrame || (time - lastFrame).seconds >= 1.0 / 15.0,
              let buffer = CMSampleBufferGetImageBuffer(sampleBuffer), frameSlot.wait(timeout: .now()) == .success else { return }
        lastFrame = time
        let input = CIImage(cvPixelBuffer: buffer)
        var result = input
        if highlighting {
            let mask = input.applyingFilter("CIColorThreshold", parameters: ["inputThreshold": 0.91])
            let orange = CIImage(color: CIColor(red: 1, green: 0.55, blue: 0, alpha: 0.75)).cropped(to: input.extent)
            let tinted = orange.composited(over: input)
            result = tinted.applyingFilter("CIBlendWithMask", parameters: [kCIInputMaskImageKey: mask, kCIInputBackgroundImageKey: input])
        }
        guard let image = context.createCGImage(result, from: input.extent) else { frameSlot.signal(); return }
        DispatchQueue.main.async {
            self.receive(.frame(image))
            self.frameSlot.signal()
        }
    }
}
