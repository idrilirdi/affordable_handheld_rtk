#!/usr/bin/env python3
"""Record the rover's debug panel to CSV over WiFi.

The panel is the only view into a running survey - attaching USB resets the
board and restarts the navigation filter - so a walked test can only be
recorded this way. Polls once a second and writes one row per poll.

A failed poll still writes a row, with `ok` set to 0 and the reason in `note`.
Gaps in a walked test are data: they say where the radio link died, and a
silently shortened file would hide exactly that.
"""

import argparse
import csv
import re
import sys
import time
import urllib.error
import urllib.request

FIELDS = [
    "t", "iso", "ok", "note",
    # fix
    "type", "sats", "snap_age_s",
    "lat", "lon", "alt_m",
    "acc_e_m", "acc_n_m", "acc_u_m", "acc_src",
    "corr_age_s", "base_id",
    # sky
    "sky_total", "gps", "glo", "gal", "bds",
    "cno_med", "cno_peak", "cno_med_hi", "cno_ge40", "cno_meas",
    "band_l1", "band_l2", "band_l5",
    "elev_60_90", "elev_60_90_n", "elev_30_60", "elev_30_60_n",
    "elev_15_30", "elev_15_30_n", "elev_0_15", "elev_0_15_n",
    # corrections
    "base_lat", "base_lon", "base_alt_m", "baseline_m",
    "rtcm_ok", "rtcm_bad",
    # window statistics
    "win_s", "win_n",
    "pct_fix", "pct_float", "pct_diff", "pct_single", "pct_none",
    "drops_type", "drops_acc", "longest_fix_s",
    "hacc_med", "hacc_min", "hacc_max",
    "scat_rms_m", "scat_worst_m", "scat_n",
    "cage_mean_s", "cage_max_s", "cage_stale",
    # health
    "ble_raw", "ble_notify_fail", "ble_down_b", "ble_down_bps", "ble_drop",
    "loop_hz", "loop_worst_ms", "up_s", "heap", "heap_min", "epochs",
    "gnss_rx_b", "gnss_tx_b", "boot_n", "reset_reason", "batt",
]

# One pattern per line of the panel. Anything absent stays empty rather than
# being defaulted: a missing measurement and a zero are different things.
PATTERNS = [
    (r"^FIX\s+(\S+)\s+sats (\d+)\s+age (\d+)s", ["type", "sats", "snap_age_s"]),
    (r"^POS\s+([-\d.]+) ([-\d.]+)\s+([-\d.]+) m", ["lat", "lon", "alt_m"]),
    (r"^ACC\s+E ([-\d.]+)\s+N ([-\d.]+)\s+U ([-\d.]+) m\s+\((\w+)\)",
     ["acc_e_m", "acc_n_m", "acc_u_m", "acc_src"]),
    (r"^CORR\s+age ([-\d.]+) s\s+base (\d+)", ["corr_age_s", "base_id"]),
    (r"^CNO\s+med (\d+)\s+peak (\d+)\s+med>30deg (\d+)\s+>=40 (\d+) of (\d+)",
     ["cno_med", "cno_peak", "cno_med_hi", "cno_ge40", "cno_meas"]),
    (r"^BAND\s+L1 (\d+)\s+L2 (\d+)\s+L5 (\d+)", ["band_l1", "band_l2", "band_l5"]),
    (r"^ELEV\s+>60: (\d+) \((\d+)\)\s+30-60: (\d+) \((\d+)\)\s+15-30: (\d+) \((\d+)\)"
     r"\s+0-15: (\d+) \((\d+)\)",
     ["elev_60_90", "elev_60_90_n", "elev_30_60", "elev_30_60_n",
      "elev_15_30", "elev_15_30_n", "elev_0_15", "elev_0_15_n"]),
    (r"^BASE\s+([-\d.]+) ([-\d.]+)\s+([-\d.]+) m(?:\s+baseline ([-\d.]+) m)?",
     ["base_lat", "base_lon", "base_alt_m", "baseline_m"]),
    (r"^RTCM\s+ok (\d+)\s+bad (\d+)", ["rtcm_ok", "rtcm_bad"]),
    (r"^STATS last ([\d.]+) s \((\d+) samples\)", ["win_s", "win_n"]),
    (r"^TYPE\s+fix (\d+)%\s+float (\d+)%\s+diff (\d+)%\s+single (\d+)%\s+none (\d+)%",
     ["pct_fix", "pct_float", "pct_diff", "pct_single", "pct_none"]),
    (r"^DROPS (\d+) type\s+(\d+) accuracy\s+longest fix (\d+) s",
     ["drops_type", "drops_acc", "longest_fix_s"]),
    (r"^HACC\s+med ([\d.]+)\s+min ([\d.]+)\s+max ([\d.]+) m",
     ["hacc_med", "hacc_min", "hacc_max"]),
    (r"^SCAT\s+rms ([-\d.]+)\s+worst ([-\d.]+) m from window mean \((\d+) fixes\)",
     ["scat_rms_m", "scat_worst_m", "scat_n"]),
    (r"^CAGE\s+mean ([-\d.]+)\s+max ([-\d.]+) s\s+stale (\d+)",
     ["cage_mean_s", "cage_max_s", "cage_stale"]),
    (r"^BLE\s+raw (\d+)\s+notify-fail (\d+)\s+down (\d+) B \((\d+) B/s\)"
     r"\s+(\d+) drop",
     ["ble_raw", "ble_notify_fail", "ble_down_b", "ble_down_bps", "ble_drop"]),
    (r"^LOOP\s+([\d.]+)/s\s+worst gap (\d+) ms", ["loop_hz", "loop_worst_ms"]),
    (r"^SYS\s+up (\d+)s\s+heap (\d+) \(min (\d+)\)\s+epochs (\d+)\s+gnss (\d+)/(\d+) B",
     ["up_s", "heap", "heap_min", "epochs", "gnss_rx_b", "gnss_tx_b"]),
    (r"^BOOT\s+#(\d+)\s+last reset: (.+)$", ["boot_n", "reset_reason"]),
    (r"^BATT\s+(.+)$", ["batt"]),
]

