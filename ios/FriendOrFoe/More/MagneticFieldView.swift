import CoreMotion
import SwiftUI

struct MagneticFieldView: View {
    @StateObject private var meter = MagneticFieldMeter()
    @Environment(\.scenePhase) private var scene

    var body: some View {
        List {
            Section {
                if meter.running {
                    if let magnitude = meter.reading.magnitude {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Field strength").font(.subheadline).foregroundStyle(.secondary)
                            Text(field(magnitude)).font(.largeTitle.weight(.semibold)).monospacedDigit()
                        }.accessibilityElement(children: .combine)
                    } else {
                        ProgressView("Waiting for a calibrated reading…")
                    }
                    if let accuracy = meter.reading.accuracy {
                        LabeledContent("Sensor accuracy", value: accuracy.label)
                    }
                    Button("Stop sweep", action: meter.stop)
                } else {
                    Text("Hold the iPhone at a starting point, then move it slowly to compare the magnetic field around nearby objects.")
                    Button("Start sweep", action: meter.start)
                }
                if let message = meter.message { Text(message).foregroundStyle(.secondary) }
                if meter.permissionDenied, let url = URL(string: UIApplication.openSettingsURLString) {
                    Link("Open Settings", destination: url)
                }
            }
            if meter.running {
                Section("Compare readings") {
                    if let baseline = meter.reading.baseline {
                        LabeledContent("Baseline", value: field(baseline))
                        if let change = meter.reading.change {
                            LabeledContent("Change from baseline", value: field(change, signed: true))
                        }
                        LabeledContent("Largest change at high accuracy", value: field(meter.reading.peakChange))
                        Button("Reset baseline", action: meter.resetBaseline)
                    } else {
                        Text("The next high-accuracy reading will set the baseline. If accuracy stays low, move the iPhone in a figure eight, then hold it still.")
                            .foregroundStyle(.secondary)
                    }
                }
            }
            Section("What affects the reading") {
                Text("The iPhone measures magnetic field strength in microteslas (µT). Magnets, metal and magnetic cases can change the reading. A change cannot identify a device or tell you whether it is recording.")
                Text("Readings stay on this iPhone and are cleared when the sweep stops.").foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Magnetic-field sweep")
        .onDisappear { meter.stop() }
        .onChange(of: scene) { _, phase in if phase == .background { meter.stop() } }
    }

    private func field(_ value: Double, signed: Bool = false) -> String {
        let format = FloatingPointFormatStyle<Double>.number.precision(.fractionLength(1))
        return value.formatted(signed ? format.sign(strategy: .always(includingZero: false)) : format) + " µT"
    }
}

enum FieldAccuracy: String {
    case high, medium, low, uncalibrated

    init(_ value: CMMagneticFieldCalibrationAccuracy) {
        switch value {
        case .high: self = .high
        case .medium: self = .medium
        case .low: self = .low
        default: self = .uncalibrated
        }
    }

    var label: String { rawValue.capitalized }
}

struct FieldSweepReading {
    private(set) var magnitude: Double?
    private(set) var accuracy: FieldAccuracy?
    private(set) var baseline: Double?
    private(set) var peakChange = 0.0
    var change: Double? {
        guard let magnitude, let baseline else { return nil }
        return magnitude - baseline
    }

    mutating func resetBaseline() {
        baseline = nil
        peakChange = 0
    }

    mutating func accept(x: Double, y: Double, z: Double, accuracy: FieldAccuracy) {
        let strength = hypot(hypot(x, y), z)
        self.accuracy = accuracy
        guard accuracy != .uncalibrated, x.isFinite, y.isFinite, z.isFinite, strength.isFinite else {
            magnitude = nil
            return
        }
        magnitude = strength
        guard accuracy == .high else { return }
        if let baseline {
            peakChange = max(peakChange, abs(strength - baseline))
        } else {
            baseline = strength
        }
    }
}

@MainActor private final class MagneticFieldMeter: ObservableObject {
    @Published private(set) var running = false
    @Published private(set) var reading = FieldSweepReading()
    @Published private(set) var message: String?
    @Published private(set) var permissionDenied = false
    private var manager: CMMotionManager?
    private var watchdog: Task<Void, Never>?
    private var lastSample = ContinuousClock.now
    private var generation = 0

    func start() {
        guard !running else { return }
        stop()
        let manager = CMMotionManager()
        guard manager.isMagnetometerAvailable, manager.isDeviceMotionAvailable,
              CMMotionManager.availableAttitudeReferenceFrames().contains(.xArbitraryCorrectedZVertical) else {
            message = "This device does not expose the calibrated magnetometer required for a sweep. Try it on an iPhone with a motion sensor."
            return
        }
        self.manager = manager
        let request = generation
        running = true
        lastSample = .now
        manager.deviceMotionUpdateInterval = 0.2
        // This reference frame corrects drift using the magnetometer and does not require location or true north.
        manager.startDeviceMotionUpdates(using: .xArbitraryCorrectedZVertical, to: .main) { [weak self] sample, error in
            MainActor.assumeIsolated {
                guard let self, self.generation == request else { return }
                if let error = error as NSError? {
                    if error.domain == CMErrorDomain, error.code == CMErrorDeviceRequiresMovement.rawValue {
                        self.lastSample = .now
                        self.reading.accept(x: 0, y: 0, z: 0, accuracy: .uncalibrated)
                    } else {
                        self.stop()
                        self.permissionDenied = error.domain == CMErrorDomain &&
                            [Int(CMErrorNotAuthorized.rawValue), Int(CMErrorMotionActivityNotAuthorized.rawValue)].contains(error.code)
                        self.message = self.permissionDenied ? "Motion access is disabled for this app." : "The motion sensor stopped providing readings. Try starting another sweep."
                    }
                    return
                }
                guard let field = sample?.magneticField else { return }
                self.lastSample = .now
                self.reading.accept(x: field.field.x, y: field.field.y, z: field.field.z, accuracy: FieldAccuracy(field.accuracy))
            }
        }
        watchdog = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(for: .seconds(5)) } catch { return }
                guard let self, self.generation == request else { return }
                if self.lastSample.duration(to: .now) >= .seconds(10) {
                    self.stop()
                    self.message = "No motion readings arrived. Check motion access in Settings and try again."
                    return
                }
            }
        }
    }

    func resetBaseline() { reading.resetBaseline() }

    func stop() {
        generation += 1
        manager?.stopDeviceMotionUpdates()
        manager = nil
        watchdog?.cancel()
        watchdog = nil
        reading = FieldSweepReading()
        running = false
        message = nil
        permissionDenied = false
    }
}
