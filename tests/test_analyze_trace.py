import unittest
from pathlib import Path

import analyze_trace
import sim_mc4k_final


def mc4k_edge_text(value: int, sof_bits: int = 1) -> str:
    bits = [(value >> (31 - i)) & 1 for i in range(32)]
    waveform = sim_mc4k_final.generate_comp1_waveform([1] * sof_bits + bits)
    events = sim_mc4k_final.simulate_tim2(waveform)
    return " ".join(f"{'H' if level else 'L'}:{duration}" for level, duration in events)


def ac2k_zero_start01_edge_text(periods: int = 31) -> str:
    return " ".join(["L:5"] + ["L:512"] * periods + ["H:25000"])


class AnalyzeTraceTests(unittest.TestCase):
    def test_edge_model_classifies_ttf_noise_no_activity_and_command_response(self):
        stable_ttf = analyze_trace.classify_edge_model(
            first_edge_us=10400,
            edges=64,
            rx_bits=64,
            first_bytes=bytes.fromhex("AA55AA55"),
        )
        shifted_ttf = analyze_trace.classify_edge_model(
            first_edge_us=10800,
            edges=64,
            rx_bits=64,
            first_bytes=bytes.fromhex("D5555555"),
        )
        no_activity = analyze_trace.classify_edge_model(
            first_edge_us=0,
            edges=0,
            rx_bits=0,
            first_bytes=b"",
        )
        short_noise = analyze_trace.classify_edge_model(
            first_edge_us=120,
            edges=3,
            rx_bits=0,
            first_bytes=b"\x00",
        )
        command_response = analyze_trace.classify_edge_model(
            first_edge_us=430,
            edges=38,
            rx_bits=32,
            first_bytes=bytes.fromhex("E6012345"),
        )

        self.assertEqual(stable_ttf.classification, "ttf_broadcast")
        self.assertEqual(stable_ttf.clock_guess, "RF/64")
        self.assertEqual(shifted_ttf.classification, "ttf_broadcast")
        self.assertEqual(no_activity.classification, "no_activity")
        self.assertEqual(short_noise.classification, "partial_noise")
        self.assertEqual(command_response.classification, "command_response")

    def test_report_includes_edge_model_events_from_trace(self):
        trace = """=== HiTag S Debug Trace ===

EDGE_MODEL phase=before first_edge_us=10400 edges=64 rx_bits=64 first=AA55AA55 ttf_score=100 low_entropy=0 clock_guess=RF/64 classification=ttf_broadcast
WRITE_RESULT method=t5577_full classification=write_ignored before=000000204C after=000000204C restored=0
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertEqual(len(parsed.edge_models), 1)
        self.assertIn("Edge Model Events", report)
        self.assertIn("phase=before", report)
        self.assertIn("classification=ttf_broadcast", report)
        self.assertIn("WRITE_RESULT method=t5577_full classification=write_ignored", report)

    def test_std_mode_redecode_uses_one_sof_bit(self):
        trace = f"""=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  RESULT: OK, UID=12345678 (mode=STD)

