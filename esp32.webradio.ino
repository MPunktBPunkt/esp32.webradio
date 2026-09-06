// ╔══════════════════════════════════════════════════════════════╗
// ║  esp32.webradio.ino — Web-Radio für ESP32                   ║
// ║  Version: 2.0.0                                             ║
// ║  Basiert auf: esp-hub-base v1.5.0                           ║
// ╠══════════════════════════════════════════════════════════════╣
// ║  Bibliotheken (Arduino Library Manager):                    ║
// ║    - WiFiManager  von tablatronix / tzapu                   ║
// ║    - ArduinoJson  von bblanchon (v6 oder v7, beide ok)      ║
// ║  Built-in: HTTPClient, Update, WebServer, Preferences,      ║
// ║            ESPmDNS                                          ║
// ╠══════════════════════════════════════════════════════════════╣
// ║  Features v2.0.0:                                           ║
// ║    - ⭐ Favoriten (persistent, unterhalb Player)             ║
// ║    - 🎙 Podcasts-Tab mit RSS-Episodenvorschau               ║
// ║    - 🔍 Entdecken-Tab via radio-browser.info API            ║
// ║    - Genre-Browser + Sendersuche mit ★-Button              ║
// ║    - Letzter Sender ESP-seitig gespeichert (reboot-fest)    ║
// ╠══════════════════════════════════════════════════════════════╣
// ║  Quickstart:                                                ║
// ║    1. HUB_HOST auf IP deines ioBroker anpassen              ║
// ║    2. Flashen → Hotspot "ESP-WebRadio" erscheint            ║
// ║    3. WLAN + Hub-IP eingeben                                ║
// ║    4. Web-Radio: http://<ESP-IP>/                           ║
// ╚══════════════════════════════════════════════════════════════╝

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>


// ================================================================
//  KONFIGURATION  <-- Diese Werte anpassen
// ================================================================

#define DEVICE_NAME            "Web-Radio"
#define FW_VERSION             "2.0.0"
#define HUB_HOST               "192.168.178.113"
#define HUB_PORT               8093
#define WIFI_AP_NAME           "ESP-WebRadio"
#define WIFI_PORTAL_TIMEOUT_S  180
#define DEFAULT_INTERVAL_S     60
#define WATCHDOG_TIMEOUT_S     0
#define RESET_BUTTON_PIN       0
#define RESET_HOLD_SEC         3
#define MAX_STATIONS           30
#define MAX_FAVORITES          30
#define MAX_PODCASTS           20


// ================================================================
//  IO-TABELLE  (leer — kein Sensor nötig)
// ================================================================
struct IoValue { String key; String type; float value; String unit; };
IoValue ioTable[] = {};
const int IO_COUNT = 0;
void updateIoValues() {}


// ================================================================
//  GLOBALE VARIABLEN
// ================================================================

Preferences   prefs;
WiFiManager   wifiManager;
WebServer     webServer(80);

unsigned long lastHeartbeat     = 0;
unsigned long heartbeatInterval = (unsigned long)DEFAULT_INTERVAL_S * 1000UL;
unsigned long lastSuccess       = 0;
bool          otaPending        = false;
String        otaUrl            = "";
String        deviceName        = DEVICE_NAME;
String        hubHost           = HUB_HOST;
int           hubPort           = HUB_PORT;


// ================================================================
//  STANDARD-SENDER
// ================================================================
const char* DEFAULT_STATIONS = R"JSON([
  {"name":"Rockantenne Bayern","url":"https://streams.rockantenne.de/rockantenne/stream/icecast"},
  {"name":"Rockantenne Heimweh","url":"https://streams.rockantenne.de/rockantenne-heimweh/stream/icecast"},
  {"name":"Radio BOB!","url":"https://streams.radiobob.de/bob-national/mp3-128/mediaplayer"},
  {"name":"Radio BOB! Rock Classics","url":"https://streams.radiobob.de/bob-rockclassic/mp3-128/mediaplayer"},
  {"name":"Radio BOB! Metal","url":"https://streams.radiobob.de/bob-metal/mp3-128/mediaplayer"},
  {"name":"Radio BOB! Blues","url":"https://streams.radiobob.de/bob-blues/mp3-128/mediaplayer"},
  {"name":"Radio BOB! Reggae","url":"https://streams.radiobob.de/bob-reggae/mp3-128/mediaplayer"}
])JSON";

// ================================================================
//  STANDARD-PODCASTS
// ================================================================
const char* DEFAULT_PODCASTS = R"JSON([
  {"name":"Lage der Nation","url":"https://lagedernation.org/feed/mp3/"},
  {"name":"Chaosradio","url":"https://chaosradio.de/feed/mp3"},
  {"name":"WDR 5 Neugier genuegt","url":"https://www1.wdr.de/mediathek/audio/wdr5/wdr5-neugier-genuegt-das-feature/podcast-wdr5-neugier-genuegt-das-feature-100.podcast"},
  {"name":"Deutschlandfunk Andruck","url":"https://www.deutschlandfunk.de/podcast-andruck-das-buchmagazin.1292.de.podcast.xml"}
])JSON";


// ================================================================
//  STORAGE HELPERS
// ================================================================

String getNvs(const char* ns, const char* key, const char* def) {
    prefs.begin(ns, true);
    String v = prefs.getString(key, def);
    prefs.end();
    return v;
}

void putNvs(const char* ns, const char* key, const String& val) {
    prefs.begin(ns, false);
    prefs.putString(key, val);
    prefs.end();
}

String getStationsJson()  { return getNvs("webradio", "stations",  DEFAULT_STATIONS); }
void   saveStationsJson(const String& j) { putNvs("webradio", "stations", j); }

String getFavoritesJson() {
    String s = getNvs("webradio", "favorites", "[]");
    if (s.length() < 2) return "[]";
    return s;
}
void saveFavoritesJson(const String& j) { putNvs("webradio", "favorites", j); }

String getPodcastsJson() {
    String s = getNvs("webradio", "podcasts", "");
    if (s.length() < 4) return String(DEFAULT_PODCASTS);
    return s;
}
void savePodcastsJson(const String& j) { putNvs("webradio", "podcasts", j); }

void saveLastPlayed(const String& name, const String& url) {
    prefs.begin("webradio", false);
    prefs.putString("lastName", name);
    prefs.putString("lastUrl",  url);
    prefs.end();
}

String getLastPlayedJson() {
    prefs.begin("webradio", true);
    String n = prefs.getString("lastName", "");
    String u = prefs.getString("lastUrl",  "");
    prefs.end();
    // Build JSON manually to avoid allocations
    String out = "{\"name\":\"";
    for (char c : n) { if (c=='"') out+="\\\""; else out+=c; }
    out += "\",\"url\":\"";
    for (char c : u) { if (c=='"') out+="\\\""; else out+=c; }
    out += "\"}";
    return out;
}


// ================================================================
//  HELPERS
// ================================================================

String getMac() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toUpperCase();
    return mac;
}

String getLocalIp()  { return WiFi.localIP().toString(); }

String fmtUptime(unsigned long s) {
    if (s < 60)   return String(s) + "s";
    if (s < 3600) return String(s / 60) + "min " + String(s % 60) + "s";
    return String(s / 3600) + "h " + String((s % 3600) / 60) + "min";
}

String escHtml(const String& s) {
    String out; out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}


// ================================================================
//  WEB-UI — HAUPTSEITE
// ================================================================

