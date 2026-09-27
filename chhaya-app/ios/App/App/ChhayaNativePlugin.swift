import Foundation
import CoreBluetooth
import AVFoundation
import Speech
import UserNotifications
import Capacitor

/// Native helpers for the Chhaya page, reachable from JavaScript as
/// `Capacitor.Plugins.ChhayaNative` (no npm package or bundler needed, so the
/// page can keep being one plain HTML file served from Vercel).
///
/// Button box (ESP32, Bluetooth LE, "Nordic UART" service):
///   box -> page : text lines such as "BTN:TALK", "VOL:40", "WIFI:OK ..."   (event "boxLine")
///   page -> box : text lines such as "LED:listening", "WIFI:<name>\t<pass>" (boxWrite)
///   connection changes                                                    (event "boxState")
///
/// Reminders: local notifications, so a reminder still rings when the app is
/// closed or the iPad is offline.
///
/// Wake word: Apple's on-device Hindi speech recognition listens continuously and sends
/// what it hears as text (wakeStart / wakeStop, events "wakeText"); the page spots "छाया".
///
/// Microphone: native recording with voice-activity detection (micRecord / micCancel,
/// level events "micLevel"). Inside the app the web page's own microphone delivered
/// no usable speech, so listening goes through Apple's recorder instead.
@objc(ChhayaNativePlugin)
public class ChhayaNativePlugin: CAPPlugin, CAPBridgedPlugin {
    public let identifier = "ChhayaNativePlugin"
    public let jsName = "ChhayaNative"
    public let pluginMethods: [CAPPluginMethod] = [
        CAPPluginMethod(name: "info", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "boxStart", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "boxStatus", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "boxWrite", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "boxForget", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "notifyPermission", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "notifySchedule", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "notifyCancelAll", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "micRecord", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "micCancel", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "wakeStart", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "wakeStop", returnType: CAPPluginReturnPromise),
    ]

    private lazy var wake = NativeWake(onText: { [weak self] text, isFinal in
        self?.notifyListeners("wakeText", data: ["text": text, "isFinal": isFinal])
    })

    // MARK: - Wake word

    /// Starts continuous listening. Resolves {} when running, or { error } (no permission,
    /// Hindi recogniser unavailable...) so the page can fall back to the server check.
    @objc func wakeStart(_ call: CAPPluginCall) {
        let locale = call.getString("locale") ?? "hi-IN"
        let hints = call.getArray("hints", String.self) ?? []
        DispatchQueue.main.async {
            self.mic.cancel()
            self.wake.start(locale: locale, hints: hints) { error in
                if let error = error { call.resolve(["error": error]) }
                else { call.resolve(["onDevice": self.wake.onDevice]) }
            }
        }
    }

    @objc func wakeStop(_ call: CAPPluginCall) {
        DispatchQueue.main.async {
            self.wake.stop()
            call.resolve()
        }
    }

    private lazy var mic = NativeMic(onLevel: { [weak self] level in
        self?.notifyListeners("micLevel", data: ["level": level])
    })

    // MARK: - Microphone

    /// Records one utterance: starts when speech is heard, stops after a pause.
    /// Options (ms): maxMs, silenceMs, noSpeechMs, minSpeechMs.
    /// Resolves { audio: base64 m4a, mime, voicedMs, durationMs } or { empty: true, reason }.
    @objc func micRecord(_ call: CAPPluginCall) {
        let opts = NativeMic.Options(
            maxMs: call.getDouble("maxMs") ?? 15000,
            silenceMs: call.getDouble("silenceMs") ?? 1000,
            noSpeechMs: call.getDouble("noSpeechMs") ?? 8000,
            minSpeechMs: call.getDouble("minSpeechMs") ?? 300)
        DispatchQueue.main.async {
            self.wake.stop()   // the question recording needs the microphone to itself
            self.mic.record(opts) { result in call.resolve(result) }
        }
    }

    @objc func micCancel(_ call: CAPPluginCall) {
        DispatchQueue.main.async {
            self.mic.cancel()
            call.resolve()
        }
    }

    private lazy var box = BoxLink(
        onState: { [weak self] state, name in
            self?.notifyListeners("boxState", data: ["state": state, "name": name])
        },
        onLine: { [weak self] line in
            self?.notifyListeners("boxLine", data: ["line": line])
        },
        onLog: { [weak self] msg in
            self?.notifyListeners("boxLog", data: ["msg": msg])
        })

