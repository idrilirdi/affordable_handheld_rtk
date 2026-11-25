// Host-native tests for separating RTCM3 frames from the NMEA stream:
//   bazel test //test:uplink_split_test
#include "uplink_split.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<std::string> frames;
static void onFrame(const uint8_t *d, size_t n) { frames.emplace_back((const char *)d, n); }

static std::string frame(size_t payload, uint8_t fill) {
  std::string f;
  f += (char)0xD3;
  f += (char)((payload >> 8) & 0x03);
  f += (char)(payload & 0xFF);
  for (size_t i = 0; i < payload; ++i) f += (char)(fill + i);
  uint32_t crc = 0;
  for (unsigned char c : f) {
    crc ^= (uint32_t)c << 16;
    for (int b = 0; b < 8; ++b) {
      crc <<= 1;
      if (crc & 0x1000000) crc ^= 0x1864CFB;
    }
  }
  f += (char)((crc >> 16) & 0xFF);
  f += (char)((crc >> 8) & 0xFF);
  f += (char)(crc & 0xFF);
  return f;
}

// Feed in slices of `step`, as the UART delivers them.
static std::string run(UplinkSplit &s, const std::string &in, size_t step) {
  std::string text;
  std::vector<uint8_t> out(step);
  for (size_t i = 0; i < in.size(); i += step) {
    const size_t n = std::min(step, in.size() - i);
    const size_t t = s.feed((const uint8_t *)in.data() + i, n, out.data(), onFrame);
    text.append((const char *)out.data(), t);
  }
  return text;
}

TEST_CASE("Uplink split", "[uplink_split]") {
  const std::string gga = "$GNGGA,120000.00,4807.0,N,01134.5,E,4,24,0.6,519.2,M,47.5,M,1.0,0401*00\r\n";
  const std::string zda = "$GNZDA,120000.00,06,10,2026,00,00*00\r\n";
  // A payload that contains '$', '\n' and 0xD3: binary must not be read as text.
  const std::string big = frame(600, 0x24), small = frame(20, 0xD3);

  for (size_t step : {1u, 7u, 64u, 512u}) {
    frames.clear();
    UplinkSplit s;
    const std::string text = run(s, gga + big + zda + small + gga, step);
    {
      INFO("slices of " << step << ": text intact");
      CHECK(text == gga + zda + gga);
    }
    {
      INFO("slices of " << step << ": frames intact");
      CHECK((frames.size() == 2 && frames[0] == big && frames[1] == small));
    }
  }

  frames.clear();
  UplinkSplit s;
  std::string bad = frame(30, 1);
  bad[10] ^= 0x40;
  const std::string text = run(s, gga + bad + zda, 16);
  // bad CRC dropped
  CHECK((frames.empty() && s.framesBad() == 1));
  // text after a bad frame survives
  CHECK(text == gga + zda);

  frames.clear();
  UplinkSplit r;
  std::string stray = "\xD3\xFF";
  const std::string t2 = run(r, stray + gga, 5);
  // stray 0xD3 costs one byte
  CHECK((frames.empty() && t2 == gga.substr(1)));
}
