#include "nmea.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// For the console and the logs.
const char *posTypeName(PosType t) {
  switch (t) {
    case PosType::Fixed: return "FIXED";
    case PosType::Float: return "FLOAT";
    case PosType::Dgps: return "DGPS";
    case PosType::Single: return "SINGLE";
    default: return "NONE";
  }
}

// "$GNGGA,...*4A" - XOR of everything between '$' and '*'. A receiver that is
// still booting, or a burst that arrives half-overwritten, shows up here rather
// than as a wild coordinate.
static bool checksumOk(const char *s, size_t n) {
  const char *star = (const char *)memchr(s, '*', n);
  if (!star || (size_t)(star - s) < 2 || (s + n) - star < 3) return false;
  uint8_t want = (uint8_t)strtoul(star + 1, nullptr, 16);
  uint8_t have = 0;
  for (const char *p = s + 1; p < star; ++p) have ^= (uint8_t)*p;
  return have == want;
}

// Split on ',' in place. Empty fields are preserved - NMEA uses them to mean
// "no value", and their position carries the meaning.
static size_t split(char *s, char **out, size_t maxFields) {
  size_t n = 0;
  out[n++] = s;
  for (char *p = s; *p && n < maxFields; ++p) {
    if (*p == ',') {
      *p = '\0';
      out[n++] = p + 1;
    } else if (*p == '*') {
      *p = '\0';
      break;
    }
  }
  return n;
}

// "4808.23201" + 'N' -> 48.137200. Degrees are the leading 2 (lat) or 3 (lon)
// digits, the rest is decimal minutes.
static bool parseLatLon(const char *val, const char *hemi, int degDigits,
                        double *out) {
  if (!val || !*val || !hemi || !*hemi) return false;
  size_t n = strlen(val);
  if (n < (size_t)degDigits + 2) return false;

  char degBuf[4] = {0};
  memcpy(degBuf, val, degDigits);
  double deg = atof(degBuf);
  double minutes = atof(val + degDigits);
  double v = deg + minutes / 60.0;

  if (*hemi == 'S' || *hemi == 'W') v = -v;
  *out = v;
  return true;
}

uint8_t NmeaParser::feed(const uint8_t *data, size_t len) {
  uint8_t updated = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = (char)data[i];

    if (c == '$') {  // resync: a new sentence always wins over a partial one
      len_ = 0;
      overflow_ = false;
    }
    if (c == '\r' || c == '\n') {
      if (len_ > 0 && !overflow_) {
        buf_[len_] = '\0';
        updated |= line(buf_, len_);
      }
      len_ = 0;
      overflow_ = false;
      continue;
    }
    if (len_ + 1 >= kMaxLine) {
      overflow_ = true;  // drop the whole sentence, not just its tail
      continue;
    }
    if (len_ > 0 || c == '$') buf_[len_++] = c;
  }
  return updated;
}

