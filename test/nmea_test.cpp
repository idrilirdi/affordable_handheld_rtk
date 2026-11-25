// Host-native tests for the NMEA parser. No hardware, Catch2 only:
//   bazel test //test:nmea_test
#include "nmea.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// Wrap a sentence body in '$' ... '*CS\r\n'.
static std::string s(const char *body) {
  uint8_t cs = 0;
  for (const char *p = body; *p; ++p) cs ^= (uint8_t)*p;
  char b[256];
  snprintf(b, sizeof(b), "$%s*%02X\r\n", body, cs);
  return b;
}

// Feed in awkward slices, to prove reassembly across chunk boundaries.
static uint8_t feed(NmeaParser &p, const std::string &data, size_t step = 5) {
  uint8_t upd = 0;
  for (size_t i = 0; i < data.size(); i += step)
    upd |= p.feed((const uint8_t *)data.data() + i,
                  std::min(step, data.size() - i));
  return upd;
}

TEST_CASE("NMEA parser", "[nmea]") {
  // An LC29H epoch: RTK fixed position, PQTMEPE accuracy, ZDA time.
  {
    NmeaParser p;
    std::string in =
        s("GNGGA,120000.00,4123.24400,N,00210.11599,E,4,24,0.60,25.000,M,49.000,M,1.0,0000") +
        s("GNZDA,120000.00,06,09,2026,00,00") +
        s("PQTMEPE,2,0.012,0.014,0.021,0.018,0.028");
    uint8_t upd = feed(p, in);
    const GnssFix &f = p.fix();
    // GGA parsed
    CHECK(upd & NMEA_UPD_POS);
    // PQTMEPE parsed
    CHECK(upd & NMEA_UPD_ACC);
    // ZDA parsed
    CHECK(upd & NMEA_UPD_TIME);
    // latitude
    CHECK(std::fabs(f.lat - 41.3874) < 1e-6);
    // longitude
    CHECK(std::fabs(f.lon - 2.1686) < 1e-6);
    // altitude
    CHECK(std::fabs(f.altMsl - 25.0) < 1e-3);
    // geoid separation
    CHECK(std::fabs(f.geoidSep - 49.0) < 1e-3);
    // GGA quality 4 -> FIXED
    CHECK(f.type == PosType::Fixed);
    // satellite count
    CHECK(f.sats == 24);
    // latStd from EPE north
    CHECK(std::fabs(f.latStd - 0.012f) < 1e-4);
    // lonStd from EPE east
    CHECK(std::fabs(f.lonStd - 0.014f) < 1e-4);
    // altStd from EPE down
    CHECK(std::fabs(f.altStd - 0.021f) < 1e-4);
    // ZDA date
    CHECK((f.year == 2026 && f.mon == 9 && f.day == 6));

    // What the NTRIP caster is sent: the sentence verbatim, checksum intact,
    // no line ending (the client adds its own).
    const std::string gga = p.lastGga();
    // GGA kept verbatim for the caster
    CHECK(gga.rfind("$GNGGA,120000.00,4123.24400,N", 0) == 0);
    // kept GGA ends with its checksum
    CHECK(gga.find('*') == gga.size() - 3);
    // kept GGA has no CRLF
    CHECK(gga.find('\r') == std::string::npos);
  }

  // Acquiring: GGA with quality/sats but empty position fields, exactly as the
  // LC29H emits indoors.
  {
    NmeaParser p;
    uint8_t upd = feed(p, s("GNGGA,000229.012,,,,,0,00,99.99,,M,,M,,"));
    // empty GGA reports state, not silence
    CHECK(upd & NMEA_UPD_STATE);
    // empty GGA does not claim a position
    CHECK(!(upd & NMEA_UPD_POS));
    // havePos stays false while acquiring
    CHECK(!p.fix().havePos);
    // no GGA for the caster without a position
    CHECK(p.lastGga()[0] == '\0');

    NmeaParser z;
    feed(z, s("GNGGA,120000.00,4123.24400,N,00210.11599,E,1,08,1.20,25.000,M,49.000,M,,"));
    // single fix goes to the caster
    CHECK(z.lastGga()[0] == '$');
    feed(z, s("GNGGA,120001.00,4123.24400,N,00210.11599,E,0,00,99.99,25.000,M,49.000,M,,"));
    // quality 0 withdraws it, coordinates or not
    CHECK(z.lastGga()[0] == '\0');
    feed(z, s("GNGGA,120002.00,4123.24400,N,00210.11599,E,1,08,1.20,25.000,M,49.000,M,,"));
    feed(z, s("GNGGA,120003.00,,,,,0,00,99.99,,M,,M,,"));
    // losing the fix withdraws it
    CHECK(z.lastGga()[0] == '\0');
    // quality 0 -> NONE
    CHECK(p.fix().type == PosType::None);

    NmeaParser q;
    uint8_t u2 = feed(q, s("GNGGA,120000.00,,,,,1,07,2.10,,M,,M,,"));
    // satellite count reported while still acquiring
    CHECK(((u2 & NMEA_UPD_STATE) && q.fix().sats == 7));
  }

  // The GPS-epoch placeholder date must not be believed.
  {
    NmeaParser p;
    feed(p, s("GNRMC,000206.012,V,,,,,,,060180,,,N,V"));
    // RMC with status V is rejected (no year 2080)
    CHECK(!p.fix().haveTime);
    NmeaParser q;
    feed(q, s("GNRMC,235959.00,A,4123.24400,N,00210.11599,E,0.0,0.0,060926,,,R"));
    // RMC with status A accepted
    CHECK((q.fix().haveTime && q.fix().year == 2026));
  }

  // Fix-quality mapping.
  {
    struct { int q; PosType want; const char *name; } cases[] = {
        {0, PosType::None, "quality 0 -> NONE"},
        {1, PosType::Single, "quality 1 -> SINGLE"},
        {2, PosType::Dgps, "quality 2 (DGPS) -> DGPS"},
        {5, PosType::Float, "quality 5 -> FLOAT"},
        {4, PosType::Fixed, "quality 4 -> FIXED"},
    };
    for (auto &c : cases) {
      NmeaParser p;
      char body[160];
      snprintf(body, sizeof(body),
               "GNGGA,120000.00,4123.24400,N,00210.11599,E,%d,12,0.6,25.0,M,49.0,M,,",
               c.q);
      feed(p, s(body));
      INFO(c.name);
      CHECK(p.fix().type == c.want);
    }
  }

  // Southern / western hemispheres must come out negative.
  {
    NmeaParser p;
    feed(p, s("GPGGA,120000.00,3345.60000,S,07012.30000,W,4,12,0.7,50.0,M,20.0,M,1.0,0000"));
    // southern latitude negative
    CHECK((p.fix().lat < -33.7 && p.fix().lat > -33.8));
    // western longitude negative
    CHECK((p.fix().lon < -70.2 && p.fix().lon > -70.3));
  }

  // RMC supplies time when ZDA is absent, with a windowed 2-digit year.
  {
    NmeaParser p;
    feed(p, s("GNRMC,235959.00,A,4123.24400,N,00210.11599,E,0.0,0.0,060926,,,R"));
    const GnssFix &f = p.fix();
    // RMC fallback year
    CHECK((f.haveTime && f.year == 2026));
    // RMC time
    CHECK((f.hour == 23 && f.min == 59 && f.sec == 59));
  }

  // Robustness.
  {
    NmeaParser p;
    const char *bad = "$GNGGA,120000.00,4123.24400,N,00210.11599,E,4,12,0.6,25.0,M,49.0,M,,*00\r\n";
    // bad checksum rejected
    CHECK((p.feed((const uint8_t *)bad, strlen(bad)) == 0 && !p.fix().havePos));

    NmeaParser q;
    const char *junk = "$GNGGA,1200\xff\xfe truncated no newline $GNGGA,";
    q.feed((const uint8_t *)junk, strlen(junk));
    feed(q, s("GNGGA,120000.00,4123.24400,N,00210.11599,E,4,12,0.6,25.0,M,49.0,M,,"));
    // recovers after mid-stream garbage
    CHECK(q.fix().havePos);

    NmeaParser r;
    std::string over = "$";
    over.append(400, 'A');
    over += "\r\n";
    r.feed((const uint8_t *)over.data(), over.size());
    feed(r, s("GNGGA,120000.00,4123.24400,N,00210.11599,E,4,12,0.6,25.0,M,49.0,M,,"));
    // recovers after an over-long sentence
    CHECK(r.fix().havePos);
  }

  // Synthesized GST, for raw-NMEA clients that read accuracy from it.
  {
    NmeaParser p;
    feed(p, s("GNZDA,120000.00,06,09,2026,00,00") +
            s("PQTMEPE,2,0.012,0.014,0.021,0.018,0.028"));
    char out[128];
    size_t n = nmeaBuildGst(out, sizeof(out), p.fix());
    // GST synthesized from PQTMEPE
    CHECK(n > 0);
    // GST fields match the EPE figures
    CHECK(strstr(out, "$GNGST,120000.00,,,,,0.012,0.014,0.021*") != nullptr);
    // and it must be a valid sentence in its own right
    NmeaParser q;
    // synthesized GST parses back cleanly
    CHECK((q.feed((const uint8_t *)out, n) & NMEA_UPD_ACC) != 0);
    NmeaParser r;
    // no GST when accuracy is unknown
    CHECK(nmeaBuildGst(out, sizeof(out), r.fix()) == 0);
  }
}

TEST_CASE("GST preferred over PQTMEPE", "[nmea]") {
  NmeaParser p;
  feed(p, "$PQTMEPE,2,7.736,7.736,14.738,10.940,18.354*6A\r\n");
  // EPE used while no GST has been seen
  CHECK(p.fix().latStd > 7.0f);
  // no real GST yet
  CHECK(!p.haveRealGst());
  feed(p, "$GNGST,120000.00,,,,,0.012,0.013,0.021*48\r\n");
  // real GST noticed
  CHECK(p.haveRealGst());
  // GST replaced the estimate
  CHECK(p.fix().latStd < 0.02f);
  feed(p, "$PQTMEPE,2,9.000,9.000,14.000,10.000,18.000*69\r\n");
  // later EPE ignored once GST exists
  CHECK(p.fix().latStd < 0.02f);
}
