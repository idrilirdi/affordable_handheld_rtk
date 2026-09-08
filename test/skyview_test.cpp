// Host-native tests for the sky view.
//
// This is the only evidence the firmware has about its own input. If it
// miscounts, a perfectly good antenna looks broken, or - worse - a starved one
// looks fine and we go on blaming the receiver.

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/nmea.h"
#include "../src/skyview.h"

static int failures = 0;
static void check(const char *what, bool ok) {
  printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

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

int main() {
  printf("counting\n");
  {
    NmeaParser p;
    // Four GPS satellites, one of them listed but not tracked (empty C/N0).
    feed(p, "$GPGSV,1,1,04,01,70,113,45,02,55,220,41,03,30,050,38,04,10,300,,1");
    const SkyView &s = p.sky();
    check("all four are visible", s.listed() == 4);
    check("only three are tracked", s.tracked() == 3);
    check("median of 38/41/45", s.medianCno() == 41);
    check("two at or above 40", s.strong(40) == 2);
    check("rendered as tracked over visible", has(render(s), "GPS 3/4"));
  }

  printf("signal ids are per constellation\n");
  {
    // Signal 1 is L1 C/A for GPS but E5a for Galileo. Treating the number as
    // universal would report L5 satellites as L1 and hide exactly the band we
    // care about for RTK.
    NmeaParser p;
    feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
    feed(p, "$GAGSV,1,1,01,01,70,113,44,1");
    const SkyView &s = p.sky();
    check("GPS signal 1 is L1", s.bandCount(1) == 1);
    check("Galileo signal 1 is L5", s.bandCount(5) == 1);
    check("and they are two different satellites", s.tracked() == 2);
  }

  printf("multi sentence sets\n");
  {
    NmeaParser p;
    feed(p, "$GPGSV,2,1,05,01,70,113,45,02,55,220,41,03,30,050,38,04,10,300,35,1");
    feed(p, "$GPGSV,2,2,05,05,20,150,33,1");
    check("both sentences accumulate", p.sky().tracked() == 5);
  }

  printf("a set replaces its predecessor\n");
  {
    // A satellite that sets must disappear, not linger and flatter the count.
    NmeaParser p;
    feed(p, "$GPGSV,1,1,03,01,70,113,45,02,55,220,41,03,30,050,38,1");
    check("three to begin with", p.sky().tracked() == 3);
    feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
    check("one after the new set", p.sky().tracked() == 1);
  }

  printf("constellations are kept apart\n");
  {
    // A new GPS set must not discard the Galileo satellites, and the same PRN
    // in two constellations is two satellites.
    NmeaParser p;
    feed(p, "$GAGSV,1,1,01,01,70,113,44,7");
    feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
    feed(p, "$GPGSV,1,1,01,02,60,100,46,1");
    const SkyView &s = p.sky();
    check("Galileo survived the GPS set", s.tracked() == 2);
    std::string r = render(s);
    check("counted separately", has(r, "GPS 1/1") && has(r, "GAL 1/1"));
  }

  printf("bands\n");
  {
    // The question that matters in a poor spot: are we getting a second and
    // third frequency, or only L1?
    NmeaParser p;
    feed(p, "$GPGSV,1,1,02,01,70,113,45,02,55,220,44,1");   // L1 C/A
    feed(p, "$GPGSV,1,1,02,01,70,113,40,02,55,220,39,5");   // L2 C-M
    feed(p, "$GPGSV,1,1,02,01,70,113,38,02,55,220,37,7");   // L5 I
    const SkyView &s = p.sky();
    std::string r = render(s);
    check("three bands counted", has(r, "BAND  L1 2  L2 2  L5 2"));
    check("but only two spacecraft", has(r, "GPS 2/2"));
  }

  printf("elevation\n");
  {
    // The discriminator between a weak antenna and an obstructed sky. Two
    // satellites overhead at 46 dB-Hz and four near the horizon at 26 give an
    // overall median of 26 - which says nothing about the antenna - while the
    // high-elevation median says it is working perfectly well.
    NmeaParser p;
    feed(p, "$GPGSV,2,1,06,01,80,113,46,02,75,220,46,03,10,050,26,04,08,300,26,1");
    feed(p, "$GPGSV,2,2,06,05,07,150,26,06,05,010,26,1");
    const SkyView &s = p.sky();
    check("overall median is dragged down", s.medianCno() == 26);
    check("but overhead is strong", s.medianCnoAbove(30) == 46);
    check("and the peak is reported", s.peakCno() == 46);
    check("rendered", has(render(s), "peak 46  med>30deg 46"));
  }

  printf("elevation bands\n");
  {
    // The antenna's signature. Falling off gently from zenith to horizon is
    // healthy; the question is whether the high band is strong, because an
    // antenna cannot be blamed for what the building is blocking.
    NmeaParser p;
    feed(p, "$GPGSV,2,1,06,01,85,113,46,02,45,220,42,03,20,050,34,04,10,300,27,1");
    feed(p, "$GPGSV,2,2,06,05,70,150,48,06,35,010,40,1");
    const SkyView &s = p.sky();
    uint8_t med = 0, cnt = 0;
    s.band(60, 90, &med, &cnt);
    check("zenith band median", med == 48 && cnt == 2);
    s.band(30, 60, &med, &cnt);
    check("mid band median", med == 42 && cnt == 2);
    s.band(0, 15, &med, &cnt);
    check("horizon band", med == 27 && cnt == 1);
    check("rendered", has(render(s), ">60: 48 (2)") && has(render(s), "0-15: 27 (1)"));
  }

  printf("empty sky\n");
  {
    // During acquisition this is the normal state and has to read as such,
    // not as an empty list with a header.
    SkyView s;
    check("says so plainly", has(render(s), "nothing visible yet"));
  }

  printf("robustness\n");
  {
    NmeaParser p;
    // No signal id at all (NMEA before 4.10), and a truncated final block.
    feed(p, "$GPGSV,1,1,02,01,70,113,45,02,55,220,41");
    check("still counts satellites", p.sky().tracked() == 2);
    check("band unknown rather than guessed", p.sky().bandCount(1) == 0);
  }
  {
    char small[24];
    memset(small, 0x7F, sizeof(small));
    NmeaParser p;
    feed(p, "$GPGSV,1,1,01,01,70,113,45,1");
    size_t n = p.sky().render(small, sizeof(small));
    check("render stays inside a short buffer", n < sizeof(small));
    check("and terminates it", memchr(small, 0, sizeof(small)) != nullptr);
  }

  printf(failures ? "\n%d FAILED\n" : "\nall passed (%d failures)\n", failures);
  return failures ? 1 : 0;
}
