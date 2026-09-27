import UIKit
import AVFoundation
import Capacitor

/// The app's main screen: Capacitor's web view plus a few things a kiosk-style
/// companion needs (screen always on, no status bar, loudspeaker audio,
/// and our own native plugin for the button box and reminders).
class ChhayaViewController: CAPBridgeViewController {

    override func capacitorDidLoad() {
        bridge?.registerPluginInstance(ChhayaNativePlugin())
    }

    override func viewDidLoad() {
        isStatusBarVisible = false
        super.viewDidLoad()
        configureAudioSession()
        webView?.isOpaque = false
        webView?.backgroundColor = UIColor(red: 1.0, green: 0.965, blue: 0.925, alpha: 1)
        webView?.scrollView.bounces = false
        webView?.allowsBackForwardNavigationGestures = false
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        // Chhaya must keep hearing "छाया", so the iPad never auto-locks while the app is open.
        UIApplication.shared.isIdleTimerDisabled = true
    }

    /// Speech and music through the loudspeaker, even while the microphone is on,
    /// and keep playing if the iPad's side switch / silent mode is set.
    private func configureAudioSession() {
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playAndRecord, mode: .default,
                                    options: [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers])
            try session.setActive(true)
        } catch {
            print("[Chhaya] audio session setup failed: \(error)")
        }
    }
}
