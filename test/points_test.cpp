// Host-native tests for the point record and its export formats:
//   bazel test //test:all
#include "points_format.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
static void check(bool ok, const char *what) {
  printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) fails++;
}

static bool has(const std::string &s, const char *part) {
  return s.find(part) != std::string::npos;
}

static GnssFix fixedFix() {
  GnssFix f;
  f.lat = 48.137154321;
  f.lon = 11.575498765;
  f.altMsl = 519.25;
  f.geoidSep = 47.5;
  f.type = PosType::Fixed;
  f.sats = 24;
  f.latStd = 0.012f;
  f.lonStd = 0.010f;
  f.altStd = 0.021f;
  f.year = 2026; f.mon = 10; f.day = 6; f.hour = 9; f.min = 5; f.sec = 7;
  f.diffAge = 1.0f;
  f.baseId = 401;
  f.havePos = f.haveAcc = f.haveTime = true;
  return f;
}

static std::string exportAll(PointsFormat fmt, const std::string &file) {
  char out[1024];
  std::string doc(out, pointsExportBegin(fmt, out, sizeof(out)));
  bool first = true;
  size_t at = 0;
  while (at < file.size()) {
    size_t nl = file.find('\n', at);
    if (nl == std::string::npos) nl = file.size();
    const std::string line = file.substr(at, nl - at);
    at = nl + 1;
    const size_t n = pointsExportLine(fmt, line.c_str(), first, out, sizeof(out));
    if (n) { doc.append(out, n); first = false; }
  }
  doc.append(out, pointsExportEnd(fmt, out, sizeof(out)));
  return doc;
}

