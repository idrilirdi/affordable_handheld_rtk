#include "status_led.h"

#include "config.h"

#include <math.h>

#define CYCLE_MS 2000u
#define BLIP 60u          // long enough to register, short enough to be cheap
#define LONG_BLIP 220u

// True for `width` starting at each of the given offsets in the cycle.
static bool pulses(uint32_t t, uint32_t width, uint32_t a, uint32_t b = 0xFFFFFFFF,
                   uint32_t c = 0xFFFFFFFF) {
  return (t >= a && t < a + width) || (b != 0xFFFFFFFF && t >= b && t < b + width) ||
         (c != 0xFFFFFFFF && t >= c && t < c + width);
}

Lamps statusPattern(Status s, uint32_t tMs) {
  const uint32_t t = tMs % CYCLE_MS;
  Lamps l = {false, false, false};
  switch (s) {
    case Status::NoFix:
      l.red = pulses(t, BLIP, 0);
      break;
    case Status::Acquiring:
      l.yellow = pulses(t, BLIP, 0);
      break;
    case Status::Single:
      l.yellow = pulses(t, BLIP, 0, 300);
      break;
    case Status::Diff:
      l.yellow = pulses(t, LONG_BLIP, 0);
      break;
    case Status::Float:
      // Two green blips: converging, not there yet.
      l.green = pulses(t, BLIP, 0, 300);
      break;
    case Status::FloatPoor:
      // Float, but the receiver rates it worse than a meter - no better than
      // single, so the single pattern follows the float one.
      l.green = pulses(t, BLIP, 0, 300);
      l.yellow = pulses(t, BLIP, 1000, 1300);
      break;
    case Status::FloatSteady:
      // Rated worse than a meter, but holding still: the estimate is likely
      // pessimistic. A long yellow, as "fixed" is a long green.
      l.green = pulses(t, BLIP, 0, 300);
      l.yellow = pulses(t, LONG_BLIP, 1000);
      break;
    case Status::Fixed:
      // One long green. The state you are waiting for should look settled.
      l.green = pulses(t, LONG_BLIP, 0);
      break;
    case Status::NoCorrections:
      // Deliberately the most insistent thing the device can do: this is the
      // failure that looks like everything working, and a walked circuit lost
      // its fix to it for 19 s without anyone noticing.
      l.red = pulses(t, BLIP, 0, 250, 500);
      break;
    case Status::Provisioning:
      // Alternating, so it cannot be mistaken for a survey state.
      l.red = pulses(t, 250, 0);
      l.green = pulses(t, 250, 250);
      break;
    case Status::LowBattery:
      l.red = pulses(t, LONG_BLIP, 0, 400);
      break;
  }
  return l;
}

// A float is only worth its green when the receiver rates it under a meter:
// above that it is no better than the single fix it replaced, and a lamp that
// says "RTK" over a 12 m solution is worse than no lamp.
static Status floatStatus(const GnssFix &fix, float scatterM) {
  if (!fix.haveAcc) return Status::Float;   // nothing to judge it by
  const float h = sqrtf(fix.latStd * fix.latStd + fix.lonStd * fix.lonStd);
  if (h <= FLOAT_GOOD_ACC_M) return Status::Float;
  if (scatterM >= 0 && scatterM <= FLOAT_STEADY_SCATTER_M) return Status::FloatSteady;
  return Status::FloatPoor;
}

Status statusOf(const GnssFix &fix, float correctionAge, uint8_t batteryPercent,
                bool provisioning, float floatScatterM) {
  if (provisioning) return Status::Provisioning;
  // Below this the cell has minutes, not hours, and that outranks fix quality:
  // a survey that dies mid-point is worse than one that never started.
  if (batteryPercent != BATTERY_UNKNOWN && batteryPercent <= 5) return Status::LowBattery;

  // Corrections that were arriving and stopped. Not the same as never having
  // had any - the receiver holds its solution for a while and looks fine.
  const bool stale = correctionAge > 5.0f;
  if (stale && fix.type >= PosType::Dgps) return Status::NoCorrections;

  switch (fix.type) {
    case PosType::Fixed: return Status::Fixed;
    case PosType::Float: return floatStatus(fix, floatScatterM);
    case PosType::Dgps: return Status::Diff;
    case PosType::Single: return Status::Single;
    default: break;
  }
  return fix.sats > 0 ? Status::Acquiring : Status::NoFix;
}
