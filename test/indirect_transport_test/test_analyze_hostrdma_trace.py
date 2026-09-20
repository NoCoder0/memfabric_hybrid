#!/usr/bin/env python3
# SPDX-License-Identifier: MulanPSL-2.0
import unittest

from analyze_hostrdma_trace import match_completions, summarize


def event(name, time, wr="0x1", **fields):
    return {"record_type": "hcom_trace", "event": name, "timestamp_ns": time, "qp_num": 1,
            "wr_id": wr, "capture_round": 1, "transfer_kind": "data", "bytes": 656, "sge_count": 1,
            "thread_slot": 1, "cq_id": "0x2", "batch_id": 1, **fields}


class TraceAnalysisTest(unittest.TestCase):
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
