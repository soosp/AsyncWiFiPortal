# Changelog

All notable changes to this project will be documented in this file.

The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-08-04

First release. The API may still change before 1.0.0.

### Added

- Reboot-into-portal recovery for ESP32 and ESP8266: the application requests the
  portal with `rebootToPortal()` and restarts; the next boot runs the portal
  alone, connects, saves, and reboots onto the network. A persisted five-state
  machine (`state()`, `clear()`) reports the outcome and is self-limiting across
  crashes and power loss.
- Captive portal: a soft-AP with the core's built-in DHCP, a wildcard DNS, and a
  Host-header redirect that pops the sign-in window on Android, iOS and Windows.
- Configuration form: SSID, password, hostname; optional static addressing
  (IP / mask / gateway / DNS) with client- and server-side coherence checks; and
  TX power — a drop-down of the discrete levels on ESP32, a bounded number input
  on ESP8266.
- Two-phase connection test, run outside the request handler: credentials are
  tried in RAM and written to flash only on success, so a mistyped password never
  overwrites a working configuration. Includes a failure taxonomy (not found /
  rejected / DHCP / timeout), a live green/red result showing the device's IP on
  success (following the ESP32 Channel Switch Announcement), and a "save anyway"
  escape hatch for false negatives.
- Three access-point security modes chosen by the deployment: `Open`,
  `Passphrase` (one fleet-wide value), and `MacDerived` (a per-device password
  from the factory base MAC and a salt). `tools/genpass.py` prints matching
  labels; firmware and tool share a bundled HMAC-SHA256 + Crockford base32 and
  produce byte-identical passwords on both platforms.
- Low-power setup AP (`WIFI_PORTAL_AP_TX_DBM`, default 8 dBm) so a weak-antenna
  board stays reachable while it is being configured.
- Configuration through `setApSecurity()`, `setTimeout()`,
  `setProfileNamespace()` and the `WIFI_PORTAL_*` compile-time macros. The
  stylesheet is reused from `AsyncConfigPortal` when it is present, falls back to
  a compact built-in one otherwise, and can be overridden with `WIFI_PORTAL_CSS`.

### Known limitations

- On ESP8266 there is no Channel Switch Announcement, so the phone drops during
  the connection test and the live result may not appear — the device still
  connects and saves in the background. See `docs/DESIGN.md` §7.
- The static-address reachability probe (a gateway ARP/ICMP check, and ACD on an
  isolated segment) is designed but not yet implemented; a static connection
  currently reports success on association plus a set address.

[Unreleased]: https://github.com/soosp/AsyncWiFiPortal/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/soosp/AsyncWiFiPortal/releases/tag/v0.1.0