void handleRoot() {
    unsigned long upSec = millis() / 1000UL;
    uint32_t heap   = ESP.getFreeHeap();
    uint32_t sketch = ESP.getFreeSketchSpace();

    String html;
    html.reserve(35000);

    // ── HEAD + CSS ──────────────────────────────────────────────
    html += F("<!DOCTYPE html>\n<html lang='de'><head>\n"
              "<meta charset='UTF-8'>\n"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
              "<title>");
    html += escHtml(deviceName);
    html += F(" \u2014 Web-Radio</title>\n<style>\n"
":root{--bg:#0d1117;--bg2:#161b22;--brd:#30363d;--txt:#e6edf3;--muted:#8b949e;--acc:#58a6ff;--green:#3fb950;--yellow:#e3b341;--red:#f85149;--purple:#bc8cff}\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{background:var(--bg);color:var(--txt);font-family:sans-serif;font-size:14px;line-height:1.5}\n"
"header{background:var(--bg2);border-bottom:1px solid var(--brd);padding:14px 20px;display:flex;align-items:center;gap:12px;flex-wrap:wrap}\n"
"h1{font-size:18px;color:var(--acc)}\n"
".badge{background:rgba(88,166,255,.15);color:var(--acc);border:1px solid rgba(88,166,255,.3);padding:2px 8px;border-radius:10px;font-size:11px}\n"
".tabs{display:flex;background:var(--bg2);border-bottom:1px solid var(--brd);padding:0 12px;overflow-x:auto;gap:0}\n"
".tab{padding:10px 16px;cursor:pointer;border:none;border-bottom:2px solid transparent;background:none;color:var(--muted);font-size:13px;font-weight:500;text-decoration:none;display:inline-flex;align-items:center;white-space:nowrap;font-family:inherit;transition:color .15s}\n"
".tab:hover,.tab.active{color:var(--acc);border-bottom-color:var(--acc)}\n"
".pane{padding:16px 20px;max-width:860px;display:none}\n"
".pane.active{display:block}\n"
".card{background:var(--bg2);border:1px solid var(--brd);border-radius:8px;padding:16px;margin-bottom:14px}\n"
".card h3{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.5px;margin-bottom:12px;display:flex;align-items:center;gap:6px}\n"
/* Now Playing */
".np{display:flex;align-items:center;gap:14px;padding:16px;border:1px solid var(--brd);border-radius:8px;margin-bottom:14px;background:linear-gradient(135deg,#161b22,#1a2335);transition:border-color .4s,box-shadow .4s}\n"
".np.playing{border-color:rgba(63,185,80,.6);box-shadow:0 0 24px rgba(63,185,80,.1)}\n"
".np-icon{font-size:32px;flex-shrink:0;width:44px;text-align:center}\n"
".np-info{flex:1;min-width:0}\n"
".np-name{font-size:16px;font-weight:700;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".np-sub{font-size:12px;color:var(--muted);margin-top:3px;min-height:18px}\n"
".np-ctrl{display:flex;align-items:center;gap:8px;flex-shrink:0}\n"
".btn-stop{background:var(--red);color:#fff;border:none;width:34px;height:34px;border-radius:50%;font-size:13px;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:opacity .2s}\n"
".btn-stop:hover{opacity:.8}\n"
".vol{display:flex;align-items:center;gap:5px;color:var(--muted);font-size:12px}\n"
"input[type=range]{accent-color:var(--acc);cursor:pointer;width:65px}\n"
/* Station rows */
".stn-row{display:flex;align-items:center;gap:8px;padding:8px 10px;border-radius:6px;border:1px solid transparent;margin-bottom:4px;transition:all .15s}\n"
".stn-row:hover{background:rgba(88,166,255,.04);border-color:rgba(88,166,255,.18)}\n"
".stn-row.playing{background:rgba(63,185,80,.07);border-color:rgba(63,185,80,.3)}\n"
".stn-info{flex:1;min-width:0}\n"
".stn-name{font-weight:500;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".stn-url{font-size:11px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:1px}\n"
/* Buttons */
".btn-play{background:var(--acc);color:#fff;border:none;padding:5px 11px;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600;flex-shrink:0;transition:all .15s;min-width:36px;text-align:center}\n"
".btn-play:hover{opacity:.85}\n"
".btn-play.on{background:var(--green)}\n"
".btn-fav{background:transparent;border:1px solid var(--brd);color:var(--muted);width:30px;height:30px;border-radius:4px;cursor:pointer;font-size:15px;flex-shrink:0;transition:all .15s;display:flex;align-items:center;justify-content:center;padding:0}\n"
".btn-fav:hover{color:var(--yellow);border-color:var(--yellow)}\n"
".btn-fav.active{color:var(--yellow);border-color:rgba(227,179,65,.5);background:rgba(227,179,65,.1)}\n"
".btn-del{background:transparent;color:var(--muted);border:1px solid var(--brd);padding:4px 8px;border-radius:4px;cursor:pointer;font-size:12px;flex-shrink:0;transition:all .15s}\n"
".btn-del:hover{color:var(--red);border-color:var(--red)}\n"
/* Form */
".form-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:4px}\n"
"input[type=text],input[type=url]{flex:1;min-width:130px;background:#0d1117;color:var(--txt);border:1px solid var(--brd);padding:7px 11px;border-radius:6px;font-size:13px;font-family:inherit}\n"
"input:focus{outline:none;border-color:var(--acc)}\n"
".btn-add{background:var(--green);color:#fff;border:none;padding:7px 16px;border-radius:6px;cursor:pointer;font-weight:600;font-size:13px;flex-shrink:0;font-family:inherit;transition:opacity .2s}\n"
".btn-add:hover{opacity:.85}\n"
".hint{font-size:11px;color:var(--muted);margin-top:8px}\n"
".hint a{color:var(--acc);text-decoration:none}\n"
/* Discover */
".chips{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:14px}\n"
".chip{background:#21262d;color:var(--muted);border:1px solid var(--brd);padding:5px 12px;border-radius:16px;cursor:pointer;font-size:12px;font-family:inherit;transition:all .15s}\n"
".chip:hover{border-color:var(--acc);color:var(--acc)}\n"
".chip.active{background:rgba(88,166,255,.15);border-color:var(--acc);color:var(--acc)}\n"
".search-row{display:flex;gap:8px;margin-bottom:12px}\n"
".search-row input{flex:1}\n"
".btn-search{background:var(--acc);color:#fff;border:none;padding:7px 16px;border-radius:6px;cursor:pointer;font-weight:600;font-family:inherit;flex-shrink:0}\n"
".result-row{display:flex;align-items:center;gap:8px;padding:8px 10px;border-radius:6px;border:1px solid transparent;margin-bottom:4px;transition:all .15s}\n"
".result-row:hover{background:rgba(88,166,255,.04);border-color:rgba(88,166,255,.18)}\n"
".result-info{flex:1;min-width:0}\n"
".result-name{font-weight:500;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".result-meta{font-size:11px;color:var(--muted);margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".tag-chip{background:#21262d;padding:1px 6px;border-radius:8px;font-size:10px;margin-left:3px}\n"
".disc-info{font-size:12px;color:var(--muted);margin-bottom:10px;min-height:16px}\n"
/* Podcasts */
".pod-card{display:flex;align-items:center;gap:8px;padding:10px 12px;border:1px solid var(--brd);border-radius:6px;margin-bottom:6px;transition:all .15s}\n"
".pod-card:hover{border-color:rgba(88,166,255,.35)}\n"
".pod-card.active{border-color:var(--acc);background:rgba(88,166,255,.05)}\n"
".pod-info{flex:1;min-width:0;cursor:pointer}\n"
".pod-name{font-weight:500;font-size:13px}\n"
".pod-url{font-size:11px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}\n"
".ep-list{max-height:380px;overflow-y:auto}\n"
".ep-row{display:flex;align-items:center;gap:8px;padding:9px 10px;border-bottom:1px solid #1c2129;transition:background .1s}\n"
".ep-row:last-child{border-bottom:none}\n"
".ep-row.playing{background:rgba(63,185,80,.06)}\n"
".ep-info{flex:1;min-width:0}\n"
".ep-title{font-size:13px;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
".ep-meta{font-size:11px;color:var(--muted);margin-top:2px}\n"
/* Status table */
"table{width:100%;border-collapse:collapse;font-size:13px}\n"
"td{padding:7px 10px;border-bottom:1px solid #1c2129}\n"
"td:first-child{color:var(--muted);width:42%}\n"
".mono{font-family:monospace;color:var(--acc)}\n"
".bar{height:6px;background:#21262d;border-radius:3px;margin-top:4px;overflow:hidden}\n"
".bar-fill{height:100%;border-radius:3px}\n"
".g{background:var(--green)}.y{background:var(--yellow)}.r{background:var(--red)}\n"
/* Equalizer */
".eq{display:inline-flex;align-items:flex-end;gap:2px;height:18px;vertical-align:middle}\n"
".eq span{display:block;width:3px;background:var(--green);border-radius:2px;transform-origin:bottom;animation:eq .9s ease-in-out infinite}\n"
".eq span:nth-child(1){height:6px;animation-delay:0s}\n"
".eq span:nth-child(2){height:14px;animation-delay:.2s}\n"
".eq span:nth-child(3){height:9px;animation-delay:.07s}\n"
".eq span:nth-child(4){height:16px;animation-delay:.3s}\n"
"@keyframes eq{0%,100%{transform:scaleY(.25)}50%{transform:scaleY(1)}}\n"
"</style>\n</head><body>\n");

    // ── HEADER ──────────────────────────────────────────────────
    html += F("<header><h1>&#127925; ");
    html += escHtml(deviceName);
    html += F("</h1>"
              "<span class='badge'>v"); html += FW_VERSION; html += F("</span>"
              "<span class='badge' style='background:rgba(63,185,80,.12);color:#3fb950;border-color:rgba(63,185,80,.3)'>ESP-Hub</span>"
              "</header>\n");

    // ── TABS ────────────────────────────────────────────────────
    html += F("<div class='tabs'>"
              "<button class='tab active' onclick='showTab(\"radio\",this)'>&#127925; Radio</button>"
              "<button class='tab' onclick='showTab(\"discover\",this)'>&#128269; Entdecken</button>"
              "<button class='tab' onclick='showTab(\"podcasts\",this)'>&#127897; Podcasts</button>"
              "<button class='tab' onclick='showTab(\"status\",this)'>&#128200; Status</button>"
              "<a class='tab' href='/ota'>&#128640; OTA</a>"
              "</div>\n");

    // ══════════════════════════════════════════════════════════
    // RADIO PANE
    // ══════════════════════════════════════════════════════════
    html += F("<div id='pane-radio' class='pane active'>\n"
              // Now Playing
              "<div class='np' id='np-card'>"
              "<div class='np-icon' id='np-icon'>&#127925;</div>"
              "<div class='np-info'>"
              "<div class='np-name' id='np-name'>Kein Sender gew&auml;hlt</div>"
              "<div class='np-sub' id='np-sub'>&mdash; bereit &mdash;</div>"
              "</div>"
              "<div class='np-ctrl'>"
              "<button class='btn-stop' onclick='stopPlay()' title='Stop'>&#9209;</button>"
              "<div class='vol'>&#128264;<input type='range' id='vol' min='0' max='1' step='0.05' value='0.8' oninput='setVol(this.value)'>&#128266;</div>"
              "</div>"
              "</div>\n"
              // Favorites
              "<div class='card'>"
              "<h3>&#11088; Favoriten <span id='fav-badge'></span></h3>"
              "<div id='fav-list'><div style='color:var(--muted);font-size:12px'>Lade...</div></div>"
              "</div>\n"
              // My Stations
              "<div class='card'>"
              "<h3>&#128251; Meine Sender</h3>"
              "<div id='station-list'><div style='color:var(--muted);font-size:12px'>Lade...</div></div>"
              "<hr style='border:none;border-top:1px solid var(--brd);margin:12px 0'>\n"
              "<div style='font-size:11px;color:var(--muted);margin-bottom:8px;text-transform:uppercase;letter-spacing:.5px'>&#10133; Sender hinzuf&uuml;gen</div>"
              "<div class='form-row'>"
              "<input id='add-name' type='text' placeholder='Name (z.B. Bayern 3)'>"
              "<input id='add-url' type='url' placeholder='Stream-URL (https://...)'>"
              "<button class='btn-add' onclick='addStation()'>Hinzuf&uuml;gen</button>"
              "</div>"
              "<p class='hint'>Stream-URLs: <a href='https://www.radio.de' target='_blank'>radio.de</a> &bull; "
              "<a href='https://www.streamurl.link' target='_blank'>streamurl.link</a></p>"
              "</div>\n"
              "</div>\n"); // end pane-radio

    // ══════════════════════════════════════════════════════════
    // DISCOVER PANE
    // ══════════════════════════════════════════════════════════
    html += F("<div id='pane-discover' class='pane'>\n"
              "<div class='card'>\n"
              "<h3>&#128269; Sender suchen</h3>"
              "<div class='search-row'>"
              "<input id='disc-q' type='text' placeholder='Sendername (z.B. Bayern, Jazz, BBC...)' "
              "onkeydown='if(event.key===\"Enter\")doSearch()'>"
              "<button class='btn-search' onclick='doSearch()'>Suchen</button>"
              "</div>"
              "<h3 style='margin-top:4px'>&#127911; Nach Genre st&ouml;bern</h3>"
              "<div class='chips'>"
              "<button class='chip' onclick='browseGenre(\"rock\",this)'>&#127928; Rock</button>"
              "<button class='chip' onclick='browseGenre(\"pop\",this)'>&#127897; Pop</button>"
              "<button class='chip' onclick='browseGenre(\"metal\",this)'>&#128308; Metal</button>"
              "<button class='chip' onclick='browseGenre(\"jazz\",this)'>&#127927; Jazz</button>"
              "<button class='chip' onclick='browseGenre(\"blues\",this)'>&#127926; Blues</button>"
              "<button class='chip' onclick='browseGenre(\"classical\",this)'>&#127908; Klassik</button>"
              "<button class='chip' onclick='browseGenre(\"electronic\",this)'>&#9889; Electronic</button>"
              "<button class='chip' onclick='browseGenre(\"house\",this)'>&#127909; House</button>"
              "<button class='chip' onclick='browseGenre(\"techno\",this)'>&#9654; Techno</button>"
              "<button class='chip' onclick='browseGenre(\"country\",this)'>&#127978; Country</button>"
              "<button class='chip' onclick='browseGenre(\"reggae\",this)'>&#127807; Reggae</button>"
              "<button class='chip' onclick='browseGenre(\"rnb\",this)'>&#127926; R&amp;B</button>"
              "<button class='chip' onclick='browseGenre(\"hiphop\",this)'>&#127907; Hip-Hop</button>"
              "<button class='chip' onclick='browseGenre(\"soul\",this)'>&#10084; Soul</button>"
              "<button class='chip' onclick='browseGenre(\"ambient\",this)'>&#127756; Ambient</button>"
              "<button class='chip' onclick='browseGenre(\"folk\",this)'>&#127795; Folk</button>"
              "<button class='chip' onclick='browseGenre(\"punk\",this)'>&#128308; Punk</button>"
              "<button class='chip' onclick='browseGenre(\"indie\",this)'>&#127917; Indie</button>"
              "<button class='chip' onclick='browseGenre(\"news\",this)'>&#128240; News</button>"
              "<button class='chip' onclick='browseGenre(\"talk\",this)'>&#128483; Talk</button>"
              "<button class='chip' onclick='browseGenre(\"german\",this)'>&#127465;&#127466; Deutsch</button>"
              "</div>"
              "</div>"
              "<div class='card' id='disc-card' style='display:none'>"
              "<h3 id='disc-title'>Ergebnisse</h3>"
              "<div class='disc-info' id='disc-info'></div>"
              "<div id='disc-results'></div>"
              "</div>"
              "</div>\n"); // end pane-discover

    // ══════════════════════════════════════════════════════════
    // PODCASTS PANE
    // ══════════════════════════════════════════════════════════
    html += F("<div id='pane-podcasts' class='pane'>\n"
              "<div class='card'>"
              "<h3>&#127897; Podcasts</h3>"
              "<div id='pod-list'><div style='color:var(--muted);font-size:12px'>Lade...</div></div>"
              "</div>"
              "<div id='ep-panel' style='display:none'>"
              "<div class='card'>"
              "<h3 id='ep-title'>Episoden</h3>"
              "<div class='ep-list' id='ep-list'></div>"
              "</div>"
              "</div>"
              "<div class='card'>"
              "<h3>&#10133; Podcast hinzuf&uuml;gen</h3>"
              "<div class='form-row'>"
              "<input id='pod-name' type='text' placeholder='Name (z.B. Lage der Nation)'>"
              "<input id='pod-url' type='url' placeholder='RSS Feed-URL (https://...feed.xml)'>"
              "<button class='btn-add' onclick='addPodcast()'>Hinzuf&uuml;gen</button>"
              "</div>"
              "<p class='hint'>RSS-Feeds findest du auf <a href='https://www.podcast.de' target='_blank'>podcast.de</a>"
              " oder in deiner Podcast-App beim &bdquo;Feed teilen&ldquo;</p>"
              "</div>"
              "</div>\n"); // end pane-podcasts

    // ══════════════════════════════════════════════════════════
    // STATUS PANE (server-side rendered)
    // ══════════════════════════════════════════════════════════
    html += F("<div id='pane-status' class='pane'>\n");

    // Device card
    html += F("<div class='card'><h3>Ger&auml;t</h3><table>");
    html += "<tr><td>Name</td><td>" + escHtml(deviceName) + "</td></tr>";
    html += "<tr><td>Chip</td><td>" + String(ESP.getChipModel()) + "</td></tr>";
    html += "<tr><td>MAC</td><td class='mono'>" + getMac() + "</td></tr>";
    html += "<tr><td>IP-Adresse</td><td class='mono'>" + getLocalIp() + "</td></tr>";
    html += "<tr><td>WLAN RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
    html += "<tr><td>Uptime</td><td>" + fmtUptime(upSec) + "</td></tr>";
    html += "<tr><td>Firmware</td><td>v" + String(FW_VERSION) + "</td></tr>";
    html += F("</table></div>\n");

    // Memory card
    String hC = (heap>100000)?"g":(heap>50000)?"y":"r";
    String sC = (sketch>500000)?"g":(sketch>200000)?"y":"r";
    html += F("<div class='card'><h3>Speicher</h3><table>");
    html += "<tr><td>Freier RAM</td><td>" + String(heap/1024) + " KB"
            "<div class='bar'><div class='bar-fill " + hC + "' style='width:" + String(min(100UL,heap/3276UL)) + "%'></div></div></td></tr>";
    html += "<tr><td>Freier Flash (OTA)</td><td>" + String(sketch/1024) + " KB"
            "<div class='bar'><div class='bar-fill " + sC + "' style='width:" + String(min(100UL,sketch/19660UL)) + "%'></div></div></td></tr>";
    html += F("</table></div>\n");

    // Hub card
    html += F("<div class='card'><h3>ESP-Hub</h3><table>");
    html += "<tr><td>Hub-Adresse</td><td class='mono'>" + escHtml(hubHost) + ":" + String(hubPort) + "</td></tr>";
    html += "<tr><td>Heartbeat</td><td>alle " + String(heartbeatInterval/1000) + " Sekunden</td></tr>";
    html += F("</table></div>"
              "<button onclick='location.reload()' style='background:#21262d;color:var(--muted);border:1px solid var(--brd);padding:8px 16px;border-radius:6px;cursor:pointer;font-size:13px'>&#8635; Aktualisieren</button>\n");
    html += F("</div>\n"); // end pane-status

    // ── AUDIO ELEMENT ────────────────────────────────────────────
    html += F("<audio id='audio' preload='none'></audio>\n");

    // ══════════════════════════════════════════════════════════
    // JAVASCRIPT
    // ══════════════════════════════════════════════════════════
    html += F("<script>\n"
// ── Global State ──────────────────────────────────────────────
"var stnList=[],favList=[],podList=[];\n"
"var curUrl='',curName='',isPlaying=false;\n"
"var discResults=[],discLoading=false;\n"
"var curPodIdx=-1,curEpisodes=[];\n"
"var audio=document.getElementById('audio');\n\n"

// ── Helpers ───────────────────────────────────────────────────
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}\n"
"function esj(s){return String(s).replace(/\\\\/g,'\\\\\\\\').replace(/'/g,\"\\\\'\").replace(/\\n/g,'\\\\n').replace(/\\r/g,'');}\n\n"

// ── Tab Management ────────────────────────────────────────────
"function showTab(id,btn){\n"
"  document.querySelectorAll('.pane').forEach(function(p){p.classList.remove('active');});\n"
"  document.querySelectorAll('.tab').forEach(function(b){b.classList.remove('active');});\n"
"  document.getElementById('pane-'+id).classList.add('active');\n"
"  btn.classList.add('active');\n"
"}\n\n"

// ── Player ────────────────────────────────────────────────────
"function playUrl(url,name){\n"
"  if(url===curUrl&&isPlaying){audio.pause();isPlaying=false;updateNP();renderAll();return;}\n"
"  curUrl=url;curName=name;\n"
"  audio.src=url;\n"
"  audio.volume=parseFloat(document.getElementById('vol').value);\n"
"  audio.play().catch(function(e){console.warn('play:',e);});\n"
"  isPlaying=true;\n"
"  document.getElementById('np-sub').textContent='\\u23f3 Verbinde...';\n"
"  updateNP();renderAll();\n"
"  fetch('/api/last',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:name,url:url})});\n"
"}\n"
"function stopPlay(){\n"
"  audio.pause();audio.src='';isPlaying=false;curUrl='';curName='';\n"
"  document.getElementById('np-sub').textContent='\\u23f9 Gestoppt';\n"
"  updateNP();renderAll();\n"
"}\n"
"function setVol(v){audio.volume=parseFloat(v);try{localStorage.setItem('rv',v);}catch(e){}}\n\n"

"function updateNP(){\n"
"  var card=document.getElementById('np-card');\n"
"  var nameEl=document.getElementById('np-name');\n"
"  var subEl=document.getElementById('np-sub');\n"
"  var iconEl=document.getElementById('np-icon');\n"
"  if(curUrl&&isPlaying){\n"
"    nameEl.textContent=curName;\n"
"    document.title='\\u25b6 '+curName;\n"
"    iconEl.innerHTML='<div class=\"eq\"><span></span><span></span><span></span><span></span></div>';\n"
"    card.classList.add('playing');\n"
"    subEl.innerHTML='<span style=\"color:var(--green)\">&bull; Live</span>';\n"
"  }else if(curUrl&&!isPlaying){\n"
"    nameEl.textContent=curName;\n"
"    document.title='\\u23f8 '+curName;\n"
"    iconEl.textContent='\\u{1F4FB}';\n"
"    card.classList.remove('playing');\n"
"    subEl.textContent='\\u23f8 Pausiert \\u2013 \\u25b6 zum Fortsetzen';\n"
"  }else{\n"
"    nameEl.textContent='Kein Sender gew\\u00e4hlt';\n"
"    document.title='\\u{1F3B5} Web-Radio';\n"
"    iconEl.textContent='\\u{1F3B5}';\n"
"    card.classList.remove('playing');\n"
"    fetch('/api/last').then(function(r){return r.json();}).then(function(d){\n"
"      if(d.name&&d.url){\n"
"        subEl.innerHTML='<span style=\"cursor:pointer;color:var(--acc)\" onclick=\"playUrl(\\''+esj(d.url)+'\\',\\''+esj(d.name)+'\\')\">'+'\\u25b6 '+esc(d.name)+' fortsetzen</span>';\n"
"      }else{subEl.textContent='\\u2014 bereit \\u2014';}\n"
"    }).catch(function(){subEl.textContent='\\u2014 bereit \\u2014';});\n"
"  }\n"
"}\n\n"

// ── Favorites ─────────────────────────────────────────────────
"function isFav(url){return favList.some(function(f){return f.url===url;});}\n"
"function toggleFav(name,url){\n"
"  var idx=favList.findIndex(function(f){return f.url===url;});\n"
"  if(idx>=0){\n"
"    fetch('/api/favorites-remove',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:idx})})\n"
"      .then(function(){return loadFavs();});\n"
"  }else{\n"
"    fetch('/api/favorites-add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:name,url:url})})\n"
"      .then(function(){return loadFavs();});\n"
"  }\n"
"}\n"
"function loadFavs(){\n"
"  return fetch('/api/favorites').then(function(r){return r.json();}).then(function(d){\n"
"    favList=d;renderAll();\n"
"  });\n"
"}\n"
"function renderFavs(){\n"
"  var el=document.getElementById('fav-list');\n"
"  var badge=document.getElementById('fav-badge');\n"
"  if(badge)badge.textContent=favList.length?'('+favList.length+')':'';\n"
"  if(!favList.length){\n"
"    el.innerHTML='<p style=\"color:var(--muted);font-size:12px;padding:6px 0\">Noch keine Favoriten &mdash; f&uuml;ge Sender mit &#11088; im Entdecken-Tab hinzu</p>';\n"
"    return;\n"
"  }\n"
"  var h='';\n"
"  favList.forEach(function(f,i){\n"
"    var playing=(f.url===curUrl&&isPlaying);\n"
"    h+='<div class=\"stn-row'+(f.url===curUrl?' playing':'')+'\">'\n"
"      +'<div class=\"stn-info\"><div class=\"stn-name\">'+esc(f.name)+'</div></div>'\n"
"      +'<button class=\"btn-play'+(playing?' on':'')+'\" onclick=\"playUrl(\\''+esj(f.url)+'\\',\\''+esj(f.name)+'\\')\">'\n"
"      +(playing?'&#9646;&#9646;':'&#9654;')+'</button>'\n"
"      +'<button class=\"btn-fav active\" onclick=\"toggleFav(\\''+esj(f.name)+'\\',\\''+esj(f.url)+'\\')\" title=\"Aus Favoriten\">&#11088;</button>'\n"
"      +'</div>';\n"
"  });\n"
"  el.innerHTML=h;\n"
"}\n\n"

// ── Stations ──────────────────────────────────────────────────
"function loadStations(){\n"
"  return fetch('/api/stations').then(function(r){return r.json();}).then(function(d){stnList=d;renderStations();});\n"
"}\n"
"function renderStations(){\n"
"  var el=document.getElementById('station-list');\n"
"  if(!stnList.length){\n"
"    el.innerHTML='<p style=\"color:var(--muted);font-size:12px;padding:6px 0\">Keine Sender. F&uuml;ge unten hinzu.</p>';\n"
"    return;\n"
"  }\n"
"  var h='';\n"
"  stnList.forEach(function(s,i){\n"
"    var playing=(s.url===curUrl&&isPlaying);\n"
"    var fav=isFav(s.url);\n"
"    h+='<div class=\"stn-row'+(s.url===curUrl?' playing':'')+'\">'\n"
"      +'<div class=\"stn-info\"><div class=\"stn-name\">'+esc(s.name)+'</div>'\n"
"      +'<div class=\"stn-url\">'+esc(s.url.replace(/^https?:\\/\\//,'').substring(0,55))+'</div></div>'\n"
"      +'<button class=\"btn-play'+(playing?' on':'')+'\" onclick=\"playUrl(\\''+esj(s.url)+'\\',\\''+esj(s.name)+'\\')\">'\n"
"      +(playing?'&#9646;&#9646;':'&#9654;')+'</button>'\n"
"      +'<button class=\"btn-fav'+(fav?' active':'')+'\" onclick=\"toggleFav(\\''+esj(s.name)+'\\',\\''+esj(s.url)+'\\')\" title=\"Favorit\">'\n"
"      +(fav?'&#11088;':'&#9734;')+'</button>'\n"
"      +'<button class=\"btn-del\" onclick=\"delStation('+i+')\" title=\"L\\u00f6schen\">&#10005;</button>'\n"
"      +'</div>';\n"
"  });\n"
"  el.innerHTML=h;\n"
"}\n"
"function addStation(){\n"
"  var n=document.getElementById('add-name').value.trim();\n"
"  var u=document.getElementById('add-url').value.trim();\n"
"  if(!n||!u){alert('Name und URL erforderlich');return;}\n"
"  if(!u.match(/^https?:\\/\\//)){alert('URL muss mit http:// beginnen');return;}\n"
"  fetch('/api/stations-add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n,url:u})})\n"
"    .then(function(r){if(!r.ok)return r.text().then(function(t){throw t;});})\n"
"    .then(function(){document.getElementById('add-name').value='';document.getElementById('add-url').value='';loadStations();})\n"
"    .catch(function(e){alert('Fehler: '+e);});\n"
"}\n"
"function delStation(idx){\n"
"  if(!confirm('Sender \"'+stnList[idx].name+'\" l\\u00f6schen?'))return;\n"
"  fetch('/api/stations-delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:idx})})\n"
"    .then(function(){loadStations();});\n"
"}\n\n"

// ── Discover ──────────────────────────────────────────────────
"async function browseGenre(tag,btn){\n"
"  if(discLoading)return;\n"
"  discLoading=true;\n"
"  document.querySelectorAll('.chip').forEach(function(c){c.classList.remove('active');});\n"
"  if(btn)btn.classList.add('active');\n"
"  var card=document.getElementById('disc-card');\n"
"  var info=document.getElementById('disc-info');\n"
"  var title=document.getElementById('disc-title');\n"
"  card.style.display='block';\n"
"  title.textContent=tag.charAt(0).toUpperCase()+tag.slice(1)+' Sender';\n"
"  info.textContent='\\u23f3 Lade...';\n"
"  document.getElementById('disc-results').innerHTML='';\n"
"  try{\n"
"    var r=await fetch('https://de1.api.radio-browser.info/json/stations/bytag/'\n"
"      +encodeURIComponent(tag)+'?limit=40&hidebroken=true&order=votes&reverse=true&codec=MP3',\n"
"      {headers:{'User-Agent':'ESP32-WebRadio/2.0','Accept':'application/json'}});\n"
"    discResults=await r.json();\n"
"    renderDiscover(tag);\n"
"  }catch(e){info.textContent='\\u26a0 Verbindungsfehler: '+e.message;}\n"
"  discLoading=false;\n"
"}\n"
"async function doSearch(){\n"
"  if(discLoading)return;\n"
"  var q=document.getElementById('disc-q').value.trim();\n"
"  if(!q){alert('Suchbegriff eingeben');return;}\n"
"  discLoading=true;\n"
"  document.querySelectorAll('.chip').forEach(function(c){c.classList.remove('active');});\n"
"  var card=document.getElementById('disc-card');\n"
"  var info=document.getElementById('disc-info');\n"
"  var title=document.getElementById('disc-title');\n"
"  card.style.display='block';\n"
"  title.textContent='Suche: '+q;\n"
"  info.textContent='\\u23f3 Suche l\\u00e4uft...';\n"
"  document.getElementById('disc-results').innerHTML='';\n"
"  try{\n"
"    var r=await fetch('https://de1.api.radio-browser.info/json/stations/search?name='\n"
"      +encodeURIComponent(q)+'&limit=40&hidebroken=true&order=votes&reverse=true',\n"
"      {headers:{'User-Agent':'ESP32-WebRadio/2.0','Accept':'application/json'}});\n"
"    discResults=await r.json();\n"
"    renderDiscover(q);\n"
"  }catch(e){info.textContent='\\u26a0 Verbindungsfehler: '+e.message;}\n"
"  discLoading=false;\n"
"}\n"
"function renderDiscover(label){\n"
"  var el=document.getElementById('disc-results');\n"
"  var info=document.getElementById('disc-info');\n"
"  if(!discResults.length){info.textContent='Keine Ergebnisse f\\u00fcr: '+label;el.innerHTML='';return;}\n"
"  info.textContent=discResults.length+' Sender gefunden (powered by radio-browser.info):';\n"
"  var h='';\n"
"  discResults.forEach(function(s){\n"
"    var url=s.url_resolved||s.url;\n"
"    if(!url)return;\n"
"    var fav=isFav(url);\n"
"    var playing=(url===curUrl&&isPlaying);\n"
"    var tagHtml=(s.tags||'').split(',').filter(function(t){return t.trim();}).slice(0,3)\n"
"      .map(function(t){return '<span class=\"tag-chip\">'+esc(t.trim())+'</span>';}).join('');\n"
"    var country=s.country?esc(s.country):'';\n"
"    h+='<div class=\"result-row\">'\n"
"      +'<div class=\"result-info\">'\n"
"      +'<div class=\"result-name\">'+esc(s.name)+'</div>'\n"
"      +'<div class=\"result-meta\">'+(country?country+' ':'')+tagHtml+'</div>'\n"
"      +'</div>'\n"
"      +'<button class=\"btn-play'+(playing?' on':'')+'\" onclick=\"playUrl(\\''+esj(url)+'\\',\\''+esj(s.name)+'\\')\">'\n"
"      +(playing?'&#9646;&#9646;':'&#9654;')+'</button>'\n"
"      +'<button class=\"btn-fav'+(fav?' active':'')+'\" onclick=\"toggleFav(\\''+esj(s.name)+'\\',\\''+esj(url)+'\\')\" title=\"Zu Favoriten\">'\n"
"      +(fav?'&#11088;':'&#9734;')+'</button>'\n"
"      +'</div>';\n"
"  });\n"
"  el.innerHTML=h;\n"
"}\n\n"

// ── Podcasts ──────────────────────────────────────────────────
"function loadPodcasts(){\n"
"  return fetch('/api/podcasts').then(function(r){return r.json();}).then(function(d){podList=d;renderPodcasts();});\n"
"}\n"
"function renderPodcasts(){\n"
"  var el=document.getElementById('pod-list');\n"
"  if(!podList.length){\n"
"    el.innerHTML='<p style=\"color:var(--muted);font-size:12px;padding:6px 0\">Keine Podcasts. F&uuml;ge unten hinzu.</p>';\n"
"    return;\n"
"  }\n"
"  var h='';\n"
"  podList.forEach(function(p,i){\n"
"    h+='<div class=\"pod-card'+(i===curPodIdx?' active':'')+'\">'\n"
"      +'<div class=\"pod-info\" onclick=\"selectPodcast('+i+')\">'\n"
"      +'<div class=\"pod-name\">'+esc(p.name)+'</div>'\n"
"      +'<div class=\"pod-url\">'+esc(p.url.replace(/^https?:\\/\\//,'').substring(0,60))+'</div>'\n"
"      +'</div>'\n"
"      +'<button class=\"btn-del\" onclick=\"delPodcast('+i+')\">&#10005;</button>'\n"
"      +'</div>';\n"
"  });\n"
"  el.innerHTML=h;\n"
"}\n"
"async function selectPodcast(idx){\n"
"  curPodIdx=idx;\n"
"  renderPodcasts();\n"
"  var panel=document.getElementById('ep-panel');\n"
"  var epList=document.getElementById('ep-list');\n"
"  var epTitle=document.getElementById('ep-title');\n"
"  panel.style.display='block';\n"
"  epTitle.textContent=podList[idx].name;\n"
"  epList.innerHTML='<div style=\"color:var(--muted);padding:16px;text-align:center\">\\u23f3 Lade Episoden via RSS...</div>';\n"
"  try{\n"
"    var proxy='https://api.allorigins.win/get?url='+encodeURIComponent(podList[idx].url);\n"
"    var r=await fetch(proxy);\n"
"    var data=await r.json();\n"
"    var xml=new DOMParser().parseFromString(data.contents,'text/xml');\n"
"    var items=Array.from(xml.querySelectorAll('item')).slice(0,25);\n"
"    curEpisodes=items.map(function(item){\n"
"      var enc=item.querySelector('enclosure');\n"
"      var dur=item.querySelector('duration');\n"
"      return{\n"
"        title:(item.querySelector('title')||{textContent:''}).textContent.trim()||'(kein Titel)',\n"
"        url:(enc&&enc.getAttribute('url'))||'',\n"
"        date:((item.querySelector('pubDate')||{textContent:''}).textContent).trim().substring(5,16),\n"
"        duration:((dur&&dur.textContent)||'').trim()\n"
"      };\n"
"    }).filter(function(e){return e.url;});\n"
"    renderEpisodes();\n"
"  }catch(e){\n"
"    epList.innerHTML='<div style=\"color:var(--red);padding:16px\">\\u26a0 Feed konnte nicht geladen werden.<br><small>'+esc(String(e))+'</small></div>';\n"
"  }\n"
"}\n"
"function renderEpisodes(){\n"
"  var el=document.getElementById('ep-list');\n"
"  if(!curEpisodes.length){el.innerHTML='<p style=\"color:var(--muted);padding:14px\">Keine Episoden im Feed.</p>';return;}\n"
"  var h='';\n"
"  curEpisodes.forEach(function(e){\n"
"    var playing=(e.url===curUrl&&isPlaying);\n"
"    h+='<div class=\"ep-row'+(e.url===curUrl?' playing':'')+'\">'\n"
"      +'<div class=\"ep-info\">'\n"
"      +'<div class=\"ep-title\">'+esc(e.title)+'</div>'\n"
"      +'<div class=\"ep-meta\">'+esc(e.date)+(e.duration?' &middot; '+esc(e.duration):'')+'</div>'\n"
"      +'</div>'\n"
"      +'<button class=\"btn-play'+(playing?' on':'')+'\" onclick=\"playUrl(\\''+esj(e.url)+'\\',\\''+esj(e.title)+'\\')\">'\n"
"      +(playing?'&#9646;&#9646;':'&#9654;')+'</button>'\n"
"      +'</div>';\n"
"  });\n"
"  el.innerHTML=h;\n"
"}\n"
"function addPodcast(){\n"
"  var n=document.getElementById('pod-name').value.trim();\n"
"  var u=document.getElementById('pod-url').value.trim();\n"
"  if(!n||!u){alert('Name und Feed-URL erforderlich');return;}\n"
"  fetch('/api/podcasts-add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:n,url:u})})\n"
"    .then(function(r){if(!r.ok)return r.text().then(function(t){throw t;});})\n"
"    .then(function(){document.getElementById('pod-name').value='';document.getElementById('pod-url').value='';loadPodcasts();})\n"
"    .catch(function(e){alert('Fehler: '+e);});\n"
"}\n"
"function delPodcast(idx){\n"
"  if(!confirm('Podcast \"'+podList[idx].name+'\" entfernen?'))return;\n"
"  if(curPodIdx===idx){curPodIdx=-1;document.getElementById('ep-panel').style.display='none';}\n"
"  fetch('/api/podcasts-remove',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:idx})})\n"
"    .then(function(){loadPodcasts();});\n"
"}\n\n"

// ── Combined Render ───────────────────────────────────────────
"function renderAll(){renderFavs();renderStations();if(discResults.length)renderDiscover('');if(curEpisodes.length)renderEpisodes();}\n\n"

// ── Audio Events ──────────────────────────────────────────────
"audio.addEventListener('playing',function(){\n"
"  isPlaying=true;updateNP();renderAll();\n"
"});\n"
"audio.addEventListener('waiting',function(){\n"
"  document.getElementById('np-sub').textContent='\\u23f3 Puffere...';\n"
"});\n"
"audio.addEventListener('stalled',function(){\n"
"  document.getElementById('np-sub').textContent='\\u26a0 Verbindung unterbrochen...';\n"
"});\n"
"audio.addEventListener('error',function(){\n"
"  if(!audio.src)return;\n"
"  isPlaying=false;\n"
"  document.getElementById('np-sub').textContent='\\u274c Stream-Fehler \\u2013 URL pr\\u00fcfen';\n"
"  document.getElementById('np-card').classList.remove('playing');\n"
"  renderAll();\n"
"});\n"
"audio.addEventListener('pause',function(){if(!audio.src)return;isPlaying=false;updateNP();renderAll();});\n\n"

// ── Init ──────────────────────────────────────────────────────
"(function init(){\n"
"  var sv=null;try{sv=localStorage.getItem('rv');}catch(e){}\n"
"  if(sv){document.getElementById('vol').value=sv;audio.volume=parseFloat(sv);}\n"
"  Promise.all([loadStations(),loadFavs(),loadPodcasts()]).then(function(){updateNP();});\n"
"})();\n"
"</script>\n</body></html>\n");

    webServer.send(200, "text/html; charset=UTF-8", html);
}


