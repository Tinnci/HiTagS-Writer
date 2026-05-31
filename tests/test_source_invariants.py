import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SourceInvariantTests(unittest.TestCase):
    def test_app_allocation_initializes_debug_trace_pointer(self):
        source = (ROOT / "hitags_writer_main.c").read_text()
        alloc_block = source.split("static HitagSApp* hitags_writer_alloc", 1)[1].split(
            "/* Open services */", 1
        )[0]

        self.assertIn("app->debug_trace = NULL", alloc_block)

    def test_dump_save_does_not_serialize_unread_pages_as_zero_valid_pages(self):
        source = (ROOT / "hitag_s_dump_file.c").read_text()
        save_block = source.split("bool hitag_s_dump_save", 1)[1].split(
            "bool hitag_s_dump_load", 1
        )[0]
        page_loop = re.search(
            r"for\(int p = 0; p <= max_page; p\+\+\) \{(?P<body>.*?)\n        \}",
            save_block,
            re.S,
        )

        self.assertIsNotNone(page_loop)
        self.assertNotIn("empty[4]", page_loop.group("body"))
        self.assertNotIn("Write placeholder", page_loop.group("body"))

    def test_ac2k_quality_check_does_not_copy_capture_buffer_onto_worker_stack(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        quality_block = source.split("static bool hitag_s_capture_has_excessive_glitches", 1)[
            1
        ].split("/**\n * @brief Decode MC4K", 1)[0]

        self.assertNotIn("HitagSEdge edges[HITAG_S_MAX_EDGES]", quality_block)

    def test_config_codec_uses_explicit_bit_masks_not_bitfield_memcpy(self):
        header = (ROOT / "hitag_s_proto.h").read_text()
        codec_block = header.split("static inline HitagSConfig hitag_s_parse_config", 1)[
            1
        ].split("/* --- BPLM timing constants", 1)[0]

        self.assertNotIn("memcpy(&cfg", codec_block)
        self.assertNotIn("memcpy(bytes", codec_block)

    def test_trace_module_header_declares_debug_trace_lifecycle(self):
        header = (ROOT / "hitag_s_trace.h").read_text()

        self.assertIn("void hitag_s_debug_trace_start(void)", header)
        self.assertIn("void* hitag_s_debug_trace_stop(void)", header)
        self.assertIn("bool hitag_s_debug_trace_save", header)

    def test_select_config_fallback_is_not_marked_as_real_page_read(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        self.assertIn("pages[1] = config;\n        page_valid[1] = false;", source)
        self.assertIn("pages[1] = config;\n        page_valid[1] = false;\n        trace_append(\"  (using config from SELECT)\\n\");", source)

    def test_session_and_8268_operations_live_outside_proto_module(self):
        proto = (ROOT / "hitag_s_proto.c").read_text()
        session = (ROOT / "hitag_s_session.c").read_text()
        operations = (ROOT / "hitag_s_8268.c").read_text()

        self.assertNotIn("HitagSResult hitag_s_uid_request", proto)
        self.assertNotIn("HitagSResult hitag_s_select", proto)
        self.assertNotIn("HitagSResult hitag_s_8268_write_sequence", proto)
        self.assertNotIn("HitagSResult hitag_s_debug_read_sequence", proto)

        self.assertIn("HitagSResult hitag_s_uid_request", session)
        self.assertIn("HitagSResult hitag_s_select", session)
        self.assertIn("HitagSResult hitag_s_read_page", session)
        self.assertIn("HitagSResult hitag_s_write_page", session)

        self.assertIn("HitagSResult hitag_s_8268_write_sequence", operations)
        self.assertIn("HitagSResult hitag_s_8268_read_all", operations)
        self.assertIn("HitagSResult hitag_s_debug_read_sequence", operations)

    def test_select_trace_includes_packed_frame_bytes_for_model_comparison(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        select_frame = source.split("static HitagSResult hitag_s_select_frame", 1)[1].split(
            "HitagSResult hitag_s_select", 1
        )[0]

        self.assertIn("frame=", select_frame)
        self.assertIn("tx_us=", select_frame)
        self.assertIn("cmd[0]", select_frame)
        self.assertIn("cmd[5]", select_frame)


if __name__ == "__main__":
    unittest.main()
