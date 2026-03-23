#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
"""
analyze.py - Baseline refresh timing optimizer for winget delta indexes.

Reads a results.csv produced by DeltaIndexTestTool and models the total compressed
egress across multiple user updates under different baseline refresh schedules.
Sweeps all candidate refresh periods and recommends the one that minimizes total
expected compressed egress given an assumed user staleness distribution.

COST MODEL SUMMARY
------------------
For a refresh period of P checkpoints, the baseline age cycles 0..P-1 in steady state.
For each user type (D days between updates, population weight W):

  - Avg delta cost per download = average of DeltaOrig[0..P-1]
    (user downloads current delta-from-baseline regardless of personal staleness)
  - P(needs new baseline) = min(D, P*interval_days) / (P*interval_days)
    (fraction of time the user's last update predates the current baseline)
  - Downloads per user over window = window_days / D
    (daily users contribute 7x more download events than weekly users)

Total egress = sum over buckets of: W * (window/D) * (avg_delta + p_baseline * full_avg)

Key approximation: DeltaOrig growth from baseline 0 is used as a proxy for delta
growth from any hypothetical baseline (reasonable when repository growth is steady).

Usage:
    python analyze.py --csv results.csv --distribution weekly
    python analyze.py --csv results.csv --distribution distribution.json
    python analyze.py --csv results.csv --distribution weekly --egress-cost-per-gb 0.087 --output-chart chart.png
"""

