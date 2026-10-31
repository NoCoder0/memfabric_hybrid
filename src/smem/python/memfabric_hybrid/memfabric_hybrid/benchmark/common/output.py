# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Render the benchmark configuration and the per-direction bandwidth matrix."""

import sys


def format_size(size):
    for suffix, unit in (("G", 1024**3), ("M", 1024**2), ("K", 1024)):
        if size >= unit and size % unit == 0:
            return f"{size // unit}{suffix}"
    return f"{size}B"


def print_table(headers, rows):
    rows = [tuple(str(value) for value in row) for row in (headers, *rows)]
    widths = [max(len(value) for value in column) for column in zip(*rows)]
    border = "+" + "+".join("-" * (width + 2) for width in widths) + "+"
    print(border)
    for index, row in enumerate(rows):
        print("| " + " | ".join(f"{value:<{width}}" for value, width in zip(row, widths)) + " |")
        if index == 0:
            print(border)
    print(border)


def print_peer_hint(host):
    print(
        f"[benchmark] Waiting for {host}. If you have not started it yet, run the same command on {host}.", flush=True
    )


def print_start(params):
    sizes = params.block_sizes
    block = format_size(sizes[0]) if len(sizes) == 1 else f"{', '.join(format_size(size) for size in sizes)}"
    if params.batch_size is None:
        batch = f"auto ({format_size(params.transfer_size)} target per copy, at least 1 block)"
    else:
        batch = params.batch_size
    if params.group < 0:
        directions = ("Directions", ", ".join(phase.name for phase in params.phases))
    else:
        # Each cross-machine command runs only the phases its side initiates.
        local = [phase.name for phase in params.phases if phase.initiator_group == params.group]
        directions = ("Directions (initiated here)", ", ".join(local) or "none")
    fields = [
        ("Protocol", params.protocol),
        directions,
        ("NPU cards (physical)", ", ".join(str(device) for device in params.devices)),
    ]
    if params.idle_devices:
        idle = ", ".join(str(device) for device in params.idle_devices)
        fields.append(("Unused NPU cards", f"{idle} (pairs need an even card count on one machine)"))
    fields += [
        ("Config store", params.store_url),
        ("Local NIC", params.nic),
        ("Block size", block),
        ("Batch size", batch),
        ("Copy count", params.copy_count),
        ("Warmup count", params.warmup_count),
    ]
    if params.local_hbm_size:
        fields.append(("HBM per rank", format_size(params.local_hbm_size)))
    if params.backend == "trans":
        fields = [(label, value) for label, value in fields if label != "Config store"]
        fields.extend(
            [
                ("Sync store", f"tcp://{params.sync_host}:{params.sync_port}"),
                ("Local control IP", params.local_ip),
            ]
        )
    print("\n" + "=" * 88, flush=True)
    print(f"[benchmark] Starting {params.backend.upper()} benchmark", flush=True)
    for label, value in fields:
        print(f"  {label}: {value}", flush=True)
    resource = "memory pools" if params.backend == "bm" else "registered buffers"
    print(f"[benchmark] Initializing workers, {resource} and connections...", flush=True)
    print("=" * 88, flush=True)


def _average_gbps(rows):
    return sum(row["bandwidth_gbps"] for row in rows) / len(rows)


def _matrix_rows(results, params, names):
    rows = []
    for size in params.block_sizes:
        values = []
        for name in names:
            group = [row for row in results if row["copy_size"] == size and row["direction"] == name]
            values.append(f"{_average_gbps(group):.3f}" if group else "-")
        rows.append((format_size(size), *values))
    return rows


def print_matrix(results, params):
    """One column per direction in plan order and one row per block size."""
    names = [phase.name for phase in params.phases if any(row["direction"] == phase.name for row in results)]
    if not names:
        print("[benchmark] PASS: this command initiated no copies; the machine that owns --ip prints the results.")
        sys.stdout.flush()
        return
    print()
    print_table(("Block", *(f"{name}(GB/s)" for name in names)), _matrix_rows(results, params, names))
    if params.log:
        _print_details(results, names)
    sys.stdout.flush()


def _print_details(results, names):
    order = {name: index for index, name in enumerate(names)}
    rows = sorted(results, key=lambda row: (order[row["direction"]], row["copy_size"], row["rank_id"]))
    print("\nPer-rank details")
    print_table(
        ("Direction", "NPU", "Rank", "Peer", "Block", "CopyCount", "Time(us)", "BW(GB/s)"),
        [
            (
                row["direction"],
                row["device_id"],
                row["rank_id"],
                row["peer_rank_id"],
                format_size(row["copy_size"]),
                row["copy_count"],
                f"{row['total_time_us']:.1f}",
                f"{row['bandwidth_gbps']:.3f}",
            )
            for row in rows
        ],
    )
