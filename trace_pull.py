#!/usr/bin/env python3
"""Pull HiTag S debug traces from a Flipper and run local analysis."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

import analyze_trace


TRACE_RE = re.compile(r"(?P<path>/ext/lfrfid/Trace(?:_NoUID)?_[0-9A-Fa-f]+\.htsd), size (?P<size>\d+)b")


def parse_trace_listing(text: str) -> list[tuple[str, int]]:
    """Parse `storage.py list /ext/lfrfid` output for HiTag S trace files."""
    traces: list[tuple[str, int]] = []
    for line in text.splitlines():
        match = TRACE_RE.search(line.strip())
        if match:
            traces.append((match.group("path"), int(match.group("size"))))
    return traces


def local_trace_name(remote_path: str, size: int) -> str:
    """Return a deterministic local filename for a remote trace."""
    name = Path(remote_path).name
    stem = Path(name).stem
    return f"flipper_{stem}_{size}b.htsd"


def run_storage(storage_script: Path, port: str | None, args: list[str]) -> str:
    cmd = [sys.executable, str(storage_script)]
    if port:
        cmd.extend(["-p", port])
    cmd.extend(args)
    result = subprocess.run(cmd, check=True, text=True, capture_output=True)
    return result.stdout


def pull_traces(storage_script: Path, port: str | None, out_dir: Path) -> list[Path]:
    listing = run_storage(storage_script, port, ["list", "/ext/lfrfid"])
    traces = parse_trace_listing(listing)
    out_dir.mkdir(parents=True, exist_ok=True)

    pulled: list[Path] = []
    for remote_path, size in traces:
        local_path = out_dir / local_trace_name(remote_path, size)
        run_storage(storage_script, port, ["receive", remote_path, str(local_path)])
        pulled.append(local_path)
    return pulled


def analyze_paths(paths: list[Path]) -> str:
    reports = []
    named_traces = []
    for path in paths:
        parsed = analyze_trace.parse_trace(path.read_text())
        named_traces.append((str(path), parsed))
        report = analyze_trace.generate_report(parsed, redecode=True)
        reports.append(f"TRACE FILE: {path}\n{report}")
    if len(named_traces) > 1:
        return analyze_trace.generate_batch_summary(named_traces) + "\n\n" + "\n\n".join(reports)
    return "\n\n".join(reports)


def main() -> int:
    parser = argparse.ArgumentParser(description="Pull and analyze HiTag S debug traces")
    parser.add_argument("--storage-script", default=str(Path.home() / ".ufbt/current/scripts/storage.py"))
    parser.add_argument("--port", default=None)
    parser.add_argument("--out-dir", default="pulled_traces")
    parser.add_argument("--report", default=None)
    args = parser.parse_args()

    pulled = pull_traces(Path(args.storage_script), args.port, Path(args.out_dir))
    if not pulled:
        print("No Trace_*.htsd files found under /ext/lfrfid")
        return 1

    report = analyze_paths(pulled)
    if args.report:
        Path(args.report).write_text(report)
        print(f"Pulled {len(pulled)} traces; report saved to {args.report}")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
