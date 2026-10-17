#!/usr/bin/env python3
"""Receive telemd frames over UDP, check them and write frames.csv + stats.json.

Runs until --capture seconds have passed since the first frame, or --timeout
seconds since start if nothing arrives. Exit code is 1 when no valid frame
was received.
"""
import argparse
import csv
import json
import socket
import statistics
import struct
import sys
import time
from pathlib import Path

FRAME = struct.Struct("<HBBIQQ3h3hH")
MAGIC = 0x5347


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def parse(pkt: bytes):
    """Return a dict for a valid frame, or a string describing the problem."""
    if len(pkt) != FRAME.size:
        return "bad_length"
    f = FRAME.unpack(pkt)
    if f[0] != MAGIC:
        return "bad_magic"
    if crc16_ccitt(pkt[:-2]) != f[-1]:
        return "bad_crc"
    return {
        "seq": f[3],
        "t_sample_ns": f[4],
        "t_send_ns": f[5],
        "accel_mg": f[6:9],
        "gyro_mdps": f[9:12],
    }


def percentile(values, p):
    if not values:
        return None
    s = sorted(values)
    k = min(len(s) - 1, max(0, round(p / 100 * (len(s) - 1))))
    return s[k]


def summarize(frames, errors, first_rx_epoch):
    st = {"frames": len(frames), "errors": errors, "first_frame_epoch": first_rx_epoch}
    if len(frames) < 2:
        return st

    seqs = [f["seq"] for f in frames]
    lost = 0
    out_of_order = 0
    for a, b in zip(seqs, seqs[1:]):
        gap = (b - a) & 0xFFFFFFFF
        if gap == 0 or gap > 0x7FFFFFFF:
            out_of_order += 1
        else:
            lost += gap - 1

    ts = [f["t_sample_ns"] for f in frames]
    intervals_us = [(b - a) / 1e3 for a, b in zip(ts, ts[1:]) if b > a]
    lat_us = [(f["t_send_ns"] - f["t_sample_ns"]) / 1e3 for f in frames]
    span_s = (ts[-1] - ts[0]) / 1e9

    st.update(
        lost=lost,
        out_of_order=out_of_order,
        span_s=round(span_s, 3),
        rate_hz=round((len(frames) - 1) / span_s, 2) if span_s > 0 else None,
        interval_us={
            "mean": round(statistics.mean(intervals_us), 1),
            "stdev": round(statistics.pstdev(intervals_us), 1),
            "min": round(min(intervals_us), 1),
            "max": round(max(intervals_us), 1),
        },
        latency_us={
            "p50": round(percentile(lat_us, 50), 1),
            "p90": round(percentile(lat_us, 90), 1),
            "p99": round(percentile(lat_us, 99), 1),
            "max": round(max(lat_us), 1),
        },
    )
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--capture", type=float, default=30.0)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--out", type=Path, default=Path("results"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.settimeout(0.5)

    frames, errors = [], {}
    start = time.monotonic()
    first_rx = None
    first_rx_epoch = None

    while True:
        now = time.monotonic()
        if first_rx is None and now - start > args.timeout:
            break
        if first_rx is not None and now - first_rx > args.capture:
            break
        try:
            pkt, _ = sock.recvfrom(256)
        except socket.timeout:
            continue
        rx_ns = time.monotonic_ns()
        res = parse(pkt)
        if isinstance(res, str):
            errors[res] = errors.get(res, 0) + 1
            continue
        if first_rx is None:
            first_rx = time.monotonic()
            first_rx_epoch = time.time()
            print(f"telem_rx: first frame after {first_rx - start:.1f}s", flush=True)
        res["rx_ns"] = rx_ns
        frames.append(res)

    with open(args.out / "frames.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["seq", "t_sample_ns", "t_send_ns", "rx_ns",
                    "ax_mg", "ay_mg", "az_mg", "gx_mdps", "gy_mdps", "gz_mdps"])
        for f in frames:
            w.writerow([f["seq"], f["t_sample_ns"], f["t_send_ns"], f["rx_ns"],
                        *f["accel_mg"], *f["gyro_mdps"]])

    stats = summarize(frames, errors, first_rx_epoch)
    (args.out / "stats.json").write_text(json.dumps(stats, indent=2))
    print(json.dumps(stats, indent=2))
    return 0 if frames else 1


if __name__ == "__main__":
    sys.exit(main())
