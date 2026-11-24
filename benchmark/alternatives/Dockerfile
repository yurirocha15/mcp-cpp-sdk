FROM ubuntu@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db

ARG DEBIAN_FRONTEND=noninteractive
ARG SDK_NAME
ARG HTTPLIB_TCP_NODELAY=OFF
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    git \
    libcurl4-openssl-dev \
    libhiredis-dev \
    nlohmann-json3-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY alternatives/ /src/alternatives/
COPY alternative-sdks/ /src/alternative-sdks/

RUN test -n "${SDK_NAME}" && test -d "/src/alternative-sdks/${SDK_NAME}"
# Compile jobs; the machine builds one image at a time and each translation unit needs gigabytes.
ARG BUILD_JOBS=4
RUN cmake -S /src/alternatives -B /build \
      -DBENCHMARK_SDK="${SDK_NAME}" \
      -DBENCHMARK_SDK_SOURCE="/src/alternative-sdks/${SDK_NAME}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DBENCHMARK_HTTPLIB_TCP_NODELAY="${HTTPLIB_TCP_NODELAY}" \
    && cmake --build /build --target alternative-benchmark-server -j"${BUILD_JOBS}"

ENV PORT=8080
EXPOSE 8080
CMD ["/build/alternative-benchmark-server"]
