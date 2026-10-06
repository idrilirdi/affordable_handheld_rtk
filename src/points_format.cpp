#include "points_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textbuf.h"

// Column order is the file format: append new columns at the end, never
// reorder, or files already on a unit stop converting.
const char kPointsCsvHeader[] =
    "name,time_utc,latitude,longitude,height_ellipsoid_m,height_msl_m,solution,"
    "satellites,std_lat_m,std_lon_m,std_height_m,correction_age_s,base_id,samples,"
    "spread_m,raw_file\n";

enum Col {
  kName, kTime, kLat, kLon, kEllh, kMsl, kSolution, kSats,
  kStdLat, kStdLon, kStdHgt, kCorrAge, kBaseId, kSamples, kSpread, kRawFile, kCols
};

static const char *const kColName[kCols] = {
    "name", "time_utc", "latitude", "longitude", "height_ellipsoid_m", "height_msl_m",
    "solution", "satellites", "std_lat_m", "std_lon_m", "std_height_m",
    "correction_age_s", "base_id", "samples", "spread_m", "raw_file"};

// Plain words: these files are read by people and by GIS filters.
static const char *solutionName(PosType t) {
  switch (t) {
    case PosType::Fixed: return "FIXED";
    case PosType::Float: return "FLOAT";
    case PosType::Dgps: return "DGPS";
    case PosType::Single: return "SINGLE";
    default: return "NONE";
  }
}

size_t pointsFormatRecord(char *out, size_t max, const char *name, const GnssFix &fix,
                          const PointStats &stats) {
  if (!max) return 0;
  out[0] = '\0';
  if (!fix.havePos || fix.type == PosType::None) return 0;

  char when[32] = "";
  if (fix.haveTime)
    snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02uZ", fix.year, fix.mon,
             fix.day, fix.hour, fix.min, fix.sec);
  char sl[12] = "", so[12] = "", sh[12] = "";
  if (fix.haveAcc) {
    snprintf(sl, sizeof(sl), "%.3f", (double)fix.latStd);
    snprintf(so, sizeof(so), "%.3f", (double)fix.lonStd);
    snprintf(sh, sizeof(sh), "%.3f", (double)fix.altStd);
  }
  char age[12] = "";
  if (fix.diffAge >= 0) snprintf(age, sizeof(age), "%.1f", (double)fix.diffAge);
  char base[8] = "";
  if (fix.diffAge >= 0) snprintf(base, sizeof(base), "%u", fix.baseId);
  char spread[12] = "";
  if (stats.spreadM >= 0) snprintf(spread, sizeof(spread), "%.3f", (double)stats.spreadM);

  // Nine decimals is well under a millimeter: no rounding the receiver did
  // not do itself.
  size_t n = appendf(out, max, 0, "%s,%s,%.9f,%.9f,%.3f,%.3f,%s,%u,%s,%s,%s,%s,%s,%u,%s,%s\n",
                     name, when, fix.lat, fix.lon, fix.altMsl + fix.geoidSep, fix.altMsl,
                     solutionName(fix.type), fix.sats, sl, so, sh, age, base, stats.samples,
                     spread, stats.rawFile);
  // Truncated is worse than nothing: half a line would corrupt the file.
  return (n + 1 < max) ? n : 0;
}

// Meters per degree of latitude. Good to a fraction of a percent anywhere, and
// it only ever scales centimeter offsets, so the error is far below a
// millimeter - and converting back uses the same factor, so the mean itself
// carries none.
static const double kMPerDeg = 111320.0;
static const double kPi = 3.14159265358979323846;

void PointAverager::reset() {
  for (Acc &a : acc_) a = Acc();
}

void PointAverager::add(const GnssFix &fix) {
  if (!fix.havePos || fix.type == PosType::None) return;
  Acc &a = acc_[(int)fix.type];
  if (!a.n) a.first = fix;
  const double n = (fix.lat - a.first.lat) * kMPerDeg;
  const double e = (fix.lon - a.first.lon) * kMPerDeg * cos(a.first.lat * kPi / 180.0);
  a.n++;
  a.north += n;
  a.east += e;
  a.north2 += n * n;
  a.east2 += e * e;
  a.alt += fix.altMsl;
  a.sep += fix.geoidSep;
  if (fix.haveAcc) {
    a.latStd += fix.latStd;
    a.lonStd += fix.lonStd;
    a.altStd += fix.altStd;
    a.nStd++;
  }
}

