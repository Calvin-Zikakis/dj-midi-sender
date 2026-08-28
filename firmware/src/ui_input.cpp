#include "ui_input.hpp"

#include <Arduino.h>

#include <atomic>
#include <cstdio>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace firmware {
namespace {

// ── Ben Buxton full-step quadrature decoder ────────────────────────────
// Emits exactly one DIR per physical detent and rejects bounce. State is
// the low nibble; the high bits carry the emitted direction for this step.
constexpr uint8_t R_START     = 0x0;
constexpr uint8_t R_CW_FINAL  = 0x1;
constexpr uint8_t R_CW_BEGIN  = 0x2;
constexpr uint8_t R_CW_NEXT   = 0x3;
constexpr uint8_t R_CCW_BEGIN = 0x4;
constexpr uint8_t R_CCW_FINAL = 0x5;
constexpr uint8_t R_CCW_NEXT  = 0x6;
constexpr uint8_t DIR_CW  = 0x10;
constexpr uint8_t DIR_CCW = 0x20;

const uint8_t kTable[7][4] = {
    // 00            01           10           11
    {R_START,    R_CW_BEGIN,  R_CCW_BEGIN, R_START},            // R_START
    {R_CW_NEXT,  R_START,     R_CW_FINAL,  R_START | DIR_CW},   // R_CW_FINAL
    {R_CW_NEXT,  R_CW_BEGIN,  R_START,     R_START},            // R_CW_BEGIN
    {R_CW_NEXT,  R_CW_BEGIN,  R_CW_FINAL,  R_START},            // R_CW_NEXT
    {R_CCW_NEXT, R_START,     R_CCW_BEGIN, R_START},            // R_CCW_BEGIN
    {R_CCW_NEXT, R_CCW_FINAL, R_START,     R_START | DIR_CCW},  // R_CCW_FINAL
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},            // R_CCW_NEXT
};

std::atomic<int32_t>  g_steps{0};
std::atomic<uint32_t> g_presses{0};
std::atomic<bool>     g_tap_held{false};
uint8_t g_enc_state = R_START;

struct Btn {
    uint8_t  pin;
    uint32_t bit;
    bool     stable;   // last debounced level (true = released/HIGH)
    bool     last;     // last raw reading
    uint32_t changed_ms;
};
// Single-press buttons (no auto-repeat): encoder push, tap.
Btn g_btns[] = {
    {ENC_SW_PIN,  kBtnEncSw, true, true, 0},
    {BTN_TAP_PIN, kBtnTap,   true, true, 0},
};
constexpr uint32_t kDebounceMs = 25;

// Nudge buttons: one signed step per press, then a controlled accelerating
// hold-to-repeat. (The desktop GUI deliberately avoided OS key-repeat because
// it was uncontrolled — "held repeats would slam dozens of ms"; this is the
// hardware-button analogue: bounded delay + ramped interval.)
std::atomic<int32_t> g_nudge_steps{0};

struct NudgeBtn {
    uint8_t  pin;
    int8_t   dir;          // step direction: +1 right / −1 left
    bool     stable;
    bool     last;
    uint32_t changed_ms;
    uint32_t next_fire_ms;
    uint16_t interval_ms;
    bool     fresh_press;  // settled-press edge, consumed by the step logic
};
NudgeBtn g_nudge[] = {
    {BTN_NUDGE_L_PIN, -1, true, true, 0, 0, 0, false},
    {BTN_NUDGE_R_PIN, +1, true, true, 0, 0, 0, false},
};
constexpr uint32_t kRepeatDelayMs = 350;  // hold this long before repeats start
constexpr uint16_t kRepeatStartMs = 160;  // first repeat interval
constexpr uint16_t kRepeatMinMs   = 35;   // fastest interval (full speed)
constexpr uint16_t kRepeatAccelMs = 18;   // interval shrink per repeat

// Both nudge buttons held together = open the settings menu (one-shot per hold).
constexpr uint32_t kComboHoldMs = 1000;
std::atomic<uint32_t> g_menu_holds{0};
uint32_t g_combo_start_ms = 0;
bool     g_combo_fired    = false;