--- SELECT ---
  TX: SELECT UID=12345678 CRC=00 (45 bits)
  RX: 64 edges mode=MC4K
  EDGES: {mc4k_edge_text(0x060000E8, sof_bits=1)}
  DECODE: 32 bits = 06 00 00 E8
  RESULT: OK, Config=060000E8
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed, redecode=True)

        self.assertIn("Re-decode MC4K: 32 bits = 060000E8", report)
        self.assertNotIn("MISMATCH", report)

    def test_mc4k_sweep_reports_better_sof_threshold_candidate(self):
        trace = f"""=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RESULT: OK, UID=52810231 (mode=STD)

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 64 edges mode=MC4K
  EDGES: {mc4k_edge_text(0x060000E8, sof_bits=0)}
  DECODE: 31 bits = 0C 00 01 D0
  RESULT: TIMEOUT (31 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed, redecode=True)
        summary = analyze_trace.generate_batch_summary([("sof0.htsd", parsed)])

        self.assertIn("MC4K sweep: best=32 bits", report)
        self.assertIn("sof=0", report)
        self.assertIn("data=", report)
        self.assertIn("select-decode-sweep", summary)

    def test_report_preserves_multiple_tx_lines_in_one_transaction(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  TX: UID_REQ_ADV1 (5 bits, val=0x19)
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  TX: UID_REQ_ADV2 (5 bits, val=0x18)
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("TX: UID_REQ_STD", report)
        self.assertIn("TX: UID_REQ_ADV1", report)
        self.assertIn("TX: UID_REQ_ADV2", report)

    def test_report_associates_captures_with_current_tx_and_abort_reason(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  TX: UID_REQ_ADV1 (5 bits, val=0x19)
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
ABORT: UID request failed (result=1)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("Capture 1 after TX: UID_REQ_STD", report)
        self.assertIn("Capture 2 after TX: UID_REQ_ADV1", report)
        self.assertIn("ABORT: UID request failed", report)

    def test_select_tx_frame_is_checked_against_model(self):
        trace = """=== HiTag S Debug Trace ===

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("TX frame check: OK", report)
        self.assertIn("tx_us=8512", report)

    def test_report_includes_lf_field_metadata(self):
        trace = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertEqual(parsed.field_pull, "release")
        self.assertIn("LF field: carrier=125000Hz duty=0.5 pull=release powerup_us=3000", report)

    def test_capture_report_includes_current_tx_frame_context(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("tx_frame=30/5b tx_us=1280", report)

    def test_report_includes_rx_window_metadata(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RX_META: elapsed_us=1500 idle_us=1200 timeout_us=25000 final_edges=4
  RX: 4 edges mode=AC2K
  EDGES: L:5 H:25000 L:512 H:2000
  DECODE: 1 bits = 00
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertEqual(parsed.transactions[0].captures[0].rx_elapsed_us, 1500)
        self.assertIn("rx_meta=elapsed:1500us idle:1200us timeout:25000us", report)

    def test_uid_request_frames_are_checked_against_model(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  TX: UID_REQ_ADV1 (5 bits, val=0x19)
  TX_FRAME: frame=C8 bits=5 tx_us=1344
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  TX: UID_REQ_ADV2 (5 bits, val=0x18)
  TX_FRAME: frame=C0 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("UID TX frame check: OK (30, 5 bits, tx_us=1280)", report)
        self.assertIn("UID TX frame check: OK (C8, 5 bits, tx_us=1344)", report)
        self.assertIn("UID TX frame check: OK (C0, 5 bits, tx_us=1280)", report)

    def test_fadv_uid_request_frame_and_modes_are_reported(self):
        trace = """=== HiTag S Debug Trace v2 ===

PROTO_MODE: FADV cmd=D0 uid_rx=AC4K data_rx=MC8K uid_sof=3 data_sof=6
SELECT_EXPECT: bits=40 crc=yes

--- UID_REQUEST ---
  TX: UID_REQ_FADV (5 bits, val=0x1A)
  TX_FRAME: frame=D0 bits=5 tx_us=1344
  RX: 2 edges mode=AC4K threshold=160/224 sof=3 expected_bits=32
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)
        summary = analyze_trace.generate_batch_summary([("fadv.htsd", parsed)])

        self.assertEqual(parsed.proto_mode, "FADV")
        self.assertIn("Protocol mode: FADV", report)
        self.assertIn("UID TX frame check: OK (D0, 5 bits, tx_us=1344)", report)
        self.assertIn("mode=AC4K", report)
        self.assertIn("mode=FADV", summary)

    def test_uid_request_frame_mismatch_is_reported(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=38 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("UID TX frame check: MISMATCH", report)
        self.assertIn("expected=30/5b", report)

    def test_select_tx_frame_mismatch_is_reported(self):
        trace = """=== HiTag S Debug Trace ===

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 00 bits=45 tx_us=9999
  RESULT: TIMEOUT (0 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("TX frame check: MISMATCH", report)
        self.assertIn("expected=02 94 08 11 8B 78", report)
        self.assertIn("expected_tx_us=8512", report)

    def test_each_select_capture_frame_is_checked_against_its_own_tx(self):
        trace = """=== HiTag S Debug Trace ===

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  TX: SELECT UID=31028152 CRC=E9 (45 bits, UID3..UID0)
  TX_FRAME: frame=01 88 14 0A 97 48 bits=45 tx_us=8448
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("SELECT TX frame check: OK (02 94 08 11 8B 78, 45 bits, tx_us=8512)", report)
        self.assertIn("SELECT TX frame check: OK (01 88 14 0A 97 48, 45 bits, tx_us=8448)", report)

    def test_select_capture_frame_mismatch_points_to_specific_tx(self):
        trace = """=== HiTag S Debug Trace ===

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 00 bits=45 tx_us=8512
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("SELECT TX frame check: MISMATCH", report)
        self.assertIn("after TX: SELECT UID=52810231", report)

    def test_select_timeout_with_valid_frame_is_diagnosed_as_rf_or_response_window(self):
        trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  RESULT: OK, UID=52810231 (mode=STD, AC2K)

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)

        self.assertIn("SELECT frame matches model but no MC4K response was decoded", report)
        self.assertIn("RF field/coil coupling or response-window timing", report)

    def test_select_partial_response_reports_sweep_still_too_short(self):
        trace = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RESULT: OK, UID=52810231 (mode=STD, AC2K)

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 6 edges mode=MC4K
  EDGES: L:5 H:256 L:128 H:128 L:256 H:14995
  DECODE: 2 bits = 40
  RESULT: TIMEOUT (2 bits)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed, redecode=True)

        self.assertIn("MC4K sweep still only recovers", report)
        self.assertIn("response window/RF coupling", report)

    def test_batch_summary_highlights_trace_evidence_state(self):
        old_trace = """=== HiTag S Debug Trace ===

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""
        new_trace = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""

        summary = analyze_trace.generate_batch_summary([
            ("old.htsd", analyze_trace.parse_trace(old_trace)),
            ("new.htsd", analyze_trace.parse_trace(new_trace)),
        ])

        self.assertIn("Batch Summary", summary)
        self.assertIn(
            "old.htsd | mode=STD | uid=- | accepted=- | marginal=- | start01=- | partial_uid=0 | "
            "empty_uid=0 | field=legacy | "
            "uid_tx=legacy | select_tx=legacy",
            summary,
        )
        self.assertIn(
            "new.htsd | mode=STD | uid=- | accepted=- | marginal=- | start01=- | partial_uid=0 | "
            "empty_uid=0 | field=release | "
            "uid_tx=ok | select_tx=ok",
            summary,
        )
        self.assertIn("select_bits=0", summary)
        self.assertIn("legacy-insufficient", summary)
        self.assertIn("uid-rf-or-window", summary)

    def test_batch_summary_separates_uid_and_select_failure_causes(self):
        uid_frame_bad = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=38 bits=5 tx_us=1280
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
"""
        select_no_response = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RESULT: OK, UID=52810231 (mode=STD, AC2K)

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 2 edges mode=MC4K
  EDGES: L:5 H:14995
  DECODE: 0 bits
  RESULT: TIMEOUT (0 bits)
"""
        select_partial = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RESULT: OK, UID=52810231 (mode=STD, AC2K)

--- SELECT ---
  TX: SELECT UID=52810231 CRC=6F (45 bits, UID0..UID3)
  TX_FRAME: frame=02 94 08 11 8B 78 bits=45 tx_us=8512
  RX: 6 edges mode=MC4K
  EDGES: L:5 H:256 L:128 H:128 L:256 H:14995
  DECODE: 2 bits = 40
  RESULT: TIMEOUT (2 bits)
"""

        summary = analyze_trace.generate_batch_summary([
            ("bad_uid.htsd", analyze_trace.parse_trace(uid_frame_bad)),
            ("select_none.htsd", analyze_trace.parse_trace(select_no_response)),
            ("select_partial.htsd", analyze_trace.parse_trace(select_partial)),
        ])

        self.assertIn("bad_uid.htsd", summary)
        self.assertIn("fix-uid-frame", summary)
        self.assertIn("select_none.htsd", summary)
        self.assertIn("select-rf-or-window", summary)
        self.assertIn("select_partial.htsd", summary)
        self.assertIn("select-decode-threshold", summary)

    def test_trace_replay_recovers_real_ac2k_uid(self):
        trace_path = Path(__file__).resolve().parents[1] / "trace_device_52810231_pm3timing.htsd"
        if not trace_path.exists():
            self.skipTest("hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        decoded = []
        for txn in parsed.transactions:
            if txn.section != "UID_REQUEST":
                continue
            for cap in txn.captures:
                if cap.mode != "AC2K":
                    continue
                bits, data = analyze_trace.decode_ac2k(cap.edges, sof_bits=0)
                if bits == 32 and not analyze_trace.ac2k_quality(cap.edges)["too_noisy"]:
                    decoded.append(data[:4].hex().upper())

        self.assertIn("52810231", decoded)

    def test_trace_replay_flags_noisy_false_uid_candidate(self):
        trace_path = Path(__file__).resolve().parents[1] / "trace_device_D4A0408C_variants.htsd"
        if not trace_path.exists():
            self.skipTest("hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        noisy_candidates = []
        for txn in parsed.transactions:
            if txn.section != "UID_REQUEST":
                continue
            for cap in txn.captures:
                if cap.mode != "AC2K":
                    continue
                bits, data = analyze_trace.decode_ac2k(cap.edges, sof_bits=0)
                if bits >= 32 and data[:4].hex().upper() == "D4A0408C":
                    noisy_candidates.append(analyze_trace.ac2k_quality(cap.edges))

        self.assertTrue(noisy_candidates)
        self.assertTrue(all(candidate["too_noisy"] for candidate in noisy_candidates))

    def test_trace_replay_flags_long_gap_false_uid_candidate(self):
        trace_path = (
            Path(__file__).resolve().parents[1] /
            "trace_device_12810231_after_no_adv_fallback.htsd"
        )
        if not trace_path.exists():
            self.skipTest("hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        false_candidates = []
        for txn in parsed.transactions:
            if txn.section != "UID_REQUEST":
                continue
            for cap in txn.captures:
                if cap.mode != "AC2K":
                    continue
                bits, data = analyze_trace.decode_ac2k(cap.edges, sof_bits=0)
                if bits == 32 and data[:4].hex().upper() == "12810231":
                    false_candidates.append(analyze_trace.ac2k_quality(cap.edges))

        self.assertTrue(false_candidates)
        self.assertTrue(all(candidate["too_noisy"] for candidate in false_candidates))

    def test_uid_candidate_requires_exactly_32_clean_ac2k_bits(self):
        fixture_names = {
            "trace_device_52810231_pm3timing.htsd": {"52810231"},
            "trace_device_D2810231_after_filter.htsd": set(),
            "trace_device_74A0408C_new.htsd": set(),
        }

        for fixture_name, expected_uids in fixture_names.items():
            trace_path = Path(__file__).resolve().parents[1] / fixture_name
            if not trace_path.exists():
                self.skipTest(f"hardware trace fixture not present: {fixture_name}")

            parsed = analyze_trace.parse_trace(trace_path.read_text())
            accepted_uids = set()
            for txn in parsed.transactions:
                if txn.section != "UID_REQUEST":
                    continue
                for cap in txn.captures:
                    if cap.mode != "AC2K":
                        continue
                    bits, data = analyze_trace.decode_ac2k(cap.edges, sof_bits=0)
                    if analyze_trace.is_valid_ac2k_uid_capture(bits, cap.edges):
                        accepted_uids.add(data[:4].hex().upper())

            self.assertEqual(accepted_uids, expected_uids, fixture_name)

    def test_batch_uid_candidate_summary_matches_hardware_trace_fixtures(self):
        fixture_names = {
            "trace_device_00000000.htsd": set(),
            "trace_device_00000000_after_idle_rx.htsd": set(),
            "trace_device_00000000_current.htsd": set(),
            "trace_device_00000000_current_after_inter_wait.htsd": set(),
            "trace_device_00000000_new.htsd": set(),
            "trace_device_00000000_reject_unreliable_uid.htsd": set(),
            "trace_device_12810231_after_no_adv_fallback.htsd": set(),
            "trace_device_28100000.htsd": set(),
            "trace_device_48000000_new.htsd": {"52810231"},
            "trace_device_52810231_new.htsd": {"52810231"},
            "trace_device_52810231_pm3timing.htsd": {"52810231"},
            "trace_device_74A0408C_new.htsd": set(),
            "trace_device_D2810231_after_filter.htsd": set(),
            "trace_device_D4A0408C_variants.htsd": set(),
        }

        for fixture_name, expected_uids in fixture_names.items():
            trace_path = Path(__file__).resolve().parents[1] / fixture_name
            if not trace_path.exists():
                self.skipTest(f"hardware trace fixture not present: {fixture_name}")

            parsed = analyze_trace.parse_trace(trace_path.read_text())
            self.assertEqual(
                analyze_trace.accepted_uid_candidates(parsed),
                expected_uids,
                fixture_name,
            )

    def test_latest_no_uid_trace_recovers_uid_after_startup_noise_filter(self):
        trace_path = (
            Path(__file__).resolve().parents[1] /
            "pulled_traces/flipper_Trace_NoUID_6A1C3A0A_15028b.htsd"
        )
        if not trace_path.exists():
            self.skipTest("latest hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        summary = analyze_trace.generate_batch_summary([(str(trace_path), parsed)])

        self.assertIn("52810231", analyze_trace.accepted_uid_candidates(parsed))
        self.assertNotIn("52810231", analyze_trace.marginal_uid_candidates(parsed))
        self.assertIn("uid-offline-recovered", summary)

    def test_latest_no_uid_trace_recovers_uid_from_start01_consensus(self):
        trace_path = (
            Path(__file__).resolve().parents[1] /
            "pulled_traces/flipper_Trace_NoUID_6A1C3C24_12263b.htsd"
        )
        if not trace_path.exists():
            self.skipTest("latest hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        summary = analyze_trace.generate_batch_summary([(str(trace_path), parsed)])

        self.assertEqual(analyze_trace.start01_uid_consensus(parsed), ("52810231", 10))
        self.assertIn("uid-start01-consensus", summary)

    def test_start01_consensus_rejects_low_vote_old_false_candidate(self):
        trace_path = Path(__file__).resolve().parents[1] / "trace_device_74A0408C_new.htsd"
        if not trace_path.exists():
            self.skipTest("hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())

        self.assertIsNone(analyze_trace.start01_uid_consensus(parsed))

    def test_latest_no_uid_trace_is_diagnosed_as_uid_preamble_loss(self):
        trace_path = (
            Path(__file__).resolve().parents[1] /
            "pulled_traces/flipper_Trace_NoUID_6A1C3EEC_15296b.htsd"
        )
        if not trace_path.exists():
            self.skipTest("latest hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        summary = analyze_trace.generate_batch_summary([(str(trace_path), parsed)])

        self.assertGreaterEqual(analyze_trace.partial_uid_response_count(parsed), 8)
        self.assertIn("uid-preamble-loss", summary)
        self.assertNotIn("uid-rf-or-window", summary)

    def test_cold_retry_trace_is_diagnosed_as_empty_uid_responses(self):
        trace = """=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
  RX_META: elapsed_us=25000 idle_us=0 timeout_us=25000 final_edges=2 early_rx=stop_tail
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RX_META: elapsed_us=25000 idle_us=0 timeout_us=25000 final_edges=2 early_rx=stop_tail
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RX_META: elapsed_us=25000 idle_us=0 timeout_us=25000 final_edges=2 early_rx=stop_tail
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RX_META: elapsed_us=25000 idle_us=0 timeout_us=25000 final_edges=2 early_rx=stop_tail
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RX_META: elapsed_us=25000 idle_us=0 timeout_us=25000 final_edges=2 early_rx=stop_tail
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RX_META: elapsed_us=25000 idle_us=0 timeout_us=25000 final_edges=2 early_rx=stop_tail
  RX: 2 edges mode=AC2K
  EDGES: L:5 H:25000
  DECODE: 0 bits
  RESULT: TIMEOUT (no UID)
ABORT: UID request failed (result=1)
"""

        parsed = analyze_trace.parse_trace(trace)
        summary = analyze_trace.generate_batch_summary([("empty.htsd", parsed)])

        self.assertEqual(analyze_trace.empty_uid_response_count(parsed), 6)
        self.assertIn("uid-empty-response", summary)
        self.assertNotIn("uid-rf-or-window", summary)

    def test_start01_low_entropy_false_uid_is_rejected_by_candidate_model(self):
        captures = "\n".join(
            f"""  RX: 33 edges mode=AC2K
  EDGES: {ac2k_zero_start01_edge_text()}
  DECODE: 31 bits = 00 00 00 00"""
            for _ in range(8)
        )
        trace = f"""=== HiTag S Debug Trace ===

=== DEBUG READ SEQUENCE ===
Field ON: carrier=125000Hz duty=0.5 pull=release powerup_us=3000

--- UID_REQUEST ---
  TX: UID_REQ_STD (5 bits, val=0x06)
  TX_FRAME: frame=30 bits=5 tx_us=1280
{captures}
  RESULT: TIMEOUT (no UID)
"""

        parsed = analyze_trace.parse_trace(trace)
        report = analyze_trace.generate_report(parsed)
        summary = analyze_trace.generate_batch_summary([("zero_start01.htsd", parsed)])
        candidates = analyze_trace.uid_candidate_summary(parsed)

        self.assertIsNone(analyze_trace.start01_uid_consensus(parsed))
        self.assertIn("40000000", report)
        self.assertIn("low-entropy", report)
        self.assertIn("uid-fallback-low-entropy-rejected", summary)
        self.assertTrue(
            any(
                candidate.uid == "40000000" and candidate.reject_reason == "low-entropy"
                for candidate in candidates
            )
        )

    def test_latest_cold_retry_trace_reports_rejected_low_entropy_start01(self):
        trace_path = (
            Path(__file__).resolve().parents[1] /
            "pulled_traces/flipper_Trace_NoUID_6A1C450A_15187b.htsd"
        )
        if not trace_path.exists():
            self.skipTest("latest cold retry hardware trace fixture not present")

        parsed = analyze_trace.parse_trace(trace_path.read_text())
        summary = analyze_trace.generate_batch_summary([(str(trace_path), parsed)])
        candidates = analyze_trace.uid_candidate_summary(parsed)

        self.assertEqual(analyze_trace.partial_uid_response_count(parsed), 6)
        self.assertIsNone(analyze_trace.start01_uid_consensus(parsed))
        self.assertIn("uid-fallback-low-entropy-rejected", summary)
        self.assertTrue(
            any(
                candidate.uid == "40000000" and candidate.reject_reason == "low-entropy"
                for candidate in candidates
            )
        )


if __name__ == "__main__":
    unittest.main()
