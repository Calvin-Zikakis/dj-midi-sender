// Tests for the front-panel source-gating helpers.
//
// firmware/src/ui_display.hpp is header-only apart from two label functions, so
// the constexpr parts compile and run on the host. That matters because these
// helpers decide whether the destructive "sync master" option is reachable at
// all — a bug here either hides a feature or lets a stray encoder spin hijack
// the DJ-Link master role mid-set.

#include "ui_display.hpp"

#include <cstdio>
#include <set>

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
}
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

void section(const char* name) { std::printf("%s\n", name); std::fflush(stdout); }

using namespace firmware;

void test_gate_hides_exactly_one_entry() {
    section("ui: the sync-master gate hides exactly one entry");
    CHECK(source_count(true)  == kSourceCount);
    CHECK(source_count(false) == kSourceCount - 1);
    CHECK(kSourceMaster < kSourceOff);      // master sits before off in the list
    CHECK(kSourceOff == kSourceCount - 1);
}

void test_cursor_never_lands_on_a_hidden_entry() {
    section("ui: with the gate off, no cursor position maps to sync master");
    for (uint8_t i = 0; i < source_count(false); ++i) {
        const uint8_t src = source_at(i, false);
        CHECK(src != kSourceMaster);
        CHECK(src < kSourceCount);
    }
    // And every other source is still reachable — hiding one entry must not
    // hide any of the rest.
    std::set<uint8_t> reachable;
    for (uint8_t i = 0; i < source_count(false); ++i) reachable.insert(source_at(i, false));
    CHECK(reachable.size() == source_count(false));
    for (uint8_t src = 0; src < kSourceCount; ++src) {
        if (src == kSourceMaster) continue;
        CHECK(reachable.count(src) == 1);
    }
}

void test_all_entries_reachable_when_enabled() {
    section("ui: with the gate on, every source is reachable exactly once");
    std::set<uint8_t> reachable;
    for (uint8_t i = 0; i < source_count(true); ++i) {
        const uint8_t src = source_at(i, true);
        CHECK(src < kSourceCount);
        reachable.insert(src);
    }
    CHECK(reachable.size() == kSourceCount);
    CHECK(reachable.count(kSourceMaster) == 1);
    CHECK(reachable.count(kSourceOff) == 1);
}

void test_mapping_is_identity_below_the_gate() {
    section("ui: sources before sync master are unaffected by the gate");
    for (uint8_t i = 0; i < kSourceMaster; ++i) {
        CHECK(source_at(i, true)  == i);
        CHECK(source_at(i, false) == i);   // only entries at/after it shift
    }
    // With the gate off, the cursor position where master would have been now
    // lands on `off` — the list closes up rather than leaving a gap.
    CHECK(source_at(kSourceMaster, false) == kSourceOff);
    CHECK(source_at(kSourceMaster, true)  == kSourceMaster);
}

void test_menu_and_step_tables_are_consistent() {
    section("ui: menu and step tables agree with their declared sizes");
    CHECK(kMenuItemCount == 3);
    CHECK(kMenuItemActAsPlayer < kMenuItemBpmStep);
    CHECK(kMenuItemBpmStep < kMenuItemOffsetStep);
    // Defaults must index inside their tables, or the menu renders garbage.
    CHECK(kBpmStepDefault < kBpmStepCount);
    CHECK(kOffsetStepDefault < kOffsetStepCount);
    CHECK(sizeof(kBpmStepValues) / sizeof(kBpmStepValues[0]) == kBpmStepCount);
    CHECK(sizeof(kOffsetStepValues) / sizeof(kOffsetStepValues[0]) == kOffsetStepCount);
    // Steps ascend, so spinning the encoder one way always means "coarser".
    for (uint8_t i = 1; i < kBpmStepCount; ++i)
        CHECK(kBpmStepValues[i] > kBpmStepValues[i - 1]);
    for (uint8_t i = 1; i < kOffsetStepCount; ++i)
        CHECK(kOffsetStepValues[i] > kOffsetStepValues[i - 1]);
}

}  // namespace

int ui_tests_main() {
    test_gate_hides_exactly_one_entry();
    test_cursor_never_lands_on_a_hidden_entry();
    test_all_entries_reachable_when_enabled();
    test_mapping_is_identity_below_the_gate();
    test_menu_and_step_tables_are_consistent();
    std::printf("\nui: %d checks, %d failure%s\n", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures;
}
