// ╔══════════════════════════════════════════════════════════════╗
// ║  esp32.webradio.ino — Web-Radio für ESP32                   ║
// ║  Version: 2.4.0                                             ║
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

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define BOARD_TYPE "esp32s3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define BOARD_TYPE "esp32c3"
#else
  #define BOARD_TYPE "esp32"
#endif

#define DEVICE_NAME            "Web-Radio"
#define FW_VERSION             "2.4.0"
#define HUB_HOST               "192.168.178.113"
#define HUB_PORT               8093
#define WIFI_AP_NAME           "ESP-WebRadio"
#define WIFI_PORTAL_TIMEOUT_S  180
#define DEFAULT_INTERVAL_S     60
#define RESET_BUTTON_PIN       0
#define RESET_HOLD_SEC         3
#define MAX_STATIONS           30
#define MAX_FAVORITES          30
#define MAX_PODCASTS           20
#define PROXY_MAX_BYTES        180000

struct IoValue { String key; String type; float value; String unit; };
IoValue ioTable[] = {};
const int IO_COUNT = 0;
void updateIoValues() {}

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

const char* DEFAULT_STATIONS = R"JSON([
  {"name":"Rockantenne Bayern","url":"https://streams.rockantenne.de/rockantenne/stream/icecast"},
  {"name":"Rockantenne Heimweh","url":"https://streams.rockantenne.de/rockantenne-heimweh/stream/icecast"},
  {"name":"Radio BOB!","url":"https://streams.radiobob.de/bob-national/mp3-128/mediaplayer"},
  {"name":"Radio BOB! Rock Classics","url":"https://streams.radiobob.de/bob-rockclassic/mp3-128/mediaplayer"},
  {"name":"Radio BOB! Metal","url":"https://streams.radiobob.de/bob-metal/mp3-128/mediaplayer"},
  {"name":"Bayern 3","url":"https://dispatcher.streams.radio.ard.de/br/bayern3/live/mp3/128"},
  {"name":"WDR 2","url":"https://wdr-wdr2-rheinland.icecastssl.wdr.de/wdr/wdr2/rheinland/mp3/128/stream.mp3"}
])JSON";

const char* DEFAULT_PODCASTS = R"JSON([
  {"name":"Lage der Nation","url":"https://lagedernation.org/feed/mp3/"},
  {"name":"Chaosradio","url":"https://chaosradio.de/feed/mp3"},
  {"name":"WDR 5 Neugier genuegt","url":"https://www1.wdr.de/mediathek/audio/wdr5/wdr5-neugier-genuegt-das-feature/podcast-wdr5-neugier-genuegt-das-feature-100.podcast"},
  {"name":"Deutschlandfunk Andruck","url":"https://www.deutschlandfunk.de/podcast-andruck-das-buchmagazin.1292.de.podcast.xml"}
])JSON";

String getNvs(const char* ns, const char* key, const char* def) {
    prefs.begin(ns, true); String v = prefs.getString(key, def); prefs.end(); return v;
}
void putNvs(const char* ns, const char* key, const String& val) {
    prefs.begin(ns, false); prefs.putString(key, val); prefs.end();
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
    prefs.putString("lastName", name); prefs.putString("lastUrl", url); prefs.end();
}
String getLastPlayedJson() {
    prefs.begin("webradio", true);
    String n = prefs.getString("lastName", ""); String u = prefs.getString("lastUrl", ""); prefs.end();
    String out = "{\"name\":\"";
    for (char c : n) { if (c=='"') out+="\\\""; else out+=c; }
    out += "\",\"url\":\"";
    for (char c : u) { if (c=='"') out+="\\\""; else out+=c; }
    out += "\"}"; return out;
}
String getMac() { String mac = WiFi.macAddress(); mac.replace(":", ""); mac.toUpperCase(); return mac; }
String getLocalIp()  { return WiFi.localIP().toString(); }
String fmtUptime(unsigned long s) {
    if (s < 60) return String(s) + "s";
    if (s < 3600) return String(s / 60) + "min " + String(s % 60) + "s";
    return String(s / 3600) + "h " + String((s % 3600) / 60) + "min";
}
String escHtml(const String& s) {
    String out; out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '&') out += "&amp;"; else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;"; else if (c == '"') out += "&quot;"; else out += c;
    }
    return out;
}
String escJson(const String& s) {
    String out; out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out;
}

