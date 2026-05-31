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

    def test_trace_buffer_has_memory_budget_and_truncation_marker(self):
        source = (ROOT / "hitag_s_trace.c").read_text()

        self.assertRegex(source, r"HITAG_S_TRACE_MAX_BYTES\s+\(12U \* 1024U\)")
        self.assertIn("HITAG_S_TRACE_APPEND_MAX", source)
        self.assertIn("g_trace_truncated", source)
        self.assertIn("TRACE TRUNCATED", source)
        self.assertIn("furi_string_left", source)
        self.assertIn("vsnprintf", source)

    def test_rx_trace_limits_raw_edge_dump_size(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        send_receive = source.split("static size_t hitag_s_send_receive", 1)[1].split(
            "/* ============================================================\n"
            " * Hitag S Command Builders",
            1,
        )[0]

        self.assertRegex(source, r"HITAG_S_TRACE_MAX_EDGES_PER_RX\s+24")
        self.assertIn("trace_edge_count", send_receive)
        self.assertIn("truncated_edges", send_receive)
        self.assertNotIn("i < hs_capture.edge_count; i++", send_receive)

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

    def test_debug_read_abort_paths_emit_trace_summary_before_return(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        debug_read = source.split("HitagSResult hitag_s_debug_read_sequence_ex", 1)[1]

        self.assertIn("hitag_s_debug_read_finish", source)
        self.assertIn("ABORT: UID/SELECT session failed", debug_read)
        self.assertIn("ABORT: AUTH failed", debug_read)
        self.assertGreaterEqual(debug_read.count("hitag_s_debug_read_finish("), 3)
        self.assertEqual(debug_read.count("hitag_s_field_off()"), 0)

    def test_em4100_write_verifies_final_config_page(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        write_em = source.split("HitagSResult hitag_s_8268_write_em4100_sequence", 1)[1].split(
            "HitagSResult hitag_s_8268_read_sequence", 1
        )[0]

        self.assertIn("hitag_s_write_page_verify(4, em_data->data_hi)", write_em)
        self.assertIn("hitag_s_write_page_verify(5, em_data->data_lo)", write_em)
        self.assertIn("hitag_s_write_page_verify(1, new_config)", write_em)
        self.assertNotIn("hitag_s_write_page(1, new_config)", write_em)

    def test_write_page_records_trace_for_offline_write_diagnosis(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        write_page = source.split("HitagSResult hitag_s_write_page", 1)[1].split(
            "HitagSResult hitag_s_read_page", 1
        )[0]

        self.assertIn("--- WRITE_PAGE %d ---", write_page)
        self.assertIn("TX: WRITE_PAGE addr=%d CRC=%02X", write_page)
        self.assertIn("TX_FRAME: frame=", write_page)
        self.assertIn("step1: ACK OK", write_page)
        self.assertIn("TX: Data=%08lX CRC=%02X", write_page)
        self.assertIn("step2: ACK OK", write_page)

    def test_uid_request_trace_includes_packed_frame_bytes_for_model_comparison(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]

        self.assertIn("TX_FRAME: frame=", uid_request)
        self.assertIn("tx_us=", uid_request)
        self.assertIn("cmd[0]", uid_request)

    def test_uid_request_keeps_marginal_32_bit_uid_as_fallback(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]

        self.assertIn("marginal_uid_valid", uid_request)
        self.assertIn("hitag_s_capture_is_marginal_uid_candidate", uid_request)
        self.assertIn("RESULT: OK, UID=%08lX (mode=%s, %s, marginal)", uid_request)
        self.assertIn("using marginal noisy UID", uid_request)

    def test_uid_request_uses_high_vote_start01_consensus_as_last_fallback(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]

        self.assertIn("HITAG_S_START01_CONSENSUS_MIN", source)
        self.assertIn("hitag_s_decode_ac2k_start01", source)
        self.assertIn("start01_consensus", uid_request)
        self.assertIn("RESULT: OK, UID=%08lX (mode=start01-consensus, AC2K)", uid_request)

    def test_start01_fallback_is_scored_and_requires_select_verification(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]
        select = source.split("static HitagSResult hitag_s_select_frame", 1)[1].split(
            "HitagSResult hitag_s_8268_authenticate", 1
        )[0]

        self.assertIn("hitag_s_codec_is_acceptable_start01_uid", uid_request)
        self.assertIn("hitag_s_codec_is_low_entropy_uid", uid_request)
        self.assertIn("active_uid_requires_select_verification = true", uid_request)
        self.assertIn("active_uid_requires_select_verification = false", uid_request)
        self.assertIn("fallback UID unverified by SELECT", select)

    def test_uid_request_tracks_partial_preamble_loss_and_cold_retries(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]

        self.assertIn("hitag_s_capture_is_partial_uid_response", source)
        self.assertIn("partial_uid_responses", uid_request)
        self.assertIn("empty_uid_responses", uid_request)
        self.assertIn("cold retry after repeated partial UID responses", uid_request)
        self.assertIn("cold retry after repeated empty UID responses", uid_request)
        self.assertIn("hitag_s_field_off()", uid_request)
        self.assertIn("hitag_s_field_on()", uid_request)
        self.assertIn("hitag_s_start01_votes_reset", uid_request)

    def test_all_uid_request_modes_decode_uid_without_extra_sof_strip(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        proto_modes = source.split("static const HitagSProtoMode proto_modes[]", 1)[1].split(
            "static size_t active_mode_idx",
            1,
        )[0]

        self.assertIn('"FADV"', proto_modes)
        self.assertIn('"ADV1"', proto_modes)
        self.assertIn('"ADV2"', proto_modes)
        self.assertIn('"STD"', proto_modes)
        self.assertLess(proto_modes.index('"FADV"'), proto_modes.index('"ADV1"'))
        self.assertLess(proto_modes.index('"ADV1"'), proto_modes.index('"ADV2"'))
        self.assertLess(proto_modes.index('"ADV2"'), proto_modes.index('"STD"'))
        self.assertIn("HitagSRxAC4K", proto_modes)
        self.assertIn("HitagSRxMC8K", proto_modes)
        self.assertIn("select_response_bits", proto_modes)

    def test_public_debug_read_session_types_are_declared(self):
        header = (ROOT / "hitag_s_proto.h").read_text()

        for token in (
            "typedef enum {\n    HitagSModeStd",
            "HitagSModeAdv1",
            "HitagSModeAdv2",
            "HitagSModeFadv",
            "HitagSRxAC4K",
            "HitagSRxMC8K",
            "HitagSSessionInfo",
            "HitagSPageStatus",
            "HitagSDebugReadReport",
            "hitag_s_open_session",
            "hitag_s_debug_read_sequence_ex",
        ):
            self.assertIn(token, header)

    def test_session_data_rx_comes_from_active_mode(self):
        source = (ROOT / "hitag_s_session.c").read_text()

        self.assertIn("hitag_s_data_rx_mode()", source)
        self.assertIn("hitag_s_select_expected_bits()", source)

        for block_name, next_name in (
            ("static HitagSResult hitag_s_select_frame", "HitagSResult hitag_s_select"),
            ("HitagSResult hitag_s_8268_authenticate", "HitagSResult hitag_s_write_page"),
            ("HitagSResult hitag_s_write_page", "HitagSResult hitag_s_read_page"),
            ("HitagSResult hitag_s_read_page", None),
        ):
            block = source.split(block_name, 1)[1]
            if next_name:
                block = block.split(next_name, 1)[0]
            self.assertNotIn("HitagSRxMC4K", block)
            self.assertIn("hitag_s_data_rx_mode()", block)

    def test_open_session_closes_uid_select_in_same_mode(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        open_session = source.split("HitagSResult hitag_s_open_session", 1)[1].split(
            "HitagSResult hitag_s_8268_authenticate", 1
        )[0]

        self.assertIn("hitag_s_uid_request_mode", open_session)
        self.assertIn("hitag_s_select_current_mode", open_session)
        self.assertIn("session->selected = true", open_session)
        self.assertIn("field reset before next protocol mode", open_session)

    def test_open_session_requires_repeated_uid_before_select(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        mode_probe = source.split("static HitagSResult hitag_s_uid_request_mode", 1)[1].split(
            "HitagSResult hitag_s_open_session", 1
        )[0]

        self.assertIn("HITAG_S_UID_MODE_CONFIRMATION_MIN", source)
        self.assertIn("confirmed_uid", mode_probe)
        self.assertIn("confirmed_votes", mode_probe)
        self.assertIn("rejected unstable UID candidate", mode_probe)
        self.assertLess(mode_probe.index("confirmed_votes >="), mode_probe.index("RESULT: OK"))

    def test_debug_read_ex_reports_stage_and_page_status(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        debug_read_ex = source.split("HitagSResult hitag_s_debug_read_sequence_ex", 1)[1]

        self.assertIn("report->failure_stage", debug_read_ex)
        self.assertIn("HitagSPageStatusSkippedProtected", debug_read_ex)
        self.assertIn("HitagSPageStatusReadError", debug_read_ex)
        self.assertIn("hitag_s_open_session", debug_read_ex)
        self.assertIn("READ_PAGE 1", debug_read_ex)

    def test_debug_read_retries_session_open_for_30_second_budget(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        debug_read_ex = source.split("HitagSResult hitag_s_debug_read_sequence_ex", 1)[1]

        self.assertRegex(source, r"HITAG_S_DEBUG_READ_BUDGET_MS\s+30000")
        self.assertIn("hitag_s_debug_read_budget_expired", source)
        self.assertIn("furi_get_tick()", source)
        self.assertIn("session_attempt", debug_read_ex)
        self.assertIn("while(!hitag_s_debug_read_budget_expired", debug_read_ex)
        self.assertIn("DEBUG_READ: session attempt", debug_read_ex)
        self.assertIn("memmgr_get_free_heap", debug_read_ex)
        self.assertIn("Debug read still probing", debug_read_ex)

    def test_debug_read_does_not_offer_save_for_noise_only_trace(self):
        worker = (ROOT / "hitags_worker.c").read_text()
        debug_read = worker.split("static void hitags_worker_debug_read", 1)[1].split(
            "int32_t hitags_writer_worker_thread", 1
        )[0]

        self.assertIn("!report.session.selected", debug_read)
        self.assertIn("!report.htu_probe.detected", debug_read)
        self.assertIn("furi_string_free((FuriString*)app->debug_trace)", debug_read)
        self.assertIn("app->debug_trace = NULL", debug_read)
        self.assertIn("report.session.selected", debug_read.split("HitagSEventDebugPartial", 1)[0])

    def test_debug_read_keeps_trace_when_htu_probe_detects_8265(self):
        source = (ROOT / "hitag_s_8268.c").read_text()
        worker = (ROOT / "hitags_worker.c").read_text()
        debug_read_ex = source.split("HitagSResult hitag_s_debug_read_sequence_ex", 1)[1]
        debug_worker = worker.split("static void hitags_worker_debug_read", 1)[1].split(
            "int32_t hitags_writer_worker_thread", 1
        )[0]

        self.assertIn("hitag_htu_probe_uid(&report->htu_probe)", debug_read_ex)
        self.assertLess(
            debug_read_ex.index("hitag_htu_probe_uid(&report->htu_probe)"),
            debug_read_ex.index("while(!hitag_s_debug_read_budget_expired"),
        )
        self.assertIn('report->failure_stage = "HTU"', debug_read_ex)
        self.assertIn("ABORT: HTU/8265 detected", debug_read_ex)
        self.assertIn("report.htu_probe.detected", debug_worker)
        self.assertIn("HitagSEventDebugPartial", debug_worker)

    def test_read_flows_run_htu_probe_before_final_failure(self):
        worker = (ROOT / "hitags_worker.c").read_text()
        self.assertIn("hitags_worker_probe_htu_once", worker)

        read_uid = worker.split("static void hitags_worker_read_uid", 1)[1].split(
            "static void hitags_worker_read_pages", 1
        )[0]
        read_pages = worker.split("static void hitags_worker_read_pages", 1)[1].split(
            "static void hitags_worker_write_uid", 1
        )[0]

        self.assertIn("hitags_worker_probe_htu_once(\"Read UID\")", read_uid)
        self.assertIn("hitags_worker_probe_htu_once(\"Read Tag Data\")", read_pages)
        self.assertIn("htu_probe_done", read_uid)
        self.assertIn("htu_probe_done", read_pages)
        self.assertLess(
            read_uid.index("hitags_worker_probe_htu_once(\"Read UID\")"),
            read_uid.index("if(attempts >= max_attempts)"),
        )
        self.assertLess(
            read_pages.index("hitags_worker_probe_htu_once(\"Read Tag Data\")"),
            read_pages.index("if(attempts >= max_attempts)"),
        )

    def test_htu_probe_logs_negative_results_to_runtime_log(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        probe = source.split("HitagSResult hitag_htu_probe_uid", 1)[1].split(
            "HitagSResult hitag_htu_probe_uid_sequence", 1
        )[0]

        self.assertIn("HTU/8265 probe: READ UID", probe)
        self.assertIn("HTU/8265 probe: no response", probe)
        self.assertIn("HTU/8265 probe: rejected response", probe)

    def test_htu_probe_uses_multi_candidate_decoder(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        probe = source.split("HitagSResult hitag_htu_probe_uid", 1)[1].split(
            "HitagSResult hitag_htu_probe_uid_sequence", 1
        )[0]

        self.assertIn("hitag_htu_decode_candidates", source)
        self.assertIn('"half-mc4k"', source)
        self.assertIn('"half-mc8k"', source)
        self.assertIn('"half-mc2k"', source)
        self.assertIn('"pm3-mc4k"', source)
        self.assertIn("sof <= 8", source)
        self.assertIn("invert", source)
        self.assertIn("HTU candidate", source)
        self.assertIn("candidates_tried", probe)

    def test_htu_probe_uses_pm3_style_first_wait_field_reset(self):
        header = (ROOT / "hitag_s_proto.h").read_text()
        transport = (ROOT / "hitag_s_transport.c").read_text()
        session = (ROOT / "hitag_s_session.c").read_text()
        sequence = session.split("HitagSResult hitag_htu_probe_uid_sequence", 1)[1].split(
            "/* ============================================================\n"
            " * Hitag S Command Builders",
            1,
        )[0]

        self.assertIn("HITAG_S_T_WAIT_FIRST_US", header)
        self.assertIn("hitag_s_field_on_no_wait", header)
        self.assertIn("void hitag_s_field_on_no_wait", transport)
        self.assertIn("HITAG_HTU_WAKE_DELAYS_US", session)
        self.assertIn("HITAG_HTU_FIELD_RESET_MS", session)
        self.assertIn("HITAG_S_T_WAIT_FIRST_US - (100U * HITAG_S_T0_US)", session)
        self.assertIn("HITAG_S_T_WAIT_FIRST_US + (50U * HITAG_S_T0_US)", session)
        self.assertIn("HITAG_S_T_WAIT_FIRST_US", sequence)
        self.assertIn("hitag_s_field_on_no_wait()", sequence)
        self.assertIn("if(field_started)", sequence)
        self.assertIn("HTU wake prep", sequence)
        self.assertIn("HTU wake attempt", sequence)
        self.assertNotIn("hitag_s_field_on();", sequence)

    def test_rf_field_off_is_idempotent(self):
        transport = (ROOT / "hitag_s_transport.c").read_text()
        self.assertIn("static bool hitag_s_field_active", transport)
        self.assertIn("if(hitag_s_field_active)", transport)
        self.assertIn("hitag_s_field_active = true", transport)
        self.assertIn("hitag_s_field_active = false", transport)

    def test_htu_ttf_classification_is_sticky_across_candidates(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        candidate = source.split("static bool hitag_htu_try_raw_candidate", 1)[1].split(
            "typedef struct {",
            1,
        )[0]

        self.assertIn("bool ttf_like", candidate)
        self.assertIn("info->ttf_broadcast |= ttf_like", candidate)

    def test_tools_menu_has_dedicated_htu_probe_entry(self):
        config = (ROOT / "scenes/hitags_writer_scene_config.h").read_text()
        tools_menu = (ROOT / "scenes/hitags_writer_scene_tools_menu.c").read_text()
        scene_path = ROOT / "scenes/hitags_writer_scene_htu_probe.c"

        self.assertTrue(scene_path.exists())
        self.assertIn("ADD_SCENE(hitags_writer, htu_probe, HtuProbe)", config)
        self.assertIn('"HTU Probe"', tools_menu)
        self.assertIn("ToolsMenuIndexHtuProbe", tools_menu)
        self.assertIn("HitagSSceneHtuProbe", tools_menu)

    def test_worker_has_dedicated_htu_probe_operation(self):
        header = (ROOT / "hitags_writer_i.h").read_text()
        worker = (ROOT / "hitags_worker.c").read_text()

        self.assertIn("HitagSEventHtuProbeOk", header)
        self.assertIn("HitagSEventHtuProbeFailed", header)
        self.assertIn("HitagSWorkerHtuProbe", header)
        self.assertIn("HitagHtuProbeInfo htu_probe", header)
        self.assertIn("hitags_worker_htu_probe", worker)
        self.assertIn("HitagSWorkerHtuProbe", worker)
        self.assertIn("HitagSEventHtuProbeOk", worker)
        self.assertIn("HitagSEventHtuProbeFailed", worker)

    def test_htu_transport_sends_eof_before_rx_capture(self):
        source = (ROOT / "hitag_s_transport.c").read_text()
        send = source.split("void hitag_s_send_htu_frame_with_early_rx", 1)[1].split(
            "void hitag_s_field_on", 1
        )[0]

        self.assertIn("Hitag µ EOF", send)
        self.assertIn("furi_hal_rfid_tim_read_pause()", send)
        self.assertIn("if(start_rx) start_rx(context)", send)
        self.assertLess(
            send.rindex("furi_hal_rfid_tim_read_pause()"),
            send.index("if(start_rx) start_rx(context)"),
        )

    def test_rf_decode_hot_path_does_not_flood_runtime_log(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        ac_decode = source.split("static size_t hitag_s_decode_ac2k", 1)[1].split(
            "static void hitag_s_ac2k_put_bit", 1
        )[0]
        mc_decode = source.split("static size_t hitag_s_decode_mc4k", 1)[1].split(
            "static const char* hitag_s_rx_mode_name", 1
        )[0]
        send_receive = source.split("static size_t hitag_s_send_receive", 1)[1].split(
            "/* ============================================================\n"
            " * Hitag S Command Builders",
            1,
        )[0]

        self.assertNotIn("e[%d]", send_receive)
        self.assertNotIn("FURI_LOG_D(", send_receive)
        self.assertNotIn("FURI_LOG_I(", ac_decode)
        self.assertNotIn("FURI_LOG_I(", mc_decode)
        self.assertNotIn("p[%d]", ac_decode)

    def test_uid_probe_rejection_logs_are_summarized(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        uid_request = source.split("HitagSResult hitag_s_uid_request", 1)[1].split(
            "static HitagSResult hitag_s_select_frame", 1
        )[0]

        self.assertIn("low_entropy_rejects", uid_request)
        self.assertIn("noisy_rejects", uid_request)
        self.assertIn("partial_noisy_rejects", uid_request)
        self.assertIn("!hitag_s_trace_is_active()", uid_request)
        self.assertNotIn("UID try %d: rejected low-entropy", uid_request)
        self.assertNotIn("UID try %d: rejected noisy", uid_request)

    def test_field_reset_does_not_flood_runtime_log(self):
        transport = (ROOT / "hitag_s_transport.c").read_text()
        field_on = transport.split("void hitag_s_field_on", 1)[1].split(
            "void hitag_s_field_off", 1
        )[0]
        field_off = transport.split("void hitag_s_field_off", 1)[1]

        self.assertNotIn("FURI_LOG_D", field_on)
        self.assertNotIn("FURI_LOG_D", field_off)

    def test_rx_trace_records_window_timing_metadata(self):
        source = (ROOT / "hitag_s_session.c").read_text()
        send_receive = source.split("static size_t hitag_s_send_receive", 1)[1].split(
            "/* ============================================================\n"
            " * Hitag S Command Builders",
            1,
        )[0]

        self.assertIn("RX_META:", send_receive)
        self.assertIn("elapsed_us=", send_receive)
        self.assertIn("idle_us=", send_receive)
        self.assertIn("timeout_us=", send_receive)

    def test_receive_capture_starts_during_stop_tail_not_after_full_frame(self):
        header = (ROOT / "hitag_s_proto.h").read_text()
        transport = (ROOT / "hitag_s_transport.c").read_text()
        session = (ROOT / "hitag_s_session.c").read_text()
        send_receive = session.split("static size_t hitag_s_send_receive", 1)[1].split(
            "/* ============================================================\n"
            " * Hitag S Command Builders",
            1,
        )[0]

        self.assertIn("HitagSRxStartCallback", header)
        self.assertIn("hitag_s_send_frame_with_early_rx", header)
        self.assertIn("hitag_s_send_frame_with_early_rx(", send_receive)
        self.assertNotIn("hitag_s_send_frame(tx_data, tx_bits)", send_receive)

        early_rx = transport.split("void hitag_s_send_frame_with_early_rx", 1)[1].split(
            "void hitag_s_field_on", 1
        )[0]
        self.assertLess(early_rx.index("start_rx(context)"), early_rx.index("furi_delay_us(t_stop)"))

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

    def test_official_em4100_read_path_has_basic_debug_logging(self):
        read_em4100 = (ROOT / "scenes/hitags_writer_scene_read_em4100.c").read_text()
        success = (ROOT / "scenes/hitags_writer_scene_read_em4100_success.c").read_text()

        self.assertIn('#define TAG "HitagSReadEM"', read_em4100)
        self.assertIn("FURI_LOG_I(TAG, \"Read EM4100: start official LF RFID worker", read_em4100)
        self.assertIn("FURI_LOG_D(TAG, \"LF RFID callback", read_em4100)
        self.assertIn("FURI_LOG_I(TAG, \"Read EM4100: switching to ASK", read_em4100)
        self.assertIn("FURI_LOG_I(TAG, \"Read EM4100: switching to PSK", read_em4100)
        self.assertIn("FURI_LOG_I(TAG, \"Read EM4100: done", read_em4100)
        self.assertIn("FURI_LOG_I(TAG, \"Read EM4100: stop official LF RFID worker", read_em4100)

        self.assertRegex(success, r'#define\s+TAG\s+"HitagSReadEMOK"')
        self.assertIn('"Read EM4100 success:', success)
        self.assertIn('"Read EM4100 data:', success)
        self.assertIn("FURI_LOG_I(TAG, \"Read EM4100 result can be written to 8268", success)


if __name__ == "__main__":
    unittest.main()
