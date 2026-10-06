#include "points.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

#include "config.h"

#define RAW_DIR "/raw"

static bool mounted = false;
static uint32_t count = 0;
static char last[256] = "";

static GnssFix current;
static uint32_t currentAt = 0;

static PointPhase phase = PointPhase::Idle;
static uint32_t phaseAt = 0;
static char name[16] = "";
static PointAverager avg;
static File raw;
static char rawName[48] = "";
static uint32_t rawBytes = 0;
static char outcome[160] = "";

static uint32_t feedbackFrom = 0;
static MarkResult feedback = MarkResult::Saved;
static bool feedbackOn = false;

// Debounce state: the level last read, when it last changed, and the level
// that has held still long enough to believe.
static bool rawDown = false, stableDown = false;
static uint32_t rawSince = 0, lastPressAt = 0;

// Counts the records and remembers the last, so the portal can show both
// without reading the file on every page.
static void scan() {
  count = 0;
  last[0] = '\0';
  File f = LittleFS.open(POINTS_FILE, "r");
  if (!f) return;
  char line[sizeof(last)];
  size_t n = 0;
  bool header = true;
  while (f.available()) {
    const int c = f.read();
    if (c == '\n') {
      line[n] = '\0';
      if (!header && n) {
        count++;
        strlcpy(last, line, sizeof(last));
      }
      header = false;
      n = 0;
    } else if (n + 1 < sizeof(line)) {
      line[n++] = (char)c;
    }
  }
  f.close();
}

void pointsBegin() {
  pinMode(PIN_MARK_BUTTON, INPUT_PULLUP);
  // Formats on first boot only; every later mount finds the file system that
  // is already there. The partition is the "spiffs" slot of huge_app.csv.
  mounted = LittleFS.begin(true);
  if (!mounted) return;
  if (!LittleFS.exists(RAW_DIR)) LittleFS.mkdir(RAW_DIR);
  scan();
}

static size_t freeBytes() {
  return mounted ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
}

static bool fresh() { return currentAt && millis() - currentAt <= POINTS_MAX_AGE_MS; }

void pointsOnEpoch(const GnssFix &fix) {
  current = fix;
  currentAt = millis();
  if (phase == PointPhase::Recording) avg.add(fix);
}

void pointsOnRtcm(const uint8_t *frame, size_t len) {
  if (phase == PointPhase::Idle || !raw) return;
  // Raw data is the extra; the point is the product. Stop logging well before
  // the point itself could no longer be written.
  if (freeBytes() < POINTS_MIN_FREE_BYTES + len) {
    raw.close();
    return;
  }
  if (raw.write(frame, len) == len) rawBytes += len;
}

bool pointsWantRaw() { return POINTS_LOG_RAW && phase != PointPhase::Idle; }

PointPhase pointsPhase() { return phase; }

uint32_t pointsPhaseLeftMs() {
  const uint32_t span = phase == PointPhase::Settling ? POINT_SETTLE_MS
                        : phase == PointPhase::Recording ? POINT_OCCUPY_MS : 0;
  const uint32_t gone = millis() - phaseAt;
  return gone >= span ? 0 : span - gone;
}

const char *pointsCurrentName() { return phase == PointPhase::Idle ? "" : name; }

unsigned pointsCurrentSamples() { return phase == PointPhase::Recording ? avg.samples() : 0; }

const char *pointsLastOutcome() { return outcome; }

static void show(MarkResult r) {
  feedback = r;
  feedbackFrom = millis();
  feedbackOn = true;
}

static void dropRaw() {
  if (raw) raw.close();
  if (rawName[0]) LittleFS.remove(String(RAW_DIR "/") + rawName);
  rawName[0] = '\0';
}

