import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ProtocolTimingTests(unittest.TestCase):
    def test_select_uses_compensated_inter_command_wait(self):
        header = (ROOT / "hitag_s_proto.h").read_text()
        source = (ROOT / "hitag_s_session.c").read_text()

        self.assertRegex(header, r"#define\s+HITAG_S_T_WAIT_INTER_US\s+400")
        select_frame = source.split("static HitagSResult hitag_s_select_frame", 1)[1].split(
            "HitagSResult hitag_s_select", 1
        )[0]

        self.assertIn("HITAG_S_T_WAIT_INTER_US", select_frame)
        self.assertNotIn("furi_delay_us(HITAG_S_T_WAIT_SC_US)", select_frame)


if __name__ == "__main__":
    unittest.main()
