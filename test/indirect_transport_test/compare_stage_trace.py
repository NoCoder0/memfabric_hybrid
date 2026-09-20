#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Device-side MF/SGL reduction and four-file comparison. No cross-host subtraction."""

import argparse
import collections as C
import json
import math
import statistics
import sys
from pathlib import Path

from analyze_hostrdma_trace import read_records, stats_us


def us(a, b):
    return round((a - b) / 1000, 3)


def histogram(values):
    return dict(sorted(C.Counter(values).items(), key=lambda p: str(p[0])))


def normalize(records, app, role):
    events, apps = [], []
    for original in records:
        e = dict(original)
        if e.get("record_type") not in ("hcom_trace", "mf_app_trace", "trace"):
            continue
        if e.get("host_role") != role:
            raise ValueError("mixed roles")
        e["round"] = e.get("capture_round") if app == "mf" else e.get("generation")
        if e["record_type"] != "hcom_trace":
            apps.append(e)
            continue
        if app == "sgl":
            e["transfer_kind"] = "other"
            if e.get("generation"):
                if e["opcode"] == 0 and role == "remote":
                    e["transfer_kind"] = "data"
                elif e["opcode"] in (2, 3):
                    e["transfer_kind"] = "request" if role == "local" else "notify"
        events.append(e)
    return events, apps


def wr_key(e):
    return e["qp_num"], e["wr_id"]


def cq_key(e):
    return e["thread_slot"], e["cq_id"], e["batch_id"], e["qp_num"], e["wr_id"]


def match(events):
    """Match each occurrence, using BEGIN (CQE may precede END). Fail ambiguous reuse."""
    groups = C.defaultdict(lambda: C.defaultdict(list))
    for e in events:
        groups[wr_key(e)][e["event"]].append(e)
    rows, errors, consumed = [], [], set()
    for key, g in groups.items():
        starts = sorted(g["verbs_post_begin"], key=lambda e: e["timestamp_ns"])
        ends = sorted(g["verbs_post_end"], key=lambda e: e["timestamp_ns"])
        cqes = sorted(g["cqe_observed"], key=lambda e: e["timestamp_ns"])
        if len(starts) != len(ends):
            errors.append("post_begin_end_count")
        for i, p in enumerate(starts):
            end = ends[i] if i < len(ends) else None
            upper = starts[i + 1]["timestamp_ns"] if i + 1 < len(starts) else math.inf
            candidates = [c for c in cqes if p["timestamp_ns"] <= c["timestamp_ns"] < upper]
            c = candidates[0] if len(candidates) == 1 and p["status"] == 0 else None
            if end is None or end["timestamp_ns"] < p["timestamp_ns"] or end["status"] != p["status"]:
                errors.append("invalid_post_end")
            if p["status"] == 0 and (c is None or c["status"] != 0):
                errors.append("missing_ambiguous_or_failed_cqe")
            expected_opcode = {0: 1, 4: 2, 2: 0, 3: 0}.get(p["opcode"])
            if c is not None and expected_opcode is not None and c["opcode"] != expected_opcode:
                errors.append("wr_wc_opcode_mismatch")
            if p["status"] or p.get("post_call_status", 0):
                errors.append("failed_post_call")
            if c is not None:
                consumed.add(id(c))
            rows.append((p, end, c))
    # Receive WCs have no send-side POST; unmatched send WCs indicate lost posts.
    errors.extend(
        "unmatched_send_cqe"
        for e in events
        if e["event"] == "cqe_observed" and e["opcode"] < 128 and id(e) not in consumed
    )
    return rows, errors


