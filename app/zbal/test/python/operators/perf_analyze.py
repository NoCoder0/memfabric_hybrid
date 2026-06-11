#!/usr/bin/env python3
"""Collect perf data incrementally and generate final report."""

import logging
import os
import sys
import csv
import json

CURRENT_DIR = os.path.dirname(os.path.abspath(__file__))
PERF_DIR = os.path.join(CURRENT_DIR, "test_output")
DATA_FILE = os.path.join(PERF_DIR, "results.json")

logger = logging.getLogger(__name__)

PERF_KEYWORDS = {
    "allgather": ("ZBALAllGatherInner", "hcom_allGather__"),
    "allreduce": ("ZBALAllReduceInner", "hcom_allReduce__"),
    "alltoallv": ("ZBALAlltoAllVInner", "hcom_alltoallv__"),
    "broadcast": ("ZBALBroadcastInner", "hcom_broad"),
    "gather": ("ZBALBroadcastInner", "hcom_broadcast__"),
    "p2p": (("ZBALSendInner", "ZBALRecvInner"), ("hcom_send__", "hcom_receive__")),
    "reducescatter": ("ZBALReduceScatterInner", "hcom_reduceScatter__"),
    "scatter": ("ZBALScatterInner", "hcom_scatter"),
}


FLOAT_OPS = {"allreduce", "reducescatter"}


def format_size(byte_size):
    """Convert byte size to human-readable string (K/M/G)."""
    if byte_size >= 1024 * 1024 * 1024:
        return f"{byte_size / (1024**3):.0f}G"
    elif byte_size >= 1024 * 1024:
        val = byte_size / (1024**2)
        return f"{val:.0f}M" if val == int(val) else f"{val:.1f}M"
    elif byte_size >= 1024:
        return f"{byte_size // 1024}K"
    else:
        return str(byte_size)


def discover_columns(data, backend):
    """Discover all unique case sizes collected for a given backend, sorted numerically."""
    sizes = set()
    for op_data in data.values():
        for key in op_data.get(backend, {}):
            sizes.add(int(key))
    return sorted(sizes)


def parse_csv(filepath, keyword):
    """Parse kernel_details.csv, return avg time (us) or None."""
    values = []
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            for row in csv.reader(f):
                if len(row) >= 11 and keyword in row[5]:
                    try:
                        values.append(float(row[10]))
                    except ValueError:
                        pass
    except Exception:
        return None
    if not values:
        return None
    return sum(values) / len(values)


def cmd_collect(op_name, csv_path):
    """Collect timing from a single profiling CSV and append to results file."""
    os.makedirs(PERF_DIR, exist_ok=True)

    # Load existing data
    data = {}
    if os.path.exists(DATA_FILE):
        with open(DATA_FILE, 'r') as f:
            data = json.load(f)

    # Parse CSV for both zbal and hccl keywords
    zbal_kw, hccl_kw = PERF_KEYWORDS[op_name]
    for backend, kws in [("zbal", zbal_kw), ("hccl", hccl_kw)]:
        if isinstance(kws, str):
            kws = (kws,)
        values = []
        for kw in kws:
            v = parse_csv(csv_path, kw)
            if v is not None:
                values.append(v)
        if values:
            data.setdefault(op_name, {}).setdefault(backend, {})
            data[op_name][backend]["_last"] = sum(values) / len(values)

    with open(DATA_FILE, 'w') as f:
        json.dump(data, f, indent=2)


def cmd_collect_final(op_name, case_size):
    # Record the final timing value for a completed case.
    # Key is byte_size so bf16 and float operators share same columns.
    os.makedirs(PERF_DIR, exist_ok=True)
    if not os.path.exists(DATA_FILE):
        return
    with open(DATA_FILE, 'r') as f:
        data = json.load(f)

    bpe = 4 if op_name in FLOAT_OPS else 2
    byte_size = case_size * bpe
    op_data = data.get(op_name, {})
    for backend in ("zbal", "hccl"):
        if "_last" in op_data.get(backend, {}):
            data[op_name][backend][str(byte_size)] = data[op_name][backend].pop("_last")

    with open(DATA_FILE, 'w') as f:
        json.dump(data, f, indent=2)


