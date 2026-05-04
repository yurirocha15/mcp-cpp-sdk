#!/usr/bin/env python3
"""
Cross-platform example runner for mcp-cpp-sdk.

Runs all non-interactive examples that are safe for CI automation.

Examples that ARE run:
  - example-feature-*: Feature demonstration examples (all use loopback pattern)
  - example-echo-websocket: WebSocket transport loopback
  - example-http-loopback: HTTP transport loopback
  - example-benchmark-*: Throughput measurement examples

Examples NOT run (require stdin or block indefinitely):
  - example-client-stdio: Interactive client, waits for stdin input
  - example-interactive-client: Interactive client, waits for stdin input
  - example-server-stdio: Server blocks waiting for stdio connection
  - example-server-simple: Server blocks waiting for stdio connection
  - example-debugger-server: Long-running HTTP server, blocks indefinitely
  - example-llama-mcp: Requires llama.cpp library
  - example-server-sampling: Requires LLM sampling connection
"""
import subprocess
import sys
from pathlib import Path

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
        list(build_dir.glob("example-http-loopback"))
    )
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
                print("✓ PASS")
                passed += 1
            else:
                print(f"✗ FAIL (exit code {result.returncode})")
                failed += 1

        except subprocess.TimeoutExpired:
            print("✗ TIMEOUT")
            failed += 1
        except Exception as e:
            print(f"✗ ERROR: {e}")
            failed += 1

    print(f"\n{'='*50}")
    print(f"Results: {passed} passed, {failed} failed")

    if failed > 0:
        print("\n❌ Some examples failed!")
        return 1
    else:
        print("\n✅ All examples passed!")
        return 0

def run_benchmarks():
    build_dir = Path("build/release")
    benchmarks = sorted(build_dir.glob("example-benchmark-*"))
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
                print("✓ PASS")
                passed += 1
            else:
                print(f"✗ FAIL (exit code {result.returncode})")
                failed += 1
        except subprocess.TimeoutExpired:
            print("✗ TIMEOUT")
            failed += 1
        except Exception as e:
            print(f"✗ ERROR: {e}")
            failed += 1
    return 1 if failed > 0 else 0

if __name__ == "__main__":
    exit_code = run_examples()
    if exit_code == 0:
        exit_code = run_benchmarks()
    sys.exit(exit_code)