    // MARK: - App info

    @objc func info(_ call: CAPPluginCall) {
        let b = Bundle.main.infoDictionary ?? [:]
        call.resolve([
            "platform": "ios",
            "version": b["CFBundleShortVersionString"] as? String ?? "",
            "build": b["CFBundleVersion"] as? String ?? "",
        ])
    }

    // MARK: - Button box

    /// Starts looking for the box (and keeps reconnecting to it from then on).
    @objc func boxStart(_ call: CAPPluginCall) {
        DispatchQueue.main.async {
            self.box.start()
            call.resolve(self.box.status())
        }
    }

    @objc func boxStatus(_ call: CAPPluginCall) {
        DispatchQueue.main.async { call.resolve(self.box.status()) }
    }

    @objc func boxWrite(_ call: CAPPluginCall) {
        guard let line = call.getString("line") else { return call.reject("line is required") }
        DispatchQueue.main.async {
            if self.box.write(line) { call.resolve() } else { call.reject("box not connected") }
        }
    }

    /// Forget the remembered box (e.g. after replacing the ESP32) and search again.
    @objc func boxForget(_ call: CAPPluginCall) {
        DispatchQueue.main.async {
            self.box.forget()
            call.resolve()
        }
    }

    // MARK: - Local notifications (reminders)

    private let reminderPrefix = "chhaya-rem-"

    @objc func notifyPermission(_ call: CAPPluginCall) {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { granted, _ in
            call.resolve(["granted": granted])
        }
    }

    /// Replaces all scheduled reminder notifications with `items`:
    /// [{ id: String, title: String, body: String, at: epoch milliseconds }]
    @objc func notifySchedule(_ call: CAPPluginCall) {
        let items = call.getArray("items", JSObject.self) ?? []
        let center = UNUserNotificationCenter.current()
        center.getPendingNotificationRequests { pending in
            let old = pending.map { $0.identifier }.filter { $0.hasPrefix(self.reminderPrefix) }
            center.removePendingNotificationRequests(withIdentifiers: old)

            let now = Date().timeIntervalSince1970
            var added = 0
            // iOS keeps at most 64 pending notifications per app.
            for item in items.prefix(60) {
                guard let id = item["id"] as? String,
                      let atMs = (item["at"] as? NSNumber)?.doubleValue else { continue }
                let seconds = atMs / 1000 - now
                if seconds < 5 { continue }
                let content = UNMutableNotificationContent()
                content.title = item["title"] as? String ?? "छाया"
                content.body = item["body"] as? String ?? ""
                content.sound = .default
                if #available(iOS 15.0, *) { content.interruptionLevel = .timeSensitive }
                let trigger = UNTimeIntervalNotificationTrigger(timeInterval: seconds, repeats: false)
                center.add(UNNotificationRequest(identifier: self.reminderPrefix + id, content: content, trigger: trigger))
                added += 1
            }
            call.resolve(["scheduled": added])
        }
    }

    @objc func notifyCancelAll(_ call: CAPPluginCall) {
        let center = UNUserNotificationCenter.current()
        center.getPendingNotificationRequests { pending in
            center.removePendingNotificationRequests(withIdentifiers:
                pending.map { $0.identifier }.filter { $0.hasPrefix(self.reminderPrefix) })
            call.resolve()
        }
    }
}