void poll_nudge(uint32_t now_ms) {
    // 1) Debounce both buttons (flag fresh settled presses for step logic).
    for (NudgeBtn& b : g_nudge) {
        const bool reading = digitalRead(b.pin);  // HIGH idle, LOW pressed
        if (reading != b.last) {
            b.last = reading;
            b.changed_ms = now_ms;
        }
        if ((now_ms - b.changed_ms) >= kDebounceMs && reading != b.stable) {
            b.stable = reading;
            if (!b.stable) {  // fresh press → arm the repeat
                b.fresh_press  = true;
                b.next_fire_ms = now_ms + kRepeatDelayMs;
                b.interval_ms  = kRepeatStartMs;
            }
        }
    }

    const bool both_held = !g_nudge[0].stable && !g_nudge[1].stable;

    // 2) Both held → open the settings menu. Fires once when the hold passes
    //    the threshold; resets when either is released.
    if (both_held) {
        if (g_combo_start_ms == 0) g_combo_start_ms = now_ms;
        if (!g_combo_fired && (now_ms - g_combo_start_ms) >= kComboHoldMs) {
            g_combo_fired = true;
            g_menu_holds.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_combo_start_ms = 0;
        g_combo_fired    = false;
    }

    // 3) Nudge steps — suppressed entirely while both are held (that's a
    //    combo, not a nudge). One step per press, then accelerating repeat.
    for (NudgeBtn& b : g_nudge) {
        if (b.stable) {            // released
            b.fresh_press = false;
            continue;
        }
        if (both_held) {           // don't accumulate during a combo
            b.fresh_press = false;
            continue;
        }
        if (b.fresh_press) {
            b.fresh_press = false;
            g_nudge_steps.fetch_add(b.dir, std::memory_order_relaxed);
        } else if (static_cast<int32_t>(now_ms - b.next_fire_ms) >= 0) {
            g_nudge_steps.fetch_add(b.dir, std::memory_order_relaxed);
            b.interval_ms = (b.interval_ms > kRepeatMinMs + kRepeatAccelMs)
                                ? static_cast<uint16_t>(b.interval_ms - kRepeatAccelMs)
                                : kRepeatMinMs;
            b.next_fire_ms = now_ms + b.interval_ms;
        }
    }
}

void poll_encoder() {
    const uint8_t a = digitalRead(ENC_A_PIN) ? 1u : 0u;
    const uint8_t b = digitalRead(ENC_B_PIN) ? 1u : 0u;
    const uint8_t pinstate = static_cast<uint8_t>((b << 1) | a);
    g_enc_state = kTable[g_enc_state & 0x0F][pinstate];
    const uint8_t dir = g_enc_state & 0x30;
    // Direction is intentionally inverted from the raw quadrature so the knob
    // scrolls the way the panel expects (matches the physical detent feel).
    if (dir == DIR_CW)       g_steps.fetch_sub(1, std::memory_order_relaxed);
    else if (dir == DIR_CCW) g_steps.fetch_add(1, std::memory_order_relaxed);
}

void poll_buttons(uint32_t now_ms) {
    for (Btn& btn : g_btns) {
        const bool reading = digitalRead(btn.pin);  // HIGH idle, LOW pressed
        if (reading != btn.last) {
            btn.last = reading;
            btn.changed_ms = now_ms;
        }
        if ((now_ms - btn.changed_ms) >= kDebounceMs && reading != btn.stable) {
            btn.stable = reading;
            if (!btn.stable) {  // settled LOW = fresh press
                g_presses.fetch_or(btn.bit, std::memory_order_relaxed);
            }
        }
        // Continuously expose the tap button's held state (LOW = held) for the
        // hold-tap + spin BPM modifier.
        if (btn.bit == kBtnTap) g_tap_held.store(!btn.stable, std::memory_order_relaxed);
    }
}

void input_task(void*) {
    pinMode(ENC_A_PIN, INPUT_PULLUP);
    pinMode(ENC_B_PIN, INPUT_PULLUP);
    for (const Btn& btn : g_btns) pinMode(btn.pin, INPUT_PULLUP);
    for (const NudgeBtn& b : g_nudge) pinMode(b.pin, INPUT_PULLUP);

#ifdef DIAG_SERIAL_STUB
    printf("[ui-in] task started: encA=%d encB=%d encSW=%d nudgeL=%d nudgeR=%d tap=%d\n",
           ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN, BTN_NUDGE_L_PIN, BTN_NUDGE_R_PIN, BTN_TAP_PIN);
    uint32_t last_dump = 0;
#endif

    for (;;) {
        const uint32_t t = millis();
        poll_encoder();
        poll_buttons(t);
        poll_nudge(t);
#ifdef DIAG_SERIAL_STUB
        // Every 500 ms, dump raw pin levels (1 = idle/pulled-up, 0 = pressed
        // or ground reached). If these never go to 0 when you press/turn, the
        // input ground isn't connected or the wire's on the wrong pin.
        const uint32_t now = millis();
        if (now - last_dump >= 500) {
            last_dump = now;
            printf("[ui-in] raw A=%d B=%d SW=%d L=%d R=%d TAP=%d | steps=%ld\n",
                   digitalRead(ENC_A_PIN), digitalRead(ENC_B_PIN), digitalRead(ENC_SW_PIN),
                   digitalRead(BTN_NUDGE_L_PIN), digitalRead(BTN_NUDGE_R_PIN), digitalRead(BTN_TAP_PIN),
                   (long)g_steps.load());
        }
#endif
        vTaskDelay(1);  // 1 ms tick (FreeRTOS HZ = 1000)
    }
}

}  // namespace

void ui_input_begin() {
    xTaskCreatePinnedToCore(input_task, "ui-input", 2560, nullptr, 4, nullptr, 0);
}

int32_t ui_input_take_encoder_steps() {
    return g_steps.exchange(0, std::memory_order_relaxed);
}

int32_t ui_input_take_nudge_steps() {
    return g_nudge_steps.exchange(0, std::memory_order_relaxed);
}

uint32_t ui_input_take_menu_holds() {
    return g_menu_holds.exchange(0, std::memory_order_relaxed);
}

bool ui_input_tap_held() {
    return g_tap_held.load(std::memory_order_relaxed);
}

uint32_t ui_input_take_button_presses() {
    return g_presses.exchange(0, std::memory_order_relaxed);
}

}  // namespace firmware
