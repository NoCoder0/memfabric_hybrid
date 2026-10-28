#!/usr/bin/env python3
# -*- coding:utf-8 -*-
"""
Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
MemFabric_Hybrid is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details.

Print the hourly batch_read / batch_write statistics of a ptracer dump file.
"""

import datetime
import logging
import os
import re
import statistics
from collections import defaultdict, namedtuple

LOGGER = logging.getLogger(__name__)

# Report item name -> latency tracepoint; its byte counter is "<name>_SIZE".
ITEMS = (
    ("batch_read", "TP_TRANS_BATCH_READ"),
    ("batch_write", "TP_TRANS_BATCH_WRITE"),
)
SIZE_SUFFIX = "_SIZE"
ITEM_BY_TRACEPOINT = {latency: item for item, latency in ITEMS}
# Fallback dump period, used only when the file holds a single snapshot.
DEFAULT_INTERVAL_SEC = 10.0
WINDOW_SECONDS = 3600
TIME_FMT = "%Y-%m-%d %H:%M:%S"
TIME_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")
Row = namedtuple("Row", "time name total")
HEADER = ("WINDOW", "TRACEPOINT", "SIZE", "ELAPSED", "AVG BW", "MIN BW", "MAX BW")
ROW_FORMAT = "{{:<24}}  {{:<{w}}}  {{:>10}}  {{:>10}}  {{:>11}}  {{:>11}}  {{:>11}}"


def _parse_time(tstr):
    return datetime.datetime.strptime(tstr, TIME_FMT)


def parse_dump_file(path):
    """Parse one dump file; truncated or malformed rows are skipped."""
    rows = []
    columns = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            tokens = raw.split()
            if not tokens:
                continue
            if tokens[0] == "TIME":
                columns = tokens[2:]  # skip TIME NAME (TIME holds a space)
                continue
            if columns is None or TIME_RE.match(raw) is None or len(tokens) < 3 or tokens[1].count(":") != 2:
                continue
            try:
                numbers = [float(token) for token in tokens[3:]]
                total = numbers[columns.index("TOTAL(us)")]
            except (ValueError, IndexError):
                continue
            rows.append(Row("{} {}".format(tokens[0], tokens[1]), tokens[2], total))
    return rows


def measure_period(stamps):
    """Median gap between snapshot stamps, in seconds."""
    parsed = sorted({_parse_time(stamp) for stamp in stamps})
    gaps = [(right - left).total_seconds() for left, right in zip(parsed, parsed[1:]) if right > left]
    return statistics.median(gaps) if gaps else DEFAULT_INTERVAL_SEC


def period_bounds(snapshots, period):
    """Map each stamp to the [start, end) interval its row measures."""
    bounds = {}
    previous = None
    for stamp in sorted(snapshots):
        moment = _parse_time(stamp)
        if previous is not None and (moment - previous).total_seconds() <= period * 1.5:
            start = previous
        else:
            start = moment - datetime.timedelta(seconds=period)
        bounds[stamp] = (start, moment)
        previous = moment
    return bounds


def collect_items(rows):
    """Join each item's byte counter and latency rows on their snapshot stamp."""
    points = {item: defaultdict(lambda: [0.0, 0.0]) for item, _ in ITEMS}
    for row in rows:
        if row.name.endswith(SIZE_SUFFIX):
            item, slot = ITEM_BY_TRACEPOINT.get(row.name[: -len(SIZE_SUFFIX)]), 0
        else:
            item, slot = ITEM_BY_TRACEPOINT.get(row.name), 1
        if item is not None:
            points[item][row.time][slot] += row.total
    return {item: dict(points[item]) for item, _ in ITEMS if points[item]}


def build_windows(items, bounds):
    """Group periods into clock hours and reduce each group to one table row."""
    buckets = defaultdict(lambda: defaultdict(list))
    for item, points in items.items():
        for stamp, (nbytes, elapsed_us) in points.items():
            start, end = bounds[stamp]
            hour = (start + (end - start) / 2).replace(minute=0, second=0, microsecond=0)
            buckets[hour][item].append((stamp, nbytes, elapsed_us))

    windows = []
    for hour in sorted(buckets):
        for item, _ in ITEMS:
            group = [point for point in sorted(buckets[hour].get(item, ())) if point[1] > 0]
            if not group:
                continue
            rates = [nbytes / elapsed_us for _, nbytes, elapsed_us in group if elapsed_us > 0]
            windows.append(
                {
                    "window": hour,
                    "item": item,
                    "bytes": sum(point[1] for point in group),
                    "elapsed_us": sum(point[2] for point in group),
                    "mean_mbps": statistics.mean(rates) if rates else 0.0,
                    "min_mbps": min(rates) if rates else 0.0,
                    "max_mbps": max(rates) if rates else 0.0,
                    "start": bounds[group[0][0]][0],
                    "end": group[-1][0],
                }
            )
    return windows


