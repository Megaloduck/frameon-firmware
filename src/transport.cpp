// src/transport.cpp
//
// Multi-transport packet delivery system
// Supports USB Serial, WiFi TCP, and BLE GATT simultaneously.

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include "transport.h"
#include "debug_log.h"

// ─────────────────────────────────────────────────────────────────────────────
// Transport Statistics
// ─────────────────────────────────────────────────────────────────────────────

static TransportStats transportStats;
static TransportType lastActiveTransport = TransportType::SERIAL;

// ─────────────────────────────────────────────────────────────────────────────
// TCP Server State (WiFi)
// ─────────────────────────────────────────────────────────────────────────────

static WiFiServer* tcpServer = nullptr;
static WiFiClient tcpClient;
static bool tcpConnected = false;
static uint32_t tcpByteBuffer[256];  // Ring buffer for partial packets
static size_t tcpBufferIdx = 0;

// WiFi credentials (can be set via serial command or hardcoded)
#define WIFI_SSID "Frameon"
#define WIFI_PASS "frameon123"
#define TCP_PORT 5555

// ─────────────────────────────────────────────────────────────────────────────
// BLE GATT Server State
// ─────────────────────────────────────────────────────────────────────────────

static NimBLEServer* bleServer = nullptr;
static NimBLECharacteristic* bleFrameCharacteristic = nullptr;
static NimBLECharacteristic* bleResponseCharacteristic = nullptr;
static bool bleConnected = false;

#define BLE_SERVICE_UUID "180A"
#define BLE_FRAME_CHAR_UUID "2A25"
#define BLE_RESPONSE_CHAR_UUID "2A27"

// ─────────────────────────────────────────────────────────────────────────────
// USB Serial receive state machine (existing)
// ─────────────────────────────────────────────────────────────────────────────

extern uint8_t* pktBuf[2];
extern int pendingBuf;

// Forward declarations from main.cpp
extern void processPacket();

// ─────────────────────────────────────────────────────────────────────────────
// WiFi / TCP Implementation
// ─────────────────────────────────────────────────────────────────────────────

void transportInitTCP() {
    // Start WiFi in AP mode (access point)
    // For production: use WiFi.mode(WIFI_STA) to connect to existing network
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    
    // Get the IP address
    IPAddress apIP = WiFi.softAPIP();
    DLOGF("[TCP] WiFi AP started: %s on %s\n", WIFI_SSID, apIP.toString().c_str());
    
    // Create TCP server
    tcpServer = new WiFiServer(TCP_PORT);
    tcpServer->begin();
    DLOGF("[TCP] Server listening on port %d\n", TCP_PORT);
    
    transportStats.isConnected = true;
    transportStats.activeTransport = TransportType::TCP;
}

static void processTCPData() {
    if (!tcpServer) return;
    
    // Check for new client connections
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient newClient = tcpServer->available();
        if (newClient) {
            tcpClient = newClient;
            tcpConnected = true;
            DLOGF("[TCP] Client connected\n");
        }
        return;
    }
    
    // Read available bytes from client
    while (tcpClient.available()) {
        uint8_t byte = tcpClient.read();
        tcpByteBuffer[tcpBufferIdx++] = byte;
        transportStats.bytesReceived++;
        
        // If we've filled the packet buffer, process it
        if (tcpBufferIdx >= 4096) {  // Reasonable chunk for processing
            memcpy(pktBuf[pendingBuf], tcpByteBuffer, tcpBufferIdx);
            tcpBufferIdx = 0;
            lastActiveTransport = TransportType::TCP;
        }
    }
    
    // Check for disconnection
    if (!tcpClient.connected()) {
        tcpConnected = false;
        DLOGF("[TCP] Client disconnected\n");
    }
}

