#include "diag.h"

#include <math.h>
#include <stdio.h>

#include "nmea.h"
#include "rtcm.h"

// Shared by both sentence kinds: NMEA checksum is the XOR of everything
// between '$' and '*'.
static size_t finish(char *out, size_t max, const char *body) {
  uint8_t cs = 0;
  for (const char *p = body; *p; ++p) cs ^= (uint8_t)*p;
  int total = snprintf(out, max, "$%s*%02X\r\n", body, cs);
  if (total <= 0 || total >= (int)max) return 0;
  return (size_t)total;
}

size_t diagSentence(char *out, size_t max, uint8_t index, const GnssFix &fix) {
  char body[88];

  if (index == 0) {
    char age[16];
    if (fix.diffAge >= 0) snprintf(age, sizeof(age), "%.1f", (double)fix.diffAge);
    else age[0] = '\0';  // empty: no corrections being applied

    // Baseline to the reference station, which for a network VRS should be a
    // few meters - it is synthesized at the position the client sends upstream.
    char baseline[16];
    double bLat, bLon, bAlt;
    if (rtcmBasePosition(&bLat, &bLon, &bAlt) && fix.havePos) {
      const double kMetersPerDeg = 111320.0;
      double dy = (bLat - fix.lat) * kMetersPerDeg;
      double dx = (bLon - fix.lon) * kMetersPerDeg * cos(fix.lat * M_PI / 180.0);
      snprintf(baseline, sizeof(baseline), "%.0f", sqrt(dx * dx + dy * dy));
    } else {
      baseline[0] = '\0';
    }

    snprintf(body, sizeof(body), "PRTKA,%s,%u,%s,%lu,%lu", age, fix.baseId,
             baseline, (unsigned long)rtcmFramesOk(),
             (unsigned long)rtcmFramesBad());
    return finish(out, max, body);
  }

  uint16_t type;
  uint32_t count, sigMask;
  uint8_t sats;
  if (!rtcmTypeInfo(index - 1, &type, &count, &sats, &sigMask)) return 0;

  int w = snprintf(body, sizeof(body), "PRTKM,%u,%lu,%u", type,
                   (unsigned long)count, sats);
  for (int b = 0; b < 32 && w > 0 && w < (int)sizeof(body); ++b)
    if (sigMask & (1u << b))
      w += snprintf(body + w, sizeof(body) - w, ",%d", b + 1);
  return finish(out, max, body);
}