int main() {
  printf("Points\n");
  char line[256];

  // The record itself.
  const size_t n = pointsFormatRecord(line, sizeof(line), "P0001", fixedFix());
  const std::string rec(line, n);
  check(n > 0 && rec.back() == '\n', "record is one terminated line");
  check(rec == "P0001,2026-10-06T09:05:07Z,48.137154321,11.575498765,566.750,519.250,"
               "FIXED,24,0.012,0.010,0.021,1.0,401,1,,\n",
        "record columns");
  int commas = 0, headerCommas = 0;
  for (char c : rec) commas += c == ',';
  for (const char *p = kPointsCsvHeader; *p; ++p) headerCommas += *p == ',';
  check(commas == headerCommas, "record matches the header");

  // What is not known is left empty, not invented.
  GnssFix bare = fixedFix();
  bare.type = PosType::Single;
  bare.haveAcc = bare.haveTime = false;
  bare.diffAge = -1;
  pointsFormatRecord(line, sizeof(line), "P0002", bare);
  check(!strcmp(line, "P0002,,48.137154321,11.575498765,566.750,519.250,SINGLE,24,,,,,,1,,\n"),
        "unknowns left empty");

  GnssFix none = fixedFix();
  none.havePos = false;
  check(pointsFormatRecord(line, sizeof(line), "P0003", none) == 0, "no position, no record");
  none = fixedFix();
  none.type = PosType::None;
  check(pointsFormatRecord(line, sizeof(line), "P0003", none) == 0, "quality 0, no record");
  check(pointsFormatRecord(line, 40, "P0003", fixedFix()) == 0, "never a truncated record");

  // Southern and western hemispheres keep their signs.
  GnssFix sw = fixedFix();
  sw.lat = -33.8688;
  sw.lon = -151.2093;
  pointsFormatRecord(line, sizeof(line), "P0004", sw);
  check(has(line, ",-33.868800000,-151.209300000,"), "negative coordinates");

  const std::string file = std::string(kPointsCsvHeader) + rec +
                           "P0002,,48.137154321,11.575498765,566.750,519.250,SINGLE,24,,,,,,1,,\n"
                           "garbage<line>\n"
                           "P0009,2026\n";

  const std::string csv = exportAll(PointsFormat::Csv, file);
  check(csv.rfind(kPointsCsvHeader, 0) == 0, "csv: header once, first");
  check(csv.find("name,", 1) == std::string::npos, "csv: stored header not repeated");

  const std::string gj = exportAll(PointsFormat::GeoJson, file);
  check(has(gj, "\"coordinates\":[11.575498765,48.137154321,566.750]"),
        "geojson: lon,lat,ellipsoidal height");
  check(has(gj, "\"solution\":\"FIXED\"") && has(gj, "\"satellites\":24"),
        "geojson: properties typed");
  check(has(gj, "\"std_lat_m\":null"), "geojson: unknown is null");
  check(has(gj, "}},\n{") && !has(gj, "[\n,"), "geojson: commas only between");
  check(!has(gj, "garbage") && !has(gj, "P0009"), "geojson: damaged lines dropped");

  const std::string kml = exportAll(PointsFormat::Kml, file);
  check(has(kml, "<coordinates>11.575498765,48.137154321,519.250</coordinates>"),
        "kml: lon,lat,sea-level height");
  check(has(kml, "<when>2026-10-06T09:05:07Z</when>"), "kml: timestamp");
  check(!has(kml, "garbage"), "kml: damaged lines dropped");

  const std::string gpx = exportAll(PointsFormat::Gpx, file);
  check(has(gpx, "<wpt lat=\"48.137154321\" lon=\"11.575498765\"><ele>519.250</ele>"
                 "<time>2026-10-06T09:05:07Z</time><geoidheight>47.500</geoidheight>"
                 "<name>P0001</name>"),
        "gpx: schema element order");
  check(has(gpx, "<fix>dgps</fix>") && has(gpx, "<fix>3d</fix>"), "gpx: fix types");

  // An occupation: fixed epochs scattered about a center, plus float ones.
  {
    PointAverager avg;
    GnssFix f = fixedFix();
    const double c = 48.137154321, d = 1.0 / 111320.0;  // one meter of latitude
    const double offs[4] = {+0.01, -0.01, +0.02, -0.02};
    for (double o : offs) { f.lat = c + o * d; avg.add(f); }
    GnssFix fl = fixedFix();
    fl.type = PosType::Float;
    fl.lat = c + 0.5 * d;
    avg.add(fl);
    GnssFix nopos = fixedFix();
    nopos.havePos = false;
    avg.add(nopos);
    GnssFix out;
    PointStats st;
    check(avg.result(&out, &st), "averager: has a result");
    check(out.type == PosType::Fixed && st.samples == 4, "averager: fixed epochs only");
    check(std::fabs((out.lat - c) / d) < 0.0005, "averager: mean within 0.5 mm");
    check(std::fabs(st.spreadM - 0.0158) < 0.0005, "averager: rms spread");

    // One fixed epoch among many float ones is not trusted.
    PointAverager few;
    for (int i = 0; i < 10; ++i) few.add(fl);
    few.add(fixedFix());
    few.result(&out, &st);
    check(out.type == PosType::Float && st.samples == 10, "averager: lone fix ignored");

    PointAverager none;
    check(!none.result(&out, &st), "averager: empty, no result");

    st.rawFile[0] = 0;
    strcpy(st.rawFile, "P0005_20261006T090507.rtcm3");
    pointsFormatRecord(line, sizeof(line), "P0005", fixedFix(), st);
    check(has(line, ",401,10,"), "record: sample count");
    check(has(line, ",P0005_20261006T090507.rtcm3\n"), "record: raw file named");
    const std::string g = exportAll(PointsFormat::GeoJson, std::string(kPointsCsvHeader) + line);
    check(has(g, "\"raw_file\":\"P0005_20261006T090507.rtcm3\""), "geojson: raw file is text");
  }

  // An empty file is still a valid, empty document.
  const std::string empty = exportAll(PointsFormat::GeoJson, kPointsCsvHeader);
  check(empty == "{\"type\":\"FeatureCollection\",\"features\":[\n\n]}\n", "empty export");

  printf(fails ? "\n%d FAILED\n" : "\nall passed (%d failures)\n", fails);
  return fails ? 1 : 0;
}