/// Finds, connects and keeps reconnecting to the Chhaya button box.
///
/// Three ways to find it, all tried at once:
///  1. the box is already connected to the iPad (paired in Settings for the volume knob),
///  2. the box this iPad used last time (a pending connect never times out on iOS,
///     so it reconnects by itself as soon as the box is powered on again),
///  3. a scan for any box advertising the Chhaya service.
final class BoxLink: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    static let service = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let rxChar = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")   // page -> box
    static let txChar = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")   // box -> page
    private static let savedKey = "chhaya.box.identifier"

    private let onState: (String, String) -> Void
    private let onLine: (String) -> Void
    private let onLog: (String) -> Void
    private var watchdog: Timer?
    private var attemptStarted: [UUID: Date] = [:]

    private var central: CBCentralManager?
    private var candidates: [UUID: CBPeripheral] = [:]   // strong refs while connecting
    private var peripheral: CBPeripheral?                 // the ready box
    private var restored: [CBPeripheral] = []
    private var rx: CBCharacteristic?
    private var state = "off"
    private var inBuffer = Data()
    private var outQueue: [Data] = []
    private var writing = false

    init(onState: @escaping (String, String) -> Void, onLine: @escaping (String) -> Void,
         onLog: @escaping (String) -> Void) {
        self.onState = onState
        self.onLine = onLine
        self.onLog = onLog
        super.init()
    }

    private func log(_ msg: String) {
        print("[Chhaya box] \(msg)")
        onLog(msg)
    }

    func start() {
        if central != nil { return }
        central = CBCentralManager(delegate: self, queue: .main, options: [
            CBCentralManagerOptionShowPowerAlertKey: true,
            CBCentralManagerOptionRestoreIdentifierKey: "chhaya-box-central",
        ])
        // Never give up: while not connected, look again every 8 s, and drop connection
        // attempts that have been hanging for more than 12 s.
        watchdog = Timer.scheduledTimer(withTimeInterval: 8, repeats: true) { [weak self] _ in
            guard let self = self, self.peripheral == nil, let c = self.central, c.state == .poweredOn else { return }
            let now = Date()
            for (id, p) in self.candidates where now.timeIntervalSince(self.attemptStarted[id] ?? now) > 12 {
                self.log("connection to \(p.name ?? "box") timed out, trying again")
                c.cancelPeripheralConnection(p)
                self.candidates.removeValue(forKey: id)
                self.attemptStarted.removeValue(forKey: id)
            }
            self.search(rescan: true)
        }
    }

    func status() -> [String: Any] {
        ["state": state, "name": peripheral?.name ?? ""]
    }

    func forget() {
        UserDefaults.standard.removeObject(forKey: Self.savedKey)
        if let p = peripheral { central?.cancelPeripheralConnection(p) }
        for p in candidates.values { central?.cancelPeripheralConnection(p) }
        candidates.removeAll()
        resetLink()
        search()
    }

    /// Queues one text line for the box; returns false when no box is connected.
    func write(_ line: String) -> Bool {
        guard let p = peripheral, rx != nil, p.state == .connected else { return false }
        var data = Data(line.utf8)
        if data.last != 0x0A { data.append(0x0A) }
        // Small chunks with a response each: the box joins them until the newline.
        let size = 100
        var i = 0
        while i < data.count {
            outQueue.append(data.subdata(in: i..<min(i + size, data.count)))
            i += size
        }
        pump()
        return true
    }

    private func pump() {
        guard !writing, let p = peripheral, let rx = rx, !outQueue.isEmpty else { return }
        writing = true
        p.writeValue(outQueue.removeFirst(), for: rx, type: .withResponse)
    }

    private func setState(_ s: String) {
        if s == state { return }
        state = s
        onState(s, peripheral?.name ?? "")
    }

    private func resetLink() {
        peripheral = nil
        rx = nil
        inBuffer.removeAll()
        outQueue.removeAll()
        writing = false
    }

    private func search(rescan: Bool = false) {
        guard let c = central, c.state == .poweredOn, peripheral == nil else { return }
        setState("searching")
        // A scan reports each box only once, so restart it to hear the box again after a failure.
        if rescan && c.isScanning { c.stopScan() }
        for p in restored where p.state == .connected { connect(p) }
        restored.removeAll()
        for p in c.retrieveConnectedPeripherals(withServices: [Self.service]) { connect(p) }
        if let s = UserDefaults.standard.string(forKey: Self.savedKey), let id = UUID(uuidString: s) {
            for p in c.retrievePeripherals(withIdentifiers: [id]) { connect(p) }
        }
        if !c.isScanning {
            c.scanForPeripherals(withServices: [Self.service], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        }
    }

    private func connect(_ p: CBPeripheral) {
        guard peripheral == nil, candidates[p.identifier] == nil else { return }
        log("connecting to \(p.name ?? p.identifier.uuidString.prefix(8).description)")
        candidates[p.identifier] = p
        attemptStarted[p.identifier] = Date()
        p.delegate = self
        central?.connect(p, options: nil)
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        let names: [CBManagerState: String] = [.poweredOn: "on", .poweredOff: "off", .unauthorized: "not allowed",
                                               .unsupported: "unsupported", .resetting: "resetting", .unknown: "starting"]
        log("iPad Bluetooth: \(names[c.state] ?? "?")")
        switch c.state {
        case .poweredOn: search()
        case .unauthorized: resetLink(); setState("unauthorized")
        case .poweredOff: resetLink(); candidates.removeAll(); setState("bluetooth-off")
        case .unsupported: setState("unsupported")
        default: break
        }
    }

    func centralManager(_ c: CBCentralManager, willRestoreState dict: [String: Any]) {
        // iOS relaunched us in the background with the box still attached: keep using it.
        // search() finds it again via retrieveConnectedPeripherals once Bluetooth reports "on".
        restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral]) ?? []
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        if candidates[p.identifier] == nil && peripheral == nil {
            log("found \((advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? "a box") (signal \(RSSI))")
        }
        connect(p)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected, reading the box's services")
        p.discoverServices([Self.service])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log("connection failed: \(error?.localizedDescription ?? "unknown")")
        candidates.removeValue(forKey: p.identifier)
        attemptStarted.removeValue(forKey: p.identifier)
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.search(rescan: true) }
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        candidates.removeValue(forKey: p.identifier)
        attemptStarted.removeValue(forKey: p.identifier)
        guard p.identifier == peripheral?.identifier else {
            // Dropped before it was ready (often a stale pairing on the iPad): try again.
            log("box disconnected before it was ready: \(error?.localizedDescription ?? "no reason given")")
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.search(rescan: true) }
            return
        }
        log("box disconnected: \(error?.localizedDescription ?? "no reason given")")
        resetLink()
        setState("searching")
        // Reconnects by itself as soon as the box is back in range / powered on.
        connect(p)
        search()
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == Self.service }) else {
            log("that device has no Chhaya service\(error.map { ": \($0.localizedDescription)" } ?? "")")
            // Not a Chhaya box after all.
            candidates.removeValue(forKey: p.identifier)
            central?.cancelPeripheralConnection(p)
            return
        }
        p.discoverCharacteristics([Self.rxChar, Self.txChar], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        guard let chars = svc.characteristics,
              let rx = chars.first(where: { $0.uuid == Self.rxChar }),
              let tx = chars.first(where: { $0.uuid == Self.txChar }) else {
            central?.cancelPeripheralConnection(p)
            return
        }
        if peripheral != nil && peripheral?.identifier != p.identifier {
            // Another box won the race.
            candidates.removeValue(forKey: p.identifier)
            central?.cancelPeripheralConnection(p)
            return
        }
        peripheral = p
        self.rx = rx
        p.setNotifyValue(true, for: tx)
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
        guard ch.uuid == Self.txChar, p.identifier == peripheral?.identifier else { return }
        if let error = error {
            log("box notify failed: \(error.localizedDescription)")
            central?.cancelPeripheralConnection(p)
            return
        }
        UserDefaults.standard.set(p.identifier.uuidString, forKey: Self.savedKey)
        log("box ready ✓")
        attemptStarted.removeAll()
        central?.stopScan()
        for (id, other) in candidates where id != p.identifier { central?.cancelPeripheralConnection(other) }
        candidates = [p.identifier: p]
        setState("connected")
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        guard ch.uuid == Self.txChar, let value = ch.value else { return }
        inBuffer.append(value)
        while let nl = inBuffer.firstIndex(of: 0x0A) {
            let lineData = inBuffer.subdata(in: inBuffer.startIndex..<nl)
            inBuffer.removeSubrange(inBuffer.startIndex...nl)
            let line = String(decoding: lineData, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            if !line.isEmpty { onLine(line) }
        }
        if inBuffer.count > 512 { inBuffer.removeAll() }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let error = error { print("[Chhaya] box write failed: \(error)") }
        writing = false
        pump()
    }
}

