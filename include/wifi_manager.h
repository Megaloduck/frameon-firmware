#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

// Call once in setup() — reads saved credentials and connects if available.
void wifi_init(Preferences &prefs);

// Call in loop() — handles reconnection and Serial provisioning commands.
// Returns true if a new IP was assigned (useful for updating display).
bool wifi_loop(Preferences &prefs);

// Returns true if currently connected.
bool wifi_connected();

// Current IP as String (empty if not connected).
String wifi_ip();
