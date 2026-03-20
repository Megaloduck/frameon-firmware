#include "usb_api.h"
#include "api_server.h"
#include "matrix.h"
#include "wifi_manager.h"
#include "config.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <WiFi.h>

// ── State ─────────────────────────────────────────────────────────────────

static String   _buf;
static bool     _gifOpen     = false;
static File     _gifFile;
static String   _gifPath;
static uint32_t _gifTotal    = 0;
static uint32_t _gifReceived = 0;

// ── Non-blocking WiFi connect state ──────────────────────────────────────
// WiFi connection is started in _handleWifi() and completed asynchronously
// in usb_api_loop(). This prevents blocking the serial loop for 15 seconds.
static bool     _wifiConnecting   = false;
static String   _wifiPendingId    = "";
static uint32_t _wifiConnectStart = 0;
static const uint32_t WIFI_ASYNC_TIMEOUT = 15000; // ms

// ── Helpers ───────────────────────────────────────────────────────────────

static void _reply(const String &id, bool ok,
                   const String &data = "", const String &error = "") {
    String out = "{\"id\":\"" + id + "\",\"ok\":" + (ok ? "true" : "false");
    if (data.length())  out += ",\"data\":"   + data;
    if (error.length()) out += ",\"error\":\"" + error + "\"";
    out += "}\n";
    Serial.print(out);
}
static void _replyOk(const String &id, const String &data = "") { _reply(id, true, data); }
static void _replyErr(const String &id, const String &msg)       { _reply(id, false, "", msg); }

// ── Command handlers ──────────────────────────────────────────────────────

static void _handlePing(const String &id, JsonDocument &) {
    _replyOk(id, "{\"pong\":true}");
}

static void _handleStatus(const String &id, JsonDocument &) {
    JsonDocument doc;
    doc["connected"]  = wifi_connected();
    doc["ip"]         = wifi_ip();
    doc["mode"]       = (int)g_mode;
    doc["brightness"] = g_brightness;
    doc["fw"]         = "1.0.0";
    String out; serializeJson(doc, out);
    _replyOk(id, out);
}

static void _handleSetMode(const String &id, JsonDocument &d, Preferences &prefs) {
    const char *modeStr = d["data"]["mode"] | "clock";
    if      (strcmp(modeStr, "clock")    == 0) g_mode = MODE_CLOCK;
    else if (strcmp(modeStr, "spotify")  == 0) g_mode = MODE_SPOTIFY;
    else if (strcmp(modeStr, "gif")      == 0) g_mode = MODE_GIF;
    else if (strcmp(modeStr, "pomodoro") == 0) g_mode = MODE_POMODORO;
    else { _replyErr(id, "unknown mode"); return; }
    prefs.putInt(PREF_MODE, (int)g_mode);
    _replyOk(id);
}

static void _handleSetBrightness(const String &id, JsonDocument &d, Preferences &prefs) {
    int v = d["data"]["value"] | -1;
    if (v < 0 || v > 255) { _replyErr(id, "value must be 0-255"); return; }
    g_brightness = (uint8_t)v;
    matrix_brightness(g_brightness);
    prefs.putInt(PREF_BRIGHTNESS, g_brightness);
    _replyOk(id);
}

static void _handleClockConfig(const String &id, JsonDocument &d, Preferences &prefs) {
    JsonObject data = d["data"];
    if (!data["format24h"].isNull())   g_clockCfg.is24h       = data["format24h"].as<bool>();
    if (!data["showDate"].isNull())    g_clockCfg.showDate    = data["showDate"].as<bool>();
    if (!data["showSeconds"].isNull()) g_clockCfg.showSeconds = data["showSeconds"].as<bool>();
    if (!data["timezone"].isNull())    g_clockCfg.timezone    = data["timezone"].as<String>();
    if (!data["ntp"].isNull())         g_clockCfg.ntpServer   = data["ntp"].as<String>();
    if (!data["brightness"].isNull()) {
        g_brightness = constrain((int)data["brightness"], 0, 255);
        matrix_brightness(g_brightness);
    }
    setenv("TZ", g_clockCfg.timezone.c_str(), 1);
    tzset();
    prefs.putBool  (PREF_CLOCK_24H,  g_clockCfg.is24h);
    prefs.putBool  (PREF_CLOCK_DATE, g_clockCfg.showDate);
    prefs.putBool  (PREF_CLOCK_SECS, g_clockCfg.showSeconds);
    prefs.putString(PREF_TIMEZONE,   g_clockCfg.timezone);
    prefs.putString(PREF_NTP_SERVER, g_clockCfg.ntpServer);
    _replyOk(id);
}