const PointAverager::Acc *PointAverager::best() const {
  unsigned total = 0;
  for (const Acc &a : acc_) total += a.n;
  // Four types share the epochs, so one of them always has a quarter.
  for (int t = (int)PosType::Fixed; t > (int)PosType::None; --t)
    if (acc_[t].n && acc_[t].n * 4 >= total) return &acc_[t];
  return nullptr;
}

unsigned PointAverager::samples() const {
  const Acc *a = best();
  return a ? a->n : 0;
}

bool PointAverager::result(GnssFix *out, PointStats *stats) const {
  const Acc *a = best();
  if (!a) return false;
  const double n = a->north / a->n, e = a->east / a->n;
  *out = a->first;
  out->lat = a->first.lat + n / kMPerDeg;
  out->lon = a->first.lon + e / (kMPerDeg * cos(a->first.lat * kPi / 180.0));
  out->altMsl = a->alt / a->n;
  out->geoidSep = a->sep / a->n;
  // The receiver's own figures, averaged rather than shrunk by the sample
  // count: successive epochs are far from independent, so dividing by the
  // square root of n would claim an accuracy nobody measured.
  out->haveAcc = a->nStd > 0;
  if (a->nStd) {
    out->latStd = (float)(a->latStd / a->nStd);
    out->lonStd = (float)(a->lonStd / a->nStd);
    out->altStd = (float)(a->altStd / a->nStd);
  }
  stats->samples = a->n;
  const double var = a->north2 / a->n - n * n + a->east2 / a->n - e * e;
  stats->spreadM = a->n > 1 ? (float)sqrt(var > 0 ? var : 0) : -1.0f;
  return true;
}

const char *pointsFileName(PointsFormat f) {
  switch (f) {
    case PointsFormat::GeoJson: return "points.geojson";
    case PointsFormat::Kml: return "points.kml";
    case PointsFormat::Gpx: return "points.gpx";
    default: return "points.csv";
  }
}

const char *pointsMimeType(PointsFormat f) {
  switch (f) {
    case PointsFormat::GeoJson: return "application/geo+json";
    case PointsFormat::Kml: return "application/vnd.google-earth.kml+xml";
    case PointsFormat::Gpx: return "application/gpx+xml";
    default: return "text/csv";
  }
}

size_t pointsExportBegin(PointsFormat f, char *out, size_t max) {
  switch (f) {
    case PointsFormat::GeoJson:
      return appendf(out, max, 0, "{\"type\":\"FeatureCollection\",\"features\":[\n");
    case PointsFormat::Kml:
      return appendf(out, max, 0,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>"
                     "<name>RTK points</name>\n");
    case PointsFormat::Gpx:
      return appendf(out, max, 0,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<gpx version=\"1.1\" creator=\"RTK Rover\" "
                     "xmlns=\"http://www.topografix.com/GPX/1/1\">\n");
    default:
      return appendf(out, max, 0, "%s", kPointsCsvHeader);
  }
}

size_t pointsExportEnd(PointsFormat f, char *out, size_t max) {
  switch (f) {
    case PointsFormat::GeoJson: return appendf(out, max, 0, "\n]}\n");
    case PointsFormat::Kml: return appendf(out, max, 0, "</Document></kml>\n");
    case PointsFormat::Gpx: return appendf(out, max, 0, "</gpx>\n");
    default: return 0;
  }
}

// Every field is written by this firmware, so anything outside its alphabet
// means a damaged line - and dropping it is what keeps the XML and JSON valid
// without escaping anything.
static bool plausible(const char *s) {
  for (; *s; ++s) {
    const char c = *s;
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          c == '.' || c == ',' || c == '-' || c == '+' || c == ':' || c == '_'))
      return false;
  }
  return true;
}

// JSON value: the text columns quoted, numbers raw, missing ones null.
static size_t jsonValue(char *out, size_t max, size_t n, int col, const char *v) {
  if (!v[0]) return appendf(out, max, n, "null");
  if (col == kName || col == kTime || col == kSolution || col == kRawFile)
    return appendf(out, max, n, "\"%s\"", v);
  return appendf(out, max, n, "%s", v);
}

