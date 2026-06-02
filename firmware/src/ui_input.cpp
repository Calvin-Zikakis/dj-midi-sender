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
uint8_t g_enc_state = R_START;

struct Btn {
    uint8_t  pin;
    uint32_t bit;
    bool     stable;   // last debounced level (true = released/HIGH)
    bool     last;     // last raw reading
    uint32_t changed_ms;
};
Btn g_btns[] = {
    {ENC_SW_PIN,      kBtnEncSw,  true, true, 0},
    {BTN_NUDGE_L_PIN, kBtnNudgeL, true, true, 0},
    {BTN_NUDGE_R_PIN, kBtnNudgeR, true, true, 0},
    {BTN_TAP_PIN,     kBtnTap,    true, true, 0},
};
constexpr uint32_t kDebounceMs = 25;

void poll_encoder() {
    const uint8_t a = digitalRead(ENC_A_PIN) ? 1u : 0u;
    const uint8_t b = digitalRead(ENC_B_PIN) ? 1u : 0u;
    const uint8_t pinstate = static_cast<uint8_t>((b << 1) | a);
    g_enc_state = kTable[g_enc_state & 0x0F][pinstate];
    const uint8_t dir = g_enc_state & 0x30;
    if (dir == DIR_CW)       g_steps.fetch_add(1, std::memory_order_relaxed);
    else if (dir == DIR_CCW) g_steps.fetch_sub(1, std::memory_order_relaxed);
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
    }
}

void input_task(void*) {
    pinMode(ENC_A_PIN, INPUT_PULLUP);
    pinMode(ENC_B_PIN, INPUT_PULLUP);
    for (const Btn& btn : g_btns) pinMode(btn.pin, INPUT_PULLUP);

#ifdef DIAG_SERIAL_STUB
    printf("[ui-in] task started: encA=%d encB=%d encSW=%d nudgeL=%d nudgeR=%d tap=%d\n",
           ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN, BTN_NUDGE_L_PIN, BTN_NUDGE_R_PIN, BTN_TAP_PIN);
    uint32_t last_dump = 0;
#endif

    for (;;) {
        poll_encoder();
        poll_buttons(millis());
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

uint32_t ui_input_take_button_presses() {
    return g_presses.exchange(0, std::memory_order_relaxed);
}

}  // namespace firmware
