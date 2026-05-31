import unittest

import trace_pull


class TracePullTests(unittest.TestCase):
    def test_parse_trace_listing_finds_uid_and_no_uid_traces(self):
        listing = """/ext/lfrfid/Gate.rfid, size 76b
/ext/lfrfid/Trace_52810231.htsd, size 1414b
/ext/lfrfid/Trace_NoUID_692C1800.htsd, size 1796b
/ext/lfrfid/assets
"""

        self.assertEqual(
            trace_pull.parse_trace_listing(listing),
            [
                ("/ext/lfrfid/Trace_52810231.htsd", 1414),
                ("/ext/lfrfid/Trace_NoUID_692C1800.htsd", 1796),
            ],
        )

    def test_local_trace_name_preserves_remote_identity_and_size(self):
        self.assertEqual(
            trace_pull.local_trace_name("/ext/lfrfid/Trace_NoUID_692C1800.htsd", 1796),
            "flipper_Trace_NoUID_692C1800_1796b.htsd",
        )


if __name__ == "__main__":
    unittest.main()
