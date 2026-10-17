#!/usr/bin/env python3
"""Turn a measurement run into results.json, results.md and a few plots.

Inputs (all in --results): stats.json and frames.csv from telem_rx.py,
serial.log from QEMU and qemu_start (epoch seconds when QEMU was launched).
"""
import argparse
import csv
import json
import re
from pathlib import Path

LINE = re.compile(r"^\[\s*(\d+\.\d+)\]\s(.*)$")
MARKERS = {
    "kernel_init_s": "Run /sbin/init",
    "driver_probe_s": "virtual IMU ready",
    "init_s90_s": "sensgw: init reached S90",
    "first_frame_s": "telemd: first frame sent",
}


def parse_serial(path: Path):
    found = {}
    if not path.exists():
        return found
    for raw in path.read_text(errors="replace").splitlines():
        m = LINE.match(raw.strip())
        if not m:
            continue
        t, msg = float(m.group(1)), m.group(2)
        for key, needle in MARKERS.items():
            if key not in found and needle in msg:
                found[key] = t
    return found


def file_size(path: Path):
    return path.stat().st_size if path.exists() else None


def sizes(images: Path):
    target = images.parent / "target"
    rootfs = images / "rootfs.ext4"
    if not rootfs.exists():
        rootfs = images / "rootfs.ext2"
    mods = sorted(target.glob("lib/modules/*/updates/vimu.ko"))
    used = None
    if target.exists():
        used = sum(p.stat().st_size for p in target.rglob("*")
                   if p.is_file() and not p.is_symlink())
    return {
        "kernel_image_bytes": file_size(images / "Image"),
        "rootfs_image_bytes": file_size(rootfs),
        "rootfs_content_bytes": used,
        "vimu_ko_bytes": file_size(mods[0]) if mods else None,
        "telemd_bytes": file_size(target / "usr/bin/telemd"),
    }


def load_frames(path: Path):
    rows = []
    if path.exists():
        with open(path, newline="") as fh:
            for r in csv.DictReader(fh):
                rows.append({k: int(v) for k, v in r.items()})
    return rows


def plots(frames, results: Path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("report: matplotlib not installed, skipping plots")
        return []

    out = []
    t0 = frames[0]["t_sample_ns"]
    first = [f for f in frames if f["t_sample_ns"] - t0 <= 5e9]
    t = [(f["t_sample_ns"] - t0) / 1e9 for f in first]

    fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 5), sharex=True)
    for key, label in (("ax_mg", "x"), ("ay_mg", "y")):
        a1.plot(t, [f[key] for f in first], label=label, linewidth=1)
    a1.set_ylabel("accel [mg]")
    a1.legend(loc="upper right")
    for key, label in (("gx_mdps", "x"), ("gy_mdps", "y"), ("gz_mdps", "z")):
        a2.plot(t, [f[key] for f in first], label=label, linewidth=1)
    a2.set_ylabel("gyro [mdps]")
    a2.set_xlabel("time [s]")
    a2.legend(loc="upper right")
    fig.suptitle("Telemetry received on the host (first 5 s)")
    fig.tight_layout()
    fig.savefig(results / "telemetry.png", dpi=130)
    plt.close(fig)
    out.append("telemetry.png")

    lat = [(f["t_send_ns"] - f["t_sample_ns"]) / 1e3 for f in frames]
    fig, ax = plt.subplots(figsize=(9, 3.5))
    ax.hist(lat, bins=60)
    ax.set_xlabel("driver -> socket latency [us]")
    ax.set_ylabel("frames")
    ax.set_title("Time a sample spends between the driver and sendto()")
    fig.tight_layout()
    fig.savefig(results / "latency.png", dpi=130)
    plt.close(fig)
    out.append("latency.png")
    return out


def fmt_bytes(n):
    if n is None:
        return "n/a"
    return f"{n / 1024 / 1024:.1f} MiB" if n >= 1024 * 1024 else f"{n / 1024:.1f} KiB"


def fmt(v, unit=""):
    return "n/a" if v is None else f"{v}{unit}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=Path, required=True)
    ap.add_argument("--results", type=Path, required=True)
    args = ap.parse_args()
    res = args.results

    stats = json.loads((res / "stats.json").read_text()) if (res / "stats.json").exists() else {}
    boot = parse_serial(res / "serial.log")
    size = sizes(args.images)
    frames = load_frames(res / "frames.csv")

    wall = None
    qs = res / "qemu_start"
    if qs.exists() and stats.get("first_frame_epoch"):
        wall = round(stats["first_frame_epoch"] - float(qs.read_text()), 2)

    images = plots(frames, res) if len(frames) > 1 else []
    data = {"boot": boot, "wall_qemu_to_first_frame_s": wall, "sizes": size,
            "telemetry": stats, "plots": images}
    (res / "results.json").write_text(json.dumps(data, indent=2))

    iv = stats.get("interval_us", {})
    lat = stats.get("latency_us", {})
    md = [
        "# Measurement run",
        "",
        "QEMU virt, Cortex-A53, 1 CPU, 256 MiB, no KVM (TCG). Timings are from",
        "an emulated CPU and are only useful for comparing runs with each other.",
        "",
        "| Boot (kernel timestamps) | |",
        "|---|---|",
        f"| kernel hands over to /sbin/init | {fmt(boot.get('kernel_init_s'), ' s')} |",
        f"| vimu driver probed | {fmt(boot.get('driver_probe_s'), ' s')} |",
        f"| init reaches S90telemd | {fmt(boot.get('init_s90_s'), ' s')} |",
        f"| first telemetry frame sent | {fmt(boot.get('first_frame_s'), ' s')} |",
        f"| QEMU launch to first frame on host (wall clock) | {fmt(wall, ' s')} |",
        "",
        "| Size | |",
        "|---|---|",
        f"| kernel Image | {fmt_bytes(size['kernel_image_bytes'])} |",
        f"| rootfs content | {fmt_bytes(size['rootfs_content_bytes'])} |",
        f"| vimu.ko | {fmt_bytes(size['vimu_ko_bytes'])} |",
        f"| telemd | {fmt_bytes(size['telemd_bytes'])} |",
        "",
        "| Telemetry | |",
        "|---|---|",
        f"| frames received | {fmt(stats.get('frames'))} |",
        f"| capture span | {fmt(stats.get('span_s'), ' s')} |",
        f"| measured rate | {fmt(stats.get('rate_hz'), ' Hz')} |",
        f"| lost (seq gaps) | {fmt(stats.get('lost'))} |",
        f"| bad frames | {sum(stats.get('errors', {}).values()) if stats else 'n/a'} |",
        f"| sample interval mean / stdev | {fmt(iv.get('mean'))} / {fmt(iv.get('stdev'))} us |",
        f"| sample interval min / max | {fmt(iv.get('min'))} / {fmt(iv.get('max'))} us |",
        f"| driver -> socket p50 / p99 / max | {fmt(lat.get('p50'))} / {fmt(lat.get('p99'))} / {fmt(lat.get('max'))} us |",
        "",
    ]
    md += [f"![{p}]({p})" for p in images]
    (res / "results.md").write_text("\n".join(md) + "\n")
    print("\n".join(md))


if __name__ == "__main__":
    main()
