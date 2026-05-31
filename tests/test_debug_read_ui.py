import unittest
from pathlib import Path


class DebugReadUiTests(unittest.TestCase):
    def test_partial_trace_is_not_presented_as_failure(self):
        scene = (
            Path(__file__).resolve().parents[1] /
            "scenes/hitags_writer_scene_debug_read.c"
        ).read_text()
        partial_block = scene.split("HitagSEventDebugPartial", 1)[1].split(
            "HitagSEventDebugFailed", 1
        )[0]

        self.assertIn('"Trace Ready"', partial_block)
        self.assertIn("&sequence_success", partial_block)
        self.assertNotIn("&sequence_error", partial_block)
        self.assertNotIn('"Partial Trace"', partial_block)

    def test_no_uid_trace_save_uses_unique_no_uid_filename(self):
        scene = (
            Path(__file__).resolve().parents[1] /
            "scenes/hitags_writer_scene_debug_read.c"
        ).read_text()
        save_block = scene.split("hitags_writer_scene_debug_read_save_trace", 1)[1].split(
            "void hitags_writer_scene_debug_read_on_enter", 1
        )[0]

        self.assertIn("app->tag_uid == 0", save_block)
        self.assertIn('"Trace_NoUID_%08lX"', save_block)
        self.assertIn("furi_hal_rtc_get_timestamp", save_block)

    def test_uid_trace_save_uses_unique_uid_filename(self):
        scene = (
            Path(__file__).resolve().parents[1] /
            "scenes/hitags_writer_scene_debug_read.c"
        ).read_text()
        save_block = scene.split("hitags_writer_scene_debug_read_save_trace", 1)[1].split(
            "void hitags_writer_scene_debug_read_on_enter", 1
        )[0]

        self.assertIn("app->tag_uid == 0", save_block)
        self.assertIn('"Trace_%08lX_%08lX"', save_block)
        self.assertIn("app->tag_uid", save_block)
        self.assertIn("timestamp", save_block)

    def test_debug_read_resets_stale_uid_before_capture(self):
        worker = (Path(__file__).resolve().parents[1] / "hitags_worker.c").read_text()
        debug_read = worker.split("static void hitags_worker_debug_read", 1)[1].split(
            "int32_t hitags_writer_worker_thread", 1
        )[0]
        init_block = debug_read.split("if(app->debug_trace)", 1)[0]

        self.assertIn("app->tag_uid = 0", init_block)


if __name__ == "__main__":
    unittest.main()
