#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-3.0-only
"""Render TermX Kitty benchmark CSV as per-(screen size, update case) bar charts."""

import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import Patch

TRANSPORT_COLORS = {
    "direct": "#4c78a8",
    "temporary-file": "#f58518",
    "shared-memory": "#54a24b",
}


def read_rows(path: Path):
    with path.open(newline="") as stream:
        lines = (line for line in stream if not line.startswith("#"))
        rows = list(csv.DictReader(lines))
    for row in rows:
        row["screen_width"] = int(row["screen_width"])
        row["screen_height"] = int(row["screen_height"])
        row["p50_ms"] = float(row["p50_ms"])
        row["p90_ms"] = float(row["p90_ms"])
    return rows


def config_label(row):
    transport = {
        "direct": "direct",
        "temporary-file": "temp-file",
        "shared-memory": "shm",
    }[row["transport"]]
    chunk = f"/{row['chunk_size']}" if row["chunk_size"] != "none" else ""
    return f"{row['zlib']} / {transport}{chunk}"


def plot_group(size, case, rows):
    labels = [config_label(row) for row in rows]
    p50 = [row["p50_ms"] for row in rows]
    p90 = [row["p90_ms"] for row in rows]
    colors = [TRANSPORT_COLORS[row["transport"]] for row in rows]
    positions = list(range(len(rows)))

    figure, axis = plt.subplots(figsize=(11, 7.5), layout="constrained")
    axis.barh(positions, p90, color="#d9d9d9", height=0.72, label="p90")
    axis.barh(positions, p50, color=colors, height=0.44, label="p50")
    axis.set_yticks(positions, labels)
    axis.invert_yaxis()
    axis.set_xlabel("Kitty acknowledgement latency (ms)")
    axis.set_title(f"{size[0]}×{size[1]} — {case}")
    axis.grid(axis="x", alpha=0.25)

    maximum = max(p90, default=1.0)
    axis.set_xlim(0, maximum * 1.22 if maximum else 1.0)
    for position, median, tail in zip(positions, p50, p90):
        axis.text(
            tail + maximum * 0.012,
            position,
            f"{median:.2f} / {tail:.2f}",
            va="center",
            fontsize=8,
        )

    legend = [
        Patch(facecolor="#d9d9d9", label="p90 extent"),
        Patch(facecolor="#4c78a8", label="p50 direct"),
        Patch(facecolor="#f58518", label="p50 temporary file"),
        Patch(facecolor="#54a24b", label="p50 shared memory"),
    ]
    axis.legend(handles=legend, loc="lower right", fontsize=8)
    return figure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--output-directory", type=Path, default=Path("kitty-benchmark-charts"))
    parser.add_argument("--pdf", type=Path, default=Path("kitty-benchmark-charts.pdf"))
    args = parser.parse_args()

    rows = read_rows(args.csv)
    grouped = {}
    for row in rows:
        key = ((row["screen_width"], row["screen_height"]), row["case"])
        grouped.setdefault(key, []).append(row)

    args.output_directory.mkdir(parents=True, exist_ok=True)
    with PdfPages(args.pdf) as pdf:
        for (size, case), group in sorted(
            grouped.items(), key=lambda item: (item[0][0][0] * item[0][0][1], item[0][1])
        ):
            figure = plot_group(size, case, group)
            output = args.output_directory / f"{size[0]}x{size[1]}-{case}.png"
            figure.savefig(output, dpi=160)
            pdf.savefig(figure)
            plt.close(figure)

    print(f"wrote {len(grouped)} PNG charts to {args.output_directory}")
    print(f"wrote multi-page PDF to {args.pdf}")


if __name__ == "__main__":
    main()
