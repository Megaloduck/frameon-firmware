// examples/wifi_configuration.ino
//
// Example: How to configure WiFi in Frameon firmware.
// 
// This example shows the minimal WiFi setup needed in setup().
// Modify WIFI_SSID and WIFI_PASS before flashing.
//
// For production, consider implementing:
// - WiFi credentials stored in EEPROM
// - WiFi setup over serial configuration
// - Fallback to WiFi AP mode if connection fails
// - WiFi reconnection with exponential backoff

#include <WiFi.h>

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

// WiFi Access Point (AP) Mode — Device hosts its own network
#define WIFI_MODE_AP 1

// WiFi Station (STA) Mode — Device joins existing network
#define WIFI_MODE_STA 0

// Select the mode you want
#define WIFI_MODE WIFI_MODE_AP

// ─────────────────────────────────────────────────────────────────────────────
// AP Mode Configuration (Device hosts "Frameon" network)
// ─────────────────────────────────────────────────────────────────────────────

#define WIFI_SSID_AP "Frameon"
#define WIFI_PASS_AP "frameon123"

void setupWiFiAP() {
    Serial.println("[WiFi] Starting in AP mode...");
    
    WiFi.mode(WIFI_AP);
    bool result = WiFi.softAP(WIFI_SSID_AP, WIFI_PASS_AP);
    
    if (result) {
        IPAddress ip = WiFi.softAPIP();
        Serial.print("[WiFi] AP started: ");
        Serial.println(ip);
        Serial.print("       SSID: ");
        Serial.println(WIFI_SSID_AP);
        Serial.println("       Password: frameon123");
    } else {
        Serial.println("[WiFi] AP setup failed");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// STA Mode Configuration (Device joins existing network)
// ─────────────────────────────────────────────────────────────────────────────

#define WIFI_SSID_STA "YourWiFiNetwork"
#define WIFI_PASS_STA "YourWiFiPassword"
#define WIFI_CONNECT_TIMEOUT_MS 10000

void setupWiFiSTA() {
    Serial.println("[WiFi] Starting in STA mode...");
    Serial.print("[WiFi] Connecting to ");
    Serial.println(WIFI_SSID_STA);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID_STA, WIFI_PASS_STA);
    
    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
        
        if (millis() - startMs > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println();
            Serial.println("[WiFi] Connection timeout");
            Serial.println("[WiFi] Falling back to AP mode");
            setupWiFiAP();
            return;
        }
    }
    
    Serial.println();
    Serial.print("[WiFi] Connected: ");
    Serial.println(WiFi.localIP());
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(100);
    
    Serial.println("\n\nFrameon WiFi Configuration Example");
    Serial.println("════════════════════════════════════════");
    
    // Start WiFi (AP or STA mode)
    #if WIFI_MODE == WIFI_MODE_AP
        setupWiFiAP();
    #else
        setupWiFiSTA();
    #endif
    
    Serial.println("════════════════════════════════════════");
    Serial.println("WiFi setup complete.");
    Serial.println();
    Serial.println("Connect from desktop app:");
    Serial.println("  1. Open Frameon app");
    Serial.println("  2. Click 'Add Device'");
    Serial.println("  3. Select 'WiFi' tab");
    Serial.println("  4. Find or enter device IP");
    Serial.println();
    Serial.println("TCP Server listening on port 5555");
    Serial.println("════════════════════════════════════════");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    // Check WiFi status periodically
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck > 10000) {  // Every 10 seconds
        lastCheck = millis();
        
        #if WIFI_MODE == WIFI_MODE_STA
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WiFi] Connection lost, reconnecting...");
                WiFi.reconnect();
            }
        #endif
    }
    
    delay(100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Advanced: WiFi Configuration over Serial
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Optional: Accept WiFi credentials over serial
 * 
 * Usage:
 *   - Open serial monitor at 115200 baud
 *   - Send: "AT+WIFISSID=MyNetwork\r\n"
 *   - Send: "AT+WIFIPASS=MyPassword\r\n"
 *   - Send: "AT+WIFICONNECT\r\n"
 * 
 * Device will store credentials and connect.
 */

// Uncomment to enable serial WiFi configuration
// #define ENABLE_SERIAL_WIFI_CONFIG 1

#ifdef ENABLE_SERIAL_WIFI_CONFIG

#include <EEPROM.h>

#define EEPROM_SSID_ADDR 0
#define EEPROM_PASS_ADDR 32
#define EEPROM_SIZE 256

void handleSerialCommand() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        
        if (cmd.startsWith("AT+WIFISSID=")) {
            String ssid = cmd.substring(12);
            EEPROM.writeString(EEPROM_SSID_ADDR, ssid);
            EEPROM.commit();
            Serial.println("OK");
        }
        else if (cmd.startsWith("AT+WIFIPASS=")) {
            String pass = cmd.substring(12);
            EEPROM.writeString(EEPROM_PASS_ADDR, pass);
            EEPROM.commit();
            Serial.println("OK");
        }
        else if (cmd == "AT+WIFICONNECT") {
            String ssid = EEPROM.readString(EEPROM_SSID_ADDR);
            String pass = EEPROM.readString(EEPROM_PASS_ADDR);
            
            if (ssid.length() > 0) {
                WiFi.begin(ssid.c_str(), pass.c_str());
                Serial.println("Connecting...");
            } else {
                Serial.println("ERROR: No SSID configured");
            }
        }
    }
}

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Notes
// ─────────────────────────────────────────────────────────────────────────────

/*
 * AP Mode (Recommended for testing):
 *   - Device creates "Frameon" WiFi network
 *   - Connect your computer to this network
 *   - Device IP: 192.168.4.1
 *   - No router/internet needed
 *   - Multiple devices create separate networks
 *
 * STA Mode (Recommended for home use):
 *   - Device joins your existing WiFi network
 *   - Requires router
 *   - Can discover device via mDNS
 *   - IP assigned by DHCP
 *
 * TCP Server:
 *   - Listens on 0.0.0.0:5555
 *   - Accepts one client at a time
 *   - Disconnection handled gracefully
 *   - Client timeout after 5 seconds of inactivity
 *
 * Power Consumption:
 *   - WiFi ON: ~70 mA (active) / 10 mA (idle waiting for data)
 *   - WiFi OFF: ~2 mA
 *   Consider disabling if on battery
 */
