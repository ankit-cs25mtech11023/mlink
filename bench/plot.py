# SPDX-License-Identifier: GPL-2.0-only
"""Plot the P6.4 matrix CSVs into docs/img/ (PNG, light surface).

Usage (Windows, repo root):  .venv\\Scripts\\python bench\\plot.py [flavour]
Every number shown comes from bench/results/<flavour>/*.csv; medians over the
repetitions, error bars = min..max.
"""
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FLAV = sys.argv[1] if len(sys.argv) > 1 else "perf"
RES = ROOT / "bench" / "results" / FLAV
IMG = ROOT / "docs" / "img"
IMG.mkdir(parents=True, exist_ok=True)

# Reference categorical palette, fixed order (never cycled), plus ink/surface.
S1, S2, S3, S4 = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": INK2, "axes.labelcolor": INK, "text.color": INK,
    "xtick.color": INK2, "ytick.color": INK2, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.axisbelow": True, "axes.spines.top": False,
    "axes.spines.right": False, "font.size": 10, "legend.frameon": False,
})
NOTE = f"{FLAV} kernel · 2 KVM guests on i5-8250U, turbo off (1.6 GHz) · median, bars = min–max"


def save(fig, name):
    fig.text(0.01, 0.01, NOTE, fontsize=8, color=INK2)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(IMG / f"{name}.png", dpi=150)
    plt.close(fig)
    print("wrote", IMG / f"{name}.png")


def agg(df, by, col):
    g = df.groupby(by)[col]
    return g.median(), g.min(), g.max()


def bars(ax, x, groups, labels, colors, width=0.38):
    """Grouped bars with min..max whiskers; 2px surface gap between bars."""
    n = len(groups)
    for i, ((med, lo, hi), lab, col) in enumerate(zip(groups, labels, colors)):
        pos = [j + (i - (n - 1) / 2) * width for j in range(len(x))]
        ax.bar(pos, med.values, width=width, color=col, label=lab,
               edgecolor=SURFACE, linewidth=2)
        ax.errorbar(pos, med.values, yerr=[med.values - lo.values, hi.values - med.values],
                    fmt="none", ecolor=INK2, elinewidth=1, capsize=3)
    ax.set_xticks(range(len(x)))
    ax.set_xticklabels([str(v) for v in x])


def e1():
    df = pd.read_csv(RES / "e1_doorbell.csv")
    cols = ["min_ns", "p50_ns", "p90_ns", "p99_ns"]
    med = df[cols].median() / 1000
    fig, ax = plt.subplots(figsize=(5.5, 3.2))
    ax.bar(range(len(cols)), med.values, color=S1, edgecolor=SURFACE, linewidth=2, width=0.6)
    for i, v in enumerate(med.values):
        ax.text(i, v, f"{v:.1f}", ha="center", va="bottom", fontsize=9, color=INK)
    ax.set_xticks(range(len(cols)))
    ax.set_xticklabels(["min", "p50", "p90", "p99"])
    ax.set_ylabel("µs")
    ax.set_title("Doorbell round trip (AP → modem → AP, IRQ to IRQ)", loc="left")
    save(fig, f"e1_doorbell_{FLAV}")


def e2():
    df = pd.read_csv(RES / "e2_mtu.csv")
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4), sharey=True)
    for ax, proto in zip(axes, ["tcp", "udp"]):
        d = df[df.proto == proto]
        mtus = sorted(d.mtu.unique())
        groups = [agg(d[d.dir == k], "mtu", "mbps") for k in ("fwd", "rev")]
        bars(ax, mtus, groups, ["AP → modem", "modem → AP"], [S1, S2])
        ax.set_title(f"{proto.upper()} throughput vs MTU", loc="left")
        ax.set_xlabel("MTU (bytes)")
    axes[0].set_ylabel("Mbit/s (receiver)")
    axes[1].legend(loc="upper left")
    save(fig, f"e2_mtu_{FLAV}")


def e3():
    df = pd.read_csv(RES / "e3_napi.csv")
    med, lo, hi = agg(df, "napi_weight", "mbps")
    fig, ax = plt.subplots(figsize=(5.5, 3.2))
    ax.errorbar(med.index, med.values, yerr=[med - lo, hi - med], color=S1, linewidth=2,
                marker="o", markersize=7, markeredgecolor=SURFACE, markeredgewidth=2,
                ecolor=INK2, elinewidth=1, capsize=3)
    ax.set_xscale("log", base=2)
    ax.set_xticks(list(med.index))
    ax.set_xticklabels([str(w) for w in med.index])
    ax.set_xlabel("NAPI weight (budget per poll)")
    ax.set_ylabel("TCP Mbit/s")
    ax.set_ylim(bottom=0)
    ax.set_title("TCP throughput vs NAPI weight", loc="left")
    save(fig, f"e3_napi_{FLAV}")


def e4():
    df = pd.read_csv(RES / "e4_latency.csv")
    order = [m for m in ["loopback_stack_only", "irq", "busy_poll_50us", "no_kick_suppression"]
             if m in set(df["mode"])]
    names = {"loopback_stack_only": "loopback\n(stack only)", "irq": "mlink0\ninterrupts",
             "busy_poll_50us": "mlink0\nbusy-poll 50µs", "no_kick_suppression": "mlink0\nkick every pkt"}
    groups = []
    for col in ("p50_us", "p99_us"):
        med = df.groupby("mode")[col].median().reindex(order)
        lo = df.groupby("mode")[col].min().reindex(order)
        hi = df.groupby("mode")[col].max().reindex(order)
        groups.append((med, lo, hi))
    fig, ax = plt.subplots(figsize=(7, 3.6))
    bars(ax, [names[m] for m in order], groups, ["p50", "p99"], [S1, S2])
    ax.set_ylabel("UDP round trip, µs (64 B)")
    ax.set_title("UDP request/response latency", loc="left")
    ax.legend(loc="upper left")
    save(fig, f"e4_latency_{FLAV}")


def e5():
    df = pd.read_csv(RES / "e5_irq_load.csv")
    df = df[df.rx_mbps.notna()]
    by = df.groupby("offered_mbps")
    x = by.rx_mbps.median()
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4))
    for ax, col, lab, color in ((axes[0], "irqs_per_pkt", "RX interrupts per packet", S1),
                                (axes[1], "modem_cpu_busy_pct", "modem guest CPU busy %", S3)):
        y = by[col].median()
        ax.plot(x.values, y.values, color=color, linewidth=2, marker="o", markersize=7,
                markeredgecolor=SURFACE, markeredgewidth=2)
        ax.set_xlabel("received UDP load, Mbit/s (1400 B)")
        ax.set_title(lab, loc="left")
        ax.set_ylim(bottom=0)
    save(fig, f"e5_irq_load_{FLAV}")


for f in (e1, e2, e3, e4, e5):
    try:
        f()
    except FileNotFoundError as exc:
        print("skip", f.__name__, "-", exc.filename)
