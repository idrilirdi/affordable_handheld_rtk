// Host-native tests for the status lamps.
//
// These are the only thing the device says to someone who is not holding a
// phone, so a pattern that is ambiguous, or a state that maps to the wrong
// lamp, is a person walking a survey believing something untrue.

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/status_led.h"
#include "../src/scatter.h"

static int failures = 0;
static void check(const char *what, bool ok) {
  printf("  %-48s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

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

int main() {
  printf("one lamp at a time\n");
  {
    bool clash = false;
    for (int s = 0; s <= (int)Status::LowBattery; ++s)
      for (uint32_t t = 0; t < 2000; t += 5) {
        Lamps l = statusPattern((Status)s, t);
        if ((l.red && l.yellow) || (l.red && l.green) || (l.yellow && l.green))
          clash = true;
      }
    check("no two lamps ever lit together", !clash);
  }

  printf("patterns are distinguishable\n");
  {
    check("fixed is one long green", bursts(cycle(Status::Fixed), 'G') == 1 &&
                                         lit(cycle(Status::Fixed), 'G') > 15);
    check("float is two short greens", bursts(cycle(Status::Float), 'G') == 2);
    check("acquiring is one yellow", bursts(cycle(Status::Acquiring), 'Y') == 1);
    check("single is two yellows", bursts(cycle(Status::Single), 'Y') == 2);
    check("no fix is one red", bursts(cycle(Status::NoFix), 'R') == 1);
    check("lost corrections is three reds",
          bursts(cycle(Status::NoCorrections), 'R') == 3);
    check("fixed and float differ", cycle(Status::Fixed) != cycle(Status::Float));
    check("poor float adds two yellows", bursts(cycle(Status::FloatPoor), 'G') == 2 &&
                                             bursts(cycle(Status::FloatPoor), 'Y') == 2);
    check("steady float adds one long yellow",
          bursts(cycle(Status::FloatSteady), 'G') == 2 &&
              bursts(cycle(Status::FloatSteady), 'Y') == 1 &&
              lit(cycle(Status::FloatSteady), 'Y') > 15);
    check("the three floats differ", cycle(Status::Float) != cycle(Status::FloatPoor) &&
                                         cycle(Status::FloatPoor) != cycle(Status::FloatSteady));
  }

  printf("lamps are cheap\n");
  {
    // A steady lamp would be a tenth of the device's draw. Nothing may exceed
    // a fifth of the cycle except the deliberately loud states.
    for (Status s : {Status::Fixed, Status::Float, Status::FloatPoor, Status::FloatSteady,
                     Status::Single,
                     Status::Acquiring, Status::NoFix}) {
      std::string c = cycle(s);
      int on = 200 - lit(c, '.');
      check("duty stays under a fifth", on <= 40);
    }
  }

  printf("state mapping\n");
  {
    check("fixed", statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 80, false) ==
                       Status::Fixed);
    check("float", statusOf(mk(PosType::Float, 30, 1.0f), 1.0f, 80, false) ==
                       Status::Float);

    // The bad-sky case: float, receiver claiming 12 m east and 9 m north.
    GnssFix bad = mk(PosType::Float, 36, 1.0f);
    bad.haveAcc = true;
    bad.latStd = 9.3f;
    bad.lonStd = 12.3f;
    check("float worse than 1 m is poor", statusOf(bad, 1.0f, 80, false) == Status::FloatPoor);
    check("poor float, scatter unknown, stays poor",
          statusOf(bad, 1.0f, 80, false, -1.0f) == Status::FloatPoor);
    check("poor float, wandering, stays poor",
          statusOf(bad, 1.0f, 80, false, 2.0f) == Status::FloatPoor);
    check("poor float holding still is steady",
          statusOf(bad, 1.0f, 80, false, 0.2f) == Status::FloatSteady);
    GnssFix good = bad;
    good.latStd = 0.4f;
    good.lonStd = 0.5f;
    check("float within 1 m is plain float", statusOf(good, 1.0f, 80, false, 2.0f) == Status::Float);
    check("sbas reads as differential",
          statusOf(mk(PosType::Dgps, 20, -1.0f), -1.0f, 80, false) == Status::Diff);
    check("tracking but no position is acquiring",
          statusOf(mk(PosType::None, 12, -1.0f), -1.0f, 80, false) == Status::Acquiring);
    check("nothing at all is no fix",
          statusOf(mk(PosType::None, 0, -1.0f), -1.0f, 80, false) == Status::NoFix);
  }

  printf("the failure that looks like success\n");
  {
    // The walked circuit lost its fix to a 19 s correction outage while the
    // sky stayed perfect. The receiver still reported FLOAT throughout, so the
    // fix state alone would have said nothing was wrong.
    check("stale corrections outrank the fix type",
          statusOf(mk(PosType::Float, 34, 19.0f), 19.0f, 80, false) ==
              Status::NoCorrections);
    check("fresh corrections do not",
          statusOf(mk(PosType::Float, 34, 1.0f), 1.0f, 80, false) == Status::Float);
    check("never having had any is only SINGLE, not an alarm",
          statusOf(mk(PosType::Single, 20, -1.0f), -1.0f, 80, false) == Status::Single);
  }

  printf("priorities\n");
  {
    check("the portal is unmistakable",
          statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 80, true) == Status::Provisioning);
    check("a flat cell outranks a good fix",
          statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 3, false) == Status::LowBattery);
    check("an unknown cell does not raise an alarm",
          statusOf(mk(PosType::Fixed, 30, 1.0f), 1.0f, 0xFF, false) == Status::Fixed);
  }

  {
    printf("scatter\n");
    Scatter sc;
    check("unknown before enough samples", sc.rms(10) < 0);
    for (int i = 0; i < 30; ++i) sc.add(48.1372000, 11.5755000);
    check("still position scatters zero", sc.rms(10) >= 0 && sc.rms(10) < 0.001f);
    sc.clear();
    // Alternating 1 m north and south of a point: rms exactly 1 m.
    const double dLat = 1.0 / 111320.0;
    for (int i = 0; i < 30; ++i) sc.add(48.1372 + (i % 2 ? dLat : -dLat), 11.5755);
    check("1 m swing reads 1 m", sc.rms(10) > 0.99f && sc.rms(10) < 1.01f);
    // The window forgets: thirty still samples later, the swing is gone.
    for (int i = 0; i < 30; ++i) sc.add(48.1372, 11.5755);
    check("old samples age out", sc.rms(10) < 0.001f);
  }

  printf(failures ? "\n%d FAILED\n" : "\nall passed (%d failures)\n", failures);
  return failures ? 1 : 0;
}