/// One recording at a time, with the same pause detection the page used:
/// calibrate on the room for 250 ms, count speech above the room level,
/// stop after `silenceMs` of quiet (a bit longer after a very short start).
final class NativeMic: NSObject {
    struct Options { let maxMs: Double; let silenceMs: Double; let noSpeechMs: Double; let minSpeechMs: Double }

    private let onLevel: (Double) -> Void
    private var recorder: AVAudioRecorder?
    private var timer: Timer?
    private var done: (([String: Any]) -> Void)?
    private var opts = Options(maxMs: 15000, silenceMs: 1000, noSpeechMs: 8000, minSpeechMs: 300)
    private var fileURL: URL?
    private var t0 = Date(), lastTick = Date(), lastVoice = Date()
    private var spoke = false, voicedMs = 0.0, cancelled = false
    private var calib: [Float] = []
    private var noiseDb: Float = -50   // running estimate of the room, in dB (0 = loudest)
    private var tickCount = 0

    init(onLevel: @escaping (Double) -> Void) { self.onLevel = onLevel }

    func record(_ o: Options, completion: @escaping ([String: Any]) -> Void) {
        if recorder != nil { finish(keep: false) }   // only one recording at a time
        let session = AVAudioSession.sharedInstance()
        switch session.recordPermission {
        case .undetermined:
            session.requestRecordPermission { granted in
                DispatchQueue.main.async {
                    if granted { self.record(o, completion: completion) }
                    else { completion(["empty": true, "reason": "microphone permission denied"]) }
                }
            }
            return
        case .denied:
            return completion(["empty": true, "reason": "microphone permission denied (Settings → Chhaya → Microphone)"])
        default: break
        }
        opts = o
        done = completion
        cancelled = false
        do {
            try session.setCategory(.playAndRecord, mode: .default,
                                    options: [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers])
            try session.setActive(true)
        } catch {
            return end(["empty": true, "reason": "session: \(error.localizedDescription)"])
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("chhaya-\(UUID().uuidString).m4a")
        let settings: [String: Any] = [
            AVFormatIDKey: Int(kAudioFormatMPEG4AAC),
            AVSampleRateKey: 16000,
            AVNumberOfChannelsKey: 1,
            AVEncoderAudioQualityKey: AVAudioQuality.medium.rawValue,
        ]
        do {
            let r = try AVAudioRecorder(url: url, settings: settings)
            r.isMeteringEnabled = true
            guard r.record() else { return end(["empty": true, "reason": "recorder would not start (microphone permission?)"]) }
            recorder = r
            fileURL = url
        } catch {
            return end(["empty": true, "reason": "recorder: \(error.localizedDescription)"])
        }
        t0 = Date(); lastTick = t0; lastVoice = t0
        spoke = false; voicedMs = 0; calib = []; tickCount = 0
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in self?.tick() }
    }

