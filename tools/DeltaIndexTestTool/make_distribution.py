#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
"""
make_distribution.py - Convert a telemetry export into an analyze.py distribution JSON.

Input is a CSV export of update-check events with the columns:

    "series name","x","y"

where `x` is the age in days of the client's previous index version at the moment
of the event, and `y` is the number of events observed at that age.  An `x` of -1
means the client had no previous version, i.e. a net-new client that must download
a full baseline regardless of schedule.

Because each row already counts *events*, the resulting weights are a distribution
over download events -- exactly what analyze.py's cost model expects.  No additional
weighting by update frequency is applied.

Output is the JSON format analyze.py consumes:

    {
      "description": "...",
      "buckets": [
        { "days": 0.5, "weight": 0.21 },
        { "new_client": true, "weight": 0.022 }
      ]
    }

TRUNCATED EXPORTS
-----------------
Exports that keep only the top N buckets by event count systematically discard the
stale tail.  Ages are floating-point differences between individual index publications
(a few hours apart), so frequent updaters pile onto a handful of shared ages while a
client returning after 40 days lands on a nearly unique age.  Those rare ages fall
below the export's cutoff and disappear entirely -- not because such clients are rare,
but because their events are spread thin.

The symptom is a sharp floor in the per-bucket counts: no bucket has fewer than some
value, and the bucket count is a round number.  --estimate-tail detects that floor,
fits a power law to the age bands that sit comfortably above it, and reinstates the
predicted missing mass as synthetic tail buckets.

Note for interpretation: any client staler than the refresh period must take a full
baseline, exactly like a net-new client.  Missing tail mass therefore acts as a
roughly constant tax on every candidate period -- it lowers the predicted savings
without greatly moving the optimal interval.

Usage:
    python make_distribution.py --csv export.csv --output distribution.json
    python make_distribution.py --csv export.csv --output distribution.json --estimate-tail
    python make_distribution.py --csv export.csv --output distribution.json --tail-fraction 0.15
"""

import argparse
import csv
import json
import math
import sys
from collections import defaultdict


def load_events(csv_path):
    """Return (staleness_events, new_client_count).

    staleness_events is a list of (days, count) with days >= 0.
    """
    staleness = []
    new_clients = 0.0
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            try:
                x = float(row["x"])
                y = float(row["y"])
            except (KeyError, TypeError, ValueError):
                continue
            if y <= 0:
                continue
            if x < 0:
                new_clients += y
            else:
                staleness.append((x, y))
    return staleness, new_clients


def detect_truncation(staleness):
    """Return (floor, is_truncated) for the export's per-bucket count floor.

    A 'top N buckets' export leaves a hard floor: the smallest surviving bucket
    count sits well above 1, and many buckets cluster just above it.
    """
    if not staleness:
        return 0.0, False
    counts = sorted(c for _, c in staleness)
    floor = counts[0]
    near_floor = sum(1 for c in counts if c < floor * 1.5)
    return floor, floor > 5 and near_floor >= 0.05 * len(counts)


def estimate_tail(staleness, floor, max_days, clean_factor=4.0):
    """Estimate mass lost to top-N truncation and return it as (days, count) buckets.

    Fits density(D) = C * D**k over geometric age bands whose mean bucket count is at
    least `clean_factor` times the truncation floor -- those bands are essentially
    unaffected by the cut -- then extrapolates out to `max_days` and reinstates the
    difference between predicted and observed mass.
    """
    if not staleness:
        return [], None

    edges = []
    e = 0.25
    while e < max_days:
        edges.append(e)
        e *= 1.5
    edges.append(max_days)

    bands = []
    for lo, hi in zip(edges, edges[1:]):
        sel = [c for d, c in staleness if lo <= d < hi]
        if sel:
            bands.append((math.sqrt(lo * hi), sum(sel) / (hi - lo), sum(sel) / len(sel)))

    clean = [(mid, dens) for mid, dens, mean_count in bands if mean_count >= clean_factor * floor]
    if len(clean) < 3:
        return [], None

    n = len(clean)
    sx = sum(math.log(d) for d, _ in clean)
    sy = sum(math.log(v) for _, v in clean)
    sxx = sum(math.log(d) ** 2 for d, _ in clean)
    sxy = sum(math.log(d) * math.log(v) for d, v in clean)
    denom = n * sxx - sx * sx
    if abs(denom) < 1e-12:
        return [], None
    k = (n * sxy - sx * sy) / denom
    c = math.exp((sy - k * sx) / n)
    if k >= -1.0:
        # Flatter than 1/D: the integral diverges and the fit cannot be trusted
        # to extrapolate.  Refuse rather than invent an enormous tail.
        return [], (c, k)

    def predicted(a, b):
        return c * (b ** (k + 1) - a ** (k + 1)) / (k + 1)

    start = max(mid for mid, _, _ in bands if mid <= max(m for m, _ in clean))
    tail = []
    lo = start
    while lo < max_days:
        hi = min(lo * 1.5, max_days)
        observed = sum(cnt for d, cnt in staleness if lo <= d < hi)
        missing = predicted(lo, hi) - observed
        if missing > 0:
            tail.append((math.sqrt(lo * hi), missing))
        lo = hi
    return tail, (c, k)


def spread_tail(mass, min_days, max_days, steps=24):
    """Distribute `mass` events log-uniformly across [min_days, max_days].

    Spreading matters: concentrating the assumed tail at a single age makes every
    refresh period shorter than that age pay the full cost and every longer period
    amortize it, which manufactures a spurious optimum right at the chosen age.
    """
    if mass <= 0 or max_days <= min_days:
        return []
    ratio = (max_days / min_days) ** (1.0 / steps)
    edges = [min_days * ratio ** i for i in range(steps + 1)]
    per = mass / steps
    return [(math.sqrt(lo * hi), per) for lo, hi in zip(edges, edges[1:])]