static const char *const WR_UI_B64[] = {
  "PCFET0NUWVBFIGh0bWw+PGh0bWwgbGFuZz1kZT48aGVhZD4KPG1ldGEgY2hhcnNldD1VVEYtOD4KPG1ldGEgbmFtZT12aWV3cG9y",
  "dCBjb250ZW50PSJ3aWR0aD1kZXZpY2Utd2lkdGgsaW5pdGlhbC1zY2FsZT0xIj4KPG1ldGEgbmFtZT10aGVtZS1jb2xvciBjb250",
  "ZW50PSMwYjBlMTQ+Cjx0aXRsZT5XZWItUmFkaW88L3RpdGxlPgo8bGluayByZWw9cHJlY29ubmVjdCBocmVmPWh0dHBzOi8vZm9u",
  "dHMuYnVubnkubmV0Pgo8bGluayBocmVmPSJodHRwczovL2ZvbnRzLmJ1bm55Lm5ldC9jc3M/ZmFtaWx5PWJyaWNvbGFnZS1ncm90",
  "ZXNxdWU6NjAwLDcwMHxmaWd0cmVlOjQwMCw1MDAsNjAwJmRpc3BsYXk9c3dhcCIgcmVsPXN0eWxlc2hlZXQ+CjxzdHlsZT4KOnJv",
  "b3R7LS1pbms6IzBiMGUxNDstLWxpbmU6cmdiYSgyNTUsMjU1LDI1NSwuMDgpOy0tdHh0OiNmM2VmZTY7LS1tdXRlZDojOWFhM2Iy",
  "Oy0tYW1iZXI6I2U4YTU0YjstLWFtYmVyMjojZmZjNTZkOy0tbWludDojNWRjYWE1Oy0tZGFuZ2VyOiNmZjZiNmI7LS1mb250LWQ6",
  "J0JyaWNvbGFnZSBHcm90ZXNxdWUnLHN5c3RlbS11aSxzYW5zLXNlcmlmOy0tZm9udC1iOkZpZ3RyZWUsc3lzdGVtLXVpLHNhbnMt",
  "c2VyaWZ9Cip7Ym94LXNpemluZzpib3JkZXItYm94O21hcmdpbjowO3BhZGRpbmc6MH0KYm9keXttaW4taGVpZ2h0OjEwMHZoO2Nv",
  "bG9yOnZhcigtLXR4dCk7Zm9udC1mYW1pbHk6dmFyKC0tZm9udC1iKTtiYWNrZ3JvdW5kOnJhZGlhbC1ncmFkaWVudCgxMjAwcHgg",
  "NjAwcHggYXQgMTAlIC0xMCUscmdiYSgyMzIsMTY1LDc1LC4xOCksdHJhbnNwYXJlbnQgNTUlKSxyYWRpYWwtZ3JhZGllbnQoOTAw",
  "cHggNTAwcHggYXQgMTAwJSAwJSxyZ2JhKDkzLDIwMiwxNjUsLjEwKSx0cmFuc3BhcmVudCA0NSUpLGxpbmVhci1ncmFkaWVudCgx",
  "ODBkZWcsIzBiMGUxNCAwJSwjMTAxNjIyIDQ1JSwjMGIwZTE0IDEwMCUpO2JhY2tncm91bmQtYXR0YWNobWVudDpmaXhlZH0KYXtj",
  "b2xvcjp2YXIoLS1hbWJlcjIpO3RleHQtZGVjb3JhdGlvbjpub25lfQouc2hlbGx7bWF4LXdpZHRoOjkyMHB4O21hcmdpbjowIGF1",
  "dG87cGFkZGluZzowIDE4cHggNDhweH0KLmhlcm97cG9zaXRpb246cmVsYXRpdmU7cGFkZGluZzoyOHB4IDAgMThweDtvdmVyZmxv",
  "dzpoaWRkZW59Ci5icmFuZHtmb250LWZhbWlseTp2YXIoLS1mb250LWQpO2ZvbnQtc2l6ZTpjbGFtcCgyLjJyZW0sN3Z3LDMuNHJl",
  "bSk7bGV0dGVyLXNwYWNpbmc6LS4wM2VtO2xpbmUtaGVpZ2h0Oi45NTtiYWNrZ3JvdW5kOmxpbmVhci1ncmFkaWVudCgxMjBkZWcs",
  "dmFyKC0tdHh0KSx2YXIoLS1hbWJlcjIpKTstd2Via2l0LWJhY2tncm91bmQtY2xpcDp0ZXh0O2JhY2tncm91bmQtY2xpcDp0ZXh0",
  "O2NvbG9yOnRyYW5zcGFyZW50O2FuaW1hdGlvbjpyaXNlIC43cyBlYXNlIGJvdGh9Ci50YWdsaW5le21hcmdpbi10b3A6MTBweDtj",
  "b2xvcjp2YXIoLS1tdXRlZCk7Zm9udC1zaXplOi45OHJlbTttYXgtd2lkdGg6MzRyZW07YW5pbWF0aW9uOnJpc2UgLjhzIC4wOHMg",
  "ZWFzZSBib3RofQoubm93e21hcmdpbi10b3A6MjJweDtkaXNwbGF5OmdyaWQ7Z3JpZC10ZW1wbGF0ZS1jb2x1bW5zOjg4cHggMWZy",
  "O2dhcDoxNnB4O2FsaWduLWl0ZW1zOmNlbnRlcjthbmltYXRpb246cmlzZSAuODVzIC4xMnMgZWFzZSBib3RofQouYXJ0e3dpZHRo",
  "Ojg4cHg7aGVpZ2h0Ojg4cHg7Ym9yZGVyLXJhZGl1czoxOHB4O2JhY2tncm91bmQ6bGluZWFyLWdyYWRpZW50KDE0NWRlZywjMWEy",
  "MjMzLCMyYTFmMTIpO2JvcmRlcjoxcHggc29saWQgdmFyKC0tbGluZSk7ZGlzcGxheTpncmlkO3BsYWNlLWl0ZW1zOmNlbnRlcjtv",
  "dmVyZmxvdzpoaWRkZW47Ym94LXNoYWRvdzowIDEycHggNDBweCByZ2JhKDAsMCwwLC4zNSl9Ci5hcnQgaW1ne3dpZHRoOjEwMCU7",
  "aGVpZ2h0OjEwMCU7b2JqZWN0LWZpdDpjb3ZlcjtkaXNwbGF5Om5vbmV9Ci5hcnQgLmVxe2Rpc3BsYXk6ZmxleDthbGlnbi1pdGVt",
  "czpmbGV4LWVuZDtnYXA6M3B4O2hlaWdodDoyOHB4fQouYXJ0IC5lcSBpe2Rpc3BsYXk6YmxvY2s7d2lkdGg6NHB4O2JhY2tncm91",
  "bmQ6dmFyKC0tYW1iZXIpO2JvcmRlci1yYWRpdXM6MnB4O2FuaW1hdGlvbjplcSAxcyBlYXNlLWluLW91dCBpbmZpbml0ZTt0cmFu",
  "c2Zvcm0tb3JpZ2luOmJvdHRvbX0KLmFydCAuZXEgaTpudGgtY2hpbGQoMSl7aGVpZ2h0OjEwcHh9LmFydCAuZXEgaTpudGgtY2hp",
  "bGQoMil7aGVpZ2h0OjIycHg7YW5pbWF0aW9uLWRlbGF5Oi4xNXN9Ci5hcnQgLmVxIGk6bnRoLWNoaWxkKDMpe2hlaWdodDoxNHB4",
  "O2FuaW1hdGlvbi1kZWxheTouMDVzfS5hcnQgLmVxIGk6bnRoLWNoaWxkKDQpe2hlaWdodDoyNnB4O2FuaW1hdGlvbi1kZWxheTou",
  "MjVzfQoubm93LmxpdmUgLmFydHtib3JkZXItY29sb3I6cmdiYSgyMzIsMTY1LDc1LC40NSk7Ym94LXNoYWRvdzowIDAgMCAxcHgg",
  "cmdiYSgyMzIsMTY1LDc1LC4yKSwwIDE2cHggNTBweCByZ2JhKDIzMiwxNjUsNzUsLjEyKX0KLm5wLW5hbWV7Zm9udC1mYW1pbHk6",
  "dmFyKC0tZm9udC1kKTtmb250LXNpemU6MS4zNXJlbTtsZXR0ZXItc3BhY2luZzotLjAyZW07d2hpdGUtc3BhY2U6bm93cmFwO292",
  "ZXJmbG93OmhpZGRlbjt0ZXh0LW92ZXJmbG93OmVsbGlwc2lzfQoubnAtc3Vie21hcmdpbi10b3A6NHB4O2NvbG9yOnZhcigtLW11",
  "dGVkKTtmb250LXNpemU6LjlyZW07bWluLWhlaWdodDoxLjJlbX0KLmN0cmx7bWFyZ2luLXRvcDoxMnB4O2Rpc3BsYXk6ZmxleDtm",
  "bGV4LXdyYXA6d3JhcDtnYXA6MTBweDthbGlnbi1pdGVtczpjZW50ZXJ9Ci5idG57YXBwZWFyYW5jZTpub25lO2JvcmRlcjowO2N1",
  "cnNvcjpwb2ludGVyO2ZvbnQtZmFtaWx5OmluaGVyaXQ7Zm9udC13ZWlnaHQ6NjAwO2JvcmRlci1yYWRpdXM6OTk5cHg7cGFkZGlu",
  "ZzoxMHB4IDE2cHg7YmFja2dyb3VuZDpsaW5lYXItZ3JhZGllbnQoMTM1ZGVnLHZhcigtLWFtYmVyKSwjYzQ3ZDI4KTtjb2xvcjoj",
  "MWExMjA4O3RyYW5zaXRpb246dHJhbnNmb3JtIC4xNXMsZmlsdGVyIC4xNXN9Ci5idG46aG92ZXJ7ZmlsdGVyOmJyaWdodG5lc3Mo",
  "MS4wNSk7dHJhbnNmb3JtOnRyYW5zbGF0ZVkoLTFweCl9Ci5idG4uZ2hvc3R7YmFja2dyb3VuZDp0cmFuc3BhcmVudDtjb2xvcjp2",
  "YXIoLS10eHQpO2JvcmRlcjoxcHggc29saWQgdmFyKC0tbGluZSl9Ci5idG4uc217cGFkZGluZzo3cHggMTJweDtmb250LXNpemU6",
  "Ljg1cmVtO2JvcmRlci1yYWRpdXM6MTBweH0KLmJ0bi5pY29ue3dpZHRoOjQwcHg7aGVpZ2h0OjQwcHg7cGFkZGluZzowO2Rpc3Bs",
  "YXk6Z3JpZDtwbGFjZS1pdGVtczpjZW50ZXI7Ym9yZGVyLXJhZGl1czoxMnB4fQouYnRuLnBsYXlvbntiYWNrZ3JvdW5kOmxpbmVh",
  "ci1ncmFkaWVudCgxMzVkZWcsdmFyKC0tbWludCksIzJmOWU3OCk7Y29sb3I6IzA0MTQwZn0KLnZvbHtkaXNwbGF5OmZsZXg7YWxp",
  "Z24taXRlbXM6Y2VudGVyO2dhcDo4cHg7Y29sb3I6dmFyKC0tbXV0ZWQpO2ZvbnQtc2l6ZTouODVyZW19CmlucHV0W3R5cGU9cmFu",
  "Z2Vde2FjY2VudC1jb2xvcjp2YXIoLS1hbWJlcik7d2lkdGg6OTBweH0KLnRpcHttYXJnaW4tdG9wOjE0cHg7Zm9udC1zaXplOi44",
  "MnJlbTtjb2xvcjp2YXIoLS1tdXRlZCk7Ym9yZGVyLWxlZnQ6MnB4IHNvbGlkIHJnYmEoMjMyLDE2NSw3NSwuNDUpO3BhZGRpbmct",
  "bGVmdDoxMHB4O2FuaW1hdGlvbjpyaXNlIC45cyAuMThzIGVhc2UgYm90aH0KbmF2LnRhYnN7ZGlzcGxheTpmbGV4O2dhcDo0cHg7",
  "b3ZlcmZsb3cteDphdXRvO3BhZGRpbmc6OHB4IDAgMTRweDtib3JkZXItYm90dG9tOjFweCBzb2xpZCB2YXIoLS1saW5lKTtwb3Np",
  "dGlvbjpzdGlja3k7dG9wOjA7YmFja2Ryb3AtZmlsdGVyOmJsdXIoMTBweCk7YmFja2dyb3VuZDpyZ2JhKDExLDE0LDIwLC43Mik7",
  "ei1pbmRleDo1fQpuYXYudGFicyBidXR0b24sbmF2LnRhYnMgYXtmbGV4OjAgMCBhdXRvO2JhY2tncm91bmQ6bm9uZTtib3JkZXI6",
  "MDtjb2xvcjp2YXIoLS1tdXRlZCk7Zm9udDo2MDAgLjkycmVtIHZhcigtLWZvbnQtYik7cGFkZGluZzoxMHB4IDEycHg7Y3Vyc29y",
  "OnBvaW50ZXI7Ym9yZGVyLWJvdHRvbToycHggc29saWQgdHJhbnNwYXJlbnQ7dGV4dC1kZWNvcmF0aW9uOm5vbmV9Cm5hdi50YWJz",
  "IGJ1dHRvbi5hY3RpdmUsbmF2LnRhYnMgYTpob3ZlcixuYXYudGFicyBidXR0b246aG92ZXJ7Y29sb3I6dmFyKC0tdHh0KTtib3Jk",
  "ZXItYm90dG9tLWNvbG9yOnZhcigtLWFtYmVyKX0KLnBhbmV7ZGlzcGxheTpub25lO3BhZGRpbmctdG9wOjE4cHh9LnBhbmUuYWN0",
  "aXZle2Rpc3BsYXk6YmxvY2s7YW5pbWF0aW9uOnJpc2UgLjM1cyBlYXNlfQouc2VjLWh7Zm9udC1mYW1pbHk6dmFyKC0tZm9udC1k",
  "KTtmb250LXNpemU6MS4wNXJlbTttYXJnaW46MThweCAwIDEwcHg7bGV0dGVyLXNwYWNpbmc6LS4wMWVtfQoubXV0ZWR7Y29sb3I6",
  "dmFyKC0tbXV0ZWQpO2ZvbnQtc2l6ZTouODZyZW19Ci5yb3d7ZGlzcGxheTpmbGV4O2FsaWduLWl0ZW1zOmNlbnRlcjtnYXA6MTBw",
  "eDtwYWRkaW5nOjEwcHggOHB4O2JvcmRlci1yYWRpdXM6MTJweDtib3JkZXI6MXB4IHNvbGlkIHRyYW5zcGFyZW50O3RyYW5zaXRp",
  "b246YmFja2dyb3VuZCAuMTVzLGJvcmRlci1jb2xvciAuMTVzfQoucm93OmhvdmVye2JhY2tncm91bmQ6cmdiYSgyNTUsMjU1LDI1",
  "NSwuMDMpO2JvcmRlci1jb2xvcjp2YXIoLS1saW5lKX0KLnJvdy5vbntiYWNrZ3JvdW5kOnJnYmEoMjMyLDE2NSw3NSwuMDcpO2Jv",
  "cmRlci1jb2xvcjpyZ2JhKDIzMiwxNjUsNzUsLjI1KX0KLnJvdyAuaW5mb3tmbGV4OjE7bWluLXdpZHRoOjB9LnJvdyAubmFtZXtm",
  "b250LXdlaWdodDo2MDA7d2hpdGUtc3BhY2U6bm93cmFwO292ZXJmbG93OmhpZGRlbjt0ZXh0LW92ZXJmbG93OmVsbGlwc2lzfQou",
  "cm93IC5tZXRhe2ZvbnQtc2l6ZTouNzhyZW07Y29sb3I6dmFyKC0tbXV0ZWQpO21hcmdpbi10b3A6MnB4O3doaXRlLXNwYWNlOm5v",
  "d3JhcDtvdmVyZmxvdzpoaWRkZW47dGV4dC1vdmVyZmxvdzplbGxpcHNpc30KLmZhdntiYWNrZ3JvdW5kOnRyYW5zcGFyZW50O2Jv",
  "cmRlcjoxcHggc29saWQgdmFyKC0tbGluZSk7Y29sb3I6dmFyKC0tbXV0ZWQpO3dpZHRoOjM2cHg7aGVpZ2h0OjM2cHg7Ym9yZGVy",
  "LXJhZGl1czoxMHB4O2N1cnNvcjpwb2ludGVyfQouZmF2Lm9ue2NvbG9yOnZhcigtLWFtYmVyKTtib3JkZXItY29sb3I6cmdiYSgy",
  "MzIsMTY1LDc1LC40NSk7YmFja2dyb3VuZDpyZ2JhKDIzMiwxNjUsNzUsLjEpfQouZGVse2JhY2tncm91bmQ6dHJhbnNwYXJlbnQ7",
  "Ym9yZGVyOjFweCBzb2xpZCB2YXIoLS1saW5lKTtjb2xvcjp2YXIoLS1tdXRlZCk7cGFkZGluZzo3cHggMTBweDtib3JkZXItcmFk",
  "aXVzOjEwcHg7Y3Vyc29yOnBvaW50ZXJ9Ci5kZWw6aG92ZXJ7Y29sb3I6dmFyKC0tZGFuZ2VyKTtib3JkZXItY29sb3I6dmFyKC0t",
  "ZGFuZ2VyKX0KLmNoaXBze2Rpc3BsYXk6ZmxleDtmbGV4LXdyYXA6d3JhcDtnYXA6OHB4O21hcmdpbjo4cHggMCAxNHB4fQouY2hp",
  "cHtiYWNrZ3JvdW5kOnJnYmEoMjU1LDI1NSwyNTUsLjA0KTtib3JkZXI6MXB4IHNvbGlkIHZhcigtLWxpbmUpO2NvbG9yOnZhcigt",
  "LW11dGVkKTtwYWRkaW5nOjdweCAxMnB4O2JvcmRlci1yYWRpdXM6OTk5cHg7Y3Vyc29yOnBvaW50ZXI7Zm9udDo1MDAgLjgycmVt",
  "IHZhcigtLWZvbnQtYil9Ci5jaGlwOmhvdmVyLC5jaGlwLm9ue2NvbG9yOnZhcigtLWluayk7YmFja2dyb3VuZDp2YXIoLS1hbWJl",
  "cik7Ym9yZGVyLWNvbG9yOnZhcigtLWFtYmVyKX0KLnNyY3tkaXNwbGF5OmZsZXg7ZmxleC13cmFwOndyYXA7Z2FwOjhweDttYXJn",
  "aW4tYm90dG9tOjEycHh9Ci5zcmMgYnV0dG9ue2JhY2tncm91bmQ6cmdiYSgyNTUsMjU1LDI1NSwuMDMpO2JvcmRlcjoxcHggc29s",
  "aWQgdmFyKC0tbGluZSk7Y29sb3I6dmFyKC0tbXV0ZWQpO3BhZGRpbmc6OHB4IDEycHg7Ym9yZGVyLXJhZGl1czoxMnB4O2N1cnNv",
  "cjpwb2ludGVyO2ZvbnQ6NjAwIC44NXJlbSB2YXIoLS1mb250LWIpfQouc3JjIGJ1dHRvbi5vbntjb2xvcjp2YXIoLS10eHQpO2Jv",
  "cmRlci1jb2xvcjpyZ2JhKDIzMiwxNjUsNzUsLjUpO2JhY2tncm91bmQ6cmdiYSgyMzIsMTY1LDc1LC4xMil9Ci5zZWFyY2h7ZGlz",
  "cGxheTpmbGV4O2dhcDo4cHg7bWFyZ2luOjEwcHggMCAxNHB4fQouc2VhcmNoIGlucHV0LGlucHV0W3R5cGU9dGV4dF0saW5wdXRb",
  "dHlwZT11cmxde2ZsZXg6MTttaW4td2lkdGg6MDtiYWNrZ3JvdW5kOnJnYmEoMCwwLDAsLjI1KTtib3JkZXI6MXB4IHNvbGlkIHZh",
  "cigtLWxpbmUpO2NvbG9yOnZhcigtLXR4dCk7cGFkZGluZzoxMXB4IDEycHg7Ym9yZGVyLXJhZGl1czoxMnB4O2ZvbnQ6NTAwIC45",
  "MnJlbSB2YXIoLS1mb250LWIpfQouc2VhcmNoIGlucHV0OmZvY3VzLGlucHV0OmZvY3Vze291dGxpbmU6bm9uZTtib3JkZXItY29s",
  "b3I6cmdiYSgyMzIsMTY1LDc1LC41NSl9Ci5mb3Jte2Rpc3BsYXk6ZmxleDtmbGV4LXdyYXA6d3JhcDtnYXA6OHB4O21hcmdpbi10",
  "b3A6OHB4fQouZm9ybSBpbnB1dHtmbGV4OjE7bWluLXdpZHRoOjE0MHB4fQp0YWJsZXt3aWR0aDoxMDAlO2JvcmRlci1jb2xsYXBz",
  "ZTpjb2xsYXBzZTtmb250LXNpemU6LjkycmVtfXRke3BhZGRpbmc6OHB4IDZweDtib3JkZXItYm90dG9tOjFweCBzb2xpZCB2YXIo",
  "LS1saW5lKX0KdGQ6Zmlyc3QtY2hpbGR7Y29sb3I6dmFyKC0tbXV0ZWQpO3dpZHRoOjQwJX0ubW9ub3tmb250LWZhbWlseTp1aS1t",
  "b25vc3BhY2UsbW9ub3NwYWNlO2NvbG9yOnZhcigtLWFtYmVyMil9Ci5iYXJ7aGVpZ2h0OjZweDtiYWNrZ3JvdW5kOnJnYmEoMjU1",
  "LDI1NSwyNTUsLjA2KTtib3JkZXItcmFkaXVzOjk5cHg7bWFyZ2luLXRvcDo1cHg7b3ZlcmZsb3c6aGlkZGVufQouYmFyPml7ZGlz",
  "cGxheTpibG9jaztoZWlnaHQ6MTAwJTtiYWNrZ3JvdW5kOmxpbmVhci1ncmFkaWVudCg5MGRlZyx2YXIoLS1hbWJlciksdmFyKC0t",
  "bWludCkpfQoudGh1bWJ7d2lkdGg6NDBweDtoZWlnaHQ6NDBweDtib3JkZXItcmFkaXVzOjEwcHg7b2JqZWN0LWZpdDpjb3Zlcjti",
  "YWNrZ3JvdW5kOiMxYTIyMzM7ZmxleC1zaHJpbms6MH0KLnBvZHtkaXNwbGF5OmZsZXg7Z2FwOjEwcHg7YWxpZ24taXRlbXM6Y2Vu",
  "dGVyO3BhZGRpbmc6MTJweCA4cHg7Ym9yZGVyLWJvdHRvbToxcHggc29saWQgdmFyKC0tbGluZSk7Y3Vyc29yOnBvaW50ZXJ9Ci5w",
  "b2QuYWN0aXZle2JhY2tncm91bmQ6cmdiYSgyMzIsMTY1LDc1LC4wNil9Ci5lcHttYXgtaGVpZ2h0OjM4MHB4O292ZXJmbG93OmF1",
  "dG99CkBrZXlmcmFtZXMgZXF7MCUsMTAwJXt0cmFuc2Zvcm06c2NhbGVZKC4zKX01MCV7dHJhbnNmb3JtOnNjYWxlWSgxKX19CkBr",
  "ZXlmcmFtZXMgcmlzZXtmcm9te29wYWNpdHk6MDt0cmFuc2Zvcm06dHJhbnNsYXRlWSgxMHB4KX10b3tvcGFjaXR5OjE7dHJhbnNm",
  "b3JtOm5vbmV9fQpAbWVkaWEobWF4LXdpZHRoOjU2MHB4KXsubm93e2dyaWQtdGVtcGxhdGUtY29sdW1uczo3MnB4IDFmcn0uYXJ0",
  "e3dpZHRoOjcycHg7aGVpZ2h0OjcycHg7Ym9yZGVyLXJhZGl1czoxNHB4fX0KPC9zdHlsZT48L2hlYWQ+PGJvZHk+PGRpdiBjbGFz",
  "cz1zaGVsbD4KPHNlY3Rpb24gY2xhc3M9aGVybz4KPGRpdiBjbGFzcz1icmFuZD5XRULCt1JBRElPPC9kaXY+CjxwIGNsYXNzPXRh",
  "Z2xpbmU+TG9rYWxlIFNlbmRlciwgR2VucmVzIHVuZCBQb2RjYXN0cyDigJQgYWJzcGllbGVuIGltIEJyb3dzZXIsIHNwZWljaGVy",
  "biBhdWYgZGVtIEVTUC48L3A+CjxkaXYgY2xhc3M9bm93IGlkPW5vdz4KPGRpdiBjbGFzcz1hcnQgaWQ9YXJ0PjxkaXYgY2xhc3M9",
  "ZXEgaWQ9ZXE+PGk+PC9pPjxpPjwvaT48aT48L2k+PGk+PC9pPjwvZGl2PjxpbWcgaWQ9YXJ0aW1nIGFsdD0iIj48L2Rpdj4KPGRp",
  "dj4KPGRpdiBjbGFzcz1ucC1uYW1lIGlkPW5wLW5hbWU+QmVyZWl0PC9kaXY+CjxkaXYgY2xhc3M9bnAtc3ViIGlkPW5wLXN1Yj5X",
  "w6RobGUgZWluZW4gU2VuZGVyPC9kaXY+CjxkaXYgY2xhc3M9Y3RybD4KPGJ1dHRvbiBjbGFzcz0iYnRuIGljb24iIGlkPWJ0bi1w",
  "bGF5IG9uY2xpY2s9dG9nZ2xlUGxheSgpIHRpdGxlPVBsYXkvUGF1c2U+4pa2PC9idXR0b24+CjxidXR0b24gY2xhc3M9ImJ0biBn",
  "aG9zdCBpY29uIiBvbmNsaWNrPXN0b3BQbGF5KCkgdGl0bGU9U3RvcD7ilqA8L2J1dHRvbj4KPGRpdiBjbGFzcz12b2w+8J+UiiA8",
  "aW5wdXQgaWQ9dm9sIHR5cGU9cmFuZ2UgbWluPTAgbWF4PTEgc3RlcD0uMDUgdmFsdWU9Ljggb25pbnB1dD1zZXRWb2wodGhpcy52",
  "YWx1ZSk+PC9kaXY+CjwvZGl2PjwvZGl2PjwvZGl2Pgo8cCBjbGFzcz10aXA+Qmx1ZXRvb3RoOiBBdWRpbyBrb21tdCBhdXMgZGll",
  "c2VtIEJyb3dzZXItVGFiIOKAlCBMYXV0c3ByZWNoZXIgw7xiZXIgQmx1ZXRvb3RoIGFtIEhhbmR5L1BDIHZlcmJpbmRlbi4gRGVy",
  "IEVTUCBzZWxic3Qgc3RyZWFtdCBuaWNodCBwZXIgQTJEUCAoUzM6IGtlaW4gQ2xhc3NpYy1CVDsgRVNQMzI6IFdpRmkrQlQgdW5n",
  "ZWVpZ25ldCkuPC9wPgo8L3NlY3Rpb24+CjxuYXYgY2xhc3M9dGFicz4KPGJ1dHRvbiBjbGFzcz0idGFiIGFjdGl2ZSIgb25jbGlj",
  "az0ic2hvd1RhYigncmFkaW8nLHRoaXMpIj5SYWRpbzwvYnV0dG9uPgo8YnV0dG9uIGNsYXNzPXRhYiBvbmNsaWNrPSJzaG93VGFi",
  "KCdkaXNjb3ZlcicsdGhpcykiPkVudGRlY2tlbjwvYnV0dG9uPgo8YnV0dG9uIGNsYXNzPXRhYiBvbmNsaWNrPSJzaG93VGFiKCdw",
  "b2RjYXN0cycsdGhpcykiPlBvZGNhc3RzPC9idXR0b24+CjxidXR0b24gY2xhc3M9dGFiIG9uY2xpY2s9InNob3dUYWIoJ3N0YXR1",
  "cycsdGhpcykiPlN0YXR1czwvYnV0dG9uPgo8YSBjbGFzcz10YWIgaHJlZj0vb3RhPk9UQTwvYT4KPC9uYXY+CjxkaXYgaWQ9cGFu",
  "ZS1yYWRpbyBjbGFzcz0icGFuZSBhY3RpdmUiPgo8aDIgY2xhc3M9c2VjLWg+4piFIEZhdm9yaXRlbiA8c3BhbiBjbGFzcz1tdXRl",
  "ZCBpZD1mYXYtYmFkZ2U+PC9zcGFuPjwvaDI+CjxkaXYgaWQ9ZmF2LWxpc3QgY2xhc3M9bXV0ZWQ+TGFkZeKApjwvZGl2Pgo8aDIg",
  "Y2xhc3M9c2VjLWg+TWVpbmUgU2VuZGVyPC9oMj4KPGRpdiBpZD1zdGF0aW9uLWxpc3QgY2xhc3M9bXV0ZWQ+TGFkZeKApjwvZGl2",
  "Pgo8aDIgY2xhc3M9c2VjLWg+U2VuZGVyIGhpbnp1ZsO8Z2VuPC9oMj4KPGRpdiBjbGFzcz1mb3JtPgo8aW5wdXQgaWQ9YWRkLW5h",
  "bWUgdHlwZT10ZXh0IHBsYWNlaG9sZGVyPSJOYW1lIj4KPGlucHV0IGlkPWFkZC11cmwgdHlwZT11cmwgcGxhY2Vob2xkZXI9IlN0",
  "cmVhbS1VUkwgaHR0cHM6Ly/igKYiPgo8YnV0dG9uIGNsYXNzPWJ0biBvbmNsaWNrPWFkZFN0YXRpb24oKT5TcGVpY2hlcm48L2J1",
  "dHRvbj4KPC9kaXY+CjxwIGNsYXNzPW11dGVkIHN0eWxlPSJtYXJnaW4tdG9wOjhweCI+UXVlbGxlbjogcmFkaW8tYnJvd3NlciDC",
  "tyByYWRpby5kZSDCtyBzdHJlYW11cmwubGluazwvcD4KPC9kaXY+CjxkaXYgaWQ9cGFuZS1kaXNjb3ZlciBjbGFzcz1wYW5lPgo8",
  "aDIgY2xhc3M9c2VjLWg+UXVlbGxlPC9oMj4KPGRpdiBjbGFzcz1zcmMgaWQ9c3JjLWJhcj4KPGJ1dHRvbiBjbGFzcz1vbiBvbmNs",
  "aWNrPSJzZXRTcmMoJ2RlJyx0aGlzKSI+RGV1dHNjaGxhbmQ8L2J1dHRvbj4KPGJ1dHRvbiBvbmNsaWNrPSJzZXRTcmMoJ2RhY2gn",
  "LHRoaXMpIj5EQUNIPC9idXR0b24+CjxidXR0b24gb25jbGljaz0ic2V0U3JjKCd3b3JsZCcsdGhpcykiPldlbHQ8L2J1dHRvbj4K",
  "PGJ1dHRvbiBvbmNsaWNrPSJzZXRTcmMoJ2NvbW11bml0eScsdGhpcykiPkNvbW11bml0eTwvYnV0dG9uPgo8L2Rpdj4KPGgyIGNs",
  "YXNzPXNlYy1oPkxva2FsICYgw5ZmZmVudGxpY2gtUmVjaHRsaWNoPC9oMj4KPGRpdiBjbGFzcz1jaGlwcyBpZD1sb2NhbC1jaGlw",
  "cz48L2Rpdj4KPGgyIGNsYXNzPXNlYy1oPkdlbnJlczwvaDI+CjxkaXYgY2xhc3M9Y2hpcHMgaWQ9Z2VucmUtY2hpcHM+PC9kaXY+",
  "CjxkaXYgY2xhc3M9c2VhcmNoPgo8aW5wdXQgaWQ9ZGlzYy1xIHR5cGU9dGV4dCBwbGFjZWhvbGRlcj0iU2VuZGVyIG9kZXIgT3J0",
  "IHN1Y2hlbuKApiIgb25rZXlkb3duPSJpZihldmVudC5rZXk9PT0nRW50ZXInKWRvU2VhcmNoKCkiPgo8YnV0dG9uIGNsYXNzPWJ0",
  "biBvbmNsaWNrPWRvU2VhcmNoKCk+U3VjaGVuPC9idXR0b24+CjwvZGl2Pgo8cCBjbGFzcz1tdXRlZCBpZD1kaXNjLWluZm8+PC9w",
  "Pgo8ZGl2IGlkPWRpc2MtcmVzdWx0cz48L2Rpdj4KPC9kaXY+CjxkaXYgaWQ9cGFuZS1wb2RjYXN0cyBjbGFzcz1wYW5lPgo8aDIg",
  "Y2xhc3M9c2VjLWg+UG9kY2FzdHM8L2gyPgo8ZGl2IGlkPXBvZC1saXN0IGNsYXNzPW11dGVkPkxhZGXigKY8L2Rpdj4KPGRpdiBp",
  "ZD1lcC1wYW5lbCBzdHlsZT0iZGlzcGxheTpub25lO21hcmdpbi10b3A6MTRweCI+CjxoMiBjbGFzcz1zZWMtaCBpZD1lcC10aXRs",
  "ZT5FcGlzb2RlbjwvaDI+CjxkaXYgY2xhc3M9ZXAgaWQ9ZXAtbGlzdD48L2Rpdj4KPC9kaXY+CjxoMiBjbGFzcz1zZWMtaD5Qb2Rj",
  "YXN0IGhpbnp1ZsO8Z2VuPC9oMj4KPGRpdiBjbGFzcz1mb3JtPgo8aW5wdXQgaWQ9cG9kLW5hbWUgdHlwZT10ZXh0IHBsYWNlaG9s",
  "ZGVyPSJOYW1lIj4KPGlucHV0IGlkPXBvZC11cmwgdHlwZT11cmwgcGxhY2Vob2xkZXI9IlJTUy1GZWVkIFVSTCI+CjxidXR0b24g",
  "Y2xhc3M9YnRuIG9uY2xpY2s9YWRkUG9kY2FzdCgpPlNwZWljaGVybjwvYnV0dG9uPgo8L2Rpdj4KPC9kaXY+Cgo8IS0tV1JTUExJ",
  "VC0tPgp2YXIgc3RuTGlzdD1bXSxmYXZMaXN0PVtdLHBvZExpc3Q9W10sY3VyVXJsPScnLGN1ck5hbWU9JycsY3VyQXJ0PScnLGlz",
  "UGxheWluZz1mYWxzZTsKdmFyIGRpc2NSZXN1bHRzPVtdLGRpc2NMb2FkaW5nPWZhbHNlLGN1clBvZElkeD0tMSxjdXJFcGlzb2Rl",
  "cz1bXSxzcmNNb2RlPSdkZSc7CnZhciBSQj1bJ2h0dHBzOi8vZGUxLmFwaS5yYWRpby1icm93c2VyLmluZm8nLCdodHRwczovL25s",
  "MS5hcGkucmFkaW8tYnJvd3Nlci5pbmZvJywnaHR0cHM6Ly9hdDEuYXBpLnJhZGlvLWJyb3dzZXIuaW5mbyddOwp2YXIgYXVkaW89",
  "ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2F1ZGlvJyk7CnZhciBMT0NBTFM9W1snVG9wIERFJywnJywnJ10sWydCYXllcm4nLCdC",
  "YXllcm4nLCdCYXZhcmlhJ10sWydCZXJsaW4nLCdCZXJsaW4nLCcnXSxbJ0hhbWJ1cmcnLCdIYW1idXJnJywnJ10sClsnTlJXJywn",
  "Tm9yZHJoZWluLVdlc3RmYWxlbicsJ05vcnRoIFJoaW5lLVdlc3RwaGFsaWEnXSxbJ0JXJywnQmFkZW4tV8O8cnR0ZW1iZXJnJywn",
  "QmFkZW4tV3VlcnR0ZW1iZXJnJ10sClsnTmllZGVyc2FjaHNlbicsJ05pZWRlcnNhY2hzZW4nLCdMb3dlciBTYXhvbnknXSxbJ0hl",
  "c3NlbicsJ0hlc3NlbicsJ0hlc3NlJ10sWydTYWNoc2VuJywnU2FjaHNlbicsJ1NheG9ueSddLApbJ8OWUlInLCcnLCcnXV07CnZh",
  "ciBHRU5SRVM9Wydyb2NrJywncG9wJywnbWV0YWwnLCdqYXp6JywnYmx1ZXMnLCdjbGFzc2ljYWwnLCdlbGVjdHJvbmljJywnaG91",
  "c2UnLCd0ZWNobm8nLCdoaXBob3AnLCdpbmRpZScsJ25ld3MnLCdzY2hsYWdlcicsJ2RldXRzY2hyYXAnLCdsb3VuZ2UnLCdhbWJp",
  "ZW50J107CnZhciBlc2M9ZnVuY3Rpb24ocyl7cmV0dXJuIFN0cmluZyhzKS5yZXBsYWNlKC8mL2csJyZhbXA7JykucmVwbGFjZSgv",
  "PC9nLCcmbHQ7JykucmVwbGFjZSgvPi9nLCcmZ3Q7JykucmVwbGFjZSgvIi9nLCcmcXVvdDsnKTt9CnZhciBlc2o9ZnVuY3Rpb24o",
  "cyl7cmV0dXJuIFN0cmluZyhzKS5yZXBsYWNlKC9cXC9nLCdcXFxcJykucmVwbGFjZSgvJy9nLCJcXCciKS5yZXBsYWNlKC9cbi9n",
  "LCdcXG4nKS5yZXBsYWNlKC9cci9nLCcnKTt9CnZhciBzaG93VGFiPWZ1bmN0aW9uKGlkLGJ0bil7CiAgZG9jdW1lbnQucXVlcnlT",
  "ZWxlY3RvckFsbCgnLnBhbmUnKS5mb3JFYWNoKGZ1bmN0aW9uKHApe3AuY2xhc3NMaXN0LnJlbW92ZSgnYWN0aXZlJyk7fSk7CiAg",
  "ZG9jdW1lbnQucXVlcnlTZWxlY3RvckFsbCgnbmF2LnRhYnMgYnV0dG9uJykuZm9yRWFjaChmdW5jdGlvbihiKXtiLmNsYXNzTGlz",
  "dC5yZW1vdmUoJ2FjdGl2ZScpO30pOwogIGRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdwYW5lLScraWQpLmNsYXNzTGlzdC5hZGQo",
  "J2FjdGl2ZScpOwogIGlmKGJ0bilidG4uY2xhc3NMaXN0LmFkZCgnYWN0aXZlJyk7Cn0KdmFyIHNldFNyYz1mdW5jdGlvbihtLGJ0",
  "bil7CiAgc3JjTW9kZT1tOwogIGRvY3VtZW50LnF1ZXJ5U2VsZWN0b3JBbGwoJyNzcmMtYmFyIGJ1dHRvbicpLmZvckVhY2goZnVu",
  "Y3Rpb24oYil7Yi5jbGFzc0xpc3QudG9nZ2xlKCdvbicsYj09PWJ0bik7fSk7Cn0KdmFyIGNvdW50cnlQYXJhbXM9ZnVuY3Rpb24o",
  "KXsKICBpZihzcmNNb2RlPT09J2RlJylyZXR1cm4gJyZjb3VudHJ5Y29kZT1ERSc7CiAgaWYoc3JjTW9kZT09PSdjb21tdW5pdHkn",
  "KXJldHVybiAnJnRhZz1jb21tdW5pdHkmY291bnRyeWNvZGU9REUnOwogIHJldHVybiAnJzsKfQp2YXIgcmJGZXRjaD1hc3luYyBm",
  "dW5jdGlvbihwYXRoKXsKICB2YXIgbGFzdEVycj1udWxsOwogIGZvcih2YXIgaT0wO2k8UkIubGVuZ3RoO2krKyl7CiAgICB0cnl7",
  "CiAgICAgIHZhciByPWF3YWl0IGZldGNoKFJCW2ldK3BhdGgse2hlYWRlcnM6eydVc2VyLUFnZW50JzonRVNQMzItV2ViUmFkaW8v",
  "Mi40JywnQWNjZXB0JzonYXBwbGljYXRpb24vanNvbid9fSk7CiAgICAgIGlmKCFyLm9rKXRocm93IG5ldyBFcnJvcignSFRUUCAn",
  "K3Iuc3RhdHVzKTsKICAgICAgcmV0dXJuIGF3YWl0IHIuanNvbigpOwogICAgfWNhdGNoKGUpe2xhc3RFcnI9ZTt9CiAgfQogIHRo",
  "cm93IGxhc3RFcnJ8fG5ldyBFcnJvcigncmFkaW8tYnJvd3NlciBvZmZsaW5lJyk7Cn0KdmFyIHJiTWVyZ2VDb3VudHJpZXM9YXN5",
  "bmMgZnVuY3Rpb24oYmFzZVBhdGhXaXRob3V0Q291bnRyeSwgY29kZXMpewogIHZhciBhbGw9W10sIHNlZW49e307CiAgZm9yKHZh",
  "ciBpPTA7aTxjb2Rlcy5sZW5ndGg7aSsrKXsKICAgIHZhciBwYXJ0PWF3YWl0IHJiRmV0Y2goYmFzZVBhdGhXaXRob3V0Q291bnRy",
  "eSsnJmNvdW50cnljb2RlPScrY29kZXNbaV0pOwogICAgKHBhcnR8fFtdKS5mb3JFYWNoKGZ1bmN0aW9uKHMpe3ZhciB1PXMudXJs",
  "X3Jlc29sdmVkfHxzLnVybDsgaWYodSYmIXNlZW5bdV0pe3NlZW5bdV09MTthbGwucHVzaChzKTt9fSk7CiAgfQogIHJldHVybiBh",
  "bGw7Cn0KdmFyIGJ1aWxkTG9jYWxDaGlwcz1mdW5jdGlvbigpewogIHZhciBlbD1kb2N1bWVudC5nZXRFbGVtZW50QnlJZCgnbG9j",
  "YWwtY2hpcHMnKTsgdmFyIGg9Jyc7CiAgTE9DQUxTLmZvckVhY2goZnVuY3Rpb24oTCxpKXsgaCs9JzxidXR0b24gY2xhc3M9Y2hp",
  "cCBvbmNsaWNrPSJicm93c2VMb2NhbCgnK2krJyx0aGlzKSI+Jytlc2MoTFswXSkrJzwvYnV0dG9uPic7IH0pOwogIGVsLmlubmVy",
  "SFRNTD1oOwp9CnZhciBidWlsZEdlbnJlQ2hpcHM9ZnVuY3Rpb24oKXsKICB2YXIgZWw9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQo",
  "J2dlbnJlLWNoaXBzJyk7IHZhciBoPScnOwogIEdFTlJFUy5mb3JFYWNoKGZ1bmN0aW9uKGcpeyBoKz0nPGJ1dHRvbiBjbGFzcz1j",
  "aGlwIG9uY2xpY2s9ImJyb3dzZUdlbnJlKFwnJytnKydcJyx0aGlzKSI+Jytlc2MoZykrJzwvYnV0dG9uPic7IH0pOwogIGVsLmlu",
  "bmVySFRNTD1oOwp9CnZhciBtYXJrQ2hpcD1mdW5jdGlvbihidG4pewogIGRvY3VtZW50LnF1ZXJ5U2VsZWN0b3JBbGwoJy5jaGlw",
  "JykuZm9yRWFjaChmdW5jdGlvbihjKXtjLmNsYXNzTGlzdC5yZW1vdmUoJ29uJyk7fSk7CiAgaWYoYnRuKWJ0bi5jbGFzc0xpc3Qu",
  "YWRkKCdvbicpOwp9CnZhciBicm93c2VMb2NhbD1hc3luYyBmdW5jdGlvbihpLGJ0bil7CiAgaWYoZGlzY0xvYWRpbmcpcmV0dXJu",
  "OyBkaXNjTG9hZGluZz10cnVlOyBtYXJrQ2hpcChidG4pOwogIHZhciBMPUxPQ0FMU1tpXTsgdmFyIGluZm89ZG9jdW1lbnQuZ2V0",
  "RWxlbWVudEJ5SWQoJ2Rpc2MtaW5mbycpOwogIGluZm8udGV4dENvbnRlbnQ9J0xhZGUgbG9rYWxlIFNlbmRlcuKApic7IGRvY3Vt",
  "ZW50LmdldEVsZW1lbnRCeUlkKCdkaXNjLXJlc3VsdHMnKS5pbm5lckhUTUw9Jyc7CiAgdHJ5ewogICAgdmFyIHBhdGg7CiAgICBp",
  "ZihMWzBdPT09J1RvcCBERScpIHBhdGg9Jy9qc29uL3N0YXRpb25zL3NlYXJjaD9jb3VudHJ5Y29kZT1ERSZsaW1pdD00MCZoaWRl",
  "YnJva2VuPXRydWUmb3JkZXI9Y2xpY2tjb3VudCZyZXZlcnNlPXRydWUmY29kZWM9TVAzJzsKICAgIGVsc2UgaWYoTFswXT09PSfD",
  "llJSJykgcGF0aD0nL2pzb24vc3RhdGlvbnMvc2VhcmNoP2NvdW50cnljb2RlPURFJnRhZz1wdWJsaWMlMjByYWRpbyZsaW1pdD00",
  "MCZoaWRlYnJva2VuPXRydWUmb3JkZXI9Y2xpY2tjb3VudCZyZXZlcnNlPXRydWUnOwogICAgZWxzZSBwYXRoPScvanNvbi9zdGF0",
  "aW9ucy9zZWFyY2g/Y291bnRyeWNvZGU9REUmc3RhdGU9JytlbmNvZGVVUklDb21wb25lbnQoTFsxXSkrJyZsaW1pdD00MCZoaWRl",
  "YnJva2VuPXRydWUmb3JkZXI9Y2xpY2tjb3VudCZyZXZlcnNlPXRydWUnOwogICAgZGlzY1Jlc3VsdHM9YXdhaXQgcmJGZXRjaChw",
  "YXRoKTsKICAgIGlmKCghZGlzY1Jlc3VsdHN8fCFkaXNjUmVzdWx0cy5sZW5ndGgpJiZMWzJdKXsKICAgICAgZGlzY1Jlc3VsdHM9",
  "YXdhaXQgcmJGZXRjaCgnL2pzb24vc3RhdGlvbnMvc2VhcmNoP2NvdW50cnljb2RlPURFJnN0YXRlPScrZW5jb2RlVVJJQ29tcG9u",
  "ZW50KExbMl0pKycmbGltaXQ9NDAmaGlkZWJyb2tlbj10cnVlJm9yZGVyPWNsaWNrY291bnQmcmV2ZXJzZT10cnVlJyk7CiAgICB9",
  "CiAgICByZW5kZXJEaXNjb3ZlcihMWzBdKTsKICB9Y2F0Y2goZSl7aW5mby50ZXh0Q29udGVudD0nRmVobGVyOiAnK2UubWVzc2Fn",
  "ZTt9CiAgZGlzY0xvYWRpbmc9ZmFsc2U7Cn0KdmFyIGJyb3dzZUdlbnJlPWFzeW5jIGZ1bmN0aW9uKHRhZyxidG4pewogIGlmKGRp",
  "c2NMb2FkaW5nKXJldHVybjsgZGlzY0xvYWRpbmc9dHJ1ZTsgbWFya0NoaXAoYnRuKTsKICB2YXIgaW5mbz1kb2N1bWVudC5nZXRF",
  "bGVtZW50QnlJZCgnZGlzYy1pbmZvJyk7CiAgaW5mby50ZXh0Q29udGVudD0nTGFkZSBHZW5yZeKApic7IGRvY3VtZW50LmdldEVs",
  "ZW1lbnRCeUlkKCdkaXNjLXJlc3VsdHMnKS5pbm5lckhUTUw9Jyc7CiAgdHJ5ewogICAgaWYoc3JjTW9kZT09PSdkYWNoJyl7CiAg",
  "ICAgIGRpc2NSZXN1bHRzPWF3YWl0IHJiTWVyZ2VDb3VudHJpZXMoJy9qc29uL3N0YXRpb25zL3NlYXJjaD90YWc9JytlbmNvZGVV",
  "UklDb21wb25lbnQodGFnKSsnJmxpbWl0PTIwJmhpZGVicm9rZW49dHJ1ZSZvcmRlcj12b3RlcyZyZXZlcnNlPXRydWUmY29kZWM9",
  "TVAzJyxbJ0RFJywnQVQnLCdDSCddKTsKICAgIH1lbHNlIGlmKHNyY01vZGU9PT0nd29ybGQnKXsKICAgICAgZGlzY1Jlc3VsdHM9",
  "YXdhaXQgcmJGZXRjaCgnL2pzb24vc3RhdGlvbnMvYnl0YWcvJytlbmNvZGVVUklDb21wb25lbnQodGFnKSsnP2xpbWl0PTQwJmhp",
  "ZGVicm9rZW49dHJ1ZSZvcmRlcj12b3RlcyZyZXZlcnNlPXRydWUmY29kZWM9TVAzJyk7CiAgICB9ZWxzZXsKICAgICAgZGlzY1Jl",
  "c3VsdHM9YXdhaXQgcmJGZXRjaCgnL2pzb24vc3RhdGlvbnMvc2VhcmNoP3RhZz0nK2VuY29kZVVSSUNvbXBvbmVudCh0YWcpK2Nv",
  "dW50cnlQYXJhbXMoKSsnJmxpbWl0PTQwJmhpZGVicm9rZW49dHJ1ZSZvcmRlcj12b3RlcyZyZXZlcnNlPXRydWUmY29kZWM9TVAz",
  "Jyk7CiAgICB9CiAgICByZW5kZXJEaXNjb3Zlcih0YWcpOwogIH1jYXRjaChlKXtpbmZvLnRleHRDb250ZW50PSdGZWhsZXI6ICcr",
  "ZS5tZXNzYWdlO30KICBkaXNjTG9hZGluZz1mYWxzZTsKfQp2YXIgZG9TZWFyY2g9YXN5bmMgZnVuY3Rpb24oKXsKICBpZihkaXNj",
  "TG9hZGluZylyZXR1cm47CiAgdmFyIHE9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2Rpc2MtcScpLnZhbHVlLnRyaW0oKTsgaWYo",
  "IXEpe2FsZXJ0KCdTdWNoYmVncmlmZiBlaW5nZWJlbicpO3JldHVybjt9CiAgZGlzY0xvYWRpbmc9dHJ1ZTsgbWFya0NoaXAobnVs",
  "bCk7CiAgdmFyIGluZm89ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2Rpc2MtaW5mbycpOwogIGluZm8udGV4dENvbnRlbnQ9J1N1",
  "Y2hl4oCmJzsgZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2Rpc2MtcmVzdWx0cycpLmlubmVySFRNTD0nJzsKICB0cnl7CiAgICBp",
  "ZihzcmNNb2RlPT09J2RhY2gnKXsKICAgICAgZGlzY1Jlc3VsdHM9YXdhaXQgcmJNZXJnZUNvdW50cmllcygnL2pzb24vc3RhdGlv",
  "bnMvc2VhcmNoP25hbWU9JytlbmNvZGVVUklDb21wb25lbnQocSkrJyZsaW1pdD0xNSZoaWRlYnJva2VuPXRydWUmb3JkZXI9Y2xp",
  "Y2tjb3VudCZyZXZlcnNlPXRydWUnLFsnREUnLCdBVCcsJ0NIJ10pOwogICAgfWVsc2UgaWYoc3JjTW9kZT09PSd3b3JsZCcpewog",
  "ICAgICBkaXNjUmVzdWx0cz1hd2FpdCByYkZldGNoKCcvanNvbi9zdGF0aW9ucy9zZWFyY2g/bmFtZT0nK2VuY29kZVVSSUNvbXBv",
  "bmVudChxKSsnJmxpbWl0PTQwJmhpZGVicm9rZW49dHJ1ZSZvcmRlcj1jbGlja2NvdW50JnJldmVyc2U9dHJ1ZScpOwogICAgfWVs",
  "c2V7CiAgICAgIGRpc2NSZXN1bHRzPWF3YWl0IHJiRmV0Y2goJy9qc29uL3N0YXRpb25zL3NlYXJjaD9uYW1lPScrZW5jb2RlVVJJ",
  "Q29tcG9uZW50KHEpK2NvdW50cnlQYXJhbXMoKSsnJmxpbWl0PTQwJmhpZGVicm9rZW49dHJ1ZSZvcmRlcj1jbGlja2NvdW50JnJl",
  "dmVyc2U9dHJ1ZScpOwogICAgfQogICAgcmVuZGVyRGlzY292ZXIocSk7CiAgfWNhdGNoKGUpe2luZm8udGV4dENvbnRlbnQ9J0Zl",
  "aGxlcjogJytlLm1lc3NhZ2U7fQogIGRpc2NMb2FkaW5nPWZhbHNlOwp9CnZhciByZW5kZXJEaXNjb3Zlcj1mdW5jdGlvbihsYWJl",
  "bCl7CiAgdmFyIGVsPWRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdkaXNjLXJlc3VsdHMnKTsgdmFyIGluZm89ZG9jdW1lbnQuZ2V0",
  "RWxlbWVudEJ5SWQoJ2Rpc2MtaW5mbycpOwogIGlmKCFkaXNjUmVzdWx0c3x8IWRpc2NSZXN1bHRzLmxlbmd0aCl7aW5mby50ZXh0",
  "Q29udGVudD0nS2VpbmUgVHJlZmZlciBmw7xyICcrbGFiZWw7ZWwuaW5uZXJIVE1MPScnO3JldHVybjt9CiAgaW5mby50ZXh0Q29u",
  "dGVudD1kaXNjUmVzdWx0cy5sZW5ndGgrJyBTZW5kZXIgwrcgcmFkaW8tYnJvd3Nlci5pbmZvIMK3IFF1ZWxsZSAnK3NyY01vZGUu",
  "dG9VcHBlckNhc2UoKTsKICB2YXIgaD0nJzsKICBkaXNjUmVzdWx0cy5mb3JFYWNoKGZ1bmN0aW9uKHMpewogICAgdmFyIHVybD1z",
  "LnVybF9yZXNvbHZlZHx8cy51cmw7IGlmKCF1cmwpcmV0dXJuOwogICAgdmFyIGZhdj1pc0Zhdih1cmwpOyB2YXIgcGxheWluZz0o",
  "dXJsPT09Y3VyVXJsJiZpc1BsYXlpbmcpOwogICAgdmFyIG1ldGE9W3MuY291bnRyeSxzLnN0YXRlLHMuY29kZWMscy5iaXRyYXRl",
  "PyAocy5iaXRyYXRlKydrYnBzJyk6JyddLmZpbHRlcihCb29sZWFuKS5qb2luKCcgwrcgJyk7CiAgICB2YXIgYXJ0PXMuZmF2aWNv",
  "bnx8Jyc7CiAgICBoKz0nPGRpdiBjbGFzcz0icm93JysodXJsPT09Y3VyVXJsPycgb24nOicnKSsnIj4nCiAgICAgICsoYXJ0Pyc8",
  "aW1nIGNsYXNzPXRodW1iIHNyYz0iJytlc2MoYXJ0KSsnIiBvbmVycm9yPSJ0aGlzLnN0eWxlLmRpc3BsYXk9XCdub25lXCciIGFs",
  "dD0iIj4nOicnKQogICAgICArJzxkaXYgY2xhc3M9aW5mbz48ZGl2IGNsYXNzPW5hbWU+Jytlc2Mocy5uYW1lKSsnPC9kaXY+PGRp",
  "diBjbGFzcz1tZXRhPicrZXNjKG1ldGEpKyc8L2Rpdj48L2Rpdj4nCiAgICAgICsnPGJ1dHRvbiBjbGFzcz0iYnRuIHNtJysocGxh",
  "eWluZz8nIHBsYXlvbic6JycpKyciIG9uY2xpY2s9InBsYXlVcmwoXCcnK2Vzaih1cmwpKydcJyxcJycrZXNqKHMubmFtZSkrJ1wn",
  "LFwnJytlc2ooYXJ0KSsnXCcpIj4nKyhwbGF5aW5nPyfinZrinZonOifilrYnKSsnPC9idXR0b24+JwogICAgICArJzxidXR0b24g",
  "Y2xhc3M9ImZhdicrKGZhdj8nIG9uJzonJykrJyIgb25jbGljaz0idG9nZ2xlRmF2KFwnJytlc2oocy5uYW1lKSsnXCcsXCcnK2Vz",
  "aih1cmwpKydcJykiPicrKGZhdj8n4piFJzon4piGJykrJzwvYnV0dG9uPjwvZGl2Pic7CiAgfSk7CiAgZWwuaW5uZXJIVE1MPWg7",
  "Cn0KdmFyIHBsYXlVcmw9ZnVuY3Rpb24odXJsLG5hbWUsYXJ0KXsKICBpZih1cmw9PT1jdXJVcmwmJmlzUGxheWluZyl7YXVkaW8u",
  "cGF1c2UoKTtpc1BsYXlpbmc9ZmFsc2U7dXBkYXRlTlAoKTtyZW5kZXJBbGwoKTtyZXR1cm47fQogIGN1clVybD11cmw7Y3VyTmFt",
  "ZT1uYW1lO2N1ckFydD1hcnR8fCcnOwogIGF1ZGlvLnNyYz11cmw7IGF1ZGlvLnZvbHVtZT1wYXJzZUZsb2F0KGRvY3VtZW50Lmdl",
  "dEVsZW1lbnRCeUlkKCd2b2wnKS52YWx1ZSk7CiAgYXVkaW8ucGxheSgpLmNhdGNoKGZ1bmN0aW9uKGUpe2NvbnNvbGUud2Fybihl",
  "KTt9KTsKICBpc1BsYXlpbmc9dHJ1ZTsgZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ25wLXN1YicpLnRleHRDb250ZW50PSdWZXJi",
  "aW5kZeKApic7CiAgdXBkYXRlTlAoKTsgcmVuZGVyQWxsKCk7IHNldE1lZGlhU2Vzc2lvbigpOwogIGZldGNoKCcvYXBpL2xhc3Qn",
  "LHttZXRob2Q6J1BPU1QnLGhlYWRlcnM6eydDb250ZW50LVR5cGUnOidhcHBsaWNhdGlvbi9qc29uJ30sYm9keTpKU09OLnN0cmlu",
  "Z2lmeSh7bmFtZTpuYW1lLHVybDp1cmx9KX0pOwogIGZldGNoKCcvYXBpL25vd3BsYXlpbmcnLHttZXRob2Q6J1BPU1QnLGhlYWRl",
  "cnM6eydDb250ZW50LVR5cGUnOidhcHBsaWNhdGlvbi9qc29uJ30sYm9keTpKU09OLnN0cmluZ2lmeSh7bmFtZTpuYW1lLHVybDp1",
  "cmwsdGl0bGU6Jyd9KX0pOwp9CnZhciB0b2dnbGVQbGF5PWZ1bmN0aW9uKCl7CiAgaWYoIWN1clVybCl7CiAgICBmZXRjaCgnL2Fw",
  "aS9sYXN0JykudGhlbihmdW5jdGlvbihyKXtyZXR1cm4gci5qc29uKCk7fSkudGhlbihmdW5jdGlvbihkKXtpZihkLnVybClwbGF5",
  "VXJsKGQudXJsLGQubmFtZXx8J1NlbmRlcicsJycpO30pOwogICAgcmV0dXJuOwogIH0KICBpZihpc1BsYXlpbmcpe2F1ZGlvLnBh",
  "dXNlKCk7aXNQbGF5aW5nPWZhbHNlO30gZWxzZSB7YXVkaW8ucGxheSgpLmNhdGNoKGZ1bmN0aW9uKCl7fSk7aXNQbGF5aW5nPXRy",
  "dWU7fQogIHVwZGF0ZU5QKCk7cmVuZGVyQWxsKCk7Cn0KdmFyIHN0b3BQbGF5PWZ1bmN0aW9uKCl7CiAgYXVkaW8ucGF1c2UoKTth",
  "dWRpby5yZW1vdmVBdHRyaWJ1dGUoJ3NyYycpO2F1ZGlvLmxvYWQoKTsKICBpc1BsYXlpbmc9ZmFsc2U7Y3VyVXJsPScnO2N1ck5h",
  "bWU9Jyc7Y3VyQXJ0PScnOwogIHVwZGF0ZU5QKCk7cmVuZGVyQWxsKCk7CiAgZmV0Y2goJy9hcGkvbm93cGxheWluZycse21ldGhv",
  "ZDonUE9TVCcsaGVhZGVyczp7J0NvbnRlbnQtVHlwZSc6J2FwcGxpY2F0aW9uL2pzb24nfSxib2R5OkpTT04uc3RyaW5naWZ5KHtu",
  "YW1lOicnLHVybDonJyx0aXRsZTonJ30pfSk7Cn0KdmFyIHNldFZvbD1mdW5jdGlvbih2KXthdWRpby52b2x1bWU9cGFyc2VGbG9h",
  "dCh2KTt0cnl7bG9jYWxTdG9yYWdlLnNldEl0ZW0oJ3J2Jyx2KTt9Y2F0Y2goZSl7fX0KdmFyIHNldE1lZGlhU2Vzc2lvbj1mdW5j",
  "dGlvbigpewogIGlmKCEoJ21lZGlhU2Vzc2lvbicgaW4gbmF2aWdhdG9yKSlyZXR1cm47CiAgdHJ5ewogICAgbmF2aWdhdG9yLm1l",
  "ZGlhU2Vzc2lvbi5tZXRhZGF0YT1uZXcgTWVkaWFNZXRhZGF0YSh7dGl0bGU6Y3VyTmFtZXx8J1dlYi1SYWRpbycsYXJ0aXN0OidF",
  "U1AgV2ViLVJhZGlvJyxhcnR3b3JrOmN1ckFydD9be3NyYzpjdXJBcnQsc2l6ZXM6Jzk2eDk2J31dOltdfSk7CiAgICBuYXZpZ2F0",
  "b3IubWVkaWFTZXNzaW9uLnNldEFjdGlvbkhhbmRsZXIoJ3BsYXknLGZ1bmN0aW9uKCl7YXVkaW8ucGxheSgpO30pOwogICAgbmF2",
  "aWdhdG9yLm1lZGlhU2Vzc2lvbi5zZXRBY3Rpb25IYW5kbGVyKCdwYXVzZScsZnVuY3Rpb24oKXthdWRpby5wYXVzZSgpO30pOwog",
  "ICAgbmF2aWdhdG9yLm1lZGlhU2Vzc2lvbi5zZXRBY3Rpb25IYW5kbGVyKCdzdG9wJyxzdG9wUGxheSk7CiAgfWNhdGNoKGUpe30K",
  "fQp2YXIgdXBkYXRlTlA9ZnVuY3Rpb24oKXsKICB2YXIgbm93PWRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdub3cnKTsgdmFyIG5h",
  "bWVFbD1kb2N1bWVudC5nZXRFbGVtZW50QnlJZCgnbnAtbmFtZScpOwogIHZhciBzdWI9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQo",
  "J25wLXN1YicpOyB2YXIgaW1nPWRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdhcnRpbWcnKTsKICB2YXIgZXE9ZG9jdW1lbnQuZ2V0",
  "RWxlbWVudEJ5SWQoJ2VxJyk7IHZhciBidG49ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2J0bi1wbGF5Jyk7CiAgaWYoY3VyVXJs",
  "JiZpc1BsYXlpbmcpewogICAgbmFtZUVsLnRleHRDb250ZW50PWN1ck5hbWU7IGRvY3VtZW50LnRpdGxlPSfilrYgJytjdXJOYW1l",
  "OyBub3cuY2xhc3NMaXN0LmFkZCgnbGl2ZScpOwogICAgc3ViLmlubmVySFRNTD0nPHNwYW4gc3R5bGU9ImNvbG9yOnZhcigtLW1p",
  "bnQpIj7il48gTGl2ZTwvc3Bhbj4nOyBidG4udGV4dENvbnRlbnQ9J+KdmuKdmic7CiAgICBpZihjdXJBcnQpe2ltZy5zcmM9Y3Vy",
  "QXJ0O2ltZy5zdHlsZS5kaXNwbGF5PSdibG9jayc7ZXEuc3R5bGUuZGlzcGxheT0nbm9uZSc7fQogICAgZWxzZXtpbWcuc3R5bGUu",
  "ZGlzcGxheT0nbm9uZSc7ZXEuc3R5bGUuZGlzcGxheT0nZmxleCc7fQogIH1lbHNlIGlmKGN1clVybCl7CiAgICBuYW1lRWwudGV4",
  "dENvbnRlbnQ9Y3VyTmFtZTsgZG9jdW1lbnQudGl0bGU9J+KdmiAnK2N1ck5hbWU7IG5vdy5jbGFzc0xpc3QucmVtb3ZlKCdsaXZl",
  "Jyk7CiAgICBzdWIudGV4dENvbnRlbnQ9J1BhdXNpZXJ0JzsgYnRuLnRleHRDb250ZW50PSfilrYnOyBlcS5zdHlsZS5kaXNwbGF5",
  "PSdub25lJzsKICB9ZWxzZXsKICAgIG5hbWVFbC50ZXh0Q29udGVudD0nQmVyZWl0JzsgZG9jdW1lbnQudGl0bGU9J1dlYi1SYWRp",
  "byc7IG5vdy5jbGFzc0xpc3QucmVtb3ZlKCdsaXZlJyk7CiAgICBidG4udGV4dENvbnRlbnQ9J+KWtic7IGltZy5zdHlsZS5kaXNw",
  "bGF5PSdub25lJzsgZXEuc3R5bGUuZGlzcGxheT0nZmxleCc7CiAgICBmZXRjaCgnL2FwaS9sYXN0JykudGhlbihmdW5jdGlvbihy",
  "KXtyZXR1cm4gci5qc29uKCk7fSkudGhlbihmdW5jdGlvbihkKXsKICAgICAgaWYoZC5uYW1lJiZkLnVybCkgc3ViLmlubmVySFRN",
  "TD0nPGEgaHJlZj0jIG9uY2xpY2s9InBsYXlVcmwoXCcnK2VzaihkLnVybCkrJ1wnLFwnJytlc2ooZC5uYW1lKSsnXCcsXCdcJyk7",
  "cmV0dXJuIGZhbHNlIj7ilrYgJytlc2MoZC5uYW1lKSsnIGZvcnRzZXR6ZW48L2E+JzsKICAgICAgZWxzZSBzdWIudGV4dENvbnRl",
  "bnQ9J1fDpGhsZSBlaW5lbiBTZW5kZXInOwogICAgfSkuY2F0Y2goZnVuY3Rpb24oKXtzdWIudGV4dENvbnRlbnQ9J1fDpGhsZSBl",
  "aW5lbiBTZW5kZXInO30pOwogIH0KfQp2YXIgaXNGYXY9ZnVuY3Rpb24odXJsKXtyZXR1cm4gZmF2TGlzdC5zb21lKGZ1bmN0aW9u",
  "KGYpe3JldHVybiBmLnVybD09PXVybDt9KTt9CnZhciB0b2dnbGVGYXY9ZnVuY3Rpb24obmFtZSx1cmwpewogIHZhciBpZHg9ZmF2",
  "TGlzdC5maW5kSW5kZXgoZnVuY3Rpb24oZil7cmV0dXJuIGYudXJsPT09dXJsO30pOwogIGlmKGlkeD49MCkgZmV0Y2goJy9hcGkv",
  "ZmF2b3JpdGVzLXJlbW92ZScse21ldGhvZDonUE9TVCcsaGVhZGVyczp7J0NvbnRlbnQtVHlwZSc6J2FwcGxpY2F0aW9uL2pzb24n",
  "fSxib2R5OkpTT04uc3RyaW5naWZ5KHtpbmRleDppZHh9KX0pLnRoZW4obG9hZEZhdnMpOwogIGVsc2UgZmV0Y2goJy9hcGkvZmF2",
  "b3JpdGVzLWFkZCcse21ldGhvZDonUE9TVCcsaGVhZGVyczp7J0NvbnRlbnQtVHlwZSc6J2FwcGxpY2F0aW9uL2pzb24nfSxib2R5",
  "OkpTT04uc3RyaW5naWZ5KHtuYW1lOm5hbWUsdXJsOnVybH0pfSkudGhlbihsb2FkRmF2cyk7Cn0KdmFyIGxvYWRGYXZzPWZ1bmN0",
  "aW9uKCl7cmV0dXJuIGZldGNoKCcvYXBpL2Zhdm9yaXRlcycpLnRoZW4oZnVuY3Rpb24ocil7cmV0dXJuIHIuanNvbigpO30pLnRo",
  "ZW4oZnVuY3Rpb24oZCl7ZmF2TGlzdD1kO3JlbmRlckFsbCgpO30pO30KdmFyIGxvYWRTdGF0aW9ucz1mdW5jdGlvbigpe3JldHVy",
  "biBmZXRjaCgnL2FwaS9zdGF0aW9ucycpLnRoZW4oZnVuY3Rpb24ocil7cmV0dXJuIHIuanNvbigpO30pLnRoZW4oZnVuY3Rpb24o",
  "ZCl7c3RuTGlzdD1kO3JlbmRlclN0YXRpb25zKCk7fSk7fQp2YXIgbG9hZFBvZGNhc3RzPWZ1bmN0aW9uKCl7cmV0dXJuIGZldGNo",
  "KCcvYXBpL3BvZGNhc3RzJykudGhlbihmdW5jdGlvbihyKXtyZXR1cm4gci5qc29uKCk7fSkudGhlbihmdW5jdGlvbihkKXtwb2RM",
  "aXN0PWQ7cmVuZGVyUG9kY2FzdHMoKTt9KTt9CnZhciByZW5kZXJGYXZzPWZ1bmN0aW9uKCl7CiAgdmFyIGVsPWRvY3VtZW50Lmdl",
  "dEVsZW1lbnRCeUlkKCdmYXYtbGlzdCcpOyB2YXIgYmFkZ2U9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2Zhdi1iYWRnZScpOwog",
  "IGlmKGJhZGdlKWJhZGdlLnRleHRDb250ZW50PWZhdkxpc3QubGVuZ3RoPycoJytmYXZMaXN0Lmxlbmd0aCsnKSc6Jyc7CiAgaWYo",
  "IWZhdkxpc3QubGVuZ3RoKXtlbC5pbm5lckhUTUw9JzxwIGNsYXNzPW11dGVkPk5vY2gga2VpbmUgRmF2b3JpdGVuIOKAlCBpbSBF",
  "bnRkZWNrZW4tVGFiIOKYhSB0aXBwZW4uPC9wPic7cmV0dXJuO30KICB2YXIgaD0nJzsKICBmYXZMaXN0LmZvckVhY2goZnVuY3Rp",
  "b24oZil7CiAgICB2YXIgcGxheWluZz0oZi51cmw9PT1jdXJVcmwmJmlzUGxheWluZyk7CiAgICBoKz0nPGRpdiBjbGFzcz0icm93",
  "JysoZi51cmw9PT1jdXJVcmw/JyBvbic6JycpKyciPjxkaXYgY2xhc3M9aW5mbz48ZGl2IGNsYXNzPW5hbWU+Jytlc2MoZi5uYW1l",
  "KSsnPC9kaXY+PC9kaXY+JwogICAgICArJzxidXR0b24gY2xhc3M9ImJ0biBzbScrKHBsYXlpbmc/JyBwbGF5b24nOicnKSsnIiBv",
  "bmNsaWNrPSJwbGF5VXJsKFwnJytlc2ooZi51cmwpKydcJyxcJycrZXNqKGYubmFtZSkrJ1wnLFwnXCcpIj4nKyhwbGF5aW5nPyfi",
  "nZrinZonOifilrYnKSsnPC9idXR0b24+JwogICAgICArJzxidXR0b24gY2xhc3M9ImZhdiBvbiIgb25jbGljaz0idG9nZ2xlRmF2",
  "KFwnJytlc2ooZi5uYW1lKSsnXCcsXCcnK2VzaihmLnVybCkrJ1wnKSI+4piFPC9idXR0b24+PC9kaXY+JzsKICB9KTsKICBlbC5p",
  "bm5lckhUTUw9aDsKfQp2YXIgcmVuZGVyU3RhdGlvbnM9ZnVuY3Rpb24oKXsKICB2YXIgZWw9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5",
  "SWQoJ3N0YXRpb24tbGlzdCcpOwogIGlmKCFzdG5MaXN0Lmxlbmd0aCl7ZWwuaW5uZXJIVE1MPSc8cCBjbGFzcz1tdXRlZD5LZWlu",
  "ZSBTZW5kZXIuPC9wPic7cmV0dXJuO30KICB2YXIgaD0nJzsKICBzdG5MaXN0LmZvckVhY2goZnVuY3Rpb24ocyxpKXsKICAgIHZh",
  "ciBwbGF5aW5nPShzLnVybD09PWN1clVybCYmaXNQbGF5aW5nKTsgdmFyIGZhdj1pc0ZhdihzLnVybCk7CiAgICBoKz0nPGRpdiBj",
  "bGFzcz0icm93Jysocy51cmw9PT1jdXJVcmw/JyBvbic6JycpKyciPjxkaXYgY2xhc3M9aW5mbz48ZGl2IGNsYXNzPW5hbWU+Jytl",
  "c2Mocy5uYW1lKSsnPC9kaXY+JwogICAgICArJzxkaXYgY2xhc3M9bWV0YT4nK2VzYygocy51cmx8fCcnKS5yZXBsYWNlKC9eaHR0",
  "cHM/OlwvXC8vLCcnKS5zdWJzdHJpbmcoMCw1MCkpKyc8L2Rpdj48L2Rpdj4nCiAgICAgICsnPGJ1dHRvbiBjbGFzcz0iYnRuIHNt",
  "JysocGxheWluZz8nIHBsYXlvbic6JycpKyciIG9uY2xpY2s9InBsYXlVcmwoXCcnK2VzaihzLnVybCkrJ1wnLFwnJytlc2oocy5u",
  "YW1lKSsnXCcsXCdcJykiPicrKHBsYXlpbmc/J+KdmuKdmic6J+KWticpKyc8L2J1dHRvbj4nCiAgICAgICsnPGJ1dHRvbiBjbGFz",
  "cz0iZmF2JysoZmF2Pycgb24nOicnKSsnIiBvbmNsaWNrPSJ0b2dnbGVGYXYoXCcnK2VzaihzLm5hbWUpKydcJyxcJycrZXNqKHMu",
  "dXJsKSsnXCcpIj4nKyhmYXY/J+KYhSc6J+KYhicpKyc8L2J1dHRvbj4nCiAgICAgICsnPGJ1dHRvbiBjbGFzcz1kZWwgb25jbGlj",
  "az0iZGVsU3RhdGlvbignK2krJykiPuKclTwvYnV0dG9uPjwvZGl2Pic7CiAgfSk7CiAgZWwuaW5uZXJIVE1MPWg7Cn0KdmFyIGFk",
  "ZFN0YXRpb249ZnVuY3Rpb24oKXsKICB2YXIgbj1kb2N1bWVudC5nZXRFbGVtZW50QnlJZCgnYWRkLW5hbWUnKS52YWx1ZS50cmlt",
  "KCk7IHZhciB1PWRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdhZGQtdXJsJykudmFsdWUudHJpbSgpOwogIGlmKCFufHwhdSl7YWxl",
  "cnQoJ05hbWUgdW5kIFVSTCBuw7Z0aWcnKTtyZXR1cm47fSBpZighL15odHRwcz86XC9cLy8udGVzdCh1KSl7YWxlcnQoJ1VSTCBt",
  "dXNzIGh0dHAocyk6Ly8gc2VpbicpO3JldHVybjt9CiAgZmV0Y2goJy9hcGkvc3RhdGlvbnMtYWRkJyx7bWV0aG9kOidQT1NUJyxo",
  "ZWFkZXJzOnsnQ29udGVudC1UeXBlJzonYXBwbGljYXRpb24vanNvbid9LGJvZHk6SlNPTi5zdHJpbmdpZnkoe25hbWU6bix1cmw6",
  "dX0pfSkKICAgIC50aGVuKGZ1bmN0aW9uKHIpe2lmKCFyLm9rKXJldHVybiByLnRleHQoKS50aGVuKGZ1bmN0aW9uKHQpe3Rocm93",
  "IHQ7fSk7fSkKICAgIC50aGVuKGZ1bmN0aW9uKCl7ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2FkZC1uYW1lJykudmFsdWU9Jyc7",
  "ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ2FkZC11cmwnKS52YWx1ZT0nJztsb2FkU3RhdGlvbnMoKTt9KQogICAgLmNhdGNoKGZ1",
  "bmN0aW9uKGUpe2FsZXJ0KCdGZWhsZXI6ICcrZSk7fSk7Cn0KdmFyIGRlbFN0YXRpb249ZnVuY3Rpb24oaWR4KXsKICBpZighY29u",
  "ZmlybSgnU2VuZGVyIGzDtnNjaGVuPycpKXJldHVybjsKICBmZXRjaCgnL2FwaS9zdGF0aW9ucy1kZWxldGUnLHttZXRob2Q6J1BP",
  "U1QnLGhlYWRlcnM6eydDb250ZW50LVR5cGUnOidhcHBsaWNhdGlvbi9qc29uJ30sYm9keTpKU09OLnN0cmluZ2lmeSh7aW5kZXg6",
  "aWR4fSl9KS50aGVuKGxvYWRTdGF0aW9ucyk7Cn0KdmFyIHJlbmRlclBvZGNhc3RzPWZ1bmN0aW9uKCl7CiAgdmFyIGVsPWRvY3Vt",
  "ZW50LmdldEVsZW1lbnRCeUlkKCdwb2QtbGlzdCcpOwogIGlmKCFwb2RMaXN0Lmxlbmd0aCl7ZWwuaW5uZXJIVE1MPSc8cCBjbGFz",
  "cz1tdXRlZD5LZWluZSBQb2RjYXN0cy48L3A+JztyZXR1cm47fQogIHZhciBoPScnOwogIHBvZExpc3QuZm9yRWFjaChmdW5jdGlv",
  "bihwLGkpewogICAgaCs9JzxkaXYgY2xhc3M9InBvZCcrKGk9PT1jdXJQb2RJZHg/JyBhY3RpdmUnOicnKSsnIj48ZGl2IGNsYXNz",
  "PWluZm8gb25jbGljaz0ic2VsZWN0UG9kY2FzdCgnK2krJykiIHN0eWxlPSJmbGV4OjEiPicKICAgICAgKyc8ZGl2IGNsYXNzPW5h",
  "bWU+Jytlc2MocC5uYW1lKSsnPC9kaXY+PGRpdiBjbGFzcz1tZXRhPicrZXNjKChwLnVybHx8JycpLnJlcGxhY2UoL15odHRwcz86",
  "XC9cLy8sJycpLnN1YnN0cmluZygwLDU1KSkrJzwvZGl2PjwvZGl2PicKICAgICAgKyc8YnV0dG9uIGNsYXNzPWRlbCBvbmNsaWNr",
  "PSJkZWxQb2RjYXN0KCcraSsnKSI+4pyVPC9idXR0b24+PC9kaXY+JzsKICB9KTsKICBlbC5pbm5lckhUTUw9aDsKfQp2YXIgc2Vs",
  "ZWN0UG9kY2FzdD1hc3luYyBmdW5jdGlvbihpZHgpewogIGN1clBvZElkeD1pZHg7IHJlbmRlclBvZGNhc3RzKCk7CiAgdmFyIHBh",
  "bmVsPWRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdlcC1wYW5lbCcpOyB2YXIgZXBMaXN0PWRvY3VtZW50LmdldEVsZW1lbnRCeUlk",
  "KCdlcC1saXN0Jyk7CiAgcGFuZWwuc3R5bGUuZGlzcGxheT0nYmxvY2snOyBkb2N1bWVudC5nZXRFbGVtZW50QnlJZCgnZXAtdGl0",
  "bGUnKS50ZXh0Q29udGVudD1wb2RMaXN0W2lkeF0ubmFtZTsKICBlcExpc3QuaW5uZXJIVE1MPSc8cCBjbGFzcz1tdXRlZD5MYWRl",
  "IEVwaXNvZGVu4oCmPC9wPic7CiAgdHJ5ewogICAgdmFyIHI9YXdhaXQgZmV0Y2goJy9hcGkvcHJveHk/dXJsPScrZW5jb2RlVVJJ",
  "Q29tcG9uZW50KHBvZExpc3RbaWR4XS51cmwpKTsKICAgIGlmKCFyLm9rKXRocm93IG5ldyBFcnJvcignUHJveHkgJytyLnN0YXR1",
  "cyk7CiAgICB2YXIgdHh0PWF3YWl0IHIudGV4dCgpOwogICAgdmFyIHhtbD1uZXcgRE9NUGFyc2VyKCkucGFyc2VGcm9tU3RyaW5n",
  "KHR4dCwndGV4dC94bWwnKTsKICAgIHZhciBpdGVtcz1BcnJheS5mcm9tKHhtbC5xdWVyeVNlbGVjdG9yQWxsKCdpdGVtJykpLnNs",
  "aWNlKDAsMjUpOwogICAgY3VyRXBpc29kZXM9aXRlbXMubWFwKGZ1bmN0aW9uKGl0ZW0pewogICAgICB2YXIgZW5jPWl0ZW0ucXVl",
  "cnlTZWxlY3RvcignZW5jbG9zdXJlJyk7IHZhciBkdXI9aXRlbS5xdWVyeVNlbGVjdG9yKCdkdXJhdGlvbicpOwogICAgICByZXR1",
  "cm57dGl0bGU6KGl0ZW0ucXVlcnlTZWxlY3RvcigndGl0bGUnKXx8e3RleHRDb250ZW50OicnfSkudGV4dENvbnRlbnQudHJpbSgp",
  "fHwnKG9obmUgVGl0ZWwpJywKICAgICAgICB1cmw6KGVuYyYmZW5jLmdldEF0dHJpYnV0ZSgndXJsJykpfHwnJywKICAgICAgICBk",
  "YXRlOigoaXRlbS5xdWVyeVNlbGVjdG9yKCdwdWJEYXRlJyl8fHt0ZXh0Q29udGVudDonJ30pLnRleHRDb250ZW50KS50cmltKCku",
  "c3Vic3RyaW5nKDUsMTYpLAogICAgICAgIGR1cmF0aW9uOigoZHVyJiZkdXIudGV4dENvbnRlbnQpfHwnJykudHJpbSgpfTsKICAg",
  "IH0pLmZpbHRlcihmdW5jdGlvbihlKXtyZXR1cm4gZS51cmw7fSk7CiAgICByZW5kZXJFcGlzb2RlcygpOwogIH1jYXRjaChlKXtl",
  "cExpc3QuaW5uZXJIVE1MPSc8cCBzdHlsZT0iY29sb3I6dmFyKC0tZGFuZ2VyKSI+RmVlZC1GZWhsZXI6ICcrZXNjKFN0cmluZyhl",
  "KSkrJzwvcD4nO30KfQp2YXIgcmVuZGVyRXBpc29kZXM9ZnVuY3Rpb24oKXsKICB2YXIgZWw9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5",
  "SWQoJ2VwLWxpc3QnKTsKICBpZighY3VyRXBpc29kZXMubGVuZ3RoKXtlbC5pbm5lckhUTUw9JzxwIGNsYXNzPW11dGVkPktlaW5l",
  "IEVwaXNvZGVuLjwvcD4nO3JldHVybjt9CiAgdmFyIGg9Jyc7CiAgY3VyRXBpc29kZXMuZm9yRWFjaChmdW5jdGlvbihlKXsKICAg",
  "IHZhciBwbGF5aW5nPShlLnVybD09PWN1clVybCYmaXNQbGF5aW5nKTsKICAgIGgrPSc8ZGl2IGNsYXNzPSJyb3cnKyhlLnVybD09",
  "PWN1clVybD8nIG9uJzonJykrJyI+PGRpdiBjbGFzcz1pbmZvPjxkaXYgY2xhc3M9bmFtZT4nK2VzYyhlLnRpdGxlKSsnPC9kaXY+",
  "JwogICAgICArJzxkaXYgY2xhc3M9bWV0YT4nK2VzYyhlLmRhdGUpKyhlLmR1cmF0aW9uPycgwrcgJytlc2MoZS5kdXJhdGlvbik6",
  "JycpKyc8L2Rpdj48L2Rpdj4nCiAgICAgICsnPGJ1dHRvbiBjbGFzcz0iYnRuIHNtJysocGxheWluZz8nIHBsYXlvbic6JycpKyci",
  "IG9uY2xpY2s9InBsYXlVcmwoXCcnK2VzaihlLnVybCkrJ1wnLFwnJytlc2ooZS50aXRsZSkrJ1wnLFwnXCcpIj4nKyhwbGF5aW5n",
  "PyfinZrinZonOifilrYnKSsnPC9idXR0b24+PC9kaXY+JzsKICB9KTsKICBlbC5pbm5lckhUTUw9aDsKfQp2YXIgYWRkUG9kY2Fz",
  "dD1mdW5jdGlvbigpewogIHZhciBuPWRvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdwb2QtbmFtZScpLnZhbHVlLnRyaW0oKTsgdmFy",
  "IHU9ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ3BvZC11cmwnKS52YWx1ZS50cmltKCk7CiAgaWYoIW58fCF1KXthbGVydCgnTmFt",
  "ZSB1bmQgRmVlZCBuw7Z0aWcnKTtyZXR1cm47fQogIGZldGNoKCcvYXBpL3BvZGNhc3RzLWFkZCcse21ldGhvZDonUE9TVCcsaGVh",
  "ZGVyczp7J0NvbnRlbnQtVHlwZSc6J2FwcGxpY2F0aW9uL2pzb24nfSxib2R5OkpTT04uc3RyaW5naWZ5KHtuYW1lOm4sdXJsOnV9",
  "KX0pCiAgICAudGhlbihmdW5jdGlvbihyKXtpZighci5vaylyZXR1cm4gci50ZXh0KCkudGhlbihmdW5jdGlvbih0KXt0aHJvdyB0",
  "O30pO30pCiAgICAudGhlbihmdW5jdGlvbigpe2RvY3VtZW50LmdldEVsZW1lbnRCeUlkKCdwb2QtbmFtZScpLnZhbHVlPScnO2Rv",
  "Y3VtZW50LmdldEVsZW1lbnRCeUlkKCdwb2QtdXJsJykudmFsdWU9Jyc7bG9hZFBvZGNhc3RzKCk7fSkKICAgIC5jYXRjaChmdW5j",
  "dGlvbihlKXthbGVydChlKTt9KTsKfQp2YXIgZGVsUG9kY2FzdD1mdW5jdGlvbihpZHgpewogIGlmKCFjb25maXJtKCdQb2RjYXN0",
  "IGVudGZlcm5lbj8nKSlyZXR1cm47CiAgaWYoY3VyUG9kSWR4PT09aWR4KXtjdXJQb2RJZHg9LTE7ZG9jdW1lbnQuZ2V0RWxlbWVu",
  "dEJ5SWQoJ2VwLXBhbmVsJykuc3R5bGUuZGlzcGxheT0nbm9uZSc7fQogIGZldGNoKCcvYXBpL3BvZGNhc3RzLXJlbW92ZScse21l",
  "dGhvZDonUE9TVCcsaGVhZGVyczp7J0NvbnRlbnQtVHlwZSc6J2FwcGxpY2F0aW9uL2pzb24nfSxib2R5OkpTT04uc3RyaW5naWZ5",
  "KHtpbmRleDppZHh9KX0pLnRoZW4obG9hZFBvZGNhc3RzKTsKfQp2YXIgcmVuZGVyQWxsPWZ1bmN0aW9uKCl7cmVuZGVyRmF2cygp",
  "O3JlbmRlclN0YXRpb25zKCk7aWYoZGlzY1Jlc3VsdHMubGVuZ3RoKXJlbmRlckRpc2NvdmVyKCcnKTtpZihjdXJFcGlzb2Rlcy5s",
  "ZW5ndGgpcmVuZGVyRXBpc29kZXMoKTt9CmF1ZGlvLmFkZEV2ZW50TGlzdGVuZXIoJ3BsYXlpbmcnLGZ1bmN0aW9uKCl7aXNQbGF5",
  "aW5nPXRydWU7dXBkYXRlTlAoKTtyZW5kZXJBbGwoKTtzZXRNZWRpYVNlc3Npb24oKTt9KTsKYXVkaW8uYWRkRXZlbnRMaXN0ZW5l",
  "cignd2FpdGluZycsZnVuY3Rpb24oKXtkb2N1bWVudC5nZXRFbGVtZW50QnlJZCgnbnAtc3ViJykudGV4dENvbnRlbnQ9J1B1ZmZl",
  "cmXigKYnO30pOwphdWRpby5hZGRFdmVudExpc3RlbmVyKCdlcnJvcicsZnVuY3Rpb24oKXtpZighYXVkaW8uc3JjKXJldHVybjtp",
  "c1BsYXlpbmc9ZmFsc2U7ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ25wLXN1YicpLnRleHRDb250ZW50PSdTdHJlYW0tRmVobGVy",
  "Jztkb2N1bWVudC5nZXRFbGVtZW50QnlJZCgnbm93JykuY2xhc3NMaXN0LnJlbW92ZSgnbGl2ZScpO3JlbmRlckFsbCgpO30pOwph",
  "dWRpby5hZGRFdmVudExpc3RlbmVyKCdwYXVzZScsZnVuY3Rpb24oKXtpZighYXVkaW8uc3JjKXJldHVybjtpc1BsYXlpbmc9ZmFs",
  "c2U7dXBkYXRlTlAoKTtyZW5kZXJBbGwoKTt9KTsKKGZ1bmN0aW9uIGluaXQoKXsKICB2YXIgc3Y9bnVsbDt0cnl7c3Y9bG9jYWxT",
  "dG9yYWdlLmdldEl0ZW0oJ3J2Jyk7fWNhdGNoKGUpe30KICBpZihzdil7ZG9jdW1lbnQuZ2V0RWxlbWVudEJ5SWQoJ3ZvbCcpLnZh",
  "bHVlPXN2O2F1ZGlvLnZvbHVtZT1wYXJzZUZsb2F0KHN2KTt9CiAgYnVpbGRMb2NhbENoaXBzKCk7IGJ1aWxkR2VucmVDaGlwcygp",
  "OwogIFByb21pc2UuYWxsKFtsb2FkU3RhdGlvbnMoKSxsb2FkRmF2cygpLGxvYWRQb2RjYXN0cygpXSkudGhlbihmdW5jdGlvbigp",
  "e3VwZGF0ZU5QKCk7fSk7Cn0pKCk7Cg==",
};
static const int WR_UI_B64_COUNT = 383;


