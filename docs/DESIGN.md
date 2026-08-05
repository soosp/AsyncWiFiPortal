# AsyncWiFiPortal — design notes

Recovery-mode WiFi setup for devices that have no other way in: a soft-AP, a
captive portal, and just enough form to get the radio onto a network.

This document records **why** things are the way they are. The conclusions are
short; the reasoning is the part that is expensive to reconstruct later.

---

## 1. A separate library, not a component

`AsyncConfigPortal` is built for composition: a component registers pages and
endpoints on the host server. The portal does none of that. It runs its own
`AsyncWebServer`, serves its own single page, and never calls `addPage()`,
`addPostHandler()` or `setNotFoundHandler()`. **A component that does not use
its host is not a component.**

Its dependencies are `WiFiProfile`, `DNSServer` and `ESPAsyncWebServer`. Putting
it inside `NetworkManager` would make every Ethernet-only build pay for the DNS
server and the web server — which is precisely the cost that
`NetConfigComponent`
was kept opt-in to avoid. Same pattern, third application.

### Why not reuse the main server's pages

Considered and rejected. Sharing the pages would mean depending on
`AsyncConfigPortal`, and the portal's form is deliberately narrower anyway:
one interface, no priority, no NTP, no menu. The apparent duplication is smaller
than it looks.

---

## 2. Reboot into the portal, not alongside it

The portal does **not** run next to the live system. The application calls
`rebootToPortal()`, which records a request and restarts; the next boot brings
up
the portal and nothing else.

This removes two whole classes of problem rather than managing them:

- **Port contention.** `AsyncWebServer` binds port 80 on all interfaces. A
  portal running beside the main configuration server could not bind.
- **Radio complexity.** No AP+STA coexistence during normal operation, no
  channel negotiation against a live connection, no teardown path back to clean
  STA mode.

The device is either live or configuring. Ending the portal is therefore just
`ESP.restart()` — there is no state to unwind.

---

## 3. State, not a boolean

A single "please enter the portal" flag is not enough for two reasons: the
portal has to report back what happened, and without a distinguishable
"the user gave up" outcome an application rule like *"no connection → enter the
portal"* turns an abandoned session into a reboot loop.

```cpp
enum class State : uint8_t {   // nested in AsyncWiFiPortal (AsyncWiFiPortal::State)
    None      = 0,  ///< nothing pending, nothing to report
    Requested = 1,  ///< the app asked for it; the next boot enters the portal
    Running   = 2,  ///< the portal started (written on entry)
    Saved     = 3,  ///< a configuration was saved
    Abandoned = 4,  ///< the portal timed out with nothing saved
};
```

Sequence: the application writes `Requested` and restarts → the portal writes
`Running` **on entry** → `Saved` or `Abandoned` → restart → the application
reads
the outcome and writes `None`.

Writing `Running` on entry rather than clearing on success is what keeps the
state self-limiting:

- A session interrupted by power loss or a crash is visible at the next boot as
  `Running`, so the device never wedges in a mode it cannot report on.
- A spurious request costs one boot, not permanent residency.
- If the portal itself cannot start, there is no loop into a mode the device
  cannot serve.

`Abandoned` is what breaks the loop: the user walked away deliberately, so the
application should not drag them back. Pressing the button again is the way in.

**This requires a portal timeout**, otherwise `Abandoned` is never produced.
That
is desirable independently: an access point left open indefinitely is not a
state
anyone should have to notice.

### Storage

The state lives in its own NVS namespace and **must not** be among the
namespaces a factory reset erases.

A factory reset does **not** set `Requested`, although it was tempting: a
remotely wiped Wi-Fi-only device would then come back up as an access point
rather than unreachable. It was rejected because the portal is a
physical-presence operation, and a device that opens an unattended access point
after a remote command is exactly the case section 4 exists to avoid. Factory
reset and portal entry are both recovery operations performed by someone
standing at the device; using one to paper over the other's limits would make
both weaker. A factory-reset warning in the host application therefore stands as
the right place to note this, not an automatic portal entry.