    func cancel() {
        cancelled = true
        if recorder != nil { finish(keep: false) }
    }

    private func tick() {
        guard let r = recorder else { return }
        r.updateMeters()
        let db = r.averagePower(forChannel: 0)
        let now = Date()
        let elapsed = now.timeIntervalSince(t0) * 1000
        if elapsed < 250 {
            calib.append(db)
        } else {
            if !calib.isEmpty {
                let sorted = calib.sorted()
                noiseDb = min(-20, 0.5 * noiseDb + 0.5 * sorted[sorted.count / 2])
                calib = []
            }
            let onDb = max(noiseDb + 12, -42)    // to start counting as speech
            let keepDb = max(noiseDb + 7, -48)   // softer words while already speaking
            let isVoice = db > (spoke ? keepDb : onDb)
            if isVoice {
                spoke = true
                lastVoice = now
                voicedMs += now.timeIntervalSince(lastTick) * 1000
            } else if !spoke {
                noiseDb = noiseDb * 0.995 + db * 0.005   // learn the room while waiting
            }
            let needSilence = voicedMs < 1200 ? opts.silenceMs + 400 : opts.silenceMs
            let quietFor = now.timeIntervalSince(lastVoice) * 1000
            if (spoke && quietFor > needSilence) || elapsed > opts.maxMs || (!spoke && elapsed > opts.noSpeechMs) {
                return finish(keep: true)
            }
        }
        lastTick = now
        tickCount += 1
        if tickCount % 2 == 0 {   // ~10 level updates a second for the face's glow
            onLevel(Double(max(0, min(1, (db + 55) / 45))))
        }
    }

    private func finish(keep: Bool) {
        timer?.invalidate(); timer = nil
        let duration = Date().timeIntervalSince(t0) * 1000
        recorder?.stop(); recorder = nil
        onLevel(0)
        defer { if let u = fileURL { try? FileManager.default.removeItem(at: u) }; fileURL = nil }
        guard keep, !cancelled, spoke, voicedMs >= opts.minSpeechMs,
              let u = fileURL, let data = try? Data(contentsOf: u), !data.isEmpty else {
            return end(["empty": true, "reason": cancelled ? "cancelled" : (spoke ? "too short" : "no speech"),
                        "voicedMs": voicedMs, "durationMs": duration, "noiseDb": Double(noiseDb)])
        }
        end(["audio": data.base64EncodedString(), "mime": "audio/mp4", "voicedMs": voicedMs,
             "durationMs": duration, "noiseDb": Double(noiseDb)])
    }

