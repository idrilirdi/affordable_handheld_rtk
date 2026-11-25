// Host-native tests for the field statistics.
//
// These numbers are the only evidence available while a survey is running -
// the USB console cannot be attached without resetting the receiver - and they
// are what a decision like "is this firmware worse than the last one" rests on.
// A percentage that quietly counts the wrong samples would not look wrong.
//
//   bazel test //test:fieldstats_test

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "fieldstats.h"

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

TEST_CASE("priming", "[fieldstats]") {
  FieldStats s;
  // nothing to show before any sample
  CHECK(has(render(s), "collecting"));
  // The first call only adopts the epoch counter; it is not a data point.
  s.sample(mk(PosType::Fixed, 0.01f, 1), 5000, 1000);
  // first sample is not counted
  CHECK(s.count() == 0);
  s.sample(mk(PosType::Fixed, 0.01f, 1), 5001, 2000);
  // second sample is
  CHECK(s.count() == 1);
  // epoch rate ignores the pre-boot backlog
  CHECK(has(render(s), "RATE  1.00"));
}

TEST_CASE("shares", "[fieldstats]") {
  FieldStats s;
  s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
  for (int i = 0; i < 25; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1), i + 1, ms(i + 1));
  for (int i = 0; i < 75; ++i) s.sample(mk(PosType::Float, 1.0f, 1), 26 + i, ms(26 + i));
  std::string r = render(s);
  // 100 samples counted
  CHECK(s.count() == 100);
  // fixed share
  CHECK(has(r, "fix 25%"));
  // float share
  CHECK(has(r, "float 75%"));
  // longest fixed run
  CHECK(has(r, "longest fix 25 s"));
}

TEST_CASE("resets", "[fieldstats]") {
  // Fixed -> float -> fixed -> single is two losses of solution, not three
  // transitions: climbing back up is not a failure.
  FieldStats s;
  s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
  const PosType seq[] = {PosType::Fixed, PosType::Float, PosType::Fixed,
                         PosType::Single, PosType::Single};
  for (int i = 0; i < 5; ++i) s.sample(mk(seq[i], 1.0f, 1), i + 1, ms(i + 1));
  // only downward transitions count
  CHECK(has(render(s), "DROPS 2 type"));
}

TEST_CASE("silent re-convergence", "[fieldstats]") {
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
  // type never changed
  CHECK(has(r, "float 100%"));
  // no type drops reported
  CHECK(has(r, "DROPS 0 type"));
  // but the accuracy resets are
  CHECK(has(r, "0 type  2 accuracy"));
}

TEST_CASE("accuracy", "[fieldstats]") {
  // A median that a single spike cannot drag: nine samples at 1 m and one at
  // 50 m is a meter-level session with one outlier, not a 5.9 m one.
  FieldStats s;
  s.sample(mk(PosType::Float, 1.0f, 1), 0, ms(0));
  for (int i = 0; i < 9; ++i) s.sample(mk(PosType::Float, 1.0f, 1), i + 1, ms(i + 1));
  s.sample(mk(PosType::Float, 50.0f, 1), 10, ms(10));
  std::string r = render(s);
  // median resists the outlier
  CHECK(has(r, "med 1.00"));
  // but the outlier is still reported
  CHECK(has(r, "max 50.00"));
  // and the best too
  CHECK(has(r, "min 1.00"));
}

TEST_CASE("corrections", "[fieldstats]") {
  {
    // An absent age is the interesting case: it means the engine is not
    // applying corrections at all, which must not read as a healthy 0 s.
    FieldStats s;
    s.sample(mk(PosType::Single, -1, -1), 0, ms(0));
    for (int i = 0; i < 4; ++i) s.sample(mk(PosType::Single, -1, -1), i + 1, ms(i + 1));
    std::string r = render(s);
    // missing corrections count as stale
    CHECK(has(r, "stale 4"));
    // no accuracy is said, not invented
    CHECK(has(r, "no accuracy estimate"));
  }
  {
    FieldStats s;
    s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
    for (int i = 0; i < 3; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1.0f), i + 1, ms(i + 1));
    s.sample(mk(PosType::Fixed, 0.01f, 9.0f), 4, ms(4));
    std::string r = render(s);
    // an aged correction is flagged
    CHECK(has(r, "stale 1"));
    // worst age is kept
    CHECK(has(r, "max 9.0 s"));
  }
}

TEST_CASE("window", "[fieldstats]") {
  // The ring must forget: a session that was single for an hour and fixed for
  // the last two minutes reads as fixed.
  FieldStats s;
  s.sample(mk(PosType::Single, 5.0f, 1), 0, ms(0));
  for (int i = 0; i < 400; ++i) s.sample(mk(PosType::Single, 5.0f, 1), i + 1, ms(i + 1));
  for (int i = 0; i < STAT_WINDOW; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1), 401 + i, ms(401 + i));
  std::string r = render(s);
  // window is bounded
  CHECK(s.count() == STAT_WINDOW);
  // old samples are gone
  CHECK(has(r, "fix 100%"));
}

TEST_CASE("starved loop", "[fieldstats]") {
  // Observed on the bench: the loop reached the sampler only every ~4 s while
  // the receiver kept producing epochs at 1 Hz. The window must then say it
  // covers 40 s, not 10, and must still report 1 Hz rather than 4.
  FieldStats s;
  s.sample(mk(PosType::Float, 1.0f, 1), 0, 0);
  for (int i = 1; i <= 10; ++i)
    s.sample(mk(PosType::Float, 1.0f, 1), i * 4, i * 4000);
  std::string r = render(s);
  // window reports real seconds
  CHECK(has(r, "last 40 s"));
  // and how few samples that was
  CHECK(has(r, "(10 samples)"));
  // epoch rate is per real second
  CHECK(has(r, "1.00 Hz"));
  // sampling rate is exposed
  CHECK(has(r, "sampling 0.25 Hz"));
}

TEST_CASE("scatter", "[fieldstats]") {
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
    // claim stays optimistic
    CHECK(has(r, "med 0.01"));
    // scatter tells the truth
    CHECK(has(r, "rms 1.00"));
    // and counts the fixes it used
    CHECK(has(r, "(20 fixes)"));
  }
  {
    FieldStats s;
    GnssFix none;
    none.havePos = false;
    none.type = PosType::None;
    s.sample(none, 0, ms(0));
    for (int i = 0; i < 5; ++i) s.sample(none, i + 1, ms(i + 1));
    // no position, no invented scatter
    CHECK(has(render(s), "no position in window"));
  }
}

TEST_CASE("truncation", "[fieldstats]") {
  // The panel is assembled into one buffer; a short one must not be written
  // past, and must not leave the offset beyond its end for the next writer.
  FieldStats s;
  s.sample(mk(PosType::Fixed, 0.01f, 1), 0, ms(0));
  for (int i = 0; i < 20; ++i) s.sample(mk(PosType::Fixed, 0.01f, 1), i + 1, ms(i + 1));
  char small[40];
  memset(small, 0x7F, sizeof(small));
  size_t n = s.render(small, sizeof(small));
  // stays inside the buffer
  CHECK(n < sizeof(small));
  // terminated
  CHECK(memchr(small, 0, sizeof(small)) != nullptr);
  // nothing written past the end
  CHECK((small[sizeof(small) - 1] == 0x7F || strlen(small) < sizeof(small)));
  // appendf clamps a full buffer
  CHECK(appendf(small, sizeof(small), 999, "x") == sizeof(small) - 1);
}