SKY_LINE = re.compile(r"^SKY\s+(.*?)\s*\(tracked/visible\)")
SKY_ONE = re.compile(r"(GPS|GLO|GAL|BDS|QZS|IRN|SBS|OTH) (\d+)/(\d+)")


def parse(text):
    row = {k: "" for k in FIELDS}
    for line in text.splitlines():
        for pat, names in PATTERNS:
            m = re.match(pat, line)
            if m:
                for name, val in zip(names, m.groups()):
                    row[name] = "" if val is None else val
                break
        m = SKY_LINE.match(line)
        if m:
            # sky_total counts every constellation; the named columns exist for
            # the four that carry corrections. An unnamed one (QZSS, SBAS) must
            # not land in somebody else's column.
            columns = {"GPS": "gps", "GLO": "glo", "GAL": "gal", "BDS": "bds"}
            total = 0
            for cons, tracked, _visible in SKY_ONE.findall(m.group(1)):
                if cons in columns:
                    row[columns[cons]] = tracked
                total += int(tracked)
            row["sky_total"] = total
    return row


def summarize(path):
    """What the run showed, printed when it ends - the file is for later."""
    import statistics
    rows = [r for r in csv.DictReader(open(path)) if r["ok"] == "1"]
    if not rows:
        print("\nno samples recorded")
        return
    def nums(key):
        out = []
        for r in rows:
            try: out.append(float(r[key]))
            except (ValueError, KeyError): pass
        return out
    types = [r["type"] for r in rows if r["type"]]
    fixed = types.count("FIXED")
    hacc = nums("acc_e_m")
    cno = nums("cno_med_hi")
    print(f"\n\n  samples        {len(rows)} ({len(rows)/60:.1f} min)")
    if types:
        print(f"  fixed          {fixed} of {len(types)}  ({100*fixed//len(types)}%)")
        seen = sorted(set(types))
        print(f"  states seen    {', '.join(seen)}")
    if hacc:
        print(f"  east accuracy  median {statistics.median(hacc):.3f} m   "
              f"worst {max(hacc):.3f} m")
    if cno:
        print(f"  cno >30 deg    median {statistics.median(cno):.0f}   "
              f"min {min(cno):.0f}   max {max(cno):.0f}")
    try:
        print(f"  accuracy drops {rows[-1]['drops_acc']} in the final window")
    except (KeyError, IndexError):
        pass
    print(f"\n  saved to {path}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://192.168.4.1/d.txt")
    ap.add_argument("--out", required=True)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--live", action="store_true",
                    help="one self-updating status line, for watching while walking")
    args = ap.parse_args()

    t0 = time.time()
    try:
        record(args, t0)
    except KeyboardInterrupt:
        pass
    summarize(args.out)


def record(args, t0):
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=FIELDS)
        w.writeheader()
        n = ngood = 0
        last_type = None
        while True:
            start = time.time()
            row = {k: "" for k in FIELDS}
            try:
                with urllib.request.urlopen(args.url, timeout=3) as r:
                    row = parse(r.read().decode("utf-8", "replace"))
                row["ok"] = 1
                ngood += 1
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                row["ok"] = 0
                row["note"] = str(e)[:80]
            row["t"] = round(start - t0, 2)
            row["iso"] = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(start))
            w.writerow(row)
            fh.flush()
            n += 1
            if args.live:
                # Scrolls rather than overwriting one line: while walking, the
                # thing you most need to know is that it is still recording,
                # and a line that redraws in place looks identical to a hung
                # process. Each poll prints, so the terminal is a heartbeat.
                mm, ss = divmod(int(time.time() - t0), 60)
                fix = row.get("type", "") or "--"
                mark = fix
                # A change of state is the whole point of a walked test, so it
                # is called out rather than left to be spotted in a column.
                flag = "  <<< " + mark if fix != last_type and last_type is not None else ""
                last_type = fix
                print(f"{mm:02d}:{ss:02d}  n={n:<4} {mark:12} "
                      f"acc {row.get('acc_e_m','--'):>6}/{row.get('acc_n_m','--'):<6} "
                      f"sat {row.get('sats','--'):>2}  cno {row.get('cno_med_hi','--'):>2}  "
                      f"fix% {row.get('pct_fix','--'):>3}  drops {row.get('drops_acc','--'):>2}"
                      f"  lost {n - ngood}{flag}",
                      flush=True)
            elif n % 15 == 0:
                print(f"{row['iso']}  n={n} ok={ngood}  {row.get('type','')}  "
                      f"acc={row.get('acc_e_m','')}/{row.get('acc_n_m','')}  "
                      f"cno_hi={row.get('cno_med_hi','')}  drops={row.get('drops_acc','')}",
                      flush=True)
            time.sleep(max(0, args.interval - (time.time() - start)))


if __name__ == "__main__":
    sys.exit(main())
