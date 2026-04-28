#pragma once

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// spotifyhelper.h — Firmware-side Spotify progress bar renderer
//
// overdrawProgressBar() repaints the 2-pixel-tall progress bar using the
// predicted song position computed from millis(). Bar geometry (barX, barY,
// barW) comes from the packet header and exactly matches what
// spotify_widget.dart baked:
//
//   artAndText : barX=33  barY=29  barW=30   (right of art)
//   textOnly   : barX=0   barY=30  barW=63   (full width)
//   artOnly    : barW=0   → function returns immediately, art untouched
//
// Must be called only from Core 0 (displayTask).
// ─────────────────────────────────────────────────────────────────────────────

/// Overdraw the Spotify progress bar using the predicted real-time position.
///
/// @param songPosMs  Predicted playback position (startPosMs + wallMs).
/// @param trackDurMs Total track duration from header (0 = skip).
/// @param barX       Left edge of bar in pixels.
/// @param barY       Top edge of bar in pixels.
/// @param barW       Width of bar in pixels (0 = skip).
/// @param barColor   RGB565 filled-portion colour from header.
void overdrawProgressBar(
    uint32_t songPosMs,
    uint32_t trackDurMs,
    uint8_t  barX,
    uint8_t  barY,
    uint8_t  barW,
    uint16_t barColor
);