static int wrB64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static String wrDecodeUi() {
    String out; out.reserve(30000);
    int val = 0, valb = -8;
    for (int i = 0; i < WR_UI_B64_COUNT; i++) {
        const char* p = WR_UI_B64[i];
        while (*p) {
            char c = *p++;
            if (c == '=') continue;
            int d = wrB64Val(c); if (d < 0) continue;
            val = (val << 6) + d; valb += 6;
            if (valb >= 0) { out += (char)((val >> valb) & 0xFF); valb -= 8; }
        }
    }
    return out;
}


void handleRoot() {
    unsigned long upSec = millis() / 1000UL;
    uint32_t heap = ESP.getFreeHeap();
    uint32_t sketchFree = ESP.getFreeSketchSpace();
    String ui = wrDecodeUi();
    int sp = ui.indexOf("<!--WRSPLIT-->");
    if (sp < 0) { webServer.send(500, "text/plain", "UI corrupt"); return; }
    String page; page.reserve(ui.length() + 1500);
    page += ui.substring(0, sp);
    page += F("<div id=pane-status class=pane><h2 class=sec-h>Ger&auml;t</h2><table>");
    page += "<tr><td>Name</td><td>" + escHtml(deviceName) + "</td></tr>";
    page += "<tr><td>Board</td><td>" + String(BOARD_TYPE) + " / " + String(ESP.getChipModel()) + "</td></tr>";
    page += "<tr><td>MAC</td><td class=mono>" + getMac() + "</td></tr>";
    page += "<tr><td>IP</td><td class=mono>" + getLocalIp() + "</td></tr>";
    page += "<tr><td>RSSI</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
    page += "<tr><td>Uptime</td><td>" + fmtUptime(upSec) + "</td></tr>";
    page += "<tr><td>Firmware</td><td>v" + String(FW_VERSION) + "</td></tr></table>";
    page += F("<h2 class=sec-h>Speicher</h2><table>");
    page += "<tr><td>RAM frei</td><td>" + String(heap/1024) + " KB</td></tr>";
    page += "<tr><td>OTA Flash</td><td>" + String(sketchFree/1024) + " KB</td></tr></table>";
    page += F("<h2 class=sec-h>ESP-Hub</h2><table>");
    page += "<tr><td>Hub</td><td class=mono>" + escHtml(hubHost) + ":" + String(hubPort) + "</td></tr>";
    page += "<tr><td>Heartbeat</td><td>alle " + String(heartbeatInterval/1000) + " s</td></tr></table>";
    page += F("<p style=\"margin-top:14px\"><button class=\"btn ghost\" onclick=location.reload()>Aktualisieren</button></p></div>");
    page += F("<audio id=audio preload=none></audio><script>");
    // skip marker
    page += ui.substring(sp + 14);
    page += F("</script></div></body></html>");
    webServer.send(200, "text/html; charset=UTF-8", page);
}


