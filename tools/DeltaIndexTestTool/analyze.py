#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
"""
analyze.py - Baseline refresh timing optimizer for winget delta indexes.

Reads a results.csv produced by DeltaIndexTestTool and models the relative compressed
egress across multiple user updates under different baseline refresh schedules.
Sweeps all candidate refresh periods and recommends the one that minimizes the
expected compressed egress given an assumed user staleness distribution.

All output is relative (percentages); absolute egress volume is never required.

COST MODEL SUMMARY
------------------
The distribution W[D] represents the fraction of *download events* from clients
that were D days stale at the time of download (not the fraction of users).
Telemetry naturally produces this view since it counts downloads, not users.
Frequency is therefore already embedded in W[D] — no additional weighting by 1/D.

For a refresh period of P checkpoints (P * interval_days days):

  cycle_avg_delta     = average of DeltaOrig[0..P-1]
                        (expected delta size at a random moment in the cycle)
  weighted_p_baseline = sum over D of W[D] * min(D, period_days) / period_days
                        (expected fraction of downloads that need a new baseline)
                        For "new_client" buckets: p_needs_baseline = 1.0 always.
  cost_per_download   = cycle_avg_delta + weighted_p_baseline * baseline_size

The status quo (no deltas at all) costs `baseline_size` per download, so the
predicted traffic reduction for a schedule is simply:

  reduction = 1 - cost_per_download / baseline_size

BASELINE SIZE
-------------
`baseline_size` is the compressed size of the index a client downloads when it
must take a fresh baseline. By default it is taken from the *last* checkpoint in
the CSV (the most current measurement). Because the tool-built index may not match
what production actually serves, supply the real value with --baseline-mb.

Delta sizes are NOT scaled along with --baseline-mb: delta growth is driven by the
repository change rate, which holds relatively steady and is measured directly by
the tool. Only the baseline-download side of the model responds to --baseline-mb.

Key approximation: DeltaOrig growth from baseline 0 is used as a proxy for delta
growth from any hypothetical baseline (reasonable when repository growth is steady).

DISTRIBUTION FORMAT
-------------------
Buckets can be either:
  { "days": N, "weight": W }         -- clients N days stale at update time
  { "new_client": true, "weight": W } -- net-new clients (no prior index; always
                                         pay full baseline cost)

Built-in presets: daily_heavy, weekly, monthly

Telemetry-derived JSON example:
  {
    "description": "Telemetry-derived YYYY-MM-DD",
    "buckets": [
      { "days": 1,  "weight": 0.30 },
      { "days": 7,  "weight": 0.50 },
      { "days": 30, "weight": 0.15 },
      { "new_client": true, "weight": 0.05 }
    ]
  }

Usage:
    python analyze.py --csv results.csv --distribution weekly
    python analyze.py --csv results.csv --distribution distribution.json
    python analyze.py --csv results.csv --distribution weekly --baseline-mb 12.4 --output-chart chart.png
"""

import argparse
import csv
import json
import sys
import textwrap
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Built-in staleness distribution presets
# ---------------------------------------------------------------------------
# Each preset is a PMF over "days since last update" at the moment a user
# triggers an update. Weights must sum to 1.0.
# Replace buckets with telemetry-derived data when available — no other code
# changes are needed; supply a JSON file matching this format via --distribution.

PRESETS = {
    "daily_heavy": {
        "description": "Assumption (download events): heavy automated/CI usage — many daily downloads",
        "buckets": [
            {"days": 1,  "weight": 0.80},
            {"days": 7,  "weight": 0.15},
            {"days": 30, "weight": 0.04},
            {"days": 90, "weight": 0.01},
        ],
    },
    "weekly": {
        "description": "Assumption (download events): typical developer tool — weekly updaters dominate downloads",
        "buckets": [
            {"days": 1,  "weight": 0.30},
            {"days": 7,  "weight": 0.50},
            {"days": 30, "weight": 0.15},
            {"days": 90, "weight": 0.05},
        ],
    },
    "monthly": {
        "description": "Assumption (download events): infrequent updaters dominate downloads",
        "buckets": [
            {"days": 1,  "weight": 0.05},
            {"days": 7,  "weight": 0.25},
            {"days": 30, "weight": 0.45},
            {"days": 90, "weight": 0.25},
        ],
    },
}


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_distribution(dist_arg):
    """Return a distribution dict from a preset name or a JSON file path."""
    if dist_arg in PRESETS:
        return PRESETS[dist_arg]
    p = Path(dist_arg)
    if not p.exists():
        raise FileNotFoundError(f"Distribution file not found: {dist_arg}")
    with p.open(encoding="utf-8") as f:
        return json.load(f)


