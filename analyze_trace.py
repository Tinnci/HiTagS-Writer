#!/usr/bin/env python3
"""
analyze_trace.py — HiTag S Debug Trace Analyzer

Parses .htsd trace files captured by the Flipper Zero HiTagS Writer app
and performs offline analysis of RF communication including:
  - Manchester decoding verification from raw edge timing
  - CRC-8 validation on decoded frames
  - Authentication sequence analysis
  - Timing anomaly detection
  - Page data summary

Usage:
    python analyze_trace.py <trace_file.htsd>
    python analyze_trace.py <trace_file.htsd> --edges    # Show raw edge data
    python analyze_trace.py <trace_file.htsd> --redecode # Re-decode Manchester
"""

import sys
import re
import argparse
from dataclasses import dataclass, field
from typing import Optional


# ============================================================
# CRC-8 Hitag S (polynomial 0x1D, init 0xFF)
# ============================================================

def hitag_s_crc8(data_bytes: bytes, bits: int) -> int:
    """Compute Hitag S CRC-8 over 'bits' MSB-first bits from data_bytes."""
    crc = 0xFF
    for i in range(bits):
        byte_idx = i // 8
        bit_idx = 7 - (i % 8)
        bit = (data_bytes[byte_idx] >> bit_idx) & 1
        if (crc >> 7) ^ bit:
            crc = ((crc << 1) ^ 0x1D) & 0xFF
        else:
            crc = (crc << 1) & 0xFF
    return crc


# ============================================================
# Edge data parsing & Manchester re-decoding
# ============================================================

@dataclass
class Edge:
    level: str  # 'H' or 'L'
    duration: int  # microseconds


@dataclass
class RxCapture:
    edges: list = field(default_factory=list)
    mode: str = ""
    decode_bits: int = 0
    decode_data: bytes = b""
    tx_desc: str = ""
    tx_frame: bytes = b""
    tx_frame_bits: int = 0
    tx_frame_us: int = 0
    rx_elapsed_us: int = 0
    rx_idle_us: int = 0
    rx_timeout_us: int = 0
    rx_final_edges: int = 0


@dataclass
class Transaction:
    """One TX/RX transaction from the trace."""
    section: str = ""       # e.g., "UID_REQUEST", "SELECT", "AUTH"
    tx_desc: str = ""       # TX description line
    tx_lines: list = field(default_factory=list)
    tx_frame: bytes = b""
    tx_frame_bits: int = 0
    tx_frame_us: int = 0
    captures: list = field(default_factory=list)  # list of RxCapture
    result: str = ""        # RESULT line
    abort: str = ""


@dataclass
class Mc4kSweepCandidate:
    bits: int = 0
    data: bytes = b""
    threshold: int = 0
    sof_bits: int = 0


def parse_edges(edge_line: str) -> list:
    """Parse 'EDGES: H:523 L:245 H:130 ...' into Edge list."""
    edges = []
    for m in re.finditer(r'([HL]):(\d+)', edge_line):
        edges.append(Edge(level=m.group(1), duration=int(m.group(2))))
    return edges