// ================================================================
//  OTA PAGE
// ================================================================

void handleOtaPage() {
    String html;
    html.reserve(5000);
    html += F("<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>OTA Update</title>"
              "<style>"
              "*{box-sizing:border-box;margin:0;padding:0}"
              "body{background:#0d1117;color:#e6edf3;font-family:sans-serif;font-size:14px}"
              "header{background:#161b22;border-bottom:1px solid #30363d;padding:14px 20px;display:flex;align-items:center;gap:12px}"
              "h1{font-size:18px;color:#58a6ff}"
              ".badge{background:rgba(88,166,255,.15);color:#58a6ff;border:1px solid rgba(88,166,255,.3);padding:2px 8px;border-radius:10px;font-size:11px}"
              ".tabs{display:flex;background:#161b22;border-bottom:1px solid #30363d;padding:0 12px}"
              ".tab{padding:10px 16px;border-bottom:2px solid transparent;color:#8b949e;text-decoration:none;display:inline-flex;align-items:center;font-size:13px;font-weight:500}"
              ".tab.active{color:#58a6ff;border-bottom-color:#58a6ff}"
              ".c{padding:20px;max-width:800px}"
              ".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:20px;margin-bottom:14px}"
              ".card h3{font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:.5px;margin-bottom:16px}"
              ".drop{border:2px dashed #30363d;border-radius:8px;padding:32px;text-align:center;cursor:pointer;color:#8b949e;transition:all .2s}"
              ".drop:hover{border-color:#58a6ff;color:#e6edf3}"
              "input[type=file]{display:none}"
              ".btn{background:#238636;color:#fff;border:none;padding:10px 24px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:600;margin-top:14px;width:100%;font-family:inherit}"
              ".btn:hover{background:#2ea043}"
              ".prog{display:none;margin-top:14px}"
              ".bar{height:8px;background:#21262d;border-radius:4px;overflow:hidden}"
              ".bar-fill{height:100%;background:#58a6ff;border-radius:4px;width:0%;transition:width .3s}"
              ".msg{margin-top:10px;font-size:13px;color:#8b949e;text-align:center}"
              ".warn{background:rgba(227,179,65,.1);border:1px solid rgba(227,179,65,.3);border-radius:6px;padding:12px;color:#e3b341;font-size:13px;margin-bottom:14px}"
              "</style></head><body>"
              "<header><h1>&#127925; ");
    html += escHtml(deviceName);
    html += F("</h1><span class='badge'>v"); html += FW_VERSION; html += F("</span></header>"
              "<div class='tabs'>"
              "<a class='tab' href='/'>&#127925; Radio</a>"
              "<a class='tab active' href='/ota'>&#128640; OTA Update</a>"
              "</div>"
              "<div class='c'><div class='card'>"
              "<h3>&#128640; Firmware hochladen</h3>"
              "<div class='warn'>&#9888; Nach dem Hochladen startet der ESP automatisch neu.</div>"
              "<div class='drop' id='drop' onclick='document.getElementById(\"fw\").click()'>"
              "&#128190; <b>.bin</b> hierher ziehen oder klicken"
              "</div>"
              "<input type='file' id='fw' accept='.bin'>"
              "<div id='fn' style='margin-top:8px;font-size:12px;color:#8b949e;text-align:center'></div>"
              "<button class='btn' onclick='go()'>&#9889; Flashen</button>"
              "<div class='prog' id='prog'>"
              "<div class='bar'><div class='bar-fill' id='bar'></div></div>"
              "<div class='msg' id='msg'></div>"
              "</div></div></div>"
              "<script>"
              "var inp=document.getElementById('fw');"
              "inp.onchange=function(){if(inp.files[0])document.getElementById('fn').textContent=inp.files[0].name+' ('+Math.round(inp.files[0].size/1024)+' KB)';};"
              "var drop=document.getElementById('drop');"
              "drop.ondragover=function(e){e.preventDefault();drop.style.borderColor='#58a6ff';};"
              "drop.ondragleave=function(){drop.style.borderColor='#30363d';};"
              "drop.ondrop=function(e){e.preventDefault();drop.style.borderColor='#30363d';var f=e.dataTransfer.files[0];if(f&&f.name.endsWith('.bin')){var dt=new DataTransfer();dt.items.add(f);inp.files=dt.files;document.getElementById('fn').textContent=f.name+' ('+Math.round(f.size/1024)+' KB)';};};"
              "function go(){if(!inp.files[0]){alert('Bitte zuerst .bin ausw\\u00e4hlen');return;}"
              "var fd=new FormData();fd.append('firmware',inp.files[0]);"
              "var xhr=new XMLHttpRequest();xhr.open('POST','/ota-upload');"
              "document.getElementById('prog').style.display='block';"
              "xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);document.getElementById('bar').style.width=p+'%';document.getElementById('msg').textContent='Hochladen: '+p+'%';}};"
              "xhr.onload=function(){if(xhr.status===200){document.getElementById('bar').style.width='100%';document.getElementById('bar').style.background='#3fb950';document.getElementById('msg').textContent='OK! ESP startet neu...';}else{document.getElementById('msg').textContent='Fehler: '+xhr.responseText;document.getElementById('bar').style.background='#f85149';}};"
              "xhr.onerror=function(){document.getElementById('msg').textContent='Verbindungsfehler';};"
              "xhr.send(fd);}"
              "</script></body></html>");
    webServer.send(200, "text/html; charset=UTF-8", html);
}