def load_csv(csv_path):
    """
    Load checkpoints from results.csv.  Returns a list of dicts (one per row).
    If compressed-size columns are absent (produced before that feature was added),
    falls back to the uncompressed values so older CSVs remain usable.
    """
    rows = []
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            r = {}
            r["Date"] = datetime.strptime(row["Date"].strip(), "%Y-%m-%d")
            r["CommitSha"] = row.get("CommitSha", "").strip()
            for col in ("FullIndexMB", "DeltaPrevMB", "DeltaOrigMB"):
                r[col] = float(row.get(col) or 0)
            # Compressed columns — fall back gracefully to uncompressed values.
            r["FullIndexCompressedMB"] = float(
                row.get("FullIndexCompressedMB") or r["FullIndexMB"])
            r["DeltaPrevCompressedMB"] = float(
                row.get("DeltaPrevCompressedMB") or r["DeltaPrevMB"])
            r["DeltaOrigCompressedMB"] = float(
                row.get("DeltaOrigCompressedMB") or r["DeltaOrigMB"])
            rows.append(r)
    return rows


# ---------------------------------------------------------------------------
# Cost model
# ---------------------------------------------------------------------------

def compute_interval_days(checkpoints):
    """Estimate the average days between consecutive checkpoints."""
    if len(checkpoints) < 2:
        return 7
    span = (checkpoints[-1]["Date"] - checkpoints[0]["Date"]).days
    return span / (len(checkpoints) - 1)


def normalize_buckets(buckets):
    """Return a copy of buckets with weights normalized to sum to 1.0.

    Supports both regular staleness buckets {"days": N, "weight": W} and
    net-new client buckets {"new_client": true, "weight": W}.
    """
    total = sum(b["weight"] for b in buckets)
    if abs(total - 1.0) > 0.01:
        print(f"Warning: distribution weights sum to {total:.3f}, normalizing to 1.0",
              file=sys.stderr)
    result = []
    for b in buckets:
        normalized = {"weight": b["weight"] / total}
        if b.get("new_client"):
            normalized["new_client"] = True
        else:
            normalized["days"] = b["days"]
        result.append(normalized)
    return result


def simulate_schedule(checkpoints, period, buckets, interval_days, baseline_size):
    """
    Simulate a periodic baseline refresh every `period` checkpoints and return
    the expected compressed egress cost per download event.

    The distribution buckets represent fractions of *download events* by client
    staleness (D days since last update).  Since frequency is already embedded
    in the weights, no additional per-user-type frequency scaling is applied.

    For a given period P:

      cycle_avg_delta     = mean(DeltaOrig[0], ..., DeltaOrig[P-1])
                            Expected delta size at a uniformly random moment in
                            the baseline lifecycle.

      weighted_p_baseline = sum over D of: W[D] * min(D, period_days) / period_days
                            Expected fraction of download events where the client's
                            index predates the current baseline, requiring a full
                            baseline download.

      cost_per_download   = cycle_avg_delta + weighted_p_baseline * baseline_size

    `baseline_size` is the compressed MB a client pays for a fresh baseline; see
    the module docstring for how it is chosen.  Delta sizes come from measured
    repository change rate and are deliberately independent of it.

    Returns cost_per_download (MB).  Only ratios between schedules (and against
    `baseline_size`, the status quo) are meaningful.
    """
    delta_curve  = [cp["DeltaOrigCompressedMB"] for cp in checkpoints]
    period_days  = period * interval_days

    cycle_deltas    = [delta_curve[min(a, len(delta_curve) - 1)] for a in range(period)]
    cycle_avg_delta = sum(cycle_deltas) / period

    weighted_p_baseline = sum(
        b["weight"] * (1.0 if b.get("new_client") else min(b["days"], period_days) / period_days)
        for b in buckets
    )

    return cycle_avg_delta + weighted_p_baseline * baseline_size


def find_crossover(checkpoints, threshold, baseline_size):
    """
    Return the index of the first checkpoint where
    DeltaOrigCompressedMB / baseline_size >= threshold, or None.
    """
    if baseline_size <= 0:
        return None
    for i, cp in enumerate(checkpoints):
        if cp["DeltaOrigCompressedMB"] / baseline_size >= threshold:
            return i
    return None


# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------

def fmt_mb(mb):
    """Format a MB value as MB or GB depending on magnitude."""
    if mb >= 1024:
        return f"{mb / 1024:.2f} GB"
    return f"{mb:.1f} MB"


