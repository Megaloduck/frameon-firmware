// ─────────────────────────────────────────────────────────────────────────────
// overdrawClock — renders clock on top of the current frame
// fontId is now honoured: 0=Polymorph 1=Brickwork 2=Waterfox 3=Vandalism
//                          4=Destroked 5=Stereotype 6=Phantasm
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawClock(uint32_t epochSec, uint32_t elapsedMs,
                           int16_t tzOffsetMin, uint8_t flags,
                           uint8_t fontId, int8_t offX, int8_t offY,
                           uint16_t hoursCol,   uint16_t minutesCol,
                           uint16_t secondsCol, uint16_t colonCol,
                           uint16_t dateCol,    uint16_t ampmCol) {

    if (!(flags & CLK_FLAG_PRESENT)) return;

    // Select font — clamp to valid range
    gActiveFont = kFonts[fontId < 7 ? fontId : 0];

    const bool h12       = flags & CLK_FLAG_H12;
    const bool showSec   = flags & CLK_FLAG_SECONDS;
    const bool showDate  = flags & CLK_FLAG_DATE;
    const bool blinkCol  = flags & CLK_FLAG_BLINK;
    const bool showAmPm  = flags & CLK_FLAG_AMPM;

    // Derive current wall time from epoch + elapsed
    const uint32_t wallSec = epochSec + elapsedMs / 1000;
    ClockTime ct = epochToTime(wallSec, tzOffsetMin);

    // Blink colon: off for the second half of each second
    const bool colonVisible = !blinkCol || (elapsedMs % 1000) < 500;

    // Build time string segments
    char hBuf[4], mBuf[4], sBuf[4], ampmBuf[4], dateBuf[10];

    int dispHour = ct.hour;
    if (h12) {
        dispHour = ct.hour % 12;
        if (dispHour == 0) dispHour = 12;
        snprintf(hBuf,    sizeof(hBuf),    "%d",   dispHour);
        snprintf(ampmBuf, sizeof(ampmBuf), "%s",   ct.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(hBuf,    sizeof(hBuf),    "%02d", dispHour);
        ampmBuf[0] = '\0';
    }
    snprintf(mBuf,    sizeof(mBuf),    "%02d", ct.minute);
    snprintf(sBuf,    sizeof(sBuf),    "%02d", ct.second);
    snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%02d",
             ct.day, ct.month, ct.year % 100);

    // Measure time row width
    int timeW = textWidth(hBuf)
              + 2 + glyphWidth(':')   // spacingBeforeColon=2, afterColon=0
              + textWidth(mBuf);
    if (showSec)   timeW += 2 + glyphWidth(':') + textWidth(sBuf);
    if (showAmPm)  timeW += 1 + textWidth(ampmBuf);

    // Vertical layout
    const int charH  = 7;
    const int totalH = charH + (showDate ? charH + 2 : 0);
    const int startY = (REAL_HEIGHT - totalH) / 2 + (int)offY;
    const int timeY  = startY;
    const int dateY  = startY + charH + 2;

    // Horizontal start (centered + offset)
    int cx = (PANEL_WIDTH - timeW) / 2 + (int)offX;

    // Draw hours
    drawText(hBuf, cx, timeY, hoursCol);
    cx += textWidth(hBuf);

    // Draw colon (hours:minutes)
    cx += 2; // spacingBeforeColon
    if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
    cx += glyphWidth(':') - 1; // colonVisualOffset = -1

    // Draw minutes
    drawText(mBuf, cx, timeY, minutesCol);
    cx += textWidth(mBuf);

    // Draw :seconds
    if (showSec) {
        cx += 2;
        if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
        cx += glyphWidth(':') - 1;
        drawText(sBuf, cx, timeY, secondsCol);
        cx += textWidth(sBuf);
    }

    // Draw AM/PM
    if (showAmPm) {
        cx += 1;
        drawText(ampmBuf, cx, timeY, ampmCol);
    }

    // Draw date row (centered, below time)
    if (showDate) {
        int dw = textWidth(dateBuf);
        int dx = (PANEL_WIDTH - dw) / 2 + (int)offX;
        drawText(dateBuf, dx, dateY, dateCol);
    }
}