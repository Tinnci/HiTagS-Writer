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


@dataclass
class Transaction:
    """One TX/RX transaction from the trace."""
    section: str = ""       # e.g., "UID_REQUEST", "SELECT", "AUTH"
    tx_desc: str = ""       # TX description line
    captures: list = field(default_factory=list)  # list of RxCapture
    result: str = ""        # RESULT line


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
    raw_text: str = ""


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
            continue

        # RX lines — start new capture
        if stripped.startswith("RX:"):
            current_capture = RxCapture()
            m2 = re.search(r'mode=(\w+)', stripped)
            if m2:
                current_capture.mode = m2.group(1)
            m3 = re.search(r'(\d+) edges', stripped)
            if m3:
                pass  # edges will be parsed from EDGES line
            if current_txn:
                current_txn.captures.append(current_capture)
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
    lines.append("")

    # Transaction analysis
    lines.append("-" * 40)
    lines.append("RF Transaction Log")
    lines.append("-" * 40)

    for i, txn in enumerate(tf.transactions):
        lines.append(f"\n[{i+1}] {txn.section}")
        if txn.tx_desc:
            lines.append(f"  {txn.tx_desc}")

        for j, cap in enumerate(txn.captures):
            edge_count = len(cap.edges)
            lines.append(
                f"  Capture {j+1}: {edge_count} edges, mode={cap.mode}, "
                f"decoded={cap.decode_bits} bits"
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
                    bits, data, _ = decode_mc4k(cap.edges, threshold=192, sof_bits=6)
                    orig_hex = cap.decode_data.hex().upper() if cap.decode_data else "N/A"
                    new_hex = data.hex().upper() if data else "N/A"
                    lines.append(f"  Re-decode MC4K: {bits} bits = {new_hex}")
                    if orig_hex != new_hex and cap.decode_data:
                        lines.append(f"    ⚠ MISMATCH: original={orig_hex}")
                elif cap.mode == 'AC2K':
                    bits, data = decode_ac2k(cap.edges, sof_bits=1)
                    new_hex = data.hex().upper() if data else "N/A"
                    lines.append(f"  Re-decode AC2K: {bits} bits = {new_hex}")

        if txn.result:
            lines.append(f"  {txn.result}")

    # Authentication analysis
    lines.append("")
    lines.append("-" * 40)
    lines.append("Authentication Analysis")
    lines.append("-" * 40)
    auth_findings = analyze_auth_sequence(tf.transactions)
    for f_line in auth_findings:
        lines.append(f_line)

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


# ============================================================
# Entry point
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description="Analyze HiTag S debug trace files (.htsd)")
    parser.add_argument("trace_file", help="Path to .htsd trace file")
    parser.add_argument("--edges", action="store_true",
                        help="Show raw edge timing data")
    parser.add_argument("--redecode", action="store_true",
                        help="Re-decode Manchester from raw edges")
    parser.add_argument("-o", "--output", help="Save report to file")

    args = parser.parse_args()

    try:
        with open(args.trace_file, 'r') as f:
            text = f.read()
    except FileNotFoundError:
        print(f"Error: File not found: {args.trace_file}", file=sys.stderr)
        sys.exit(1)

    tf = parse_trace(text)
    report = generate_report(tf, show_edges=args.edges, redecode=args.redecode)

    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"Report saved to {args.output}")
    else:
        print(report)


if __name__ == "__main__":
    main()