def fmt_days(days):
    """Format a number of days as 'd' or 'wk' shorthand."""
    if days % 7 == 0 and days >= 7:
        return f"~{int(days // 7)}wk"
    return f"~{days:.0f}d"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # Windows consoles often default to cp1252, which cannot encode the arrows and
    # dashes used below.  Prefer UTF-8, and degrade to replacement chars if unavailable.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(
        description="Optimize baseline refresh timing for winget delta indexes.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Built-in distribution presets: " + ", ".join(PRESETS),
    )
    parser.add_argument("--csv", required=True,
                        help="Path to results.csv from DeltaIndexTestTool")
    parser.add_argument("--distribution", required=True,
                        help="Staleness distribution: preset name or path to JSON file. "
                             f"Presets: {', '.join(PRESETS)}")
    parser.add_argument("--baseline-mb", type=float, default=None, dest="baseline_mb",
                        help="Compressed size (MB) of the baseline index clients actually download. "
                             "Defaults to the last checkpoint's FullIndexCompressedMB from the CSV. "
                             "Delta sizes are not scaled by this value.")
    parser.add_argument("--output-chart", default=None, dest="output_chart",
                        help="Optional path to save a chart image (e.g. chart.png). Requires matplotlib.")
    args = parser.parse_args()

    # --- Load inputs --------------------------------------------------------
    checkpoints = load_csv(args.csv)
    if not checkpoints:
        print("Error: no checkpoints in CSV.", file=sys.stderr)
        sys.exit(1)

    dist = load_distribution(args.distribution)
    buckets = normalize_buckets(dist["buckets"])
    description = dist.get("description", args.distribution)

    interval_days = compute_interval_days(checkpoints)
    n = len(checkpoints)

    # The size a client pays for a fresh baseline.  The CSV's last checkpoint is
    # the most current measurement, but production may serve a different index;
    # --baseline-mb lets telemetry-observed reality drive the model instead.
    csv_baseline_size = checkpoints[-1]["FullIndexCompressedMB"]
    baseline_size = args.baseline_mb if args.baseline_mb is not None else csv_baseline_size
    if baseline_size <= 0:
        print("Error: baseline size is zero; supply --baseline-mb.", file=sys.stderr)
        sys.exit(1)

    # --- Header -------------------------------------------------------------
    print(f"\n{'='*68}")
    print(f"  Baseline Timing Analysis")
    print(f"{'='*68}")
    print(f"  CSV:          {args.csv}")
    print(f"  Checkpoints:  {n}  "
          f"({checkpoints[0]['Date']:%Y-%m-%d} to {checkpoints[-1]['Date']:%Y-%m-%d})")
    print(f"  Avg interval: {interval_days:.1f} days  "
          f"(total span: {(checkpoints[-1]['Date'] - checkpoints[0]['Date']).days} days)")
    print(f"  Distribution: {description}")
    if args.baseline_mb is not None:
        print(f"  Baseline:     {fmt_mb(baseline_size)}  (supplied; "
              f"CSV measured {fmt_mb(csv_baseline_size)})")
    else:
        print(f"  Baseline:     {fmt_mb(baseline_size)}  (last checkpoint in CSV)")
    print()

    # --- Delta growth crossovers -------------------------------------------
    c50  = find_crossover(checkpoints, 0.50, baseline_size)
    c100 = find_crossover(checkpoints, 1.00, baseline_size)
    print("  Delta (compressed) growth from baseline:")
    if c50 is not None:
        print(f"    Exceeds 50%  of baseline at checkpoint {c50:>3} "
              f"({fmt_days(c50 * interval_days)})")
    else:
        print("    Never exceeds 50% of baseline within measured period")
    if c100 is not None:
        print(f"    Exceeds 100% of baseline at checkpoint {c100:>3} "
              f"({fmt_days(c100 * interval_days)})")
    else:
        print("    Never exceeds 100% of baseline within measured period")
    print()

    # --- Simulate all periods -----------------------------------------------
    results = []
    for period in range(1, n + 1):
        cost_per_dl = simulate_schedule(checkpoints, period, buckets, interval_days, baseline_size)
        results.append({
            "period":      period,
            "period_days": period * interval_days,
            "total_mb":    cost_per_dl,   # MB per download event
        })

    optimal       = min(results, key=lambda r: r["total_mb"])
    never_refresh = results[-1]   # period == n

    # Status quo: no deltas at all, every download fetches the whole index.
    status_quo_mb = baseline_size

    # --- Determine which periods to print in the table ----------------------
    # Always show: period 1, optimal, and period n.
    # Also show a sample of ~15-20 evenly-spaced periods in between.
    show = {1, optimal["period"], n}
    step = max(1, n // 18)
    for p in range(step, n, step):
        show.add(p)

    # --- Print table --------------------------------------------------------
    print(f"--- Schedule Comparison (expected compressed egress per download event) ---")
    print(f"    (lower = cheaper per update on average; % is reduction vs. today's "
          f"full-index-every-time behavior)")
    print(f"  {'Period':>6}  {'Interval':>9}  {'MB/Download':>13}  {'vs Status Quo':>14}")
    print(f"  {'-'*6}  {'-'*9}  {'-'*13}  {'-'*14}")

    print(f"  {'-':>6}  {'-':>9}  {status_quo_mb:>11.2f} MB  "
          f"{'baseline':>14}  (status quo: full index every download)")

    for r in sorted(results, key=lambda r: r["period"]):
        if r["period"] not in show:
            continue
        tag = ""
        if r["period"] == n:
            tag = "  (never refresh)"
        elif r["period"] == optimal["period"]:
            tag = "  ← optimal"

        reduction = 100.0 * (1.0 - r["total_mb"] / status_quo_mb)
        print(f"  {r['period']:>6}  {fmt_days(r['period_days']):>9}  "
              f"{r['total_mb']:>11.2f} MB  {reduction:>13.1f}%{tag}")

    print()

    # --- Recommendation summary ---------------------------------------------
    reduction_vs_status_quo = 100.0 * (1.0 - optimal["total_mb"] / status_quo_mb)
    savings_vs_never = (100.0 * (1.0 - optimal["total_mb"] / never_refresh["total_mb"])
                        if never_refresh["total_mb"] > 0 else 0.0)

    print(f"  Recommendation: refresh baseline every {optimal['period']} checkpoint(s) "
          f"({fmt_days(optimal['period_days'])})")
    print(f"    {optimal['total_mb']:.2f} MB / download event")
    print(f"    Predicted outbound traffic reduction: {reduction_vs_status_quo:.1f}%")
    if optimal["period"] < n:
        print(f"    vs never-refresh:  {savings_vs_never:+.1f}%")
    print()

    # --- Chart --------------------------------------------------------------
    if args.output_chart:
        _write_chart(args.output_chart, checkpoints, optimal, interval_days,
                     description, baseline_size)


def _mb_precision(ticks):
    """Decimal places needed so adjacent axis ticks render as distinct labels."""
    spacing = min((abs(b - a) for a, b in zip(ticks, ticks[1:])), default=1.0)
    if spacing >= 1.0:
        return 0
    if spacing >= 0.1:
        return 1
    return 2


def _write_chart(path, checkpoints, optimal, interval_days, description, baseline_size):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.ticker as ticker
    except ImportError:
        print("  Note: matplotlib not available; skipping chart. "
              "Install with: pip install matplotlib", file=sys.stderr)
        return

    dates      = [cp["Date"] for cp in checkpoints]
    full_vals  = [cp["FullIndexCompressedMB"] for cp in checkpoints]
    delta_vals = [cp["DeltaOrigCompressedMB"] for cp in checkpoints]
    n = len(checkpoints)

    fig, ax = plt.subplots(figsize=(13, 6))

    ax.plot(dates, full_vals,  label="Full index (compressed)",        color="#C0392B", linewidth=2)
    ax.plot(dates, delta_vals, label="Delta from baseline (compressed)", color="#27AE60", linewidth=2)
    ax.axhline(y=baseline_size, color="#7F8C8D", linestyle=":", linewidth=1.5,
               label=f"Baseline download size ({baseline_size:.1f} MB)")

    # Vertical lines at recommended baseline refresh points
    opt_period = optimal["period"]
    baseline_indices = list(range(opt_period, n, opt_period))
    first_line = True
    for bi in baseline_indices:
        lbl = (f"Recommended baseline ({fmt_days(opt_period * interval_days)} period)"
               if first_line else None)
        ax.axvline(x=checkpoints[bi]["Date"], color="#2980B9",
                   linestyle="--", linewidth=0.9, alpha=0.7, label=lbl)
        first_line = False

    # Shade the area between the curves for visual clarity
    ax.fill_between(dates, delta_vals, full_vals,
                    where=[d < f for d, f in zip(delta_vals, full_vals)],
                    alpha=0.07, color="#27AE60", label="Potential savings region")

    ax.set_xlabel("Date")
    ax.set_ylabel("Compressed Size (MB)")
    ax.set_title("Delta Index Baseline Timing Analysis\n"
                 + "\n".join(textwrap.wrap(f"Distribution: {description}", 110)),
                 fontsize=11)
    ax.legend(loc="upper left")
    # Whole-MB labels collapse into duplicates when the plotted range is only a few MB,
    # so pick the precision from the actual tick spacing.
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(
        lambda v, _: f"{v:.{_mb_precision(ax.get_yticks())}f} MB"))
    fig.autofmt_xdate()
    plt.tight_layout()
    plt.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Chart saved to: {path}")


if __name__ == "__main__":
    main()
