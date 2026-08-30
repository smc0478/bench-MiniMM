#!/usr/bin/env python3
"""Reproduce MM-UFFD-001 through loopback-only MiniMM CLI processes."""

from __future__ import annotations

import argparse
import json
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import List, Optional, Tuple


CASE_ID = "MM-UFFD-001"
LOOPBACK = "127.0.0.1"
READY_PATTERN = re.compile(
    r"^listening address=127\.0\.0\.1 port=([0-9]+)(?:\s.*)?$"
)
TOKEN_PATTERN = re.compile(
    rb"^token=([0-9a-fA-F]{32}) size=4096 rights=([a-z-]+)\r?\n$"
)
RESULT_PATTERN = re.compile(
    rb"^swap_entry=([0-9]+) expected_folio=([0-9]+) moved_folio=([0-9]+) "
    rb"pte_entry_matches=(true|false) folio_identity_valid=(true|false) "
    rb"accounting_valid=(true|false)\r?\n$"
)
UINT32_MAX = (1 << 32) - 1


class ReproductionError(RuntimeError):
    """A command or semantic oracle did not match the benchmark contract."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--startup-timeout", type=float, default=5.0)
    parser.add_argument("--command-timeout", type=float, default=5.0)
    args = parser.parse_args()
    if args.startup_timeout <= 0.0 or args.command_timeout <= 0.0:
        parser.error("timeouts must be positive")
    return args


def start_server(
    program: Path, timeout: float
) -> Tuple[subprocess.Popen, threading.Thread, List[str], int]:
    command = [
        str(program),
        "--bind",
        LOOPBACK,
        "--enable-uffd-move",
        "--port",
        "0",
        "--max-clients",
        "4",
        "--max-notes",
        "2",
        "--max-note-size",
        "4096",
        "--max-total-note-size",
        "4096",
        "--memory-pages",
        "4",
        "--timeout-ms",
        "2000",
    ]
    try:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
    except OSError as error:
        raise ReproductionError(f"could not start minimm-server: {error}") from error

    assert process.stdout is not None
    lines: queue.Queue = queue.Queue()
    transcript: List[str] = []

    def read_output() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            transcript.append(line)
            lines.put(line)
        lines.put(None)

    reader = threading.Thread(target=read_output, name="minimm-server-output", daemon=True)
    reader.start()
    deadline = time.monotonic() + timeout

    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise ReproductionError(
                    "minimm-server did not publish its loopback port"
                )
            try:
                line = lines.get(timeout=remaining)
            except queue.Empty as error:
                raise ReproductionError(
                    "minimm-server did not publish its loopback port"
                ) from error
            if line is None:
                detail = "".join(transcript).strip() or "no output"
                raise ReproductionError(
                    f"minimm-server exited before becoming ready: {detail}"
                )
            match = READY_PATTERN.fullmatch(line.strip())
            if match is None:
                continue
            port = int(match.group(1))
            if not 1 <= port <= 65535:
                raise ReproductionError(f"minimm-server reported invalid port {port}")
            return process, reader, transcript, port
    except BaseException:
        stop_server(process, reader)
        raise


def run_client(
    program: Path, port: int, timeout: float, operation: str, *arguments: str
) -> bytes:
    command = [
        str(program),
        "--host",
        LOOPBACK,
        "--port",
        str(port),
        "--timeout-ms",
        "2000",
        operation,
        *arguments,
    ]
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ReproductionError(f"client {operation} failed to run: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ReproductionError(
            f"client {operation} exited {completed.returncode}: {detail or 'no error output'}"
        )
    return completed.stdout


def parse_token(output: bytes, operation: str, expected_rights: bytes) -> str:
    match = TOKEN_PATTERN.fullmatch(output)
    if match is None:
        rendered = output.decode("utf-8", errors="replace").strip()
        raise ReproductionError(f"unexpected {operation} output: {rendered!r}")
    if match.group(2) != expected_rights:
        actual = match.group(2).decode("ascii", errors="replace")
        raise ReproductionError(
            f"{operation} returned rights {actual!r}, expected {expected_rights.decode()!r}"
        )
    return match.group(1).decode("ascii")


def parse_u32(value: bytes, field: str) -> int:
    parsed = int(value)
    if not 1 <= parsed <= UINT32_MAX:
        raise ReproductionError(f"uffd-move returned invalid {field}={parsed}")
    return parsed


def parse_result(output: bytes) -> Tuple[int, int, int, bool, bool, bool]:
    match = RESULT_PATTERN.fullmatch(output)
    if match is None:
        rendered = output.decode("utf-8", errors="replace").strip()
        raise ReproductionError(f"unexpected uffd-move output: {rendered!r}")
    return (
        parse_u32(match.group(1), "swap_entry"),
        parse_u32(match.group(2), "expected_folio"),
        parse_u32(match.group(3), "moved_folio"),
        match.group(4) == b"true",
        match.group(5) == b"true",
        match.group(6) == b"true",
    )


def stop_server(process: subprocess.Popen, reader: threading.Thread) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)
    reader.join(timeout=1.0)
    if process.stdout is not None:
        process.stdout.close()


def main() -> int:
    args = parse_args()
    process: Optional[subprocess.Popen] = None
    reader: Optional[threading.Thread] = None
    transcript: List[str] = []
    port: Optional[int] = None
    token: Optional[str] = None
    failure: Optional[ReproductionError] = None

    try:
        process, reader, transcript, port = start_server(args.server, args.startup_timeout)

        token = parse_token(
            run_client(
                args.client,
                port,
                args.command_timeout,
                "create",
                "4096",
                "rwsd",
            ),
            "create",
            b"rwsd",
        )

        (
            swap_entry,
            expected_folio,
            moved_folio,
            pte_entry_matches,
            folio_identity_valid,
            accounting_valid,
        ) = parse_result(
            run_client(
                args.client,
                port,
                args.command_timeout,
                "uffd-move",
                token,
                "7",
                "100",
                "200",
            )
        )

        if (
            swap_entry != 7
            or expected_folio != 100
            or moved_folio != 200
            or not pte_entry_matches
            or folio_identity_valid
            or accounting_valid
        ):
            raise ReproductionError(
                "uffd-move did not expose the swap-entry ABA: "
                f"swap_entry={swap_entry} expected_folio={expected_folio} "
                f"moved_folio={moved_folio} "
                f"pte_entry_matches={str(pte_entry_matches).lower()} "
                f"folio_identity_valid={str(folio_identity_valid).lower()} "
                f"accounting_valid={str(accounting_valid).lower()}"
            )

        print(
            json.dumps(
                {
                    "accounting_valid": accounting_valid,
                    "case_id": CASE_ID,
                    "expected_folio": expected_folio,
                    "folio_identity_valid": folio_identity_valid,
                    "moved_folio": moved_folio,
                    "note_size": 4096,
                    "pte_entry_matches": pte_entry_matches,
                    "replacement_folio": 200,
                    "source_folio": 100,
                    "swap_entry": swap_entry,
                },
                sort_keys=True,
            )
        )
    except ReproductionError as error:
        failure = error
    finally:
        if port is not None and token is not None:
            try:
                run_client(args.client, port, args.command_timeout, "delete", token)
            except ReproductionError:
                pass
        if process is not None and reader is not None:
            stop_server(process, reader)

    if failure is not None:
        detail = "".join(transcript).strip()
        print(f"{CASE_ID}: {failure}", file=sys.stderr)
        if detail:
            print(f"server output:\n{detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
