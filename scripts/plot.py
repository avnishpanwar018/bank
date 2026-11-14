#!/usr/bin/env python3
"""Create small dependency-free PNG plots from Phase 6 CSV files."""

import csv
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"


def rows(name):
    with (RESULTS / name).open(newline="") as handle:
        return list(csv.DictReader(handle))


def grouped_mean(name, key, value):
    values = {}
    for row in rows(name):
        try:
            values.setdefault(row[key], []).append(float(row[value]))
        except (KeyError, ValueError):
            continue
    return [(label, sum(items) / len(items)) for label, items in sorted(values.items()) if items]


def png(path, series):
    width, height = 800, 500
    pixels = [[(255, 255, 255) for _ in range(width)] for _ in range(height)]

    def point(x, y, color):
        if 0 <= x < width and 0 <= y < height:
            pixels[y][x] = color

    colors = [(31, 119, 180), (214, 39, 40), (44, 160, 44), (148, 103, 189)]
    all_values = [value for points in series for _, value in points]
    if not all_values:
        all_values = [0.0, 1.0]
    minimum, maximum = min(all_values), max(all_values)
    if minimum == maximum:
        maximum = minimum + 1.0
    left, right, top, bottom = 60, width - 30, 30, height - 50
    for x in range(left, right + 1):
        point(x, bottom, (180, 180, 180))
    for y in range(top, bottom + 1):
        point(left, y, (180, 180, 180))
    for index, points in enumerate(series):
        if not points:
            continue
        color = colors[index % len(colors)]
        count = max(1, len(points) - 1)
        previous = None
        for position, (_, value) in enumerate(points):
            x = left + int((right - left) * position / count)
            y = bottom - int((bottom - top) * (value - minimum) / (maximum - minimum))
            if previous is not None:
                old_x, old_y = previous
                steps = max(abs(x - old_x), abs(y - old_y), 1)
                for step in range(steps + 1):
                    point(old_x + (x - old_x) * step // steps,
                          old_y + (y - old_y) * step // steps, color)
            point(x, y, color)
            point(x + 1, y, color)
            point(x, y + 1, color)
            previous = (x, y)
    raw = b"".join(b"\x00" + bytes(sum(row, ())) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    path.write_bytes(data)


def line_plot(name, x_key, y_key, output, title, xlabel, ylabel):
    points = grouped_mean(name, x_key, y_key)
    png(RESULTS / output, [points])


line_plot("throughput_workers.csv", "workers", "throughput_tx_s",
           "throughput_vs_workers.png", "Throughput vs workers", "Workers", "tx/s")
line_plot("throughput_workers.csv", "workers", "avg_latency_ms",
           "latency_vs_workers.png", "Latency vs workers", "Workers", "Average latency (ms)")
line_plot("isolation.csv", "isolation", "throughput_tx_s",
           "isolation_comparison.png", "Isolation comparison", "Isolation", "tx/s")
line_plot("safe_vs_unsafe.csv", "mode", "balance_error",
           "safe_vs_unsafe_balance_error.png", "Safe vs unsafe balance error", "Mode", "Balance error")
line_plot("lock_ordering.csv", "ordering", "deadlock_or_retry_rate",
           "retry_rate_lock_ordering.png", "Retry rate with/without lock ordering", "Ordering", "Retry rate")