---

## 4. The access point is open, and presence is the gate

The portal exists **because** someone is standing at the device. Entry is
triggered by a physical button, or from the still-working web interface by
someone in the room; either way the session is attended and bounded by the
timeout. Access is gated by radio range and by a person being present — which is
what a passphrase would have been trying to approximate anyway.

### Why not a generated passphrase

Considered first, and rejected on delivery rather than on principle. A
device-unique secret in NVS is unguessable and does not leak through the MAC —
but the user needs it precisely when the device is unreachable. Not from the
serial console, since avoiding that is the point of the portal; not from the web
interface, since that is what they cannot reach; not from the label, since a
generated value cannot be printed there. The only remaining channel is having
written it down beforehand, which nobody does.

A secret that cannot be delivered is not protection, only one more way to get
stuck.

### Three modes, the deployment chooses

That rejection is specific to a *random* per-device secret — unguessable but
undeliverable. A **MAC-derived** password is a different case: it *is*
deliverable, because it comes from the factory base MAC printed on the chip.
That reopens the choice, and since presence is the primary gate, the AP password
is a secondary barrier the deployment picks:

- **Open** (default) — a trusted physical space; range and presence are enough.
- **Passphrase** — the application sets one fleet-wide value. Honest about its
  weakness: a shared secret leaks across the fleet, so it is barely better than
  none once one device is handled carelessly.
- **MAC-derived** — a per-device value the deployer prints on a label. No single
  shared secret to leak. On its own the derivation is only obscurity (the
  algorithm is open source and the base MAC is knowable), so it takes an optional
  application **salt**: without it a casual barrier, with it a genuine per-device
  secret at zero per-device provisioning cost. The salt is what the end user
  cannot compute — they see only the MAC — so delivery is the printed label the
  deployer makes, which is exactly why this works where the random NVS secret did
  not.

Deriving it must be byte-identical in firmware and on the deployer's host, or the
label will not open the device:

```txt
password = crockford32( HMAC-SHA256(salt, base_mac6)[:10] )   // 16 chars
```

The MAC is the factory **base** MAC — the STA MAC (`esp_read_mac(ESP_MAC_WIFI_STA)`
on ESP32, `WiFi.macAddress()` on ESP8266), the one printed on the chip, identical
on both platforms and independent of build config. The soft-AP BSSID is *not*
used: on ESP32 it is the base MAC or base+1 depending on
`CONFIG_ESP32_UNIVERSAL_MAC_ADDRESSES`, and on ESP8266 it is a local-bit variant,
so it is neither stable nor knowable from the label. HMAC-SHA256 is a small
bundled implementation (`AsyncWiFiPortalHmac.h`, not mbedTLS/BearSSL), so the firmware
and `hmac`+`hashlib` on the host produce identical bytes everywhere. The MAC is
normalised to six raw bytes before hashing (separators dropped, lower-cased) so
any label style hashes the same, and Crockford base32 (uppercase, no `I/L/O/U`)
transcribes from a label without the `0/O`, `1/I/L` ambiguities. `tools/genpass.py`
(Python 3 stdlib, Windows/Linux/mac) is the host side: it takes the salt from the
environment — never a file — and a MAC or a CSV of MACs, and emits `mac,password`
labels. Test vector: salt `deploy-secret-2026`,
MAC `AA:BB:CC:DD:EE:FF` -> `RMVR4MXXMF690WRK`.

```txt
setApSecurity(Open) | setApSecurity(Passphrase, "...") | setApSecurity(MacDerived, salt=nullptr)
```

### The trigger carries the security property, not the portal

This is the one constraint worth stating plainly. If `rebootToPortal()` is
reachable from the web interface, it can be invoked **remotely**, and the device
then opens an unattended access point — the exact case rejected above, only
opened by the application instead of by a factory reset.

The library does not forbid this; it cannot know which trigger is wired up. But
a GPIO button makes the presence property real, while a web button makes it
merely conventional. An application that exposes the remote path should set a
passphrase.

