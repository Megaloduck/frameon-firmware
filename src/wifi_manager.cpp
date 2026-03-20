#include "wifi_manager.h"

static bool     _connected     = false;
static String   _ip            = "";
static uint32_t _lastReconnect = 0;

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

// ── Public API ────────────────────────────────────────────────────────────

void wifi_init(Preferences &prefs) {
    String ssid = prefs.getString(PREF_SSID, "");
    String pass = prefs.getString(PREF_PASS, "");
    if (ssid.length() > 0) {
        _connect(ssid, pass);
    } else {
        Serial.println("[WiFi] No saved credentials — use USB to provision");
    }
}

// NOTE: Serial reading has been removed from wifi_loop().
// usb_api_loop() in main.cpp now owns the Serial port and handles
// all commands including "wifi" provisioning and "get_ip".
// wifi_loop() only handles reconnection logic.
bool wifi_loop(Preferences &prefs) {
    bool newConnection = false;

    // ── Reconnect watchdog ──────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED && millis() - _lastReconnect > WIFI_RETRY_INTERVAL) {
        _lastReconnect = millis();
        _connected     = false;

        String ssid = prefs.getString(PREF_SSID, "");
        String pass = prefs.getString(PREF_PASS, "");
        if (ssid.length() > 0) {
            Serial.println("[WiFi] Reconnecting...");
            if (_connect(ssid, pass)) newConnection = true;
        }
    } else if (WiFi.status() == WL_CONNECTED && !_connected) {
        _ip           = WiFi.localIP().toString();
        _connected    = true;
        newConnection = true;
    }

    return newConnection;
}

// Called by usb_api.cpp _handleWifi() after connecting via USB command
void wifi_set_connected(const String &ip) {
    _ip        = ip;
    _connected = true;
}

bool   wifi_connected() { return _connected; }
String wifi_ip()        { return _ip; }