static void _handlePomoConfig(const String &id, JsonDocument &d, Preferences &prefs) {
    JsonObject data = d["data"];
    if (!data["work"].isNull())       g_pomoCfg.workMinutes        = (int)data["work"];
    if (!data["shortBreak"].isNull()) g_pomoCfg.shortBreakMinutes  = (int)data["shortBreak"];
    if (!data["longBreak"].isNull())  g_pomoCfg.longBreakMinutes   = (int)data["longBreak"];
    if (!data["sessions"].isNull())   g_pomoCfg.sessionsBeforeLong = (int)data["sessions"];
    prefs.putInt(PREF_POMO_WORK,     g_pomoCfg.workMinutes);
    prefs.putInt(PREF_POMO_SHORT,    g_pomoCfg.shortBreakMinutes);
    prefs.putInt(PREF_POMO_LONG,     g_pomoCfg.longBreakMinutes);
    prefs.putInt(PREF_POMO_SESSIONS, g_pomoCfg.sessionsBeforeLong);
    _replyOk(id);
}

static void _handlePomoCmd(const String &id, JsonDocument &d) {
    const char *cmd = d["data"]["cmd"] | "";
    if      (strcmp(cmd, "start") == 0) { g_pomoTimer.running = true; g_pomoTimer.lastTick = millis(); }
    else if (strcmp(cmd, "pause") == 0) { g_pomoTimer.running = false; }
    else if (strcmp(cmd, "reset") == 0) {
        g_pomoTimer.running = false;
        g_pomoTimer.phase = POMO_WORK;
        g_pomoTimer.secondsRemaining = g_pomoCfg.workMinutes * 60;
        g_pomoTimer.sessionsCompleted = 0;
    } else { _replyErr(id, "unknown cmd"); return; }
    _replyOk(id);
}

static void _handleSpotifyState(const String &id, JsonDocument &d) {
    JsonObject data = d["data"];
    g_spotify.track     = data["track"]   | "";
    g_spotify.artist    = data["artist"]  | "";
    g_spotify.isPlaying = data["playing"] | false;
    if (!data["art"].isNull()) {
        g_spotify.artBase64 = data["art"].as<String>();
        g_spotify.hasArt    = true;
    }
    _replyOk(id);
}

// ── GIF upload ────────────────────────────────────────────────────────────

static void _handleGifStart(const String &id, JsonDocument &d) {
    const char *filename = d["data"]["filename"] | "";
    if (strlen(filename) == 0) { _replyErr(id, "missing filename"); return; }
    if (_gifOpen) { _gifFile.close(); _gifOpen = false; }
    _gifPath = String("/gifs/") + filename;
    _gifTotal = d["data"]["totalBytes"] | 0;
    _gifReceived = 0;
    if (!SPIFFS.exists("/gifs")) SPIFFS.mkdir("/gifs");
    _gifFile = SPIFFS.open(_gifPath, FILE_WRITE);
    if (!_gifFile) { _replyErr(id, "SPIFFS open failed"); return; }
    _gifOpen = true;
    _replyOk(id, "{\"ready\":true}");
}

