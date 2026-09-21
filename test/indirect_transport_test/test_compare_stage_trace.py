#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
"""Synthetic correctness tests only: none of these timings are device evidence."""

import copy
import json
import unittest
import contextlib
import io
import tempfile
from pathlib import Path

from compare_stage_trace import reduce_run, match, compare, merge


def ev(name, time, wr="0x1", **kw):
    return dict(
        record_type="hcom_trace",
        host_role="remote",
        event=name,
        timestamp_ns=time,
        qp_num=1,
        wr_id=wr,
        capture_round=101,
        transfer_kind="data",
        bytes=656,
        sge_count=1,
        send_flags=2,
        post_call_status=0,
        status=0,
        opcode=0,
        thread_slot=0,
        cq_id="0x2",
        batch_id=1,
        count=1,
        poll_begin_ns=time - 20,
        previous_poll_end_ns=time - 30,
        empty_polls=3,
        max_poll_gap_ns=10,
        max_poll_call_ns=20,
        **kw,
    )


def remote(app="mf"):
    records = []
    names = [("request_observed", 100), ("request_decoded", 200), ("copy_batch_begin", 300), ("copy_batch_end", 4000)]
    if app == "sgl":
        names = [
            ("remote_request_received", 90),
            ("remote_request_observed", 100),
            ("remote_request_decoded", 200),
            ("remote_source_prepared", 250),
            ("remote_requests_prepared", 300),
        ]
    for name, time in names:
        records.append(
            dict(
                record_type="mf_app_trace" if app == "mf" else "trace",
                host_role="remote",
                event=name,
                timestamp_ns=time,
                capture_round=101,
                generation=121,
                blocks=2,
                block_bytes=656,
                notify_every_wrs=32,
                sgl_items=30,
            )
        )
    for i, (start, end, complete) in enumerate([(1000, 2000, 1500), (2200, 2300, 3000)]):
        for name, time in [
            ("verbs_post_begin", start),
            ("verbs_post_end", end),
            ("cq_poll_batch", complete),
            ("cqe_observed", complete),
            ("cq_dispatch_begin", complete + 20),
            ("cq_dispatch_end", complete + 40),
        ]:
            e = ev(name, time)
            e.update(batch_id=i + 1, generation=121 if name.startswith("verbs_post") else None)
            if not name.startswith("verbs_post"):
                e["opcode"] = 1
            records.append(e)
    records += (
        [dict(record_type="mf_trace_config", host_role="remote", rounds=1, count=2, size=656)] if app == "mf" else []
    )
    records.append(
        dict(
            record_type="mf_trace_summary" if app == "mf" else "hcom_trace_summary",
            status="ok",
            dropped=0,
            post_records=2,
            cqe_records=2,
            records=12,
            app_records=len(names),
        )
    )
    return records


IDENTITY = {"exit_code": 0, "trace_rounds": 1, "trace": True, "comparison_id": "synthetic-test"}


def local(app):
    records = [e for e in remote(app) if e.get("record_type") not in ("mf_app_trace", "trace")]
    for e in records:
        e["host_role"] = "local"
        if e.get("record_type") == "hcom_trace":
            e["transfer_kind"] = "request"
            e["opcode"] = 2 if e["event"].startswith("verbs_post") else 0
    names = [("local_round_begin", 100), ("request_submit_begin", 300), ("request_end", 450), ("local_round_end", 3500)]
    if app == "sgl":
        names = [
            ("local_begin", 100),
            ("local_request_submit_begin", 300),
            ("local_request_posted", 450),
            ("local_end", 3500),
        ]

    def point(name, time, **fields):
        return dict(
            record_type="mf_app_trace" if app == "mf" else "trace",
            host_role="local",
            event=name,
            timestamp_ns=time,
            capture_round=101,
            generation=121,
            blocks=2,
            block_bytes=656,
            notify_every_wrs=32,
            sgl_items=1,
            **fields,
        )

    points = [point(n, t) for n, t in names]
    for i in range(2):
        for name, time in [("watermark_observed", 1500), ("scatter_begin", 1510), ("scatter_end", 1600)]:
            if app == "sgl":
                name = {
                    "watermark_observed": "local_ready_observed",
                    "scatter_begin": "local_scatter_begin",
                    "scatter_end": "local_scatter_end",
                }[name]
            points.append(point(name, time + i * 1500, rail=0, chunk_id=i, **{"from": i, "upto": i + 1}))
    records[-1]["app_records"] = len(points)
    return records + points