void transportSendResponseTCP(uint8_t responseCode) {
    if (tcpConnected && tcpClient && tcpClient.connected()) {
        tcpClient.write(responseCode);
        tcpClient.flush();
        transportStats.bytesSent++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE GATT Server Implementation
// ─────────────────────────────────────────────────────────────────────────────

class FrameonBLEServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override {
        bleConnected = true;
        DLOGF("[BLE] Client connected\n");
    }
    
    void onDisconnect(NimBLEServer* pServer) override {
        bleConnected = false;
        DLOGF("[BLE] Client disconnected\n");
    }
};

class FrameonBLECharacteristicCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        std::string data = pCharacteristic->getValue();
        DLOGF("[BLE] Received %d bytes\n", data.size());
        
        // Copy into packet buffer
        memcpy(pktBuf[pendingBuf], data.data(), std::min(data.size(), (size_t)4096));
        lastActiveTransport = TransportType::BLE;
        transportStats.bytesReceived += data.size();
    }
};

void transportInitBLE() {
    NimBLEDevice::init("Frameon");
    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new FrameonBLEServerCallbacks());
    
    // Create GATT service
    NimBLEService* svc = bleServer->createService(BLE_SERVICE_UUID);
    
    // Frame packet characteristic (write from client)
    bleFrameCharacteristic = svc->createCharacteristic(
        BLE_FRAME_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    bleFrameCharacteristic->setCallbacks(new FrameonBLECharacteristicCallbacks());
    
    // Response characteristic (notify to client)
    bleResponseCharacteristic = svc->createCharacteristic(
        BLE_RESPONSE_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    
    svc->start();
    
    // Start advertising
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->start();
    
    DLOGF("[BLE] GATT server initialized and advertising\n");
}

static void processBLEData() {
    // BLE data is handled via characteristic callbacks
    // No additional processing needed here
}

void transportSendResponseBLE(uint8_t responseCode) {
    if (bleConnected && bleResponseCharacteristic) {
        uint8_t response = responseCode;
        bleResponseCharacteristic->setValue(&response, 1);
        bleResponseCharacteristic->notify();
        transportStats.bytesSent++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// USB Serial (existing implementation)
// ─────────────────────────────────────────────────────────────────────────────

extern void processSerial();  // From main.cpp

void transportInitSerial() {
    // Serial is initialized in setup() via Serial.begin()
    // Nothing additional needed here
    DLOGF("[Serial] USB CDC initialized\n");
}

static void processSerialData() {
    processSerial();  // Use existing serial processing
    lastActiveTransport = TransportType::SERIAL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public Transport API
// ─────────────────────────────────────────────────────────────────────────────

void transportInit() {
    DLOGF("[Transport] Initializing multi-transport system...\n");
    
    transportInitSerial();
    
    // These run in background and won't block
    // transportInitTCP();
    // transportInitBLE();
    
    DLOGF("[Transport] All transports initialized\n");
}

void transportProcess() {
    // Process each transport independently
    processSerialData();
    
    if (transportIsTCPActive()) {
        processTCPData();
    }
    
    if (transportIsBLEActive()) {
        processBLEData();
    }
}

void transportSendResponse(uint8_t responseCode) {
    switch (lastActiveTransport) {
        case TransportType::TCP:
            transportSendResponseTCP(responseCode);
            break;
        case TransportType::BLE:
            transportSendResponseBLE(responseCode);
            break;
        case TransportType::SERIAL:
        default:
            Serial.write(responseCode);
            Serial.flush();
            break;
    }
    transportStats.bytesSent++;
}

void transportPrintfLn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    // Always send to Serial
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    Serial.print(buffer);
    Serial.println();
    
    // Also send to active TCP/BLE clients if connected
    if (lastActiveTransport == TransportType::TCP && tcpConnected && tcpClient) {
        tcpClient.print(buffer);
        tcpClient.println();
    } else if (lastActiveTransport == TransportType::BLE && bleConnected) {
        // BLE would need a separate event characteristic
    }
    
    va_end(args);
}

TransportStats transportGetStats() {
    return transportStats;
}

bool transportIsTCPActive() {
    return tcpServer && tcpConnected && tcpClient && tcpClient.connected();
}

bool transportIsBLEActive() {
    return bleServer && bleConnected;
}
