FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    python3 \
    pipx \
    wget \
    && rm -rf /var/lib/apt/lists/*

ENV PATH="/root/.local/bin:${PATH}"
RUN pipx install "conan>=2.0"
RUN conan profile detect --force

WORKDIR /build

COPY benchmark/cpp/conanfile.txt .
RUN mkdir -p build && \
    cd build && \
    conan install .. --output-folder=. --build=missing -s build_type=Release -s compiler.cppstd=20

COPY include/ include/
COPY benchmark/cpp/CMakeLists.txt benchmark/cpp/benchmark_server.cpp ./

WORKDIR /build/build
RUN cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20
RUN cmake --build . -j$(nproc)

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    wget \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/build/benchmark-server /usr/local/bin/

EXPOSE 8080 9080

CMD ["benchmark-server"]
