import unittest
from pathlib import Path

import analyze_trace
import sim_mc4k_final


def mc4k_edge_text(value: int, sof_bits: int = 1) -> str:
    bits = [(value >> (31 - i)) & 1 for i in range(32)]
    waveform = sim_mc4k_final.generate_comp1_waveform([1] * sof_bits + bits)
    events = sim_mc4k_final.simulate_tim2(waveform)
    return " ".join(f"{'H' if level else 'L'}:{duration}" for level, duration in events)


class AnalyzeTraceTests(unittest.TestCase):
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
                    for sof_bits in (0, 3):
                        bits, data = analyze_trace.decode_ac2k(cap.edges, sof_bits=sof_bits)
                        if analyze_trace.is_valid_ac2k_uid_capture(bits, cap.edges):
                            accepted_uids.add(data[:4].hex().upper())

            self.assertEqual(accepted_uids, expected_uids, fixture_name)


if __name__ == "__main__":
    unittest.main()
