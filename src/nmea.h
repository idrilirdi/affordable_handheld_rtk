#pragma once

// Minimal NMEA 0183 reader.
//
// The phone gets the receiver's sentences untouched; this parse is for the
// device's own use - the LEDs, point recording, the synthesized GST and the
// diagnostics.
//
// Decoded: GGA (position, fix quality, satellites), ZDA/RMC (UTC date and
// time), and accuracy from whichever source the receiver provides - $PQTMEPE on
// the Quectel LC29H, or GST on receivers that support it. Everything else is
// skipped.

#include <stddef.h>
#include <stdint.h>

#include "skyview.h"

// Bit flags returned by NmeaParser::feed(), so the caller only acts on what
// actually changed.
enum : uint8_t {
  NMEA_UPD_POS = 1 << 0,   // GGA: latitude, longitude, altitude, type, sats
  NMEA_UPD_ACC = 1 << 1,   // GST: per-axis standard deviations
  NMEA_UPD_TIME = 1 << 2,  // ZDA / RMC: UTC date and time
  // GGA carrying fix quality and satellite count but no position yet. Worth
  // reporting: "connected, acquiring, 7 satellites" beats silence.
  NMEA_UPD_STATE = 1 << 3,
  // GSV: the sky view changed. Used for diagnostics, where it is the only
  // evidence about the antenna's input.
  NMEA_UPD_SKY = 1 << 4,
};

// GGA fix quality.
enum class PosType : uint8_t { None, Single, Dgps, Float, Fixed };

const char *posTypeName(PosType t);

struct GnssFix {
  double lat = 0;      // degrees, + north
  double lon = 0;      // degrees, + east
  double altMsl = 0;   // meters above mean sea level (GGA field 9)
  // GGA field 11: geoid height above the WGS84 ellipsoid, from the receiver's
  // own geoid model. altMsl + geoidSep is the ellipsoidal height, which is
  // what the receiver actually solved for; altMsl is only as good as that model.
  double geoidSep = 0;
  PosType type = PosType::None;
  uint8_t sats = 0;
  float latStd = 0;    // meters, 1 sigma
  float lonStd = 0;
  float altStd = 0;
  uint16_t year = 0;   // UTC
  uint8_t mon = 0, day = 0, hour = 0, min = 0, sec = 0;

  // GGA field 13, seconds since the corrections used in this solution were
  // generated; negative when the field is empty. Empty means the engine is not
  // applying corrections at all, which looks identical to a good SINGLE fix
  // from the outside. A value that climbs means they are arriving too slowly
  // to resolve ambiguities, which looks identical to a stubborn FLOAT.
  float diffAge = -1;
  uint16_t baseId = 0;  // GGA field 14

  bool havePos = false;
  bool haveAcc = false;
  bool haveTime = false;
};

class NmeaParser {
 public:
  // Feed raw receiver bytes. Returns the OR of the NMEA_UPD_* flags for
  // whatever completed inside this call.
  uint8_t feed(const uint8_t *data, size_t len);

  const GnssFix &fix() const { return fix_; }

  // True once the receiver has emitted a GST of its own, in which case there
  // is nothing to synthesize and doing so would put two of them in the stream.
  bool haveRealGst() const { return haveGst_; }

  // What the antenna sees, as opposed to what the receiver concluded from it.
  const SkyView &sky() const { return sky_; }

  // The current GGA if it carries a fix (quality 1 or better), verbatim and
  // without CRLF, for an NTRIP caster. Empty whenever there is no fix.
  const char *lastGga() const { return gga_; }

 private:
  uint8_t line(char *s, size_t n);  // one '$'-terminated sentence, checksum ok

  static const size_t kMaxLine = 128;  // longest GSV is ~82 incl. CRLF
  char buf_[kMaxLine];
  size_t len_ = 0;
  bool overflow_ = false;
  // ZDA is preferred over RMC (unambiguous 4-digit year), but that preference
  // needs its own flag: keying it off fix_.haveTime would make the first RMC
  // block every later one and freeze the clock on a receiver with no ZDA.
  bool haveZda_ = false;
  // GST is the real per-axis standard deviation; $PQTMEPE is the receiver's
  // own error estimate and is far less trustworthy - it read 7.7 m while the
  // measured stationary scatter was 0.09 m. Firmware before A04S has no GST at
  // all, so EPE stays as the fallback and this flag decides which wins.
  bool haveGst_ = false;
  SkyView sky_;
  GnssFix fix_;
  char gga_[kMaxLine] = "";
};

// Build a $GNGST sentence from the parsed accuracy figures, for raw-NMEA
// clients that read accuracy from GST. Receivers that emit GST themselves do
// not need this; the LC29H has no GST at all, so its $PQTMEPE figures would
// otherwise be invisible to those apps. Writes a NUL-terminated sentence
// including "\r\n" and returns its length, or 0 if there is nothing to report.
size_t nmeaBuildGst(char *out, size_t max, const GnssFix &fix);
