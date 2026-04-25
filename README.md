# 1/32 scan mode panel settings
/*
 * Frameon Firmware v1.1
 * ESP32-S3-N16R8  ·  P4-2121-64×32 HUB75E
 *
 * Receives pre-rendered RGB565 frame packets from the Frameon desktop app
 * over USB Serial, validates them, and plays them back on the LED matrix
 * in a seamless loop — independent of further host communication.
 *
 * Architecture
 * ────────────
 *   Core 0  displayTask  — continuously renders the active frame buffer to the
 *                          matrix, advancing one frame per frameDurationMs.
 *
 *   Core 1  loop()       — runs the serial receive state machine; when a
 *                          complete, valid packet arrives it swaps the pending
 *                          buffer into the active slot under a mutex.
 *
 * PSRAM usage (2 × ~1.2 MB double buffer)
 * ─────────────────────────────────────────
 *   pktBuf[0], pktBuf[1]: alternating receive / display buffers.
 *   Layout of each buffer:
 *     [ header 16 B ][ frame_0 4096 B ][ frame_1 ]…[ frame_N-1 ][ crc 2 B ]
 *   Frames are read directly from the buffer — no extra copy needed.
 *
 * Protocol (matches Frameon's FrameExporter — frame_exporter.dart)
 * ──────────────────────────────────────────────────────────────────
 *   Header (16 B, all multi-byte fields big-endian):
 *     [0-2]   "FRM" magic
 *     [3]     version 0x01
 *     [4-5]   frame count  (uint16 BE)
 *     [6-7]   width        (uint16 BE)  must be 64
 *     [8-9]   height       (uint16 BE)  must be 32
 *     [10-11] duration ms  (uint16 BE)
 *     [12-15] payload bytes (uint32 BE)
 *   Payload: frame_count × 64 × 32 × 2 bytes  (RGB565 BE, row-major)
 *   CRC:     CRC-16/CCITT over header + payload  (poly=0x1021, init=0xFFFF)
 *
 * Serial responses:
 *   0x06 ACK — valid packet committed
 *   0x15 NAK — CRC mismatch
 *   0x1B ERR — malformed header / unsupported dimensions
 *
 * FIX v1.1 — Serial.flush() after every Serial.write() response byte
 * ────────────────────────────────────────────────────────────────────
 *   ESP32-S3 native USB CDC (TinyUSB) buffers outgoing data and waits to
 *   fill a 64-byte USB packet before transmitting. A single-byte response
 *   (ACK/NAK/ERR) would sit in the TinyUSB TX buffer indefinitely without
 *   an explicit flush, causing the Frameon app to time out after 15 s with
 *   "No response from device". Serial.flush() forces immediate transmission.
 *
 * Response byte ordering rule (unchanged from v1.0):
 *   Serial.printf() (debug text) must always be sent BEFORE Serial.write()
 *   (the response byte). Flutter's readResponseByte() polls for the first
 *   available byte; if debug text arrives first it reads '[' (0x5B) instead
 *   of the ACK/NAK/ERR byte and throws "Unexpected response byte: 0x5B".
 */

