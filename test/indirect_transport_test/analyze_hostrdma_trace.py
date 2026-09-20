#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Summarize one host's MF trace. Never subtract clocks from different hosts."""
import argparse
import bisect
import collections
import json
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
    args = parser.parse_args()
    try:
        result = summarize(read_records(args.log))
    except (ValueError, KeyError, StopIteration) as error:
        print(f"ERROR: invalid or incomplete MF trace: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["status"] == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
