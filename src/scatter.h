#pragma once

#include <stdint.h>

// How much the reported position actually moves, over the last few seconds.
//
// The receiver's own accuracy figure is an estimate, and while a float solution
// settles it is often far more pessimistic than the position deserves. Spread
// is measured, not estimated: a float claiming 10 m whose position holds within
// a few decimeters is steadier than its label says.
//
// Free of Arduino, so it is tested on the host.
#define SCATTER_WINDOW 30

class Scatter {
 public:
  void add(double lat, double lon);
  void clear() { n_ = head_ = 0; }
  // Horizontal rms distance from the window mean, meters. Negative until there
  // are enough samples to mean anything.
  float rms(uint16_t minSamples) const;

 private:
  float e_[SCATTER_WINDOW], n2_[SCATTER_WINDOW];
  uint16_t n_ = 0, head_ = 0;
  double refLat_ = 0, refLon_ = 0;
};