def integrity(records, events, apps, app):
    summaries = [
        r for r in records if r.get("record_type") == ("mf_trace_summary" if app == "mf" else "hcom_trace_summary")
    ]
    errors = []
    if len(summaries) != 1:
        return ["missing_or_multiple_collector_summary"], {}
    summary = summaries[0]
    counts = C.Counter(e["event"] for e in events)
    if summary.get("status") != "ok" or any(summary.get(k, 0) for k in ("dropped", "clock_errors", "event_errors")):
        errors.append("collector_incomplete")
    for field, event in [("post_records", "verbs_post_end"), ("cqe_records", "cqe_observed")]:
        if summary.get(field) != counts[event]:
            errors.append("export_" + field)
    if any("send_flags" not in e or "post_call_status" not in e for e in events if e["event"] == "verbs_post_begin"):
        errors.append("missing_submission_metadata")
    if "records" in summary and summary["records"] != len(events):
        errors.append("export_record_count")
    if app == "mf" and (summary.get("app_records") != len(apps) or summary.get("records") != len(events)):
        errors.append("export_app_or_event_count")
    polls = {(e["thread_slot"], e["cq_id"], e["batch_id"]): e for e in events if e["event"] == "cq_poll_batch"}
    batches = C.Counter((e["thread_slot"], e["cq_id"], e["batch_id"]) for e in events if e["event"] == "cqe_observed")
    if any(p["count"] != batches[k] for k, p in polls.items()) or set(batches) - set(polls):
        errors.append("poll_cqe_count")
    if any(e["poll_begin_ns"] > e["timestamp_ns"] for e in polls.values()):
        errors.append("poll_clock_order")
    for event in ("cq_dispatch_begin", "cq_dispatch_end"):
        if C.Counter(cq_key(e) for e in events if e["event"] == event) != C.Counter(
            cq_key(e) for e in events if e["event"] == "cqe_observed"
        ):
            errors.append(event + "_coverage")
    observed = {cq_key(e): e["timestamp_ns"] for e in events if e["event"] == "cqe_observed"}
    if any(
        e["timestamp_ns"] < observed.get(cq_key(e), 0)
        for e in events
        if e["event"] in ("cq_dispatch_begin", "cq_dispatch_end")
    ):
        errors.append("dispatch_before_cqe")
    if any(e.get("status", 0) or e["timestamp_ns"] <= 0 for e in events + apps):
        errors.append("event_error_or_clock")
    return errors, summary


def interval_metrics(apps, names, required, errors):
    times = {e["event"]: e["timestamp_ns"] for e in apps}
    errors.extend("missing_" + n for n in required if n not in times)
    result = {}
    for name, begin, end in names:
        if begin in times and end in times:
            result[name] = us(times[end], times[begin])
            if result[name] < 0:
                errors.append("negative_" + name)
    return result, times


