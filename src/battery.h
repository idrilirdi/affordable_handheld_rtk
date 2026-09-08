#pragma once

// Battery monitoring for the XIAO ESP32-C6.
//
// An external 100k/100k divider halves the cell voltage into A0, so the
// reading is analogReadMilliVolts(A0) * 2. No enable pin is involved.

#include <stdint.h>

void batteryBegin();

// Samples periodically; cheap to call every loop.
void batteryTask();

// Smoothed cell voltage in millivolts, 0 if no plausible battery is present.
uint16_t batteryMillivolts();

// State of charge, 0-100. 0xFF when unknown (no battery, or USB powered with
// nothing on the BAT pads).
uint8_t batteryPercent();

// True when the reading looks like a real cell rather than a charger rail.
bool batteryPresent();