class CompareTests(unittest.TestCase):
    def test_source_preparation_keeps_transfer_window(self):
        for app in ("mf", "sgl"):
            records = remote(app)
            before = self.reduce(records, app)
            prefix = "" if app == "mf" else "remote_"
            names = [(prefix + "source_prepare_begin", 210)]
            if app == "mf":
                names.append(("source_prepared", 250))
                next(r for r in records if r.get("record_type") == "mf_trace_config")["source_update"] = "markers"
                next(r for r in records if r.get("record_type") == "mf_trace_summary")["app_records"] += 2
            for name, time in names:
                records.append(dict(records[0], event=name, timestamp_ns=time))
            after = self.reduce(records, app)
            self.assertEqual(after["status"], "ok", after)
            self.assertEqual(after["rounds"][0]["source_prepare_us"], 0.04)
            self.assertEqual(after["rounds"][0]["first_data_post_to_last_cqe_us"],
                             before["rounds"][0]["first_data_post_to_last_cqe_us"])

    def test_split_merge_requires_all_rounds_without_duplicates(self):
        r = reduce_run(remote(), IDENTITY, "mf", "remote")
        r["identity"] = dict(IDENTITY, trace_rounds=2)
        with tempfile.TemporaryDirectory() as directory:
            files = []
            for number in (101, 102):
                part = copy.deepcopy(r)
                part["rounds"][0]["round"] = number
                path = Path(directory) / (str(number) + ".json")
                path.write_text(json.dumps(part))
                files.append(str(path))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(merge(files), 0)
            self.assertEqual(len(json.loads(output.getvalue())["rounds"]), 2)
            with self.assertRaises(ValueError):
                merge(files[:1])
            with self.assertRaises(ValueError):
                merge([files[0], files[0]])

    def reduce(self, records, app="mf", role="remote"):
        return reduce_run(records, IDENTITY, app, role)

    def test_reuse_early_completion_bytes_tail(self):
        r = self.reduce(remote())
        self.assertEqual(r["status"], "ok", r)
        row = r["rounds"][0]
        self.assertEqual(row["first_data_post_to_last_post_end_us"], 1.3)
        self.assertEqual(row["last_data_post_end_to_last_cqe_us"], 0.7)
        self.assertEqual(row["completed_pct_us"], {25: 0.5, 50: 0.5, 75: 2, 100: 2})
        self.assertEqual(row["max_inflight_wr_observed"], 1)
        self.assertEqual(row["inflight_at_last_post_end"], 1)
        self.assertEqual(row["cqe_to_dispatch_us"]["p50"], 0.02)

    def test_sgl_same_core(self):
        r = self.reduce(remote("sgl"), "sgl")
        self.assertEqual(r["status"], "ok", r)
        self.assertEqual(r["rounds"][0]["data_bytes"], 1312)
        self.assertEqual(r["rounds"][0]["request_callback_to_first_data_post_us"], 0.91)

    def test_missing_cqe_cannot_look_fast(self):
        records = [e for e in remote() if not (e.get("event") == "cqe_observed" and e["batch_id"] == 2)]
        r = self.reduce(records)
        self.assertEqual(r["status"], "incomplete")
        self.assertNotIn("first_data_post_to_last_cqe_us", r["rounds"][0])
        self.assertEqual(r["rounds"][0]["data_completed_bytes"], 656)

    def test_missing_app_tail_or_summary(self):
        for name in ["copy_batch_end", "request_decoded"]:
            r = self.reduce([e for e in remote() if e.get("event") != name])
            self.assertEqual(r["status"], "incomplete")
            self.assertFalse(r["rounds"][0]["completion_metrics_valid"])
        self.assertEqual(self.reduce(remote()[:-1])["status"], "incomplete")

    def test_partial_post_failure_no_successful_timing(self):
        records = remote()
        for e in records:
            if e.get("event", "").startswith("verbs_post") and e["batch_id"] == 2:
                e["post_call_status"] = 12
        r = self.reduce(records)
        self.assertEqual(r["status"], "incomplete")
        self.assertNotIn("first_data_post_to_last_cqe_us", r["rounds"][0])

    def test_failed_wr_not_matched(self):
        a, b, c = ev("verbs_post_begin", 1), ev("verbs_post_end", 2), ev("cqe_observed", 3)
        a["status"] = b["status"] = 12
        rows, errors = match([a, b, c])
        self.assertIsNone(rows[0][2])
        self.assertIn("unmatched_send_cqe", errors)

    def test_duplicate_completion_rejected(self):
        records = remote()
        records.append(copy.deepcopy(next(e for e in records if e.get("event") == "cqe_observed")))
        self.assertEqual(self.reduce(records)["status"], "incomplete")

    def test_poll_and_dispatch_coverage(self):
        for name in ["cq_poll_batch", "cq_dispatch_begin", "verbs_post_end"]:
            self.assertEqual(self.reduce([e for e in remote() if e.get("event") != name])["status"], "incomplete")

    def test_one_call_multiple_wr(self):
        records = remote()
        for e in records:
            if e.get("record_type") == "hcom_trace" and e["batch_id"] == 2:
                e["wr_id"] = "0x2"
                if e["event"] == "verbs_post_begin":
                    e["timestamp_ns"] = 1000
                if e["event"] == "verbs_post_end":
                    e["timestamp_ns"] = 2000
        r = self.reduce(records)
        self.assertEqual(r["status"], "ok")
        self.assertEqual(r["rounds"][0]["data_post_call_us"]["n"], 1)
        self.assertEqual(r["rounds"][0]["max_inflight_wr_observed"], 2)

    def test_error_exit_and_dropped(self):
        records = remote()
        records[-1]["dropped"] = 1
        self.assertEqual(self.reduce(records)["status"], "incomplete")
        self.assertEqual(reduce_run(remote(), {"exit_code": 1}, "mf", "remote")["status"], "incomplete")

    def test_qp_not_confused_and_missing_reused_completion(self):
        a, b, c = ev("verbs_post_begin", 1), ev("verbs_post_end", 2), ev("cqe_observed", 3)
        c["qp_num"] = 2
        self.assertIsNone(match([a, b, c])[0][0][2])
        records = [e for e in remote() if not (e.get("event") == "cqe_observed" and e["batch_id"] == 1)]
        r = self.reduce(records)
        self.assertEqual(r["rounds"][0]["data_cqes"], 1)

    def test_compact_contains_no_raw_addresses_or_event_arrays(self):
        result = self.reduce(remote())
        text = json.dumps(result)
        self.assertNotIn("wr_id", text)
        self.assertNotIn("timestamp_ns", text)
        self.assertLess(len(text), 6000)

    def test_local_scatter_overlap_and_missing_group(self):
        for app in ("mf", "sgl"):
            r = self.reduce(local(app), app, "local")
            self.assertEqual(r["status"], "ok", r)
            self.assertEqual(r["rounds"][0]["scatter_sum_us"], 0.18)
            self.assertEqual(r["rounds"][0]["e2e_us"], 3.4)
            self.assertEqual(r["rounds"][0]["last_ready_to_end_us"], 0.5)
            broken = [e for e in local(app) if not (e.get("chunk_id") == 1 and "scatter_end" in e.get("event", ""))]
            self.assertEqual(self.reduce(broken, app, "local")["status"], "incomplete")

    def test_four_file_comparison_rejects_different_actual_library(self):
        files = []
        with tempfile.TemporaryDirectory() as directory:
            for app in ("mf", "sgl"):
                for role in ("local", "remote"):
                    r = self.reduce(local(app) if role == "local" else remote(app), app, role)
                    r["hcom_build"] = "same-source-config"
                    r["identity"] = dict(IDENTITY, artifact_verified=True)
                    r["qp"] = [{"qp_num": 1}]
                    r["config"].update(
                        count=2,
                        size=656,
                        blocks=2,
                        block_bytes=656,
                        warmup=100,
                        warmup_rounds=100,
                        chunk=32,
                        sgl_items=1,
                        notify_every_wrs=32,
                        links=1,
                        pipeline="on",
                    )
                    path = Path(directory) / (app + role + ".json")
                    path.write_text(json.dumps(r))
                    files.append(str(path))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(compare(files), 0)
                r["hcom_build"] = "different-library"
                path.write_text(json.dumps(r))
                self.assertEqual(compare(files), 1)


if __name__ == "__main__":
    unittest.main()