def human_bytes(num):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(num) < 1024.0:
            return "{:.2f} {}".format(num, unit)
        num /= 1024.0
    return "{:.2f} PiB".format(num)


def human_rate(mbps):
    """Format a MB/s rate, stepping to GB/s or KB/s for readability."""
    if mbps <= 0:
        return "--"
    if mbps >= 1000.0:
        return "{:.2f} GB/s".format(mbps / 1000.0)
    if mbps >= 1.0:
        return "{:.2f} MB/s".format(mbps)
    return "{:.2f} KB/s".format(mbps * 1000.0)


def human_seconds(us):
    """Format an accumulated time, stepping us -> ms -> s -> min -> h."""
    if us <= 0:
        return "0"
    if us < 1000.0:
        return "{:.0f} us".format(us)
    if us < 1000000.0:
        return "{:.2f} ms".format(us / 1000.0)
    seconds = us / 1000000.0
    if seconds < 60.0:
        return "{:.2f} s".format(seconds)
    if seconds < 3600.0:
        return "{:.1f} min".format(seconds / 60.0)
    return "{:.2f} h".format(seconds / 3600.0)


def window_label(hour):
    """The clock-hour range a row covers."""
    end = hour + datetime.timedelta(seconds=WINDOW_SECONDS)
    return "{:%Y-%m-%d %H:%M}-{:%H:%M}".format(hour, end)


def print_header(path, snapshots, period, windows):
    """Print the file, the measured period and the traffic window."""
    start = min(window["start"] for window in windows)
    end = max(window["end"] for window in windows)
    print("file    : {}".format(path))
    print("source  : 1 file(s), {} snapshot(s), period {:.1f}s".format(snapshots, period))
    print("traffic : {} -> {}".format(start.strftime(TIME_FMT), end))
    print("window  : {}s per row, starting on the whole hour".format(WINDOW_SECONDS))


def print_table(windows):
    """Print one line per (clock hour, item)."""
    width = max([len("TRACEPOINT")] + [len(window["item"]) for window in windows])
    row = ROW_FORMAT.format(w=width)
    print(row.format(*HEADER))
    print("-" * len(row.format(*HEADER)))
    for window in windows:
        print(
            row.format(
                window_label(window["window"]),
                window["item"],
                human_bytes(window["bytes"]),
                human_seconds(window["elapsed_us"]),
                human_rate(window["mean_mbps"]),
                human_rate(window["min_mbps"]),
                human_rate(window["max_mbps"]),
            )
        )


def add_arguments(parser):
    """Register the `mfcli trace` arguments on the given parser."""
    parser.add_argument(
        "-f",
        "--file",
        required=True,
        metavar="FILE",
        help="ptracer dump file to parse, e.g. /var/log/memfabric_hybrid/ptracer_1234.dat",
    )


def report(args):
    """Print the hourly table; returns the process exit code."""
    path = args.file
    if not os.path.isfile(path):
        LOGGER.error("ptracer dump file not found: %s; enable ENABLE_PTRACER and run a transfer workload first", path)
        return 1

    rows = parse_dump_file(path)
    if not rows:
        LOGGER.error("no tracepoint row parsed from %s (the dump must hold a TOTAL(us) column)", path)
        return 1

    items = collect_items(rows)
    if not items:
        present = {row.name for row in rows}
        expected = [name for _, name in ITEMS] + [name + SIZE_SUFFIX for _, name in ITEMS]
        LOGGER.error(
            "no batch_read/batch_write tracepoint in %s; missing %s",
            path,
            ", ".join(name for name in expected if name not in present),
        )
        return 1

    snapshots = {row.time for row in rows}
    period = measure_period(snapshots)
    windows = build_windows(items, period_bounds(snapshots, period))
    if not windows:
        LOGGER.error("no batch_read/batch_write traffic in %s (every period reported 0 bytes)", path)
        return 1

    print_header(path, len(snapshots), period, windows)
    print("")
    print_table(windows)
    return 0
