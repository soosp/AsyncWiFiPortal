# AsyncWiFiPortal

Recovery-mode Wi-Fi setup for ESP32 and ESP8266. When a device has no other way
in, trigger the portal — with a button, or from a still-working web UI: it
reboots into a soft-AP with a captive portal, you pick the network from your
phone, the device tests the credentials and saves them, then reboots onto your
network.

- **Reboot into the portal**, not alongside the live system — so there is no
  AP+STA coexistence or port contention to manage. Ending the portal is just a
  restart.
- A **captive portal** that pops on Android / iOS / Windows, a small form (SSID,
  password, hostname; optional static IP and TX power), and a **two-phase test**
  that only writes to flash once the connection actually works.
- **ESP32 and ESP8266**, one code path (with per-radio behaviour where the
  hardware forces it).

For *why* it is built this way, and the pitfalls, see [`docs/DESIGN.md`](docs/DESIGN.md).

## Origin and styling

AsyncWiFiPortal grew out of [`AsyncConfigPortal`](https://github.com/soosp/AsyncConfigPortal)
— the recovery path for Wi-Fi-only devices that could otherwise become
unreachable — but it stands on its own and needs neither AsyncConfigPortal nor a
host web server.

When the two are built together it reuses AsyncConfigPortal's stylesheet, so the
portal matches the rest of the UI; on its own it falls back to a small built-in
one that defines the same class names.

A house style is set on the portal itself — `setCssExtra()` appends your snippet,
`setCss()` replaces the sheet. It cannot inherit the config server's supplement:
the recovery portal runs in its own boot, where that server does not exist. Pass
the same PROGMEM string to both and the two match:

```cpp
web.setCssExtra(MY_CSS);       // the config portal, in the normal boot
portal.setCssExtra(MY_CSS);    // the recovery portal, before run()
```

`WIFI_PORTAL_CSS` still replaces the sheet at compile time if you want it out of
flash — this library is header-only, so a `#define` before the include is enough.
See AsyncConfigPortal's `docs/CSS.md` for the class contract. Only the stylesheet
is shared — the portal always writes its own page and header.

## Install

The two manifests name the same libraries differently: PlatformIO takes an
`owner/name`, the Arduino Library Manager takes the library's own `name=` field.

|PlatformIO|Arduino Library Manager|Notes|
|---|---|---|
|`soosp/NetworkProfile`|`NetworkProfile`|provides `WiFiProfile`|
|`ESP32Async/AsyncTCP`|`Async TCP`|ESP32 only|
|`ESP32Async/ESPAsyncTCP`|`ESP Async TCP`|ESP8266 only|
|`ESP32Async/ESPAsyncWebServer`|`ESP Async WebServer`|both platforms|
|`vshymanskyy/Preferences`|`Preferences`|ESP8266 only; the core provides it on ESP32|

`DNSServer` is bundled with both cores and needs no entry.

Pin the ESP32Async forks. Several incompatible async-TCP libraries sit in the
Library Manager under names a glance cannot tell apart — `Async TCP`,
`ESP Async TCP`, `ESP AsyncTCP` — and the web server declares no `depends=` of
its own, so nothing installs the right one for you. Both are named here for that
reason; the one your board does not use is installed but never compiled, since
nothing includes its header.

PlatformIO: add `soosp/AsyncWiFiPortal` to `lib_deps`, which pulls the rest.
Arduino IDE: install AsyncWiFiPortal from the Library Manager.

## Quick start

Check the portal state at the top of `setup()`: enter the portal if it was
requested, otherwise connect with the saved config. A button press requests the
portal and reboots.

```cpp
#include <WiFiProfile.h>
#include <AsyncWiFiPortal.h>

WiFiProfile profile;
AsyncWiFiPortal portal;
const char* ns = "wifi";                     // namespace shared by portal + profile

void setup() {
    portal.setProfileNamespace(ns);

    switch (portal.state()) {
        case AsyncWiFiPortal::State::Requested:
            portal.setApSecurity(AsyncWiFiPortal::ApSecurity::Open);
            portal.run("Setup-AP");          // brings up the portal; does NOT return
            break;
        case AsyncWiFiPortal::State::Running:     // interrupted (power loss/crash): do NOT
        case AsyncWiFiPortal::State::Saved:       // auto-re-enter — a crash on entry would
        case AsyncWiFiPortal::State::Abandoned:   // loop. Boot normally; retry via the button.
            portal.clear();                  // acknowledge the last outcome
            break;
        default:
            break;                           // None — normal boot
    }

    // Normal boot: connect with the stored config, if any.
    if (profile.loadCfg(ns)) {
        WiFiProfile::WiFiConfig cfg;
        profile.getConfig(cfg);
        if (!cfg.dhcp)
            WiFi.config(cfg.ip, cfg.gateway, cfg.mask, cfg.dns[0], cfg.dns[1]);
        WiFi.begin(cfg.ssid, cfg.password);
    }
}

void loop() {
    // In a button ISR, set a flag; here in loop() call portal.rebootToPortal().
}
```

See [`examples/PortalButton`](examples/PortalButton) for a complete sketch with
the button wired up.

## API

|Member|What it does|
|---|---|
|`State state() const`|The stored state, no side effect. Branch on it at boot.|
|`void clear()`|Acknowledge the outcome (write `None`).|
|`void rebootToPortal()`|Record a request and restart into the portal. **Not from an ISR.**|
|`void setApSecurity(ApSecurity mode, const char* secret = nullptr)`|AP security; call before `run()`.|
|`void setTimeout(uint32_t ms)`|Idle timeout before `Abandoned` (default 180 s).|
|`void setCss(const char* css)`|Replace the stylesheet (PROGMEM); `nullptr` restores the default.|
|`void setCssExtra(const char* css)`|Append an application stylesheet (PROGMEM) after it.|
|`void setProfileNamespace(const char* ns)`|Namespace the config is saved to (default `"net_wifi"`).|
|`void run(const char* apSsid)`|Enter portal mode. Owns the device and restarts when done — **does not return**.|

```cpp
enum class State      { None, Requested, Running, Saved, Abandoned };
enum class ApSecurity { Open, Passphrase, MacDerived };
```

Compile-time options (override with `-D`):

|Macro|Default|Meaning|
|---|---|---|
|`WIFI_PORTAL_NVS_NS`|`"wifiportal"`|NVS namespace for the portal's own state (must survive a factory reset).|
|`WIFI_PORTAL_IDLE_MS`|`180000`|Idle timeout.|
|`WIFI_PORTAL_TEST_MS`|`15000`|Connection-test timeout.|
|`WIFI_PORTAL_RESULT_MS`|`10000`|How long the AP stays up after success, so a phone that dropped can reconnect and see the result.|
|`WIFI_PORTAL_AP_TX_DBM`|`8`|Setup-AP TX power — kept low so weak-antenna boards stay reachable.|

## Caller rules

1. **Enter early.** Run the `state()` / `run()` branch as the first thing in
   `setup()`, before starting your own tasks or radio — `run()` owns the device.
2. **Never call `rebootToPortal()` from an ISR.** It writes NVS and restarts,
   neither of which is ISR-safe; set a flag in the ISR and call it from `loop()`.

## Connecting: a platform difference to expect

Testing the credentials briefly runs AP+STA, and a single radio has to move to
the router's channel. On **ESP32** the AP announces the move (a Channel Switch
Announcement), so the phone follows and the page shows the live result — green
*Connected* with the device's IP address, or red *Not connected*. On **ESP8266**
there is no such announcement: the phone drops when the channel changes and its
OS usually rejoins a *saved* network, so the live result often does not appear —
but the device still connects and saves in the background, then reboots onto the
network. After a wrong password on ESP8266 the user has to reselect the setup AP
by hand to retry. This is a hardware limitation, not a bug; see
[`docs/DESIGN.md`](docs/DESIGN.md) §7.

## AP security

Presence — radio range plus a person at the device — is the primary gate. The AP
password is a secondary barrier the deployment picks:

- **Open** (default) — no passphrase; for a trusted physical space.
- **Passphrase** — one fleet-wide value: `setApSecurity(ApSecurity::Passphrase, "secret")`.
- **MacDerived** — a per-device password from the factory base MAC and a salt:
  `setApSecurity(ApSecurity::MacDerived, "salt")`. Print the labels ahead of
  deployment with [`tools/genpass.py`](tools/genpass.py) — feed it the base/STA
  MAC (the one printed on the chip), not the AP BSSID. Firmware and tool produce
  identical passwords.

## License

MIT
