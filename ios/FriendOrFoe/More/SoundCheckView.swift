import Accelerate
import AVFAudio
import SwiftUI

struct SoundCheckView: View {
    @StateObject private var meter = SoundMeter()
    @Environment(\.scenePhase) private var scene

    var body: some View {
        List {
            Section {
                Text("Measure the microphone input and look for energy between 18 and 22 kHz. Voices and sound samples are never saved.")
                if meter.running {
                    LabeledContent("Input level", value: meter.reading.levelText)
                    LabeledContent("18–22 kHz band", value: meter.reading.bandText)
                    LabeledContent("Microphone sample rate", value: "\(Int(meter.rate)) Hz")
                    Button("Stop sound check", action: meter.stop)
                } else {
                    Button("Start sound check", action: meter.start).disabled(meter.starting)
                }
                if let message = meter.message { Text(message).foregroundStyle(.secondary) }
            }
            Section("Reading the levels") {
                Text("Levels are relative to the microphone’s maximum digital input. Microphones and audio routes vary. A high reading cannot establish that another device is listening.")
            }
        }.navigationTitle("Sound check")
        .onDisappear { meter.stop() }
        .onChange(of: scene) { _, phase in if phase == .background { meter.stop() } }
        .onReceive(NotificationCenter.default.publisher(for: AVAudioSession.interruptionNotification)) { _ in meter.handleInterruption() }
        .onReceive(NotificationCenter.default.publisher(for: AVAudioSession.routeChangeNotification)) { note in
            guard let reason = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
                  reason == AVAudioSession.RouteChangeReason.oldDeviceUnavailable.rawValue else { return }
            meter.handleInterruption()
        }
    }
}

struct SoundReading: Sendable {
    var level: Double?
    var band: Double?
    private func text(_ value: Double?) -> String { value.map { String(format: "%.0f dBFS", $0) } ?? "Unavailable" }
    var levelText: String { text(level) }
    var bandText: String { text(band) }
}

final class SoundAnalyzer {
    private let length = 1024
    private let power: vDSP_Length = 10
    private let setup: FFTSetup
    private var window: [Float]
    private let meanWindowPower: Double
    private var windowed: [Float]
    private var real: [Float]
    private var imaginary: [Float]

    init?() {
        guard let setup = vDSP_create_fftsetup(power, FFTRadix(kFFTRadix2)) else { return nil }
        self.setup = setup
        window = [Float](repeating: 0, count: length)
        windowed = window
        real = [Float](repeating: 0, count: length / 2)
        imaginary = real
        vDSP_hann_window(&window, vDSP_Length(length), Int32(vDSP_HANN_DENORM))
        meanWindowPower = window.reduce(0.0) { $0 + Double($1) * Double($1) } / Double(length)
    }
    deinit { vDSP_destroy_fftsetup(setup) }

    // The engine calls a single tap serially on its audio thread. Keep the FFT
    // and frame counter there; only small numeric readings cross to the UI.
    func makeTap(rate: Double, publish: @escaping @Sendable (SoundReading) -> Void) -> AVAudioNodeTapBlock {
        var frames = 0
        return { [self] buffer, _ in
            guard let channel = buffer.floatChannelData?[0] else { return }
            frames += Int(buffer.frameLength)
            guard frames >= Int(rate / 5) else { return }
            frames = 0
            publish(analyze(samples: channel, count: Int(buffer.frameLength), rate: rate))
        }
    }

    func analyze(samples: UnsafePointer<Float>, count: Int, rate: Double) -> SoundReading {
        guard rate.isFinite, rate > 0, count > 0 else { return SoundReading() }
        var rms: Float = 0
        vDSP_rmsqv(samples, 1, &rms, vDSP_Length(count))
        var result = SoundReading(level: 20 * log10(max(Double(rms), 0.000001)))
        guard count >= length, rate >= 44_100 else { return result }
        vDSP_vmul(samples, 1, window, 1, &windowed, 1, vDSP_Length(length))
        real.withUnsafeMutableBufferPointer { r in
            imaginary.withUnsafeMutableBufferPointer { i in
                var split = DSPSplitComplex(realp: r.baseAddress!, imagp: i.baseAddress!)
                windowed.withUnsafeBufferPointer { signal in
                    signal.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: length / 2) {
                        vDSP_ctoz($0, 2, &split, 1, vDSP_Length(length / 2))
                    }
                }
                vDSP_fft_zrip(setup, &split, 1, power, FFTDirection(kFFTDirection_Forward))
            }
        }
        let first = Int(ceil(18_000 * Double(length) / rate))
        let last = min(length / 2 - 1, Int(22_000 * Double(length) / rate))
        guard first >= 1, first <= last else { return result }
        var energy = 0.0
        for bin in first...last {
            let inPhase = Double(real[bin])
            let quadrature = Double(imaginary[bin])
            energy += inPhase * inPhase + quadrature * quadrature
        }
        // One-sided energy for vDSP's doubled real FFT, corrected for the Hann window.
        result.band = 10 * log10(max(energy / (2 * Double(length * length) * meanWindowPower), 1e-12))
        return result
    }
}

@MainActor
private final class SoundMeter: ObservableObject {
    @Published var running = false
    @Published var starting = false
    @Published var rate = 0.0
    @Published var reading = SoundReading()
    @Published var message: String?
    private var engine: AVAudioEngine?
    private var generation = 0
    private var ownsSession = false

    func start() {
        guard !running && !starting else { return }
        generation += 1
        let request = generation
        starting = true
        message = nil
        Task {
            let allowed = await AVAudioApplication.requestRecordPermission()
            guard request == generation else { return }
            starting = false
            guard allowed else {
                message = "Allow microphone access in iPhone Settings to start a sound check."
                return
            }
            do {
                let audio = AVAudioSession.sharedInstance()
                try audio.setCategory(.record, mode: .measurement)
                try audio.setActive(true)
                ownsSession = true
                let engine = AVAudioEngine()
                let input = engine.inputNode
                let format = input.outputFormat(forBus: 0)
                guard format.sampleRate > 0, format.channelCount > 0,
                      format.commonFormat == .pcmFormatFloat32, !format.isInterleaved,
                      let analyzer = SoundAnalyzer() else {
                    stop()
                    message = "No microphone input is available."
                    return
                }
                let sampleRate = format.sampleRate
                self.engine = engine
                rate = sampleRate
                let tap = analyzer.makeTap(rate: sampleRate) { [weak self] value in
                    Task { @MainActor in
                        guard let self, self.generation == request, self.running else { return }
                        self.reading = value
                    }
                }
                input.installTap(onBus: 0, bufferSize: 2048, format: nil, block: tap)
                try engine.start()
                running = true
            } catch {
                stop()
                message = "Microphone input is unavailable. Check the audio route and try again."
            }
        }
    }

    func handleInterruption() {
        guard running || starting else { return }
        stop()
        message = "Sound check stopped when the audio input changed."
    }

    func stop() {
        generation += 1
        if let engine {
            engine.stop()
            engine.inputNode.removeTap(onBus: 0)
        }
        engine = nil
        if ownsSession {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
            ownsSession = false
        }
        running = false
        starting = false
        reading = SoundReading()
    }
}
