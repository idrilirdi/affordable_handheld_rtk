#!/usr/bin/env python3
"""Plot a recorded walk and correlate fix quality against sky quality.

The question a walked test exists to answer is whether losing the fix follows
the sky or something else. Position and signal are recorded together once a
second, so they can simply be laid side by side.
"""

import argparse
import csv
import math
import statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

STATE_COLOR = {"FIXED": "#2ca02c", "FLOAT": "#1f77b4",
                "DGPS": "#ff7f0e", "SINGLE": "#d62728", "NONE": "#888888"}


def num(row, key):
    try:
        return float(row[key])
    except (ValueError, KeyError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvfile")
    ap.add_argument("--out", default="walk.png")
    ap.add_argument("--powerloss", type=float, default=None,
                    help="seconds after which the board lost power; excluded")
    ap.add_argument("--anonymize", action="store_true",
                    help="rotate the track onto its long axis and drop compass "
                         "directions, so a published plot cannot be matched to "
                         "a building footprint")
    args = ap.parse_args()

    rows = [r for r in csv.DictReader(open(args.csvfile)) if r["ok"] == "1"]
    if args.powerloss is not None:
        live = [r for r in rows if num(r, "t") < args.powerloss]
    else:
        # After power is lost the receiver reports no satellites but the last
        # position lingers in the panel, so those rows would draw a phantom
        # point exactly where the track ends. Drop them.
        live = [r for r in rows if (num(r, "sats") or 0) > 0]

    lat0 = st.median([num(r, "lat") for r in live])
    lon0 = st.median([num(r, "lon") for r in live])
    kx = 111320.0 * math.cos(math.radians(lat0))
    for r in live:
        r["_e"] = (num(r, "lon") - lon0) * kx
        r["_n"] = (num(r, "lat") - lat0) * 111320.0
    xlabel, ylabel = "east (m)", "north (m)"
    if args.anonymize:
        # Principal axis of the track, from the second moments.
        see = sum(r["_e"] ** 2 for r in live)
        snn = sum(r["_n"] ** 2 for r in live)
        sen = sum(r["_e"] * r["_n"] for r in live)
        th = 0.5 * math.atan2(2 * sen, see - snn)
        c, s_ = math.cos(-th), math.sin(-th)
        for r in live:
            e, n = r["_e"], r["_n"]
            r["_e"], r["_n"] = e * c - n * s_, e * s_ + n * c
        xlabel, ylabel = "x (m)", "y (m)"

    fig, ax = plt.subplots(2, 2, figsize=(14, 11))
    fig.suptitle("RTK walked circuit - fix quality against sky quality", fontsize=13)

    # Where it was, and what it was doing there.
    a = ax[0][0]
    for state, color in STATE_COLOR.items():
        pts = [(r["_e"], r["_n"]) for r in live if r["type"] == state]
        if pts:
            a.scatter(*zip(*pts), s=14, c=color, label=f"{state} ({len(pts)} s)")
    a.plot([r["_e"] for r in live], [r["_n"] for r in live], "-", lw=0.4, c="#bbb", zorder=0)
    a.scatter([live[0]["_e"]], [live[0]["_n"]], marker="*", s=220, c="k", zorder=5, label="start")
    a.set_aspect("equal"); a.grid(alpha=.3); a.legend(fontsize=8)
    a.set_title("track by fix state"); a.set_xlabel(xlabel); a.set_ylabel(ylabel)

    # The same track, colored by what the antenna could hear.
    a = ax[0][1]
    cno = [num(r, "cno_med_hi") for r in live]
    sc = a.scatter([r["_e"] for r in live], [r["_n"] for r in live], s=16, c=cno,
                   cmap="viridis")
    fig.colorbar(sc, ax=a, label="median C/N0 above 30 deg (dB-Hz)")
    a.set_aspect("equal"); a.grid(alpha=.3)
    a.set_title("track by sky quality"); a.set_xlabel(xlabel); a.set_ylabel(ylabel)

    # Signal and state through time, so cause and effect line up.
    a = ax[1][0]
    t = [num(r, "t") for r in live]
    a.plot(t, cno, lw=1.2, c="#333", label="C/N0 >30 deg")
    a.plot(t, [num(r, "elev_60_90") for r in live], lw=1, c="#2ca02c", alpha=.7,
           label="C/N0 overhead (>60 deg)")
    for r in live:
        a.axvspan(num(r, "t"), num(r, "t") + 1, color=STATE_COLOR.get(r["type"], "#888"),
                  alpha=.13, lw=0)
    a.set_xlabel("seconds"); a.set_ylabel("dB-Hz"); a.grid(alpha=.3); a.legend(fontsize=8)
    a.set_title("signal through time (band color = fix state)")

    # What it cost in accuracy, and how old the corrections were.
    a = ax[1][1]
    a.semilogy(t, [max(num(r, "acc_e_m") or 1e-4, 1e-4) for r in live], lw=1.1,
               c="#d62728", label="east accuracy (m)")
    a.semilogy(t, [max(num(r, "corr_age_s") or .1, .1) for r in live], lw=1,
               c="#1f77b4", alpha=.8, label="correction age (s)")
    a.set_xlabel("seconds"); a.grid(alpha=.3, which="both"); a.legend(fontsize=8)
    a.set_title("accuracy and correction age")

    fig.tight_layout()
    fig.savefig(args.out, dpi=130)
    print(f"wrote {args.out}")

    # Does the fix follow the sky? Fix rate per C/N0 bin answers it directly.
    print("\n  fix rate by sky quality")
    bins = [(0, 40), (40, 44), (44, 47), (47, 60)]
    for lo, hi in bins:
        sel = [r for r in live if lo <= (num(r, "cno_med_hi") or -1) < hi]
        if not sel:
            continue
        fixed = sum(1 for r in sel if r["type"] == "FIXED")
        print(f"    C/N0 {lo}-{hi:<3} dB-Hz   {fixed:4d}/{len(sel):<4d} fixed  "
              f"({100 * fixed // len(sel):3d}%)")

    fixed = [num(r, "cno_med_hi") for r in live if r["type"] == "FIXED"]
    notfixed = [num(r, "cno_med_hi") for r in live if r["type"] not in ("FIXED", None)]
    if fixed and notfixed:
        print(f"\n    median C/N0 when fixed      {st.median(fixed):.0f} dB-Hz  ({len(fixed)} s)")
        print(f"    median C/N0 when not fixed  {st.median(notfixed):.0f} dB-Hz  ({len(notfixed)} s)")


if __name__ == "__main__":
    main()
