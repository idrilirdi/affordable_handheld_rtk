#pragma once

// Recorded points: the on-flash record, and the exchange formats it converts
// to on download.
//
// The file on flash is itself the CSV download, one point per line, so a unit
// that is read out with nothing but a file browser still yields something any
// GIS can open. The other formats are produced from it line by line while the
// download runs - nothing is ever held whole in memory.
//
// Pure, so the formats can be tested on a host.

#include <stddef.h>

#include "nmea.h"

// The first line of the file.
extern const char kPointsCsvHeader[];

// What a point was averaged from. A single epoch is one sample and no spread.
struct PointStats {
  unsigned samples = 1;
  float spreadM = -1;        // horizontal rms about the mean; negative = unknown
  char rawFile[48] = "";     // raw observations logged for it, if any
};

// One record, newline-terminated, for the point `name` at `fix`. Returns its
// length, or 0 when the fix holds no position worth recording.
size_t pointsFormatRecord(char *out, size_t max, const char *name, const GnssFix &fix,
                          const PointStats &stats = PointStats());

// Averages the epochs of an occupation into one position.
//
// Only the best solution type seen counts: forty fixed epochs and ten float
// ones average to the forty, because folding in the float ones can only pull
// the point toward a worse answer. The record then says FIXED, honestly. But
// the best type has to make up at least a quarter of the epochs: one fixed
// epoch among sixty float ones is more likely a wrong fix than a right one.
class PointAverager {
 public:
  void reset();
  void add(const GnssFix &fix);  // fixes without a position are ignored
  // The averaged fix - time, satellites and the rest from the first epoch
  // used - and what it came from. False if nothing usable was added.
  bool result(GnssFix *out, PointStats *stats) const;
  unsigned samples() const;      // of the type result() would use

 private:
  // Offsets from the first epoch of each type, in meters: squaring degrees
  // directly loses the millimeters to rounding.
  struct Acc {
    unsigned n = 0;
    GnssFix first;
    double north = 0, east = 0, north2 = 0, east2 = 0;
    double alt = 0, sep = 0;
    double latStd = 0, lonStd = 0, altStd = 0;
    unsigned nStd = 0;
  };
  const Acc *best() const;
  Acc acc_[5];
};

enum class PointsFormat : unsigned char {
  Csv,      // the file as stored
  GeoJson,  // RFC 7946: QGIS, geojson.io, most web maps
  Kml,      // Google Earth, and the KML import of mapping apps
  Gpx,      // handheld GPS units, OsmAnd, Locus
};

// Download name and MIME type.
const char *pointsFileName(PointsFormat f);
const char *pointsMimeType(PointsFormat f);

// The download is begin, then one call per stored line, then end. Each writes
// into `out` and returns the length. pointsExportLine skips the header and
// anything malformed by returning 0; `first` must be true until a call has
// returned non-zero, because GeoJSON separates features with commas.
size_t pointsExportBegin(PointsFormat f, char *out, size_t max);
size_t pointsExportLine(PointsFormat f, const char *line, bool first, char *out, size_t max);
size_t pointsExportEnd(PointsFormat f, char *out, size_t max);
