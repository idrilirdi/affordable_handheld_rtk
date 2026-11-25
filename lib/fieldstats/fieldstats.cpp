#include "fieldstats.h"

#include <math.h>
#include <stdio.h>

void FieldStats::sample(const GnssFix &f, uint32_t epochs, uint32_t nowMs) {
  if (!primed_) {
    // First call: adopt the counters rather than charging everything since boot
    // to a single bucket.
    primed_ = true;
    lastEpochs_ = epochs;
    lastAt_ = nowMs;
    return;
  }
  if (!haveRef_ && f.havePos) {
    refLat_ = f.lat;
    refLon_ = f.lon;
    haveRef_ = true;
  }

  Sample &s = ring_[head_];
  s.havePos = f.havePos && haveRef_;
  if (s.havePos) {
    s.north = (float)((f.lat - refLat_) * 111320.0);
    s.east = (float)((f.lon - refLon_) * 111320.0 * cos(refLat_ * M_PI / 180.0));
  } else {
    s.north = s.east = 0;
  }
  uint32_t dt = nowMs - lastAt_;
  s.dtMs = dt > 65535 ? 65535 : (uint16_t)dt;
  lastAt_ = nowMs;
  s.type = (uint8_t)f.type;
  s.hAcc = f.haveAcc ? sqrtf(f.latStd * f.latStd + f.lonStd * f.lonStd) : -1.0f;
  s.age = f.diffAge;
  uint32_t d = epochs - lastEpochs_;
  s.epochs = d > 255 ? 255 : (uint8_t)d;
  lastEpochs_ = epochs;
  head_ = (head_ + 1) % STAT_WINDOW;
  if (n_ < STAT_WINDOW) n_++;
}

// Insertion sort over at most 120 floats, once per poll: the median is far more
// honest than a mean here, because a single re-convergence spike drags a mean
// somewhere the receiver never actually was.
static float medianOf(float *v, uint16_t n) {
  if (!n) return -1;
  for (uint16_t i = 1; i < n; ++i) {
    float k = v[i];
    int32_t j = (int32_t)i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
    v[j + 1] = k;
  }
  return v[n / 2];
}

float FieldStats::spanSeconds() const {
  uint32_t ms = 0;
  for (uint16_t i = 0; i < n_; ++i)
    ms += ring_[(head_ + STAT_WINDOW - n_ + i) % STAT_WINDOW].dtMs;
  return ms / 1000.0f;
}

size_t FieldStats::render(char *out, size_t max) const {
  if (!n_) return appendf(out, max, 0, "\nSTATS collecting...\n");

  uint16_t byType[5] = {0, 0, 0, 0, 0};
  uint16_t resets = 0, stale = 0, accN = 0, run = 0, longestFix = 0;
  // An engine can throw its ambiguity solution away without ever leaving
  // FLOAT: the type never changes, but the accuracy walks down to sub-meter
  // and then jumps back to several meters. Counting type changes alone reports
  // a perfectly stable session while exactly that is happening, so track the
  // accuracy independently - a climb to more than triple the best value seen
  // since the last such event is one re-convergence.
  uint16_t accResets = 0;
  float bestSince = -1;
  uint32_t epochSum = 0;
  float accs[STAT_WINDOW], ageSum = 0, ageMax = 0, accMin = 0, accMax = 0;
  uint16_t ageN = 0;
  uint8_t prev = 0xFF;

  for (uint16_t i = 0; i < n_; ++i) {
    const Sample &s = ring_[(head_ + STAT_WINDOW - n_ + i) % STAT_WINDOW];
    if (s.type < 5) byType[s.type]++;
    // A reset is the engine giving up a solution it had, which is the failure
    // we are chasing - not the slow climb back up afterward.
    if (prev != 0xFF && s.type < prev) resets++;
    prev = s.type;
    run = (s.type == (uint8_t)PosType::Fixed) ? run + 1 : 0;
    if (run > longestFix) longestFix = run;
    if (s.hAcc >= 0) {
      if (!accN || s.hAcc < accMin) accMin = s.hAcc;
      if (!accN || s.hAcc > accMax) accMax = s.hAcc;
      accs[accN++] = s.hAcc;
      if (bestSince < 0 || s.hAcc < bestSince) {
        bestSince = s.hAcc;
      } else if (s.hAcc > bestSince * 3 && bestSince > 0) {
        accResets++;
        bestSince = s.hAcc;   // start again from where it landed
      }
    }
    if (s.age >= 0) {
      ageSum += s.age;
      ageN++;
      if (s.age > ageMax) ageMax = s.age;
    }
    if (s.age < 0 || s.age > 5) stale++;
    epochSum += s.epochs;
  }

  // What the position actually did, as opposed to what the receiver claims
  // about it. A solution can report centimeters while wandering meters, and
  // only the scatter will say so.
  float sumE = 0, sumN = 0;
  uint16_t posN = 0;
  for (uint16_t i = 0; i < n_; ++i) {
    const Sample &s = ring_[(head_ + STAT_WINDOW - n_ + i) % STAT_WINDOW];
    if (!s.havePos) continue;
    sumE += s.east;
    sumN += s.north;
    posN++;
  }
  float rms = -1, worst = 0;
  if (posN > 1) {
    const float mE = sumE / posN, mN = sumN / posN;
    float acc2 = 0;
    for (uint16_t i = 0; i < n_; ++i) {
      const Sample &s = ring_[(head_ + STAT_WINDOW - n_ + i) % STAT_WINDOW];
      if (!s.havePos) continue;
      const float dE = s.east - mE, dN = s.north - mN;
      const float d2 = dE * dE + dN * dN;
      acc2 += d2;
      if (d2 > worst * worst) worst = sqrtf(d2);   // worst starts at 0, never negative
    }
    rms = sqrtf(acc2 / posN);
  }

  const float med = medianOf(accs, accN);
  const float span = spanSeconds();
  size_t n = appendf(out, max, 0, "\nSTATS last %.0f s (%u samples)\n", span, n_);
  n = appendf(out, max, n, "TYPE  fix %u%%  float %u%%  diff %u%%  single %u%%  none %u%%\n",
              byType[4] * 100 / n_, byType[3] * 100 / n_, byType[2] * 100 / n_,
              byType[1] * 100 / n_, byType[0] * 100 / n_);
  n = appendf(out, max, n, "DROPS %u type  %u accuracy   longest fix %u s\n", resets,
              accResets, longestFix);
  if (accN)
    n = appendf(out, max, n, "HACC  med %.2f  min %.2f  max %.2f m\n", med, accMin, accMax);
  else
    n = appendf(out, max, n, "HACC  no accuracy estimate in window\n");
  if (posN > 1)
    n = appendf(out, max, n, "SCAT  rms %.2f  worst %.2f m from window mean (%u fixes)\n",
                rms, worst, posN);
  else
    n = appendf(out, max, n, "SCAT  no position in window\n");
  n = appendf(out, max, n, "CAGE  mean %.1f  max %.1f s  stale %u\n",
              ageN ? ageSum / ageN : -1.0f, ageMax, stale);
  // Against the wall clock, so a starved loop cannot flatter itself: the rate
  // is epochs per real second, not epochs per sample.
  n = appendf(out, max, n, "RATE  %.2f Hz epochs   sampling %.2f Hz\n",
              span > 0 ? epochSum / span : 0.0f, span > 0 ? n_ / span : 0.0f);
  return n;
}