void handleOtaPage() {
    String html; html.reserve(4200);
    html += F("<!DOCTYPE html><html lang=de><head><meta charset=UTF-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
              "<title>OTA</title><link href=\"https://fonts.bunny.net/css?family=bricolage-grotesque:700|figtree:500&display=swap\" rel=stylesheet>"
              "<style>body{margin:0;background:#0b0e14;color:#f3efe6;font-family:Figtree,sans-serif;padding:24px}"
              "h1{font-family:'Bricolage Grotesque',sans-serif;background:linear-gradient(120deg,#f3efe6,#ffc56d);-webkit-background-clip:text;background-clip:text;color:transparent}"
              "a{color:#ffc56d}.card{margin-top:18px;border:1px solid rgba(255,255,255,.08);border-radius:16px;padding:18px}"
              ".drop{border:2px dashed rgba(255,255,255,.15);border-radius:12px;padding:28px;text-align:center;cursor:pointer;color:#9aa3b2}"
              ".btn{margin-top:12px;width:100%;border:0;border-radius:999px;padding:12px;font-weight:700;background:linear-gradient(135deg,#e8a54b,#c47d28);color:#1a1208;cursor:pointer}"
              ".bar{height:8px;background:rgba(255,255,255,.06);border-radius:99px;overflow:hidden;margin-top:12px}.bar i{display:block;height:100%;width:0;background:#e8a54b}"
              "</style></head><body><h1>WEB·RADIO OTA</h1><p><a href=/>&larr; zur&uuml;ck</a> · v");
    html += FW_VERSION;
    html += F("</p><div class=card><div class=drop id=drop onclick=\"document.getElementById('fw').click()\">.bin hierher oder klicken</div>"
              "<input type=file id=fw accept=.bin style=\"display:none\">"
              "<div id=fn style=\"margin-top:8px;color:#9aa3b2;font-size:12px\"></div>"
              "<button class=btn onclick=go()>Flashen</button><div class=bar><i id=bar></i></div><div id=msg style=\"margin-top:8px;color:#9aa3b2\"></div></div>"
              "<script>var inp=document.getElementById('fw');inp.onchange=function(){if(inp.files[0])document.getElementById('fn').textContent=inp.files[0].name;};"
              "var drop=document.getElementById('drop');drop.ondragover=function(e){e.preventDefault();};drop.ondrop=function(e){e.preventDefault();var f=e.dataTransfer.files[0];if(f){var dt=new DataTransfer();dt.items.add(f);inp.files=dt.files;document.getElementById('fn').textContent=f.name;}};"
              "function go(){if(!inp.files[0]){alert('bin waehlen');return;}var fd=new FormData();fd.append('firmware',inp.files[0]);var x=new XMLHttpRequest();x.open('POST','/ota-upload');"
              "x.upload.onprogress=function(e){if(e.lengthComputable){document.getElementById('bar').style.width=Math.round(e.loaded/e.total*100)+'%';}};"
              "x.onload=function(){document.getElementById('msg').textContent=x.status===200?'OK — Neustart…':'Fehler';};x.send(fd);}</script></body></html>");
    webServer.send(200, "text/html; charset=UTF-8", html);
}