MarkResult pointsStart() {
  if (phase != PointPhase::Idle) return MarkResult::Started;
  // Not a position - the settle time is there to wait for one - but proof the
  // receiver is talking. While the portal holds the device nothing is parsed.
  if (!fresh()) {
    snprintf(outcome, sizeof(outcome), "Not started: no data from the receiver.");
    show(MarkResult::NoPosition);
    return MarkResult::NoPosition;
  }
  if (!mounted || freeBytes() < POINTS_MIN_FREE_BYTES) {
    snprintf(outcome, sizeof(outcome), "Not started: storage is full - download and "
                                       "delete points.");
    show(MarkResult::Full);
    return MarkResult::Full;
  }

  snprintf(name, sizeof(name), "P%04lu", (unsigned long)(count + 1));
  avg.reset();
  rawBytes = 0;
  rawName[0] = '\0';
  // Without room for a whole occupation's worth, the point is still taken -
  // just without the raw log.
  if (POINTS_LOG_RAW && freeBytes() >= POINTS_MIN_FREE_BYTES + POINTS_RAW_RESERVE_BYTES) {
    // The date in the name is what the RINEX converter needs: RTCM carries
    // time of week only, not which week.
    if (current.haveTime)
      snprintf(rawName, sizeof(rawName), "%s_%04u%02u%02uT%02u%02u%02u.rtcm3", name,
               current.year, current.mon, current.day, current.hour, current.min,
               current.sec);
    else
      snprintf(rawName, sizeof(rawName), "%s.rtcm3", name);
    raw = LittleFS.open(String(RAW_DIR "/") + rawName, "w");
    if (!raw) rawName[0] = '\0';
  }
  phase = PointPhase::Settling;
  phaseAt = millis();
  feedbackOn = false;
  return MarkResult::Started;
}

void pointsCancel() {
  if (phase == PointPhase::Idle) return;
  dropRaw();
  phase = PointPhase::Idle;
  snprintf(outcome, sizeof(outcome), "%s canceled, nothing saved.", name);
  show(MarkResult::Canceled);
}

void pointsFinish() {
  if (phase == PointPhase::Idle) return;
  if (phase == PointPhase::Settling) {
    pointsCancel();
    return;
  }
  phase = PointPhase::Idle;
  if (raw) raw.close();

  GnssFix fix;
  PointStats st;
  if (!avg.result(&fix, &st)) {
    dropRaw();
    snprintf(outcome, sizeof(outcome), "%s not saved: no position while recording.", name);
    show(MarkResult::NoPosition);
    return;
  }
  // An empty log means the receiver sent nothing raw - say so in the record
  // by naming no file, rather than keep a file that only looks like data.
  if (!rawBytes) dropRaw();
  strlcpy(st.rawFile, rawName, sizeof(st.rawFile));

  char line[sizeof(last)];
  const size_t n = pointsFormatRecord(line, sizeof(line), name, fix, st);
  const bool fileNew = !LittleFS.exists(POINTS_FILE);
  File f = n ? LittleFS.open(POINTS_FILE, "a") : File();
  bool ok = (bool)f;
  if (ok && fileNew) ok = f.print(kPointsCsvHeader) == strlen(kPointsCsvHeader);
  ok = ok && f.write((const uint8_t *)line, n) == n;
  // Closed every time: power lost mid-survey must cost at most the
  // point being written, never the ones before it.
  if (f) f.close();
  if (!ok) {
    snprintf(outcome, sizeof(outcome), "%s not saved: storage error.", name);
    show(MarkResult::Error);
    return;
  }

  count++;
  strlcpy(last, line, sizeof(last));
  last[strcspn(last, "\n")] = '\0';

  const bool fixed = fix.type == PosType::Fixed;
  char spread[24] = "";
  if (st.spreadM >= 0) snprintf(spread, sizeof(spread), ", spread %.3f m", (double)st.spreadM);
  char rawNote[40];
  if (!POINTS_LOG_RAW) rawNote[0] = '\0';
  else if (rawBytes) snprintf(rawNote, sizeof(rawNote), ", raw %lu kB", (unsigned long)(rawBytes / 1024));
  else snprintf(rawNote, sizeof(rawNote), ", no raw data");
  snprintf(outcome, sizeof(outcome), "%s saved%s: %u epochs%s%s.", name,
           fixed ? " (RTK fixed)" : ", but NOT RTK fixed - check before relying on it",
           st.samples, spread, rawNote);
  show(fixed ? MarkResult::Saved : MarkResult::NotFixed);
}

void pointsPress() {
  switch (phase) {
    case PointPhase::Idle: pointsStart(); break;
    case PointPhase::Settling: pointsCancel(); break;
    case PointPhase::Recording: pointsFinish(); break;
  }
}

