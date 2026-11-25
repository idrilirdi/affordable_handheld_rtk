// Host-native tests for the status lamps.
//
// These are the only thing the device says to someone who is not holding a
// phone, so a pattern that is ambiguous, or a state that maps to the wrong
// lamp, is a person walking a survey believing something untrue.
//
//   bazel test //test:status_led_test

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "status_led.h"
#include "scatter.h"

// A cycle rendered as text, sampled every 10 ms: 'R', 'Y', 'G' or '.'.
static std::string cycle(Status s) {
  std::string out;
  for (uint32_t t = 0; t < 2000; t += 10) {
    Lamps l = statusPattern(s, t);
    out += l.red ? 'R' : l.yellow ? 'Y' : l.green ? 'G' : '.';
  }
  return out;
}

static int lit(const std::string &c, char ch) {
  int n = 0;
  for (char x : c) if (x == ch) n++;
  return n;
}

static int bursts(const std::string &c, char ch) {
  int n = 0;
  bool in = false;
  for (char x : c) {
    if (x == ch && !in) { n++; in = true; }
    else if (x != ch) in = false;
  }
  return n;
}

static GnssFix mk(PosType t, uint8_t sats, float age) {
  GnssFix f;
  f.type = t;
  f.sats = sats;
  f.diffAge = age;
  return f;
}

TEST_CASE("one lamp at a time", "[status_led]") {
  bool clash = false;
  for (int s = 0; s <= (int)Status::LowBattery; ++s)
    for (uint32_t t = 0; t < 2000; t += 5) {
      Lamps l = statusPattern((Status)s, t);
      if ((l.red && l.yellow) || (l.red && l.green) || (l.yellow && l.green))
        clash = true;
    }
  // no two lamps ever lit together
  CHECK(!clash);
}

TEST_CASE("patterns are distinguishable", "[status_led]") {
  // fixed is one long green
  CHECK((bursts(cycle(Status::Fixed), 'G') == 1 && lit(cycle(Status::Fixed), 'G') > 15));
  // float is two short greens
  CHECK(bursts(cycle(Status::Float), 'G') == 2);
  // acquiring is one yellow
  CHECK(bursts(cycle(Status::Acquiring), 'Y') == 1);
  // single is two yellows
  CHECK(bursts(cycle(Status::Single), 'Y') == 2);
  // no fix is one red
  CHECK(bursts(cycle(Status::NoFix), 'R') == 1);
  // lost corrections is three reds
  CHECK(bursts(cycle(Status::NoCorrections), 'R') == 3);
  // fixed and float differ
  CHECK(cycle(Status::Fixed) != cycle(Status::Float));
  // poor float adds two yellows
  CHECK((bursts(cycle(Status::FloatPoor), 'G') == 2 && bursts(cycle(Status::FloatPoor), 'Y') == 2));
  // steady float adds one long yellow
  CHECK((bursts(cycle(Status::FloatSteady), 'G') == 2 &&
         bursts(cycle(Status::FloatSteady), 'Y') == 1 &&
         lit(cycle(Status::FloatSteady), 'Y') > 15));
  // the three floats differ
  CHECK((cycle(Status::Float) != cycle(Status::FloatPoor) &&
         cycle(Status::FloatPoor) != cycle(Status::FloatSteady)));
}

TEST_CASE("lamps are cheap", "[status_led]") {
  // A steady lamp would be a tenth of the device's draw. Nothing may exceed
  // a fifth of the cycle except the deliberately loud states.
  for (Status s : {Status::Fixed, Status::Float, Status::FloatPoor, Status::FloatSteady,
                   Status::Single,
                   Status::Acquiring, Status::NoFix}) {
    std::string c = cycle(s);
    int on = 200 - lit(c, '.');
    // duty stays under a fifth
    CHECK(on <= 40);
  }
}

TEST_CASE("state mapping", "[status_led]") {
  // fixed
  CHECK(statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 80, false) == Status::Fixed);
  // float
  CHECK(statusOf(mk(PosType::Float, 30, 1.0f), 1.0f, 80, false) == Status::Float);

  // The bad-sky case: float, receiver claiming 12 m east and 9 m north.
  GnssFix bad = mk(PosType::Float, 36, 1.0f);
  bad.haveAcc = true;
  bad.latStd = 9.3f;
  bad.lonStd = 12.3f;
  // float worse than 1 m is poor
  CHECK(statusOf(bad, 1.0f, 80, false) == Status::FloatPoor);
  // poor float, scatter unknown, stays poor
  CHECK(statusOf(bad, 1.0f, 80, false, -1.0f) == Status::FloatPoor);
  // poor float, wandering, stays poor
  CHECK(statusOf(bad, 1.0f, 80, false, 2.0f) == Status::FloatPoor);
  // poor float holding still is steady
  CHECK(statusOf(bad, 1.0f, 80, false, 0.2f) == Status::FloatSteady);
  GnssFix good = bad;
  good.latStd = 0.4f;
  good.lonStd = 0.5f;
  // float within 1 m is plain float
  CHECK(statusOf(good, 1.0f, 80, false, 2.0f) == Status::Float);
  // sbas reads as differential
  CHECK(statusOf(mk(PosType::Dgps, 20, -1.0f), -1.0f, 80, false) == Status::Diff);
  // tracking but no position is acquiring
  CHECK(statusOf(mk(PosType::None, 12, -1.0f), -1.0f, 80, false) == Status::Acquiring);
  // nothing at all is no fix
  CHECK(statusOf(mk(PosType::None, 0, -1.0f), -1.0f, 80, false) == Status::NoFix);
}

TEST_CASE("the failure that looks like success", "[status_led]") {
  // The walked circuit lost its fix to a 19 s correction outage while the
  // sky stayed perfect. The receiver still reported FLOAT throughout, so the
  // fix state alone would have said nothing was wrong.
  // stale corrections outrank the fix type
  CHECK(statusOf(mk(PosType::Float, 34, 19.0f), 19.0f, 80, false) == Status::NoCorrections);
  // fresh corrections do not
  CHECK(statusOf(mk(PosType::Float, 34, 1.0f), 1.0f, 80, false) == Status::Float);
  // never having had any is only SINGLE, not an alarm
  CHECK(statusOf(mk(PosType::Single, 20, -1.0f), -1.0f, 80, false) == Status::Single);
}

TEST_CASE("priorities", "[status_led]") {
  // the portal is unmistakable
  CHECK(statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 80, true) == Status::Provisioning);
  // a flat cell outranks a good fix
  CHECK(statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 3, false) == Status::LowBattery);
  // an unknown cell does not raise an alarm
  CHECK(statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 0xFF, false) == Status::Fixed);
}

TEST_CASE("scatter", "[status_led]") {
  Scatter sc;
  // unknown before enough samples
  CHECK(sc.rms(10) < 0);
  for (int i = 0; i < 30; ++i) sc.add(48.1372000, 11.5755000);
  // still position scatters zero
  CHECK((sc.rms(10) >= 0 && sc.rms(10) < 0.001f));
  sc.clear();
  // Alternating 1 m north and south of a point: rms exactly 1 m.
  const double dLat = 1.0 / 111320.0;
  for (int i = 0; i < 30; ++i) sc.add(48.1372 + (i % 2 ? dLat : -dLat), 11.5755);
  // 1 m swing reads 1 m
  CHECK((sc.rms(10) > 0.99f && sc.rms(10) < 1.01f));
  // The window forgets: thirty still samples later, the swing is gone.
  for (int i = 0; i < 30; ++i) sc.add(48.1372, 11.5755);
  // old samples age out
  CHECK(sc.rms(10) < 0.001f);
}