import argparse
import csv
import json
import sys
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
        "description": "Assumption: heavy daily-update user base (CI systems, power users)",
        "buckets": [
            {"days": 1,  "weight": 0.40},
            {"days": 7,  "weight": 0.35},
            {"days": 30, "weight": 0.20},
            {"days": 90, "weight": 0.05},
        ],
    },
    "weekly": {
        "description": "Assumption: mostly weekly updaters (typical developer)",
        "buckets": [
            {"days": 1,  "weight": 0.10},
            {"days": 7,  "weight": 0.50},
            {"days": 30, "weight": 0.30},
            {"days": 90, "weight": 0.10},
        ],
    },
    "monthly": {
        "description": "Assumption: mostly monthly or infrequent updaters",
        "buckets": [
            {"days": 1,  "weight": 0.05},
            {"days": 7,  "weight": 0.20},
            {"days": 30, "weight": 0.45},
            {"days": 90, "weight": 0.30},
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
    """Return a copy of buckets with weights normalized to sum to 1.0."""
    total = sum(b["weight"] for b in buckets)
    if abs(total - 1.0) > 0.01:
        print(f"Warning: distribution weights sum to {total:.3f}, normalizing to 1.0",
              file=sys.stderr)
    return [{"days": b["days"], "weight": b["weight"] / total} for b in buckets]


def simulate_schedule(checkpoints, period, buckets, interval_days):
    """
    Simulate a periodic baseline refresh every `period` checkpoints and return
    the total expected compressed egress in MB across the measurement window.

    MODEL
    -----
    For a given refresh period P (checkpoints), baseline age cycles 0..P-1.
    When a user updates at a moment when the baseline is `a` checkpoints old:

      - They always download the current delta:  DeltaOrig[a]
      - If they are MORE stale than the baseline (their last update was before
        the baseline was published), they additionally download the full index.

    The probability that a user of type D (days between updates) needs the
    baseline at a random update moment equals the fraction of the baseline
    cycle during which the baseline is newer than the user's last update:

        p_needs_baseline(D) = min(D, period_days) / period_days

    This is derived from: the baseline was published `a` days ago; user needs
    it if a < D; in steady state `a` is uniform over [0, period_days), so the
    probability is min(D, period_days) / period_days.

    The average delta cost across a complete baseline cycle is the mean of
    DeltaOrig[0..P-1], since in steady state the baseline is equally likely
    to be at any age.

    FREQUENCY CORRECTION
    --------------------
    A user who updates every D days generates window_days/D download events
    over the measurement window — not one per checkpoint.  The total egress
    contribution of each user type is therefore:

        W[D] * (window_days / D) * (cycle_avg_delta + p_needs_baseline * full_avg)

    where W[D] is the population fraction for that update frequency.  Summing
    across all user types gives the total expected egress for the window.

    This correctly accounts for the fact that daily updaters contribute far
    more download events than monthly updaters, even though each individual
    download for a daily updater is cheaper (smaller delta, less likely to
    need a baseline reset).
    """
    delta_curve  = [cp["DeltaOrigCompressedMB"] for cp in checkpoints]
    n            = len(checkpoints)
    period_days  = period * interval_days
    window_days  = max((n - 1) * interval_days, interval_days)

    # Average full index size over the measurement window (baselines are
    # published at various sizes as the index grows; use the mean as the
    # representative cost of downloading a baseline).
    full_avg = sum(cp["FullIndexCompressedMB"] for cp in checkpoints) / n

    # Average delta cost across a complete baseline cycle.
    # DeltaOrig[a] is the measured delta growth from baseline 0; used here as
    # a proxy for delta growth from any baseline (the key approximation).
    cycle_deltas    = [delta_curve[min(a, len(delta_curve) - 1)] for a in range(period)]
    cycle_avg_delta = sum(cycle_deltas) / period

    total_mb = 0.0
    for bucket in buckets:
        D = bucket["days"]
        W = bucket["weight"]

        # Number of download events from this user type over the window.
        num_downloads = window_days / D

        # Fraction of those downloads that require a new baseline.
        p_needs_baseline = min(D, period_days) / period_days

        cost_per_download = cycle_avg_delta + p_needs_baseline * full_avg
        total_mb += W * num_downloads * cost_per_download

    return total_mb


def find_crossover(checkpoints, threshold):
    """
    Return the index of the first checkpoint where
    DeltaOrigCompressedMB / FullIndexCompressedMB >= threshold, or None.
    """
    for i, cp in enumerate(checkpoints):
        full = cp["FullIndexCompressedMB"]
        if full > 0 and cp["DeltaOrigCompressedMB"] / full >= threshold:
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
    parser.add_argument("--egress-cost-per-gb", type=float, default=None, dest="cost_per_gb",
                        help="Optional egress cost per GB. If omitted, output is in bytes only.")
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
    if args.cost_per_gb is not None:
        print(f"  Egress cost:  ${args.cost_per_gb:.4f}/GB")
    print()

    # --- Delta growth crossovers -------------------------------------------
    c50  = find_crossover(checkpoints, 0.50)
    c100 = find_crossover(checkpoints, 1.00)
    print("  Delta (compressed) growth from baseline:")
    if c50 is not None:
        print(f"    Exceeds 50%  of full index at checkpoint {c50:>3} "
              f"({fmt_days(c50 * interval_days)})")
    else:
        print("    Never exceeds 50% of full index within measured period")
    if c100 is not None:
        print(f"    Exceeds 100% of full index at checkpoint {c100:>3} "
              f"({fmt_days(c100 * interval_days)})")
    else:
        print("    Never exceeds 100% of full index within measured period")
    print()

    # --- Simulate all periods -----------------------------------------------
    results = []
    for period in range(1, n + 1):
        total_mb = simulate_schedule(checkpoints, period, buckets, interval_days)
        results.append({
            "period":      period,
            "period_days": period * interval_days,
            "total_mb":    total_mb,
            "avg_mb":      total_mb / n,
        })

    optimal      = min(results, key=lambda r: r["total_mb"])
    always_full  = results[0]   # period == 1
    never_refresh = results[-1] # period == n

    # --- Determine which periods to print in the table ----------------------
    # Always show: period 1, optimal, and period n.
    # Also show a sample of ~15-20 evenly-spaced periods in between.
    show = {1, optimal["period"], n}
    step = max(1, n // 18)
    for p in range(step, n, step):
        show.add(p)

    # --- Print table --------------------------------------------------------
    print(f"--- Schedule Comparison (total compressed egress across measurement window) ---")
    print(f"    ('Total' = sum of all user download events × cost; comparable across schedules)")
    col_cost = args.cost_per_gb is not None
    hdr = (f"  {'Period':>6}  {'Interval':>9}  {'Total Egress':>14}  {'Avg/Checkpoint':>16}")
    if col_cost:
        hdr += f"  {'Total Cost':>12}"
    print(hdr)
    sep = f"  {'-'*6}  {'-'*9}  {'-'*14}  {'-'*16}" + (f"  {'-'*12}" if col_cost else "")
    print(sep)

    for r in sorted(results, key=lambda r: r["period"]):
        if r["period"] not in show:
            continue
        tag = ""
        if r["period"] == 1:
            tag = "  (always full)"
        elif r["period"] == n:
            tag = "  (never refresh)"
        elif r["period"] == optimal["period"]:
            tag = "  ← optimal"

        line = (f"  {r['period']:>6}  {fmt_days(r['period_days']):>9}  "
                f"{fmt_mb(r['total_mb']):>14}  {fmt_mb(r['avg_mb']):>16}{tag}")
        if col_cost:
            cost = r["total_mb"] / 1024 * args.cost_per_gb
            line += f"  ${cost:>11.2f}"
        print(line)

    print()

    # --- Recommendation summary ---------------------------------------------
    savings_vs_full  = 100.0 * (1.0 - optimal["total_mb"] / always_full["total_mb"])
    savings_vs_never = (100.0 * (1.0 - optimal["total_mb"] / never_refresh["total_mb"])
                        if never_refresh["total_mb"] > 0 else 0.0)

    print(f"  Recommendation: refresh baseline every {optimal['period']} checkpoint(s) "
          f"({fmt_days(optimal['period_days'])})")
    print(f"    Total egress:  {fmt_mb(optimal['total_mb'])}  "
          f"(avg {fmt_mb(optimal['avg_mb'])} per checkpoint)")
    print(f"    vs always-full:    {savings_vs_full:+.1f}%")
    if optimal["period"] < n:
        print(f"    vs never-refresh:  {savings_vs_never:+.1f}%")
    if col_cost:
        opt_cost  = optimal["total_mb"]    / 1024 * args.cost_per_gb
        full_cost = always_full["total_mb"] / 1024 * args.cost_per_gb
        print(f"    Estimated cost: ${opt_cost:.2f}  (vs ${full_cost:.2f} always-full)")
    print()

    # --- Chart --------------------------------------------------------------
    if args.output_chart:
        _write_chart(args.output_chart, checkpoints, optimal, interval_days,
                     description, always_full, never_refresh)


def _write_chart(path, checkpoints, optimal, interval_days, description,
                 always_full, never_refresh):
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
    ax.set_title(f"Delta Index Baseline Timing Analysis\nDistribution: {description}")
    ax.legend(loc="upper left")
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{x:.0f} MB"))
    fig.autofmt_xdate()
    plt.tight_layout()
    plt.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Chart saved to: {path}")


if __name__ == "__main__":
    main()