    private func end(_ result: [String: Any]) {
        let d = done
        done = nil
        d?(result)
    }
}

/// Continuous speech recognition for the wake word, like "Hey Siri": audio never leaves the
/// iPad when on-device Hindi recognition is available. Each recognition session is kept short
/// and restarted, so the transcript stays small and a stalled session heals by itself.
final class NativeWake: NSObject {
    private let onText: (String, Bool) -> Void
    // A fresh engine for every session, created AFTER the audio session is set to record:
    // an engine made while the session was playback-only reports "no microphone" forever.
    private var engine: AVAudioEngine?
    private var recognizer: SFSpeechRecognizer?
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var task: SFSpeechRecognitionTask?
    private var restartTimer: Timer?
    private var wanted = false
    private var session = 0
    private var hints: [String] = []
    private(set) var onDevice = false

    init(onText: @escaping (String, Bool) -> Void) { self.onText = onText }

    func start(locale: String, hints: [String], completion: @escaping (String?) -> Void) {
        self.hints = hints
        switch SFSpeechRecognizer.authorizationStatus() {
        case .notDetermined:
            SFSpeechRecognizer.requestAuthorization { _ in
                DispatchQueue.main.async { self.start(locale: locale, hints: hints, completion: completion) }
            }
            return
        case .denied, .restricted:
            return completion("speech recognition not allowed (Settings → Chhaya → Speech Recognition)")
        default: break
        }
        guard let rec = SFSpeechRecognizer(locale: Locale(identifier: locale)), rec.isAvailable else {
            return completion("\(locale) speech recognition not available on this iPad")
        }
        recognizer = rec
        onDevice = rec.supportsOnDeviceRecognition
        wanted = true
        if let error = begin() { wanted = false; return completion(error) }
        completion(nil)
    }

    func stop() {
        wanted = false
        end()
    }

    /// Starts one recognition session; returns an error text if the microphone can't start.
    private func begin() -> String? {
        end()
        guard wanted, let rec = recognizer else { return "not started" }
        let audio = AVAudioSession.sharedInstance()
        do {
            try audio.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers])
            try audio.setActive(true)
        } catch { return "audio session: \(error.localizedDescription)" }

        let req = SFSpeechAudioBufferRecognitionRequest()
        req.shouldReportPartialResults = true
        req.taskHint = .search
        req.contextualStrings = hints            // makes "छाया" much more likely to be recognised
        if rec.supportsOnDeviceRecognition { req.requiresOnDeviceRecognition = true }
        request = req

        let eng = AVAudioEngine()
        let input = eng.inputNode
        var format = input.outputFormat(forBus: 0)
        if format.sampleRate == 0 { format = input.inputFormat(forBus: 0) }
        guard format.sampleRate > 0, format.channelCount > 0 else {
            return "microphone not available (input \(audio.isInputAvailable ? "present" : "missing"), category \(audio.category.rawValue))"
        }
        input.installTap(onBus: 0, bufferSize: 1024, format: format) { buffer, _ in req.append(buffer) }
        eng.prepare()
        do { try eng.start() } catch {
            input.removeTap(onBus: 0)
            return "microphone: \(error.localizedDescription)"
        }
        engine = eng

        session += 1
        let mySession = session
        task = rec.recognitionTask(with: req) { [weak self] result, error in
            DispatchQueue.main.async {
                guard let self = self, mySession == self.session else { return }
                if let r = result {
                    self.onText(r.bestTranscription.formattedString, r.isFinal)
                    if r.isFinal { self.restartSoon(0.1) }
                }
                if error != nil { self.restartSoon(0.5) }
            }
        }
        // Keep each session short (server recognition stops after about a minute anyway).
        restartTimer = Timer.scheduledTimer(withTimeInterval: 25, repeats: false) { [weak self] _ in self?.restartSoon(0) }
        return nil
    }

    private func restartSoon(_ delay: TimeInterval) {
        guard wanted else { return }
        let mySession = session
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self = self, self.wanted, mySession == self.session else { return }
            if let error = self.begin() {
                print("[Chhaya] wake restart failed: \(error)")
                self.restartSoon(2)
            }
        }
    }

    private func end() {
        session += 1
        restartTimer?.invalidate(); restartTimer = nil
        if let eng = engine {
            if eng.isRunning { eng.stop() }
            eng.inputNode.removeTap(onBus: 0)
            engine = nil
        }
        request?.endAudio(); request = nil
        task?.cancel(); task = nil
    }
}
