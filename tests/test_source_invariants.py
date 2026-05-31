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

    def test_hitag_s_field_on_releases_lf_antenna_pull(self):
        source = (ROOT / "hitag_s_transport.c").read_text()
        field_on = source.split("void hitag_s_field_on", 1)[1].split(
            "void hitag_s_field_off", 1
        )[0]

        self.assertIn("furi_hal_rfid_tim_read_start(125000", field_on)
        self.assertIn("furi_hal_rfid_pin_pull_release()", field_on)
        self.assertNotIn("furi_hal_rfid_pin_pull_pulldown()", field_on)

    def test_debug_trace_records_lf_field_drive_mode(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        debug_read = source.split("HitagSResult hitag_s_debug_read_sequence", 1)[1]

        self.assertIn("carrier=125000Hz", debug_read)
        self.assertIn("pull=release", debug_read)
        self.assertIn("powerup_us=", debug_read)

    def test_uid_request_trace_includes_packed_frame_bytes_for_model_comparison(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]

        self.assertIn("TX_FRAME: frame=", uid_request)
        self.assertIn("tx_us=", uid_request)
        self.assertIn("cmd[0]", uid_request)

    def test_app_menu_matches_official_style_grouped_flow(self):
        start = (ROOT / "scenes/hitags_writer_scene_start.c").read_text()
        config = (ROOT / "scenes/hitags_writer_scene_config.h").read_text()
        read_menu = (ROOT / "scenes/hitags_writer_scene_read_menu.c").read_text()
        write_menu = (ROOT / "scenes/hitags_writer_scene_write_menu.c").read_text()
        dump_menu = (ROOT / "scenes/hitags_writer_scene_dump_menu.c").read_text()
        tools_menu = (ROOT / "scenes/hitags_writer_scene_tools_menu.c").read_text()

        for item in ('"Read"', '"Write"', '"Dump"', '"Tools"'):
            self.assertIn(item, start)

        for old_top_level in (
            '"Write EM4100 ID"',
            '"Load from File"',
            '"Read Tag Data"',
            '"Read Tag UID"',
            '"Write Tag UID"',
            '"Full Tag Dump"',
            '"Load & Clone Dump"',
            '"Wipe Tag"',
            '"Debug Read"',
            '"About"',
        ):
            self.assertNotIn(old_top_level, start)

        self.assertIn("ADD_SCENE(hitags_writer, read_menu, ReadMenu)", config)
        self.assertIn("ADD_SCENE(hitags_writer, write_menu, WriteMenu)", config)
        self.assertIn("ADD_SCENE(hitags_writer, dump_menu, DumpMenu)", config)
        self.assertIn("ADD_SCENE(hitags_writer, tools_menu, ToolsMenu)", config)

        self.assertIn('"Read EM4100"', read_menu)
        self.assertIn('"Read 8268 Pages"', read_menu)
        self.assertIn('"Read 8268 UID"', read_menu)
        self.assertIn("HitagSSceneReadEm4100", read_menu)
        self.assertIn("HitagSSceneReadTag", read_menu)
        self.assertIn("HitagSSceneReadUid", read_menu)

        self.assertIn('"Write EM4100 ID"', write_menu)
        self.assertIn('"Load from File"', write_menu)
        self.assertIn('"Write Tag UID"', write_menu)
        self.assertIn("HitagSSceneInputId", write_menu)
        self.assertIn("HitagSSceneSelectFile", write_menu)
        self.assertIn("HitagSSceneWriteUid", write_menu)

        self.assertIn('"Full Tag Dump"', dump_menu)
        self.assertIn('"Load & Clone Dump"', dump_menu)
        self.assertIn("HitagSSceneFullDump", dump_menu)
        self.assertIn("HitagSSceneLoadDump", dump_menu)
        self.assertNotIn("HitagSSceneDebugRead", dump_menu)

        self.assertIn('"Wipe Tag"', tools_menu)
        self.assertIn('"Debug Read"', tools_menu)
        self.assertIn('"About"', tools_menu)
        self.assertIn("HitagSSceneWipeTag", tools_menu)
        self.assertIn("HitagSSceneDebugRead", tools_menu)
        self.assertIn("HitagSSceneAbout", tools_menu)

    def test_read_flow_uses_official_lfrfid_worker_for_em4100_air_read(self):
        header = (ROOT / "hitags_writer_i.h").read_text()
        main = (ROOT / "hitags_writer_main.c").read_text()
        config = (ROOT / "scenes/hitags_writer_scene_config.h").read_text()
        read_menu = (ROOT / "scenes/hitags_writer_scene_read_menu.c").read_text()
        read_em4100_path = ROOT / "scenes/hitags_writer_scene_read_em4100.c"
        self.assertTrue(read_em4100_path.exists())
        read_em4100 = read_em4100_path.read_text()
        read_pages = (ROOT / "scenes/hitags_writer_scene_read_tag.c").read_text()

        self.assertIn("#include <lfrfid/lfrfid_worker.h>", header)
        self.assertIn("LFRFIDWorker* lfworker", header)
        self.assertIn("LFRFIDWorkerReadType read_type", header)
        self.assertIn("ProtocolId protocol_id_next", header)

        self.assertIn("lfrfid_worker_alloc(app->dict)", main)
        self.assertIn("lfrfid_worker_free(app->lfworker)", main)

        self.assertIn("ADD_SCENE(hitags_writer, read_em4100, ReadEm4100)", config)
        self.assertIn("ADD_SCENE(hitags_writer, read_em4100_success, ReadEm4100Success)", config)

        self.assertIn('"Read EM4100"', read_menu)
        self.assertIn('"Read 8268 Pages"', read_menu)
        self.assertIn('"Read 8268 UID"', read_menu)
        self.assertIn("HitagSSceneReadEm4100", read_menu)

        self.assertIn("lfrfid_worker_read_start", read_em4100)
        self.assertIn("LFRFIDWorkerReadTypeAuto", read_em4100)
        self.assertIn("HitagSSceneReadEm4100Success", read_em4100)
        self.assertIn("HitagSViewPopup", read_em4100)
        self.assertNotIn("HitagSWorkerReadPages", read_em4100)

        self.assertNotIn("No tag found", read_pages)
        self.assertIn("8268 protocol", read_pages)


if __name__ == "__main__":
    unittest.main()
