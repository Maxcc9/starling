#!/usr/bin/env python3
"""Generate combined Pareto reports for Starling feature ablations."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from generate_starling_reports import FIELDS, parse_logs, qps_frontier, latency_frontier, write_csv


def plot_metric(
    analysis_dir: Path,
    rows_by_method: dict[str, list[dict[str, object]]],
    metric: str,
    ylabel: str,
    stem: str,
    higher_is_better: bool,
) -> None:
    fig, ax = plt.subplots(figsize=(8.5, 5.5), dpi=160)

    for method, rows in rows_by_method.items():
        if not rows:
            continue
        frontier = qps_frontier(rows) if higher_is_better else latency_frontier(rows, metric)
        ax.scatter(
            [r["recall_percent"] for r in rows],
            [r[metric] for r in rows],
            s=18,
            alpha=0.22,
        )
        ax.plot(
            [r["recall_percent"] for r in frontier],
            [r[metric] for r in frontier],
            marker="o",
            linewidth=1.9,
            label=method,
        )

    ax.set_title(f"Starling feature ablation: Recall vs {ylabel}")
    ax.set_xlabel("Recall (%)")
    ax.set_ylabel(ylabel)
    ax.grid(True, linestyle="--", linewidth=0.5, alpha=0.45)
    ax.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(analysis_dir / f"{stem}.png")
    fig.savefig(analysis_dir / f"{stem}.svg")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ablation-dir", required=True, type=Path)
    parser.add_argument("--dataset", required=True)
    args = parser.parse_args()

    ablation_dir = args.ablation_dir
    analysis_dir = ablation_dir / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)

    rows_by_method: dict[str, list[dict[str, object]]] = {}
    all_rows: list[dict[str, object]] = []

    for line_dir in sorted(p for p in ablation_dir.iterdir() if p.is_dir() and p.name != "analysis"):
        search_dir = line_dir / "search"
        if not search_dir.is_dir():
            continue
        rows = parse_logs(search_dir, args.dataset, line_dir.name)
        if not rows:
            continue
        rows_by_method[line_dir.name] = rows
        all_rows.extend(rows)

    if not all_rows:
        raise SystemExit(f"No parseable ablation logs found under {ablation_dir}")

    write_csv(analysis_dir / f"{args.dataset}_starling_ablation_all_points.csv", all_rows)

    frontier_rows = []
    latency_rows = []
    for method, rows in rows_by_method.items():
        for row in qps_frontier(rows):
            item = dict(row)
            item["frontier_metric"] = "qps"
            frontier_rows.append(item)
        for metric in ["mean_latency_us", "p50_latency_us", "p99_latency_us"]:
            for row in latency_frontier(rows, metric):
                item = dict(row)
                item["frontier_metric"] = metric
                item["latency_us"] = row[metric]
                latency_rows.append(item)

    write_csv(
        analysis_dir / f"{args.dataset}_starling_ablation_qps_frontiers.csv",
        frontier_rows,
        ["frontier_metric"] + FIELDS,
    )
    write_csv(
        analysis_dir / f"{args.dataset}_starling_ablation_latency_frontiers.csv",
        latency_rows,
        ["frontier_metric", "latency_us"] + FIELDS,
    )

    plot_metric(analysis_dir, rows_by_method, "qps", "QPS (queries/s)", f"{args.dataset}_starling_ablation_recall_qps", True)
    plot_metric(
        analysis_dir,
        rows_by_method,
        "mean_latency_us",
        "Mean latency (us)",
        f"{args.dataset}_starling_ablation_recall_mean_latency",
        False,
    )
    plot_metric(
        analysis_dir,
        rows_by_method,
        "p50_latency_us",
        "P50 latency (us)",
        f"{args.dataset}_starling_ablation_recall_p50_latency",
        False,
    )
    plot_metric(
        analysis_dir,
        rows_by_method,
        "p99_latency_us",
        "P99 latency (us)",
        f"{args.dataset}_starling_ablation_recall_p99_latency",
        False,
    )

    print(f"Wrote combined ablation reports under {analysis_dir}")


if __name__ == "__main__":
    main()