Digest auth is unnecessary either way: the portal's surface is a single
configuration form with no OTA, no factory reset and no password change, and the
captive-portal mini-browsers of most operating systems handle digest auth badly.

## 5. Captive-portal detection: the Host header, not a URL table

Established from the tzapu/WiFiManager source rather than inferred.

### The networking layers

Three pieces stand up the portal, but only two are our code:

- **AP + DHCP** — `WiFi.softAP()` brings up the access point, and the IDF network
  layer starts its built-in DHCP server (`dhcps`) on it automatically. The client
  gets an address (192.168.4.2+ by default, the AP at 192.168.4.1) with no code
  of ours; there is no DHCP server to write.
- **Captive DNS** — the `DNSServer` dependency, bound to port 53, answers *every*
  hostname with the AP's address (`start(53, "*", apIP)`). That is what makes the
  Host-header trick below work.
- **The pages** — `AsyncWebServer`.

One thing to confirm on hardware: on some IDF versions the softAP's `dhcps` does
not hand the AP out as the client's DNS server by default (option 6). If the
captive window does not pop by itself, that option must be set explicitly on
`dhcps`; otherwise the client never queries our DNS.

### The redirect

The DNS server answers every query with the AP's address, so every request
arrives at the device carrying its *original* hostname in the `Host` header. The
not-found handler compares that header against the device's own address and
redirects with a `302` when they differ.

**One rule covers every operating system**, because they all probe some external
hostname and all of them get redirected instead of the answer they expect.
Android wants a `204`, Apple a specific body, Windows a particular file — the
redirect defeats all three identically.

Loop prevention is the comparison itself: the device's own address, including
the
port when it is not 80, must not be redirected.

Per-URL handling was tried in that project historically and removed. Answering
`/generate_204` with a real `204` actively tells Android that there is **no**
portal, so the sign-in window never appears and the user has to open a browser
by
hand. The remaining route comments there say as much.

RFC 8910 (DHCP option 114, advertising the portal URL) is the standardised
successor and should be added **alongside** this, not instead of it.

---

## 6. The connection test runs outside the request handler

Both reference implementations do this, and for `ESPAsyncWebServer` it is not
optional — blocking in an async handler is not allowed.

- WiFiManager: `handleWifiSave` only sets a `connect` flag; the attempt happens
  in `processConfigPortal()` on the main loop, after an explicit delay
  (`_cpclosedelay`) whose stated purpose is to let the captive portal close
  cleanly first.
- Tasmota: the save arms a 9-second test evaluated in its per-second tick, while
  the page reloads itself on a timer.

So: the handler stores the submitted values, arms the test and responds
immediately. The radio is touched only afterwards.

### Two phases: RAM first, flash only on success

Tasmota writes the new credentials into its live settings but sets
`save_data = 0` for the duration of the test, so nothing reaches flash unless
the
test succeeds. Same shape as `NetConfigComponent::loadFromJson()`, and the same
reason: **a bad value from the user must not leave the device worse off than it
started.** A mistyped password should not overwrite a configuration that worked.

### Success means an IP, not an association

Tasmota's criterion is `WifiGetIP(&ip, true)` — an address that is not the AP's
own. Association alone proves little.

This is weaker with a static address, where the value the user typed satisfies
the test by construction. Coherence was already checked at the form (client
`fvCheckStatic`, server `checkConfig`), so a static config that got this far is
structurally sound; what association does not prove is runtime *reachability*.

A layered reachability check is **designed but not yet implemented** — the one
deferred piece. With a gateway set, an ARP probe (ICMP fallback) toward it would
catch a wrong subnet or mistyped mask and let the page distinguish *connected*
from *connected, gateway reachable*. With no gateway (a deliberate isolated
segment) the gateway-independent signal is **ACD**: an ARP for our own address,
a reply meaning it is already taken, reported honestly as *connected, on-link only*.
It needs raw ARP, which may not be cleanly exposed in the Arduino-ESP32 / lwIP
stack, so it is best-effort by nature. Until it lands, a static connection
reports success on association plus a set address, and the escape hatch below
covers a genuine reachability problem.

