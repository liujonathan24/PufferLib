#!/usr/bin/env python3
"""Plot writable-global bytes per commit.

Reads sizes.csv (produced by measure.sh) and renders globals.png /
globals.svg showing the per-commit total of .data + .bss + .tdata +
.tbss in libnethack.so. The horizontal axis is the commit ordinal,
the vertical axis is bytes. Each refactor commit is annotated with
its short subject so the drops align with the work that caused them.
"""
import csv
import os
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV  = os.path.join(HERE, "sizes.csv")
OUT  = os.path.join(HERE, "globals.png")
SVG  = os.path.join(HERE, "globals.svg")


def load():
    rows = []
    with open(CSV) as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append(row)
    return rows


def short_subject(s: str, n: int = 60) -> str:
    return s if len(s) <= n else s[: n - 1] + "…"


def main():
    rows = load()
    if not rows:
        print("no rows in", CSV, file=sys.stderr)
        sys.exit(1)

    idx   = [int(r["idx"]) for r in rows]
    data  = [int(r["data"])  for r in rows]
    bss   = [int(r["bss"])   for r in rows]
    tdata = [int(r["tdata"]) for r in rows]
    tbss  = [int(r["tbss"])  for r in rows]
    total = [d + b + td + tb for d, b, td, tb in zip(data, bss, tdata, tbss)]

    fig, ax = plt.subplots(figsize=(13, 7))

    # Stacked area: .data + .bss = process-shared; .tdata + .tbss = TLS.
    proc = [d + b   for d, b in zip(data, bss)]
    tls  = [td + tb for td, tb in zip(tdata, tbss)]
    ax.fill_between(idx, 0, proc, color="#d04444", alpha=0.7,
                    label=".data + .bss (process-shared)")
    ax.fill_between(idx, proc, [p + t for p, t in zip(proc, tls)],
                    color="#3a78c8", alpha=0.7,
                    label=".tdata + .tbss (thread-local)")
    ax.plot(idx, total, color="black", linewidth=2.2, marker="o",
            markersize=4, label="total writable globals")

    # Highlight the commits where the total dropped meaningfully.
    annotate_drops(ax, rows, total)

    ax.set_xlabel("commit ordinal (refactor progression)")
    ax.set_ylabel("bytes of writable global storage in libnethack.so")
    ax.set_title("NetHack thread-safety refactor — globals shrink toward 0")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right")

    # Mark the target.
    ax.axhline(0, color="green", linestyle="--", linewidth=1.5, alpha=0.6,
               label="target (no globals)")

    # Set x ticks at every commit, but only show short label every Nth.
    ax.set_xticks(idx)
    labels = []
    for i, r in enumerate(rows):
        if i % max(1, len(rows)//12) == 0:
            labels.append(r["short"])
        else:
            labels.append("")
    ax.set_xticklabels(labels, rotation=45, ha="right", fontsize=8)

    plt.tight_layout()
    plt.savefig(OUT, dpi=140)
    plt.savefig(SVG)
    print(f"wrote {OUT} and {SVG}")
    # Final-line summary.
    print(f"baseline (commit 0): {total[0]:>7,} bytes")
    print(f"latest   (commit {idx[-1]}): {total[-1]:>7,} bytes")
    print(f"reduction: {total[0]-total[-1]:>7,} bytes "
          f"({100.0*(total[0]-total[-1])/total[0]:.1f}%)")


def annotate_drops(ax, rows, total, threshold=500):
    for i in range(1, len(total)):
        drop = total[i-1] - total[i]
        if drop < threshold:
            continue
        subj = short_subject(rows[i]["subject"], 50)
        ax.annotate(f"-{drop:,}B\n{subj}",
                    xy=(i, total[i]),
                    xytext=(i, total[i] + drop / 2),
                    fontsize=7, ha="center", va="bottom",
                    arrowprops=dict(arrowstyle="->", color="gray", lw=0.6))


if __name__ == "__main__":
    main()