// ================================================================
//  OTA UPLOAD
// ================================================================

void handleOtaUpload() {
    HTTPUpload& upload = webServer.upload();
    if      (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[WEB-OTA] Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[WEB-OTA] OK! %u Bytes\n", upload.totalSize);
        else Update.printError(Serial);
    }
}

void handleOtaUploadFinish() {
    if (Update.hasError()) webServer.send(500, "text/plain", "OTA fehlgeschlagen!");
    else                   webServer.send(200, "text/plain", "OK");
    delay(500);
    ESP.restart();
}

void handleNotFound() {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
}


// ================================================================
//  GENERIC JSON ARRAY ADD/REMOVE HELPER
// ================================================================

// Parses existing JSON array, calls addFn to add an item, saves via saveFn
static bool jsonArrayAdd(
    const String& existing,
    const String& newName, const String& newUrl,
    int maxItems,
    String& outJson)
{
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    DynamicJsonDocument doc(8192);
#endif
    deserializeJson(doc, existing);
    JsonArray arr = doc.as<JsonArray>();
    if ((int)arr.size() >= maxItems) return false;

#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonObject obj = arr.add<JsonObject>();
#else
    JsonObject obj = arr.createNestedObject();
#endif
    obj["name"] = newName;
    obj["url"]  = newUrl;
    serializeJson(doc, outJson);
    return true;
}

