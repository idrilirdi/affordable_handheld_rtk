#pragma once

#include <stddef.h>
#include <stdint.h>

// What the antenna can actually see.
//
// Everything else the firmware reports is the receiver's conclusion. This is
// its input: how many satellites each constellation is giving us, how strong
// they are, and on which bands. Without it a stubborn float solution cannot be
// told apart from a poor sky, which is the question that matters when a
// receiver refuses to fix in a poor spot.
//
// Built from GSV, which arrives as a set of sentences per constellation and
// signal. A set is replaced wholesale when its first sentence appears, so a
// satellite that drops out disappears rather than lingering.

enum class Cons : uint8_t { Gps, Glonass, Galileo, Beidou, Qzss, Navic, Sbas, Other, Count };

const char *consName(Cons c);

// L1-class, L2-class, L5-class, or 0 when the signal is not one of those.
// The NMEA signal id is only meaningful together with its constellation.
uint8_t bandOf(Cons c, uint8_t sigId);

class SkyView {
 public:
  // First sentence of a set: everything previously known for this
  // constellation and signal is discarded.
  void beginSet(Cons c, uint8_t sigId);
  void add(Cons c, uint8_t sigId, uint8_t prn, uint8_t elevDeg, uint8_t cno);

  size_t render(char *out, size_t max) const;

  uint8_t tracked() const;             // satellites with a usable carrier
  uint8_t listed() const { return n_; }
  uint8_t strong(uint8_t dbHz) const;  // how many at or above this C/N0
  uint8_t medianCno() const;
  // Median among satellites at or above this elevation. Low satellites are
  // weak everywhere and drag the overall median down on any antenna; what an
  // antenna is judged on is what it does with the ones overhead.
  uint8_t medianCnoAbove(uint8_t elevDeg) const;
  uint8_t peakCno() const;

  // Median C/N0 and measurement count within an elevation band, inclusive of
  // lo and exclusive of hi. This is how an antenna is actually characterized:
  // a single median over the whole sky conflates the antenna's performance
  // with how much sky it can see.
  void band(uint8_t loDeg, uint8_t hiDeg, uint8_t *medCno, uint8_t *count) const;
  uint8_t bandCount(uint8_t band) const;

 private:
  struct Sat {
    uint8_t cons;
    uint8_t sig;
    uint8_t prn;
    uint8_t elev;  // degrees above the horizon
    uint8_t cno;   // dB-Hz, 0 when the satellite is listed but not tracked
  };
  static const uint8_t kMax = 96;
  Sat sats_[kMax];
  uint8_t n_ = 0;
};