def write_md_table(out, rows, columns, data, title, backend):
    """Write a markdown table with aligned columns."""
    # Build all rows first to compute column widths
    col_headers = ["Operator"] + [format_size(int(c)) for c in columns]
    table_rows = [col_headers]
    for op_name in rows:
        cells = [op_name]
        for c in columns:
            val = data.get(op_name, {}).get(backend, {}).get(str(c))
            cells.append(f"{val:.2f}" if val else "N/A")
        table_rows.append(cells)

    # Compute max width per column
    ncols = len(col_headers)
    widths = [max(len(row[i]) for row in table_rows) for i in range(ncols)]

    def fmt_row(row):
        parts = [
            row[0].ljust(widths[0]) if i == 0 else row[i].rjust(widths[i])
            for i in range(ncols)
        ]
        return "| " + " | ".join(parts) + " |"

    sep = "|" + "|".join(
        " " + "-" * w + " " for w in widths
    ) + "|"

    out.write(f"### {title}\n\n")
    out.write(fmt_row(col_headers) + "\n")
    out.write(sep + "\n")
    for row in table_rows[1:]:
        out.write(fmt_row(row) + "\n")
    out.write("\n")


ROWS = sorted(PERF_KEYWORDS)


def has_backend_data(data, backend):
    """Check if any operator has collected data for a given backend."""
    for op_data in data.values():
        if op_data.get(backend):
            return True
    return False


def cmd_show():
    """Print collected data (for debugging partial results), does NOT delete the data file."""
    if not os.path.exists(DATA_FILE):
        logging.warning("No collected perf data found in %s", DATA_FILE)
        return
    with open(DATA_FILE, 'r') as f:
        data = json.load(f)
    if not data:
        logging.warning("No collected perf data found in %s", DATA_FILE)
        return

    columns = set()
    for op_data in data.values():
        for backend in ("zbal", "hccl"):
            for key in op_data.get(backend, {}):
                if key != "_last":
                    columns.add(int(key))
    columns = sorted(columns)

    col_labels = [format_size(c) for c in columns] if columns else ["_last"]
    op_width = max(max(len(op) for op in data), 8)
    col_widths = [max(len(lbl) + 1, 9) for lbl in col_labels]

    out = []
    out.append("")
    out.append("=== Collected Perf Data (partial) ===")

    header_line = " " * (op_width + 1) + "|"
    for i, lbl in enumerate(col_labels):
        header_line += " " + lbl.rjust(col_widths[i] - 1)
    out.append(header_line)
    out.append("-" * len(header_line))

    for op in sorted(data):
        op_data = data[op]
        for backend in ("zbal", "hccl"):
            label = op if backend == "zbal" else ""
            cells = [label.ljust(op_width)]
            for i, c in enumerate(columns if columns else ["_last"]):
                val = op_data.get(backend, {}).get(str(c))
                cell = f"{val:.2f}" if val else "-"
                cells.append(cell.rjust(col_widths[i] - 1))
            out.append(" " + " |".join(cells))
        out.append("")

    out.append("=" * 40)
    logger.info("\n".join(out))


def cmd_report(world_size=None):
    """Generate final report from accumulated data."""
    data = {}
    if os.path.exists(DATA_FILE):
        with open(DATA_FILE, 'r') as f:
            data = json.load(f)

    if not data:
        logging.warning("No profiling data collected. "
                        "Ensure ENABLE_PROFILING=1 and kernel_details.csv exist.")
    else:
        ws_suffix = f" world_size={world_size}" if world_size else ""
        ops_with_data = [op for op in ROWS if op in data]
        for backend, title in [("zbal", "ZBAL (us)"), ("hccl", "HCCL (us)")]:
            if not has_backend_data(data, backend):
                continue
            columns = discover_columns(data, backend)
            write_md_table(sys.stdout, ops_with_data, columns, data, f"{title}{ws_suffix}", backend)

    # Clean up data file
    if os.path.exists(DATA_FILE):
        os.remove(DATA_FILE)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "report"
    if cmd == "collect":
        cmd_collect(sys.argv[2], sys.argv[3])
    elif cmd == "collect_final":
        cmd_collect_final(sys.argv[2], int(sys.argv[3]))
    elif cmd == "show":
        cmd_show()
    else:
        ws = sys.argv[2] if len(sys.argv) > 2 else None
        cmd_report(ws)
