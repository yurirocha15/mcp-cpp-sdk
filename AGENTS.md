## Build and test

- Build and run the tests with `python scripts/build.py --test`.
- Format with `make format` and lint with `make lint` before committing.
- Add or update tests for any code you change.

## Commit messages

- Use conventional commit format for every commit subject, matching `docs/contributing.rst`: `feat:`, `fix:`, `docs:`, `style:`, `refactor:`, `perf:`, `test:`, `build:`, `ci:`, or `chore:` followed by an imperative summary under 80 characters.
- Before pushing, check branch commit subjects with `git log --format='%s' <base>..HEAD` and reword any non-conforming commits.

## CI guardrails

- Keep example runner status output ASCII-only. Windows GitHub runners can use legacy console encodings such as cp1252, so Unicode symbols in `scripts/run_examples.py` can fail before examples finish.
- When discovering example binaries on Windows, run only `.exe` files. MSVC can emit `.exe.manifest` sidecars next to examples, and trying to execute those manifests fails with `WinError 193`.
- Treat `examples/features/transport_memory.cpp` as compiler-sensitive on Ubuntu 22/GCC 11. Prefer direct `ITransport::write_message()` / `read_message()` memory-pair demos in that file; do not add ad-hoc full `Client`/`Server` shutdown, close, or timer teardown paths there without proving Ubuntu 22 and Windows CI.
- Use `hendrikmuhs/ccache-action@v1.2` in GitHub Actions. The broad `@v1` tag targets deprecated Node.js 20 and emits runner warnings.
