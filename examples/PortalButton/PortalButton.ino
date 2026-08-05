// PortalButton — minimal AsyncWiFiPortal demo.
//
// Normal boot: load the saved Wi-Fi config and connect. Press the button (GPIO0,
// the BOOT button on most dev boards) to reboot into the configuration portal,
// enter credentials from a phone, and have the device connect and save them.
//
// Shows the reboot-into-portal pattern: the app asks for the portal and restarts;
// the next boot runs the portal alone; when it saves, the device restarts again
// and the app connects with the new config. See DESIGN.md.

#include <Arduino.h>
#include <WiFiProfile.h>
#include <AsyncWiFiPortal.h>

// The button interrupt handler must live in IRAM; the attribute differs by core.
#ifdef ARDUINO_ARCH_ESP32
#define ISR_ATTR ARDUINO_ISR_ATTR
#elif (defined ARDUINO_ARCH_ESP8266)
#define ISR_ATTR IRAM_ATTR
#else
#error "Unsupported architecture."
#endif

const int   buttonPin  = 0;             // GPIO0 = BOOT button, active low
const char* apSsid     = "AsyncWiFiPortal";  // the setup AP the phone joins
const char* apPassword = "12345678";    // AP passphrase (>= 8 chars for WPA2)
const char* nvsNs      = "wifi";        // Preferences namespace for the Wi-Fi config

volatile bool buttonPressed   = false;  // set in the ISR, handled in loop()
bool          alreadyConnected = false;

WiFiProfile::WiFiConfig cfg;
WiFiProfile             profile;
AsyncWiFiPortal         portal;

// Keep the ISR trivial: rebootToPortal() writes NVS and restarts, neither of
// which is ISR-safe, so it is deferred to loop() via this flag.
void ISR_ATTR buttonISR() {
    buttonPressed = true;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\nAsyncWiFiPortal button demo started."));

    // The portal saves to this namespace; the profile below loads from the
    // same one, so the two must match.
    portal.setProfileNamespace(nvsNs);

    // Boot-time state machine. Run this before bringing up anything else, so
    // that when the portal runs it owns the device alone.
    switch (portal.state()) {
        case AsyncWiFiPortal::State::None:
            break;                       // nothing pending — normal boot
        case AsyncWiFiPortal::State::Requested:
            // The portal was requested (button press last boot). run() brings
            // up the AP + captive portal, tests the credentials, saves on
            // success, and restarts — it does not return. Configure the AP
            // security first; Open / Passphrase / MacDerived are the three
            // modes.
            Serial.println(F("Configuration portal starts."));
            Serial.print(F("SSID:  ")); Serial.println(apSsid);
            portal.setApSecurity(AsyncWiFiPortal::ApSecurity::Passphrase, apPassword);
            portal.run(apSsid);
            break;                       // not reached (run restarts)
        case AsyncWiFiPortal::State::Running:
            // A previous portal session was cut short (power loss or a crash).
            Serial.println(F("Configuration portal's last run was interrupted."));
            portal.clear();
            break;
        case AsyncWiFiPortal::State::Abandoned:
            Serial.println(F("Configuration portal timed out previously."));
            portal.clear();
            break;
        case AsyncWiFiPortal::State::Saved:
            // The portal saved a config last run; loadCfg() below picks it up.
            Serial.println(F("Configuration portal's last run was successful."));
            portal.clear();
            break;
    }

    pinMode(buttonPin, INPUT_PULLUP);
    attachInterrupt(buttonPin, buttonISR, FALLING);
    Serial.println(F("Press the button to reboot to the configuration portal."));

    // Normal boot: connect with the stored config, if any.
    if (profile.loadCfg(nvsNs)) {
        profile.getConfig(cfg);
        Serial.println(F("Saved WiFi configuration found and loaded."));
        Serial.print(F("Connecting to network ")); Serial.println(cfg.ssid);
        if (!cfg.dhcp) {
            // Static addressing. This uses two DNS servers, which assumes
            // NetworkProfile was built with DNS_SERVER_COUNT >= 2 (the default
            // on ESP). With a single DNS server, drop cfg.dns[1].
            WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
        }
        WiFi.begin(cfg.ssid, cfg.password);
        // Restore the saved TX power. Some ESP32 boards have a poor antenna
        // design and are unstable at full power, so the config can lower it.
        // The API differs per platform (ESP32 discrete level, ESP8266 dBm).
#if defined(ARDUINO_ARCH_ESP32)
        WiFi.setTxPower(static_cast<wifi_power_t>(cfg.txPower * WiFiProfile::WIFI_TX_POWER_MULTIPLIER));
#elif defined(ARDUINO_ARCH_ESP8266)
        WiFi.setOutputPower(cfg.txPower);
#endif
    }
}

void loop() {
    // Handle the button press here, not in the ISR (see buttonISR).
    if (buttonPressed) {
        Serial.println(F("Button pressed, rebooting to the configuration portal."));
        portal.rebootToPortal();         // writes Requested, then restarts
    }
    if (WiFi.isConnected() && !alreadyConnected) {
        Serial.print(F("Connected to network ")); Serial.println(cfg.ssid);
        alreadyConnected = true;
    }
    delay(10);
}