static void _handleGifChunk(const String &id, JsonDocument &d) {
    if (!_gifOpen) { _replyErr(id, "no upload in progress"); return; }
    const char *b64 = d["data"]["data"] | "";
    bool isFinal    = d["data"]["final"] | false;
    int  chunkIndex = d["data"]["index"] | -1;

    const char b64chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto b64val = [&](char c) -> int {
        const char *p = strchr(b64chars, c); return p ? (int)(p - b64chars) : -1;
    };
    size_t srcLen = strlen(b64);
    size_t maxOut = (srcLen / 4) * 3 + 3;
    uint8_t *buf = new uint8_t[maxOut];
    size_t outLen = 0;
    for (size_t i = 0; i + 3 < srcLen; i += 4) {
        int v0 = b64val(b64[i]), v1 = b64val(b64[i+1]);
        int v2 = b64[i+2]=='='? 0 : b64val(b64[i+2]);
        int v3 = b64[i+3]=='='? 0 : b64val(b64[i+3]);
        if (v0 < 0 || v1 < 0) break;
        buf[outLen++] = (uint8_t)((v0<<2)|(v1>>4));
        if (b64[i+2]!='=') buf[outLen++] = (uint8_t)((v1<<4)|(v2>>2));
        if (b64[i+3]!='=') buf[outLen++] = (uint8_t)((v2<<6)|v3);
    }
    size_t written = _gifFile.write(buf, outLen);
    delete[] buf;
    _gifReceived += written;
    if (written != outLen) {
        _gifFile.close(); _gifOpen = false;
        _replyErr(id, "SPIFFS write failed"); return;
    }
    if (isFinal) {
        _gifFile.close(); _gifOpen = false;
        _replyOk(id, "{\"done\":true,\"bytes\":" + String(_gifReceived) + "}");
    } else {
        _replyOk(id, "{\"chunk\":" + String(chunkIndex) + ",\"received\":" + String(_gifReceived) + "}");
    }
}

static void _handleGifSelect(const String &id, JsonDocument &d, Preferences &prefs) {
    const char *file = d["data"]["file"] | "";
    if (strlen(file) == 0) { _replyErr(id, "missing file"); return; }
    prefs.putString("gif_file", file);
    g_mode = MODE_GIF;
    _replyOk(id);
}

static void _handleGifList(const String &id) {
    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();
    File root = SPIFFS.open("/gifs");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                JsonObject entry = files.add<JsonObject>();
                String name = String(f.name());
                if (name.startsWith("/gifs/")) name = name.substring(6);
                entry["name"] = name;
                entry["size"] = f.size();
            }
            f = root.openNextFile();
        }
    }
    String out; serializeJson(doc, out);
    _replyOk(id, out);
}

static void _handleGifDelete(const String &id, JsonDocument &d) {
    const char *file = d["data"]["file"] | "";
    String path = String("/gifs/") + file;
    if (!SPIFFS.exists(path)) { _replyErr(id, "not found"); return; }
    SPIFFS.remove(path);
    _replyOk(id);
}

// ── WiFi — NON-BLOCKING ────────────────────────────────────────────────────
// OLD: had delay(200) loop for up to 15s → blocked usb_api_loop() entirely
//      → any command sent during connect would time out
// NEW: start WiFi and return immediately; check connection in _poll_wifi()
//      called from usb_api_loop() every iteration

static void _handleWifi(const String &id, JsonDocument &d, Preferences &prefs) {
    if (_wifiConnecting) {
        _replyErr(id, "wifi connect already in progress");
        return;
    }

    // Accept ssid from BOTH d["data"]["ssid"] (new format) and d["ssid"] (old format)
    const char *ssid = "";
    const char *pass = "";
    if (!d["data"]["ssid"].isNull()) {
        ssid = d["data"]["ssid"] | "";
        pass = d["data"]["password"] | "";
    } else if (!d["ssid"].isNull()) {
        // Legacy format fallback
        ssid = d["ssid"] | "";
        pass = d["password"] | "";
    }

    if (strlen(ssid) == 0) { _replyErr(id, "missing ssid"); return; }

    prefs.putString(PREF_SSID, ssid);
    prefs.putString(PREF_PASS, pass);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    // Don't block — record state and return. _poll_wifi() in usb_api_loop()
    // will check WiFi.status() each loop iteration and send the response.
    _wifiConnecting   = true;
    _wifiPendingId    = id;
    _wifiConnectStart = millis();

    Serial.printf("[USB] WiFi connecting to: %s (async)\n", ssid);
    // No reply yet — will be sent by _poll_wifi() when connected or timed out
}

