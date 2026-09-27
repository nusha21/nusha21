import Foundation
import CoreBluetooth
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
    ]

    private lazy var box = BoxLink(
        onState: { [weak self] state, name in
            self?.notifyListeners("boxState", data: ["state": state, "name": name])
        },
        onLine: { [weak self] line in
            self?.notifyListeners("boxLine", data: ["line": line])
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

    private var central: CBCentralManager?
    private var candidates: [UUID: CBPeripheral] = [:]   // strong refs while connecting
    private var peripheral: CBPeripheral?                 // the ready box
    private var restored: [CBPeripheral] = []
    private var rx: CBCharacteristic?
    private var state = "off"
    private var inBuffer = Data()
    private var outQueue: [Data] = []
    private var writing = false

    init(onState: @escaping (String, String) -> Void, onLine: @escaping (String) -> Void) {
        self.onState = onState
        self.onLine = onLine
        super.init()
    }

    func start() {
        if central != nil { return }
        central = CBCentralManager(delegate: self, queue: .main, options: [
            CBCentralManagerOptionShowPowerAlertKey: true,
            CBCentralManagerOptionRestoreIdentifierKey: "chhaya-box-central",
        ])
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

    private func search() {
        guard let c = central, c.state == .poweredOn, peripheral == nil else { return }
        setState("searching")
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
        candidates[p.identifier] = p
        p.delegate = self
        central?.connect(p, options: nil)
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
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
        connect(p)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        p.discoverServices([Self.service])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        candidates.removeValue(forKey: p.identifier)
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.search() }
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        candidates.removeValue(forKey: p.identifier)
        guard p.identifier == peripheral?.identifier else { return }
        resetLink()
        setState("searching")
        // Reconnects by itself as soon as the box is back in range / powered on.
        connect(p)
        search()
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == Self.service }) else {
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
            print("[Chhaya] box notify failed: \(error)")
            central?.cancelPeripheralConnection(p)
            return
        }
        UserDefaults.standard.set(p.identifier.uuidString, forKey: Self.savedKey)
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
