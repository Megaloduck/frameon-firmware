#pragma once
#include <Arduino.h>
#include <Preferences.h>

// ── USB API ───────────────────────────────────────────────────────────────
// Handles JSON command/response protocol over Serial (USB CDC).
// Commands arrive as single-line JSON terminated with \n.
// Responses are single-line JSON terminated with \n.
//
// Protocol:
//   Request:  {"id":"<uuid>","cmd":"<command>","data":{...}}\n
//   Response: {"id":"<uuid>","ok":true,"data":{...}}\n
//          or {"id":"<uuid>","ok":false,"error":"<message>"}\n
//   Push:     {"push":"<event>","data":{...}}\n   (ESP32-initiated)
//
// Commands (all accept optional "data" object):
//   wifi         — {"ssid":"...","password":"..."}     → connects WiFi
//   get_ip       — {}                                  → {"ip":"x.x.x.x"}
//   status       — {}                                  → full device status
//   set_mode     — {"mode":"clock"|"spotify"|"gif"|"pomodoro"}
//   set_brightness — {"value":0-255}
//   clock_config — {"format24h":bool,"showDate":bool,"showSeconds":bool,
//                   "timezone":"...","ntp":"...","brightness":int}
//   pomo_config  — {"work":int,"shortBreak":int,"longBreak":int,"sessions":int}
//   pomo_cmd     — {"cmd":"start"|"pause"|"reset"}
//   gif_start    — {"filename":"...","totalBytes":int,"chunks":int}
//   gif_chunk    — {"index":int,"data":"<base64>","final":bool}
//   gif_select   — {"file":"..."}
//   gif_list     — {}                                  → {"files":[...]}
//   gif_delete   — {"file":"..."}
//   spotify_state — {"track":"...","artist":"...","playing":bool}
//   ping         — {}                                  → {"pong":true}

// Call once in setup(), after Serial.begin()
void usb_api_init(Preferences &prefs);

// Call every loop() — drains Serial and dispatches commands
void usb_api_loop(Preferences &prefs);

// Push an event to the USB host (Flutter app)
// Only sends if a USB host is actually connected (DTR line check)
void usb_push(const String &event, const String &jsonData);
