#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Summarize one host's MF trace. Never subtract clocks from different hosts."""
import argparse
import bisect
import collections
import json
import math
import statistics
import sys


def read_records(path):
    records = []
    with open(path, encoding="utf-8") as source:
        for line in source:
            if line.startswith("{"):
                records.append(json.loads(line))
    return records


def match_completions(events):
    """Use POST_BEGIN, not POST_END: a CQE can arrive before post_send returns.

    A wr_id can be reused after dispatch. Match by QP + wr_id and ordered time;
    capture_round on a CQ event describes its observation window, not ownership.
    """
    posts = collections.defaultdict(list)
    completions = collections.defaultdict(list)
    for e in events:
        key = (e["qp_num"], e["wr_id"])
        if e["event"] == "verbs_post_begin":
            posts[key].append(e)
        elif e["event"] == "cqe_observed":
            completions[key].append(e)
    matches, missing = [], []
    for key, submissions in posts.items():
        submissions.sort(key=lambda e: e["timestamp_ns"])
        cqes = sorted(completions[key], key=lambda e: e["timestamp_ns"])
        timestamps = [e["timestamp_ns"] for e in submissions]
        used = set()
        for cqe in cqes:
            index = bisect.bisect_right(timestamps, cqe["timestamp_ns"]) - 1
            if index >= 0 and index not in used:
                used.add(index)
                matches.append((submissions[index], cqe))
        missing.extend(e for i, e in enumerate(submissions) if i not in used)
    return matches, missing


def delta_us(end, begin):
    return round((end - begin) / 1000, 3)


def stats_us(values):
    """Nanoseconds in; nearest-rank percentiles in microseconds out."""
    values = sorted(values)
    if not values:
        return None
    return {"n": len(values), "min": round(values[0] / 1000, 3),
            "p50": round(values[math.ceil(len(values) * 0.5) - 1] / 1000, 3),
            "p95": round(values[math.ceil(len(values) * 0.95) - 1] / 1000, 3),
            "max": round(values[-1] / 1000, 3)}


def post_call_stats(events):
    # Both records are emitted together by their submitting thread; group by
    # producer too because a completed wr_id can be reused by another producer.
    groups = collections.defaultdict(lambda: collections.defaultdict(list))
    for e in events:
        if e["event"] in ("verbs_post_begin", "verbs_post_end") and e["transfer_kind"] == "data":
            groups[(e["thread_slot"], e["qp_num"], e["wr_id"])][e["event"]].append(e)
    calls = set()
    for (thread, _, _), group in groups.items():
        begins = sorted(group["verbs_post_begin"], key=lambda e: e["timestamp_ns"])
        ends = sorted(group["verbs_post_end"], key=lambda e: e["timestamp_ns"])
        if len(begins) != len(ends):
            continue
        for begin, end in zip(begins, ends):
            # One post_send may submit several WRs with identical timestamps.
            calls.add((thread, begin["timestamp_ns"], end["timestamp_ns"]))
    return stats_us([end - begin for _, begin, end in calls])


def completion_stats(events, pairs):
    batches = {(c["thread_slot"], c["cq_id"], c["batch_id"]) for _, c in pairs}
    polls = [e for e in events if e["event"] == "cq_poll_batch" and
             (e["thread_slot"], e["cq_id"], e["batch_id"]) in batches]
    times = sorted(c["timestamp_ns"] for _, c in pairs)
    return {
        "successful_data_poll_us": stats_us([e["timestamp_ns"] - e["poll_begin_ns"] for e in polls]),
        "cqes_per_data_poll": dict(sorted(collections.Counter(e["count"] for e in polls).items())),
        "reported_empty_polls": sum(e.get("empty_polls", 0) for e in polls),
        "max_reported_poll_gap_us": max((e.get("max_poll_gap_ns", 0) for e in polls), default=0) / 1000,
        "max_reported_poll_call_us": max((e.get("max_poll_call_ns", 0) for e in polls), default=0) / 1000,
        "data_cqe_interval_us": stats_us([b - a for a, b in zip(times, times[1:])]),
        "data_post_to_cqe_us": stats_us([c["timestamp_ns"] - p["timestamp_ns"] for p, c in pairs]),
    }