def bin_events(staleness, bin_days):
    """Aggregate (days, count) pairs into bins.

    Each bin is represented by its count-weighted mean age, which keeps the
    expectation of min(D, P) exact for every bin that does not straddle P.
    A bin_days of 0 disables binning and keeps every distinct age.
    """
    if bin_days <= 0:
        merged = defaultdict(float)
        for days, count in staleness:
            merged[days] += count
        return sorted(merged.items())

    sums = defaultdict(float)
    counts = defaultdict(float)
    for days, count in staleness:
        key = int(days / bin_days)
        sums[key] += days * count
        counts[key] += count
    return [(sums[k] / counts[k], counts[k]) for k in sorted(counts)]


def main():
    parser = argparse.ArgumentParser(
        description="Convert a telemetry export into an analyze.py distribution JSON.")
    parser.add_argument("--csv", required=True, help="Telemetry export CSV path")
    parser.add_argument("--output", required=True, help="Destination JSON path")
    parser.add_argument("--bin-days", type=float, default=0.0, dest="bin_days",
                        help="Bin width in days for aggregating ages. "
                             "0 (default) keeps every distinct age, which is exact.")
    parser.add_argument("--description", default=None,
                        help="Description recorded in the JSON. Defaults to a generated one.")
    parser.add_argument("--estimate-tail", action="store_true", dest="estimate_tail",
                        help="Detect top-N truncation and reinstate the missing stale tail "
                             "by power-law extrapolation. See the module docstring.")
    parser.add_argument("--tail-fraction", type=float, default=None, dest="tail_fraction",
                        help="Instead of estimating, assert that this fraction (0-1) of all "
                             "events are staler than the export shows. Overrides --estimate-tail.")
    parser.add_argument("--tail-min-days", type=float, default=21.0, dest="tail_min_days",
                        help="Youngest age for --tail-fraction mass. Default 21.")
    parser.add_argument("--tail-max-days", type=float, default=365.0, dest="tail_max_days",
                        help="Upper age limit when extrapolating with --estimate-tail. Default 365.")
    args = parser.parse_args()

    staleness, new_clients = load_events(args.csv)
    if not staleness and not new_clients:
        print("Error: no usable events found in CSV.", file=sys.stderr)
        sys.exit(1)

    floor, truncated = detect_truncation(staleness)
    observed_total = sum(c for _, c in staleness) + new_clients

    tail = []
    fit = None
    if args.tail_fraction is not None:
        if not 0.0 <= args.tail_fraction < 1.0:
            print("Error: --tail-fraction must be in [0, 1).", file=sys.stderr)
            sys.exit(1)
        if args.tail_fraction > 0:
            # Mass m such that m / (observed + m) == tail_fraction.
            mass = observed_total * args.tail_fraction / (1.0 - args.tail_fraction)
            # Spread log-uniformly rather than lumping at one age: a point mass creates
            # an artificial cliff at that age, because periods longer than it suddenly
            # start amortizing those clients and a spurious second optimum appears.
            tail = spread_tail(mass, args.tail_min_days, args.tail_max_days)
    elif args.estimate_tail:
        tail, fit = estimate_tail(staleness, floor, args.tail_max_days)
        if not tail:
            print("Warning: could not fit a usable tail; emitting the observed data unchanged.",
                  file=sys.stderr)

    binned = bin_events(staleness, args.bin_days) + [(d, c) for d, c in tail]
    binned.sort()
    total = sum(c for _, c in binned) + new_clients

    buckets = [{"days": round(days, 6), "weight": count / total} for days, count in binned]
    if new_clients > 0:
        buckets.append({"new_client": True, "weight": new_clients / total})

    max_age = max((d for d, _ in binned), default=0.0)
    tail_mass = sum(c for _, c in tail)
    description = args.description or (
        f"Telemetry-derived from {args.csv}: {total:,.0f} download events, "
        f"{100.0 * new_clients / total:.2f}% net-new clients, "
        f"observed ages 0-{max(d for d, _ in staleness):.1f} days"
        + (f", plus {100.0 * tail_mass / total:.1f}% reinstated stale tail out to "
           f"{max_age:.0f} days" if tail_mass else ""))

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump({"description": description, "buckets": buckets}, f, indent=2)

    mean_age = sum(d * c for d, c in binned) / sum(c for _, c in binned) if binned else 0.0
    print(f"Wrote {args.output}")
    print(f"  Events:        {total:,.0f}")
    print(f"  Buckets:       {len(buckets)}")
    print(f"  New clients:   {100.0 * new_clients / total:.2f}%")
    print(f"  Mean age:      {mean_age:.3f} days")
    print(f"  Max age:       {max_age:.2f} days")
    if truncated:
        print(f"  NOTE: export looks truncated -- no bucket below {floor:.0f} events. "
              f"The stale tail is under-counted.")
        if not tail_mass:
            print(f"        Re-run with --estimate-tail or --tail-fraction to model it.")
    if fit:
        print(f"  Tail fit:      density = {fit[0]:.0f} * D^{fit[1]:.3f}")
    if tail_mass:
        print(f"  Tail added:    {tail_mass:,.0f} events ({100.0 * tail_mass / total:.2f}% of total)")


if __name__ == "__main__":
    main()
