#!/usr/bin/env python3
"""
Cross-platform example runner for mcp-cpp-sdk.

Runs all non-interactive examples that are safe for CI automation.

Examples that ARE run:
  - example-feature-*: Feature demonstration examples (all use loopback pattern)
  - example-echo-websocket: WebSocket transport loopback
  - example-http-loopback: HTTP transport loopback
  - example-benchmark-*: Throughput measurement examples
  - example-client-stdio: Launches example-server-stdio as a child process

  - example-sync-minimal-client: Launches example-sync-minimal-server
  - example-sync-notes-client and example-async-notes-client: Each launches
    example-sync-notes-server and example-async-notes-server
  - example-sync-server and example-sync-client, example-async-server and
    example-async-client: HTTP pairs; each server is started on a free port and
    then called by its client

Examples NOT run:
  - example-interactive-client: Needs the path of a server to launch
  - example-server-stdio: Server blocks waiting for stdio connection
  - example-server-simple: Server blocks waiting for stdio connection
  - example-debugger-server: Server, blocks waiting for a stdio or HTTP client
  - example-llama-mcp: Server, needs a running llama.cpp llama-server
  - example-server-sampling: Requires LLM sampling connection
"""
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

# (HTTP server, its client, lines the client prints, whether the client succeeds); for a client
# that fails, the lines are text its error output contains.
SYNC_HTTP_PAIRS = [
    ("server", "client",
     ["Hello, Ada!", 'log: "Composing a greeting"', "progress: 1", "Be kind to each other.",
      "Write a short welcome message for Ada.", "completion: Ada", "Hello, Dr. Ada!",
      "Greetings, friend!", "The client has 1 root(s)", "Hello, Grace!",
      "updated: notifications/resources/updated", "request error -32602", "tool: greet",
      "tool: greeting_card", "from a coroutine: Hello, Linus!"], True),
]

# The async-mode HTTP pair, as SYNC_HTTP_PAIRS.
ASYNC_HTTP_PAIRS = [
    ("server", "client",
     ["Hello, Ada!", 'log: "Composing a greeting"', "progress: 1", "Be kind to each other.",
      "Write a short welcome message for Ada.", "completion: Ada", "Hello, Dr. Ada!",
      "Greetings, friend!", "The client has 1 root(s)", "Hello, Grace!",
      "updated: notifications/resources/updated", "request error -32602", "tool: greet",
      "tool: greeting_card", "together: Hello, Ada!", "together: Hello, Grace!"], True),
]

# Lines each notes client prints, whichever notes server it launches.
NOTES_OUTPUT = ["changed: notes://all", "- Buy milk", "Summarize these 1 notes."]

def is_example_executable(path):
    if not path.is_file():
        return False
    if sys.platform == "win32":
        return path.suffix.lower() == ".exe"
    return path.stat().st_mode & 0o111 != 0

