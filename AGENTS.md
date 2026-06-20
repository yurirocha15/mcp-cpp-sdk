## graphify

This project has a graphify knowledge graph at graphify-out/.

Rules:
- Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
- If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
- For cross-module "how does X relate to Y" questions, prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, or `graphify explain "<concept>"` over grep — these traverse the graph's EXTRACTED + INFERRED edges instead of scanning files
- After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)

## CI guardrails

- Keep example runner status output ASCII-only. Windows GitHub runners can use legacy console encodings such as cp1252, so Unicode symbols in `scripts/run_examples.py` can fail before examples finish.
- Treat `examples/features/transport_memory.cpp` as compiler-sensitive on Ubuntu 22/GCC 11. Prefer direct `ITransport::write_message()` / `read_message()` memory-pair demos in that file; do not add ad-hoc full `Client`/`Server` shutdown, close, or timer teardown paths there without proving Ubuntu 22 and Windows CI.
- Use `hendrikmuhs/ccache-action@v1.2` in GitHub Actions. The broad `@v1` tag targets deprecated Node.js 20 and emits runner warnings.
