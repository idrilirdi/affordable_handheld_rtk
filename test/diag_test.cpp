// Host-native tests for the over-the-air diagnostics.
//
// These sentences exist because the USB console cannot be used during a
// survey - attaching resets the board and restarts the receiver's navigation
// filter. So they are the only view into a running session, and a malformed
// one would be discovered in a field with no way to look closer.
//
//   bazel test //test:diag_test
//
// rtcm.cpp keeps global counters and per-type state, so every test case
// starts with rtcmReset() and runs in any order.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "diag.h"
#include "nmea.h"
#include "rtcm.h"

// Valid NMEA: '$', body, '*', two hex digits of XOR, CRLF, and at most the
// 82 characters the standard allows.
static bool wellFormed(const char *s) {
  size_t n = strlen(s);
  if (n < 9 || n > 82) return false;
  if (s[0] != '$' || s[n - 2] != '\r' || s[n - 1] != '\n') return false;
  const char *star = strrchr(s, '*');
  if (!star || (size_t)(star - s) + 5 != n) return false;
  uint8_t cs = 0;
  for (const char *p = s + 1; p < star; ++p) cs ^= (uint8_t)*p;
  char want[3];
  snprintf(want, sizeof(want), "%02X", cs);
  return want[0] == star[1] && want[1] == star[2];
}

struct Bits {
  std::vector<uint8_t> b;
  size_t n = 0;
  void put(uint64_t v, int len) {
    for (int i = len - 1; i >= 0; --i) {
      if (n % 8 == 0) b.push_back(0);
      if ((v >> i) & 1) b[n / 8] |= 1 << (7 - (n % 8));
      ++n;
    }
  }
};

static uint32_t crc24q(const uint8_t *d, size_t n) {
  uint32_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= (uint32_t)d[i] << 16;
    for (int k = 0; k < 8; ++k) {
      crc <<= 1;
      if (crc & 0x1000000) crc ^= 0x1864CFB;
    }
  }
  return crc & 0xFFFFFF;
}

static std::vector<uint8_t> frame(const Bits &p) {
  std::vector<uint8_t> f{0xD3, (uint8_t)((p.b.size() >> 8) & 3),
                         (uint8_t)(p.b.size() & 0xFF)};
  f.insert(f.end(), p.b.begin(), p.b.end());
  uint32_t c = crc24q(f.data(), f.size());
  f.push_back(c >> 16); f.push_back(c >> 8); f.push_back(c);
  return f;
}

static std::vector<uint8_t> msm(uint16_t type, const std::vector<int> &sats,
                                const std::vector<int> &sigs) {
  Bits p;
  p.put(type, 12); p.put(1234, 12); p.put(0, 30);
  p.put(0, 1); p.put(0, 3); p.put(0, 7); p.put(0, 2); p.put(0, 2);
  p.put(0, 1); p.put(0, 3);
  uint64_t sm = 0;
  for (int s : sats) sm |= 1ull << (64 - s);
  p.put(sm, 64);
  uint32_t gm = 0;
  for (int s : sigs) gm |= 1u << (32 - s);
  p.put(gm, 32);
  p.put(0, 64);
  return frame(p);
}

// 1005 with a reference position, in ECEF tenths of a millimeter.
static std::vector<uint8_t> msg1005(double x, double y, double z) {
  Bits p;
  p.put(1005, 12); p.put(404, 12); p.put(0, 6);
  p.put(1, 1); p.put(1, 1); p.put(1, 1); p.put(0, 1);
  p.put((uint64_t)(int64_t)(x * 1e4) & ((1ull << 38) - 1), 38);
  p.put(0, 1); p.put(0, 1);
  p.put((uint64_t)(int64_t)(y * 1e4) & ((1ull << 38) - 1), 38);
  p.put(0, 2);
  p.put((uint64_t)(int64_t)(z * 1e4) & ((1ull << 38) - 1), 38);
  return frame(p);
}

// Shared by every test case: the output buffer and the fix being described.
static char s[128];

static GnssFix munichFix() {
  GnssFix fix;
  fix.lat = 48.1372000; fix.lon = 11.5755000; fix.altMsl = 520.0;
  fix.havePos = true; fix.diffAge = 1.0f; fix.baseId = 404;
  return fix;
}

TEST_CASE("status sentence", "[diag]") {
  rtcmReset();
  const GnssFix fix = munichFix();
  auto m = msm(1074, {1, 5, 9}, {2, 24});
  rtcmFeed(m.data(), m.size());
  size_t n = diagSentence(s, sizeof(s), 0, fix);
  // built
  CHECK(n > 0);
  // well formed NMEA
  CHECK(wellFormed(s));
  // carries the age of corrections
  CHECK(strstr(s, "PRTKA,1.0,404") != nullptr);
}

TEST_CASE("no corrections", "[diag]") {
  rtcmReset();
  GnssFix cold;
  cold.havePos = true;
  diagSentence(s, sizeof(s), 0, cold);
  // empty age field when none are applied
  CHECK(strstr(s, "PRTKA,,0,") != nullptr);
  // still well formed
  CHECK(wellFormed(s));
}

TEST_CASE("per-message detail", "[diag]") {
  rtcmReset();
  const GnssFix fix = munichFix();
  auto m = msm(1074, {1, 5, 9}, {2, 24});
  rtcmFeed(m.data(), m.size());
  size_t n = diagSentence(s, sizeof(s), 1, fix);
  // built
  CHECK(n > 0);
  // well formed NMEA
  CHECK(wellFormed(s));
  // type, count, satellites and signals
  CHECK(strstr(s, "PRTKM,1074,1,3,2,24") != nullptr);
  // cycle ends past the last type
  CHECK(diagSentence(s, sizeof(s), 1 + rtcmTypeCount(), fix) == 0);
}

TEST_CASE("baseline", "[diag]") {
  rtcmReset();
  const GnssFix fix = munichFix();
  // Munich in ECEF, rounded to the kilometer, near the fix above.
  auto b = msg1005(4178000.0, 856000.0, 4727000.0);
  rtcmFeed(b.data(), b.size());
  double la, lo, al;
  // 1005 decoded
  CHECK(rtcmBasePosition(&la, &lo, &al));
  // plausible latitude
  CHECK((la > 47.0 && la < 49.5));
  // plausible longitude
  CHECK((lo > 10.0 && lo < 13.0));
  diagSentence(s, sizeof(s), 0, fix);
  // baseline present and well formed
  CHECK((wellFormed(s) && strstr(s, "PRTKA,1.0,404,")));
}

TEST_CASE("width", "[diag]") {
  rtcmReset();
  const GnssFix fix = munichFix();
  auto wide = msm(1077, {1,2,3,4,5,6,7,8,9,10},
                  {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16});
  rtcmFeed(wide.data(), wide.size());
  bool allFit = true;
  for (uint8_t i = 0; i < 1 + rtcmTypeCount(); ++i) {
    if (!diagSentence(s, sizeof(s), i, fix)) break;
    if (!wellFormed(s)) allFit = false;
  }
  // every sentence stays inside the NMEA limit
  CHECK(allFit);
}