### Failure taxonomy

From `WiFi.status()`, because these lead to different user actions:

|Status|Meaning|What the user should do|
|--|--|--|
|`WL_CONNECTED`, no IP|associated, DHCP failed|a DHCP question, not a password one|
|`WL_NO_SSID_AVAIL`|network not found|check the SSID, or move closer|
|`WL_CONNECT_FAILED`|rejected|the password|
|otherwise|timeout|see below|

### The test can lie, so there must be a way past it

Tasmota saves the credentials and restarts in STA-only mode after the timeout
error occurs **twice**. Their stated reason is an ESP8266 limitation: in AP+STA
mode the radio manages only 11b and 11g, so an 11n-only router never answers.

That specific cause does **not** apply here — the ESP-IDF documents the default
protocol set as 11B|11G|11N per interface, with no AP+STA restriction. But the
escape hatch is still needed for slower routers, long DHCP negotiations and a
timeout that is necessarily a guess. Without it, a false negative locks the user
out of their own device while they hold the correct password.

---

## 7. Channel switching and the success page

The ESP-IDF documents that a device is limited to one channel, so in AP+STA mode
the soft-AP adjusts its channel to match the station. That is why a success page
can be lost: the AP moves out from under the client at the moment of success.

**ESP32 has a Channel Switch Announcement.** When the SDK moves the AP to the
station's channel it issues a CSA (`wifi_ap_config_t::csa_count`, default 3
beacons), so a CSA-aware phone follows without dropping, and the live result
(green *Connected* / red *Not connected*) is reliable. The portal must *not*
force the channel with a hard `softAP(channel)` restart — that drops the client
instead of announcing the move, defeating the CSA. Plain `WIFI_AP_STA` +
`begin()` is what lets the SDK do it cleanly.

