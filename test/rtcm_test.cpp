// Host-native RTCM3 tests. rtcm.cpp has no Arduino dependencies on purpose.
//   bazel test //test:rtcm_test
//
// The MSM masks are read at fixed bit offsets into the payload, so an
// off-by-one in the header arithmetic would silently report the wrong
// satellites and the wrong signals - and the whole point of that decode is to
// answer "does this stream carry the band the rover needs", which is not a
// question you want answered wrongly.
//
// rtcm.cpp keeps global counters and per-type state, so every test case
// starts with rtcmReset() and runs in any order.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rtcm.h"

// Big-endian bit writer, matching how RTCM defines every field.
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

// One MSM frame with the given satellite and signal masks set.
static std::vector<uint8_t> msmFrame(uint16_t type, const std::vector<int> &sats,
                                     const std::vector<int> &sigs) {
  Bits p;
  p.put(type, 12);
  p.put(1234, 12);   // reference station
  p.put(0, 30);      // epoch
  p.put(0, 1);       // multiple message
  p.put(0, 3);       // IODS
  p.put(0, 7);       // reserved
  p.put(0, 2);       // clock steering
  p.put(0, 2);       // external clock
  p.put(0, 1);       // smoothing
  p.put(0, 3);       // smoothing interval
  uint64_t satMask = 0;
  for (int s : sats) satMask |= 1ull << (64 - s);   // ID 1 is the MSB
  p.put(satMask, 64);
  uint32_t sigMask = 0;
  for (int s : sigs) sigMask |= 1u << (32 - s);
  p.put(sigMask, 32);
  p.put(0, 64);      // some payload past the header

  std::vector<uint8_t> f;
  f.push_back(0xD3);
  f.push_back((uint8_t)((p.b.size() >> 8) & 0x03));
  f.push_back((uint8_t)(p.b.size() & 0xFF));
  f.insert(f.end(), p.b.begin(), p.b.end());
  uint32_t c = crc24q(f.data(), f.size());
  f.push_back((uint8_t)(c >> 16));
  f.push_back((uint8_t)(c >> 8));
  f.push_back((uint8_t)c);
  return f;
}

static char buf[512];

TEST_CASE("RTCM frame check", "[rtcm]") {
  rtcmReset();
  auto f = msmFrame(1077, {1, 5, 9, 30}, {2, 17, 22});
  rtcmFeed(f.data(), f.size());
  // valid frame accepted
  CHECK((rtcmFramesOk() == 1 && rtcmFramesBad() == 0));

  rtcmSummary(buf, sizeof(buf));
  // message type decoded
  CHECK(strstr(buf, "1077") != nullptr);
  // satellite count from the 64-bit mask
  CHECK(strstr(buf, "sats 4") != nullptr);
  // signal IDs from the 32-bit mask
  CHECK(strstr(buf, "sig 2 17 22") != nullptr);
}

TEST_CASE("L5 detection", "[rtcm]") {
  rtcmReset();
  auto l12 = msmFrame(1087, {2, 3}, {2, 10});     // L1 C/A + L2 W: no L5
  rtcmFeed(l12.data(), l12.size());
  rtcmSummary(buf, sizeof(buf));
  // an L1/L2 base shows no ID 22-24
  CHECK(strstr(buf, "1087 x1 sats 2 sig 2 10") != nullptr);
}

TEST_CASE("corruption", "[rtcm]") {
  rtcmReset();
  auto bad = msmFrame(1097, {1}, {2});
  bad[8] ^= 0xFF;                                  // flip a payload byte
  uint32_t okBefore = rtcmFramesOk();
  rtcmFeed(bad.data(), bad.size());
  // CRC failure rejected
  CHECK((rtcmFramesOk() == okBefore && rtcmFramesBad() == 1));
}

TEST_CASE("framing", "[rtcm]") {
  rtcmReset();
  auto a = msmFrame(1005, {}, {});
  uint32_t ok2 = rtcmFramesOk();
  for (size_t i = 0; i < a.size(); ++i) rtcmFeed(&a[i], 1);   // one byte at a time
  // frame reassembled across chunks
  CHECK(rtcmFramesOk() == ok2 + 1);

  uint8_t junk[] = {0x00, 0xFF, 0x12, 0xD3};       // noise, then a false start
  rtcmFeed(junk, sizeof(junk));
  auto b = msmFrame(1074, {7}, {2, 23});
  rtcmFeed(b.data(), b.size());
  rtcmSummary(buf, sizeof(buf));
  // resyncs after junk
  CHECK(strstr(buf, "1074 x1 sats 1 sig 2 23") != nullptr);
}
