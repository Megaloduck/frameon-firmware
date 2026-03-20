#include "usb_api.h"
#include "api_server.h"   // g_mode, g_brightness, g_clockCfg, g_pomoCfg, g_pomoTimer
#include "matrix.h"       // matrix_brightness()
#include "wifi_manager.h" // wifi_connected(), wifi_ip(), wifi_set_connected()
#include "config.h"
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <WiFi.h>

// ── State ─────────────────────────────────────────────────────────────────

static String   _buf;               // incoming serial line buffer
static bool     _gifOpen  = false;  // GIF upload in progress
static File     _gifFile;
static String   _gifPath;
static uint32_t _gifTotal = 0;
static uint32_t _gifReceived = 0;

// ── Helpers ───────────────────────────────────────────────────────────────

static void _reply(const String &id, bool ok,
                   const String &data = "", const String &error = "") {
    // Build minimal JSON manually to avoid heap churn on large payloads
    String out = "{\"id\":\"" + id + "\",\"ok\":" + (ok ? "true" : "false");
    if (data.length())  out += ",\"data\":"  + data;
    if (error.length()) out += ",\"error\":\"" + error + "\"";
    out += "}\n";
    Serial.print(out);
}

static void _replyOk(const String &id, const String &data = "") {
    _reply(id, true, data);
}

static void _replyErr(const String &id, const String &msg) {
    _reply(id, false, "", msg);
}

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
    String out;
    serializeJson(doc, out);
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
    if (strcmp(cmd, "start") == 0) {
        g_pomoTimer.running  = true;
        g_pomoTimer.lastTick = millis();
    } else if (strcmp(cmd, "pause") == 0) {
        g_pomoTimer.running = false;
    } else if (strcmp(cmd, "reset") == 0) {
        g_pomoTimer.running           = false;
        g_pomoTimer.phase             = POMO_WORK;
        g_pomoTimer.secondsRemaining  = g_pomoCfg.workMinutes * 60;
        g_pomoTimer.sessionsCompleted = 0;
    } else {
        _replyErr(id, "unknown cmd"); return;
    }
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

// ── GIF upload (chunked base64) ───────────────────────────────────────────

static void _handleGifStart(const String &id, JsonDocument &d) {
    const char *filename = d["data"]["filename"] | "";
    if (strlen(filename) == 0) { _replyErr(id, "missing filename"); return; }
    if (_gifOpen) { _gifFile.close(); _gifOpen = false; }

    _gifPath  = String("/gifs/") + filename;
    _gifTotal = d["data"]["totalBytes"] | 0;
    _gifReceived = 0;

    if (!SPIFFS.exists("/gifs")) SPIFFS.mkdir("/gifs");
    _gifFile = SPIFFS.open(_gifPath, FILE_WRITE);
    if (!_gifFile) { _replyErr(id, "SPIFFS open failed"); return; }
    _gifOpen = true;
    Serial.printf("[USB] GIF upload start: %s (%u bytes)\n",
                  _gifPath.c_str(), _gifTotal);
    _replyOk(id, "{\"ready\":true}");
}

