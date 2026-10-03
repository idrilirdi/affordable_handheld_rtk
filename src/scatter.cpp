#include "scatter.h"

#include <math.h>

void Scatter::add(double lat, double lon) {
  // Meters in a local plane anchored at the first sample of the window, so a
  // float keeps centimeters instead of losing them to a degree's magnitude.
  if (n_ == 0) {
    refLat_ = lat;
    refLon_ = lon;
  }
  const double kMetersPerDeg = 111320.0;
  e_[head_] = (float)((lon - refLon_) * kMetersPerDeg * cos(refLat_ * M_PI / 180.0));
  n2_[head_] = (float)((lat - refLat_) * kMetersPerDeg);
  head_ = (head_ + 1) % SCATTER_WINDOW;
  if (n_ < SCATTER_WINDOW) n_++;
}

float Scatter::rms(uint16_t minSamples) const {
  if (n_ < minSamples || n_ == 0) return -1.0f;
  double me = 0, mn = 0;
  for (uint16_t i = 0; i < n_; ++i) { me += e_[i]; mn += n2_[i]; }
  me /= n_;
  mn /= n_;
  double ss = 0;
  for (uint16_t i = 0; i < n_; ++i) {
    const double de = e_[i] - me, dn = n2_[i] - mn;
    ss += de * de + dn * dn;
  }
  return (float)sqrt(ss / n_);
}
