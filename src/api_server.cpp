#include "api_server.h"
#include "matrix.h"
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <WiFi.h>

// ── Globals ───────────────────────────────────────────────────────────────
AppMode g_mode       = MODE_CLOCK;
uint8_t g_brightness = PANEL_BRIGHTNESS;

// ── Spotify state (pushed from Flutter app) ───────────────────────────────
struct SpotifyState {
    String  track;
    String  artist;
    bool    isPlaying = false;
    uint8_t artJpeg[64 * 32 * 3]; // decoded RGB888, 64×32 px
    bool    hasArt = false;
} g_spotify;

// ── Pomodoro config ───────────────────────────────────────────────────────
struct PomodoroConfig {
    int  workMinutes        = 25;
    int  shortBreakMinutes  = 5;
    int  longBreakMinutes   = 15;
    int  sessionsBeforeLong = 4;
    bool alertOnComplete    = true;
} g_pomoCfg;

enum PomodoroPhase { POMO_WORK, POMO_SHORT_BREAK, POMO_LONG_BREAK };
struct PomodoroTimer {
    PomodoroPhase phase    = POMO_WORK;
    int  secondsRemaining  = 25 * 60;
    int  sessionsCompleted = 0;
    bool running           = false;
    uint32_t lastTick      = 0;
} g_pomoTimer;

// ── Clock config ──────────────────────────────────────────────────────────
struct ClockConfig {
    bool   is24h       = true;
    bool   showDate    = true;
    bool   showSeconds = false;
    String timezone    = "UTC0";
    String ntpServer   = "pool.ntp.org";
} g_clockCfg;

// ── Server instances ──────────────────────────────────────────────────────
static AsyncWebServer  _server(API_PORT);
static AsyncWebSocket  _ws(WS_PATH);
static Preferences    *_prefs = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────

static void _sendJson(AsyncWebServerRequest *req, int code, const String &json) {
    req->send(code, "application/json", json);
}

static void _ok(AsyncWebServerRequest *req) {
    _sendJson(req, 200, "{\"ok\":true}");
}

static String _getBody(AsyncWebServerRequest *req) {
    // Body already collected via onBody — stored in request params
    if (req->hasParam("_body", true)) {
        return req->getParam("_body", true)->value();
    }
    return "";
}

// ── WebSocket ─────────────────────────────────────────────────────────────

static void _onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        // Send current state immediately on connect
        JsonDocument doc;
        doc["mode"]       = (int)g_mode;
        doc["brightness"] = g_brightness;
        String out;
        serializeJson(doc, out);
        client->text(out);
    }
}

void api_ws_broadcast(const String &json) {
    _ws.textAll(json);
}

// ── Route handlers ────────────────────────────────────────────────────────

// GET /api/ping
static void _handlePing(AsyncWebServerRequest *req) {
    _sendJson(req, 200, "{\"pong\":true}");
}

// GET /api/info
static void _handleInfo(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["name"]       = "Frameon Matrix";
    doc["mode"]       = (int)g_mode;
    doc["brightness"] = g_brightness;
    doc["ip"]         = WiFi.localIP().toString();
    doc["fw"]         = "1.0.0";
    String out;
    serializeJson(doc, out);
    _sendJson(req, 200, out);
}

// POST /api/mode  {"mode":"clock"}
static void _handleSetMode(AsyncWebServerRequest *req, JsonDocument &body) {
    const char *modeStr = body["mode"] | "clock";
    if      (strcmp(modeStr, "clock")    == 0) g_mode = MODE_CLOCK;
    else if (strcmp(modeStr, "spotify")  == 0) g_mode = MODE_SPOTIFY;
    else if (strcmp(modeStr, "gif")      == 0) g_mode = MODE_GIF;
    else if (strcmp(modeStr, "pomodoro") == 0) g_mode = MODE_POMODORO;

    if (_prefs) _prefs->putInt(PREF_MODE, (int)g_mode);

    // Notify WebSocket clients
    String ws;
    JsonDocument wsDoc;
    wsDoc["mode"] = modeStr;
    serializeJson(wsDoc, ws);
    api_ws_broadcast(ws);

    _ok(req);
}

// POST /api/brightness  {"value":128}
static void _handleBrightness(AsyncWebServerRequest *req, JsonDocument &body) {
    int v = body["value"] | PANEL_BRIGHTNESS;
    v = constrain(v, 0, 255);
    g_brightness = (uint8_t)v;
    matrix_brightness(g_brightness);
    if (_prefs) _prefs->putInt(PREF_BRIGHTNESS, g_brightness);
    _ok(req);
}