void handleOtaUpload() {
    HTTPUpload& upload = webServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
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
    else webServer.send(200, "text/plain", "OK");
    delay(500); ESP.restart();
}
void handleNotFound() { webServer.sendHeader("Location", "/", true); webServer.send(302, "text/plain", ""); }

static bool jsonArrayAdd(const String& existing, const String& newName, const String& newUrl, int maxItems, String& outJson) {
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
    obj["name"] = newName; obj["url"] = newUrl;
    serializeJson(doc, outJson); return true;
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
    arr.remove(idx); serializeJson(doc, outJson); return true;
}

void handleApiStations() { webServer.send(200, "application/json", getStationsJson()); }
void handleApiStationsAdd() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String n = req["name"].as<String>(), u = req["url"].as<String>();
    if (!n.length()||!u.length()) { webServer.send(400,"text/plain","name+url"); return; }
    if (!u.startsWith("http")) { webServer.send(400,"text/plain","URL"); return; }
    String out; if (!jsonArrayAdd(getStationsJson(), n, u, MAX_STATIONS, out)) { webServer.send(400,"text/plain","Maximum"); return; }
    saveStationsJson(out); webServer.send(200,"application/json",out);
}
void handleApiStationsDelete() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(64);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String out; if (!jsonArrayRemove(getStationsJson(), req["index"]|(-1), out)) { webServer.send(400,"text/plain","Index"); return; }
    saveStationsJson(out); webServer.send(200,"application/json",out);
}
void handleApiFavorites() { webServer.send(200,"application/json",getFavoritesJson()); }
void handleApiFavoritesAdd() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String n = req["name"].as<String>(), u = req["url"].as<String>();
    if (!n.length()||!u.length()) { webServer.send(400,"text/plain","name+url"); return; }
    String out; if (!jsonArrayAdd(getFavoritesJson(), n, u, MAX_FAVORITES, out)) { webServer.send(400,"text/plain","Maximum"); return; }
    saveFavoritesJson(out); webServer.send(200,"application/json",out);
}
void handleApiFavoritesRemove() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(64);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String out; if (!jsonArrayRemove(getFavoritesJson(), req["index"]|(-1), out)) { webServer.send(400,"text/plain","Index"); return; }
    saveFavoritesJson(out); webServer.send(200,"application/json",out);
}
void handleApiPodcasts() { webServer.send(200,"application/json",getPodcastsJson()); }
void handleApiPodcastsAdd() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(512);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String n = req["name"].as<String>(), u = req["url"].as<String>();
    if (!n.length()||!u.length()) { webServer.send(400,"text/plain","name+url"); return; }
    String out; if (!jsonArrayAdd(getPodcastsJson(), n, u, MAX_PODCASTS, out)) { webServer.send(400,"text/plain","Maximum"); return; }
    savePodcastsJson(out); webServer.send(200,"application/json",out);
}
void handleApiPodcastsRemove() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(64);
#endif
    deserializeJson(req, webServer.arg("plain"));
    String out; if (!jsonArrayRemove(getPodcastsJson(), req["index"]|(-1), out)) { webServer.send(400,"text/plain","Index"); return; }
    savePodcastsJson(out); webServer.send(200,"application/json",out);
}
void handleApiLastGet() { webServer.send(200,"application/json",getLastPlayedJson()); }
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
void handleApiStatus() {
    String j = "{";
    j += "\"name\":\"" + escJson(deviceName) + "\",";
    j += "\"version\":\"" + String(FW_VERSION) + "\",";
    j += "\"hwType\":\"" + String(BOARD_TYPE) + "\",";
    j += "\"chipModel\":\"" + String(ESP.getChipModel()) + "\",";
    j += "\"mac\":\"" + getMac() + "\",";
    j += "\"ip\":\"" + getLocalIp() + "\",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    j += "\"uptime\":" + String(millis()/1000UL) + ",";
    j += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"freeSketch\":" + String(ESP.getFreeSketchSpace()) + ",";
    j += "\"hub\":\"" + escJson(hubHost) + ":" + String(hubPort) + "\"}";
    webServer.send(200, "application/json", j);
}
void handleApiNowPlayingGet() {
    prefs.begin("webradio", true);
    String n = prefs.getString("lastName", ""); String u = prefs.getString("lastUrl", ""); String t = prefs.getString("nowTitle", "");
    prefs.end();
    webServer.send(200, "application/json",
        "{\"name\":\"" + escJson(n) + "\",\"url\":\"" + escJson(u) + "\",\"title\":\"" + escJson(t) + "\"}");
}
void handleApiNowPlayingPost() {
    if (!webServer.hasArg("plain")) { webServer.send(400,"text/plain","Kein Body"); return; }
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument req;
#else
    DynamicJsonDocument req(768);
#endif
    deserializeJson(req, webServer.arg("plain"));
    prefs.begin("webradio", false);
    prefs.putString("lastName", req["name"] | "");
    prefs.putString("lastUrl", req["url"] | "");
    prefs.putString("nowTitle", req["title"] | "");
    prefs.end();
    webServer.send(200, "application/json", "{\"ok\":true}");
}
void handleApiProxy() {
    if (!webServer.hasArg("url")) { webServer.send(400, "text/plain", "url fehlt"); return; }
    String url = webServer.arg("url");
    if (!url.startsWith("http://") && !url.startsWith("https://")) { webServer.send(400, "text/plain", "url"); return; }
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(12000);
    http.begin(url);
    http.setUserAgent("ESP32-WebRadio/2.4");
    int code = http.GET();
    if (code != 200) { String err = "upstream " + String(code); http.end(); webServer.send(502, "text/plain", err); return; }
    WiFiClient* stream = http.getStreamPtr();
    String body; body.reserve(16384);
    unsigned long start = millis();
    while ((http.connected() || stream->available()) && (millis() - start < 15000UL)) {
        while (stream->available()) {
            body += (char)stream->read();
            if ((int)body.length() >= PROXY_MAX_BYTES) break;
        }
        if ((int)body.length() >= PROXY_MAX_BYTES) break;
        if (!stream->available()) delay(1);
        if (!stream->available() && !http.connected()) break;
    }
    http.end();
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/xml; charset=UTF-8", body);
}

