// Host-native tests for the field statistics.
//
// These numbers are the only evidence available while a survey is running -
// the USB console cannot be attached without resetting the receiver - and they
// are what a decision like "is this firmware worse than the last one" rests on.
// A percentage that quietly counts the wrong samples would not look wrong.

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/fieldstats.h"

static int failures = 0;
static void check(const char *what, bool ok) {
  printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

static GnssFix mk(PosType t, float acc, float age, double dLat = 0, double dLon = 0) {
  GnssFix f;
  f.type = t;
  f.lat = 48.1372 + dLat;
  f.lon = 11.5755 + dLon;
  f.havePos = true;
  f.latStd = acc;
  f.lonStd = 0;
  f.haveAcc = acc >= 0;
  f.diffAge = age;
  return f;
}

// One sample per second unless a test says otherwise.
static uint32_t ms(uint32_t sampleIndex) { return (sampleIndex + 1) * 1000; }

static std::string render(const FieldStats &s) {
  char buf[512];
  size_t n = s.render(buf, sizeof(buf));
  return std::string(buf, n);
}

static bool has(const std::string &h, const char *needle) {
  return h.find(needle) != std::string::npos;
}

int main() {
  printf("priming\n");
  {
    FieldStats s;
    check("nothing to show before any sample", has(render(s), "collecting"));
    // The first call only adopts the epoch counter; it is not a data point.
    s.sample(mk(PosType::Fixed, 0.01f, 1), 5000, 1000);
    check("first sample is not counted", s.count() == 0);
    s.sample(mk(PosType::Fixed, 0.01f, 1), 5001, 2000);
    check("second sample is", s.count() == 1);
    check("epoch rate ignores the pre-boot backlog", has(render(s), "RATE  1.00"));
  }

  printf("shares\n");
  {
    FieldStats s;
    s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
    for (int i = 0; i < 25; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1), i + 1, ms(i + 1));
    for (int i = 0; i < 75; ++i) s.sample(mk(PosType::Float, 1.0f, 1), 26 + i, ms(26 + i));
    std::string r = render(s);
    check("100 samples counted", s.count() == 100);
    check("fixed share", has(r, "fix 25%"));
    check("float share", has(r, "float 75%"));
    check("longest fixed run", has(r, "longest fix 25 s"));
  }

  printf("resets\n");
  {
    // Fixed -> float -> fixed -> single is two losses of solution, not three
    // transitions: climbing back up is not a failure.
    FieldStats s;
    s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
    const PosType seq[] = {PosType::Fixed, PosType::Float, PosType::Fixed,
                           PosType::Single, PosType::Single};
    for (int i = 0; i < 5; ++i) s.sample(mk(seq[i], 1.0f, 1), i + 1, ms(i + 1));
    check("only downward transitions count", has(render(s), "DROPS 2 type"));
  }

  printf("silent re-convergence\n");
  {
    // Observed low beside a building: FLOAT throughout, so no type ever
    // changed, while the accuracy walked to sub-meter and was thrown away
    // twice. A stability report that calls this session clean is useless.
    FieldStats s;
    s.sample(mk(PosType::Float, 8.0f, 1), 0, ms(0));
    const float walk[] = {8.0f, 4.0f, 1.0f, 0.7f,   // converging
                          6.5f, 3.0f, 0.9f,         // thrown away, converging
                          5.5f, 2.0f, 1.2f};        // thrown away again
    for (int i = 0; i < 10; ++i) s.sample(mk(PosType::Float, walk[i], 1), i + 1, ms(i + 1));
    std::string r = render(s);
    check("type never changed", has(r, "float 100%"));
    check("no type drops reported", has(r, "DROPS 0 type"));
    check("but the accuracy resets are", has(r, "0 type  2 accuracy"));
  }

  printf("accuracy\n");
  {
    // A median that a single spike cannot drag: nine samples at 1 m and one at
    // 50 m is a meter-level session with one outlier, not a 5.9 m one.
    FieldStats s;
    s.sample(mk(PosType::Float, 1.0f, 1), 0, ms(0));
    for (int i = 0; i < 9; ++i) s.sample(mk(PosType::Float, 1.0f, 1), i + 1, ms(i + 1));
    s.sample(mk(PosType::Float, 50.0f, 1), 10, ms(10));
    std::string r = render(s);
    check("median resists the outlier", has(r, "med 1.00"));
    check("but the outlier is still reported", has(r, "max 50.00"));
    check("and the best too", has(r, "min 1.00"));
  }

  printf("corrections\n");
  {
    // An absent age is the interesting case: it means the engine is not
    // applying corrections at all, which must not read as a healthy 0 s.
    FieldStats s;
    s.sample(mk(PosType::Single, -1, -1), 0, ms(0));
    for (int i = 0; i < 4; ++i) s.sample(mk(PosType::Single, -1, -1), i + 1, ms(i + 1));
    std::string r = render(s);
    check("missing corrections count as stale", has(r, "stale 4"));
    check("no accuracy is said, not invented", has(r, "no accuracy estimate"));
  }
  {
    FieldStats s;
    s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
    for (int i = 0; i < 3; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1.0f), i + 1, ms(i + 1));
    s.sample(mk(PosType::Fixed, 0.01f, 9.0f), 4, ms(4));
    std::string r = render(s);
    check("an aged correction is flagged", has(r, "stale 1"));
    check("worst age is kept", has(r, "max 9.0 s"));
  }

  printf("window\n");
  {
    // The ring must forget: a session that was single for an hour and fixed for
    // the last two minutes reads as fixed.
    FieldStats s;
    s.sample(mk(PosType::Single, 5.0f, 1), 0, ms(0));
    for (int i = 0; i < 400; ++i) s.sample(mk(PosType::Single, 5.0f, 1), i + 1, ms(i + 1));
    for (int i = 0; i < STAT_WINDOW; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1), 401 + i, ms(401 + i));
    std::string r = render(s);
    check("window is bounded", s.count() == STAT_WINDOW);
    check("old samples are gone", has(r, "fix 100%"));
  }

  printf("starved loop\n");
  {
    // Observed on the bench: the loop reached the sampler only every ~4 s while
    // the receiver kept producing epochs at 1 Hz. The window must then say it
    // covers 40 s, not 10, and must still report 1 Hz rather than 4.
    FieldStats s;
    s.sample(mk(PosType::Float, 1.0f, 1), 0, 0);
    for (int i = 1; i <= 10; ++i)
      s.sample(mk(PosType::Float, 1.0f, 1), i * 4, i * 4000);
    std::string r = render(s);
    check("window reports real seconds", has(r, "last 40 s"));
    check("and how few samples that was", has(r, "(10 samples)"));
    check("epoch rate is per real second", has(r, "1.00 Hz"));
    check("sampling rate is exposed", has(r, "sampling 0.25 Hz"));
  }

  printf("scatter\n");
  {
    // The receiver's own claim and what the position actually did are two
    // different measurements, and only the second one is evidence. A meter of
    // north-south wander must show up even while GST insists on centimeters.
    FieldStats s;
    s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
    for (int i = 0; i < 20; ++i) {
      // +/- 1 m about the mean, alternating, with a confident 1 cm claim
      const double d = (i % 2 ? 1.0 : -1.0) / 111320.0;
      s.sample(mk(PosType::Fixed, 0.01f, 1, d), i + 1, ms(i + 1));
    }
    std::string r = render(s);
    check("claim stays optimistic", has(r, "med 0.01"));
    check("scatter tells the truth", has(r, "rms 1.00"));
    check("and counts the fixes it used", has(r, "(20 fixes)"));
  }
  {
    FieldStats s;
    GnssFix none;
    none.havePos = false;
    none.type = PosType::None;
    s.sample(none, 0, ms(0));
    for (int i = 0; i < 5; ++i) s.sample(none, i + 1, ms(i + 1));
    check("no position, no invented scatter", has(render(s), "no position in window"));
  }

  printf("truncation\n");
  {
    // The panel is assembled into one buffer; a short one must not be written
    // past, and must not leave the offset beyond its end for the next writer.
    FieldStats s;
    s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
    for (int i = 0; i < 20; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1), i + 1, ms(i + 1));
    char small[40];
    memset(small, 0x7F, sizeof(small));
    size_t n = s.render(small, sizeof(small));
    check("stays inside the buffer", n < sizeof(small));
    check("terminated", memchr(small, 0, sizeof(small)) != nullptr);
    check("nothing written past the end", small[sizeof(small) - 1] == 0x7F ||
                                              strlen(small) < sizeof(small));
    check("appendf clamps a full buffer", appendf(small, sizeof(small), 999, "x") ==
                                              sizeof(small) - 1);
  }

  printf(failures ? "\n%d FAILED\n" : "\nall passed (%d failures)\n", failures);
  return failures ? 1 : 0;
}
