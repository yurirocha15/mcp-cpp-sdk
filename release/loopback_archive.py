"""Serve one release candidate archive over loopback-only ephemeral HTTP."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
from typing import Iterator, Sequence
from urllib.parse import quote

from .artifacts import write_atomic
from .model import ValidationError


@contextmanager
def archive_server(archive: Path) -> Iterator[str]:
    if archive.is_symlink() or not archive.is_file():
        raise ValidationError("loopback archive must be a regular file")
    expected_path = "/" + quote(archive.name)
    content = archive.read_bytes()
    if not content:
        raise ValidationError("loopback archive must not be empty")

    class Handler(BaseHTTPRequestHandler):
        def _respond(self, include_body: bool) -> None:
            if self.path != expected_path:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            if include_body:
                self.wfile.write(content)

        def do_GET(self) -> None:  # noqa: N802 - stdlib callback name
            self._respond(True)

        def do_HEAD(self) -> None:  # noqa: N802 - stdlib callback name
            self._respond(False)

        def log_message(self, format: str, *args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}{expected_path}"
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()


def serve_until_stopped(*, archive: Path, url_file: Path) -> None:
    if url_file.exists() or url_file.is_symlink():
        raise ValidationError("loopback URL output must be new")
    with archive_server(archive) as url:
        write_atomic(url_file, (url + "\n").encode("ascii"))
        threading.Event().wait()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--url-file", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        serve_until_stopped(archive=args.archive, url_file=args.url_file)
    except (OSError, ValidationError) as error:
        raise SystemExit(f"loopback-archive: {error}") from error
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
