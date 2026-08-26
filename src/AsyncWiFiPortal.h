/**
 * @file AsyncWiFiPortal.h
 * @brief Reboot-into-portal Wi-Fi recovery captive portal for ESP32 and ESP8266.
 *
 * The portal does not run beside the live system: the application asks for it
 * and restarts, and the next boot brings up the portal and nothing else. Ending
 * the portal is a plain restart, so there is no state to unwind. See
 * docs/DESIGN.md for the reasoning.
 *
 * @note Caller-side rules (thread-safety and structure) are on the application:
 *   -# Enter early. Run the state()/run() branch as the first meaningful thing
 *      in setup(), before bringing up your own tasks, radio or subsystems. run()
 *      owns the device; anything started first keeps running beside it and breaks
 *      that guarantee.
 *   -# Never from an ISR. rebootToPortal() writes NVS and restarts, neither of
 *      which is ISR-safe. From a button interrupt, set a flag and call it from
 *      loop() instead. Calling it from a web handler (the AsyncTCP task) is fine:
 *      the state write is serialised by the NVS layer. A single AsyncWiFiPortal
 *      instance's NVS access is not meant to be shared across tasks running at
 *      the same time, which the intended flow never does.
 */

#pragma once

#include <Arduino.h>
#if defined(ARDUINO_ARCH_ESP32)
#  include <WiFi.h>
#  include <esp_mac.h>
#elif defined(ARDUINO_ARCH_ESP8266)
#  include <ESP8266WiFi.h>
#endif
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFiProfile.h>
#include <Preferences.h>   // core on ESP32; vshymanskyy/Preferences on ESP8266
#include "AsyncWiFiPortalHmac.h"

/**
 * @brief NVS namespace for the portal's own state.
 *
 * MUST be excluded from whatever a factory reset erases — the portal has to
 * survive one to be a recovery path, and a factory reset must NOT itself request
 * the portal (see DESIGN.md §3).
 */
#ifndef WIFI_PORTAL_NVS_NS
#  define WIFI_PORTAL_NVS_NS "wifiportal"
#endif

/**
 * @brief Idle timeout (ms) before a session is Abandoned. Default 180 s.
 *
 * Long enough that a phone backgrounding the setup tab (power-save pausing the
 * keepalive poll) is not falsely Abandoned.
 */
#ifndef WIFI_PORTAL_IDLE_MS
#  define WIFI_PORTAL_IDLE_MS 180000UL
#endif

/**
 * @brief How long (ms) to wait for the STA to join during the connection test.
 *
 * On ESP32 the window covers a full-channel scan as well as the association and
 * the lease — see _startTest() for why that scan is worth its 1.5-2 s. The
 * default leaves ample room; shortening it should account for the scan.
 */
#ifndef WIFI_PORTAL_TEST_MS
#  define WIFI_PORTAL_TEST_MS 15000UL
#endif

/**
 * @brief How long (ms) to keep the AP up after a successful connect before
 *        restarting.
 *
 * Connecting to a router on another channel drops the AP's client on a
 * single-radio device; this window lets a dropped phone reconnect and see the
 * result.
 */
#ifndef WIFI_PORTAL_RESULT_MS
#  define WIFI_PORTAL_RESULT_MS 10000UL
#endif

/**
 * @brief TX power (dBm) the setup AP runs at. Default 8.
 *
 * Kept low on purpose: some boards have a poor antenna design and are unreliable
 * at full power, and the user is nearby (the presence gate), so range is not
 * needed. The saved STA config is separate and can still use full power.
 */
#ifndef WIFI_PORTAL_AP_TX_DBM
#  define WIFI_PORTAL_AP_TX_DBM 8
#endif

/**
 * @brief Portal stylesheet, with precedence: user, then shared, then built-in.
 *
 * A user-defined WIFI_PORTAL_CSS (a PROGMEM string, declared before this header)
 * wins; otherwise AsyncConfigPortal's, if the app also builds it (ConfigWebPages.h
 * includes nothing but <Arduino.h>, so no extra dependency); otherwise a compact
 * built-in one.
 */
