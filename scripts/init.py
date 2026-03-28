#!/usr/bin/env python3
"""Cross-platform dependency installation script for mcp-cpp-sdk.

This script installs all project dependencies across Linux, macOS, and Windows.
It supports optional flags for dev tools and documentation dependencies.
"""

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


def detect_os():
    """Detect the operating system.

    Returns:
        str: 'linux', 'macos', or 'windows'
    """
    system = platform.system().lower()
    if system == 'linux':
        return 'linux'
    elif system == 'darwin':
        return 'macos'
    elif system == 'windows':
        return 'windows'
    else:
        print(f"[!] Unsupported OS: {system}", file=sys.stderr)
        sys.exit(1)


def check_command_exists(cmd):
    """Check if a command exists in PATH.

    Args:
        cmd: Command name to check

    Returns:
        bool: True if command exists, False otherwise
    """
    return shutil.which(cmd) is not None


def run_command(cmd, shell=False, check=True, capture_output=False):
    """Run a command and handle errors.

    Args:
        cmd: Command to run (list or string)
        shell: Whether to run in shell
        check: Whether to raise on non-zero exit
        capture_output: Whether to capture stdout/stderr

    Returns:
        CompletedProcess or None
    """
    try:
        if capture_output:
            result = subprocess.run(cmd, shell=shell, check=check,
                                   capture_output=True, text=True)
            return result
        else:
            subprocess.run(cmd, shell=shell, check=check)
            return None
    except subprocess.CalledProcessError as e:
        print(f"[!] Command failed: {cmd}", file=sys.stderr)
        if capture_output and e.stderr:
            print(f"[!] Error: {e.stderr}", file=sys.stderr)
        raise


def install_system_package(pkg, os_type, optional=False):
    """Install a system package using the OS package manager.

    Args:
        pkg: Package name
        os_type: Operating system ('linux', 'macos', 'windows')
        optional: If True, warn but don't fail on install errors
    """
    print(f"[*] Installing system package: {pkg}...")

    if os_type == 'linux':
        result = run_command(['dpkg', '-l', pkg], check=False, capture_output=True)
        if result and result.returncode == 0 and f'ii  {pkg}' in result.stdout:
            print(f"[+] {pkg} already installed")
            return

        result = run_command(['dpkg-query', '-W', '-f=${Status}', pkg],
                           check=False, capture_output=True)
        if result and 'install ok installed' in result.stdout:
            print(f"[+] {pkg} already installed")
            return

        print(f"[!] {pkg} not found. Attempting to install (may require password)...")
        try:
            run_command(['sudo', 'apt-get', 'update', '-qq'])
            run_command(['sudo', 'apt-get', 'install', '-y', pkg])
        except subprocess.CalledProcessError:
            msg = f"Failed to install {pkg}. Please install manually: sudo apt-get install {pkg}"
            if optional:
                print(f"[!] Warning: {msg}", file=sys.stderr)
                return
            else:
                print(f"[!] {msg}", file=sys.stderr)
                raise

    elif os_type == 'macos':
        result = run_command(['brew', 'list', pkg], check=False, capture_output=True)
        if result and result.returncode == 0:
            print(f"[+] {pkg} already installed")
            return

        try:
            run_command(['brew', 'install', pkg])
        except subprocess.CalledProcessError:
            msg = f"Failed to install {pkg}. Please install manually: brew install {pkg}"
            if optional:
                print(f"[!] Warning: {msg}", file=sys.stderr)
                return
            else:
                print(f"[!] {msg}", file=sys.stderr)
                raise

    elif os_type == 'windows':
        result = run_command(['choco', 'list', '--local-only', pkg],
                           check=False, capture_output=True)
        if result and pkg.lower() in result.stdout.lower():
            print(f"[+] {pkg} already installed")
            return

        try:
            run_command(['choco', 'install', '-y', pkg])
        except subprocess.CalledProcessError:
            msg = f"Failed to install {pkg}. Please install manually: choco install {pkg}"
            if optional:
                print(f"[!] Warning: {msg}", file=sys.stderr)
                return
            else:
                print(f"[!] {msg}", file=sys.stderr)
                raise

    print(f"[+] {pkg} installed")


def ensure_pip():
    """Ensure pip is installed."""
    if not check_command_exists('pip') and not check_command_exists('pip3'):
        print("[*] pip not found, installing...")
        if platform.system().lower() == 'linux':
            run_command(['sudo', 'apt-get', 'update', '-qq'])
            run_command(['sudo', 'apt-get', 'install', '-y', 'python3-pip'])
        else:
            print("[!] Please install pip manually", file=sys.stderr)
            sys.exit(1)
        print("[+] pip installed")
    else:
        print("[+] pip available")


def ensure_pipx():
    """Ensure pipx is installed and configured."""
    if not check_command_exists('pipx'):
        print("[*] Installing pipx...")
        ensure_pip()

        # Install pipx using pip --user
        pip_cmd = 'pip3' if check_command_exists('pip3') else 'pip'
        run_command([pip_cmd, 'install', '--user', 'pipx'])

        # Ensure pipx path is configured
        run_command(['python3', '-m', 'pipx', 'ensurepath'], check=False)

        print("[+] pipx installed (you may need to restart your shell)")

        # Try to find pipx in common locations
        home = Path.home()
        possible_paths = [
            home / '.local' / 'bin' / 'pipx',
            home / 'Library' / 'Python' / '*' / 'bin' / 'pipx',  # macOS
        ]

        pipx_found = False
        for path_pattern in possible_paths:
            matches = list(Path('/').glob(str(path_pattern).lstrip('/')))
            if matches and matches[0].exists():
                pipx_found = True
                break

        if not pipx_found and not check_command_exists('pipx'):
            print("[!] Warning: pipx may not be in PATH. You may need to restart your shell.",
                  file=sys.stderr)
    else:
        print("[+] pipx available")