def compact_remote(row, events, app, pairs, expected_bytes, poll_events):
    data = [e for e in events if e["event"] == "verbs_post_begin" and e["transfer_kind"] == "data"]
    times = {e["event"]: e["timestamp_ns"] for e in app}
    curve = row.pop("data_completion_curve", [])
    row["completion_metrics_valid"] = bool(data) and len(pairs) == len(data) and row["data_bytes"] == expected_bytes
    if data:
        first = min(e["timestamp_ns"] for e in data)
        row["request_to_first_data_post_us"] = delta_us(first, times["request_observed"])
        row["data_post_begin_span_us"] = delta_us(max(e["timestamp_ns"] for e in data), first)
        if "copy_batch_end" in times:
            row["first_data_post_to_batch_return_us"] = delta_us(times["copy_batch_end"], first)
    if "request_decoded" in times:
        row["request_decode_us"] = delta_us(times["request_decoded"], times["request_observed"])
    if "copy_batch_begin" in times and "copy_batch_end" in times:
        row["copy_batch_us"] = delta_us(times["copy_batch_end"], times["copy_batch_begin"])
    row["data_post_call_us"] = post_call_stats(events)
    if not row["completion_metrics_valid"]:
        for key in ("first_data_post_to_last_cqe_us", "first_to_last_data_cqe_us", "successful_data_poll_median_us"):
            row.pop(key, None)
        return
    row.update(completion_stats(poll_events, pairs))
    row["data_completed_pct_since_first_post_us"] = {
        str(pct): next((point["since_first_post_us"] for point in curve
                        if point["bytes"] * 100 >= expected_bytes * pct), None)
        for pct in (10, 25, 50, 75, 100)}
    ends = [e["timestamp_ns"] for e in events if e["event"] == "verbs_post_end" and e["transfer_kind"] == "data"]
    if ends:
        # May be negative if completion is observed before the last call returns.
        row["last_data_post_end_to_last_cqe_us"] = delta_us(max(c["timestamp_ns"] for _, c in pairs), max(ends))


def check_export(records, events):
    summary = next(e for e in records if e.get("record_type") == "mf_trace_summary")
    counts = collections.Counter(e["event"] for e in events)
    checks = {"collector_status": summary["status"], "collector_dropped": summary.get("dropped"),
              "expected_post_end": summary.get("post_records"), "actual_post_end": counts["verbs_post_end"],
              "expected_cqe": summary.get("cqe_records"), "actual_cqe": counts["cqe_observed"]}
    complete = summary["status"] == "ok" and summary.get("dropped", 0) == 0
    complete &= summary.get("clock_errors", 0) == 0 and summary.get("event_errors", 0) == 0
    for name, observed in (("post_records", counts["verbs_post_end"]), ("cqe_records", counts["cqe_observed"])):
        complete &= summary.get(name) == observed
    complete &= counts["verbs_post_begin"] == counts["verbs_post_end"]
    complete &= counts["cq_dispatch_begin"] == counts["cq_dispatch_end"] == counts["cqe_observed"]
    polls = {(e["thread_slot"], e["cq_id"], e["batch_id"]) for e in events if e["event"] == "cq_poll_batch"}
    complete &= all((e["thread_slot"], e["cq_id"], e["batch_id"]) in polls
                    for e in events if e["event"] == "cqe_observed")
    checks["record_counts_match"] = bool(complete)
    return checks


def compact_summary(records):
    result = summarize(records)
    config = next(e for e in records if e.get("record_type") == "mf_trace_config")
    events = [e for e in records if e.get("record_type") == "hcom_trace"]
    apps = [e for e in records if e.get("record_type") == "mf_app_trace"]
    matches, _ = match_completions(events)
    poll_records = [e for e in events if e["event"] == "cq_poll_batch"]
    result["format"] = "mf-compact-v1"
    result["config"] = config
    result["library"] = next((e.get("path") for e in records if e.get("record_type") == "mf_trace_library"), None)
    result["integrity"] = check_export(records, events)
    result["integrity"]["missing_app_events"] = {}
    if not result["integrity"]["record_counts_match"]:
        result["status"] = "incomplete"
    for row in result["rounds"]:
        per_round = [e for e in events if e["capture_round"] == row["round"]]
        app = [e for e in apps if e["capture_round"] == row["round"]]
        required = ({"request_observed", "request_decoded", "copy_batch_begin", "copy_batch_end"}
                    if config["host_role"] == "remote" else {"local_round_begin", "local_round_end"})
        missing_app = sorted(required - {e["event"] for e in app})
        if missing_app:
            result["status"] = "incomplete"
            result["integrity"]["missing_app_events"][str(row["round"])] = missing_app
        if config["host_role"] == "remote":
            pairs = [(p, c) for p, c in matches if p["capture_round"] == row["round"] and p["transfer_kind"] == "data"]
            # Poll records can belong to a later observation window than the POST.
            compact_remote(row, per_round, app, pairs, config["count"] * config["size"], poll_records)
        else:
            # Bound output even for chunk=1; the complete per-group trace stays on the device.
            groups = row["ready_groups"]
            row["ready_group_count"] = len(groups)
            if len(groups) > 8:
                row["ready_groups"] = groups[:4] + groups[-4:]
                row["omitted_ready_groups"] = len(groups) - 8
    return result


