#include "skyview.h"

#include <stdio.h>

#include "textbuf.h"
#include <string.h>

const char *consName(Cons c) {
  switch (c) {
    case Cons::Gps: return "GPS";
    case Cons::Glonass: return "GLO";
    case Cons::Galileo: return "GAL";
    case Cons::Beidou: return "BDS";
    case Cons::Qzss: return "QZS";
    case Cons::Navic: return "IRN";
    case Cons::Sbas: return "SBS";
    default: return "OTH";
  }
}

// NMEA 0183 v4.11 signal ids. They are per constellation and reused across
// them, so the same number means different things for GPS and Galileo. Only
// the bands that matter for RTK are classified; anything else returns 0 rather
// than being guessed at.
uint8_t bandOf(Cons c, uint8_t sigId) {
  switch (c) {
    case Cons::Gps:
    case Cons::Qzss:
      if (sigId == 1 || sigId == 2 || sigId == 3) return 1;   // L1 C/A, P, M
      if (sigId == 4 || sigId == 5 || sigId == 6) return 2;   // L2 P, C-M, C-L
      if (sigId == 7 || sigId == 8) return 5;                 // L5 I, Q
      return 0;
    case Cons::Glonass:
      if (sigId == 1 || sigId == 2) return 1;                 // G1
      if (sigId == 3 || sigId == 4) return 2;                 // G2
      return 0;
    case Cons::Galileo:
      if (sigId == 6 || sigId == 7) return 1;                 // E1 A, B/C
      if (sigId == 2) return 2;                               // E5b, the L2 analog
      if (sigId == 1 || sigId == 3) return 5;                 // E5a, E5a+b
      return 0;
    case Cons::Beidou:
      if (sigId == 1 || sigId == 2 || sigId == 3 || sigId == 4) return 1;  // B1
      if (sigId == 8 || sigId == 9 || sigId == 10) return 2;               // B3
      if (sigId == 5 || sigId == 6 || sigId == 7) return 5;                // B2a/b
      return 0;
    default:
      return 0;
  }
}

void SkyView::beginSet(Cons c, uint8_t sigId) {
  uint8_t w = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (!(sats_[i].cons == (uint8_t)c && sats_[i].sig == sigId)) sats_[w++] = sats_[i];
  n_ = w;
}

void SkyView::add(Cons c, uint8_t sigId, uint8_t prn, uint8_t elevDeg, uint8_t cno) {
  if (!prn) return;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cons == (uint8_t)c && sats_[i].sig == sigId && sats_[i].prn == prn) {
      sats_[i].elev = elevDeg;
      sats_[i].cno = cno;
      return;
    }
  if (n_ >= kMax) return;
  sats_[n_++] = {(uint8_t)c, sigId, prn, elevDeg, cno};
}

static uint8_t medianOfBytes(uint8_t *v, uint8_t n) {
  if (!n) return 0;
  for (uint8_t i = 1; i < n; ++i) {
    uint8_t k = v[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; --j; }
    v[j + 1] = k;
  }
  return v[n / 2];
}

uint8_t SkyView::medianCnoAbove(uint8_t elevDeg) const {
  uint8_t v[kMax], n = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno && sats_[i].elev >= elevDeg) v[n++] = sats_[i].cno;
  return medianOfBytes(v, n);
}

void SkyView::band(uint8_t loDeg, uint8_t hiDeg, uint8_t *medCno, uint8_t *count) const {
  uint8_t v[kMax], n = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno && sats_[i].elev >= loDeg && sats_[i].elev < hiDeg) v[n++] = sats_[i].cno;
  if (count) *count = n;
  if (medCno) *medCno = medianOfBytes(v, n);
}

uint8_t SkyView::peakCno() const {
  uint8_t best = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno > best) best = sats_[i].cno;
  return best;
}

uint8_t SkyView::tracked() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno) n++;
  return n;
}

uint8_t SkyView::strong(uint8_t dbHz) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno >= dbHz) n++;
  return n;
}

uint8_t SkyView::medianCno() const {
  uint8_t v[kMax], n = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno) v[n++] = sats_[i].cno;
  return medianOfBytes(v, n);
}

uint8_t SkyView::bandCount(uint8_t band) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < n_; ++i)
    if (sats_[i].cno && bandOf((Cons)sats_[i].cons, sats_[i].sig) == band) n++;
  return n;
}

size_t SkyView::render(char *out, size_t max) const {
  // Distinct satellites per constellation, counted once however many signals
  // each is carrying - the count people expect is "how many spacecraft", not
  // "how many measurements".
  uint8_t seen[(size_t)Cons::Count] = {0}, withSig[(size_t)Cons::Count] = {0};
  for (uint8_t c = 0; c < (uint8_t)Cons::Count; ++c) {
    uint8_t prns[kMax], np = 0, nt = 0;
    for (uint8_t i = 0; i < n_; ++i) {
      if (sats_[i].cons != c) continue;
      bool dup = false;
      for (uint8_t k = 0; k < np; ++k)
        if (prns[k] == sats_[i].prn) { dup = true; break; }
      if (dup) continue;
      prns[np++] = sats_[i].prn;
      if (sats_[i].cno) nt++;
    }
    seen[c] = np;
    withSig[c] = nt;
  }

  if (!n_) return (size_t)snprintf(out, max, "SKY   nothing visible yet\n");

  size_t n = appendf(out, max, 0, "SKY   ");
  for (uint8_t c = 0; c < (uint8_t)Cons::Count; ++c) {
    if (!seen[c]) continue;
    n = appendf(out, max, n, "%s %u/%u  ", consName((Cons)c), withSig[c], seen[c]);
  }
  n = appendf(out, max, n, "(tracked/visible)\n");
  n = appendf(out, max, n,
              "CNO   med %u  peak %u  med>30deg %u  >=40 %u of %u\n"
              "BAND  L1 %u  L2 %u  L5 %u\n",
              medianCno(), peakCno(), medianCnoAbove(30), strong(40), tracked(),
              bandCount(1), bandCount(2), bandCount(5));

  // The antenna's own signature. A healthy one falls off gently from zenith to
  // horizon; one that is deaf overhead is a hardware problem, while one that is
  // strong overhead and poor low down is simply looking at a blocked sky.
  static const uint8_t edges[] = {60, 90, 30, 60, 15, 30, 0, 15};
  n = appendf(out, max, n, "ELEV  ");
  for (uint8_t b = 0; b < 4; ++b) {
    uint8_t med = 0, cnt = 0;
    band(edges[b * 2], edges[b * 2 + 1], &med, &cnt);
    if (edges[b * 2 + 1] == 90) n = appendf(out, max, n, ">60: %u (%u)  ", med, cnt);
    else n = appendf(out, max, n, "%u-%u: %u (%u)  ", edges[b * 2], edges[b * 2 + 1], med, cnt);
  }
  n = appendf(out, max, n, "med (count)\n");
  return n;
}