def local_metrics(apps, app, count, errors, events=()):
    if app == "mf":
        begin, end, submit, submitted = "local_round_begin", "local_round_end", "request_submit_begin", "request_end"
        ready, sb, se = "watermark_observed", "scatter_begin", "scatter_end"
    else:
        begin, end, submit, submitted = "local_begin", "local_end", "local_request_submit_begin", "local_request_posted"
        ready, sb, se = "local_ready_observed", "local_scatter_begin", "local_scatter_end"
    row, times = interval_metrics(
        apps,
        [("e2e_us", begin, end), ("request_prepare_us", begin, submit), ("request_submit_us", submit, submitted)],
        [begin, end, submit, submitted, ready, sb, se],
        errors,
    )
    if begin not in times or end not in times:
        return row
    origin = times[begin]
    groups = C.defaultdict(dict)
    for e in apps:
        key = (e.get("rail"), e.get("chunk_id") if app == "sgl" else (e.get("from"), e.get("upto")))
        if e["event"] in (ready, sb, se, "local_chunk_ready"):
            if e["event"] in groups[key]:
                errors.append("duplicate_scatter_or_ready")
            groups[key][e["event"]] = e
    intervals, observations = [], []
    for key, g in groups.items():
        if not all(n in g for n in (ready, sb, se)):
            errors.append("scatter_or_ready_coverage")
            continue
        a, b, r = g[sb]["timestamp_ns"], g[se]["timestamp_ns"], g[ready]["timestamp_ns"]
        if not origin <= r <= a <= b <= times[end]:
            errors.append("scatter_order")
        intervals.append((a, b))
        observations.append(
            {
                "group": key,
                "ready_us": us(r, origin),
                "scatter_begin_us": us(a, origin),
                "scatter_end_us": us(b, origin),
                "scatter_us": us(b, a),
            }
        )
    if app == "mf" and sum(e["upto"] - e["from"] for e in apps if e["event"] == ready) != count:
        errors.append("local_ready_block_coverage")
    row["scatter_sum_us"] = round(sum(b - a for a, b in intervals) / 1000, 3)
    row["scatter_call_us"] = stats_us([b - a for a, b in intervals])
    row["scatter_interval_count"] = len(intervals)
    if app == "sgl":
        # Single-port comparison: every K-sized chunk must have one ready/scatter interval.
        k = next((e.get("sgl_items") for e in apps if e.get("sgl_items")), None)
        if k is None or len(intervals) != math.ceil(count / k):
            errors.append("local_scatter_chunk_coverage")
    row["last_ready_to_end_us"] = us(times[end], times[ready]) if ready in times else None
    # Full per-chunk arrays stay in raw logs. Group summaries cover every chunk.
    row["ready_groups"] = observations if app == "mf" else sgl_ready_groups(apps, origin, observations)
    notifications = [
        e
        for e in events
        if e.get("round") == apps[0]["round"] and e["event"] in ("notify_decoded", "notify_ready_published")
    ]
    if notifications:
        row["notification_handler_marks"] = [
            {
                "event": e["event"],
                "rail": e["rail"],
                "chunk": e["chunk_id"],
                "since_begin_us": us(e["timestamp_ns"], origin),
            }
            for e in notifications
        ]
        published = [e["timestamp_ns"] for e in notifications if e["event"] == "notify_ready_published"]
        if published:
            row["last_notification_published_to_end_us"] = us(times[end], max(published))
    return row


