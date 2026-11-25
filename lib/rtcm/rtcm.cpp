#include "rtcm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t framesOk = 0, framesBad = 0;

// A correction stream carries a handful of types, repeated. Sixteen is well
// clear of what a network or a base station sends, and anything past it is
// counted but not itemized rather than displacing what we already track.
#define MAX_TYPES 16

struct TypeStat {
  uint16_t type;
  uint32_t count;
  uint32_t sigMask;  // union of RTCM signal IDs seen; bit i means ID i+1
  uint8_t sats;      // satellites in the most recent frame of this type
};

static TypeStat stats[MAX_TYPES];
static uint8_t nTypes = 0;
static uint32_t untracked = 0;

// Big-endian bit field, as every RTCM data field is defined.
static uint32_t bitsAt(const uint8_t *p, size_t start, size_t len) {
  uint32_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    size_t b = start + i;
    v = (v << 1) | ((p[b >> 3] >> (7 - (b & 7))) & 1);
  }
  return v;
}

// MSM header layout, in bits from the start of the payload:
//
//   12 message number     12 reference station    30 epoch time
//    1 multiple message    3 IODS                  7 reserved
//    2 clock steering      2 external clock        1 smoothing
//    3 smoothing interval
//   64 satellite mask     32 signal mask
//
// which puts the satellite mask at bit 73 and the signal mask at bit 137.
#define MSM_SAT_MASK_BIT 73
#define MSM_SIG_MASK_BIT 137
#define MSM_HEADER_BITS (MSM_SIG_MASK_BIT + 32)

// 38-bit ECEF components need more than the 32 a plain field read returns.
static int64_t bitsAt64(const uint8_t *p, size_t start, size_t len) {
  int64_t v = 0;
  for (size_t i = 0; i < len; ++i) {
    size_t b = start + i;
    v = (v << 1) | ((p[b >> 3] >> (7 - (b & 7))) & 1);
  }
  // Sign-extend from the field width.
  if (v & (1ll << (len - 1))) v -= (1ll << len);
  return v;
}

static bool haveBase = false;
static double baseLat = 0, baseLon = 0, baseAlt = 0;

// 1005 layout, in bits from the start of the payload: 12 message number,
// 12 station ID, 6 ITRF year, 4 GPS/GLONASS/Galileo/reference indicators,
// then ECEF X at 34, Y at 74 (past a 1-bit oscillator flag and 1 reserved),
// and Z at 114 (past the 2-bit quarter-cycle indicator). Each is 38 bits
// signed in units of 0.1 mm.
static void recordBase(const uint8_t *payload, size_t payloadLen) {
  if (payloadLen * 8 < 152) return;
  double x = (double)bitsAt64(payload, 34, 38) * 1e-4;
  double y = (double)bitsAt64(payload, 74, 38) * 1e-4;
  double z = (double)bitsAt64(payload, 114, 38) * 1e-4;

  // ECEF to geodetic, WGS-84, by Bowring's closed form - accurate well past
  // what is needed to judge a baseline.
  const double a = 6378137.0, f = 1.0 / 298.257223563;
  const double b = a * (1 - f), e2 = f * (2 - f);
  const double ep2 = (a * a - b * b) / (b * b);
  double p = sqrt(x * x + y * y);
  if (p < 1.0) return;  // a base at the pole is a decoding error, not a base
  double th = atan2(a * z, b * p);
  double lat = atan2(z + ep2 * b * sin(th) * sin(th) * sin(th),
                     p - e2 * a * cos(th) * cos(th) * cos(th));
  double n = a / sqrt(1 - e2 * sin(lat) * sin(lat));
  baseLat = lat * 180.0 / M_PI;
  baseLon = atan2(y, x) * 180.0 / M_PI;
  baseAlt = p / cos(lat) - n;
  haveBase = true;
}

bool rtcmBasePosition(double *lat, double *lon, double *alt) {
  if (!haveBase) return false;
  *lat = baseLat; *lon = baseLon; *alt = baseAlt;
  return true;
}

static bool isMsm(uint16_t type) {
  return type >= 1071 && type <= 1127 && ((type - 1071) % 10) <= 6;
}

static uint8_t popcount64(const uint8_t *p, size_t startBit) {
  uint8_t n = 0;
  for (int i = 0; i < 64; ++i)
    if (bitsAt(p, startBit + i, 1)) ++n;
  return n;
}