void setupWebServer() {
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/ota", HTTP_GET, handleOtaPage);
    webServer.on("/ota-upload", HTTP_POST, handleOtaUploadFinish, handleOtaUpload);
    webServer.on("/api/stations", HTTP_GET, handleApiStations);
    webServer.on("/api/stations-add", HTTP_POST, handleApiStationsAdd);
    webServer.on("/api/stations-delete", HTTP_POST, handleApiStationsDelete);
    webServer.on("/api/favorites", HTTP_GET, handleApiFavorites);
    webServer.on("/api/favorites-add", HTTP_POST, handleApiFavoritesAdd);
    webServer.on("/api/favorites-remove", HTTP_POST, handleApiFavoritesRemove);
    webServer.on("/api/podcasts", HTTP_GET, handleApiPodcasts);
    webServer.on("/api/podcasts-add", HTTP_POST, handleApiPodcastsAdd);
    webServer.on("/api/podcasts-remove", HTTP_POST, handleApiPodcastsRemove);
    webServer.on("/api/last", HTTP_GET, handleApiLastGet);
    webServer.on("/api/last", HTTP_POST, handleApiLastPost);
    webServer.on("/api/status", HTTP_GET, handleApiStatus);
    webServer.on("/api/nowplaying", HTTP_GET, handleApiNowPlayingGet);
    webServer.on("/api/nowplaying", HTTP_POST, handleApiNowPlayingPost);
    webServer.on("/api/proxy", HTTP_GET, handleApiProxy);
    webServer.onNotFound(handleNotFound);
    webServer.begin();
    Serial.println("[WEB] http://" + getLocalIp() + "/");
}