def install_pipx_tool(tool):
    """Install a tool using pipx.

    Args:
        tool: Tool name to install via pipx
    """
    print(f"[*] Installing pipx tool: {tool}...")

    # Check if already installed
    result = run_command(['pipx', 'list'], check=False, capture_output=True)
    if result and result.returncode == 0 and tool in result.stdout:
        print(f"[+] {tool} already installed")
        return

    run_command(['pipx', 'install', tool])
    print(f"[+] {tool} installed")


def install_pip_package(pkg):
    """Install a Python package using pip.

    Args:
        pkg: Package name to install via pip
    """
    print(f"[*] Installing pip package: {pkg}...")

    pip_cmd = 'pip3' if check_command_exists('pip3') else 'pip'

    result = run_command([pip_cmd, 'show', pkg], check=False, capture_output=True)
    if result and result.returncode == 0:
        print(f"[+] {pkg} already installed")
        return

    try:
        run_command([pip_cmd, 'install', '--user', pkg])
    except subprocess.CalledProcessError:
        try:
            run_command([pip_cmd, 'install', '--user', '--break-system-packages', pkg])
        except subprocess.CalledProcessError as e:
            print(f"[!] Failed to install {pkg}. Try: pipx install {pkg} or pip install --user --break-system-packages {pkg}",
                  file=sys.stderr)
            raise

    print(f"[+] {pkg} installed")


def setup_conan_profile():
    """Set up Conan profile if it doesn't exist."""
    print("[*] Checking Conan profile...")

    # Check if profile exists
    result = run_command(['conan', 'profile', 'show', 'default'],
                        check=False, capture_output=True)

    if result and result.returncode != 0:
        print("[*] Detecting Conan profile...")
        run_command(['conan', 'profile', 'detect', '--force'])
        print("[+] Conan profile configured")
    else:
        print("[+] Conan profile already exists")


def install_cmake(os_type):
    """Install CMake if not present.

    Args:
        os_type: Operating system type
    """
    if check_command_exists('cmake'):
        print("[+] cmake already available")
        return

    print("[*] Installing cmake...")

    if os_type == 'linux':
        install_system_package('cmake', os_type)
    elif os_type == 'macos':
        install_system_package('cmake', os_type)
    elif os_type == 'windows':
        install_system_package('cmake', os_type)


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Install mcp-cpp-sdk dependencies across platforms',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s              # Install build dependencies only
  %(prog)s --dev        # Install build + dev dependencies
  %(prog)s --docs       # Install build + docs dependencies
  %(prog)s --all        # Install all dependencies
        """
    )
    parser.add_argument('--dev', action='store_true',
                       help='Install development tools (clang-format, clang-tidy, pre-commit)')
    parser.add_argument('--docs', action='store_true',
                       help='Install documentation tools (Sphinx, Breathe, Furo)')
    parser.add_argument('--all', action='store_true',
                       help='Install all dependencies (build + dev + docs)')

    args = parser.parse_args()

    # Detect OS
    os_type = detect_os()
    print(f"[*] Detected OS: {os_type}")

    try:
        # === Build dependencies (always installed) ===
        print("\n=== Installing build dependencies ===")

        # Ensure pip and pipx
        ensure_pip()
        ensure_pipx()

        # Install CMake if missing
        install_cmake(os_type)

        if os_type == 'linux':
            install_system_package('doxygen', os_type, optional=True)
        elif os_type == 'macos':
            install_system_package('doxygen', os_type, optional=True)
        elif os_type == 'windows':
            install_system_package('doxygen.install', os_type, optional=True)

        # Install build tools via pipx
        install_pipx_tool('conan')
        install_pipx_tool('gcovr')
        install_pipx_tool('ninja')

        # Set up Conan profile
        setup_conan_profile()

        if args.dev or args.all:
            print("\n=== Installing development dependencies ===")

            if os_type == 'linux':
                install_system_package('clang-format', os_type, optional=True)
                install_system_package('clang-tidy', os_type, optional=True)
            elif os_type == 'macos':
                install_system_package('llvm', os_type, optional=True)
            elif os_type == 'windows':
                install_system_package('llvm', os_type, optional=True)

            # Install pre-commit
            install_pipx_tool('pre-commit')

            # Install pre-commit hooks if in git repo
            if (Path.cwd() / '.git').exists():
                print("[*] Installing pre-commit hooks...")
                run_command(['pre-commit', 'install'], check=False)
                print("[+] pre-commit hooks installed")

        # === Documentation dependencies (optional) ===
        if args.docs or args.all:
            print("\n=== Installing documentation dependencies ===")

            # Install Sphinx and extensions
            install_pip_package('sphinx')
            install_pip_package('breathe')
            install_pip_package('furo')

        print("\n[+] All dependencies installed successfully!")
        print("\nNote: If pipx tools are not in PATH, restart your shell or run:")
        print("  export PATH=\"$HOME/.local/bin:$PATH\"")

    except subprocess.CalledProcessError as e:
        print(f"\n[!] Installation failed: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n[!] Installation interrupted by user", file=sys.stderr)
        sys.exit(130)


if __name__ == '__main__':
    main()
