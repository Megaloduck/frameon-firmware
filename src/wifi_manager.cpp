#include "wifi_manager.h"

static bool     _connected    = false;
static String   _ip           = "";
static uint32_t _lastReconnect = 0;
static String   _serialBuf    = "";

// ── Internal helpers ──────────────────────────────────────────────────────

static bool _connect(const String &ssid, const String &pass) {
    Serial.printf("[WiFi] Connecting to: %s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_CONNECT_TIMEOUT) {
            Serial.println("[WiFi] Timeout");
            return false;
        }
        delay(200);
    }

    _ip        = WiFi.localIP().toString();
    _connected = true;
    Serial.printf("[WiFi] Connected. IP:%s\n", _ip.c_str());
    return true;
}

// Handle one complete JSON line from Serial.
// Protocol (matches Flutter app):
//   {"cmd":"wifi","ssid":"...","password":"..."}  → connect + respond IP:x.x.x.x
//   {"cmd":"get_ip"}                              → respond IP:x.x.x.x
static void _handleSerialCommand(const String &json, Preferences &prefs) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    const char *cmd = doc["cmd"] | "";

    if (strcmp(cmd, "wifi") == 0) {
        const char *ssid = doc["ssid"] | "";
        const char *pass = doc["password"] | "";

        if (strlen(ssid) == 0) {
            Serial.println("ERR:missing ssid");
            return;
        }

        // Persist credentials
        prefs.putString(PREF_SSID, ssid);
        prefs.putString(PREF_PASS, pass);

        if (_connect(ssid, pass)) {
            // Respond with IP — Flutter app listens for "IP:" prefix
            Serial.printf("IP:%s\n", _ip.c_str());
        } else {
            Serial.println("ERR:wifi_connect_failed");
        }

    } else if (strcmp(cmd, "get_ip") == 0) {
        if (_connected) {
            Serial.printf("IP:%s\n", _ip.c_str());
        } else {
            Serial.println("ERR:not_connected");
        }

    } else if (strcmp(cmd, "status") == 0) {
        Serial.printf("{\"connected\":%s,\"ip\":\"%s\",\"ssid\":\"%s\"}\n",
            _connected ? "true" : "false",
            _ip.c_str(),
            WiFi.SSID().c_str()
        );
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void wifi_init(Preferences &prefs) {
    // Try to connect with saved credentials on boot
    String ssid = prefs.getString(PREF_SSID, "");
    String pass = prefs.getString(PREF_PASS, "");

    if (ssid.length() > 0) {
        _connect(ssid, pass);
    } else {
        Serial.println("[WiFi] No saved credentials. Use Serial provisioning.");
    }
}

bool wifi_loop(Preferences &prefs) {
    bool newConnection = false;

    // ── Serial provisioning ─────────────────────────────────────────────
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            _serialBuf.trim();
            if (_serialBuf.startsWith("{")) {
                _handleSerialCommand(_serialBuf, prefs);
            }
            _serialBuf = "";
        } else {
            if (_serialBuf.length() < 512) {  // guard against overflow
                _serialBuf += c;
            }
        }
    }

    // ── Reconnect watchdog ──────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED && millis() - _lastReconnect > WIFI_RETRY_INTERVAL) {
        _lastReconnect = millis();
        _connected     = false;

        String ssid = prefs.getString(PREF_SSID, "");
        String pass = prefs.getString(PREF_PASS, "");

        if (ssid.length() > 0) {
            Serial.println("[WiFi] Reconnecting...");
            if (_connect(ssid, pass)) {
                newConnection = true;
            }
        }
    } else if (WiFi.status() == WL_CONNECTED && !_connected) {
        // Recovered without our watchdog (e.g. router came back)
        _ip        = WiFi.localIP().toString();
        _connected = true;
        newConnection = true;
    }

    return newConnection;
}

bool wifi_connected() { return _connected; }
String wifi_ip()      { return _ip; }
