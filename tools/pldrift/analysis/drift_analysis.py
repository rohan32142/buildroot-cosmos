#!/usr/bin/env python3
"""
Drift analysis for pldrift CSVs (and optional ptp4l logs).

Usage:
  python drift_analysis.py run.csv --ref mono_raw
  python drift_analysis.py run.csv --ref realtime --ptp ptp.log
  python drift_analysis.py run.csv --fnom 200e6 --window 600 --out figs/

Reports the PL tick-rate error in ppm relative to the chosen reference:
  ppm = (slope - 1) * 1e6, where slope = d(PL seconds) / d(reference seconds)
A positive value means the PL counter runs fast relative to the reference.

This is the tick-rate error only. The 1.699 ppm phase-increment deficit in
the sample rate is a separate design term and is not included here.
"""
import argparse
import os
import re
import sys

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

TWO32 = 1 << 32


def load(path):
    df = pd.read_csv(path, comment="#")
    need = {"mono_raw_ns", "realtime_ns", "pl64", "pl_lo", "scan_ns"}
    missing = need - set(df.columns)
    if missing:
        sys.exit(f"missing columns: {missing}")
    return df


def pl_ticks(df):
    """Return unwrapped PL ticks as float64 relative to the first sample,
    and a note on which path was used."""
    if (df["pl64"].astype("uint64").to_numpy() >> np.uint64(32)).any():
        ticks = df["pl64"].astype("int64").to_numpy()
        note = "64-bit counter (upper word seen non-zero)"
    else:
        lo = df["pl_lo"].astype("int64").to_numpy()
        d = np.diff(lo) % TWO32          # modular step, valid if interval < wrap period
        ticks = np.concatenate([[0], np.cumsum(d)]) + lo[0]
        note = "32-bit unwrap of low word (upper word always zero)"
    return (ticks - ticks[0]).astype(np.float64), note


def check_resets(ticks):
    d = np.diff(ticks)
    bad = np.where(d <= 0)[0]
    return bad


def fit(x, y):
    """Least-squares y = a + b x. Returns b, a, residuals, slope std error."""
    A = np.vstack([np.ones_like(x), x]).T
    coef, *_ = np.linalg.lstsq(A, y, rcond=None)
    a, b = coef
    r = y - (a + b * x)
    dof = max(len(x) - 2, 1)
    s2 = (r @ r) / dof
    se_b = np.sqrt(s2 / np.sum((x - x.mean()) ** 2))
    return b, a, r, se_b


def windowed_ppm(x, y, window_s):
    out_t, out_ppm = [], []
    start = x[0]
    while start + window_s <= x[-1]:
        m = (x >= start) & (x < start + window_s)
        if m.sum() > 10:
            b, *_ = fit(x[m], y[m])
            out_t.append(start + window_s / 2)
            out_ppm.append((b - 1) * 1e6)
        start += window_s / 2
    return np.array(out_t), np.array(out_ppm)