// POST /api/clock/config
static void _handleClockConfig(AsyncWebServerRequest *req, JsonDocument &body) {
    if (!body["format24h"].isNull())   g_clockCfg.is24h       = body["format24h"].as<bool>();
    if (!body["showDate"].isNull())    g_clockCfg.showDate    = body["showDate"].as<bool>();
    if (!body["showSeconds"].isNull()) g_clockCfg.showSeconds = body["showSeconds"].as<bool>();
    if (!body["timezone"].isNull())    g_clockCfg.timezone    = body["timezone"].as<String>();
    if (!body["ntp"].isNull())         g_clockCfg.ntpServer   = body["ntp"].as<String>();
    if (!body["brightness"].isNull()) {
        g_brightness = constrain((int)body["brightness"], 0, 255);
        matrix_brightness(g_brightness);
    }

    // Apply timezone immediately
    setenv("TZ", g_clockCfg.timezone.c_str(), 1);
    tzset();

    if (_prefs) {
        _prefs->putBool  (PREF_CLOCK_24H,  g_clockCfg.is24h);
        _prefs->putBool  (PREF_CLOCK_DATE, g_clockCfg.showDate);
        _prefs->putBool  (PREF_CLOCK_SECS, g_clockCfg.showSeconds);
        _prefs->putString(PREF_TIMEZONE,   g_clockCfg.timezone);
        _prefs->putString(PREF_NTP_SERVER, g_clockCfg.ntpServer);
    }
    _ok(req);
}

// POST /api/spotify/state
static void _handleSpotifyState(AsyncWebServerRequest *req, JsonDocument &body) {
    g_spotify.track    = body["track"]  | "";
    g_spotify.artist   = body["artist"] | "";
    g_spotify.isPlaying = body["playing"] | false;

    // Decode base64 JPEG art if present
    if (!body["art"].isNull()) {
        // Art is base64-encoded JPEG from Flutter — store raw for display module
        // Full decode happens in the spotify display module to keep this file lean
        g_spotify.hasArt = true;
        // Store the base64 string temporarily — display module decodes it
        // We just signal that new art is available
    }
    _ok(req);
}

// POST /api/spotify/cmd  {"cmd":"play"|"pause"|"next"|"prev"}
static void _handleSpotifyCmd(AsyncWebServerRequest *req, JsonDocument &body) {
    // Commands are informational — Flutter app controls Spotify directly.
    // ESP32 just updates its local playing state to match.
    const char *cmd = body["cmd"] | "";
    if (strcmp(cmd, "pause") == 0) g_spotify.isPlaying = false;
    if (strcmp(cmd, "play")  == 0) g_spotify.isPlaying = true;
    _ok(req);
}

// POST /api/pomodoro/config
static void _handlePomodoroConfig(AsyncWebServerRequest *req, JsonDocument &body) {
    if (!body["work"].isNull())      g_pomoCfg.workMinutes       = body["work"];
    if (!body["shortBreak"].isNull()) g_pomoCfg.shortBreakMinutes = body["shortBreak"];
    if (!body["longBreak"].isNull()) g_pomoCfg.longBreakMinutes  = body["longBreak"];
    if (!body["sessions"].isNull())  g_pomoCfg.sessionsBeforeLong = body["sessions"];

    if (_prefs) {
        _prefs->putInt(PREF_POMO_WORK,     g_pomoCfg.workMinutes);
        _prefs->putInt(PREF_POMO_SHORT,    g_pomoCfg.shortBreakMinutes);
        _prefs->putInt(PREF_POMO_LONG,     g_pomoCfg.longBreakMinutes);
        _prefs->putInt(PREF_POMO_SESSIONS, g_pomoCfg.sessionsBeforeLong);
    }
    _ok(req);
}

// POST /api/pomodoro/cmd  {"cmd":"start"|"pause"|"reset"}
static void _handlePomodoroCmd(AsyncWebServerRequest *req, JsonDocument &body) {
    const char *cmd = body["cmd"] | "";
    if (strcmp(cmd, "start") == 0) {
        g_pomoTimer.running  = true;
        g_pomoTimer.lastTick = millis();
    } else if (strcmp(cmd, "pause") == 0) {
        g_pomoTimer.running = false;
    } else if (strcmp(cmd, "reset") == 0) {
        g_pomoTimer.running          = false;
        g_pomoTimer.phase            = POMO_WORK;
        g_pomoTimer.secondsRemaining = g_pomoCfg.workMinutes * 60;
        g_pomoTimer.sessionsCompleted = 0;
    }
    _ok(req);
}