static bool jsonArrayRemove(const String& existing, int idx, String& outJson) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    DynamicJsonDocument doc(8192);
#endif
    deserializeJson(doc, existing);
    JsonArray arr = doc.as<JsonArray>();
    if (idx < 0 || idx >= (int)arr.size()) return false;
    arr.remove(idx);
    serializeJson(doc, outJson);
    return true;
}


// ================================================================
//  API HANDLERS — STATIONS
// ================================================================

void handleApiStations() {
    webServer.send(200, "application/json", getStationsJson());
}

void handleApiStationsAdd() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String n = req["name"].as<String>(), u = req["url"].as<String>();
    if (!n.length()||!u.length()) { webServer.send(400,"text/plain","name+url erford."); return; }
    if (!u.startsWith("http"))    { webServer.send(400,"text/plain","Ungueltige URL"); return; }
    String out;
    if (!jsonArrayAdd(getStationsJson(), n, u, MAX_STATIONS, out)) {
        webServer.send(400,"text/plain","Maximum erreicht"); return;
    }
    saveStationsJson(out);
    webServer.send(200,"application/json",out);
}

void handleApiStationsDelete() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(64);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String out;
    if (!jsonArrayRemove(getStationsJson(), req["index"]|(-1), out)) {
        webServer.send(400,"text/plain","Ungültiger Index"); return;
    }
    saveStationsJson(out);
    webServer.send(200,"application/json",out);
}


