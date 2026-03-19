#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "config.h"

// Initialise HTTP server, WebSocket, and ElegantOTA.
// Call once after Wi-Fi connects.
void api_init(Preferences &prefs);

// Call in loop() — drives ElegantOTA and WebSocket cleanup.
void api_loop();

// Push current state to all connected WebSocket clients.
void api_ws_broadcast(const String &json);

// Current active mode (read by display modules).
extern AppMode g_mode;

// Current brightness (0–255).
extern uint8_t g_brightness;