def sgl_ready_groups(apps, origin, observations):
    width = apps[0]["notify_every_wrs"]
    groups = C.defaultdict(list)
    for o in observations:
        rail, chunk = o["group"]
        groups[(rail, chunk // width)].append(o)
    return [
        {
            "rail": k[0],
            "group": k[1],
            "chunks": len(v),
            "first_consumer_ready_us": min(o["ready_us"] for o in v),
            "last_consumer_ready_us": max(o["ready_us"] for o in v),
            "scatter_begin_us": min(o["scatter_begin_us"] for o in v),
            "scatter_end_us": max(o["scatter_end_us"] for o in v),
            "scatter_sum_us": round(sum(o["scatter_end_us"] - o["scatter_begin_us"] for o in v), 3),
            "callback_ready_first_us": min(
                (
                    us(e["timestamp_ns"], origin)
                    for e in apps
                    if e["event"] == "local_chunk_ready" and e["rail"] == k[0] and e["chunk_id"] // width == k[1]
                ),
                default=None,
            ),
        }
        for k, v in sorted(groups.items())
    ]


def remote_app_metrics(apps, app, first, errors):
    observed = "request_observed" if app == "mf" else "remote_request_observed"
    decoded = "request_decoded" if app == "mf" else "remote_request_decoded"
    names = [("request_decode_us", observed, decoded)]
    if app == "mf":
        names += [
            ("decoded_to_copy_begin_us", decoded, "copy_batch_begin"),
            ("copy_batch_us", "copy_batch_begin", "copy_batch_end"),
        ]
        required = [observed, decoded, "copy_batch_begin", "copy_batch_end"]
    else:
        names += [
            ("callback_to_request_observed_us", "remote_request_received", observed),
            ("request_copy_us", observed, "remote_request_copied"),
            ("request_parse_us", "remote_request_copied", decoded),
            ("source_prepare_us", decoded, "remote_source_prepared"),
            ("requests_prepare_us", "remote_source_prepared", "remote_requests_prepared"),
        ]
        required = [observed, decoded, "remote_request_received", "remote_source_prepared", "remote_requests_prepared"]
    row, times = interval_metrics(apps, names, required, errors)
    if first is not None and observed in times:
        row["request_to_first_data_post_us"] = us(first, times[observed])
        if app == "sgl" and "remote_request_received" in times:
            row["request_callback_to_first_data_post_us"] = us(first, times["remote_request_received"])
    return row


def completion_metrics(rows, events, expected, errors):
    data = [(p, e, c) for p, e, c in rows if p["transfer_kind"] == "data" and p["status"] == 0]
    row = {
        "data_wr": len(data),
        "data_bytes": sum(p["bytes"] for p, _, _ in data),
        "data_completed_bytes": sum(p["bytes"] for p, _, c in data if c and c["status"] == 0),
        "data_cqes": sum(c is not None for _, _, c in data),
    }
    if not data or row["data_bytes"] != expected or row["data_completed_bytes"] != expected:
        errors.append("data_byte_coverage")
    row["completion_metrics_valid"] = not errors and all(e and c for _, e, c in data)
    if not row["completion_metrics_valid"]:
        return row
    first = min(p["timestamp_ns"] for p, _, _ in data)
    last_end = max(e["timestamp_ns"] for _, e, _ in data)
    last_cqe = max(c["timestamp_ns"] for _, _, c in data)
    row.update(
        first_data_post_to_last_post_end_us=us(last_end, first),
        last_data_post_end_to_last_cqe_us=us(last_cqe, last_end),
        first_data_post_to_last_cqe_us=us(last_cqe, first),
    )
    row["data_post_to_cqe_us"] = stats_us([c["timestamp_ns"] - p["timestamp_ns"] for p, _, c in data])
    changes = C.defaultdict(lambda: [0, 0])
    for p, _, c in data:
        changes[p["timestamp_ns"]][0] += 1
        changes[c["timestamp_ns"]][0] -= 1
        changes[c["timestamp_ns"]][1] += p["bytes"]
    live = maximum = completed = 0
    milestones = {}
    for t, (delta, nbytes) in sorted(changes.items()):
        live += delta
        maximum = max(maximum, live)
        completed += nbytes
        for pct in (25, 50, 75, 100):
            if pct not in milestones and completed * 100 >= expected * pct:
                milestones[pct] = us(t, first)
    row["max_inflight_wr_observed"] = maximum
    row["inflight_at_last_post_end"] = sum(p["timestamp_ns"] <= last_end < c["timestamp_ns"] for p, _, c in data)
    row["completed_pct_us"] = milestones
    row["notification_boundaries"] = [
        {
            "kind": p["transfer_kind"],
            "since_first_post_us": us(p["timestamp_ns"], first),
            "inflight_wr": sum(a["timestamp_ns"] <= p["timestamp_ns"] < c["timestamp_ns"] for a, _, c in data),
            "completed_bytes": sum(a["bytes"] for a, _, c in data if c["timestamp_ns"] <= p["timestamp_ns"]),
        }
        for p, _, _ in rows
        if p["transfer_kind"] in ("notify", "watermark")
    ]
    row.update(poll_metrics(data, events))
    calls = sorted({(p["thread_slot"], p["timestamp_ns"], e["timestamp_ns"]) for p, e, _ in data})
    gaps = []
    for thread in {t for t, _, _ in calls}:
        ordered = sorted((a, b) for t, a, b in calls if t == thread)
        gaps.extend(a2 - b1 for (_, b1), (a2, _) in zip(ordered, ordered[1:]))
    row["between_data_post_calls_us"] = stats_us(gaps)
    return row


def poll_metrics(data, events):
    keys = {cq_key(c)[:3] for _, _, c in data}
    polls = [e for e in events if e["event"] == "cq_poll_batch" and cq_key(e)[:3] in keys]
    selected = {cq_key(c): c for _, _, c in data}
    dispatch, callbacks, durations, dispatch_durations = [], [], [], []
    begins, dispatch_begins = {}, {}
    for e in sorted(events, key=lambda e: e["timestamp_ns"]):
        key = cq_key(e)
        if key not in selected:
            continue
        if e["event"] == "cq_dispatch_begin":
            dispatch.append(e["timestamp_ns"] - selected[key]["timestamp_ns"])
            dispatch_begins[key] = e["timestamp_ns"]
        if e["event"] == "cq_dispatch_end" and key in dispatch_begins:
            dispatch_durations.append(e["timestamp_ns"] - dispatch_begins[key])
        if e["event"] == "data_callback_begin":
            callbacks.append(e["timestamp_ns"] - selected[key]["timestamp_ns"])
            begins[key] = e["timestamp_ns"]
        if e["event"] == "data_callback_end" and key in begins:
            durations.append(e["timestamp_ns"] - begins[key])
    return {
        "data_poll_call_us": stats_us([e["timestamp_ns"] - e["poll_begin_ns"] for e in polls]),
        "immediate_previous_poll_gap_us": stats_us(
            [e["poll_begin_ns"] - e["previous_poll_end_ns"] for e in polls if e["previous_poll_end_ns"]]
        ),
        "max_gap_in_poll_windows_us": max((e["max_poll_gap_ns"] for e in polls), default=0) / 1000,
        "max_call_in_poll_windows_us": max((e["max_poll_call_ns"] for e in polls), default=0) / 1000,
        "empty_polls_in_reported_windows": sum(e["empty_polls"] for e in polls),
        "cqes_per_data_poll": histogram(e["count"] for e in polls),
        "poll_window_histogram_upper_ns": [125, 250, 500, 1000, 2000, 4000, 8000, "inf"],
        "poll_call_bins": [sum(e.get("poll_call_bins", [0] * 8)[i] for e in polls) for i in range(8)],
        "poll_gap_bins": [sum(e.get("poll_gap_bins", [0] * 8)[i] for e in polls) for i in range(8)],
        "cqe_to_dispatch_us": stats_us(dispatch),
        "cqe_to_application_callback_us": stats_us(callbacks),
        "dispatch_us": stats_us(dispatch_durations),
        "application_callback_us": stats_us(durations),
    }


def reduce_run(records, identity, app, role):
    events, apps = normalize(records, app, role)
    errors, collector = integrity(records, events, apps, app)
    rows, match_errors = match(events)
    errors += match_errors
    config = (
        next((r for r in records if r.get("record_type") == "mf_trace_config"), {})
        if app == "mf"
        else (next((r for r in records if r.get("schema_version") == 8 and r.get("role") == role), {}))
    )
    config = dict(config)
    if app == "mf":
        config.update(next((r for r in records if r.get("record_type") == "mf_trace_layout"), {}))
    rounds = sorted({e["round"] for e in apps if e["round"] is not None})
    if not rounds:
        errors.append("no_app_rounds")
    count = config.get("count", config.get("blocks", apps[0].get("blocks", 0) if apps else 0))
    size = config.get("size", config.get("block_bytes", apps[0].get("block_bytes", 0) if apps else 0))
    expected_rounds = config.get("rounds", config.get("trace_rounds", identity.get("trace_rounds")))
    if expected_rounds != len(rounds):
        errors.append("round_count_or_missing_expected_rounds")
    if identity.get("exit_code") != 0:
        errors.append("process_exit_missing_or_failed")
    if identity.get("artifact_verified") is False:
        errors.append("artifact_mismatch")
    if len({e.get("case_index", 1) for e in apps}) != 1:
        errors.append("multiple_cases_not_supported_use_single_case")
    result = {
        "format": "stage-compact-v1",
        "app": app,
        "role": role,
        "identity": identity,
        "hcom_build": next((r["id"] for r in records if r.get("record_type") == "hcom_build_identity"), None),
        "qp": [r for r in records if r.get("record_type") == "hcom_qp_identity"],
        "config": {
            k: config[k]
            for k in (
                "count",
                "size",
                "blocks",
                "block_bytes",
                "chunk",
                "stride",
                "warmup",
                "warmup_rounds",
                "rounds",
                "trace_rounds",
                "links",
                "sgl_items",
                "notify_every_wrs",
                "pipeline",
                "source_order",
                "kind",
            )
            if k in config
        },
        "rounds": [],
    }
    for number in rounds:
        ae = sorted((e for e in apps if e["round"] == number), key=lambda e: e["timestamp_ns"])
        rr = [r for r in rows if r[0]["round"] == number]
        re = list(errors)
        first = min((p["timestamp_ns"] for p, _, _ in rr if p["transfer_kind"] == "data"), default=None)
        row = local_metrics(ae, app, count, re, events) if role == "local" else remote_app_metrics(ae, app, first, re)
        if role == "remote":
            row.update(completion_metrics(rr, events, count * size, re))
        row["wr_shapes"] = histogram(
            f"{p['transfer_kind']}:{p['sge_count']}SGE/{p['bytes']}B/flags={p.get('send_flags', '?')}" for p, _, _ in rr
        )
        row["wr_counts"] = histogram(p["transfer_kind"] for p, _, _ in rr)
        row["qp_by_kind"] = {
            kind: sorted({p["qp_num"] for p, _, _ in rr if p["transfer_kind"] == kind}) for kind in row["wr_counts"]
        }
        calls = {
            (p["thread_slot"], p["timestamp_ns"], e["timestamp_ns"])
            for p, e, _ in rr
            if e and p["transfer_kind"] == "data"
        }
        row["data_post_call_us"] = stats_us([b - a for _, a, b in calls])
        row["round"] = number
        row["errors"] = histogram(re)
        result["rounds"].append(row)
    result["integrity"] = {
        "collector": collector,
        "errors": histogram(errors),
        "matched_posts": sum(c is not None for _, _, c in rows),
        "posts": len(rows),
    }
    result["status"] = (
        "ok" if rounds and not errors and all(not r["errors"] for r in result["rounds"]) else "incomplete"
    )
    result["basis"] = (
        "same-host ns; CQE=software observation; per-round event percentiles; poll windows may precede data; inflight=BEGIN-to-CQE"
    )
    return result


def compare(files):
    runs = [json.loads(Path(f).read_text(encoding="utf-8")) for f in files]
    keys = {(r["app"], r["role"]) for r in runs}
    if keys != {(a, b) for a in ("mf", "sgl") for b in ("local", "remote")} or len(runs) != 4:
        raise ValueError("need exactly MF/SGL x local/remote")
    builds = {r.get("hcom_build") for r in runs}
    issues = []
    if len(builds) != 1 or None in builds or "standalone-unverified" in builds or "missing" in builds:
        issues.append("actual HCOM build identities differ or are missing")
    if any(r["status"] != "ok" for r in runs):
        issues.append("incomplete capture: completion timing is not established")
    if len({r["identity"].get("comparison_id") for r in runs}) != 1 or not runs[0]["identity"].get("comparison_id"):
        issues.append("comparison IDs differ")
    local = {r["app"]: r for r in runs if r["role"] == "local"}
    mf, sgl = local["mf"]["config"], local["sgl"]["config"]
    if (mf.get("count"), mf.get("size"), mf.get("warmup")) != (
        sgl.get("blocks"),
        sgl.get("block_bytes"),
        sgl.get("warmup_rounds"),
    ):
        issues.append("workload or warmup differs")
    if mf.get("chunk") != sgl.get("sgl_items", 0) * sgl.get("notify_every_wrs", 0):
        issues.append("chunk and K*G differ")
    if sgl.get("links") != 1 or sgl.get("pipeline") != "on":
        issues.append("requires single-port pipeline-on baseline")
    if mf.get("links") != 1:
        issues.append("MF single-port identity missing")
    if any(not r["identity"].get("trace") for r in runs):
        issues.append("trace identity missing")
    if any(not r["identity"].get("artifact_verified") or not r["qp"] for r in runs):
        issues.append("actual artifact or QP identity is not verified")
    if any(len(r["rounds"]) != r["identity"].get("trace_rounds") for r in runs):
        issues.append("selected rounds only: merge the complete per-role round files first")
    print("# MF / SGL same-source stage comparison\n")
    print(
        "Evidence status: " + ("; ".join(issues) if issues else "trace complete; review recorded configuration below")
    )
    print("\nTimes are same-host microseconds. Trace timings include instrumentation overhead.\n")
    for role in ("local", "remote"):
        subset = [r for r in runs if r["role"] == role]
        fields = sorted(
            {
                k
                for r in subset
                for row in r["rounds"]
                for k, v in row.items()
                if (
                    k.endswith("_us")
                    or k in ("data_wr", "data_bytes", "max_inflight_wr_observed", "inflight_at_last_post_end")
                )
                and isinstance(v, (int, float))
            }
        )
        print(f"## {role}\n\n| metric | MF rounds | SGL rounds | SGL-MF mean delta |\n|---|---|---|---|")
        for field in fields:
            values = [
                " / ".join(str(row.get(field, "NA")) for row in next(r for r in subset if r["app"] == a)["rounds"])
                for a in ("mf", "sgl")
            ]
            samples = [
                [
                    row[field]
                    for row in next(r for r in subset if r["app"] == a)["rounds"]
                    if isinstance(row.get(field), (int, float))
                ]
                for a in ("mf", "sgl")
            ]
            delta = str(round(statistics.mean(samples[1]) - statistics.mean(samples[0]), 3)) if all(samples) else "NA"
            print("| " + " | ".join([field] + values + [delta]) + " |")
    print("\nDo not sum overlapped remote transfer and local scatter. MF dispatch and SGL application callback differ.")
    print("No new hardware result is implied by synthetic tests. Formal e2e comparisons require trace-off runs.")
    for r in runs:
        print("\n### " + r["app"] + " " + r["role"] + "\n")
        print(
            "```json\n"
            + json.dumps(
                {
                    "trace_off_baseline": r["identity"].get("baseline"),
                    "config": r["config"],
                    "qp": r["qp"],
                    "rounds": r["rounds"],
                },
                ensure_ascii=False,
                indent=2,
            )
            + "\n```"
        )
    return 1 if issues else 0


def merge(files):
    parts = [json.loads(Path(f).read_text(encoding="utf-8")) for f in files]
    result = dict(parts[0], rounds=[])
    seen = set()
    for part in parts:
        if any(part.get(k) != result.get(k) for k in ("identity", "app", "role", "hcom_build", "integrity", "status")):
            raise ValueError("round files are not from the same complete run")
        for row in part["rounds"]:
            if row["round"] in seen:
                raise ValueError("duplicate round")
            seen.add(row["round"])
            result["rounds"].append(row)
    result["rounds"].sort(key=lambda r: r["round"])
    if len(seen) != result["identity"]["trace_rounds"]:
        raise ValueError("round files do not cover the configured capture")
    print(json.dumps(result, separators=(",", ":"), ensure_ascii=False))
    return 0 if result["status"] == "ok" else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    r = sub.add_parser("reduce")
    r.add_argument("log")
    r.add_argument("--identity", required=True)
    r.add_argument("--app", choices=["mf", "sgl"], required=True)
    r.add_argument("--role", choices=["local", "remote"], required=True)
    r.add_argument("--round", type=int, help="extract just this round from the full device log")
    c = sub.add_parser("compare")
    c.add_argument("files", nargs=4)
    m = sub.add_parser("merge")
    m.add_argument("files", nargs="+")
    args = p.parse_args()
    try:
        if args.command == "compare":
            return compare(args.files)
        if args.command == "merge":
            return merge(args.files)
        result = reduce_run(
            read_records(args.log), json.loads(Path(args.identity).read_text(encoding="utf-8")), args.app, args.role
        )
        if args.round is not None:
            result["rounds"] = [r for r in result["rounds"] if r["round"] == args.round]
            if not result["rounds"]:
                raise ValueError("requested round not present")
        print(json.dumps(result, separators=(",", ":"), ensure_ascii=False))
        return 0 if result["status"] == "ok" else 1
    except (OSError, ValueError, KeyError, StopIteration) as e:
        print("ERROR: incomplete/invalid trace: " + str(e), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
