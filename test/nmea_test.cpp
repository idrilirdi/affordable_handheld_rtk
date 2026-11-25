// Host-native tests for the NMEA parser. No hardware, no framework:
//   bazel test //test:all
#include "nmea.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
static void check(bool ok, const char *what) {
  printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) fails++;
}

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

int main() {
  printf("NMEA parser\n");

  // An LC29H epoch: RTK fixed position, PQTMEPE accuracy, ZDA time.
  {
    NmeaParser p;
    std::string in =
        s("GNGGA,120000.00,4123.24400,N,00210.11599,E,4,24,0.60,25.000,M,49.000,M,1.0,0000") +
        s("GNZDA,120000.00,06,09,2026,00,00") +
        s("PQTMEPE,2,0.012,0.014,0.021,0.018,0.028");
    uint8_t upd = feed(p, in);
    const GnssFix &f = p.fix();
    check(upd & NMEA_UPD_POS, "GGA parsed");
    check(upd & NMEA_UPD_ACC, "PQTMEPE parsed");
    check(upd & NMEA_UPD_TIME, "ZDA parsed");
    check(std::fabs(f.lat - 41.3874) < 1e-6, "latitude");
    check(std::fabs(f.lon - 2.1686) < 1e-6, "longitude");
    check(std::fabs(f.altMsl - 25.0) < 1e-3, "altitude");
    check(std::fabs(f.geoidSep - 49.0) < 1e-3, "geoid separation");
    check(f.type == PosType::Fixed, "GGA quality 4 -> FIXED");
    check(f.sats == 24, "satellite count");
    check(std::fabs(f.latStd - 0.012f) < 1e-4, "latStd from EPE north");
    check(std::fabs(f.lonStd - 0.014f) < 1e-4, "lonStd from EPE east");
    check(std::fabs(f.altStd - 0.021f) < 1e-4, "altStd from EPE down");
    check(f.year == 2026 && f.mon == 9 && f.day == 6, "ZDA date");

    // What the NTRIP caster is sent: the sentence verbatim, checksum intact,
    // no line ending (the client adds its own).
    const std::string gga = p.lastGga();
    check(gga.rfind("$GNGGA,120000.00,4123.24400,N", 0) == 0, "GGA kept verbatim for the caster");
    check(gga.find('*') == gga.size() - 3, "kept GGA ends with its checksum");
    check(gga.find('\r') == std::string::npos, "kept GGA has no CRLF");
  }

  // Acquiring: GGA with quality/sats but empty position fields, exactly as the
  // LC29H emits indoors.
  {
    NmeaParser p;
    uint8_t upd = feed(p, s("GNGGA,000229.012,,,,,0,00,99.99,,M,,M,,"));
    check(upd & NMEA_UPD_STATE, "empty GGA reports state, not silence");
    check(!(upd & NMEA_UPD_POS), "empty GGA does not claim a position");
    check(!p.fix().havePos, "havePos stays false while acquiring");
    check(p.lastGga()[0] == '\0', "no GGA for the caster without a position");

    NmeaParser z;
    feed(z, s("GNGGA,120000.00,4123.24400,N,00210.11599,E,1,08,1.20,25.000,M,49.000,M,,"));
    check(z.lastGga()[0] == '$', "single fix goes to the caster");
    feed(z, s("GNGGA,120001.00,4123.24400,N,00210.11599,E,0,00,99.99,25.000,M,49.000,M,,"));
    check(z.lastGga()[0] == '\0', "quality 0 withdraws it, coordinates or not");
    feed(z, s("GNGGA,120002.00,4123.24400,N,00210.11599,E,1,08,1.20,25.000,M,49.000,M,,"));
    feed(z, s("GNGGA,120003.00,,,,,0,00,99.99,,M,,M,,"));
    check(z.lastGga()[0] == '\0', "losing the fix withdraws it");
    check(p.fix().type == PosType::None, "quality 0 -> NONE");

    NmeaParser q;
    uint8_t u2 = feed(q, s("GNGGA,120000.00,,,,,1,07,2.10,,M,,M,,"));
    check((u2 & NMEA_UPD_STATE) && q.fix().sats == 7,
          "satellite count reported while still acquiring");
  }

  // The GPS-epoch placeholder date must not be believed.
  {
    NmeaParser p;
    feed(p, s("GNRMC,000206.012,V,,,,,,,060180,,,N,V"));
    check(!p.fix().haveTime, "RMC with status V is rejected (no year 2080)");
    NmeaParser q;
    feed(q, s("GNRMC,235959.00,A,4123.24400,N,00210.11599,E,0.0,0.0,060926,,,R"));
    check(q.fix().haveTime && q.fix().year == 2026, "RMC with status A accepted");
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
      check(p.fix().type == c.want, c.name);
    }
  }

  // Southern / western hemispheres must come out negative.
  {
    NmeaParser p;
    feed(p, s("GPGGA,120000.00,3345.60000,S,07012.30000,W,4,12,0.7,50.0,M,20.0,M,1.0,0000"));
    check(p.fix().lat < -33.7 && p.fix().lat > -33.8, "southern latitude negative");
    check(p.fix().lon < -70.2 && p.fix().lon > -70.3, "western longitude negative");
  }

  // RMC supplies time when ZDA is absent, with a windowed 2-digit year.
  {
    NmeaParser p;
    feed(p, s("GNRMC,235959.00,A,4123.24400,N,00210.11599,E,0.0,0.0,060926,,,R"));
    const GnssFix &f = p.fix();
    check(f.haveTime && f.year == 2026, "RMC fallback year");
    check(f.hour == 23 && f.min == 59 && f.sec == 59, "RMC time");
  }

  // Robustness.
  {
    NmeaParser p;
    const char *bad = "$GNGGA,120000.00,4123.24400,N,00210.11599,E,4,12,0.6,25.0,M,49.0,M,,*00\r\n";
    check(p.feed((const uint8_t *)bad, strlen(bad)) == 0 && !p.fix().havePos,
          "bad checksum rejected");

    NmeaParser q;
    const char *junk = "$GNGGA,1200\xff\xfe truncated no newline $GNGGA,";
    q.feed((const uint8_t *)junk, strlen(junk));
    feed(q, s("GNGGA,120000.00,4123.24400,N,00210.11599,E,4,12,0.6,25.0,M,49.0,M,,"));
    check(q.fix().havePos, "recovers after mid-stream garbage");

    NmeaParser r;
    std::string over = "$";
    over.append(400, 'A');
    over += "\r\n";
    r.feed((const uint8_t *)over.data(), over.size());
    feed(r, s("GNGGA,120000.00,4123.24400,N,00210.11599,E,4,12,0.6,25.0,M,49.0,M,,"));
    check(r.fix().havePos, "recovers after an over-long sentence");
  }

  // Synthesized GST, for raw-NMEA clients that read accuracy from it.
  {
    NmeaParser p;
    feed(p, s("GNZDA,120000.00,06,09,2026,00,00") +
            s("PQTMEPE,2,0.012,0.014,0.021,0.018,0.028"));
    char out[128];
    size_t n = nmeaBuildGst(out, sizeof(out), p.fix());
    check(n > 0, "GST synthesized from PQTMEPE");
    check(strstr(out, "$GNGST,120000.00,,,,,0.012,0.014,0.021*") != nullptr,
          "GST fields match the EPE figures");
    // and it must be a valid sentence in its own right
    NmeaParser q;
    check((q.feed((const uint8_t *)out, n) & NMEA_UPD_ACC) != 0,
          "synthesized GST parses back cleanly");
    NmeaParser r;
    check(nmeaBuildGst(out, sizeof(out), r.fix()) == 0,
          "no GST when accuracy is unknown");
  }

  {
    printf("GST preferred over PQTMEPE\n");
    NmeaParser p;
    feed(p, "$PQTMEPE,2,7.736,7.736,14.738,10.940,18.354*6A\r\n");
    check(p.fix().latStd > 7.0f, "EPE used while no GST has been seen");
    check(!p.haveRealGst(), "no real GST yet");
    feed(p, "$GNGST,120000.00,,,,,0.012,0.013,0.021*48\r\n");
    check(p.haveRealGst(), "real GST noticed");
    check(p.fix().latStd < 0.02f, "GST replaced the estimate");
    feed(p, "$PQTMEPE,2,9.000,9.000,14.000,10.000,18.000*69\r\n");
    check(p.fix().latStd < 0.02f, "later EPE ignored once GST exists");
  }

  printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "all passed", fails,
         fails == 1 ? "" : "s");
  return fails != 0;
}