// ================================================================
//  API HANDLERS — FAVORITES
// ================================================================

void handleApiFavorites() {
    webServer.send(200,"application/json",getFavoritesJson());
}

void handleApiFavoritesAdd() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String n = req["name"].as<String>(), u = req["url"].as<String>();
    if (!n.length()||!u.length()) { webServer.send(400,"text/plain","name+url erford."); return; }
    String out;
    if (!jsonArrayAdd(getFavoritesJson(), n, u, MAX_FAVORITES, out)) {
        webServer.send(400,"text/plain","Maximum erreicht"); return;
    }
    saveFavoritesJson(out);
    webServer.send(200,"application/json",out);
}

void handleApiFavoritesRemove() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(64);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String out;
    if (!jsonArrayRemove(getFavoritesJson(), req["index"]|(-1), out)) {
        webServer.send(400,"text/plain","Ungültiger Index"); return;
    }
    saveFavoritesJson(out);
    webServer.send(200,"application/json",out);
}


// ================================================================
//  API HANDLERS — PODCASTS
// ================================================================

void handleApiPodcasts() {
    webServer.send(200,"application/json",getPodcastsJson());
}

void handleApiPodcastsAdd() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String n = req["name"].as<String>(), u = req["url"].as<String>();
    if (!n.length()||!u.length()) { webServer.send(400,"text/plain","name+url erford."); return; }
    String out;
    if (!jsonArrayAdd(getPodcastsJson(), n, u, MAX_PODCASTS, out)) {
        webServer.send(400,"text/plain","Maximum erreicht"); return;
    }
    savePodcastsJson(out);
    webServer.send(200,"application/json",out);
}