String buildHeartbeat() {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    DynamicJsonDocument doc(1536);
#endif
    doc["mac"] = getMac(); doc["name"] = deviceName; doc["hwType"] = BOARD_TYPE;
    doc["chipModel"] = ESP.getChipModel(); doc["version"] = FW_VERSION;
    doc["ip"] = getLocalIp(); doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = millis() / 1000UL; doc["freeHeap"] = ESP.getFreeHeap();
    doc["freeSketch"] = ESP.getFreeSketchSpace();
    prefs.begin("webradio", true);
    String lastName = prefs.getString("lastName", "");
    String nowTitle = prefs.getString("nowTitle", "");
    prefs.end();
    String favJson = getFavoritesJson(); String stnJson = getStationsJson();
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument fa; JsonDocument sa;
#else
    DynamicJsonDocument fa(4096); DynamicJsonDocument sa(4096);
#endif
    deserializeJson(fa, favJson); deserializeJson(sa, stnJson);
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonObject ios = doc["ios"].to<JsonObject>();
    JsonObject ioFav = ios["favoriten"].to<JsonObject>();
    JsonObject ioStn = ios["sender"].to<JsonObject>();
    JsonObject ioNow = ios["nowStation"].to<JsonObject>();
    JsonObject ioTit = ios["nowTitle"].to<JsonObject>();
#else
    JsonObject ios = doc.createNestedObject("ios");
    JsonObject ioFav = ios.createNestedObject("favoriten");
    JsonObject ioStn = ios.createNestedObject("sender");
    JsonObject ioNow = ios.createNestedObject("nowStation");
    JsonObject ioTit = ios.createNestedObject("nowTitle");
#endif
    ioFav["type"]="info"; ioFav["value"]=(float)fa.as<JsonArray>().size(); ioFav["unit"]="";
    ioStn["type"]="info"; ioStn["value"]=(float)sa.as<JsonArray>().size(); ioStn["unit"]="";
    ioNow["type"]="info"; ioNow["value"]=0; ioNow["unit"]=lastName;
    ioTit["type"]="info"; ioTit["value"]=0; ioTit["unit"]=nowTitle;
    String out; serializeJson(doc, out); return out;
}

void sendHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.begin("http://" + hubHost + ":" + String(hubPort) + "/api/register");
    http.addHeader("Content-Type", "application/json"); http.setTimeout(8000);
    int code = http.POST(buildHeartbeat());
    if (code == 200) {
        String body = http.getString(); lastSuccess = millis();
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
    } else Serial.printf("[HB] HTTP %d\n", code);
    http.end();
}

void performOta(const String& url) {
    HTTPClient http; http.begin(url); http.setTimeout(30000);
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    int total = http.getSize();
    if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) { http.end(); return; }
    WiFiClient* s = http.getStreamPtr(); uint8_t buf[512]; size_t written = 0;
    while (http.connected() && (total <= 0 || written < (size_t)total)) {
        size_t av = s->available(); if (!av) { delay(1); continue; }
        size_t r = s->readBytes(buf, min(av, sizeof(buf))); if (!r) break;
        Update.write(buf, r); written += r;
    }
    if (Update.end(true)) { http.end(); delay(500); ESP.restart(); }
    http.end();
}

void checkResetButton() {
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(RESET_BUTTON_PIN) == HIGH) return;
    unsigned long start = millis();
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
        if (millis() - start > (unsigned long)RESET_HOLD_SEC * 1000UL) {
            wifiManager.resetSettings();
            prefs.begin("esphub", false); prefs.clear(); prefs.end();
            delay(500); ESP.restart();
        }
        delay(100);
    }
}

void setupWifi() {
    prefs.begin("esphub", false);
    deviceName = prefs.getString("name", deviceName);
    hubHost = prefs.getString("hub_host", hubHost);
    hubPort = prefs.getInt("hub_port", hubPort);
    prefs.end();
    WiFiManagerParameter paramName("name", "Geraetename", deviceName.c_str(), 32);
    WiFiManagerParameter paramHost("hub_host", "ESP-Hub IP", hubHost.c_str(), 40);
    WiFiManagerParameter paramPort("hub_port", "ESP-Hub Port", String(hubPort).c_str(), 6);
    wifiManager.addParameter(&paramName); wifiManager.addParameter(&paramHost); wifiManager.addParameter(&paramPort);
    wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
    if (!wifiManager.autoConnect(WIFI_AP_NAME)) { delay(1000); ESP.restart(); }
    prefs.begin("esphub", false);
    prefs.putString("name", String(paramName.getValue()));
    prefs.putString("hub_host", String(paramHost.getValue()));
    prefs.putInt("hub_port", String(paramPort.getValue()).toInt());
    prefs.end();
    deviceName = String(paramName.getValue());
    hubHost = String(paramHost.getValue());
    hubPort = String(paramPort.getValue()).toInt();
}

void setup() {
    Serial.begin(115200); delay(500);
    Serial.println();
    Serial.println(String("=== ESP32 Web-Radio v") + FW_VERSION + " ===");
    checkResetButton(); setupWifi();
    String mdnsName = "webradio-" + getMac().substring(6);
    if (MDNS.begin(mdnsName.c_str())) Serial.println("[mDNS] " + mdnsName + ".local");
    setupWebServer();
    lastSuccess = millis(); sendHeartbeat(); lastHeartbeat = millis();
}

void loop() {
    webServer.handleClient();
    unsigned long now = millis();
    if (now - lastHeartbeat >= heartbeatInterval) { lastHeartbeat = now; sendHeartbeat(); }
    if (otaPending) { otaPending = false; performOta(otaUrl); otaUrl = ""; }
    if (WiFi.status() != WL_CONNECTED) { delay(5000); if (WiFi.status() != WL_CONNECTED) WiFi.reconnect(); }
    delay(10);
}
