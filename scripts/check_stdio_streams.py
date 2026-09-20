#!/usr/bin/env python3
"""
Assert that the stdio examples keep stdout clean for the protocol.

On the stdio transport stdout *is* the JSON-RPC channel, so a human-readable
line printed there corrupts the message stream for the peer. Running an example
and checking its exit code does not notice this: the process still exits 0 while
emitting garbage the peer cannot parse.

This check drives each stdio example through a short scripted exchange and
requires every non-empty line it writes to stdout to parse as a JSON-RPC 2.0
message. Diagnostics are expected on stderr and are not inspected.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

CLIENT_INITIALIZE_RESPONSE = json.dumps({
    "jsonrpc": "2.0",
    "id": "1",
    "result": {
        "protocolVersion": "2025-11-25",
        "capabilities": {"tools": {}},
        "serverInfo": {"name": "stream-check-server", "version": "1.0.0"},
    },
})

SERVER_REQUESTS = [
    json.dumps({
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-06-18",
            "clientInfo": {"name": "stream-check", "version": "1.0.0"},
            "capabilities": {},
        },
    }),
    json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
    json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
]

# Each case is (binary name, lines fed to stdin). A binary that is not present
# in the build directory is skipped, so this runs against partial builds.
CASES = [
    ("example-client-stdio", [CLIENT_INITIALIZE_RESPONSE]),
    ("example-server-stdio", SERVER_REQUESTS),
]


def executable(build_dir, name):
    for candidate in (build_dir / name, build_dir / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    return None


def run_exchange(path, stdin_lines, settle_seconds=3.0):
    """Feed the example its scripted input, then hold stdin open.

    Closing stdin straight away makes the peer see EOF and exit before it has
    printed anything, which would let a corrupting example pass. Keeping the
    pipe open lets the exchange run far enough for diagnostics to appear.
    """
    process = subprocess.Popen(
        [str(path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        for line in stdin_lines:
            process.stdin.write(line + "\n")
        process.stdin.flush()
        time.sleep(settle_seconds)
    except (BrokenPipeError, OSError):
        pass

    try:
        process.stdin.close()
    except (BrokenPipeError, OSError):
        pass
    # communicate() would try to flush the pipe we just closed.
    process.stdin = None

    try:
        stdout, _ = process.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, _ = process.communicate()
    return stdout


def check_stream(name, path, stdin_lines):
    stdout = run_exchange(path, stdin_lines)

    offenders = []
    messages = 0
    for number, line in enumerate(stdout.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            message = json.loads(line)
        except ValueError:
            offenders.append((number, line))
            continue
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
            offenders.append((number, line))
            continue
        messages += 1

    if offenders:
        print(f"{name}: FAIL")
        print("  stdout is the JSON-RPC channel, but these lines are not JSON-RPC:")
        for number, line in offenders[:10]:
            print(f"    line {number}: {line[:160]}")
        print("  Send diagnostics to stderr instead.")
        return False

    if messages == 0:
        print(f"{name}: FAIL (no JSON-RPC written to stdout; the check proved nothing)")
        return False

    print(f"{name}: PASS ({messages} JSON-RPC messages, no stray output)")
    return True


def main():
    build_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/release")

    checked = 0
    failed = 0
    for name, stdin_lines in CASES:
        path = executable(build_dir, name)
        if path is None:
            print(f"{name}: SKIP (not built in {build_dir})")
            continue
        checked += 1
        if not check_stream(name, path, stdin_lines):
            failed += 1

    print(f"\n{'=' * 50}")
    if checked == 0:
        print(f"No stdio examples found in {build_dir}; build them with "
              f"python scripts/build.py --examples")
        return 1
    print(f"Results: {checked - failed} clean, {failed} corrupting stdout")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