size_t pointsExportLine(PointsFormat f, const char *line, bool first, char *out, size_t max) {
  if (!max) return 0;
  out[0] = '\0';
  if (f == PointsFormat::Csv) {
    if (!strncmp(line, "name,", 5)) return 0;
    return appendf(out, max, 0, "%s\n", line);
  }

  char buf[256];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  size_t len = strlen(buf);
  while (len && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) buf[--len] = '\0';
  if (!len || !strncmp(buf, "name,", 5) || !plausible(buf)) return 0;

  const char *v[kCols];
  int nv = 0;
  for (char *p = buf; nv < kCols;) {
    v[nv++] = p;
    char *c = strchr(p, ',');
    if (!c) break;
    *c = '\0';
    p = c + 1;
  }
  // Newer firmware may append columns; fewer than ours means damage.
  if (nv < kCols || !v[kName][0] || !v[kLat][0] || !v[kLon][0]) return 0;

  size_t n = 0;
  switch (f) {
    case PointsFormat::GeoJson: {
      // RFC 7946 heights are above the ellipsoid, not above sea level.
      n = appendf(out, max, n,
                  "%s{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
                  "\"coordinates\":[%s,%s,%s]},\"properties\":{",
                  first ? "" : ",\n", v[kLon], v[kLat], v[kEllh]);
      bool firstProp = true;
      for (int i = 0; i < kCols; ++i) {
        if (i == kLat || i == kLon) continue;
        n = appendf(out, max, n, "%s\"%s\":", firstProp ? "" : ",", kColName[i]);
        n = jsonValue(out, max, n, i, v[i]);
        firstProp = false;
      }
      n = appendf(out, max, n, "}}");
      break;
    }
    case PointsFormat::Kml: {
      // KML's absolute altitude is above sea level (EGM96), so the MSL height.
      n = appendf(out, max, n, "<Placemark><name>%s</name>", v[kName]);
      if (v[kTime][0]) n = appendf(out, max, n, "<TimeStamp><when>%s</when></TimeStamp>", v[kTime]);
      n = appendf(out, max, n, "<ExtendedData>");
      for (int i = 0; i < kCols; ++i)
        if (i != kName && v[i][0])
          n = appendf(out, max, n, "<Data name=\"%s\"><value>%s</value></Data>",
                      kColName[i], v[i]);
      n = appendf(out, max, n,
                  "</ExtendedData><Point><altitudeMode>absolute</altitudeMode>"
                  "<coordinates>%s,%s,%s</coordinates></Point></Placemark>\n",
                  v[kLon], v[kLat], v[kMsl]);
      break;
    }
    case PointsFormat::Gpx: {
      // Element order is fixed by the GPX schema: ele, time, geoidheight, name,
      // desc, fix, sat. GPX has no RTK fix type; dgps is the closest it has.
      n = appendf(out, max, n, "<wpt lat=\"%s\" lon=\"%s\">", v[kLat], v[kLon]);
      if (v[kMsl][0]) n = appendf(out, max, n, "<ele>%s</ele>", v[kMsl]);
      if (v[kTime][0]) n = appendf(out, max, n, "<time>%s</time>", v[kTime]);
      if (v[kEllh][0] && v[kMsl][0])
        n = appendf(out, max, n, "<geoidheight>%.3f</geoidheight>",
                    atof(v[kEllh]) - atof(v[kMsl]));
      n = appendf(out, max, n, "<name>%s</name><desc>%s", v[kName], v[kSolution]);
      if (v[kStdLat][0])
        n = appendf(out, max, n, " std N %s E %s U %s m", v[kStdLat], v[kStdLon], v[kStdHgt]);
      const bool rtk = !strcmp(v[kSolution], "FIXED") || !strcmp(v[kSolution], "FLOAT") ||
                       !strcmp(v[kSolution], "DGPS");
      n = appendf(out, max, n, "</desc><fix>%s</fix><sat>%s</sat></wpt>\n",
                  rtk ? "dgps" : "3d", v[kSats]);
      break;
    }
    default:
      break;
  }
  // A partial element would break the whole document; drop the point instead.
  return (n + 1 < max) ? n : 0;
}
