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

const char* ui_source_label(uint8_t src) {
    switch (src) {
        case 0:  return "auto";   // follow whichever deck holds the master flag
        case 1:  return "P1";
        case 2:  return "P2";
        case 3:  return "P3";
        case 4:  return "P4";
        case 5:  return "mstr";   // the BOX is the tempo master
        case 6:  return "off";    // ignore players — standalone tempo
        default: return "?";
    }
}

namespace {

// Source-select overlay: list mstr / P1..P4 with a ">" cursor on the proposed
// option and "(on)" on the currently-active one. Push confirms, tap cancels.
void render_source_select(U8G2& oled, const UiSnapshot& s) {
    char buf[24];
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(0, 7, "CLOCK SOURCE");
    for (uint8_t i = 0; i < kSourceCount; ++i) {
        const int y = 14 + i * 7;  // 7 entries: 14..56, clear of the footer
        snprintf(buf, sizeof buf, "%s %s%s",
                 (i == s.proposed_src) ? ">" : " ",
                 ui_source_label(i),
                 (i == s.selected_src) ? " (on)" : "");
        oled.drawStr(0, y, buf);
    }
    oled.drawStr(0, 63, "push set   tap back");
}

// Settings menu. List of items with their values; ">" marks the highlighted
// row. While editing, that row's value is shown in [brackets] and reflects the
// live edit value (committed on push, discarded on tap).
void render_menu(U8G2& oled, const UiSnapshot& s) {
    const bool editing = (s.ui_mode == UiMode::kMenuEdit);
    char buf[48];
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(0, 8, "SETTINGS");

    for (uint8_t i = 0; i < kMenuItemCount; ++i) {
        const int   y   = 18 + i * 11;  // 18, 29, 40, 51
        const bool  sel = (i == s.menu_index);
        const bool  ed  = editing && sel;
        const char* op  = ed ? "[" : "";
        const char* cl  = ed ? "]" : "";
        const char* mk  = sel ? ">" : " ";

        if (i == kMenuItemMode) {
            const bool free_val = ed ? (s.menu_edit != 0) : s.free_run;
            snprintf(buf, sizeof buf, "%s Mode: %s%s%s", mk, op, free_val ? "Free" : "Sync", cl);
        } else if (i == kMenuItemBpmStep) {
            const uint8_t idx = ed ? static_cast<uint8_t>(s.menu_edit) : s.bpm_step_idx;
            const double  v   = (idx < kBpmStepCount) ? kBpmStepValues[idx] : 1.0;
            snprintf(buf, sizeof buf, "%s BPM step: %s%g%s", mk, op, v, cl);
        } else if (i == kMenuItemFineStep) {
            const uint8_t idx = ed ? static_cast<uint8_t>(s.menu_edit) : s.fine_step_idx;
            const double  v   = (idx < kFineStepCount) ? kFineStepValues[idx] : 0.1;
            snprintf(buf, sizeof buf, "%s Fine step: %s%g%s", mk, op, v, cl);
        } else {  // kMenuItemOffsetStep
            const uint8_t idx = ed ? static_cast<uint8_t>(s.menu_edit) : s.offset_step_idx;
            const double  v   = (idx < kOffsetStepCount) ? kOffsetStepValues[idx] : 1.0;
            snprintf(buf, sizeof buf, "%s Offset: %s%gms%s", mk, op, v, cl);
        }
        oled.drawStr(0, y, buf);
    }

    oled.drawStr(0, 62, editing ? "spin set  push ok  tap x"
                                : "push edit     tap back");
}

}  // namespace

void ui_display_render(const UiSnapshot& s) {
    char buf[24];
    g_oled.clearBuffer();

    if (s.ui_mode == UiMode::kSourceSelect) {
        render_source_select(g_oled, s);
        g_oled.sendBuffer();
        return;
    }
    if (s.ui_mode == UiMode::kMenu || s.ui_mode == UiMode::kMenuEdit) {
        render_menu(g_oled, s);
        g_oled.sendBuffer();
        return;
    }

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
    g_oled.drawStr(58, 34, s.playing ? "PLAY" : "STOP");
    // Mode tag: RSYNC flashes briefly right after a re-sync tap; otherwise
    // OFF = standalone; else FREE (MAN while manual-latched) / SYNC.
    const char* mode_tag = s.resync_flash ? "RSYN"
                         : s.is_master     ? "MSTR"   // we hold the master role
                         : s.master_wanted ? "REQ "   // handshake in flight
                         : s.ignore_master ? "OFF "
                         : s.free_run       ? (s.manual_bpm ? "MAN " : "FREE")
                                            : "SYNC";
    g_oled.drawStr(96, 34, mode_tag);

    // ── Source select + active master ──────────────────────────────────
    snprintf(buf, sizeof buf, "src %s", ui_source_label(s.selected_src));
    g_oled.drawStr(0, 48, buf);
    if (s.is_master) snprintf(buf, sizeof buf, "mst us");
    else             snprintf(buf, sizeof buf, "mst %u", s.master_dev);
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