def decode_mc4k(edges: list, threshold: int = 192, sof_bits: int = 6) -> tuple:
    """
    Re-decode MC4K Manchester from raw edges.
    Returns (data_bits: int, data: bytes, half_periods: list).
    """
    if len(edges) < 4:
        return 0, b"", []

    glitch_min = 80 if threshold > 200 else 40

    # Build half-period stream from edge pairs
    hp_levels = []
    started = False
    last_high_dur = 0

    for e in edges:
        if e.level == 'H':
            if e.duration >= glitch_min:
                last_high_dur = e.duration
            continue

        # L event = period
        if last_high_dur == 0 or e.duration <= last_high_dur:
            last_high_dur = 0
            continue

        high_dur = last_high_dur
        low_dur = e.duration - high_dur
        last_high_dur = 0

        if not started:
            started = True
            if low_dur >= glitch_min:
                n = 1 if low_dur < threshold else 2
                hp_levels.extend([False] * n)
            continue

        if high_dur >= glitch_min:
            n = 1 if high_dur < threshold else 2
            hp_levels.extend([True] * n)
        if low_dur >= glitch_min:
            n = 1 if low_dur < threshold else 2
            hp_levels.extend([False] * n)

    if len(hp_levels) % 2 == 1:
        hp_levels.append(True)

    total_bits = len(hp_levels) // 2
    max_bits = 256
    data = bytearray((max_bits + 7) // 8)
    sof_remaining = sof_bits
    data_bits = 0

    for i in range(total_bits):
        if data_bits >= max_bits:
            break
        second_half = hp_levels[i * 2 + 1]
        if sof_remaining > 0:
            sof_remaining -= 1
        else:
            if second_half:
                data[data_bits // 8] |= (1 << (7 - (data_bits % 8)))
            data_bits += 1

    return data_bits, bytes(data[:(data_bits + 7) // 8]), hp_levels


def sweep_mc4k_decode(cap: RxCapture) -> Optional[Mc4kSweepCandidate]:
    """Find the best MC4K decode candidate across plausible thresholds and SOF lengths."""
    if cap.mode != "MC4K" or not cap.edges:
        return None

    best = Mc4kSweepCandidate()
    for sof_bits in range(0, 7):
        for threshold in range(144, 257, 16):
            bits, data, _ = decode_mc4k(cap.edges, threshold=threshold, sof_bits=sof_bits)
            if bits > best.bits:
                best = Mc4kSweepCandidate(bits=bits, data=data, threshold=threshold, sof_bits=sof_bits)

    return best if best.bits > 0 else None


def decode_ac2k(edges: list, sof_bits: int = 1) -> tuple:
    """
    Re-decode AC2K anti-collision response from raw edges.
    Returns (data_bits: int, data: bytes).
    """
    # AC2K thresholds (µs)
    THRESH_34 = 448  # between 3-half and 4-half
    THRESH_23 = 320  # between 2-half and 3-half
    GLITCH = 80

    lastbit = 0
    bSkip = False
    total_bits = 0
    sof_remaining = sof_bits
    data_bits = 0
    first_period = True
    max_bits = 64
    data = bytearray(8)

    for e in edges:
        if e.level == 'H':
            continue
        if data_bits >= max_bits:
            break

        rb = e.duration
        if first_period:
            first_period = False
            continue
        if rb < GLITCH:
            continue

        if rb >= THRESH_34:
            lastbit = 0
            total_bits += 1
            if sof_remaining > 0:
                sof_remaining -= 1
            else:
                data_bits += 1
        elif rb >= THRESH_23:
            lastbit = 1 - lastbit
            total_bits += 1
            if sof_remaining > 0:
                sof_remaining -= 1
            else:
                if lastbit:
                    data[data_bits // 8] |= (1 << (7 - (data_bits % 8)))
                data_bits += 1
            bSkip = (lastbit != 0)
        else:
            if not bSkip:
                lastbit = 1
                total_bits += 1
                if sof_remaining > 0:
                    sof_remaining -= 1
                else:
                    data[data_bits // 8] |= (1 << (7 - (data_bits % 8)))
                    data_bits += 1
            bSkip = not bSkip

    return data_bits, bytes(data[:(data_bits + 7) // 8])


def ac2k_quality(edges: list) -> dict:
    """Return simple quality metrics for a Flipper AC2K capture."""
    usable_periods = 0
    glitches = 0
    long_gaps = 0
    long_ac_periods = 0

    for e in edges:
        if e.level == "H":
            continue
        if e.duration < 80:
            glitches += 1
        elif e.duration > 1100:
            long_gaps += 1
            long_ac_periods += 1
        elif e.duration > 600:
            long_ac_periods += 1
        else:
            usable_periods += 1

    return {
        "usable_periods": usable_periods,
        "glitches": glitches,
        "long_gaps": long_gaps,
        "long_ac_periods": long_ac_periods,
        "too_noisy": glitches > 1 or long_ac_periods > 1,
    }


def is_valid_ac2k_uid_capture(bits: int, edges: list) -> bool:
    """Return whether an AC2K capture is safe to accept as a UID response."""
    return bits == 32 and not ac2k_quality(edges)["too_noisy"]


def is_marginal_ac2k_uid_capture(bits: int, edges: list) -> bool:
    """Return whether a complete noisy UID capture is usable only as a fallback."""
    quality = ac2k_quality(edges)
    return bits == 32 and quality["glitches"] <= 2 and quality["long_ac_periods"] <= 1


def accepted_uid_candidates(tf) -> set:
    """Return clean 32-bit AC2K UID candidates from a parsed trace."""
    accepted = set()
    for txn in tf.transactions:
        if txn.section != "UID_REQUEST":
            continue
        for cap in txn.captures:
            if cap.mode != "AC2K":
                continue
            bits, data = decode_ac2k(cap.edges, sof_bits=0)
            if is_valid_ac2k_uid_capture(bits, cap.edges):
                accepted.add(data[:4].hex().upper())
    return accepted


def marginal_uid_candidates(tf) -> set:
    """Return noisy but complete AC2K UID candidates that firmware may use as fallback."""
    candidates = set()
    for txn in tf.transactions:
        if txn.section != "UID_REQUEST":
            continue
        for cap in txn.captures:
            if cap.mode != "AC2K":
                continue
            bits, data = decode_ac2k(cap.edges, sof_bits=0)
            if (
                not is_valid_ac2k_uid_capture(bits, cap.edges)
                and is_marginal_ac2k_uid_capture(bits, cap.edges)
            ):
                candidates.add(data[:4].hex().upper())
    return candidates


def pack_bits(buf: bytearray, bit_pos: int, value: int, n_bits: int) -> int:
    """Pack MSB-first bits into buf and return the updated bit position."""
    for i in range(n_bits):
        pos = bit_pos + i
        bit_val = (value >> (n_bits - 1 - i)) & 1
        if bit_val:
            buf[pos // 8] |= 1 << (7 - (pos % 8))
        else:
            buf[pos // 8] &= ~(1 << (7 - (pos % 8)))
    return bit_pos + n_bits


def build_select_frame(uid: int) -> tuple:
    """Build SELECT command bytes as sent by the firmware model."""
    frame = bytearray(6)
    bit_pos = 0
    bit_pos = pack_bits(frame, bit_pos, 0, 5)
    bit_pos = pack_bits(frame, bit_pos, uid, 32)
    crc = hitag_s_crc8(bytes(frame), bit_pos)
    bit_pos = pack_bits(frame, bit_pos, crc, 8)
    return bytes(frame), bit_pos, crc


def build_uid_req_frame(value: int) -> tuple:
    """Build a 5-bit UID request command frame."""
    frame = bytearray(1)
    bit_pos = pack_bits(frame, 0, value, 5)
    return bytes(frame), bit_pos


def bplm_frame_duration_us(data: bytes, bits: int) -> int:
    """Return theoretical BPLM TX duration using firmware transport timing."""
    duration = 0
    for i in range(bits):
        bit = (data[i // 8] >> (7 - (i % 8))) & 1
        duration += 224 if bit else 160
    return duration + 352


def select_frame_check(tx_desc: str, tx_frame: bytes, tx_frame_bits: int, tx_frame_us: int) -> Optional[dict]:
    """Compare a SELECT TX_FRAME line against the model-built frame."""
    if not tx_frame:
        return None
    match = re.search(r"SELECT UID=([0-9A-Fa-f]{8}) CRC=([0-9A-Fa-f]{2})", tx_desc)
    if not match:
        return None

    uid = int(match.group(1), 16)
    logged_crc = int(match.group(2), 16)
    expected_frame, expected_bits, expected_crc = build_select_frame(uid)
    expected_us = bplm_frame_duration_us(expected_frame, expected_bits)
    return {
        "ok": tx_frame == expected_frame and tx_frame_bits == expected_bits and
        (tx_frame_us == 0 or tx_frame_us == expected_us) and
        logged_crc == expected_crc,
        "expected_frame": expected_frame,
        "expected_bits": expected_bits,
        "expected_crc": expected_crc,
        "expected_us": expected_us,
        "logged_frame": tx_frame,
        "logged_bits": tx_frame_bits,
        "logged_crc": logged_crc,
        "logged_us": tx_frame_us,
    }


def select_tx_frame_check(txn: Transaction) -> Optional[dict]:
    """Compare the latest transaction SELECT TX_FRAME against the model-built frame."""
    if txn.section != "SELECT":
        return None
    return select_frame_check(txn.tx_desc, txn.tx_frame, txn.tx_frame_bits, txn.tx_frame_us)


def select_capture_frame_check(cap: RxCapture) -> Optional[dict]:
    """Compare the TX frame associated with one SELECT capture against the model."""
    return select_frame_check(cap.tx_desc, cap.tx_frame, cap.tx_frame_bits, cap.tx_frame_us)


def uid_req_frame_check(cap: RxCapture) -> Optional[dict]:
    """Compare a UID_REQ TX_FRAME line against the 5-bit command model."""
    if not cap.tx_frame:
        return None
    match = re.search(r"UID_REQ_\w+\s+\(5 bits, val=0x([0-9A-Fa-f]+)\)", cap.tx_desc)
    if not match:
        return None

    value = int(match.group(1), 16)
    expected_frame, expected_bits = build_uid_req_frame(value)
    expected_us = bplm_frame_duration_us(expected_frame, expected_bits)
    return {
        "ok": cap.tx_frame == expected_frame and cap.tx_frame_bits == expected_bits and
        (cap.tx_frame_us == 0 or cap.tx_frame_us == expected_us),
        "expected_frame": expected_frame,
        "expected_bits": expected_bits,
        "expected_us": expected_us,
        "logged_frame": cap.tx_frame,
        "logged_bits": cap.tx_frame_bits,
        "logged_us": cap.tx_frame_us,
    }


# ============================================================
# Trace file parser
# ============================================================

@dataclass
class TraceFile:
    header: str = ""
    transactions: list = field(default_factory=list)
    page_table: dict = field(default_factory=dict)
    summary: str = ""
    uid: Optional[int] = None
    config: Optional[int] = None
    proto_mode: str = "STD"
    raw_text: str = ""
    field_carrier_hz: int = 0
    field_duty: str = ""
    field_pull: str = ""
    field_powerup_us: int = 0


def parse_trace(text: str) -> TraceFile:
    """Parse a .htsd trace file into structured data."""
    tf = TraceFile(raw_text=text)
    lines = text.split('\n')

    current_txn = None
    current_capture = None
    in_page_table = False

    for line in lines:
        stripped = line.strip()

        # Header
        if stripped.startswith("=== HiTag S Debug Trace"):
            tf.header = stripped
            continue

        # LF field metadata, emitted by newer Debug Read traces.
        if stripped.startswith("Field ON:"):
            field_match = re.search(
                r'carrier=(\d+)Hz\s+duty=([0-9.]+)\s+pull=(\w+)\s+powerup_us=(\d+)',
                stripped)
            if field_match:
                tf.field_carrier_hz = int(field_match.group(1))
                tf.field_duty = field_match.group(2)
                tf.field_pull = field_match.group(3)
                tf.field_powerup_us = int(field_match.group(4))
            continue

        # Section headers like "--- UID_REQUEST ---"
        m = re.match(r'^---\s+(.+?)\s+---$', stripped)
        if m:
            if current_txn:
                tf.transactions.append(current_txn)
            current_txn = Transaction(section=m.group(1))
            current_capture = None
            in_page_table = False
            continue

        # TX lines
        if stripped.startswith("TX:"):
            if current_txn:
                current_txn.tx_desc = stripped
                current_txn.tx_lines.append(stripped)
            continue

        if stripped.startswith("TX_FRAME:"):
            if current_txn:
                frame_match = re.search(
                    r'frame=([0-9A-Fa-f ]+)\s+bits=(\d+)(?:\s+tx_us=(\d+))?', stripped)
                if frame_match:
                    current_txn.tx_frame = bytes.fromhex(frame_match.group(1))
                    current_txn.tx_frame_bits = int(frame_match.group(2))
                    if frame_match.group(3):
                        current_txn.tx_frame_us = int(frame_match.group(3))
            continue

        # RX lines — start new capture
        if stripped.startswith("RX:"):
            if current_capture is None or current_capture.mode or current_capture.edges:
                current_capture = RxCapture()
                if current_txn:
                    current_txn.captures.append(current_capture)
            if current_txn:
                current_capture.tx_desc = current_txn.tx_desc
                current_capture.tx_frame = current_txn.tx_frame
                current_capture.tx_frame_bits = current_txn.tx_frame_bits
                current_capture.tx_frame_us = current_txn.tx_frame_us
            m2 = re.search(r'mode=(\w+)', stripped)
            if m2:
                current_capture.mode = m2.group(1)
            m3 = re.search(r'(\d+) edges', stripped)
            if m3:
                pass  # edges will be parsed from EDGES line
            continue

        if stripped.startswith("RX_META:"):
            meta_match = re.search(
                r'elapsed_us=(\d+)\s+idle_us=(\d+)\s+timeout_us=(\d+)\s+final_edges=(\d+)',
                stripped)
            if meta_match and current_txn:
                current_capture = RxCapture()
                current_capture.tx_desc = current_txn.tx_desc
                current_capture.tx_frame = current_txn.tx_frame
                current_capture.tx_frame_bits = current_txn.tx_frame_bits
                current_capture.tx_frame_us = current_txn.tx_frame_us
                current_capture.rx_elapsed_us = int(meta_match.group(1))
                current_capture.rx_idle_us = int(meta_match.group(2))
                current_capture.rx_timeout_us = int(meta_match.group(3))
                current_capture.rx_final_edges = int(meta_match.group(4))
                current_txn.captures.append(current_capture)
            continue

        # ABORT lines
        if stripped.startswith("ABORT:"):
            if current_txn:
                current_txn.abort = stripped
            continue

        # EDGES line
        if stripped.startswith("EDGES:"):
            if current_capture:
                current_capture.edges = parse_edges(stripped)
            continue

        # DECODE line
        if stripped.startswith("DECODE:"):
            if current_capture:
                m4 = re.match(r'DECODE:\s+(\d+)\s+bits(?:\s+=\s+(.+))?', stripped)
                if m4:
                    current_capture.decode_bits = int(m4.group(1))
                    if m4.group(2):
                        hex_str = m4.group(2).strip().replace(' ', '')
                        try:
                            current_capture.decode_data = bytes.fromhex(hex_str)
                        except ValueError:
                            pass
            continue

        # RESULT lines
        if stripped.startswith("RESULT:"):
            if current_txn:
                current_txn.result = stripped
            # Extract UID
            m5 = re.search(r'UID=([0-9A-Fa-f]{8})', stripped)
            if m5:
                tf.uid = int(m5.group(1), 16)
                mode_match = re.search(r'mode=(\w+)', stripped)
                if mode_match:
                    tf.proto_mode = mode_match.group(1)
            # Extract Config
            m6 = re.search(r'Config=([0-9A-Fa-f]{8})', stripped)
            if m6:
                tf.config = int(m6.group(1), 16)
            continue

        # Summary line
        if stripped.startswith("=== SUMMARY"):
            tf.summary = stripped
            if current_txn:
                tf.transactions.append(current_txn)
                current_txn = None
            continue

        # Page table
        if stripped == "PAGE TABLE:":
            in_page_table = True
            continue
        if in_page_table:
            m7 = re.match(r'\[\s*(\d+)\]\s+([0-9A-Fa-f]{8}|--------)', stripped)
            if m7:
                page_num = int(m7.group(1))
                page_val = m7.group(2)
                if page_val != "--------":
                    tf.page_table[page_num] = int(page_val, 16)
            continue

        # Config detail lines
        if stripped.startswith("Config:"):
            continue

        # Auth attempt lines
        if re.match(r'step\d:', stripped):
            if current_txn:
                current_txn.result += " | " + stripped

    if current_txn:
        tf.transactions.append(current_txn)

    return tf


# ============================================================
# Analysis functions
# ============================================================

def analyze_timing(capture: RxCapture) -> list:
    """Check for timing anomalies in edge data."""
    issues = []
    if not capture.edges:
        return issues

    durations = [e.duration for e in capture.edges if e.level == 'L']

    if capture.mode in ('MC4K', 'MC2K'):
        # Expected half-periods: ~128µs (MC4K) or ~256µs (MC2K)
        threshold = 192 if capture.mode == 'MC4K' else 384
        nominal_short = 128 if capture.mode == 'MC4K' else 256
        nominal_long = 256 if capture.mode == 'MC4K' else 512

        for i, d in enumerate(durations):
            if d > 0:
                # Check for very short pulses (glitches)
                if d < 40:
                    issues.append(f"  Glitch at period[{i}]: {d}µs (too short)")
                # Check for very long pulses (missed edges)
                elif d > nominal_long * 2:
                    issues.append(f"  Gap at period[{i}]: {d}µs (>{nominal_long*2}µs)")

    elif capture.mode == 'AC2K':
        for i, d in enumerate(durations):
            if d > 0 and d < 80:
                issues.append(f"  Glitch at period[{i}]: {d}µs")
            if d > 1200:
                issues.append(f"  Gap at period[{i}]: {d}µs (very long)")

    return issues


def analyze_auth_sequence(transactions: list) -> list:
    """Analyze authentication attempts and their results."""
    findings = []
    auth_txns = [t for t in transactions if 'AUTH' in t.section.upper()]

    if not auth_txns:
        findings.append("No authentication attempts found in trace.")
        return findings

    for t in auth_txns:
        pwd_match = re.search(r'pwd=0x([0-9A-Fa-f]+)', t.section)
        pwd_str = pwd_match.group(1) if pwd_match else "unknown"
        findings.append(f"Auth attempt: password=0x{pwd_str}")

        if "AUTH OK" in t.result:
            findings.append(f"  -> SUCCESS with 0x{pwd_str}")
        elif "NACK" in t.result:
            findings.append(f"  -> REJECTED (wrong password)")
        elif "TIMEOUT" in t.result or "no ACK" in t.result:
            findings.append(f"  -> TIMEOUT (tag not responding)")
        else:
            findings.append(f"  -> {t.result}")

    return findings


def diagnose_select_transactions(transactions: list) -> list:
    """Return high-level SELECT failure diagnoses."""
    findings = []
    for txn in transactions:
        if txn.section != "SELECT":
            continue

        frame_check = select_tx_frame_check(txn)
        timeout = "TIMEOUT" in txn.result
        max_decode_bits = max((cap.decode_bits for cap in txn.captures), default=0)
        max_sweep_bits = max(
            ((sweep.bits if sweep else 0) for sweep in (sweep_mc4k_decode(cap) for cap in txn.captures)),
            default=0,
        )

        if frame_check and not frame_check["ok"]:
            findings.append(
                "SELECT TX frame does not match the model; fix frame packing/CRC before RF tuning.")
        elif frame_check and frame_check["ok"] and timeout and max_decode_bits == 0:
            findings.append(
                "SELECT frame matches model but no MC4K response was decoded; focus on "
                "RF field/coil coupling or response-window timing.")
        elif not frame_check and timeout and max_decode_bits == 0:
            findings.append(
                "SELECT timed out with no decoded MC4K response; trace lacks TX_FRAME, "
                "so capture a new trace before separating frame bugs from RF/timing issues.")
        elif timeout and max_decode_bits > 0 and max_sweep_bits >= 32:
            findings.append(
                f"SELECT produced only {max_decode_bits} decoded MC4K bits, but threshold/SOF "
                f"sweep can recover {max_sweep_bits}; tune MC4K decode parameters.")
        elif timeout and max_decode_bits > 0:
            findings.append(
                f"SELECT produced only {max_decode_bits} decoded MC4K bits. "
                f"MC4K sweep still only recovers {max_sweep_bits} bits, so prioritize "
                "response window/RF coupling over Manchester threshold tuning.")

    return findings


def verify_crc(data: bytes, bits: int, expected_crc: int) -> bool:
    """Verify CRC-8 on decoded data."""
    if len(data) * 8 < bits:
        return False
    calc = hitag_s_crc8(data, bits)
    return calc == expected_crc


def format_config(config_val: int) -> str:
    """Format config page value into human-readable fields."""
    b = config_val.to_bytes(4, 'big')
    con0, con1, con2, pwdh0 = b[0], b[1], b[2], b[3]

    memt = con0 & 0x03
    memt_names = {0: "32pg", 1: "8pg", 2: "reserved", 3: "64pg"}

    auth = (con1 >> 7) & 1
    ttfc = (con1 >> 6) & 1
    ttfdr = (con1 >> 4) & 3
    ttfm = (con1 >> 2) & 3
    lcon = (con1 >> 1) & 1
    lkp = con1 & 1

    lines = [
        f"  MEMT={memt} ({memt_names.get(memt, '?')})",
        f"  auth={auth} LKP={lkp} LCON={lcon}",
        f"  TTFC={ttfc} ({'Manchester' if ttfc == 0 else 'Biphase'})",
        f"  TTFDR={ttfdr} TTFM={ttfm}",
        f"  CON2=0x{con2:02X} (lock bits)",
        f"  PWDH0=0x{pwdh0:02X}",
    ]
    return '\n'.join(lines)


# ============================================================
# Main report generator
# ============================================================

def generate_report(tf: TraceFile, show_edges: bool = False, redecode: bool = False) -> str:
    """Generate analysis report from parsed trace."""
    lines = []
    lines.append("=" * 60)
    lines.append("  HiTag S Debug Trace Analysis Report")
    lines.append("=" * 60)
    lines.append("")

    if tf.uid is not None:
        lines.append(f"Tag UID: 0x{tf.uid:08X}")
    if tf.config is not None:
        lines.append(f"Config:  0x{tf.config:08X}")
        lines.append(format_config(tf.config))
    if tf.field_carrier_hz:
        lines.append(
            f"LF field: carrier={tf.field_carrier_hz}Hz duty={tf.field_duty} "
            f"pull={tf.field_pull} powerup_us={tf.field_powerup_us}")
    lines.append("")

    # Transaction analysis
    lines.append("-" * 40)
    lines.append("RF Transaction Log")
    lines.append("-" * 40)

    for i, txn in enumerate(tf.transactions):
        lines.append(f"\n[{i+1}] {txn.section}")
        if txn.tx_desc:
            for tx_line in txn.tx_lines or [txn.tx_desc]:
                lines.append(f"  {tx_line}")
        tx_frame_check = select_tx_frame_check(txn)
        if tx_frame_check:
            expected = " ".join(f"{b:02X}" for b in tx_frame_check["expected_frame"])
            logged = " ".join(f"{b:02X}" for b in tx_frame_check["logged_frame"])
            if tx_frame_check["ok"]:
                tx_us = tx_frame_check["logged_us"] or tx_frame_check["expected_us"]
                lines.append(
                    f"  TX frame check: OK ({logged}, {tx_frame_check['logged_bits']} bits, "
                    f"tx_us={tx_us})")
            else:
                lines.append(
                    "  TX frame check: MISMATCH "
                    f"logged={logged}/{tx_frame_check['logged_bits']}b "
                    f"tx_us={tx_frame_check['logged_us'] or 'n/a'} "
                    f"expected={expected}/{tx_frame_check['expected_bits']}b "
                    f"expected_tx_us={tx_frame_check['expected_us']}"
                )

        for j, cap in enumerate(txn.captures):
            edge_count = len(cap.edges)
            tx_context = f" after {cap.tx_desc}" if cap.tx_desc else ""
            capture_line = (
                f"  Capture {j+1}{tx_context}: {edge_count} edges, mode={cap.mode}, "
                f"decoded={cap.decode_bits} bits"
            )
            if cap.tx_frame:
                tx_frame = " ".join(f"{b:02X}" for b in cap.tx_frame)
                capture_line += (
                    f", tx_frame={tx_frame}/{cap.tx_frame_bits}b tx_us="
                    f"{cap.tx_frame_us or 'n/a'}"
                )
            if cap.rx_timeout_us:
                capture_line += (
                    f", rx_meta=elapsed:{cap.rx_elapsed_us}us idle:{cap.rx_idle_us}us "
                    f"timeout:{cap.rx_timeout_us}us final_edges:{cap.rx_final_edges}"
                )
            lines.append(capture_line)

            if txn.section == "SELECT":
                cap_frame_check = select_capture_frame_check(cap)
                if cap_frame_check:
                    expected = " ".join(f"{b:02X}" for b in cap_frame_check["expected_frame"])
                    logged = " ".join(f"{b:02X}" for b in cap_frame_check["logged_frame"])
                    if cap_frame_check["ok"]:
                        tx_us = cap_frame_check["logged_us"] or cap_frame_check["expected_us"]
                        lines.append(
                            "    SELECT TX frame check: OK "
                            f"({logged}, {cap_frame_check['logged_bits']} bits, tx_us={tx_us})")
                    else:
                        lines.append(
                            "    SELECT TX frame check: MISMATCH "
                            f"logged={logged}/{cap_frame_check['logged_bits']}b "
                            f"tx_us={cap_frame_check['logged_us'] or 'n/a'} "
                            f"expected={expected}/{cap_frame_check['expected_bits']}b "
                            f"expected_tx_us={cap_frame_check['expected_us']}"
                        )
            elif txn.section == "UID_REQUEST":
                cap_frame_check = uid_req_frame_check(cap)
                if cap_frame_check:
                    expected = " ".join(f"{b:02X}" for b in cap_frame_check["expected_frame"])
                    logged = " ".join(f"{b:02X}" for b in cap_frame_check["logged_frame"])
                    if cap_frame_check["ok"]:
                        tx_us = cap_frame_check["logged_us"] or cap_frame_check["expected_us"]
                        lines.append(
                            "    UID TX frame check: OK "
                            f"({logged}, {cap_frame_check['logged_bits']} bits, tx_us={tx_us})")
                    else:
                        lines.append(
                            "    UID TX frame check: MISMATCH "
                            f"logged={logged}/{cap_frame_check['logged_bits']}b "
                            f"tx_us={cap_frame_check['logged_us'] or 'n/a'} "
                            f"expected={expected}/{cap_frame_check['expected_bits']}b "
                            f"expected_tx_us={cap_frame_check['expected_us']}"
                        )

            if show_edges and cap.edges:
                edge_strs = [f"{e.level}:{e.duration}" for e in cap.edges[:30]]
                lines.append(f"    Edges: {' '.join(edge_strs)}")
                if edge_count > 30:
                    lines.append(f"    ... ({edge_count - 30} more)")

            # Timing analysis
            timing_issues = analyze_timing(cap)
            if timing_issues:
                lines.append("  ⚠ Timing anomalies:")
                lines.extend(timing_issues)

            # Re-decode if requested
            if redecode and cap.edges:
                if cap.mode == 'MC4K':
                    sof_bits = 6 if tf.proto_mode.upper().startswith("ADV") else 1
                    bits, data, _ = decode_mc4k(cap.edges, threshold=192, sof_bits=sof_bits)
                    orig_hex = cap.decode_data.hex().upper() if cap.decode_data else "N/A"
                    new_hex = data.hex().upper() if data else "N/A"
                    lines.append(f"  Re-decode MC4K: {bits} bits = {new_hex}")
                    if orig_hex != new_hex and cap.decode_data:
                        lines.append(f"    ⚠ MISMATCH: original={orig_hex}")
                    sweep = sweep_mc4k_decode(cap)
                    if sweep and sweep.bits > bits:
                        sweep_hex = sweep.data.hex().upper() if sweep.data else "N/A"
                        lines.append(
                            "    MC4K sweep: "
                            f"best={sweep.bits} bits threshold={sweep.threshold} "
                            f"sof={sweep.sof_bits} data={sweep_hex}"
                        )
                elif cap.mode == 'AC2K':
                    bits, data = decode_ac2k(cap.edges, sof_bits=0)
                    new_hex = data.hex().upper() if data else "N/A"
                    lines.append(f"  Re-decode AC2K: {bits} bits = {new_hex}")
                    quality = ac2k_quality(cap.edges)
                    if quality["too_noisy"]:
                        severity = (
                            "marginal fallback candidate"
                            if is_marginal_ac2k_uid_capture(bits, cap.edges)
                            else "noisy candidate"
                        )
                        lines.append(
                            f"    ⚠ AC2K {severity}: "
                            f"{quality['glitches']} glitches, "
                            f"{quality['long_ac_periods']} long AC gaps"
                        )

        if txn.result:
            lines.append(f"  {txn.result}")
        if txn.abort:
            lines.append(f"  {txn.abort}")

    # Authentication analysis
    lines.append("")
    lines.append("-" * 40)
    lines.append("Authentication Analysis")
    lines.append("-" * 40)
    auth_findings = analyze_auth_sequence(tf.transactions)
    for f_line in auth_findings:
        lines.append(f_line)

    select_findings = diagnose_select_transactions(tf.transactions)
    if select_findings:
        lines.append("")
        lines.append("-" * 40)
        lines.append("SELECT Diagnosis")
        lines.append("-" * 40)
        lines.extend(select_findings)

    # Page table
    if tf.page_table:
        lines.append("")
        lines.append("-" * 40)
        lines.append("Page Table")
        lines.append("-" * 40)
        max_page = max(tf.page_table.keys())
        valid_count = len(tf.page_table)
        lines.append(f"Pages read: {valid_count}/{max_page + 1}")
        lines.append("")
        for p in range(max_page + 1):
            if p in tf.page_table:
                val = tf.page_table[p]
                # Show ASCII printable bytes
                b = val.to_bytes(4, 'big')
                ascii_repr = ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)
                lines.append(f"  [{p:2d}] {val:08X}  |{ascii_repr}|")
            else:
                lines.append(f"  [{p:2d}] --------")

        # CRC verification on config page
        if 1 in tf.page_table:
            lines.append("")
            lines.append("Config page analysis:")
            lines.append(format_config(tf.page_table[1]))

        # EM4100 data check on pages 4-5
        if 4 in tf.page_table and 5 in tf.page_table:
            data_hi = tf.page_table[4]
            data_lo = tf.page_table[5]
            lines.append("")
            lines.append(f"EM4100 data pages: {data_hi:08X} {data_lo:08X}")
            # Try to extract EM4100 ID from Manchester-encoded data
            em_bits = (data_hi << 32) | data_lo
            lines.append(f"  Combined 64 bits: {em_bits:016X}")

    # Summary
    if tf.summary:
        lines.append("")
        lines.append(tf.summary)

    lines.append("")
    lines.append("=" * 60)
    lines.append("  End of analysis")
    lines.append("=" * 60)

    return '\n'.join(lines)


def _frame_status(checks: list[Optional[dict]], saw_tx: bool) -> str:
    present = [check for check in checks if check is not None]
    if any(check and not check["ok"] for check in present):
        return "mismatch"
    if present:
        return "ok"
    return "legacy" if saw_tx else "-"


def _batch_hint(
    tf: TraceFile,
    uid_tx: str,
    select_tx: str,
    select_bits: int,
    select_sweep_bits: int,
    accepted: set[str],
    marginal: set[str],
) -> str:
    """Return the next investigation target for one trace row."""
    if tf.field_pull == "" or uid_tx == "legacy" or select_tx == "legacy":
        return "legacy-insufficient"
    if tf.field_carrier_hz and tf.field_carrier_hz != 125000:
        return "fix-field-carrier"
    if tf.field_pull and tf.field_pull != "release":
        return "fix-field-pull"
    if uid_tx == "mismatch":
        return "fix-uid-frame"
    if select_tx == "mismatch":
        return "fix-select-frame"
    if tf.uid is None and accepted and select_tx == "-":
        return "uid-offline-recovered"
    if tf.uid is None and not accepted and marginal:
        return "uid-marginal-fallback"
    if tf.uid is None and not accepted:
        return "uid-rf-or-window"
    if select_tx == "ok" and select_bits == 0:
        return "select-rf-or-window"
    if 0 < select_bits < 32 and select_sweep_bits >= 32:
        return "select-decode-sweep"
    if 0 < select_bits < 32:
        return "select-decode-threshold"
    if select_bits >= 32:
        return "inspect-select-payload"
    return "needs-more-evidence"


def generate_batch_summary(named_traces: list[tuple[str, TraceFile]]) -> str:
    """Generate a compact matrix for multiple trace files."""
    lines = [
        "============================================================",
        "  Batch Summary",
        "============================================================",
    ]

    for name, tf in named_traces:
        uid = f"{tf.uid:08X}" if tf.uid is not None else "-"
        accepted_set = accepted_uid_candidates(tf)
        accepted = ",".join(sorted(accepted_set)) or "-"
        marginal_set = marginal_uid_candidates(tf)
        marginal = ",".join(sorted(marginal_set)) or "-"
        field = tf.field_pull or "legacy"

        uid_caps = [
            cap for txn in tf.transactions if txn.section == "UID_REQUEST" for cap in txn.captures
        ]
        select_caps = [
            cap for txn in tf.transactions if txn.section == "SELECT" for cap in txn.captures
        ]

        uid_tx = _frame_status([uid_req_frame_check(cap) for cap in uid_caps], bool(uid_caps))
        select_tx = _frame_status(
            [select_capture_frame_check(cap) for cap in select_caps], bool(select_caps))
        select_bits = max((cap.decode_bits for cap in select_caps), default=0)
        select_sweep_bits = max(
            ((sweep.bits if sweep else 0) for sweep in (sweep_mc4k_decode(cap) for cap in select_caps)),
            default=0,
        )
        hint = _batch_hint(
            tf, uid_tx, select_tx, select_bits, select_sweep_bits, accepted_set, marginal_set)

        lines.append(
            f"{name} | uid={uid} | accepted={accepted} | marginal={marginal} | field={field} | "
            f"uid_tx={uid_tx} | select_tx={select_tx} | select_bits={select_bits} | "
            f"select_sweep={select_sweep_bits} | {hint}"
        )

    return "\n".join(lines)


# ============================================================
# Entry point
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description="Analyze HiTag S debug trace files (.htsd)")
    parser.add_argument("trace_file", nargs="+", help="Path to one or more .htsd trace files")
    parser.add_argument("--edges", action="store_true",
                        help="Show raw edge timing data")
    parser.add_argument("--redecode", action="store_true",
                        help="Re-decode Manchester from raw edges")
    parser.add_argument("-o", "--output", help="Save report to file")

    args = parser.parse_args()

    try:
        reports = []
        named_traces = []
        for trace_file in args.trace_file:
            with open(trace_file, 'r') as f:
                text = f.read()
            tf = parse_trace(text)
            named_traces.append((trace_file, tf))
            report = generate_report(tf, show_edges=args.edges, redecode=args.redecode)
            if len(args.trace_file) > 1:
                report = f"TRACE FILE: {trace_file}\n" + report
            reports.append(report)
    except FileNotFoundError as exc:
        print(f"Error: File not found: {exc.filename}", file=sys.stderr)
        sys.exit(1)

    if len(named_traces) > 1:
        output = generate_batch_summary(named_traces) + "\n\n" + "\n\n".join(reports)
    else:
        output = "\n\n".join(reports)

    if args.output:
        with open(args.output, 'w') as f:
            f.write(output)
        print(f"Report saved to {args.output}")
    else:
        print(output)


if __name__ == "__main__":
    main()
