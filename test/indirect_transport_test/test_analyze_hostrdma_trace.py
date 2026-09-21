#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
import json
import unittest

from analyze_hostrdma_trace import compact_summary, match_completions, post_call_stats, stats_us, summarize


def event(name, time, wr="0x1", **fields):
    return {"record_type": "hcom_trace", "event": name, "timestamp_ns": time, "qp_num": 1,
            "wr_id": wr, "capture_round": 1, "transfer_kind": "data", "bytes": 656, "sge_count": 1,
            "thread_slot": 1, "cq_id": "0x2", "batch_id": 1, **fields}


def complete_remote(count=2):
    records = [{"record_type": "mf_trace_config", "host_role": "remote", "rounds": 1, "count": count, "size": 656},
               {"record_type": "mf_trace_summary", "status": "ok", "dropped": 0,
                "post_records": count, "cqe_records": count}]
    for name, time in [("request_observed", 0), ("request_decoded", 100),
                       ("copy_batch_begin", 200), ("copy_batch_end", count * 2000 + 2000)]:
        records.append({"record_type": "mf_app_trace", "event": name, "timestamp_ns": time, "capture_round": 1})
    for i in range(count):
        start = 1000 + i * 2000
        for name, time in [("verbs_post_begin", start), ("verbs_post_end", start + 100),
                           ("cq_poll_batch", start + 1000), ("cqe_observed", start + 1000),
                           ("cq_dispatch_begin", start + 1200), ("cq_dispatch_end", start + 1500)]:
            records.append(event(name, time, wr=hex(i), batch_id=i, count=1, poll_begin_ns=start + 900,
                                 empty_polls=3, max_poll_gap_ns=30, max_poll_call_ns=100))
    return records


class TraceAnalysisTest(unittest.TestCase):
    def test_source_update_modes_and_legacy_timing(self):
        legacy = compact_summary(complete_remote())
        self.assertNotIn("source_prepare_us", legacy["rounds"][0])
        for mode in ("static", "markers"):
            records = complete_remote()
            records[0]["source_update"] = mode
            for name, time in (("source_prepare_begin", 120), ("source_prepared", 170)):
                records.append({"record_type": "mf_app_trace", "event": name, "timestamp_ns": time, "capture_round": 1})
            current = compact_summary(records)
            self.assertEqual(current["status"], "ok")
            self.assertEqual(current["config"]["source_update"], mode)
            self.assertEqual(current["rounds"][0]["source_prepare_us"], 0.05)
            self.assertEqual(current["rounds"][0]["first_data_post_to_last_cqe_us"],
                             legacy["rounds"][0]["first_data_post_to_last_cqe_us"])
            records.pop()
            self.assertEqual(compact_summary(records)["status"], "incomplete")

    def test_compact_output_is_bounded_and_keeps_percentiles(self):
        result = compact_summary(complete_remote(1000))
        self.assertEqual(result["status"], "ok")
        row = result["rounds"][0]
        self.assertNotIn("data_completion_curve", row)
        self.assertEqual(row["successful_data_poll_us"]["p95"], 0.1)
        self.assertEqual(row["data_completed_pct_since_first_post_us"]["100"], 1999)
        self.assertLess(len(json.dumps(result)), 4000)

    def test_compact_detects_terminal_truncation_despite_collector_ok(self):
        records = complete_remote()
        records = [e for e in records if e.get("event") not in ("cqe_observed", "cq_poll_batch")]
        result = compact_summary(records)
        self.assertEqual(result["status"], "incomplete")
        self.assertFalse(result["integrity"]["record_counts_match"])
        self.assertFalse(result["rounds"][0]["completion_metrics_valid"])
        self.assertNotIn("first_data_post_to_last_cqe_us", result["rounds"][0])

    def test_compact_detects_missing_poll_with_all_cqes_present(self):
        records = [e for e in complete_remote() if e.get("event") != "cq_poll_batch"]
        self.assertEqual(compact_summary(records)["status"], "incomplete")

    def test_post_calls_do_not_double_count_two_wrs_in_one_call(self):
        records = [event("verbs_post_begin", 100, wr=hex(i)) for i in (1, 2)]
        records += [event("verbs_post_end", 150, wr=hex(i)) for i in (1, 2)]
        self.assertEqual(post_call_stats(records), {"n": 1, "min": 0.05, "p50": 0.05, "p95": 0.05, "max": 0.05})

    def test_nearest_rank_percentiles_and_no_samples(self):
        self.assertIsNone(stats_us([]))
        self.assertEqual(stats_us([1000, 3000, 2000])["p95"], 3)

    def test_mixed_runs_are_rejected(self):
        with self.assertRaises(ValueError):
            summarize([{"record_type": "mf_trace_config"}] * 2)

    def test_cqe_before_post_return_and_reused_id(self):
        records = [event("verbs_post_end", 180), event("cqe_observed", 130),
                   event("verbs_post_begin", 100), event("verbs_post_begin", 200),
                   event("cqe_observed", 220, capture_round=2)]
        pairs, missing = match_completions(records)
        self.assertEqual([(p["timestamp_ns"], c["timestamp_ns"]) for p, c in pairs], [(100, 130), (200, 220)])
        self.assertEqual(missing, [])

    def test_qp_separates_identical_wr_id(self):
        pairs, missing = match_completions([event("verbs_post_begin", 100), event("cqe_observed", 150, qp_num=2)])
        self.assertEqual(pairs, [])
        self.assertEqual(len(missing), 1)

    def test_missing_first_completion_is_not_replaced_by_reused_id(self):
        pairs, missing = match_completions([event("verbs_post_begin", 100), event("verbs_post_begin", 200),
                                           event("cqe_observed", 210)])
        self.assertEqual(missing[0]["timestamp_ns"], 100)
        self.assertEqual(pairs[0][0]["timestamp_ns"], 200)

    def test_remote_byte_coverage_and_missing_cqe(self):
        records = [{"record_type": "mf_trace_config", "host_role": "remote", "rounds": 1, "count": 1, "size": 656},
                   {"record_type": "mf_trace_summary", "status": "ok"},
                   {"record_type": "mf_app_trace", "event": "request_observed", "timestamp_ns": 50,
                    "capture_round": 1}, event("verbs_post_begin", 100), event("cqe_observed", 300),
                   event("cq_poll_batch", 300, poll_begin_ns=280)]
        result = summarize(records)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["rounds"][0]["first_data_post_to_last_cqe_us"], 0.2)
        records[0]["size"] = 657
        self.assertEqual(summarize(records)["status"], "incomplete")
        records[0]["size"] = 656
        self.assertEqual(summarize([r for r in records if r.get("event") != "cqe_observed"])["status"], "incomplete")


if __name__ == "__main__":
    unittest.main()
