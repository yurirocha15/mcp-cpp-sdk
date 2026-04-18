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


def _is_externally_managed_python():
    """Detect whether the system Python is PEP 668 externally managed (Ubuntu 23.04+, Debian 12+, Fedora 38+)."""
    import sysconfig
    marker = Path(sysconfig.get_path('data')) / 'lib' / 'python{}.{}'.format(
        sys.version_info.major, sys.version_info.minor) / 'EXTERNALLY-MANAGED'
    if marker.exists():
        return True
    # Also check the stdlib path (used on some distros)
    stdlib_marker = Path(sysconfig.get_path('stdlib')) / 'EXTERNALLY-MANAGED'
    return stdlib_marker.exists()


def _detect_linux_package_manager():
    """Return the first available Linux package manager command, or None."""
    for pm in ('apt-get', 'dnf', 'yum'):
        if check_command_exists(pm):
            return pm
    return None


def ensure_pipx():
    """Ensure pipx is installed and configured.

    Installation strategy (per https://pipx.pypa.io/stable/how-to/install-pipx/):
      - macOS:          brew install pipx
      - Linux apt:      sudo apt install pipx   (Ubuntu 23.04+/Debian 12+ are PEP 668 – never use pip here)
      - Linux dnf/yum:  sudo dnf install pipx
      - Windows/other:  py -m pip install --user pipx
      - Fallback:       bootstrap via a throw-away venv to avoid touching the system Python
    """
    if check_command_exists('pipx'):
        print("[+] pipx available")
        return

    print("[*] Installing pipx...")
    os_type = detect_os()

    if os_type == 'macos':
        if check_command_exists('brew'):
            run_command(['brew', 'install', 'pipx'])
        else:
            # Homebrew not available – fall back to pip
            pip_cmd = 'pip3' if check_command_exists('pip3') else 'pip'
            run_command([pip_cmd, 'install', '--user', 'pipx'])

    elif os_type == 'linux':
        pm = _detect_linux_package_manager()
        if _is_externally_managed_python():
            # PEP 668 system (Ubuntu 23.04+, Debian 12+, Fedora 38+): pip install --user is blocked.
            # Use the distro package manager; fall back to venv bootstrap if the package isn't available.
            if pm == 'apt-get':
                try:
                    run_command(['sudo', 'apt-get', 'update', '-qq'])
                    run_command(['sudo', 'apt-get', 'install', '-y', 'pipx'])
                except subprocess.CalledProcessError:
                    _install_pipx_via_venv_bootstrap()
            elif pm in ('dnf', 'yum'):
                try:
                    run_command(['sudo', pm, 'install', '-y', 'pipx'])
                except subprocess.CalledProcessError:
                    _install_pipx_via_venv_bootstrap()
            else:
                _install_pipx_via_venv_bootstrap()
        else:
            # Older / non-PEP-668 distro (e.g. Ubuntu 22.04, Debian 11): pip --user is fine.
            # Ensure pip is available first.
            ensure_pip()
            pip_cmd = 'pip3' if check_command_exists('pip3') else 'pip'
            run_command([pip_cmd, 'install', '--user', 'pipx'])

    elif os_type == 'windows':
        # Windows: py -m pip install --user pipx (scoop is optional, not always present)
        py_cmd = 'py' if check_command_exists('py') else 'python3'
        run_command([py_cmd, '-m', 'pip', 'install', '--user', 'pipx'])

    # Add ~/.local/bin to PATH for this process so subsequent pipx calls work
    local_bin = str(Path.home() / '.local' / 'bin')
    if local_bin not in os.environ.get('PATH', ''):
        os.environ['PATH'] = local_bin + os.pathsep + os.environ.get('PATH', '')

    # Ensure pipx's own bin dir is on PATH
    run_command(['pipx', 'ensurepath'], check=False)

    if not check_command_exists('pipx'):
        print("[!] Warning: pipx may not be in PATH. You may need to restart your shell "
              "or run: export PATH=\"$HOME/.local/bin:$PATH\"", file=sys.stderr)
    else:
        print("[+] pipx installed")


def _install_pipx_via_venv_bootstrap():
    """Install pipx via a throw-away venv (fallback for PEP 668 systems without a distro package).

    Mirrors the official bootstrap from https://pipx.pypa.io/stable/how-to/install-pipx/:
        python3 -m venv /tmp/bootstrap
        /tmp/bootstrap/bin/pip install pipx
        /tmp/bootstrap/bin/pipx install pipx
        rm -rf /tmp/bootstrap
        pipx ensurepath
    """
    import tempfile
    bootstrap_dir = Path(tempfile.mkdtemp(prefix='pipx-bootstrap-'))
    try:
        print(f"[*] Bootstrapping pipx via temporary venv at {bootstrap_dir}...")
        run_command([sys.executable, '-m', 'venv', str(bootstrap_dir)])
        bootstrap_pip = bootstrap_dir / 'bin' / 'pip'
        bootstrap_pipx = bootstrap_dir / 'bin' / 'pipx'
        run_command([str(bootstrap_pip), 'install', 'pipx'])
        run_command([str(bootstrap_pipx), 'install', 'pipx'])
    finally:
        import shutil as _shutil
        _shutil.rmtree(bootstrap_dir, ignore_errors=True)


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
