# The upstream benchmark's Rust server, ported to the official Rust MCP SDK (rmcp) 3.8.0, which
# serves protocol revision 2026-07-28. The workload is unchanged except that checkout returns the
# rate-limit counter (the INCR result), as the other servers do. The compiler image, the runtime
# image, the dependency set (Cargo.lock) and rmcp itself are pinned so the build cannot drift.
FROM rust@sha256:b8e062b1c3a80183df83d9ba187cadd2b5ab84bc78d453dbfeb2fccd5f982247 AS builder
WORKDIR /app

# Build the dependencies once, apart from the sources.
COPY Cargo.toml Cargo.lock ./
RUN mkdir src && echo 'fn main() {}' > src/main.rs
RUN cargo build --release --locked
RUN rm src/main.rs

COPY src/ src/
RUN touch src/main.rs && cargo build --release --locked

FROM debian@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3
RUN apt-get update && apt-get install -y ca-certificates curl && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/target/release/rust-mcp-server /usr/local/bin/
EXPOSE 8095
CMD ["rust-mcp-server"]
