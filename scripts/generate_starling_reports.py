#!/usr/bin/env python3
"""Generate Starling benchmark CSVs and Pareto plots from search logs."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


LOG_RE = re.compile(
    r"search_SQ(?P<sq>\d+)_K(?P<k>\d+)_CACHE(?P<cache>\d+)_BW(?P<bw>\d+)_T(?P<threads>\d+)"
    r"_MEML(?P<mem_l>\d+)_MEMK(?P<mem_topk>\d+)_MEM_USE_FREQ(?P<mem_use_freq>\d+)"
    r"_PS(?P<page>\d+)_USE_RATIO(?P<ratio>[0-9.]+)_GP_USE_FREQ(?P<gp_use_freq>\d+)"
    r"_GP_LOCK_NUMS(?P<gp_lock_nums>\d+)_GP_CUT(?P<gp_cut>\d+)\.log$"
)


FIELDS = [
    "dataset",
    "method",
    "recall_metric",
    "recall_percent",
    "qps",
    "mean_latency_us",
    "p50_latency_us",
    "p99_latency_us",
    "p999_latency_us",
    "mean_ios",
    "L",
    "beamwidth",
    "threads",
    "ps_use_ratio",
    "mem_l",
    "mem_topk",
    "mem_use_freq",
    "gp_use_freq",
    "gp_lock_nums",
    "gp_cut",
    "cache",
    "use_page_search",
    "use_sq",
    "log_path",
]


def parse_logs(search_dir: Path, dataset: str, method: str) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for log in sorted(search_dir.glob("search_*.log")):
        match = LOG_RE.search(log.name)
        if not match:
            continue
        meta = match.groupdict()
        for line in log.read_text(errors="replace").splitlines():
            parts = line.split()
            if len(parts) == 13 and parts[0].isdigit():
                rows.append(
                    {
                        "dataset": dataset,
                        "method": method,
                        "recall_metric": f"Recall@{meta['k']}",
                        "recall_percent": float(parts[12]),
                        "qps": float(parts[2]),
                        "mean_latency_us": float(parts[3]),
                        "p50_latency_us": float(parts[4]),
                        "p99_latency_us": float(parts[5]),
                        "p999_latency_us": float(parts[6]),
                        "mean_ios": float(parts[7]),
                        "L": int(parts[0]),
                        "beamwidth": int(parts[1]),
                        "threads": int(meta["threads"]),
                        "ps_use_ratio": float(meta["ratio"]),
                        "mem_l": int(meta["mem_l"]),
                        "mem_topk": int(meta["mem_topk"]),
                        "mem_use_freq": int(meta["mem_use_freq"]),
                        "gp_use_freq": int(meta["gp_use_freq"]),
                        "gp_lock_nums": int(meta["gp_lock_nums"]),
                        "gp_cut": int(meta["gp_cut"]),
                        "cache": int(meta["cache"]),
                        "use_page_search": int(meta["page"]),
                        "use_sq": int(meta["sq"]),
                        "log_path": str(log),
                    }
                )
    return rows


def write_csv(path: Path, rows: list[dict[str, object]], fields: list[str] = FIELDS) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row[field] for field in fields})


def qps_frontier(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    frontier = []
    for row in rows:
        dominated = False
        for other in rows:
            if other is row:
                continue
            if (
                other["recall_percent"] >= row["recall_percent"]
                and other["qps"] >= row["qps"]
                and (other["recall_percent"] > row["recall_percent"] or other["qps"] > row["qps"])
            ):
                dominated = True
                break
        if not dominated:
            frontier.append(row)
    return sorted(frontier, key=lambda item: (item["recall_percent"], -item["qps"]))


def latency_frontier(rows: list[dict[str, object]], metric: str) -> list[dict[str, object]]:
    frontier = []
    for row in rows:
        dominated = False
        for other in rows:
            if other is row:
                continue
            if (
                other["recall_percent"] >= row["recall_percent"]
                and other[metric] <= row[metric]
                and (other["recall_percent"] > row["recall_percent"] or other[metric] < row[metric])
            ):
                dominated = True
                break
        if not dominated:
            frontier.append(row)
    return sorted(frontier, key=lambda item: item["recall_percent"])


def plot_qps(report_dir: Path, rows: list[dict[str, object]], frontier: list[dict[str, object]], stem: str) -> None:
    fig, ax = plt.subplots(figsize=(8, 5), dpi=160)
    ax.scatter([r["recall_percent"] for r in rows], [r["qps"] for r in rows], s=24, alpha=0.5, label="all points")
    ax.plot(
        [r["recall_percent"] for r in frontier],
        [r["qps"] for r in frontier],
        marker="o",
        linewidth=2.2,
        color="tab:orange",
        label="QPS Pareto frontier",
    )
    ax.set_title("Starling benchmark: Recall vs QPS")
    ax.set_xlabel("Recall (%)")
    ax.set_ylabel("QPS (queries/s)")
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.45)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(report_dir / f"{stem}.png")
    fig.savefig(report_dir / f"{stem}.svg")
    plt.close(fig)


def plot_latency(report_dir: Path, rows: list[dict[str, object]], metric: str, ylabel: str, stem: str) -> None:
    frontier = latency_frontier(rows, metric)
    fig, ax = plt.subplots(figsize=(8, 5), dpi=160)
    for ratio in sorted({r["ps_use_ratio"] for r in rows}):
        ratio_rows = [r for r in rows if r["ps_use_ratio"] == ratio]
        ax.scatter(
            [r["recall_percent"] for r in ratio_rows],
            [r[metric] for r in ratio_rows],
            s=24,
            alpha=0.5,
            label=f"all points ratio={ratio:g}",
        )
    ax.plot(
        [r["recall_percent"] for r in frontier],
        [r[metric] for r in frontier],
        marker="o",
        linewidth=2.2,
        color="tab:red",
        label="latency Pareto frontier",
    )
    ax.set_title(f"Starling benchmark: Recall vs {ylabel}")
    ax.set_xlabel("Recall (%)")
    ax.set_ylabel(ylabel)
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.45)
    ax.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(report_dir / f"{stem}.png")
    fig.savefig(report_dir / f"{stem}.svg")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report-dir", required=True, type=Path)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--method", default="Starling")
    parser.add_argument("--prefix", default=None)
    args = parser.parse_args()

    report_dir = args.report_dir
    search_dir = report_dir / "search"
    analysis_dir = report_dir / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)

    rows = parse_logs(search_dir, args.dataset, args.method)
    if not rows:
        raise SystemExit(f"No parseable search logs found under {search_dir}")

    prefix = args.prefix or f"{args.method.lower().replace(' ', '_')}_{args.dataset}"
    rows = sorted(rows, key=lambda item: (item["recall_percent"], -item["qps"], item["threads"], item["ps_use_ratio"], item["L"]))

    all_csv = analysis_dir / f"{prefix}_pareto_all_points.csv"
    frontier_csv = analysis_dir / f"{prefix}_pareto_frontier.csv"
    write_csv(all_csv, rows)

    qps_rows = qps_frontier(rows)
    write_csv(frontier_csv, qps_rows)
    plot_qps(analysis_dir, rows, qps_rows, f"{prefix}_recall_qps_pareto")

    combined_latency = []
    for metric, ylabel, stem in [
        ("mean_latency_us", "Mean latency (us)", "recall_mean_latency_pareto"),
        ("p50_latency_us", "P50 latency (us)", "recall_p50_latency_pareto"),
        ("p99_latency_us", "P99 latency (us)", "recall_p99_latency_pareto"),
    ]:
        plot_latency(analysis_dir, rows, metric, ylabel, f"{prefix}_{stem}")
        for row in latency_frontier(rows, metric):
            item = dict(row)
            item["latency_metric"] = metric
            item["latency_us"] = row[metric]
            combined_latency.append(item)

    latency_fields = ["latency_metric", "latency_us"] + FIELDS
    write_csv(analysis_dir / f"{prefix}_recall_latency_pareto_frontiers.csv", combined_latency, latency_fields)

    print(f"Wrote {len(rows)} points to {all_csv}")
    print(f"Wrote {len(qps_rows)} QPS Pareto points to {frontier_csv}")
    print(f"Wrote latency Pareto plots and CSV under {analysis_dir}")


if __name__ == "__main__":
    main()
