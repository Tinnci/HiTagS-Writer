import unittest
import contextlib
import io
import subprocess
import tempfile
from pathlib import Path

import trace_pull


class TracePullTests(unittest.TestCase):
    def test_parse_trace_listing_finds_uid_and_no_uid_traces(self):
        listing = """/ext/lfrfid/Gate.rfid, size 76b
/ext/lfrfid/Trace_52810231.htsd, size 1414b
/ext/lfrfid/Trace_52810231_692C1901.htsd, size 2448b
/ext/lfrfid/Trace_NoUID_692C1800.htsd, size 1796b
/ext/lfrfid/assets
"""

        self.assertEqual(
            trace_pull.parse_trace_listing(listing),
            [
                ("/ext/lfrfid/Trace_52810231.htsd", 1414),
                ("/ext/lfrfid/Trace_52810231_692C1901.htsd", 2448),
                ("/ext/lfrfid/Trace_NoUID_692C1800.htsd", 1796),
            ],
        )

    def test_local_trace_name_preserves_remote_identity_and_size(self):
        self.assertEqual(
            trace_pull.local_trace_name("/ext/lfrfid/Trace_NoUID_692C1800.htsd", 1796),
            "flipper_Trace_NoUID_692C1800_1796b.htsd",
        )
        self.assertEqual(
            trace_pull.local_trace_name("/ext/lfrfid/Trace_52810231_692C1901.htsd", 2448),
            "flipper_Trace_52810231_692C1901_2448b.htsd",
        )

    def test_pull_traces_continues_after_one_receive_failure(self):
        calls = []

        def fake_run_storage(storage_script, port, args):
            calls.append(args)
            if args == ["list", "/ext/lfrfid"]:
                return """/ext/lfrfid/Trace_52810231.htsd, size 1414b
/ext/lfrfid/Trace_12810231.htsd, size 2437b
"""
            if args[1] == "/ext/lfrfid/Trace_12810231.htsd":
                raise subprocess.CalledProcessError(
                    1,
                    ["storage.py", *args],
                    output="",
                    stderr="receive failed",
                )
            return ""

        original = trace_pull.run_storage
        trace_pull.run_storage = fake_run_storage
        try:
            with tempfile.TemporaryDirectory() as tmp_dir:
                with contextlib.redirect_stderr(io.StringIO()):
                    pulled = trace_pull.pull_traces(Path("/tmp/storage.py"), None, Path(tmp_dir))
        finally:
            trace_pull.run_storage = original

        self.assertEqual(len(pulled), 1)
        self.assertEqual(pulled[0].name, "flipper_Trace_52810231_1414b.htsd")
        self.assertEqual(len([call for call in calls if call[0] == "receive"]), 2)


if __name__ == "__main__":
    unittest.main()