void handleApiPodcastsRemove() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(64);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String out;
    if (!jsonArrayRemove(getPodcastsJson(), req["index"]|(-1), out)) {
        webServer.send(400,"text/plain","Ungültiger Index"); return;
    }
    savePodcastsJson(out);
    webServer.send(200,"application/json",out);
}


// ================================================================
//  API HANDLERS — LAST PLAYED
// ================================================================

void handleApiLastGet() {
    webServer.send(200,"application/json",getLastPlayedJson());
}

void handleApiLastPost() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    saveLastPlayed(req["name"].as<String>(), req["url"].as<String>());
    webServer.send(200,"application/json","{\"ok\":true}");
}


// ================================================================
//  WEB SERVER SETUP
// ================================================================

void setupWebServer() {
    webServer.on("/",                    HTTP_GET,  handleRoot);
    webServer.on("/ota",                 HTTP_GET,  handleOtaPage);
    webServer.on("/ota-upload",          HTTP_POST, handleOtaUploadFinish, handleOtaUpload);

    webServer.on("/api/stations",        HTTP_GET,  handleApiStations);
    webServer.on("/api/stations-add",    HTTP_POST, handleApiStationsAdd);
    webServer.on("/api/stations-delete", HTTP_POST, handleApiStationsDelete);

    webServer.on("/api/favorites",        HTTP_GET,  handleApiFavorites);
    webServer.on("/api/favorites-add",    HTTP_POST, handleApiFavoritesAdd);
    webServer.on("/api/favorites-remove", HTTP_POST, handleApiFavoritesRemove);

    webServer.on("/api/podcasts",        HTTP_GET,  handleApiPodcasts);
    webServer.on("/api/podcasts-add",    HTTP_POST, handleApiPodcastsAdd);
    webServer.on("/api/podcasts-remove", HTTP_POST, handleApiPodcastsRemove);

    webServer.on("/api/last",            HTTP_GET,  handleApiLastGet);
    webServer.on("/api/last",            HTTP_POST, handleApiLastPost);

    webServer.onNotFound(handleNotFound);
    webServer.begin();
    Serial.println("[WEB] Server: http://" + getLocalIp() + "/");
}