def run_examples():
    build_dir = Path("build/release")

    print("Building examples...")
    result = subprocess.run(
        [sys.executable, "scripts/build.py", "--examples"],
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        print(f"Build failed:\n{result.stderr}")
        return 1

    # Find all example binaries (non-interactive, multiplatform examples)
    examples = sorted(
        list(build_dir.glob("example-feature-*")) +
        list(build_dir.glob("example-echo-websocket")) +
        list(build_dir.glob("example-http-loopback")) +
        list(build_dir.glob("example-client-stdio*"))
    )
    examples = [exe for exe in examples if is_example_executable(exe)]
    if not examples:
        print("No example binaries found!")
        return 1

    print(f"\nFound {len(examples)} examples to run\n")

    passed = 0
    failed = 0

    for exe in examples:
        exe_name = exe.name

        print(f"Running {exe_name}...", end=" ", flush=True)

        try:
            result = subprocess.run(
                [str(exe)],
                capture_output=True,
                timeout=15,
                text=True
            )

            if result.returncode == 0:
                print("PASS")
                passed += 1
            else:
                print(f"FAIL (exit code {result.returncode})")
                print_process_output(result)
                failed += 1

        except subprocess.TimeoutExpired as e:
            print("TIMEOUT")
            print_timeout_output(e)
            failed += 1
        except Exception as e:
            print(f"ERROR: {e}")
            failed += 1

    print("\nRunning the sync-mode and async-mode programs\n")
    sync_passed, sync_failed = run_sync_examples(build_dir)
    passed += sync_passed
    failed += sync_failed

    print(f"\n{'='*50}")
    print(f"Results: {passed} passed, {failed} failed")

    if failed > 0:
        print("\nSome examples failed!")
        return 1
    else:
        print("\nAll examples passed!")
        return 0

def example_executable(build_dir, name, mode="sync"):
    for candidate in (build_dir / f"example-{mode}-{name}", build_dir / f"example-{mode}-{name}.exe"):
        if is_example_executable(candidate):
            return candidate
    return None


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_listener(process, port, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def stop_server(process):
    if process.poll() is None:
        if sys.platform == "win32":
            process.terminate()
        else:
            process.send_signal(signal.SIGINT)
    try:
        return process.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()


def check_client(label, command, expected_lines, should_succeed):
    """Run one client; return True when it ends and prints as expected."""
    print(f"Running {label}...", end=" ", flush=True)
    try:
        result = subprocess.run(command, capture_output=True, timeout=30, text=True)
    except subprocess.TimeoutExpired as error:
        print("TIMEOUT")
        print_timeout_output(error)
        return False
    if not should_succeed:
        missing = [text for text in expected_lines if text not in result.stderr]
        if result.returncode != 0 and not missing:
            print("PASS (refused)")
            return True
        print(f"FAIL (exit code {result.returncode}, missing error output {missing})")
        print_process_output(result)
        return False
    lines = result.stdout.splitlines()
    missing = [line for line in expected_lines if line not in lines]
    if result.returncode == 0 and not missing:
        print("PASS")
        return True
    print(f"FAIL (exit code {result.returncode}, missing output {missing})")
    print_process_output(result)
    return False


def run_sync_examples(build_dir):
    """Run the sync-mode and async-mode programs in pairs; return (passed, failed)."""
    passed = 0
    failed = 0

    def record(ok):
        nonlocal passed, failed
        if ok:
            passed += 1
        else:
            failed += 1

    def check_stdio_pair(client_mode, client_name, server_mode, server_name, expected):
        client = example_executable(build_dir, client_name, client_mode)
        server = example_executable(build_dir, server_name, server_mode)
        if client is None or server is None:
            print(f"example-{client_mode}-{client_name} or "
                  f"example-{server_mode}-{server_name} not found")
            record(False)
            return
        record(check_client(f"example-{client_mode}-{client_name} with {server_mode} {server_name}",
                            [str(client), str(server)], expected, True))

    check_stdio_pair("sync", "minimal-client", "sync", "minimal-server", ["Hello, Ada!"])
    # Each notes client launches each notes server.
    for client_mode in ("sync", "async"):
        for server_mode in ("sync", "async"):
            check_stdio_pair(client_mode, "notes-client", server_mode, "notes-server", NOTES_OUTPUT)

    http_pairs = [("sync", pair) for pair in SYNC_HTTP_PAIRS]
    http_pairs += [("async", pair) for pair in ASYNC_HTTP_PAIRS]
    for mode, (server, client, expected, should_succeed) in http_pairs:
        if not should_succeed and sys.platform == "win32":
            # The refused client ends with an uncaught exception, which Windows may report in a
            # dialog instead of an exit status.
            continue
        server_path = example_executable(build_dir, server, mode)
        client_path = example_executable(build_dir, client, mode)
        if client_path is None or server_path is None:
            print(f"example-{mode}-{client} or example-{mode}-{server} not found")
            record(False)
            continue
        port = free_port()
        process = subprocess.Popen([str(server_path), str(port)], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if not wait_for_listener(process, port):
            stdout, stderr = stop_server(process)
            print(f"example-{mode}-{server} did not listen on port {port}")
            print_process_output(subprocess.CompletedProcess([], process.returncode, stdout, stderr))
            record(False)
            continue
        ok = check_client(f"example-{mode}-{client} with {server}",
                          [str(client_path), f"http://127.0.0.1:{port}/mcp"], expected,
                          should_succeed)
        stdout, stderr = stop_server(process)
        # On Windows the server is terminated, which gives it no exit status of its own.
        if ok and sys.platform != "win32" and process.returncode != 0:
            print(f"example-{mode}-{server} ended with exit code {process.returncode} on SIGINT")
            ok = False
        if not ok:
            print_process_output(subprocess.CompletedProcess([], process.returncode, stdout, stderr))
        record(ok)

    return passed, failed


def run_benchmarks():
    build_dir = Path("build/release")
    benchmarks = sorted(exe for exe in build_dir.glob("example-benchmark-*") if is_example_executable(exe))
    if not benchmarks:
        return 0
    print(f"\nRunning {len(benchmarks)} benchmarks (30s timeout)...\n")
    passed = 0
    failed = 0
    for exe in benchmarks:
        exe_name = exe.name
        print(f"Running {exe_name}...", end=" ", flush=True)
        try:
            result = subprocess.run([str(exe)], capture_output=True, timeout=30, text=True)
            if result.returncode == 0:
                print("PASS")
                passed += 1
            else:
                print(f"FAIL (exit code {result.returncode})")
                print_process_output(result)
                failed += 1
        except subprocess.TimeoutExpired as e:
            print("TIMEOUT")
            print_timeout_output(e)
            failed += 1
        except Exception as e:
            print(f"ERROR: {e}")
            failed += 1
    return 1 if failed > 0 else 0

def print_process_output(result):
    if result.stdout:
        print("--- stdout ---")
        print(result.stdout.rstrip())
    if result.stderr:
        print("--- stderr ---")
        print(result.stderr.rstrip())

def print_timeout_output(error):
    stdout = error.stdout.decode() if isinstance(error.stdout, bytes) else error.stdout
    stderr = error.stderr.decode() if isinstance(error.stderr, bytes) else error.stderr
    if stdout:
        print("--- stdout ---")
        print(stdout.rstrip())
    if stderr:
        print("--- stderr ---")
        print(stderr.rstrip())

if __name__ == "__main__":
    exit_code = run_examples()
    if exit_code == 0:
        exit_code = run_benchmarks()
    sys.exit(exit_code)
