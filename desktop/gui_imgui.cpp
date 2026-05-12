#include "gui_imgui.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION   // OpenGL on macOS still works; we don't need Metal here.
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace desktop {

namespace {

constexpr float BEAT_FLASH_DECAY_PER_SEC = 4.0f;   // full bright → 0 in 0.25 s
constexpr float TICK_FLASH_DECAY_PER_SEC = 20.0f;  // visible blip per tick

ImVec4 lerp(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

void glfw_error_cb(int err, const char* msg) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", err, msg);
}

}  // namespace

GuiVisualizer::GuiVisualizer(const prolink::Bridge& bridge,
                             const prolink::Clock&  clock,
                             OffsetActions          actions,
                             std::string            midi_port_name)
    : bridge_(bridge)
    , clock_(clock)
    , actions_(std::move(actions))
    , midi_port_(std::move(midi_port_name)) {}

GuiVisualizer::~GuiVisualizer() {
    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(static_cast<GLFWwindow*>(window_));
        glfwTerminate();
        window_ = nullptr;
    }
}

bool GuiVisualizer::init(int width, int height) {
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) {
        std::fprintf(stderr, "[gui] glfwInit failed\n");
        return false;
    }
    // OpenGL 3.2 Core (works on macOS).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE,        GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* w = glfwCreateWindow(width, height,
                                     "dj-midi-sender — XDJ-XZ → MIDI",
                                     nullptr, nullptr);
    if (!w) {
        std::fprintf(stderr, "[gui] glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1);  // vsync
    window_ = w;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    auto& style = ImGui::GetStyle();
    style.WindowRounding   = 8.0f;
    style.FrameRounding    = 4.0f;
    style.GrabRounding     = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding    = ImVec2(14, 12);
    style.ItemSpacing      = ImVec2(8, 8);

    last_tick_sample_at_ = std::chrono::steady_clock::now();
    return true;
}

void GuiVisualizer::request_stop() {
    if (window_) {
        glfwSetWindowShouldClose(static_cast<GLFWwindow*>(window_), GLFW_TRUE);
        glfwPostEmptyEvent();
    }
}

void GuiVisualizer::run() {
    if (!window_) return;
    auto* w = static_cast<GLFWwindow*>(window_);
    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        handle_keys();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        render_frame();
        ImGui::Render();

        int fb_w, fb_h;
        glfwGetFramebufferSize(w, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(w);
    }
}

void GuiVisualizer::on_beat(const prolink::BeatPacket& pkt) {
    std::lock_guard<std::mutex> lk(snap_mu_);
    last_beat_     = pkt;
    have_beat_     = true;
    last_beat_at_  = std::chrono::steady_clock::now();
    beat_flash_    = 1.0f;
}

void GuiVisualizer::on_status(const prolink::StatusPacket& pkt) {
    std::lock_guard<std::mutex> lk(snap_mu_);
    last_status_  = pkt;
    have_status_  = true;
}

void GuiVisualizer::handle_keys() {
    if (!actions_.adjust_grid_ms || !actions_.adjust_clock_ms) return;

    ImGuiIO& io = ImGui::GetIO();
    // Only fire on key-pressed transitions, not held repeats — held repeats
    // would slam dozens of ms in a fraction of a second.
    auto pressed = [&](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    bool shift = io.KeyShift;

    // Arrow keys: grid offset (per-track, session)
    if (pressed(ImGuiKey_LeftArrow))  actions_.adjust_grid_ms(shift ? -10.0f : -1.0f);
    if (pressed(ImGuiKey_RightArrow)) actions_.adjust_grid_ms(shift ? +10.0f : +1.0f);
    if (pressed(ImGuiKey_DownArrow))  actions_.adjust_grid_ms(-10.0f);
    if (pressed(ImGuiKey_UpArrow))    actions_.adjust_grid_ms(+10.0f);

    // [ ] for clock offset (persisted)
    if (pressed(ImGuiKey_LeftBracket))  actions_.adjust_clock_ms(shift ? -10.0f : -1.0f);
    if (pressed(ImGuiKey_RightBracket)) actions_.adjust_clock_ms(shift ? +10.0f : +1.0f);

    if (pressed(ImGuiKey_0)) actions_.reset_grid();
    if (pressed(ImGuiKey_R)) actions_.reset_grid();
    if (pressed(ImGuiKey_Q) || pressed(ImGuiKey_Escape)) request_stop();
}

void GuiVisualizer::render_frame() {
    // Tick rate over the last second.
    uint64_t now_ticks = clock_.ticks_emitted_total();
    auto now_t = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now_t - last_tick_sample_at_).count();
    if (dt >= 0.5f) {
        tick_rate_hz_       = static_cast<float>(now_ticks - last_tick_count_) / dt;
        last_tick_count_    = now_ticks;
        last_tick_sample_at_ = now_t;
    }
    if (now_ticks > prev_tick_total_) {
        tick_flash_ = 1.0f;
        prev_tick_total_ = now_ticks;
    }
    ImGuiIO& io = ImGui::GetIO();
    beat_flash_ = std::max(0.0f, beat_flash_ - io.DeltaTime * BEAT_FLASH_DECAY_PER_SEC);
    tick_flash_ = std::max(0.0f, tick_flash_ - io.DeltaTime * TICK_FLASH_DECAY_PER_SEC);

    // Take a consistent snapshot of the most recent packets.
    prolink::BeatPacket   beat{};
    prolink::StatusPacket status{};
    bool have_beat, have_status;
    {
        std::lock_guard<std::mutex> lk(snap_mu_);
        beat = last_beat_;
        status = last_status_;
        have_beat = have_beat_;
        have_status = have_status_;
    }

    bool playing = bridge_.is_playing();
    uint8_t master_num = bridge_.current_master_num();

    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(viewport);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // --- Header ---
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.95f, 1.0f));
    ImGui::Text("XDJ-XZ → MIDI clock");
    ImGui::PopStyleColor();
    ImGui::SameLine(viewport.x - 180.0f);
    if (playing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.65f, 1.0f));
        ImGui::Text("●  PLAYING");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.5f, 0.5f, 1.0f));
        ImGui::Text("○  stopped");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ---- Master panel ----
    ImGui::Spacing();
    ImGui::PushFont(nullptr);
    ImGui::TextColored(ImVec4(0.65f, 0.75f, 0.95f, 1.0f), "Master");
    ImGui::PopFont();
    ImGui::Separator();

    if (have_beat || have_status) {
        const char* device = have_status && status.device_name[0] ? status.device_name
                            : (have_beat ? beat.device_name : "—");
        ImGui::Text("Device:        %s  #%u", device, static_cast<unsigned>(master_num));

        // Bar position dots: 4 circles, the current one filled & red on the downbeat
        uint8_t bib = have_status ? status.beat_in_bar : (have_beat ? beat.beat_in_bar : 0);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float dot_r = 11.0f, dot_pitch = 36.0f;
        const float row_y = p.y + dot_r + 8.0f;
        const float row_x = p.x + 110.0f;
        for (int i = 0; i < 4; ++i) {
            bool current = (bib == i + 1);
            ImVec4 fill;
            if (current) {
                ImVec4 base = (i == 0)
                    ? ImVec4(1.0f, 0.30f, 0.35f, 1.0f)   // downbeat = red
                    : ImVec4(0.95f, 0.95f, 0.98f, 1.0f); // others = white
                fill = lerp(ImVec4(0.20f, 0.22f, 0.28f, 1.0f), base, std::min(1.0f, beat_flash_ + 0.6f));
            } else {
                fill = ImVec4(0.15f, 0.17f, 0.22f, 1.0f);
            }
            dl->AddCircleFilled(ImVec2(row_x + i * dot_pitch, row_y), dot_r,
                                ImGui::ColorConvertFloat4ToU32(fill), 24);
            if (current) {
                dl->AddCircle(ImVec2(row_x + i * dot_pitch, row_y), dot_r + 2.0f,
                              IM_COL32(220, 220, 230, 110), 24, 1.5f);
            }
        }
        ImGui::Dummy(ImVec2(0, dot_r * 2 + 12));
        ImGui::SameLine(0, 0);
        ImGui::Text("Bar position:");

        float track_bpm = have_status ? status.track_bpm() : beat.track_bpm();
        float mult      = have_status ? status.pitch_multiplier() : beat.pitch_multiplier();
        float effective = have_status ? status.effective_bpm() : beat.effective_bpm();
        ImGui::Text("Track BPM:     %7.2f  ×  %.4f  (%+.2f %%)",
                    track_bpm, mult, (mult - 1.0f) * 100.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 1.0f, 0.65f, 1.0f));
        ImGui::Text("Effective:     %7.2f BPM", effective);
        ImGui::PopStyleColor();

        if (have_status) {
            ImGui::Text("Flags:         master=%s  playing=%s  sync=%s  on-air=%s",
                        status.is_master() ? "Y" : "·",
                        status.is_playing() ? "Y" : "·",
                        status.is_synced()  ? "Y" : "·",
                        status.is_on_air()  ? "Y" : "·");
            ImGui::Text("Mv valid:      %s   (0x%04X)",
                        status.bpm_valid() ? "yes" : "NO (BPM frozen)", status.mv);
        } else {
            ImGui::TextDisabled("status packets:   not yet seen (no virtual CDJ?)");
        }

        ImGui::Text("Packets:       %llu beat / %llu status",
                    static_cast<unsigned long long>(bridge_.beat_packet_count()),
                    static_cast<unsigned long long>(bridge_.status_packet_count()));
    } else {
        ImGui::TextDisabled("Waiting for packets…");
    }

    // ---- MIDI clock out ----
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.5f, 1.0f), "MIDI clock out");
    ImGui::Separator();
    ImGui::Text("Port:          %s", midi_port_.empty() ? "(none)" : midi_port_.c_str());

    // Tick pulse bar — alpha based on time since last tick.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float bar_w = 220.0f, bar_h = 14.0f;
        ImVec2 a(p.x + 120.0f, p.y + 4.0f);
        ImVec2 b(a.x + bar_w, a.y + bar_h);
        dl->AddRectFilled(a, b, IM_COL32(30, 35, 45, 255), 3.0f);
        ImU32 fill = ImGui::ColorConvertFloat4ToU32(
            ImVec4(0.4f + 0.5f * tick_flash_,
                   0.9f,
                   0.55f + 0.2f * tick_flash_,
                   0.9f));
        float pulse_w = bar_w * std::max(0.06f, tick_flash_);
        dl->AddRectFilled(a, ImVec2(a.x + pulse_w, b.y), fill, 3.0f);
        ImGui::Text("Tick pulse:");
        ImGui::Dummy(ImVec2(0, 6));
    }

    ImGui::Text("Rate:          %6.2f ticks/sec   (%.2f BPM)",
                tick_rate_hz_, tick_rate_hz_ * 60.0f / 24.0f);
    ImGui::Text("Tick in beat:  %u / 24",
                static_cast<unsigned>(clock_.current_tick_in_beat()));
    ImGui::Text("Phase error:   %+7.2f ms",
                clock_.current_phase_error_us() / 1000.0f);
    ImGui::Text("Tick period:   %u µs",
                clock_.current_tick_period_us());

    // ---- Offsets ----
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "Offsets");
    ImGui::Separator();

    float clock_ms = actions_.get_clock_ms ? actions_.get_clock_ms() : 0.0f;
    float grid_ms  = actions_.get_grid_ms  ? actions_.get_grid_ms()  : 0.0f;

    auto nudge_row = [&](const char* label, const char* tag,
                         std::function<void(float)> apply,
                         float current_ms) {
        ImGui::PushID(label);
        ImGui::Text("%-7s", label);
        ImGui::SameLine();
        if (ImGui::Button("-10")) apply(-10.0f);
        ImGui::SameLine();
        if (ImGui::Button("-1"))  apply(-1.0f);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,
            std::abs(current_ms) < 0.05f ? ImVec4(0.65f, 0.65f, 0.70f, 1.0f)
                                          : ImVec4(0.55f, 1.0f, 0.65f, 1.0f));
        ImGui::Text("%+7.1f ms", current_ms);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("+1"))  apply(+1.0f);
        ImGui::SameLine();
        if (ImGui::Button("+10")) apply(+10.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tag);
        ImGui::PopID();
    };

    if (actions_.adjust_clock_ms) {
        nudge_row("Clock", "(per-port, persisted)",
                  actions_.adjust_clock_ms, clock_ms);
    }
    if (actions_.adjust_grid_ms) {
        nudge_row("Grid", "(per-track, session)",
                  actions_.adjust_grid_ms, grid_ms);
    }

    ImGui::Spacing();
    ImGui::Text("Total:    %+7.1f ms      ", clock_ms + grid_ms);
    ImGui::SameLine();
    if (actions_.reset_grid && ImGui::Button("Reset grid")) actions_.reset_grid();

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Keys:  ← →  grid ±1 ms     ↑ ↓  grid ±10 ms     0 / R  reset grid");
    ImGui::TextDisabled(
        "       [ ]  clock ±1 ms    Shift+[/]  clock ±10 ms    Q / Esc  quit");

    ImGui::End();
}

}  // namespace desktop