def adev_from_phase(phase_s, tau0, max_points=30):
    """Overlapping Allan deviation from time-error samples (seconds).
    Assumes uniform sampling at tau0."""
    N = len(phase_s)
    ns = np.unique(np.logspace(0, np.log10(max((N - 1) // 3, 1)), max_points).astype(int))
    taus, devs = [], []
    for n in ns:
        if N - 2 * n < 2:
            break
        d = phase_s[2 * n:] - 2 * phase_s[n:-n] + phase_s[:-2 * n]
        tau = n * tau0
        taus.append(tau)
        devs.append(np.sqrt(np.mean(d ** 2) / (2 * tau ** 2)))
    return np.array(taus), np.array(devs)


PTP_RE = re.compile(
    r"ptp4l\[(?P<t>[\d.]+)\]: master offset\s+(?P<off>-?\d+)\s+s(?P<st>\d)"
    r"\s+freq\s+(?P<freq>[+-]?\d+)\s+path delay\s+(?P<dly>-?\d+)")


def load_ptp(path):
    rows = []
    with open(path, errors="replace") as f:
        for line in f:
            m = PTP_RE.search(line)
            if m:
                rows.append((float(m["t"]), int(m["off"]), int(m["st"]),
                             int(m["freq"]), int(m["dly"])))
    return pd.DataFrame(rows, columns=["t", "offset_ns", "state", "freq_ppb", "delay_ns"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--ref", choices=["mono_raw", "realtime"], default="mono_raw")
    ap.add_argument("--fnom", type=float, default=200e6,
                    help="nominal PL counter frequency in Hz (confirm from design_top.vhd)")
    ap.add_argument("--window", type=float, default=600, help="windowed fit length, s")
    ap.add_argument("--reject", type=float, default=5.0,
                    help="drop rows with scan_ns > this multiple of the median")
    ap.add_argument("--skip", type=float, default=0.0,
                    help="discard the first N seconds (e.g. PTP convergence)")
    ap.add_argument("--ptp", help="ptp4l log captured with -m")
    ap.add_argument("--out", default=".")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    df = load(args.csv)
    n0 = len(df)
    med = df["scan_ns"].median()
    df = df[df["scan_ns"] <= args.reject * med].reset_index(drop=True)

    ticks, note = pl_ticks(df)
    bad = check_resets(ticks)
    if len(bad):
        print(f"WARNING: {len(bad)} non-increasing steps (counter reset?). "
              f"First at row {bad[0]}. Split the file there and analyse each part.")

    ref_col = "mono_raw_ns" if args.ref == "mono_raw" else "realtime_ns"
    ref_ns = df[ref_col].astype("int64").to_numpy()
    x = (ref_ns - ref_ns[0]) / 1e9                 # reference seconds
    y = ticks / args.fnom                          # PL seconds at nominal rate

    keep = x >= args.skip
    x, y = x[keep], y[keep]
    temp = df["temp_mc"].to_numpy()[keep] if "temp_mc" in df else None

    b, a, r, se_b = fit(x, y)
    ppm = (b - 1) * 1e6
    dur = x[-1] - x[0]

    print(f"file:            {args.csv}")
    print(f"rows:            {len(x)} used / {n0} total (scan_ns filter at {args.reject}x median {med:.0f} ns)")
    print(f"counter:         {note}")
    print(f"reference:       {ref_col}")
    print(f"duration:        {dur:.0f} s ({dur/3600:.2f} h)")
    print(f"PL rate error:   {ppm:+.4f} ppm  (+/- {se_b*1e6:.4f} ppm 1-sigma, white-noise assumption)")
    print(f"implied f_PL:    {args.fnom * b:.3f} Hz")
    print(f"residual RMS:    {np.std(r)*1e6:.2f} us, peak-to-peak {np.ptp(r)*1e6:.2f} us")
    print("note: the slope error is optimistic if residuals are correlated (wander); "
          "use the windowed and ADEV plots to judge.")

    # Residuals
    fig, ax = plt.subplots(figsize=(9, 4))
    ax.plot(x / 60, r * 1e6, lw=0.7)
    ax.set_xlabel("time (min)"); ax.set_ylabel("residual (us)")
    ax.set_title(f"PL vs {args.ref}: residual after linear fit ({ppm:+.3f} ppm)")
    ax.grid(True, alpha=0.3); fig.tight_layout()
    fig.savefig(os.path.join(args.out, "residuals.png"), dpi=150)

    # Windowed ppm and temperature
    wt, wp = windowed_ppm(x, y, args.window)
    if len(wt):
        fig, ax = plt.subplots(figsize=(9, 4))
        ax.plot(wt / 60, wp, "o-", ms=3, label="windowed ppm")
        ax.set_xlabel("time (min)"); ax.set_ylabel("ppm")
        ax.grid(True, alpha=0.3)
        if temp is not None and (temp > -1_000_000).any():
            ax2 = ax.twinx()
            tm = temp > -1_000_000
            ax2.plot(x[tm] / 60, temp[tm] / 1000, "r", lw=0.7, alpha=0.6, label="die temp")
            ax2.set_ylabel("die temperature (C)", color="r")
        ax.set_title(f"Windowed rate error ({args.window:.0f} s windows, 50% overlap)")
        fig.tight_layout()
        fig.savefig(os.path.join(args.out, "windowed_ppm.png"), dpi=150)

    # ADEV of time error (PL minus reference, linear trend removed)
    tau0 = np.median(np.diff(x))
    taus, devs = adev_from_phase(r, tau0)
    if len(taus):
        fig, ax = plt.subplots(figsize=(6, 4.5))
        ax.loglog(taus, devs, "o-", ms=3)
        ax.set_xlabel("tau (s)"); ax.set_ylabel("overlapping Allan deviation")
        ax.set_title("PL vs reference stability")
        ax.grid(True, which="both", alpha=0.3); fig.tight_layout()
        fig.savefig(os.path.join(args.out, "adev.png"), dpi=150)

    # PTP log
    if args.ptp:
        p = load_ptp(args.ptp)
        locked = p[p["state"] == 2]
        if locked.empty:
            print("ptp4l: no s2 (locked) lines found")
        else:
            lt = locked[locked["t"] >= locked["t"].iloc[0] + args.skip]
            f_ppb = lt["freq_ppb"].median()
            # ptp4l 'freq' is the correction applied to the local clock, so the
            # free-running CPU clock error is approximately its negative.
            cpu_ppm = -f_ppb / 1000
            print(f"ptp4l locked:    {len(lt)} lines, offset RMS {lt['offset_ns'].std():.0f} ns, "
                  f"median path delay {lt['delay_ns'].median():.0f} ns")
            print(f"ptp4l freq:      median {f_ppb:+.0f} ppb -> CPU clock error vs GM ~ {cpu_ppm:+.3f} ppm")
            if args.ref == "mono_raw":
                print(f"chained PL vs GM ~ {ppm + cpu_ppm:+.3f} ppm "
                      "(cross-check against a --ref realtime run of the same file; "
                      "if the two disagree in sign, the freq sign convention is the first suspect)")
            fig, axs = plt.subplots(2, 1, figsize=(9, 6), sharex=True)
            axs[0].plot(p["t"] - p["t"].iloc[0], p["offset_ns"] / 1000, lw=0.7)
            axs[0].set_ylabel("master offset (us)"); axs[0].grid(True, alpha=0.3)
            axs[1].plot(p["t"] - p["t"].iloc[0], p["freq_ppb"], lw=0.7)
            axs[1].set_ylabel("freq (ppb)"); axs[1].set_xlabel("ptp4l time (s)")
            axs[1].grid(True, alpha=0.3); fig.tight_layout()
            fig.savefig(os.path.join(args.out, "ptp4l.png"), dpi=150)

    print(f"figures written to {os.path.abspath(args.out)}")


if __name__ == "__main__":
    main()