**ESP8266 has no CSA.** The older NONOS stack does not announce the switch, so
the phone drops when the radio changes channel. On success it often reconnects to
the still-open AP in time to see the result; on failure the phone's OS tends to
rejoin a *saved* network (the user's own Wi-Fi) rather than the open portal AP,
so the user must reselect the portal by hand to retry. There is no fix without
CSA — a documented limitation, softened only by saying so on the page ("if this
network disappears it worked; otherwise reconnect").

Either way the rule from "often, not always" holds: **show the address, don't
rely on the page.** On success the portal echoes the address it actually got
(`WiFi.localIP()`) so the device can be reached without hunting through the
router's client list — with DHCP that is the leased address (usually stable
across the reboot), with a static config the one the user supplied.

---

## 8. The form

Visible by default: SSID, password, hostname. Everything else sits behind an
**Advanced** checkbox: DHCP toggle, static address fields, TX power.

The hostname stays in the basic set on purpose — under DHCP it is the only thing
the user can look for afterwards, whether in the router's list or via mDNS.

TX power sits in the Advanced section with the other rarely-changed fields, but
it is worth offering rather than omitting: one of its purposes is a workaround
for modules whose radios misbehave at full power, and the portal is where someone
first meets that problem.

**TX power is a drop-down on ESP32, a number input on ESP8266.** On ESP32 the
radio supports a dozen discrete levels; a number field with `step="0.25"` would
advertise 71 choices of which 59 do not exist, and the firmware would reject them
one at a time — so it is a drop-down, built from
`WiFiProfile::WIFI_TX_POWER_LEVELS_Q` (which follows an Arduino core update rather
than being restated here). ESP8266 output power is a continuous range, so there
a plain number input is the honest control.

Why the setting exists at all: some ESP32 boards have a poor antenna design and
are unstable at full power, so the saved config can lower it. The form defaults
to full power, and the user can walk it down if a board misbehaves.

That interacts with the setup AP itself: if the *portal's* AP ran at full power
on such a board, a phone could not connect to it reliably — the user could not even
reach the form to fix it. So the portal brings its AP up at a low, safe power
(`WIFI_PORTAL_AP_TX_DBM`, default 8 dBm; set after the radio is up). The user is
nearby by definition (the presence gate), so range is not needed. This is
independent of the saved STA power, which the form still defaults to full.

Both default to full power. The ESP8266 input is bounded by
`MIN_/MAX_WIFI_TX_POWER_dBm` and stepped by `WIFI_TX_POWER_STEP_dBm`, and
`checkConfig` validates per platform — an exact level on ESP32, a range on
ESP8266.

---

## 9. Styling

`ConfigWebPages.h` includes nothing but `<Arduino.h>`, so its stylesheet can be
reused without depending on `AsyncConfigPortal`. Precedence is user, then shared,
then built-in:

```cpp
#if !defined(WIFI_PORTAL_CSS)          // a user-defined one wins
#  if __has_include(<ConfigWebPages.h>)
#    include <ConfigWebPages.h>
#    define WIFI_PORTAL_CSS CONFIG_PORTAL_CSS
#  else
     /* compact built-in stylesheet */
#  endif
#endif
```

Sharing a stylesheet only works if both sides agree on the class names, so the
portal's markup uses `AsyncConfigPortal`'s: `.content`, `.group`, `.row` with
`.label`/`.value`, `.field-error`, `.hint`. Its own compact sheet defines exactly
those, so the page looks right either way.

Matching the rest of the UI is the application's call, not something inherited:
the recovery portal runs in its own boot, with no config server around to take a
supplement from. Hence its own `setCss()` / `setCssExtra()`, with the same shape
as that server's — one PROGMEM string handed to both keeps the two consistent.

Two parts of that contract are behavioural rather than cosmetic: `.field-error`
is hidden until a script gives it a message, and structure is shown and hidden
with the **`hidden` attribute** (plus the `[hidden]{display:none!important}` rule
the sheets carry, which `.row`'s flex would otherwise beat).

Defining `WIFI_PORTAL_CSS` before the header — naming a `PROGMEM` stylesheet of
your own — overrides both. It is served straight from flash (`FPSTR`), so it
must be a `PROGMEM` string.

`CONFIG_PORTAL_HEADER` is deliberately **not** reused: it carries a
`<span id="project_name">` filled in by `/common.js` from the `/project`
endpoint, neither of which the portal serves. The stylesheet is what carries the
visual identity; the portal writes its own header.

---

## 10. It complements an application's own credential entry, not replaces it

An application sometimes needs a Wi-Fi credential outside the portal — restoring
a backup that changes the SSID, say. If it already has an in-place way to supply
one — a password field on a form it serves over the existing connection — the
portal does not replace that field.

The reason is the reboot. An in-place field works remotely: an office device
administered over Ethernet stays reachable while the credential is entered. The
portal instead reboots into AP mode, where Ethernet is gone — turning a remote
operation into a trip to the device. The portal is a **physical-presence**
recovery path; wherever the application can stay remote, it should keep its own
path and use the portal only as the way in when there is no other.

The portal is also an optional dependency: a build that does not include it must
still be able to complete such a restore, which an in-place field allows and the
portal alone would not.

(Concretely, in the `AsyncConfigPortal` ecosystem this in-place path is the Wi-Fi
password field on the restore form; the portal becomes a second way to satisfy
the same *needs-a-password* outcome, and the two coexist.)

---

## Open questions

Resolved above: **optional AP passphrase** (§4 — three modes, deployment
chooses); **portal timeout** (default 180 s idle, extended by the page's
keepalive poll rather than per-field activity, so a phone that backgrounds the
tab is not falsely abandoned).

Still open:

- **Static-address reachability** (§6): the gateway ARP/ICMP probe and the
  island-case ACD are designed but not yet implemented — a static connection
  currently reports success on association plus a set address. Raw-ARP
  feasibility on the Arduino-ESP32 / lwIP stack is the question.
- **RFC 8910**: worth adding at the same time, or once the Host-header path is
  proven on hardware?
