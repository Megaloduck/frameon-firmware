// ── PATCH: src/main.cpp ───────────────────────────────────────────────────────
//
// Two changes needed:
//
// 1. Add two new volatile active-display-state fields alongside the existing
//    pktPomodoroOffsetX/Y block (in the "Active display state" section):
//
//    ADD after `static int8_t   activePomodoroOffsetY = 0;`:
//
//      static volatile uint8_t  activePomodoroLayout      = 0;   // v1.9
//      static volatile uint8_t  activePomodoroSessTotal   = 4;   // v1.9
//
// ─────────────────────────────────────────────────────────────────────────────
// 2. In parseHeader() — read the two new bytes from the packet buffer.
//    The existing pomodoro block ends at offset 62 (pomodoroColor uint16).
//    Offsets [63] and [64] are the new layout and sessionsTotal bytes.
//
//    ADD after parsing pomodoroColor at [61-62]:
//
//      pktPomodoroLayout    = h[63];              // layout  (v1.9)
//      pktPomodoroSessTotal = h[64];              // sessionsTotal (v1.9)
//      // bytes [65-67] remain reserved
//
//    ADD the matching packet-level variables near pktPomodoroOffsetY:
//
//      static uint8_t  pktPomodoroLayout    = 0;
//      static uint8_t  pktPomodoroSessTotal = 4;
//
// ─────────────────────────────────────────────────────────────────────────────
// 3. In processPacket() — copy new packet fields into active state under mutex:
//
//    ADD after `activePomodoroColor = pktPomodoroColor;`:
//
//      activePomodoroLayout    = pktPomodoroLayout;
//      activePomodoroSessTotal = pktPomodoroSessTotal;
//
// ─────────────────────────────────────────────────────────────────────────────
// 4. In displayTask() — snapshot the two new fields and update the call:
//
//    ADD to the local snapshot block (after `const uint16_t pomoColor`):
//
//      const uint8_t  pomoLayout    = activePomodoroLayout;
//      const uint8_t  pomoSessTotal = activePomodoroSessTotal;
//
//    REPLACE the existing overdrawPomodoro() call:
//
//    OLD:
//      overdrawPomodoro(
//          pomoRemSec, wallMs,
//          pomoPhase, pomoFlags,
//          pomoSession,
//          pomoOffX, pomoOffY,
//          pomoColor);
//
//    NEW:
//      overdrawPomodoro(
//          pomoRemSec,
//          pomoTotalSec,    // see note below
//          wallMs,
//          pomoPhase,
//          pomoLayout,
//          pomoFlags,
//          pomoSession,
//          pomoSessTotal,
//          pomoOffX,
//          pomoOffY,
//          pomoColor);
//
// ─────────────────────────────────────────────────────────────────────────────
// 5. pomoTotalSec — the firmware needs the full phase duration to compute the
//    arc/bar progress fraction. The simplest approach: add one more volatile
//    field `activePomodoroTotalSec` and pass it from the app in reserved
//    byte [65-66] as a uint16 (max 3600 s = 60 min long break).
//    OR compute it on the firmware from session/phase — but that requires
//    knowing focusDurationMinutes which isn't in the header.
//
//    RECOMMENDED: use header bytes [65-66] for pomodoroTotalSec (uint16 BE).
//    Update frame_exporter.dart accordingly:
//
//      bd.setUint16(off, totalSecsForPhase, Endian.big); off += 2; // [65-66]
//      packet[off++] = 0x00;                                        // [67]
//
//    where totalSecsForPhase = (pomodoroState.remaining.inSeconds +
//                               /* elapsed */ 0).  Actually the cleanest is:
//
//      final int totalSecsForPhase = switch (pomodoroState.phase) {
//        PomodoroState.focus      => pomodoroLayer.focusDurationMinutes * 60,
//        PomodoroState.shortBreak => pomodoroLayer.shortBreakMinutes    * 60,
//        PomodoroState.longBreak  => pomodoroLayer.longBreakMinutes     * 60,
//      };
//
//    Then on the firmware side:
//
//      static volatile uint16_t activePomodoroTotalSec = 1500; // default 25 min
//      // parseHeader:
//      pktPomodoroTotalSec = ((uint16_t)h[65] << 8) | h[66];
//      // displayTask snapshot:
//      const uint32_t pomoTotalSec = activePomodoroTotalSec;
//
// ─────────────────────────────────────────────────────────────────────────────
// SUMMARY OF ALL NEW FIELDS USING RESERVED BYTES [63-67]:
//
//   [63]     pomodoroLayout     uint8    0=split  1=minimalist
//   [64]     pomodoroSessTotal  uint8    sessionsBeforeLongBreak
//   [65-66]  pomodoroTotalSec   uint16   total seconds for current phase
//   [67]     reserved           0x00
//
// Header size stays 68 — no firmware protocol version bump needed.
// ─────────────────────────────────────────────────────────────────────────────