// Phase 2 firmware entry point. Most work happens in FreeRTOS tasks
// pinned to specific cores; setup() will eventually only spin them up.
// See docs/handoff.md "Phase 2" for the planned task topology.

#include <Arduino.h>

#include "bridge.hpp"  // lib/prolink — pure C++17, no platform deps

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[xdj-bridge] firmware skeleton up");
}

void loop() {
    delay(1000);
}