// GET /api/gif/list
static void _handleGifList(AsyncWebServerRequest *req) {
    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();
    // SPIFFS scan — list .gif files
    File root = SPIFFS.open("/gifs");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                JsonObject entry = files.add<JsonObject>();
                entry["name"]    = String(f.name()).substring(6); // strip "/gifs/"
                entry["size"]    = f.size();
                entry["playing"] = false; // TODO: track active file
            }
            f = root.openNextFile();
        }
    }
    String out;
    serializeJson(doc, out);
    _sendJson(req, 200, out);
}

// POST /api/gif/select  {"file":"name.gif"}
static void _handleGifSelect(AsyncWebServerRequest *req, JsonDocument &body) {
    // Signal the GIF display module to switch files
    // (handled by g_mode + file name stored in preferences)
    String filename = body["file"] | "";
    if (_prefs) _prefs->putString("gif_file", filename);
    g_mode = MODE_GIF;
    _ok(req);
}

// DELETE /api/gif/delete  {"file":"name.gif"}
static void _handleGifDelete(AsyncWebServerRequest *req, JsonDocument &body) {
    String filename = String("/gifs/") + (body["file"] | "");
    if (SPIFFS.exists(filename)) {
        SPIFFS.remove(filename);
        _ok(req);
    } else {
        _sendJson(req, 404, "{\"error\":\"not found\"}");
    }
}

// ── Body accumulation helper ──────────────────────────────────────────────
// ESPAsyncWebServer delivers body in chunks; we accumulate and parse JSON.

struct BodyContext {
    uint8_t *data;
    size_t   len;
};

static void _registerJsonRoute(
    const char *path,
    WebRequestMethodComposite method,
    std::function<void(AsyncWebServerRequest*, JsonDocument&)> handler
) {
    _server.on(path, method,
        // Request handler — called after body is complete
        [handler](AsyncWebServerRequest *req) {
            if (req->_tempObject) {
                auto *ctx = (BodyContext *)req->_tempObject;
                JsonDocument doc;
                if (deserializeJson(doc, ctx->data, ctx->len) == DeserializationError::Ok) {
                    handler(req, doc);
                } else {
                    req->send(400, "application/json", "{\"error\":\"bad json\"}");
                }
                delete[] ctx->data;
                delete ctx;
                req->_tempObject = nullptr;
            } else {
                JsonDocument empty;
                handler(req, empty);
            }
        },
        // Upload handler (not used for JSON routes)
        nullptr,
        // Body handler — accumulate chunks
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len,
           size_t index, size_t total) {
            if (index == 0) {
                auto *ctx  = new BodyContext;
                ctx->data  = new uint8_t[total + 1];
                ctx->len   = total;
                req->_tempObject = ctx;
            }
            if (req->_tempObject) {
                auto *ctx = (BodyContext *)req->_tempObject;
                memcpy(ctx->data + index, data, len);
                if (index + len == total) {
                    ctx->data[total] = 0; // null-terminate for ArduinoJson
                }
            }
        }
    );
}

// ── GIF upload (multipart) ────────────────────────────────────────────────

static File    _uploadFile;
static String  _uploadPath;

static void _handleGifUpload(AsyncWebServerRequest *req, String filename,
                              size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        _uploadPath = String("/gifs/") + filename;
        SPIFFS.mkdir("/gifs");
        _uploadFile = SPIFFS.open(_uploadPath, FILE_WRITE);
        Serial.printf("[GIF] Upload start: %s\n", _uploadPath.c_str());
    }
    if (_uploadFile) {
        _uploadFile.write(data, len);
    }
    if (final) {
        _uploadFile.close();
        Serial.printf("[GIF] Upload done: %u bytes\n", index + len);
    }
}

// ── Init ──────────────────────────────────────────────────────────────────

