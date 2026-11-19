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

  - example-simple-*: The simple-mode programs, in pairs: each stdio server is
    launched by a client, and each HTTP server is started on a free port and
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

# The simple-mode stdio servers, each launched by example-simple-client-base.
SIMPLE_STDIO_SERVERS = [
    "server-base",
    "server-add-resource",
    "server-add-prompt",
    "server-add-context",
    "server-add-completions",
    "server-add-handler-pool",
    "server-add-own-loop",
    "server-add-elicitation",
    "server-add-sampling",
    "server-add-roots",
    "server-add-middleware",
    "server-add-pagination",
    "server-add-tasks",
    "server-add-subscriptions",
    "server-add-apps",
    "server-add-skills",
]

# (client, the stdio server it launches, lines the client prints).
SIMPLE_STDIO_CLIENTS = [
    ("client-add-progress", "server-add-context", ["progress: 1", "Hello, Ada!"]),
    ("client-add-logging", "server-add-context", ['log: "Composing a greeting"', "Hello, Ada!"]),
    ("client-add-coroutine", "server-base", ["Hello, Ada!", "tool: greet"]),
    ("client-add-restart", "server-base", ["Hello, Ada!"]),
    ("client-add-own-loop", "server-base", ["Hello, Ada!"]),
    ("client-add-elicitation", "server-add-elicitation", ["Hello, Dr. Ada!"]),
    ("client-add-sampling", "server-add-sampling", ["Greetings, friend!"]),
    ("client-add-roots", "server-add-roots", ["The client has 1 root(s)"]),
    ("client-add-tasks", "server-add-tasks", ["Hello, Grace!"]),
    ("client-add-subscriptions", "server-add-subscriptions",
     ["updated: notifications/resources/updated"]),
    ("client-add-errors", "server-base", ["Hello, Ada!", "request error -32602"]),
    ("client-add-legacy", "server-base", ["protocol: 2025-11-25", "Hello, Ada!"]),
]

# (HTTP server, its client, lines the client prints, whether the client succeeds). The last pair
# checks that the server with a bearer token validator refuses a client without the token.
SIMPLE_HTTP_PAIRS = [
    ("server-add-http", "client-add-http", ["Hello, Ada!"], True),
    ("server-add-auth", "client-add-auth", ["Hello, Ada!"], True),
    ("server-all", "client-all",
     ['log: "Composing a greeting"', "progress: 1", "Hello, Ada!", "Hello, Dr. Ada!",
      "Greetings, friend!", "The client has 1 root(s)", "Hello, Grace!",
      "updated: notifications/resources/updated", "request error -32602", "tool: greet"], True),
    ("server-add-auth", "client-add-http", [], False),
]

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

    print("\nRunning the simple-mode programs\n")
    simple_passed, simple_failed = run_simple_examples(build_dir)
    passed += simple_passed
    failed += simple_failed

    print(f"\n{'='*50}")
    print(f"Results: {passed} passed, {failed} failed")

    if failed > 0:
        print("\nSome examples failed!")
        return 1
    else:
        print("\nAll examples passed!")
        return 0

def simple_executable(build_dir, name):
    for candidate in (build_dir / f"example-simple-{name}", build_dir / f"example-simple-{name}.exe"):
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
    """Run one simple-mode client; return True when it ends and prints as expected."""
    print(f"Running {label}...", end=" ", flush=True)
    try:
        result = subprocess.run(command, capture_output=True, timeout=30, text=True)
    except subprocess.TimeoutExpired as error:
        print("TIMEOUT")
        print_timeout_output(error)
        return False
    if not should_succeed:
        if result.returncode != 0:
            print("PASS (refused)")
            return True
        print("FAIL (the client was served)")
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


def run_simple_examples(build_dir):
    """Run the simple-mode programs in pairs; return (passed, failed)."""
    passed = 0
    failed = 0

    def record(ok):
        nonlocal passed, failed
        if ok:
            passed += 1
        else:
            failed += 1

    client_base = simple_executable(build_dir, "client-base")
    if client_base is None:
        print("example-simple-client-base not found")
        return 0, 1
    for server in SIMPLE_STDIO_SERVERS:
        server_path = simple_executable(build_dir, server)
        if server_path is None:
            print(f"example-simple-{server} not found")
            record(False)
            continue
        record(check_client(f"example-simple-client-base with {server}",
                            [str(client_base), str(server_path)], ["Hello, Ada!"], True))

    for client, server, expected in SIMPLE_STDIO_CLIENTS:
        client_path = simple_executable(build_dir, client)
        server_path = simple_executable(build_dir, server)
        if client_path is None or server_path is None:
            print(f"example-simple-{client} or example-simple-{server} not found")
            record(False)
            continue
        record(check_client(f"example-simple-{client} with {server}",
                            [str(client_path), str(server_path)], expected, True))

    for server, client, expected, should_succeed in SIMPLE_HTTP_PAIRS:
        if not should_succeed and sys.platform == "win32":
            # The refused client ends with an uncaught exception, which Windows may report in a
            # dialog instead of an exit status.
            continue
        server_path = simple_executable(build_dir, server)
        client_path = simple_executable(build_dir, client)
        if client_path is None or server_path is None:
            print(f"example-simple-{client} or example-simple-{server} not found")
            record(False)
            continue
        port = free_port()
        process = subprocess.Popen([str(server_path), str(port)], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if not wait_for_listener(process, port):
            stdout, stderr = stop_server(process)
            print(f"example-simple-{server} did not listen on port {port}")
            print_process_output(subprocess.CompletedProcess([], process.returncode, stdout, stderr))
            record(False)
            continue
        ok = check_client(f"example-simple-{client} with {server}",
                          [str(client_path), f"http://127.0.0.1:{port}/mcp"], expected,
                          should_succeed)
        stdout, stderr = stop_server(process)
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