def summarize(records):
    configs = [r for r in records if r.get("record_type") == "mf_trace_config"]
    summaries = [r for r in records if r.get("record_type") == "mf_trace_summary"]
    if len(configs) != 1 or len(summaries) != 1:
        raise ValueError("expected one complete run from one host, with one config and one summary")
    config, summary = configs[0], summaries[0]
    events = [r for r in records if r.get("record_type") == "hcom_trace"]
    apps = [r for r in records if r.get("record_type") == "mf_app_trace"]
    if any(r.get("host_role", config["host_role"]) != config["host_role"] for r in events + apps):
        raise ValueError("mixed host roles: analyze local and remote files separately")
    matches, missing = match_completions(events)
    rounds = sorted({r["capture_round"] for r in apps})
    valid = summary["status"] == "ok" and not missing and len(rounds) == config["rounds"]
    results = []
    for round_id in rounds:
        app = sorted((e for e in apps if e["capture_round"] == round_id), key=lambda e: e["timestamp_ns"])
        times = {e["event"]: e["timestamp_ns"] for e in app}
        posts = [e for e in events if e["event"] == "verbs_post_begin" and e["capture_round"] == round_id]
        data = [e for e in posts if e["transfer_kind"] == "data"]
        row = {"round": round_id, "host_role": config["host_role"],
               "wr_counts": dict(collections.Counter(e["transfer_kind"] for e in posts))}
        if config["host_role"] == "remote":
            pairs = [(p, c) for p, c in matches if p["capture_round"] == round_id and p["transfer_kind"] == "data"]
            row["data_wr_shapes"] = dict(collections.Counter(f'{e["sge_count"]}SGE/{e["bytes"]}B' for e in data))
            row["data_bytes"] = sum(e["bytes"] for e in data)
            row["data_cqes"] = len(pairs)
            valid &= bool(data) and len(pairs) == len(data) and row["data_bytes"] == config["count"] * config["size"]
            if data and pairs:
                first_post = min(e["timestamp_ns"] for e in data)
                first_cq = min(c["timestamp_ns"] for _, c in pairs)
                last_cq = max(c["timestamp_ns"] for _, c in pairs)
                row["request_to_first_data_post_us"] = delta_us(first_post, times["request_observed"])
                row["first_data_post_to_last_cqe_us"] = delta_us(last_cq, first_post)
                row["first_to_last_data_cqe_us"] = delta_us(last_cq, first_cq)
                row["data_completion_curve"] = []
                completed = 0
                for post, cqe in sorted(pairs, key=lambda pair: pair[1]["timestamp_ns"]):
                    completed += post["bytes"]
                    row["data_completion_curve"].append({
                        "since_first_post_us": delta_us(cqe["timestamp_ns"], first_post), "bytes": completed})
                # Include only the poll batches that actually returned data CQEs.
                batch_ids = {(c["thread_slot"], c["cq_id"], c["batch_id"]) for _, c in pairs}
                polls = [e for e in events if e["event"] == "cq_poll_batch" and
                         (e["thread_slot"], e["cq_id"], e["batch_id"]) in batch_ids]
                row["successful_data_poll_median_us"] = statistics.median(
                    delta_us(e["timestamp_ns"], e["poll_begin_ns"]) for e in polls) if polls else None
        else:
            row["e2e_us"] = delta_us(times["local_round_end"], times["local_round_begin"])
            row["request_call_us"] = delta_us(times["request_end"], times["request_begin"])
            ready = [e for e in app if e["event"] == "watermark_observed"]
            row["ready_groups"] = [{"rail": e["rail"], "from": e["from"], "upto": e["upto"],
                                    "since_begin_us": delta_us(e["timestamp_ns"], times["local_round_begin"])}
                                   for e in ready]
            valid &= sum(e["upto"] - e["from"] for e in ready) == config["count"]
            if ready:
                row["last_ready_to_end_us"] = delta_us(times["local_round_end"], ready[-1]["timestamp_ns"])
            begins = [e for e in app if e["event"] == "scatter_begin"]
            ends = [e for e in app if e["event"] == "scatter_end"]
            valid &= len(begins) == len(ends)
            row["scatter_sum_us"] = round(sum(delta_us(e["timestamp_ns"], b["timestamp_ns"])
                                              for b, e in zip(begins, ends)), 3)
        results.append(row)
    return {"status": "ok" if valid else "incomplete", "missing_post_completions": len(missing), "rounds": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="one host's full stdout log (local OR remote)")
    parser.add_argument("--compact", action="store_true", help="small export with statistics, without per-CQE curves")
    args = parser.parse_args()
    try:
        records = read_records(args.log)
        result = compact_summary(records) if args.compact else summarize(records)
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f"ERROR: invalid or incomplete MF trace: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=False))
    if result["status"] != "ok":
        print("ERROR: incomplete trace; export cannot establish complete completion timing", file=sys.stderr)
    return 0 if result["status"] == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