static void _poll_wifi() {
    if (!_wifiConnecting) return;

    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        wifi_set_connected(ip);
        _wifiConnecting = false;
        // New JSON response
        _replyOk(_wifiPendingId, "{\"ip\":\"" + ip + "\"}");
        // Legacy "IP:" line for setup_screen.dart requestDeviceIp()
        Serial.printf("IP:%s\n", ip.c_str());
        Serial.printf("[USB] WiFi connected: %s\n", ip.c_str());
    } else if (millis() - _wifiConnectStart > WIFI_ASYNC_TIMEOUT) {
        _wifiConnecting = false;
        WiFi.disconnect();
        _replyErr(_wifiPendingId, "wifi_connect_failed");
        Serial.println("[USB] WiFi connect timeout");
    }
    // Otherwise: still connecting, try again next loop iteration
}

static void _handleGetIp(const String &id) {
    if (wifi_connected()) {
        _replyOk(id, "{\"ip\":\"" + wifi_ip() + "\"}");
        Serial.printf("IP:%s\n", wifi_ip().c_str());
    } else {
        _replyErr(id, "not_connected");
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────

static void _dispatch(const String &line, Preferences &prefs) {
    if (line.length() < 2 || line[0] != '{') return;

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return;

    const char *id  = doc["id"]  | "";
    const char *cmd = doc["cmd"] | "";
    if (strlen(cmd) == 0) return;

    if      (strcmp(cmd, "ping")           == 0) _handlePing(id, doc);
    else if (strcmp(cmd, "status")         == 0) _handleStatus(id, doc);
    else if (strcmp(cmd, "set_mode")       == 0) _handleSetMode(id, doc, prefs);
    else if (strcmp(cmd, "set_brightness") == 0) _handleSetBrightness(id, doc, prefs);
    else if (strcmp(cmd, "clock_config")   == 0) _handleClockConfig(id, doc, prefs);
    else if (strcmp(cmd, "pomo_config")    == 0) _handlePomoConfig(id, doc, prefs);
    else if (strcmp(cmd, "pomo_cmd")       == 0) _handlePomoCmd(id, doc);
    else if (strcmp(cmd, "spotify_state")  == 0) _handleSpotifyState(id, doc);
    else if (strcmp(cmd, "gif_start")      == 0) _handleGifStart(id, doc);
    else if (strcmp(cmd, "gif_chunk")      == 0) _handleGifChunk(id, doc);
    else if (strcmp(cmd, "gif_select")     == 0) _handleGifSelect(id, doc, prefs);
    else if (strcmp(cmd, "gif_list")       == 0) _handleGifList(id);
    else if (strcmp(cmd, "gif_delete")     == 0) _handleGifDelete(id, doc);
    else if (strcmp(cmd, "wifi")           == 0) _handleWifi(id, doc, prefs);
    else if (strcmp(cmd, "get_ip")         == 0) _handleGetIp(id);
    else _reply(id, false, "", String("unknown command: ") + cmd);
}

// ── Public API ────────────────────────────────────────────────────────────

void usb_api_init(Preferences &prefs) {
    _buf.reserve(2048);
    Serial.printf("[USB] API ready — baud %d\n", SERIAL_BAUD);
}

void usb_api_loop(Preferences &prefs) {
    // Check async WiFi connect state first (non-blocking)
    _poll_wifi();

    // Drain serial and dispatch commands
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            _buf.trim();
            if (_buf.length() > 0) _dispatch(_buf, prefs);
            _buf = "";
        } else {
            if (_buf.length() < 65535) _buf += c;
        }
    }
}

void usb_push(const String &event, const String &jsonData) {
    String out = "{\"push\":\"" + event + "\",\"data\":" + jsonData + "}\n";
    Serial.print(out);
}
