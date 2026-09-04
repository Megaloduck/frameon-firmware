#pragma once

#include <Arduino.h>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Transport Abstraction Layer
// ─────────────────────────────────────────────────────────────────────────────
//
// Unified interface for USB Serial, TCP, and BLE packet delivery.
// Allows firmware to seamlessly support multiple transports simultaneously.
//
// Architecture:
//   - USB Serial (CDC)  — existing implementation, native USB
//   - TCP Server        — WiFi connectivity on port 5555
//   - BLE GATT          — Bluetooth LE with characteristic-based fragmentation
//
// Each transport:
//   1. Receives packets independently into shared packet buffer (pktBuf)
//   2. Validates and processes via processPacket()
//   3. Sends ACK/NAK/ERR responses
//   4. Handles disconnection gracefully

#undef SERIAL
enum class TransportType {
    SERIAL,    // USB CDC
    TCP,       // WiFi TCP server
    BLE        // Bluetooth LE GATT
};

struct TransportStats {
    uint32_t bytesReceived = 0;
    uint32_t bytesSent = 0;
    uint32_t packetsReceived = 0;
    uint32_t crcErrors = 0;
    bool isConnected = false;
    TransportType activeTransport = TransportType::SERIAL;
};

// Response codes (must match firmware protocol)
#define RESP_ACK 0x06
#define RESP_NAK 0x15
#define RESP_ERR 0x1B

// ─────────────────────────────────────────────────────────────────────────────
// Transport Initialization
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Initialize all available transports.
 * Called once from setup().
 * Does NOT block; transports initialize in background.
 */
void transportInit();

/**
 * Process inbound data from all active transports.
 * Called repeatedly from loop().
 * Handles USB serial, TCP, and BLE receive independently.
 */
void transportProcess();

/**
 * Send a response byte (ACK/NAK/ERR) to the currently active transport.
 * Auto-detects which transport sent the last packet.
 */
void transportSendResponse(uint8_t responseCode);

/**
 * Broadcast a debug/event line to all transports.
 * Used for EVT logging and debug output.
 */
void transportPrintfLn(const char* format, ...);

/**
 * Get connection status and statistics for all transports.
 */
TransportStats transportGetStats();

// ─────────────────────────────────────────────────────────────────────────────
// Transport-specific initialization
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Initialize USB CDC Serial (existing implementation).
 */
void transportInitSerial();

/**
 * Initialize WiFi + TCP server on port 5555.
 * Requires WiFi SSID and password configured.
 */
void transportInitTCP();

/**
 * Initialize BLE GATT server.
 * Advertises as "Frameon" over BLE.
 */
void transportInitBLE();

/**
 * Check if WiFi is connected and TCP server is listening.
 */
bool transportIsTCPActive();

/**
 * Check if BLE is connected and GATT client is ready.
 */
bool transportIsBLEActive();