static void record(const uint8_t *payload, size_t payloadLen, uint16_t type) {
  TypeStat *s = nullptr;
  for (uint8_t i = 0; i < nTypes; ++i)
    if (stats[i].type == type) { s = &stats[i]; break; }
  if (!s) {
    if (nTypes == MAX_TYPES) { untracked++; return; }
    s = &stats[nTypes++];
    s->type = type;
    s->count = 0;
    s->sigMask = 0;
    s->sats = 0;
  }
  s->count++;

  if (type == 1005 || type == 1006) recordBase(payload, payloadLen);

  // Only MSM carries the masks, and only if the frame is long enough to hold
  // the header - a truncated frame would otherwise read whatever follows it.
  if (!isMsm(type) || payloadLen * 8 < MSM_HEADER_BITS) return;
  s->sats = popcount64(payload, MSM_SAT_MASK_BIT);
  for (int i = 0; i < 32; ++i)
    if (bitsAt(payload, MSM_SIG_MASK_BIT + i, 1)) s->sigMask |= 1u << i;
}

// Slide past a false start. A 0xD3 in noise - or in the tail of a frame we
// just rejected - must not swallow the genuine frame behind it, so drop the
// leading byte and resume at the next candidate rather than clearing
// everything buffered.
static void resync(uint8_t *f, size_t *have) {
  size_t next = 1;
  while (next < *have && f[next] != 0xD3) ++next;
  if (next >= *have) { *have = 0; return; }
  memmove(f, f + next, *have - next);
  *have -= next;
}

static void consume(uint8_t *f, size_t *have) {
  for (;;) {
    if (*have == 0) return;
    if (f[0] != 0xD3) { resync(f, have); continue; }
    if (*have < 3) return;

    // RTCM3 header: 0xD3, six reserved bits that are always zero, then ten
    // bits of length. Any reserved bit set means this is not a header at all -
    // worth testing before the length, because trusting a bogus length means
    // waiting for bytes that will never come while real frames pass unseen.
    // Not counted as a bad frame: it is a resync, not a corrupted message.
    if (f[1] & 0xFC) { resync(f, have); continue; }

    size_t need = (((size_t)(f[1] & 0x03)) << 8 | f[2]) + 6;
    if (*have < need) return;

    uint32_t want = ((uint32_t)f[need - 3] << 16) |
                    ((uint32_t)f[need - 2] << 8) | f[need - 1];
    uint32_t crc = 0;
    for (size_t k = 0; k < need - 3; ++k) {
      crc ^= (uint32_t)f[k] << 16;
      for (int b = 0; b < 8; ++b) {
        crc <<= 1;
        if (crc & 0x1000000) crc ^= 0x1864CFB;
      }
    }
    if ((crc & 0xFFFFFF) == want) {
      framesOk++;
      record(f + 3, need - 6, (uint16_t)(((uint16_t)f[3] << 4) | (f[4] >> 4)));
      memmove(f, f + need, *have - need);
      *have -= need;
      continue;
    }
    framesBad++;
    resync(f, have);
  }
}

// Longest RTCM3 frame is 1023 payload bytes plus six of framing, so this
// never has to grow and a frame can always be held whole.
static uint8_t frameBuf[1029];
static size_t frameHave = 0;

void rtcmReset() {
  frameHave = 0;
  framesOk = framesBad = 0;
  nTypes = 0;
  untracked = 0;
  haveBase = false;
}

void rtcmFeed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    // unreachable: the frame length is bounded
    if (frameHave == sizeof(frameBuf)) resync(frameBuf, &frameHave);
    frameBuf[frameHave++] = data[i];
    consume(frameBuf, &frameHave);
  }
}

uint32_t rtcmFramesOk() { return framesOk; }
uint32_t rtcmFramesBad() { return framesBad; }

uint8_t rtcmTypeCount() { return nTypes; }

bool rtcmTypeInfo(uint8_t index, uint16_t *type, uint32_t *count, uint8_t *sats,
                  uint32_t *sigMask) {
  if (index >= nTypes) return false;
  const TypeStat &s = stats[index];
  *type = s.type;
  *count = s.count;
  *sats = s.sats;
  *sigMask = s.sigMask;
  return true;
}

size_t rtcmSummary(char *out, size_t n) {
  size_t w = 0;
  if (nTypes == 0) return (size_t)snprintf(out, n, "none");
  for (uint8_t i = 0; i < nTypes && w < n; ++i) {
    const TypeStat &s = stats[i];
    w += snprintf(out + w, n - w, "%s%u x%lu", i ? "  " : "", s.type,
                  (unsigned long)s.count);
    if (w < n && s.sigMask) {
      w += snprintf(out + w, n - w, " sats %u sig", s.sats);
      for (int b = 0; b < 32 && w < n; ++b)
        if (s.sigMask & (1u << b)) w += snprintf(out + w, n - w, " %d", b + 1);
    }
  }
  if (untracked && w < n)
    w += snprintf(out + w, n - w, "  (+%lu untracked)", (unsigned long)untracked);
  return w;
}