void pointsTask() {
  const uint32_t now = millis();
  const bool down = digitalRead(PIN_MARK_BUTTON) == LOW;
  if (down != rawDown) {
    rawDown = down;
    rawSince = now;
  }
  if (rawDown != stableDown && now - rawSince >= MARK_DEBOUNCE_MS) {
    stableDown = rawDown;
    // On the press, not the release. A second press inside the repeat window
    // is a bounce or an impatient thumb, not a decision.
    if (stableDown && now - lastPressAt >= MARK_REPEAT_MS) {
      lastPressAt = now;
      pointsPress();
    }
  }

  if (phase == PointPhase::Settling && pointsPhaseLeftMs() == 0) {
    phase = PointPhase::Recording;
    phaseAt = now;
    avg.reset();
  } else if (phase == PointPhase::Recording && pointsPhaseLeftMs() == 0) {
    pointsFinish();
  }
}

uint32_t pointsCount() { return count; }

const char *pointsLast() { return last; }

bool pointsClear() {
  if (!mounted || phase != PointPhase::Idle) return false;
  if (LittleFS.exists(POINTS_FILE) && !LittleFS.remove(POINTS_FILE)) return false;
  File dir = LittleFS.open(RAW_DIR);
  if (dir) {
    // Collected first: removing entries while walking the directory skips some.
    String names[64];
    size_t n = 0;
    for (File f = dir.openNextFile(); f && n < 64; f = dir.openNextFile())
      names[n++] = String(RAW_DIR "/") + f.name();
    dir.close();
    for (size_t i = 0; i < n; ++i) LittleFS.remove(names[i]);
  }
  count = 0;
  last[0] = '\0';
  outcome[0] = '\0';
  return true;
}

void pointsExport(PointsFormat fmt, void (*emit)(const char *data, size_t len)) {
  char out[1024];
  size_t n = pointsExportBegin(fmt, out, sizeof(out));
  if (n) emit(out, n);

  File f = mounted ? LittleFS.open(POINTS_FILE, "r") : File();
  if (f) {
    char line[sizeof(last)];
    size_t ln = 0;
    bool first = true;
    while (f.available()) {
      const int c = f.read();
      if (c != '\n') {
        if (ln + 1 < sizeof(line)) line[ln++] = (char)c;
        continue;
      }
      line[ln] = '\0';
      ln = 0;
      n = pointsExportLine(fmt, line, first, out, sizeof(out));
      if (n) {
        emit(out, n);
        first = false;
      }
    }
    f.close();
  }

  n = pointsExportEnd(fmt, out, sizeof(out));
  if (n) emit(out, n);
}

void pointsRawList(void (*each)(const char *name, size_t bytes, void *ctx), void *ctx) {
  if (!mounted) return;
  File dir = LittleFS.open(RAW_DIR);
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    // The one still being written is not a download yet.
    if (phase != PointPhase::Idle && !strcmp(f.name(), rawName)) continue;
    each(f.name(), f.size(), ctx);
  }
  dir.close();
}

// Only names this firmware makes, so a crafted request cannot leave the
// directory.
static bool rawNameOk(const char *s) {
  if (!s[0] || strlen(s) >= sizeof(rawName)) return false;
  for (; *s; ++s)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
          *s == '_' || (*s == '.' && s[1] != '.')))
      return false;
  return true;
}

bool pointsRawSend(const char *file, void (*emit)(const char *data, size_t len)) {
  if (!mounted || !rawNameOk(file)) return false;
  File f = LittleFS.open(String(RAW_DIR "/") + file, "r");
  if (!f) return false;
  if (!emit) {
    f.close();
    return true;
  }
  char buf[1024];
  for (size_t n; (n = f.read((uint8_t *)buf, sizeof(buf))) > 0;) emit(buf, n);
  f.close();
  return true;
}

bool pointsLamps(Lamps *l) {
  const uint32_t now = millis();
  *l = {false, false, false};

  if (phase == PointPhase::Settling) {
    const uint32_t t = (now - phaseAt) % 500;
    l->green = t < 80;
    l->yellow = t >= 250 && t < 330;
    return true;
  }
  if (phase == PointPhase::Recording) {
    const bool blip = (now - phaseAt) % 500 < 80;
    if (!fresh() || !current.havePos) l->red = blip;
    else if (current.type == PosType::Fixed) l->green = blip;
    else l->yellow = blip;
    return true;
  }

  if (!feedbackOn) return false;
  const uint32_t t = now - feedbackFrom;
  if (feedback == MarkResult::Saved || feedback == MarkResult::NotFixed) {
    if (t >= 900) return feedbackOn = false;
    const bool lit = (t % 300) < 150;
    if (feedback == MarkResult::Saved) l->green = lit;
    else l->yellow = lit;
    return true;
  }
  if (t >= 1000) return feedbackOn = false;
  l->red = true;
  return true;
}
