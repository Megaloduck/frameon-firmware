#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

// Call once in setup() — reads saved credentials and connects if available.
void wifi_init(Preferences &prefs);

// Call in loop() — handles reconnection. Serial reading removed (usb_api owns Serial).
// Returns true if a new IP was assigned.
bool wifi_loop(Preferences &prefs);

// Called by usb_api.cpp after a successful USB-provisioned WiFi connect.
void wifi_set_connected(const String &ip);

// Returns true if currently connected.
bool wifi_connected();

// Current IP as String (empty if not connected).
String wifi_ip();