// ================================================================
//  HEARTBEAT + OTA
// ================================================================

String buildHeartbeat() {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    DynamicJsonDocument doc(1024);
#endif
    doc["mac"]        = getMac();
    doc["name"]       = deviceName;
    doc["hwType"]     = "esp32";
    doc["chipModel"]  = ESP.getChipModel();
    doc["version"]    = FW_VERSION;
    doc["ip"]         = getLocalIp();
    doc["rssi"]       = WiFi.RSSI();
    doc["uptime"]     = millis() / 1000UL;
    doc["freeHeap"]   = ESP.getFreeHeap();
    doc["freeSketch"] = ESP.getFreeSketchSpace();

    String favJson = getFavoritesJson();
    String stnJson = getStationsJson();
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument fa; JsonDocument sa;
#else
    DynamicJsonDocument fa(4096); DynamicJsonDocument sa(4096);
#endif
    deserializeJson(fa, favJson);
    deserializeJson(sa, stnJson);

    JsonObject ios = doc["ios"].to<JsonObject>();

#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonObject ioFav = ios["favoriten"].to<JsonObject>();
    JsonObject ioStn = ios["sender"].to<JsonObject>();
#else
    JsonObject ioFav = ios.createNestedObject("favoriten");
    JsonObject ioStn = ios.createNestedObject("sender");
#endif
    ioFav["type"]="info"; ioFav["value"]=(float)fa.as<JsonArray>().size(); ioFav["unit"]="";
    ioStn["type"]="info"; ioStn["value"]=(float)sa.as<JsonArray>().size(); ioStn["unit"]="";

    String out; serializeJson(doc, out); return out;
}

void sendHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[HB] Kein WLAN"); return; }
    String url = "http://" + hubHost + ":" + String(hubPort) + "/api/register";
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    int code = http.POST(buildHeartbeat());
    if (code == 200) {
        String body = http.getString();
        lastSuccess = millis();
#if ARDUINOJSON_VERSION_MAJOR >= 7
        JsonDocument resp;
#else
        DynamicJsonDocument resp(512);
#endif
        if (deserializeJson(resp, body) == DeserializationError::Ok) {
            if (resp.containsKey("interval")) {
                unsigned long ni = (unsigned long)(int)resp["interval"] * 1000UL;
                if (ni != heartbeatInterval && ni >= 5000UL) heartbeatInterval = ni;
            }
            if (resp.containsKey("otaUrl") && !resp["otaUrl"].isNull()) {
                String u = resp["otaUrl"].as<String>();
                if (u.length() > 0) { otaPending = true; otaUrl = u; }
            }
        }
    } else { Serial.printf("[HB] HTTP %d\n", code); }
    http.end();
}

void performOta(const String& url) {
    Serial.println("[OTA] Start: " + url);
    HTTPClient http; http.begin(url); http.setTimeout(30000);
    int code = http.GET();
    if (code != 200) { Serial.printf("[OTA] HTTP %d\n", code); http.end(); return; }
    int total = http.getSize();
    if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); http.end(); return; }
    WiFiClient* s = http.getStreamPtr();
    uint8_t buf[512]; size_t written = 0;
    while (http.connected() && (total <= 0 || written < (size_t)total)) {
        size_t av = s->available();
        if (!av) { delay(1); continue; }
        size_t r = s->readBytes(buf, min(av, sizeof(buf)));
        if (!r) break;
        Update.write(buf, r); written += r;
    }
    if (Update.end(true)) { Serial.println("[OTA] OK! Neustart..."); http.end(); delay(500); ESP.restart(); }
    else { Update.printError(Serial); }
    http.end();
}


// ================================================================
//  WLAN + RESET
// ================================================================

void checkResetButton() {
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(RESET_BUTTON_PIN) == HIGH) return;
    Serial.println("[RESET] Halte " + String(RESET_HOLD_SEC) + "s...");
    unsigned long start = millis();
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
        if (millis() - start > (unsigned long)RESET_HOLD_SEC * 1000UL) {
            Serial.println("[RESET] WLAN geloescht!");
            wifiManager.resetSettings();
            prefs.begin("esphub", false); prefs.clear(); prefs.end();
            delay(500); ESP.restart();
        }
        delay(100);
    }
}

void setupWifi() {
    prefs.begin("esphub", false);
    String savedName = prefs.getString("name",     deviceName);
    String savedHost = prefs.getString("hub_host", hubHost);
    int    savedPort = prefs.getInt   ("hub_port", hubPort);
    prefs.end();
    deviceName = savedName; hubHost = savedHost; hubPort = savedPort;

    WiFiManagerParameter paramName("name",     "Geraetename", deviceName.c_str(), 32);
    WiFiManagerParameter paramHost("hub_host", "ESP-Hub IP",  hubHost.c_str(),    40);
    WiFiManagerParameter paramPort("hub_port", "ESP-Hub Port",String(hubPort).c_str(), 6);
    wifiManager.addParameter(&paramName);
    wifiManager.addParameter(&paramHost);
    wifiManager.addParameter(&paramPort);
    wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    wifiManager.setAPCallback([](WiFiManager*){ Serial.println("[WiFi] Hotspot: " WIFI_AP_NAME); });

    if (!wifiManager.autoConnect(WIFI_AP_NAME)) {
        Serial.println("[WiFi] Timeout - Neustart"); delay(1000); ESP.restart();
    }

    prefs.begin("esphub", false);
    prefs.putString("name",     String(paramName.getValue()));
    prefs.putString("hub_host", String(paramHost.getValue()));
    prefs.putInt   ("hub_port", String(paramPort.getValue()).toInt());
    prefs.end();
    deviceName = String(paramName.getValue());
    hubHost    = String(paramHost.getValue());
    hubPort    = String(paramPort.getValue()).toInt();
    Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
    Serial.println("[WiFi] " + deviceName + " | Hub: " + hubHost + ":" + String(hubPort));
}


// ================================================================
//  SETUP & LOOP
// ================================================================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ESP32 Web-Radio v" FW_VERSION " ===");
    Serial.println("BOOT-Taste 3s = WLAN zuruecksetzen");
    checkResetButton();
    setupWifi();

    String mdnsName = "webradio-" + getMac().substring(6);
    if (MDNS.begin(mdnsName.c_str()))
        Serial.println("[mDNS] " + mdnsName + ".local");

    setupWebServer();
    lastSuccess = millis();
    sendHeartbeat();
    lastHeartbeat = millis();
}

void loop() {
    webServer.handleClient();
    unsigned long now = millis();

    if (now - lastHeartbeat >= heartbeatInterval) {
        lastHeartbeat = now;
        sendHeartbeat();
    }

    if (otaPending) {
        otaPending = false;
        performOta(otaUrl);
        otaUrl = "";
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Verbindung verloren - reconnect...");
        delay(5000);
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    }

    delay(10);
}
