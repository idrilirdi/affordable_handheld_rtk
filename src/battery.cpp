#include "battery.h"

#include <Arduino.h>

#include "config.h"

static uint16_t mv = 0;
static uint8_t pct = BATTERY_UNKNOWN;

// Lithium-ion open-circuit voltage against state of charge. A straight line
// from 3.0 V to 4.2 V is badly wrong for this chemistry: the curve is steep at
// both ends and almost flat between roughly 20% and 80%, so a linear map
// reports a nearly full cell for most of the discharge and then collapses.
// Piecewise-linear through the knees is far closer, and still cheap.
struct Point { uint16_t mv; uint8_t pct; };
static const Point kCurve[] = {
    {4200, 100}, {4100, 92}, {4000, 84}, {3900, 74}, {3800, 62},
    {3700, 50},  {3650, 40}, {3600, 31}, {3550, 22}, {3500, 15},
    {3400, 8},   {3300, 4},  {3200, 2},  {3000, 0},
};

static uint8_t curveLookup(uint16_t v) {
  if (v >= kCurve[0].mv) return 100;
  const size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
  if (v <= kCurve[n - 1].mv) return 0;
  for (size_t i = 1; i < n; ++i) {
    if (v >= kCurve[i].mv) {
      const Point &hi = kCurve[i - 1], &lo = kCurve[i];
      uint16_t span = hi.mv - lo.mv;
      return lo.pct + (uint8_t)((uint32_t)(v - lo.mv) * (hi.pct - lo.pct) / span);
    }
  }
  return 0;
}

void batteryBegin() {
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
}

void batteryTask() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (mv != 0 && now - last < BATTERY_SAMPLE_MS) return;
  last = now;

  // Average a burst: the ADC is noisy and the load is switching (BLE bursts,
  // the receiver's own draw), so a single sample swings by tens of millivolts.
  uint32_t sum = 0;
  for (int i = 0; i < 16; ++i) sum += analogReadMilliVolts(BATTERY_ADC_PIN);
  uint16_t sample = (uint16_t)(2 * sum / 16);  // undo the 1:2 divider

  if (sample < BATTERY_MIN_PLAUSIBLE_MV || sample > BATTERY_MAX_PLAUSIBLE_MV) {
    mv = 0;
    pct = BATTERY_UNKNOWN;
    return;
  }

  // Exponential smoothing. Under a 0.5C load on a small cell the terminal
  // voltage sags visibly with every transmit burst; without this the reported
  // percentage would jitter by several points second to second.
  mv = mv ? (uint16_t)((mv * 3 + sample) / 4) : sample;
  pct = curveLookup(mv);
}

uint16_t batteryMillivolts() { return mv; }
uint8_t batteryPercent() { return pct; }
bool batteryPresent() { return mv != 0; }
