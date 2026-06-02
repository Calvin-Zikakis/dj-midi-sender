#include "ui_display.hpp"

#include <U8g2lib.h>

#include <cstdio>

namespace firmware {
namespace {

// Hardware I2C with explicit pins. The default Wire pins are wrong on this
// build (the Arduino-as-IDF-component fallback uses the generic `esp32`
// variant, SDA=21/SCL=22 — and GPIO22 doesn't exist on the S3), but U8g2's
// HW-I2C constructor takes clock/data pins and, on ESP32, calls
// Wire.begin(data, clock) with them — so we get the right pins AND the
// hardware peripheral. SW I2C was a mistake: it toggles pin *direction*
// every bit, the IDF gpio driver logs each change, and a full 128x64 frame
// backed up the console until the UI task tripped the task watchdog.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_oled(
    U8G2_R0, /*reset*/ U8X8_PIN_NONE, /*clock=SCL*/ OLED_SCL_PIN, /*data=SDA*/ OLED_SDA_PIN);

}  // namespace

void ui_display_begin() {
    g_oled.setBusClock(400000);  // 400 kHz HW I2C; set before begin()
    g_oled.begin();
    g_oled.clearBuffer();
    g_oled.setFont(u8g2_font_6x12_tr);
    g_oled.drawStr(0, 14, "xdj-bridge");
    g_oled.drawStr(0, 30, "starting...");
    g_oled.sendBuffer();
}

void ui_display_render(const UiSnapshot& s) {
    char buf[24];
    g_oled.clearBuffer();

    // ── Big BPM, top-left ──────────────────────────────────────────────
    g_oled.setFont(u8g2_font_logisoso20_tn);
    if (s.clock_running && s.bpm > 0.0f) snprintf(buf, sizeof buf, "%.1f", s.bpm);
    else                                 snprintf(buf, sizeof buf, "0.0");
    g_oled.drawStr(0, 21, buf);

    // BPM label + live pitch %, top-right
    g_oled.setFont(u8g2_font_5x8_tr);
    g_oled.drawStr(92, 7, "BPM");
    snprintf(buf, sizeof buf, "%+.1f%%", s.pitch_pct);
    g_oled.drawStr(92, 18, buf);

    // ── Beat-in-bar dots (left) + play state (right) ───────────────────
    for (int i = 0; i < 4; ++i) {
        const int x = 8 + i * 12;
        const int y = 31;
        if ((i + 1) == s.beat_in_bar) g_oled.drawDisc(x, y, 3);
        else                          g_oled.drawCircle(x, y, 3);
    }
    g_oled.setFont(u8g2_font_6x12_tr);
    g_oled.drawStr(74, 34, s.playing ? "PLAY" : "STOP");

    // ── Source select + active master ──────────────────────────────────
    if (s.selected_src == 0) snprintf(buf, sizeof buf, "src auto");
    else                     snprintf(buf, sizeof buf, "src P%u", s.selected_src);
    g_oled.drawStr(0, 48, buf);
    snprintf(buf, sizeof buf, "mst %u", s.master_dev);
    g_oled.drawStr(74, 48, buf);

    // ── Offset (the nudge value) + link/USB status ─────────────────────
    g_oled.setFont(u8g2_font_5x8_tr);
    snprintf(buf, sizeof buf, "off %+.1fms", s.offset_ms);
    g_oled.drawStr(0, 62, buf);
    snprintf(buf, sizeof buf, "%s %s", s.link_up ? "L" : "-", s.usb_state);
    g_oled.drawStr(90, 62, buf);

    g_oled.sendBuffer();
}

}  // namespace firmware