#if !defined(WIFI_PORTAL_CSS)
#  if __has_include(<ConfigWebPages.h>)
#    include <ConfigWebPages.h>
#    define WIFI_PORTAL_CSS CONFIG_PORTAL_CSS
#  else
static const char WIFI_PORTAL_CSS_BUILTIN[] PROGMEM = R"CSS(
:root{--bg:#eef3f7;--card-bg:#fff;--border:#d0d7de;--fg:#1a2b3c;--muted:#5b6b7b;
--accent:#2c6faa;--alarm:#c62828}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.content{max-width:34em;margin:0 auto;padding:20px}
h1{font-size:20px;margin:8px 0 16px}
.group{background:var(--card-bg);border:1px solid var(--border);border-radius:10px;
padding:16px;margin-bottom:14px}
label{display:block;font-weight:600;margin:10px 0 4px}
input,select{width:100%;padding:9px 10px;border:1px solid var(--border);
border-radius:7px;font-size:15px;background:#fff}
.row{display:flex;justify-content:space-between;align-items:center;gap:1em;
padding:.35em 0}
.label{flex:0 0 45%;color:var(--muted);font-size:.92em}
.label label{margin:0;font-weight:600}
.value{flex:1 1 55%}
.value input[type=checkbox]{width:auto}
.field-error{display:none;color:var(--alarm);font-size:.8em;margin-top:.2em}
.hint{color:var(--muted);font-size:.8em;margin-top:.2em}
button{width:100%;padding:11px;border:0;border-radius:8px;background:var(--accent);
color:#fff;font-size:16px;font-weight:600;margin-top:8px}
button:disabled{opacity:.5}
[hidden]{display:none!important}
@media (max-width:480px){.row{flex-direction:column;align-items:stretch;gap:.15em}
.label{flex:none}}
)CSS";
#    define WIFI_PORTAL_CSS WIFI_PORTAL_CSS_BUILTIN
#  endif
#endif

/**
 * @brief Minimal client script for the single portal page.
 *
 * The Advanced toggle and the static-addressing coherence check; @c checkStatic
 * mirrors NetworkProfile::checkStatic (kept in sync with AsyncConfigPortal's
 * fields.js).
 */
static const char WIFI_PORTAL_JS[] PROGMEM = R"JS(
function $(id){return document.getElementById(id)}
function ip2n(s){s=(s||'').replace(/^\s+|\s+$/g,'');var p=s.split('.');if(p.length!==4)return -1;
 var n=0;for(var i=0;i<4;i++){if(!/^\d{1,3}$/.test(p[i]))return -1;var o=+p[i];if(o>255)return -1;n=n*256+o}return n>>>0}
function uni(n){if(((n&0xFF000000)>>>0)===0)return false;if(((n&0xFF000000)>>>0)===0x7F000000)return false;
 if(((n&0xF0000000)>>>0)===0xE0000000)return false;if(((n&0xF0000000)>>>0)===0xF0000000)return false;return true}
function unset(s){s=(s||'').replace(/^\s+|\s+$/g,'');return s===''||s==='0.0.0.0'}
function checkStatic(ip,mask,gw,dns){
 var m=ip2n(mask);if(m<=0)return{key:'mask',msg:'Not a valid subnet mask'};
 var host=(~m)>>>0;if(((host&(host+1))>>>0)!==0)return{key:'mask',msg:'Not a valid subnet mask'};
 var a=ip2n(ip);if(a<0)return{key:'ip',msg:'Not a valid IP address'};
 if(!uni(a))return{key:'ip',msg:'Not a usable host address'};
 if(host!==0&&host!==1){var h=(a&host)>>>0;if(h===0||h===host)return{key:'ip',msg:'This is the network or broadcast address'}}
 if(!unset(gw)){var g=ip2n(gw);if(g<0)return{key:'gw',msg:'Not a valid IP address'};
  if(((g&m)>>>0)!==((a&m)>>>0))return{key:'gw',msg:'Gateway is not on this subnet'}}
 if(dns){for(var i=0;i<dns.length;i++){if(unset(dns[i]))continue;var d=ip2n(dns[i]);
  if(d<0||!uni(d))return{key:'dns'+i,msg:'Not a usable DNS address'}}}
 return null}
function adv(){$('advbox').hidden=!$('advanced').checked}
function stat(){$('statbox').hidden=$('dhcp').checked}
function setErr(id,m){var e=$(id);if(!e)return;e.textContent=m||'';e.style.display=m?'block':'none'}
function clearErr(){var e=document.getElementsByClassName('field-error');
 for(var i=0;i<e.length;i++){e[i].textContent='';e[i].style.display='none'}}
function dnsList(){var a=[],n=document.querySelectorAll('[name^=dns]');for(var i=0;i<n.length;i++)a.push(n[i].value);return a}
function validate(){
 clearErr();var ok=true;
 if(!$('ssid').value){setErr('ssid_err','Required');ok=false}
 if($('advanced').checked&&!$('dhcp').checked){
  var r=checkStatic($('ip').value,$('mask').value,$('gw').value,dnsList());
  if(r){setErr(r.key+'_err',r.msg);ok=false}
 }
 $('save').disabled=!ok;return ok}
function init(){
 $('advanced').addEventListener('change',adv);
 $('dhcp').addEventListener('change',stat);
 $('f').addEventListener('input',validate);
 $('f').addEventListener('submit',function(e){if(!validate())e.preventDefault()});
 adv();stat();validate()}
window.addEventListener('load',init);
)JS";

/**
 * @brief Reboot-into-portal Wi-Fi recovery captive portal.
 *
 * Check state() at the top of setup(); enter run() when a session was requested,
 * otherwise connect with the stored config. A button (or web) trigger calls
 * rebootToPortal(). See the file-level notes for the caller-side rules.
 */
class AsyncWiFiPortal {
public:
    /**
     * @brief The persisted state machine (DESIGN.md §3).
     *
     * Sequence: the app writes Requested and restarts; the portal writes Running
     * on entry, then Saved or Abandoned, and restarts; the app reads the outcome
     * and clears it.
     */
    enum class State : uint8_t {
        None      = 0,  ///< nothing pending, nothing to report
        Requested = 1,  ///< the app asked; the next boot enters the portal
        Running   = 2,  ///< the portal started (written on entry)
        Saved     = 3,  ///< a configuration was saved
        Abandoned = 4,  ///< the portal timed out with nothing saved
    };

    /**
     * @brief Access-point security mode.
     *
     * Presence is the primary gate (DESIGN.md §4); this is a secondary barrier
     * the deployment chooses.
     */
    enum class ApSecurity : uint8_t {
        Open,         ///< no passphrase (default) — trusted physical space
        Passphrase,   ///< one fleet-wide value; `secret` is the passphrase
        MacDerived,   ///< per-device value; `secret` is the salt (or nullptr)
    };

    // ------------------------------------------------------------------ boot ---

    /**
     * @brief The stored state, with no side effect.
     *
     * Branch on this at the top of setup(): Requested/Running -> run();
     * Saved/Abandoned -> apply and clear().
     * @return The current State.
     */
    State state() const {
        Preferences p;
        if (!p.begin(WIFI_PORTAL_NVS_NS, /*readOnly=*/true)) return State::None;
        uint8_t v = p.getUChar("state", static_cast<uint8_t>(State::None));
        p.end();
        return (v <= static_cast<uint8_t>(State::Abandoned))
                   ? static_cast<State>(v) : State::None;
    }

    /** @brief Acknowledge the outcome: write None. Call after handling Saved/Abandoned. */
    void clear() { _write(State::None); }

    // --------------------------------------------------------------- request ---

    /**
     * @brief Record a portal request and restart; the next boot sees Requested.
     * @warning MUST NOT be called from an ISR — it writes NVS, then restarts.
     */
    void rebootToPortal() {
        _write(State::Requested);
        delay(50);            // let the NVS commit settle before the reset
        ESP.restart();
    }

    // --------------------------------------------------------------- configure -

    /**
     * @brief Choose the AP security mode. Set before run().
     * @param mode   Open, Passphrase or MacDerived.
     * @param secret For Passphrase, the passphrase; for MacDerived, the salt
     *               (nullptr = obscurity-only). Must outlive run() (a string
     *               literal is fine).
     */
    void setApSecurity(ApSecurity mode, const char* secret = nullptr) {
        _apMode = mode;
        _apSecret = secret;
    }

    /**
     * @brief Idle timeout before the session is Abandoned.
     * @param ms Milliseconds. Default WIFI_PORTAL_IDLE_MS (180 s).
     */
    void setTimeout(uint32_t ms) { _idleMs = ms; }

    /**
     * @brief Preferences namespace the accepted config is saved to on success.
     *
     * The application loads its WiFiProfile from the same namespace at boot.
     * @param ns Namespace name (ignored if null or empty). Default "net_wifi".
     */
    void setProfileNamespace(const char* ns) { if (ns && *ns) _saveNs = ns; }

    // -------------------------------------------------------------------- run --

    /**
     * @brief Replaces the stylesheet the portal serves.
     * @param css A PROGMEM stylesheet, or nullptr to restore the default.
     */
    void setCss(const char* css) { _css = css ? css : WIFI_PORTAL_CSS; }

    /**
     * @brief Appends an application stylesheet after the one served.
     *
     * The recovery portal runs in its own boot, with no AsyncConfigPortal
     * instance around, so it cannot inherit that server's supplement. Passing
     * the same PROGMEM string to both is what keeps one house style across the
     * two: `web.setCssExtra(MY_CSS); portal.setCssExtra(MY_CSS);`
     *
     * @param css A PROGMEM stylesheet, or nullptr for none.
     */
    void setCssExtra(const char* css) { _cssExtra = css; }

    /**
     * @brief Enter portal mode. Does not return.
     *
     * Writes Running, brings up the AP / DNS / server, runs until a save or the
     * idle timeout, then restarts. Call it early in setup() (see the caller-side
     * rules in the file notes).
     * @param apSsid SSID of the setup access point.
     */
    void run(const char* apSsid) {
        _write(State::Running);
        _apSsid = apSsid ? apSsid : "setup-portal";

        // Bring up the access point. softAP() starts the IDF's built-in DHCP
        // server automatically, so the client gets an address with no code of
        // ours. persistent(false) keeps this from rewriting the stored STA
        // credentials in the Wi-Fi NVS.
        WiFi.persistent(false);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(_apSsid, _apPassword());
        // Low power for the setup AP (must be set after the radio is up), so a
        // weak-antenna board still lets a nearby phone connect to configure it.
#if defined(ARDUINO_ARCH_ESP32)
        WiFi.setTxPower(static_cast<wifi_power_t>(
            WIFI_PORTAL_AP_TX_DBM * WiFiProfile::WIFI_TX_POWER_MULTIPLIER));
#elif defined(ARDUINO_ARCH_ESP8266)
        WiFi.setOutputPower(WIFI_PORTAL_AP_TX_DBM);
#endif
        const IPAddress apIP = WiFi.softAPIP();

        // Captive DNS: every hostname resolves to the AP, so every request
        // arrives carrying its original Host header (see DESIGN.md §5).
        _dns.setErrorReplyCode(DNSReplyCode::NoError);
        _dns.start(53, "*", apIP);
        _apIP = apIP;

        _startServer();

        // TODO(step 3b): the real config form. TODO(step 3c): the two-phase
        // connection test (RAM -> flash, success taxonomy, ACD). TODO(step 4):
        // derive() for MacDerived (portable HMAC-SHA256).
        _runShell();

        _server.end();
        _dns.stop();
        WiFi.softAPdisconnect(true);
        _write(State::Abandoned);
        delay(50);
        ESP.restart();
    }

private:
    void _write(State s) {
        Preferences p;
        if (!p.begin(WIFI_PORTAL_NVS_NS, /*readOnly=*/false)) return;
        p.putUChar("state", static_cast<uint8_t>(s));
        p.end();
    }

    /// Streams one or two PROGMEM segments as a single body. Chunked with
    /// memcpy_P rather than buffered: an ESP8266 cannot byte-read flash, and
    /// building the asset in a response stream would copy it into RAM — which is
    /// what exhausts the heap when several assets load at once.
    static void _sendProgmem(AsyncWebServerRequest* req, const char* type,
                             const char* body, const char* extra = nullptr) {
        const size_t n1 = strlen_P(body);
        const size_t total = n1 + (extra ? strlen_P(extra) : 0);
        req->send(req->beginChunkedResponse(type,
            [body, extra, n1, total](uint8_t* dst, size_t maxLen, size_t index) -> size_t {
                const size_t rem = total - index;
                size_t n = rem < maxLen ? rem : maxLen;
                if (!n) return 0;
                if (index < n1) {
                    if (index + n > n1) n = n1 - index;
                    memcpy_P(dst, body + index, n);
                } else {
                    memcpy_P(dst, extra + (index - n1), n);
                }
                return n;
            }));
    }

    /// The AP passphrase passed to softAP(): nullptr opens the network.
    const char* _apPassword() {
        switch (_apMode) {
            case ApSecurity::Passphrase: return _apSecret;   // fleet value
            case ApSecurity::MacDerived: return _derived();  // per-device
            case ApSecurity::Open:
            default:                     return nullptr;      // open
        }
    }

    /// MacDerived password: crockford32(HMAC-SHA256(salt, base-MAC)[:10]), 16
    /// chars, byte-identical to tools/genpass.py. The salt is _apSecret (empty if
    /// none — then obscurity-only, the algorithm being public). Derived from the
    /// factory *base* MAC (== the STA MAC on both platforms), which is what is
    /// printed on the chip and is independent of build config — unlike the soft-AP
    /// BSSID (base or base+1 on ESP32 per CONFIG_ESP32_UNIVERSAL_MAC_ADDRESSES,
    /// a local-bit variant on ESP8266). Also readable before the AP is up.
    const char* _derived() {
        uint8_t mac[6] = {0};
        _readBaseMac(mac);
        wifiportal_detail::deriveApPassword(_apSecret ? _apSecret : "",
                                            mac, _derivedBuf, sizeof(_derivedBuf));
        return _derivedBuf;
    }

    static void _readBaseMac(uint8_t mac[6]) {
#if defined(ARDUINO_ARCH_ESP32)
        esp_read_mac(mac, ESP_MAC_WIFI_STA);   // eFuse base MAC, mode-independent
#else
        WiFi.macAddress(mac);                  // ESP8266: STA MAC == base MAC
#endif
    }

    /// step 3a: the captive web server. The not-found handler turns any foreign
    /// Host (an OS connectivity probe, pointed here by our DNS) into a 302 to our
    /// page, which is what makes the captive window appear (DESIGN.md §5).
    void _startServer() {
        _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { _sendPortal(r); });
        _server.on("/css", HTTP_GET, [this](AsyncWebServerRequest* r) {
            // The application supplement, if any, follows the sheet: later rules
            // win, so a house style needs no copy of the original.
            _sendProgmem(r, "text/css", _css, _cssExtra);
        });
        _server.on("/portal.js", HTTP_GET, [](AsyncWebServerRequest* r) {
            _sendProgmem(r, "application/javascript", WIFI_PORTAL_JS);
        });
        _server.on("/save", HTTP_POST,
                   [this](AsyncWebServerRequest* r) { _handleSave(r); });
        _server.on("/saveanyway", HTTP_POST,
                   [this](AsyncWebServerRequest* r) { _handleSaveAnyway(r); });
        _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest* r) {
            _idleFrom = millis();   // the poll is a keepalive
            const __FlashStringHelper* st =
                  _test == Test::Connected ? F("connected")
                : _test == Test::Failed    ? F("failed") : F("running");
            AsyncResponseStream* s = r->beginResponseStream("application/json");
            s->print(F("{\"state\":\""));
            s->print(st);
            s->print(F("\",\"msg\":\""));
            s->print(FPSTR(_testMsg));
            s->print(F("\",\"ip\":\""));
            if (_test == Test::Connected) s->print(_staIp);   // IPAddress -> a.b.c.d
            s->print(F("\"}"));
            r->send(s);
        });
        _server.onNotFound([this](AsyncWebServerRequest* r) {
            if (_foreignHost(r)) _redirectHome(r);
            else                 r->send(404, "text/plain", "Not found");
        });
        _server.begin();
    }

    /// True when the request's Host is not our own address — an external hostname
    /// the DNS pointed here. Our own address (with the port when it is not 80)
    /// must not be redirected, or the captive check loops.
    bool _foreignHost(AsyncWebServerRequest* r) const {
        const String& host = r->host();
        const String self = _apIP.toString();
        return host != self && !host.startsWith(self + ":");
    }

    void _redirectHome(AsyncWebServerRequest* r) {
        r->redirect(String("http://") + _apIP.toString() + "/");
    }

    void _sendPortal(AsyncWebServerRequest* r) {
        AsyncResponseStream* s = r->beginResponseStream("text/html");
        s->print(F(
            "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>Wi-Fi setup</title><link rel=\"stylesheet\" href=\"/css\"></head>"
            "<body><div class=\"content\"><h1>Wi-Fi setup</h1>"
            "<form id=\"f\" method=\"post\" action=\"/save\"><div class=\"group\">"
            "<label for=\"ssid\">Network (SSID)</label>"
            "<input id=\"ssid\" name=\"ssid\" autocapitalize=\"none\" autocomplete=\"off\">"
            "<span class=\"field-error\" id=\"ssid_err\"></span>"
            "<label for=\"pass\">Password</label>"
            "<input id=\"pass\" name=\"pass\" type=\"password\">"
            "<label for=\"host\">Hostname</label>"));
        char hn[64];
        if (!_profile.getHostname(hn, sizeof(hn), NetworkProfile::ConfigSource::FACTORY))
            hn[0] = '\0';
        s->print(F("<input id=\"host\" name=\"host\" placeholder=\""));
        s->print(hn);
        s->print(F("\">"));
        s->print(F(
            "<div class=\"hint\">Left empty, the device names itself as shown "
            "(from its MAC). Used to find it after it joins: router list, mDNS.</div>"
            "</div>"
            "<div class=\"row\"><span class=\"label\">"
            "<label for=\"advanced\">Advanced</label></span>"
            "<span class=\"value\"><input type=\"checkbox\" id=\"advanced\"></span></div>"
            "<div class=\"group\" id=\"advbox\" hidden>"
            "<div class=\"row\"><span class=\"label\">"
            "<label for=\"dhcp\">Use DHCP</label></span>"
            "<span class=\"value\"><input type=\"checkbox\" id=\"dhcp\" name=\"dhcp\" checked></span></div>"
            "<div id=\"statbox\" hidden>"
            "<label for=\"ip\">IP address</label>"
            "<input id=\"ip\" name=\"ip\"><span class=\"field-error\" id=\"ip_err\"></span>"
            "<label for=\"mask\">Subnet mask</label>"
            "<input id=\"mask\" name=\"mask\" placeholder=\"255.255.255.0\">"
            "<span class=\"field-error\" id=\"mask_err\"></span>"
            "<label for=\"gw\">Gateway <span class=\"hint\">(optional)</span></label>"
            "<input id=\"gw\" name=\"gw\"><span class=\"field-error\" id=\"gw_err\"></span>"));
        for (uint8_t i = 0; i < WiFiProfile::DNS_SERVER_COUNT; i++) {
            s->print(F("<label for=\"dns")); s->print(i);
            s->print(F("\">DNS ")); s->print(i + 1);
            s->print(F(" <span class=\"hint\">(optional)</span></label>"
                      "<input id=\"dns")); s->print(i);
            s->print(F("\" name=\"dns")); s->print(i);
            s->print(F("\"><span class=\"field-error\" id=\"dns")); s->print(i);
            s->print(F("_err\"></span>"));
        }
        s->print(F("</div>"));   // statbox
        // TX power. ESP32 has discrete levels (a drop-down); ESP8266 has a
        // continuous range (a number input). Both default to full power.
#if defined(ARDUINO_ARCH_ESP32)
        s->print(F("<label for=\"txpwr\">TX power (dBm)</label>"
                   "<select id=\"txpwr\" name=\"txpwr\">"));
        for (uint8_t i = 0; i < WiFiProfile::WIFI_TX_POWER_LEVEL_COUNT; i++) {
            const double dbm = (double)WiFiProfile::WIFI_TX_POWER_LEVELS_Q[i]
                             / WiFiProfile::WIFI_TX_POWER_MULTIPLIER;
            const bool isMax = (i + 1 == WiFiProfile::WIFI_TX_POWER_LEVEL_COUNT);
            s->print(F("<option value=\""));
            s->print(dbm, 2);
            s->print('"');
            if (isMax) s->print(F(" selected"));   // radio's factory default
            s->print('>');
            s->print(dbm, 2);
            s->print(F("</option>"));
        }
        s->print(F("</select>"));
#elif defined(ARDUINO_ARCH_ESP8266)
        s->print(F("<label for=\"txpwr\">TX power (dBm)</label>"
                   "<input id=\"txpwr\" name=\"txpwr\" type=\"number\" min=\""));
        s->print(WiFiProfile::MIN_WIFI_TX_POWER_dBm, 2);
        s->print(F("\" max=\""));
        s->print(WiFiProfile::MAX_WIFI_TX_POWER_dBm, 2);
        s->print(F("\" step=\""));
        s->print(WiFiProfile::WIFI_TX_POWER_STEP_dBm, 2);
        s->print(F("\" value=\""));
        s->print(WiFiProfile::DEFAULT_WIFI_TX_POWER_dBm, 2);
        s->print(F("\">"));
#endif
        s->print(F("<div class=\"hint\">Full power by default; some boards with a "
                   "weaker antenna design need it lower to stay stable.</div>"));
        s->print(F(
            "</div>"   // advbox
            "<button id=\"save\" type=\"submit\">Save &amp; connect</button>"
            "</form></div><script src=\"/portal.js\"></script></body></html>"));
        r->send(s);
    }

    /// step 3b-2: parse the form into a staged WiFiConfig and validate it. The
    /// two-phase connection test (step 3c) runs on _staged when it passes.
    void _handleSave(AsyncWebServerRequest* r) {
        WiFiProfile::WiFiConfig cfg;
        _profile.getConfig(cfg);

        _formStr(r, "ssid", cfg.ssid,     sizeof(cfg.ssid));
        _formStr(r, "pass", cfg.password, sizeof(cfg.password));
        _formStr(r, "host", cfg.hostname, sizeof(cfg.hostname));

        cfg.dhcp = _formBool(r, "dhcp");
        if (!cfg.dhcp) {
            _formIp(r, "ip",   cfg.ip);
            _formIp(r, "mask", cfg.mask);
            _formIp(r, "gw",   cfg.gateway);   // empty -> 0.0.0.0 (island)
            char key[8];
            for (uint8_t i = 0; i < WiFiProfile::DNS_SERVER_COUNT; i++) {
                snprintf(key, sizeof(key), "dns%u", i);
                _formIp(r, key, cfg.dns[i]);
            }
        }
        // TX power (ESP32 select or ESP8266 number); checkConfig validates it
        // per platform (discrete levels vs range).
        if (r->hasParam("txpwr", true))
            cfg.txPower = r->getParam("txpwr", true)->value().toFloat();

        const NetworkProfile::ConfigCheck cc = WiFiProfile::checkConfig(cfg);
        if (cc != NetworkProfile::ConfigCheck::Ok) {
            _sendResult(r, false, _checkMsg(cc));
            return;
        }

        _staged     = cfg;
        _haveStaged = true;
        _idleFrom   = millis();   // activity: hold off the idle timeout
        _testReq    = true;       // _runShell starts the test
        _sendConnecting(r);       // a page that polls /status for the result
    }

    // --- form-field helpers (String confined to the ESPAsyncWebServer edge) ---

    static void _formStr(AsyncWebServerRequest* r, const char* name,
                         char* out, size_t len) {
        if (r->hasParam(name, true))
            snprintf(out, len, "%s", r->getParam(name, true)->value().c_str());
        else
            out[0] = '\0';
    }

    static bool _formBool(AsyncWebServerRequest* r, const char* name) {
        return r->hasParam(name, true);   // a checkbox is present only when checked
    }

    /// Empty, absent or unparseable -> 0.0.0.0 (unset); a valid address -> set.
    /// Coherence is checkConfig()'s job; the page already validated syntax.
    static void _formIp(AsyncWebServerRequest* r, const char* name, IPAddress& out) {
        out = IPAddress(0, 0, 0, 0);
        if (r->hasParam(name, true)) {
            const String& v = r->getParam(name, true)->value();
            if (v.length()) { IPAddress tmp; if (tmp.fromString(v)) out = tmp; }
        }
    }

    // Flash-resident: the strings are only ever handed to Print::print(), which
    // reads a __FlashStringHelper* correctly on both platforms, so they cost no
    // RAM on ESP8266.
    //
    // Every enumerator is listed and the fallback is the initialiser rather than
    // a default: label. The two behave identically — an unrecognised value still
    // produces the generic line — but default: also silences -Wswitch, so a
    // value added to NetworkProfile::ConfigCheck later would reach the page as
    // "not valid" with nothing to say a new reason had appeared.
    static const __FlashStringHelper* _checkMsg(NetworkProfile::ConfigCheck c) {
        using CC = NetworkProfile::ConfigCheck;
        const __FlashStringHelper* m = F("the configuration is not valid");
        switch (c) {
            case CC::Ok:               m = F("the configuration is valid"); break;
            case CC::BadHostname:      m = F("the hostname is not valid"); break;
            case CC::BadIp:            m = F("the IP address is not a usable host address on its subnet"); break;
            case CC::BadMask:          m = F("the subnet mask is not valid"); break;
            case CC::GatewayOffSubnet: m = F("the gateway is not on the configured subnet"); break;
            case CC::BadDns:           m = F("a DNS server address is not valid"); break;
            case CC::BadSsid:          m = F("the network name (SSID) is not valid"); break;
            case CC::BadPassword:      m = F("the password is not valid (8-63 characters)"); break;
            case CC::BadTxPower:       m = F("the TX power is not an allowed level"); break;
        }
        return m;
    }

    void _sendResult(AsyncWebServerRequest* r, bool ok,
                     const __FlashStringHelper* msg) {
        AsyncResponseStream* s = r->beginResponseStream("text/html");
        s->print(F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                   "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                   "<title>Wi-Fi setup</title><link rel=\"stylesheet\" href=\"/css\"></head>"
                   "<body><div class=\"content\"><div class=\"group\">"));
        s->print(F("<h1>"));
        s->print(ok ? F("Saved") : F("Not saved"));
        s->print(F("</h1><p>"));
        s->print(msg);
        s->print(F("</p>"));
        if (!ok) s->print(F("<p><a href=\"/\">Back to the form</a></p>"));
        s->print(F("</div></div></body></html>"));
        r->send(s);
    }

    /// Run loop: answer captive DNS, drive the connection test when the form asks
    /// for one, and idle out into Abandoned when nothing is happening.
    void _runShell() {
        _idleFrom = millis();
        for (;;) {
            _dns.processNextRequest();
            if (_testReq) { _testReq = false; _startTest(); }
            if (_test == Test::Running) _pollTest();
            if (_restartPending && (int32_t)(millis() - _restartAt) >= 0) {
                delay(50);
                ESP.restart();
            }
            // Idle out only while waiting for the user — not during a test or a
            // pending restart.
            if (!_restartPending && (_test == Test::Idle || _test == Test::Failed)
                && millis() - _idleFrom >= _idleMs) return;
            delay(10);
        }
    }

    /// Phase 1: try the credentials with nothing written to flash. AP_STA keeps
    /// the AP up. On ESP32 the SDK moves the AP to the STA's channel with a
    /// Channel Switch Announcement, so a CSA-aware phone follows without dropping;
    /// ESP8266 has no CSA, so the phone drops and the page falls back to its
    /// reconnect message.
    void _startTest() {
        _test = Test::Running;
        _testMsg = PSTR("Trying the network...");
        _testStart = millis();
        WiFi.mode(WIFI_AP_STA);

#if defined(ARDUINO_ARCH_ESP32)
        // Scan every channel before choosing, rather than taking the first
        // beacon carrying the right SSID — which is what the core's default,
        // WIFI_FAST_SCAN, does. The companion sort_method already prefers the
        // strongest signal, but with one candidate there is nothing to sort.
        //
        // It matters more here than anywhere else in this library. Where a
        // controller enforces a signal floor, a distant access point refuses the
        // association, and that arrives as WL_CONNECT_FAILED — which the verdict
        // below reports as "check the password". The credentials would be
        // correct, and the user would be sent to fix the one thing that is not
        // wrong. The scan costs 1.5-2 s of the WIFI_PORTAL_TEST_MS budget, which
        // is cheap next to a wrong answer given to somebody standing at the
        // device.
        //
        // ESP8266 has no equivalent setting; its SDK chooses on its own.
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
#endif
        if (_staged.dhcp) {
            WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
        } else {
            const IPAddress d2 = (WiFiProfile::DNS_SERVER_COUNT > 1)
                               ? _staged.dns[1] : IPAddress(0, 0, 0, 0);
            WiFi.config(_staged.ip, _staged.gateway, _staged.mask, _staged.dns[0], d2);
        }
        WiFi.begin(_staged.ssid, _staged.password);
    }

    void _pollTest() {
        const wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
            _finishSuccess();          // persist + Saved + restart (does not return)
            return;
        }
        if (st == WL_NO_SSID_AVAIL) { _fail(PSTR("Network not found - check the name or move closer")); return; }
        if (st == WL_CONNECT_FAILED) { _fail(PSTR("Rejected - check the password")); return; }
        if (millis() - _testStart >= WIFI_PORTAL_TEST_MS) {
            _fail(st == WL_CONNECTED
                    ? PSTR("Joined the network but got no IP address (DHCP)")
                    : PSTR("Timed out - check the network and password"));
        }
    }

    void _fail(const char* msg) {
        _test = Test::Failed;
        _testMsg = msg;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);            // back to AP-only; the user can retry
        _idleFrom = millis();
    }

    /// Phase 2: only now does anything reach flash. checkConfig() already passed.
    /// The restart is held off briefly so the polling page can report success
    /// before the AP disappears.
    void _finishSuccess() {
        _test = Test::Connected;
        _testMsg = PSTR("Connected");
        _staIp = WiFi.localIP();
        if (!_persist(WIFI_PORTAL_RESULT_MS))
            _fail(PSTR("Connected, but the configuration could not be saved"));
    }

    /// Write the staged config to flash and schedule the restart. Shared by a
    /// successful test and the "save anyway" escape hatch.
    /// Commits the staged configuration and schedules the restart.
    ///
    /// Both calls are checked. _staged has already passed checkConfig(), so
    /// setConfig() can only fail on a mutex timeout and saveCfg() on storage —
    /// but an unchecked failure here is the worst kind this portal has: it would
    /// mark State::Saved and reboot into an empty or stale profile, with the AP
    /// gone and no way back except the button. Better to stay in the portal and
    /// say so.
    ///
    /// @return false if nothing was committed; the caller reports it.
    bool _persist(uint32_t delayMs) {
        if (!_profile.setConfig(_staged) || !_profile.saveCfg(_saveNs)) return false;
        _write(State::Saved);
        _restartPending = true;
        _restartAt = millis() + delayMs;
        return true;
    }

    /// Escape hatch (DESIGN.md §7): commit the staged config even though the test
    /// did not confirm a connection — for a false negative such as a slow router,
    /// a long DHCP, or the ESP8266 AP+STA limitation.
    void _handleSaveAnyway(AsyncWebServerRequest* r) {
        if (!_haveStaged) { r->send(409, "text/plain", "Nothing to save"); return; }
        if (!_persist(1500)) {
            _sendResult(r, false, F("the configuration could not be saved; "
                                    "nothing was changed"));
            return;
        }
        AsyncResponseStream* s = r->beginResponseStream("text/html");
        s->print(F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                   "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                   "<title>Wi-Fi setup</title><link rel=\"stylesheet\" href=\"/css\"></head>"
                   "<body><div class=\"content\"><div class=\"group\"><h1>Saved</h1>"
                   "<p>Configuration saved without a confirmed connection. "
                   "The device is restarting.</p></div></div></body></html>"));
        r->send(s);
    }

    /// A page that polls /status and reports the test result in place.
    void _sendConnecting(AsyncWebServerRequest* r) {
        AsyncResponseStream* s = r->beginResponseStream("text/html");
        s->print(F(
            "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>Wi-Fi setup</title><link rel=\"stylesheet\" href=\"/css\">"
            "<style>#m a{color:#2563eb;text-decoration:underline}#m a:visited{color:#2563eb}</style></head>"
            "<body><div class=\"content\"><div class=\"group\">"
            "<h1 id=\"h\">Connecting...</h1><p id=\"m\">Trying the network...</p>"
            "<p class=\"hint\" id=\"hint\">You may briefly disconnect from this setup network "
            "while the device connects.</p>"
            "<p id=\"r\" hidden><a href=\"/\">Back to the form</a>"
            " &middot; <a href=\"#\" id=\"sa\">Save anyway</a></p></div></div>"
            "<script>var f=0;"
            "document.getElementById('sa').addEventListener('click',function(e){e.preventDefault();"
            "fetch('/saveanyway',{method:'POST'});"
            "var h=document.getElementById('h');h.textContent='Saved';h.style.color='#15803d';"
            "document.getElementById('m').textContent='Configuration saved without a confirmed connection. The device is restarting.';"
            "document.getElementById('r').hidden=true});"
            "function p(){fetch('/status').then(function(x){return x.json()}).then(function(j){"
            "f=0;document.getElementById('m').textContent=j.msg;"
            "if(j.state==='connected'){var hc=document.getElementById('h');"
            "hc.textContent='Connected';hc.style.color='#15803d';"
            "document.getElementById('hint').hidden=true;"
            "document.getElementById('m').innerHTML='IP address: <a href=\"http://'+j.ip+'\">'+j.ip+'</a><br>Configuration saved, the device is restarting soon.'}"
            "else if(j.state==='failed'){var hf=document.getElementById('h');"
            "hf.textContent='Not connected';hf.style.color='#b91c1c';"
            "document.getElementById('hint').hidden=true;"
            "document.getElementById('r').hidden=false}"
            "else{setTimeout(p,1000)}}).catch(function(){"
            "if(++f===4){document.getElementById('m').textContent="
            "'If this setup network has disappeared, the device connected and joined your network. "
            "Otherwise reconnect to it; this page will update.'}"
            "setTimeout(p,1500)})}"
            "setTimeout(p,1000)</script></body></html>"));
        r->send(s);
    }

    /// Later steps call this on a successful, tested configuration.
    void _saveAndRestart() {
        _write(State::Saved);
        delay(50);
        ESP.restart();
    }

    enum class Test : uint8_t { Idle, Running, Connected, Failed };

    DNSServer      _dns;
    AsyncWebServer _server{80};
    WiFiProfile    _profile;   // for the default hostname now, config staging later
    IPAddress      _apIP;
    IPAddress      _staIp;
    WiFiProfile::WiFiConfig _staged;
    bool           _haveStaged = false;
    Test           _test       = Test::Idle;
    const char*    _testMsg    = "";
    bool           _testReq    = false;
    uint32_t       _testStart  = 0;
    uint32_t       _idleFrom   = 0;
    uint32_t       _restartAt  = 0;
    bool           _restartPending = false;
    const char*    _saveNs     = "net_wifi";
    const char*    _css        = WIFI_PORTAL_CSS;   ///< served on /css
    const char*    _cssExtra   = nullptr;           ///< appended after it
    char           _derivedBuf[17] = {};   // 16 Crockford chars + NUL
    ApSecurity     _apMode     = ApSecurity::Open;
    const char*    _apSecret   = nullptr;
    const char*    _apSsid     = "setup-portal";
    uint32_t       _idleMs     = WIFI_PORTAL_IDLE_MS;
};
