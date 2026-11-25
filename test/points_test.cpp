// Host-native tests for the point record and its export formats:
//   bazel test //test:points_test
#include "points_format.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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

TEST_CASE("Points", "[points]") {
  char line[256];

  // The record itself.
  const size_t n = pointsFormatRecord(line, sizeof(line), "P0001", fixedFix());
  const std::string rec(line, n);
  // record is one terminated line
  CHECK((n > 0 && rec.back() == '\n'));
  // record columns
  CHECK(rec == "P0001,2026-10-06T09:05:07Z,48.137154321,11.575498765,566.750,519.250,"
               "FIXED,24,0.012,0.010,0.021,1.0,401,1,,\n");
  int commas = 0, headerCommas = 0;
  for (char c : rec) commas += c == ',';
  for (const char *p = kPointsCsvHeader; *p; ++p) headerCommas += *p == ',';
  // record matches the header
  CHECK(commas == headerCommas);

  // What is not known is left empty, not invented.
  GnssFix bare = fixedFix();
  bare.type = PosType::Single;
  bare.haveAcc = bare.haveTime = false;
  bare.diffAge = -1;
  pointsFormatRecord(line, sizeof(line), "P0002", bare);
  // unknowns left empty
  CHECK(!strcmp(line, "P0002,,48.137154321,11.575498765,566.750,519.250,SINGLE,24,,,,,,1,,\n"));

  GnssFix none = fixedFix();
  none.havePos = false;
  // no position, no record
  CHECK(pointsFormatRecord(line, sizeof(line), "P0003", none) == 0);
  none = fixedFix();
  none.type = PosType::None;
  // quality 0, no record
  CHECK(pointsFormatRecord(line, sizeof(line), "P0003", none) == 0);
  // never a truncated record
  CHECK(pointsFormatRecord(line, 40, "P0003", fixedFix()) == 0);

  // Southern and western hemispheres keep their signs.
  GnssFix sw = fixedFix();
  sw.lat = -33.8688;
  sw.lon = -151.2093;
  pointsFormatRecord(line, sizeof(line), "P0004", sw);
  // negative coordinates
  CHECK(has(line, ",-33.868800000,-151.209300000,"));

  const std::string file = std::string(kPointsCsvHeader) + rec +
                           "P0002,,48.137154321,11.575498765,566.750,519.250,SINGLE,24,,,,,,1,,\n"
                           "garbage<line>\n"
                           "P0009,2026\n";

  const std::string csv = exportAll(PointsFormat::Csv, file);
  // csv: header once, first
  CHECK(csv.rfind(kPointsCsvHeader, 0) == 0);
  // csv: stored header not repeated
  CHECK(csv.find("name,", 1) == std::string::npos);

  const std::string gj = exportAll(PointsFormat::GeoJson, file);
  // geojson: lon,lat,ellipsoidal height
  CHECK(has(gj, "\"coordinates\":[11.575498765,48.137154321,566.750]"));
  // geojson: properties typed
  CHECK((has(gj, "\"solution\":\"FIXED\"") && has(gj, "\"satellites\":24")));
  // geojson: unknown is null
  CHECK(has(gj, "\"std_lat_m\":null"));
  // geojson: commas only between
  CHECK((has(gj, "}},\n{") && !has(gj, "[\n,")));
  // geojson: damaged lines dropped
  CHECK((!has(gj, "garbage") && !has(gj, "P0009")));

  const std::string kml = exportAll(PointsFormat::Kml, file);
  // kml: lon,lat,sea-level height
  CHECK(has(kml, "<coordinates>11.575498765,48.137154321,519.250</coordinates>"));
  // kml: timestamp
  CHECK(has(kml, "<when>2026-10-06T09:05:07Z</when>"));
  // kml: damaged lines dropped
  CHECK(!has(kml, "garbage"));

  const std::string gpx = exportAll(PointsFormat::Gpx, file);
  // gpx: schema element order
  CHECK(has(gpx, "<wpt lat=\"48.137154321\" lon=\"11.575498765\"><ele>519.250</ele>"
                 "<time>2026-10-06T09:05:07Z</time><geoidheight>47.500</geoidheight>"
                 "<name>P0001</name>"));
  // gpx: fix types
  CHECK((has(gpx, "<fix>dgps</fix>") && has(gpx, "<fix>3d</fix>")));

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
    // averager: has a result
    CHECK(avg.result(&out, &st));
    // averager: fixed epochs only
    CHECK((out.type == PosType::Fixed && st.samples == 4));
    // averager: mean within 0.5 mm
    CHECK(std::fabs((out.lat - c) / d) < 0.0005);
    // averager: rms spread
    CHECK(std::fabs(st.spreadM - 0.0158) < 0.0005);

    // One fixed epoch among many float ones is not trusted.
    PointAverager few;
    for (int i = 0; i < 10; ++i) few.add(fl);
    few.add(fixedFix());
    few.result(&out, &st);
    // averager: lone fix ignored
    CHECK((out.type == PosType::Float && st.samples == 10));

    PointAverager none;
    // averager: empty, no result
    CHECK(!none.result(&out, &st));

    st.rawFile[0] = 0;
    strcpy(st.rawFile, "P0005_20261006T090507.rtcm3");
    pointsFormatRecord(line, sizeof(line), "P0005", fixedFix(), st);
    // record: sample count
    CHECK(has(line, ",401,10,"));
    // record: raw file named
    CHECK(has(line, ",P0005_20261006T090507.rtcm3\n"));
    const std::string g = exportAll(PointsFormat::GeoJson, std::string(kPointsCsvHeader) + line);
    // geojson: raw file is text
    CHECK(has(g, "\"raw_file\":\"P0005_20261006T090507.rtcm3\""));
  }

  // An empty file is still a valid, empty document.
  const std::string empty = exportAll(PointsFormat::GeoJson, kPointsCsvHeader);
  // empty export
  CHECK(empty == "{\"type\":\"FeatureCollection\",\"features\":[\n\n]}\n");
}
