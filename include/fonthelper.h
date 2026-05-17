
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// fonthelper.h — Frameon shared LED font engine
//
// Provides glyph data and rendering helpers for all seven fonts. Both
// clockhelper.cpp and pomodorohelper.cpp (and any future renderer) #include
// this file to access digit tables, full A-Z alphabets, and the draw helpers.
//
// The active font is selected by calling fontSetActive(fontId) before drawing.
// Digit rendering uses gActiveFont; label rendering uses gActiveAlphabet.
//
// Row format: each row is a uint16_t bitmask, MSB = leftmost pixel, w bits
// wide. Widths up to 9 are used (Waterfox/Destroked M and W).
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>

// ─── Glyph ───────────────────────────────────────────────────────────────────

struct Glyph {
    uint8_t  w;         // pixel width
    uint16_t rows[7];   // 7-row bitmap (uint16_t supports widths up to 16)
};

// ─── Digit tables (16 entries each) ─────────────────────────────────────────
// Index mapping:  0-9 → '0'-'9', 10 → ':', 11 → '.', 12 → 'A',
//                13 → 'M',  14 → 'P',  15 → ' '
extern const Glyph kFontPolymorph[16];
extern const Glyph kFontBrickwork[16];
extern const Glyph kFontWaterfox[16];
extern const Glyph kFontVandalism[16];
extern const Glyph kFontDestroked[16];
extern const Glyph kFontStereotype[16];
extern const Glyph kFontPhantasm[16];

// ─── Alphabet tables (A-Z, 26 entries each) ──────────────────────────────────
extern const Glyph kFontPolymorphAlphabet[26];
extern const Glyph kFontBrickworkAlphabet[26];
extern const Glyph kFontWaterfoxAlphabet[26];
extern const Glyph kFontVandalismAlphabet[26];
extern const Glyph kFontDestrokedAlphabet[26];
extern const Glyph kFontStereotypeAlphabet[26];
extern const Glyph kFontPhantasmAlphabet[26];

// ─── Font registries (indexed by fontId: 0=Polymorph … 6=Phantasm) ───────────
extern const Glyph* const kFonts[7];
extern const Glyph* const kFontAlphabets[7];

// ─── Active font state ────────────────────────────────────────────────────────
// Set by fontSetActive(). Renderers read these directly.
extern const Glyph* gActiveFont;
extern const Glyph* gActiveAlphabet;

// ─── API ─────────────────────────────────────────────────────────────────────

/// Select the active font by ID (0-6). Clamps out-of-range IDs to Polymorph.
void fontSetActive(uint8_t fontId);

/// Glyph index for a digit/punctuation character in the current font table.
int glyphIndex(char c);

/// Pixel width of character c (including 1px inter-character gap) in current font.
int glyphWidth(char c);

/// Total pixel width of a null-terminated digit/punctuation string, no trailing gap.
int textWidth(const char* s);

/// Draw a single digit/punctuation glyph at (x, y) in color.
void drawGlyph(char c, int x, int y, uint16_t color);

/// Draw a null-terminated digit/punctuation string at (x, y) in color.
void drawText(const char* s, int x, int y, uint16_t color);

/// Return the alphabet glyph for letter c (A-Z / a-z) in the current font,
/// or nullptr for non-letter characters.
const Glyph* letterGlyph(char c);

/// Pixel width of letter c (including 1px gap), or 0 for non-letters.
int letterWidth(char c);

/// Total pixel width of a null-terminated ASCII label, no trailing gap.
int labelWidth(const char* s);

/// Draw a single letter at (x, y) in color using the current font's alphabet.
void drawLetter(char c, int x, int y, uint16_t color);

/// Draw a null-terminated ASCII label at (x, y). Non-letter chars are skipped.
void drawLabel(const char* s, int x, int y, uint16_t color);