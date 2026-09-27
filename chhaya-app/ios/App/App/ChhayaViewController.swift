import UIKit
import Capacitor

/// The app's main screen: Capacitor's web view plus a few things a kiosk-style
/// companion needs (screen always on, no status bar,
/// and our own native plugin for the button box and reminders).
class ChhayaViewController: CAPBridgeViewController {

    override func capacitorDidLoad() {
        bridge?.registerPluginInstance(ChhayaNativePlugin())
    }

    override func viewDidLoad() {
        isStatusBarVisible = false
        super.viewDidLoad()
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
}