void api_init(Preferences &prefs) {
    _prefs = &prefs;

    // Load persisted config
    g_brightness             = prefs.getInt   (PREF_BRIGHTNESS,    PANEL_BRIGHTNESS);
    g_mode                   = (AppMode)prefs.getInt(PREF_MODE, MODE_CLOCK);
    g_clockCfg.is24h         = prefs.getBool  (PREF_CLOCK_24H,  true);
    g_clockCfg.showDate      = prefs.getBool  (PREF_CLOCK_DATE, true);
    g_clockCfg.showSeconds   = prefs.getBool  (PREF_CLOCK_SECS, false);
    g_clockCfg.timezone      = prefs.getString(PREF_TIMEZONE,   "UTC0");
    g_clockCfg.ntpServer     = prefs.getString(PREF_NTP_SERVER, "pool.ntp.org");
    g_pomoCfg.workMinutes    = prefs.getInt   (PREF_POMO_WORK,    25);
    g_pomoCfg.shortBreakMinutes = prefs.getInt(PREF_POMO_SHORT,   5);
    g_pomoCfg.longBreakMinutes  = prefs.getInt(PREF_POMO_LONG,   15);
    g_pomoCfg.sessionsBeforeLong = prefs.getInt(PREF_POMO_SESSIONS, 4);

    // Apply saved timezone
    setenv("TZ", g_clockCfg.timezone.c_str(), 1);
    tzset();

    matrix_brightness(g_brightness);

    // ── WebSocket ──────────────────────────────────────────────────────
    _ws.onEvent(_onWsEvent);
    _server.addHandler(&_ws);

    // ── CORS for browser access ────────────────────────────────────────
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin",  "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    _server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest *req) {
        req->send(200);
    });

    // ── GET routes ─────────────────────────────────────────────────────
    _server.on("/api/ping", HTTP_GET, _handlePing);
    _server.on("/api/info", HTTP_GET, _handleInfo);
    _server.on("/api/gif/list", HTTP_GET, _handleGifList);

    // ── POST routes (JSON body) ────────────────────────────────────────
    _registerJsonRoute("/api/mode",            HTTP_POST, _handleSetMode);
    _registerJsonRoute("/api/brightness",      HTTP_POST, _handleBrightness);
    _registerJsonRoute("/api/clock/config",    HTTP_POST, _handleClockConfig);
    _registerJsonRoute("/api/spotify/state",   HTTP_POST, _handleSpotifyState);
    _registerJsonRoute("/api/spotify/cmd",     HTTP_POST, _handleSpotifyCmd);
    _registerJsonRoute("/api/pomodoro/config", HTTP_POST, _handlePomodoroConfig);
    _registerJsonRoute("/api/pomodoro/cmd",    HTTP_POST, _handlePomodoroCmd);
    _registerJsonRoute("/api/gif/select",      HTTP_POST, _handleGifSelect);

    // DELETE with body
    _registerJsonRoute("/api/gif/delete", HTTP_DELETE, _handleGifDelete);

    // ── GIF upload (multipart) ─────────────────────────────────────────
    _server.on("/api/gif/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) { req->send(200); },
        _handleGifUpload
    );

    // ── ElegantOTA ─────────────────────────────────────────────────────
    ElegantOTA.begin(&_server);
    // OTA available at http://<device-ip>/update

    _server.begin();
    Serial.printf("[API] Server started on port %d\n", API_PORT);
    Serial.printf("[OTA] ElegantOTA at http://%s/update\n",
                  WiFi.localIP().toString().c_str());
}

void api_loop() {
    ElegantOTA.loop();
    _ws.cleanupClients();

    // ── Pomodoro tick ──────────────────────────────────────────────────
    if (g_pomoTimer.running && millis() - g_pomoTimer.lastTick >= 1000) {
        g_pomoTimer.lastTick = millis();

        if (g_pomoTimer.secondsRemaining > 0) {
            g_pomoTimer.secondsRemaining--;
        } else {
            // Phase complete — advance
            if (g_pomoTimer.phase == POMO_WORK) {
                g_pomoTimer.sessionsCompleted++;
                bool longBreak = (g_pomoTimer.sessionsCompleted % g_pomoCfg.sessionsBeforeLong == 0);
                g_pomoTimer.phase = longBreak ? POMO_LONG_BREAK : POMO_SHORT_BREAK;
                g_pomoTimer.secondsRemaining = longBreak
                    ? g_pomoCfg.longBreakMinutes  * 60
                    : g_pomoCfg.shortBreakMinutes * 60;
            } else {
                g_pomoTimer.phase            = POMO_WORK;
                g_pomoTimer.secondsRemaining = g_pomoCfg.workMinutes * 60;
            }
            g_pomoTimer.running = false; // stop — wait for user to start next

            // Notify Flutter app via WebSocket
            JsonDocument wsDoc;
            wsDoc["pomoDone"] = true;
            wsDoc["phase"]    = (int)g_pomoTimer.phase;
            String ws;
            serializeJson(wsDoc, ws);
            api_ws_broadcast(ws);
        }
    }
}