static void _handleGifChunk(const String &id, JsonDocument &d) {
    if (!_gifOpen) { _replyErr(id, "no upload in progress — send gif_start first"); return; }

    const char *b64 = d["data"]["data"] | "";
    bool isFinal    = d["data"]["final"] | false;
    int  chunkIndex = d["data"]["index"] | -1;

    // Decode base64 chunk
    // We use a simple local decoder to avoid adding a library dependency.
    // ArduinoJson's base64 support was removed in v7; do it manually.
    const char b64chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto b64val = [&](char c) -> int {
        const char *p = strchr(b64chars, c);
        return p ? (int)(p - b64chars) : -1;
    };

    size_t srcLen = strlen(b64);
    // Output buffer: decoded bytes can be at most 3/4 of base64 length
    size_t maxOut = (srcLen / 4) * 3 + 3;
    uint8_t *buf = new uint8_t[maxOut];
    size_t outLen = 0;

    for (size_t i = 0; i + 3 < srcLen; i += 4) {
        int v0 = b64val(b64[i]);
        int v1 = b64val(b64[i+1]);
        int v2 = b64[i+2] == '=' ? 0 : b64val(b64[i+2]);
        int v3 = b64[i+3] == '=' ? 0 : b64val(b64[i+3]);
        if (v0 < 0 || v1 < 0) break;
        buf[outLen++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (b64[i+2] != '=') buf[outLen++] = (uint8_t)((v1 << 4) | (v2 >> 2));
        if (b64[i+3] != '=') buf[outLen++] = (uint8_t)((v2 << 6) | v3);
    }

    size_t written = _gifFile.write(buf, outLen);
    delete[] buf;
    _gifReceived += written;

    if (written != outLen) {
        _gifFile.close();
        _gifOpen = false;
        _replyErr(id, "SPIFFS write failed — disk full?");
        return;
    }

    if (isFinal) {
        _gifFile.close();
        _gifOpen = false;
        Serial.printf("[USB] GIF upload done: %u bytes written\n", _gifReceived);
        _replyOk(id, "{\"done\":true,\"bytes\":" + String(_gifReceived) + "}");
    } else {
        _replyOk(id, "{\"chunk\":" + String(chunkIndex) + ",\"received\":" +
                      String(_gifReceived) + "}");
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
    String out;
    serializeJson(doc, out);
    _replyOk(id, out);
}

static void _handleGifDelete(const String &id, JsonDocument &d) {
    const char *file = d["data"]["file"] | "";
    String path = String("/gifs/") + file;
    if (!SPIFFS.exists(path)) { _replyErr(id, "not found"); return; }
    SPIFFS.remove(path);
    _replyOk(id);
}

// ── WiFi commands (keep backwards compat with provisioning) ───────────────

static void _handleWifi(const String &id, JsonDocument &d, Preferences &prefs) {
    // Legacy path handled by wifi_manager; here we just surface an error
    // if WiFi was already configured via the old protocol.
    // The wifi_manager serial loop reads raw lines starting with '{' —
    // the USB API intercepts before wifi_manager, so we handle it here.
    const char *ssid = d["data"]["ssid"] | "";
    const char *pass = d["data"]["password"] | "";
    if (strlen(ssid) == 0) { _replyErr(id, "missing ssid"); return; }
    prefs.putString(PREF_SSID, ssid);
    prefs.putString(PREF_PASS, pass);
    // Actual connection is triggered by wifi_manager on next boot or reconnect.
    // For immediate connect we call WiFi directly:
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        wifi_set_connected(ip);          // update wifi_manager state
        _replyOk(id, "{\"ip\":\"" + ip + "\"}");
        // Also emit legacy "IP:" line so Flutter provisioning flow still works
        Serial.printf("IP:%s\n", ip.c_str());
    } else {
        _replyErr(id, "wifi_connect_failed");
    }
}

static void _handleGetIp(const String &id) {
    if (wifi_connected()) {
        _replyOk(id, "{\"ip\":\"" + wifi_ip() + "\"}");
        // Legacy compat
        Serial.printf("IP:%s\n", wifi_ip().c_str());
    } else {
        _replyErr(id, "not_connected");
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────

static void _dispatch(const String &line, Preferences &prefs) {
    // Fast bail — must start with '{' to be a command
    if (line.length() < 2 || line[0] != '{') return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err != DeserializationError::Ok) {
        // Might be legacy "IP:" or other raw serial — ignore silently
        return;
    }

    const char *id  = doc["id"]  | "";
    const char *cmd = doc["cmd"] | "";

    if (strlen(cmd) == 0) return; // not a command packet

    if      (strcmp(cmd, "ping")          == 0) _handlePing(id, doc);
    else if (strcmp(cmd, "status")        == 0) _handleStatus(id, doc);
    else if (strcmp(cmd, "set_mode")      == 0) _handleSetMode(id, doc, prefs);
    else if (strcmp(cmd, "set_brightness")== 0) _handleSetBrightness(id, doc, prefs);
    else if (strcmp(cmd, "clock_config")  == 0) _handleClockConfig(id, doc, prefs);
    else if (strcmp(cmd, "pomo_config")   == 0) _handlePomoConfig(id, doc, prefs);
    else if (strcmp(cmd, "pomo_cmd")      == 0) _handlePomoCmd(id, doc);
    else if (strcmp(cmd, "spotify_state") == 0) _handleSpotifyState(id, doc);
    else if (strcmp(cmd, "gif_start")     == 0) _handleGifStart(id, doc);
    else if (strcmp(cmd, "gif_chunk")     == 0) _handleGifChunk(id, doc);
    else if (strcmp(cmd, "gif_select")    == 0) _handleGifSelect(id, doc, prefs);
    else if (strcmp(cmd, "gif_list")      == 0) _handleGifList(id);
    else if (strcmp(cmd, "gif_delete")    == 0) _handleGifDelete(id, doc);
    else if (strcmp(cmd, "wifi")          == 0) _handleWifi(id, doc, prefs);
    else if (strcmp(cmd, "get_ip")        == 0) _handleGetIp(id);
    else {
        _reply(id, false, "", String("unknown command: ") + cmd);
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void usb_api_init(Preferences &prefs) {
    _buf.reserve(2048); // reserve for chunked GIF data
    Serial.printf("[USB] API ready — baud %d\n", SERIAL_BAUD);
}

void usb_api_loop(Preferences &prefs) {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            _buf.trim();
            if (_buf.length() > 0) {
                _dispatch(_buf, prefs);
            }
            _buf = "";
        } else {
            // Guard against absurdly large packets (malformed data)
            if (_buf.length() < 65535) _buf += c;
        }
    }
}

void usb_push(const String &event, const String &jsonData) {
    // Only write if USB host is connected (DTR line asserted)
    // On ESP32-S3 with native CDC, Serial is always "connected" once boot finishes.
    // This is a best-effort send.
    String out = "{\"push\":\"" + event + "\",\"data\":" + jsonData + "}\n";
    Serial.print(out);
}
