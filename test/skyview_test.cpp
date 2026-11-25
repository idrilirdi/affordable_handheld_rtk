// Host-native tests for the sky view.
//
// This is the only evidence the firmware has about its own input. If it
// miscounts, a perfectly good antenna looks broken, or - worse - a starved one
// looks fine and we go on blaming the receiver.
//
//   bazel test //test:skyview_test

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "nmea.h"
#include "skyview.h"

// Append the NMEA checksum and feed the sentence to a parser.
static void feed(NmeaParser &p, const std::string &body) {
  uint8_t sum = 0;
  for (size_t i = 1; i < body.size(); ++i) sum ^= (uint8_t)body[i];
  char line[128];
  snprintf(line, sizeof(line), "%s*%02X\r\n", body.c_str(), sum);
  p.feed((const uint8_t *)line, strlen(line));
}

static std::string render(const SkyView &s) {
  char buf[512];
  size_t n = s.render(buf, sizeof(buf));
  return std::string(buf, n);
}

static bool has(const std::string &h, const char *n) { return h.find(n) != std::string::npos; }

TEST_CASE("counting", "[skyview]") {
  NmeaParser p;
  // Four GPS satellites, one of them listed but not tracked (empty C/N0).
  feed(p, "$GPGSV,1,1,04,01,70,113,45,02,55,220,41,03,30,050,38,04,10,300,,1");
  const SkyView &s = p.sky();
  // all four are visible
  CHECK(s.listed() == 4);
  // only three are tracked
  CHECK(s.tracked() == 3);
  // median of 38/41/45
  CHECK(s.medianCno() == 41);
  // two at or above 40
  CHECK(s.strong(40) == 2);
  // rendered as tracked over visible
  CHECK(has(render(s), "GPS 3/4"));
}

TEST_CASE("signal ids are per constellation", "[skyview]") {
  // Signal 1 is L1 C/A for GPS but E5a for Galileo. Treating the number as
  // universal would report L5 satellites as L1 and hide exactly the band we
  // care about for RTK.
  NmeaParser p;
  feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
  feed(p, "$GAGSV,1,1,01,01,70,113,44,1");
  const SkyView &s = p.sky();
  // GPS signal 1 is L1
  CHECK(s.bandCount(1) == 1);
  // Galileo signal 1 is L5
  CHECK(s.bandCount(5) == 1);
  // and they are two different satellites
  CHECK(s.tracked() == 2);
}

TEST_CASE("multi sentence sets", "[skyview]") {
  NmeaParser p;
  feed(p, "$GPGSV,2,1,05,01,70,113,45,02,55,220,41,03,30,050,38,04,10,300,35,1");
  feed(p, "$GPGSV,2,2,05,05,20,150,33,1");
  // both sentences accumulate
  CHECK(p.sky().tracked() == 5);
}

TEST_CASE("a set replaces its predecessor", "[skyview]") {
  // A satellite that sets must disappear, not linger and flatter the count.
  NmeaParser p;
  feed(p, "$GPGSV,1,1,03,01,70,113,45,02,55,220,41,03,30,050,38,1");
  // three to begin with
  CHECK(p.sky().tracked() == 3);
  feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
  // one after the new set
  CHECK(p.sky().tracked() == 1);
}

TEST_CASE("constellations are kept apart", "[skyview]") {
  // A new GPS set must not discard the Galileo satellites, and the same PRN
  // in two constellations is two satellites.
  NmeaParser p;
  feed(p, "$GAGSV,1,1,01,01,70,113,44,7");
  feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
  feed(p, "$GPGSV,1,1,01,02,60,100,46,1");
  const SkyView &s = p.sky();
  // Galileo survived the GPS set
  CHECK(s.tracked() == 2);
  std::string r = render(s);
  // counted separately
  CHECK((has(r, "GPS 1/1") && has(r, "GAL 1/1")));
}

TEST_CASE("bands", "[skyview]") {
  // The question that matters in a poor spot: are we getting a second and
  // third frequency, or only L1?
  NmeaParser p;
  feed(p, "$GPGSV,1,1,02,01,70,113,45,02,55,220,44,1");   // L1 C/A
  feed(p, "$GPGSV,1,1,02,01,70,113,40,02,55,220,39,5");   // L2 C-M
  feed(p, "$GPGSV,1,1,02,01,70,113,38,02,55,220,37,7");   // L5 I
  const SkyView &s = p.sky();
  std::string r = render(s);
  // three bands counted
  CHECK(has(r, "BAND  L1 2  L2 2  L5 2"));
  // but only two spacecraft
  CHECK(has(r, "GPS 2/2"));
}

TEST_CASE("elevation", "[skyview]") {
  // The discriminator between a weak antenna and an obstructed sky. Two
  // satellites overhead at 46 dB-Hz and four near the horizon at 26 give an
  // overall median of 26 - which says nothing about the antenna - while the
  // high-elevation median says it is working perfectly well.
  NmeaParser p;
  feed(p, "$GPGSV,2,1,06,01,80,113,46,02,75,220,46,03,10,050,26,04,08,300,26,1");
  feed(p, "$GPGSV,2,2,06,05,07,150,26,06,05,010,26,1");
  const SkyView &s = p.sky();
  // overall median is dragged down
  CHECK(s.medianCno() == 26);
  // but overhead is strong
  CHECK(s.medianCnoAbove(30) == 46);
  // and the peak is reported
  CHECK(s.peakCno() == 46);
  // rendered
  CHECK(has(render(s), "peak 46  med>30deg 46"));
}

TEST_CASE("elevation bands", "[skyview]") {
  // The antenna's signature. Falling off gently from zenith to horizon is
  // healthy; the question is whether the high band is strong, because an
  // antenna cannot be blamed for what the building is blocking.
  NmeaParser p;
  feed(p, "$GPGSV,2,1,06,01,85,113,46,02,45,220,42,03,20,050,34,04,10,300,27,1");
  feed(p, "$GPGSV,2,2,06,05,70,150,48,06,35,010,40,1");
  const SkyView &s = p.sky();
  uint8_t med = 0, cnt = 0;
  s.band(60, 90, &med, &cnt);
  // zenith band median
  CHECK((med == 48 && cnt == 2));
  s.band(30, 60, &med, &cnt);
  // mid band median
  CHECK((med == 42 && cnt == 2));
  s.band(0, 15, &med, &cnt);
  // horizon band
  CHECK((med == 27 && cnt == 1));
  // rendered
  CHECK((has(render(s), ">60: 48 (2)") && has(render(s), "0-15: 27 (1)")));
}

TEST_CASE("empty sky", "[skyview]") {
  // During acquisition this is the normal state and has to read as such,
  // not as an empty list with a header.
  SkyView s;
  // says so plainly
  CHECK(has(render(s), "nothing visible yet"));
}

TEST_CASE("robustness", "[skyview]") {
  {
    NmeaParser p;
    // No signal id at all (NMEA before 4.10), and a truncated final block.
    feed(p, "$GPGSV,1,1,02,01,70,113,45,02,55,220,41");
    // still counts satellites
    CHECK(p.sky().tracked() == 2);
    // band unknown rather than guessed
    CHECK(p.sky().bandCount(1) == 0);
  }
  {
    char small[24];
    memset(small, 0x7F, sizeof(small));
    NmeaParser p;
    feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
    size_t n = p.sky().render(small, sizeof(small));
    // render stays inside a short buffer
    CHECK(n < sizeof(small));
    // and terminates it
    CHECK(memchr(small, 0, sizeof(small)) != nullptr);
  }
}