uint8_t NmeaParser::line(char *s, size_t n) {
  if (n < 9 || s[0] != '$' || !checksumOk(s, n)) return 0;

  char *f[24];

  // Quectel proprietary. The LC29H has no GST at all - the protocol spec says
  // "GST is not supported on LC29H (BA, CA, DA, EA)" - so $PQTMEPE is the only
  // accuracy source on that receiver. North/east/down map onto latitude,
  // longitude and height error.
  //   $PQTMEPE,<MsgVer>,<North>,<East>,<Down>,<2D>,<3D>*CS
  if (n > 9 && !memcmp(s + 1, "PQTMEPE,", 8)) {
    if (haveGst_) return 0;  // GST is better, and this would fight with it
    size_t nf = split(s, f, 24);
    if (nf < 5) return 0;
    fix_.latStd = (float)atof(f[2]);
    fix_.lonStd = (float)atof(f[3]);
    fix_.altStd = (float)atof(f[4]);
    fix_.haveAcc = true;
    return NMEA_UPD_ACC;
  }

  // Talker id varies with the constellation mix ($GP, $GN, $GL...), so match
  // on the three-letter sentence type only.
  const char *type = s + 3;

  // $xxGSV,<total>,<num>,<visible>,{<prn>,<elev>,<azim>,<cno>}x1..4[,<sigId>]
  //
  // The one sentence that describes the sky rather than the solution. Talker
  // id carries the constellation here and must not be ignored: $GAGSV and
  // $GPGSV reuse the same signal id numbers for different bands.
  if (!memcmp(type, "GSV", 3)) {
    Cons cons;
    if (!memcmp(s + 1, "GP", 2)) cons = Cons::Gps;
    else if (!memcmp(s + 1, "GL", 2)) cons = Cons::Glonass;
    else if (!memcmp(s + 1, "GA", 2)) cons = Cons::Galileo;
    else if (!memcmp(s + 1, "GB", 2) || !memcmp(s + 1, "BD", 2)) cons = Cons::Beidou;
    else if (!memcmp(s + 1, "GQ", 2)) cons = Cons::Qzss;
    else if (!memcmp(s + 1, "GI", 2)) cons = Cons::Navic;
    else cons = Cons::Other;

    size_t nf = split(s, f, 24);
    if (nf < 4) return 0;

    // Present only from NMEA 4.10. When absent every band looks alike, which
    // is worth knowing rather than guessing: signal 0 classifies as unknown.
    const size_t blocks = (nf - 4) / 4;
    uint8_t sigId = 0;
    if (nf >= 5 + blocks * 4) sigId = (uint8_t)atoi(f[4 + blocks * 4]);

    if (atoi(f[2]) == 1) sky_.beginSet(cons, sigId);
    for (size_t b = 0; b < blocks; ++b) {
      const char **g = (const char **)&f[4 + b * 4];
      sky_.add(cons, sigId, (uint8_t)atoi(g[0]), (uint8_t)atoi(g[1]), (uint8_t)atoi(g[3]));
    }
    return NMEA_UPD_SKY;
  }

  if (!memcmp(type, "GGA", 3)) {
    // Copied before split() cuts the sentence up at its commas.
    char raw[kMaxLine];
    size_t rn = n < sizeof(raw) - 1 ? n : sizeof(raw) - 1;
    while (rn && (s[rn - 1] == '\r' || s[rn - 1] == '\n')) --rn;
    memcpy(raw, s, rn);
    raw[rn] = '\0';
    size_t nf = split(s, f, 24);
    if (nf < 10) return 0;

    int quality = atoi(f[6]);
    PosType t;
    switch (quality) {
      case 0: t = PosType::None; break;
      case 4: t = PosType::Fixed; break;
      case 5: t = PosType::Float; break;
      case 2: t = PosType::Dgps; break;  // code-differential
      default: t = PosType::Single; break;
    }

    fix_.type = t;
    fix_.sats = (uint8_t)atoi(f[7]);

    // While acquiring, GGA still reports quality and satellite count with the
    // position fields empty. Report that state rather than going silent - a
    // client showing "no fix, 7 satellites" is far better informed than one
    // showing nothing at all.
    double lat, lon;
    if (!parseLatLon(f[2], f[3], 2, &lat) || !parseLatLon(f[4], f[5], 3, &lon)) {
      fix_.havePos = false;
      fix_.diffAge = -1;
      gga_[0] = '\0';
      return NMEA_UPD_STATE;
    }

    fix_.lat = lat;
    fix_.lon = lon;
    fix_.altMsl = atof(f[9]);
    fix_.geoidSep = (nf > 11 && f[11][0]) ? atof(f[11]) : 0;
    fix_.diffAge = (nf > 13 && f[13][0]) ? (float)atof(f[13]) : -1.0f;
    fix_.baseId = (nf > 14) ? (uint16_t)atoi(f[14]) : 0;
    fix_.havePos = true;
    // Only a real fix goes to the caster: with quality 0 the coordinates are
    // a leftover, and a network caster would build corrections for them.
    if (t != PosType::None) memcpy(gga_, raw, rn + 1);
    else gga_[0] = '\0';
    return NMEA_UPD_POS;
  }

  if (!memcmp(type, "GST", 3)) {
    size_t nf = split(s, f, 24);
    if (nf < 9) return 0;
    haveGst_ = true;
    fix_.latStd = (float)atof(f[6]);
    fix_.lonStd = (float)atof(f[7]);
    fix_.altStd = (float)atof(f[8]);
    fix_.haveAcc = true;
    return NMEA_UPD_ACC;
  }

  if (!memcmp(type, "ZDA", 3)) {
    size_t nf = split(s, f, 24);
    if (nf < 5 || strlen(f[1]) < 6) return 0;
    char hh[3] = {f[1][0], f[1][1], 0};
    char mm[3] = {f[1][2], f[1][3], 0};
    char ss[3] = {f[1][4], f[1][5], 0};
    fix_.hour = (uint8_t)atoi(hh);
    fix_.min = (uint8_t)atoi(mm);
    fix_.sec = (uint8_t)atoi(ss);
    fix_.day = (uint8_t)atoi(f[2]);
    fix_.mon = (uint8_t)atoi(f[3]);
    fix_.year = (uint16_t)atoi(f[4]);
    fix_.haveTime = fix_.year > 1980;
    haveZda_ = fix_.haveTime;
    return fix_.haveTime ? NMEA_UPD_TIME : 0;
  }

  // RMC is the fallback when a receiver has no ZDA: same time, and a 2-digit
  // year that has to be windowed.
  if (!memcmp(type, "RMC", 3)) {
    if (haveZda_) return 0;  // ZDA already supplies this, and unambiguously
    size_t nf = split(s, f, 24);
    if (nf < 10 || strlen(f[1]) < 6 || strlen(f[9]) < 6) return 0;
    // Status 'A' = valid. Without it the receiver reports the GPS epoch date
    // (060180), which would otherwise be published as a confident 2080.
    if (f[2][0] != 'A') return 0;
    char hh[3] = {f[1][0], f[1][1], 0};
    char mm[3] = {f[1][2], f[1][3], 0};
    char ss[3] = {f[1][4], f[1][5], 0};
    char dd[3] = {f[9][0], f[9][1], 0};
    char mo[3] = {f[9][2], f[9][3], 0};
    char yy[3] = {f[9][4], f[9][5], 0};
    fix_.hour = (uint8_t)atoi(hh);
    fix_.min = (uint8_t)atoi(mm);
    fix_.sec = (uint8_t)atoi(ss);
    fix_.day = (uint8_t)atoi(dd);
    fix_.mon = (uint8_t)atoi(mo);
    fix_.year = (uint16_t)(2000 + atoi(yy));
    fix_.haveTime = true;
    return NMEA_UPD_TIME;
  }

  return 0;
}

size_t nmeaBuildGst(char *out, size_t max, const GnssFix &fix) {
  if (!fix.haveAcc) return 0;

  // $GNGST,<UTC>,<RMS>,<major>,<minor>,<orient>,<latStd>,<lonStd>,<altStd>*CS
  // RMS and the error ellipse are left empty: the receiver reports per-axis
  // sigmas only, and inventing the ellipse would be worse than omitting it.
  char body[96];
  int n = snprintf(body, sizeof(body),
                   "GNGST,%02u%02u%02u.00,,,,,%.3f,%.3f,%.3f", fix.hour,
                   fix.min, fix.sec, (double)fix.latStd, (double)fix.lonStd,
                   (double)fix.altStd);
  if (n <= 0 || n >= (int)sizeof(body)) return 0;

  uint8_t cs = 0;
  for (const char *p = body; *p; ++p) cs ^= (uint8_t)*p;

  int total = snprintf(out, max, "$%s*%02X\r\n", body, cs);
  if (total <= 0 || total >= (int)max) return 0;
  return (size_t)total;
}
