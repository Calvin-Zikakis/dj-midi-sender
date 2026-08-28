#include "ui_display.hpp"

#include <U8g2lib.h>

#include <cstdio>
#include <cstring>

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

// Pushing a frame is expensive: 1024 bytes over I2C at 400 kHz is ~25 ms of
// bus time, and the UI task renders at 25 fps. Most frames are identical to the
// last one (nothing on screen changes between beats), so compare the rendered
// buffer against what the panel already has and skip the transfer when they
// match. Comparing the buffer rather than the snapshot means this stays correct
// no matter which fields the renderer starts or stops using.
uint8_t  g_last_frame[1024];
bool     g_have_last_frame = false;
uint32_t g_frames_skipped  = 0;

// Resend even an unchanged frame this often. Skipping transfers assumes the
// panel still holds what we last sent, which a glitch on the I2C bus or a
// browned-out display would break — and with a static screen nothing would ever
// refresh it. At 25 fps this is a redraw roughly every two seconds, which costs
// nothing and makes the display self-healing.
constexpr uint32_t kForceRedrawEvery = 50;

void send_if_changed(U8G2& oled) {
    const size_t len = static_cast<size_t>(oled.getBufferTileHeight()) * 8u *
                       static_cast<size_t>(oled.getBufferTileWidth());
    uint8_t* buf = oled.getBufferPtr();
    if (len <= sizeof(g_last_frame)) {
        if (g_have_last_frame && g_frames_skipped < kForceRedrawEvery &&
            std::memcmp(buf, g_last_frame, len) == 0) {
            ++g_frames_skipped;
            return;
        }
        std::memcpy(g_last_frame, buf, len);
        g_have_last_frame = true;
        g_frames_skipped  = 0;
    }
    oled.sendBuffer();
}

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
        case 0:  return "folw";   // follower master: track whoever holds master
        case 1:  return "P1";
        case 2:  return "P2";
        case 3:  return "P3";
        case 4:  return "P4";
        case 5:  return "sync";   // sync master: the box holds the role
        case 6:  return "off";    // ignore players — standalone tempo
        default: return "?";
    }
}

const char* ui_source_label_long(uint8_t src) {
    switch (src) {
        case 0:  return "follower master";
        case 1:  return "player 1";
        case 2:  return "player 2";
        case 3:  return "player 3";
        case 4:  return "player 4";
        case 5:  return "sync master";
        case 6:  return "off (standalone)";
        default: return "?";
    }
}

namespace {

// Source-select overlay: list mstr / P1..P4 with a ">" cursor on the proposed
// option and "(on)" on the currently-active one. Push confirms, tap cancels.
void render_source_select(U8G2& oled, const UiSnapshot& s) {
    char buf[32];
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(0, 7, "CLOCK SOURCE");
    const uint8_t n = source_count(s.act_as_player);
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t src = source_at(i, s.act_as_player);
        const int y = 14 + i * 7;  // up to 7 entries: 14..56, clear of the footer
        snprintf(buf, sizeof buf, "%s %s%s",
                 (src == s.proposed_src) ? ">" : " ",
                 ui_source_label_long(src),
                 (src == s.selected_src) ? " (on)" : "");
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
        const int   y   = 18 + i * 13;  // 18, 31, 44
        const bool  sel = (i == s.menu_index);
        const bool  ed  = editing && sel;
        const char* op  = ed ? "[" : "";
        const char* cl  = ed ? "]" : "";
        const char* mk  = sel ? ">" : " ";

        if (i == kMenuItemActAsPlayer) {
            const bool on = ed ? (s.menu_edit != 0) : s.act_as_player;
            snprintf(buf, sizeof buf, "%s Act as player: %s%s%s",
                     mk, op, on ? "yes" : "no", cl);
        } else if (i == kMenuItemBpmStep) {
            const uint8_t idx = ed ? static_cast<uint8_t>(s.menu_edit) : s.bpm_step_idx;
            const double  v   = (idx < kBpmStepCount) ? kBpmStepValues[idx] : 0.1;
            snprintf(buf, sizeof buf, "%s BPM step: %s%g%s", mk, op, v, cl);
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
        send_if_changed(g_oled);
        return;
    }
    if (s.ui_mode == UiMode::kMenu || s.ui_mode == UiMode::kMenuEdit) {
        render_menu(g_oled, s);
        send_if_changed(g_oled);
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
    // Tag slot shows only what the `src` line does NOT already say: transient
    // confirmations and in-flight state. (`MSTR`/`OFF` used to live here and
    // merely repeated the source.)
    const char* mode_tag = s.resync_flash                  ? "RSYN"  // just re-synced
                         : (s.master_wanted && !s.is_master) ? "REQ "  // handshake running
                                                             : "    ";
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

    send_if_changed(g_oled);
}

}  // namespace firmware
