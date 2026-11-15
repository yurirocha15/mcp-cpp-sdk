# The upstream benchmark's Go server, built against the latest Go MCP SDK release (v1.8.0), which
# serves protocol revision 2026-07-28. The workload is unchanged. The compiler image, the runtime
# image, the dependency set (go.sum) and the SDK are pinned so the build cannot drift.
FROM golang@sha256:d200d2dd76b03a99b52651fd1325aff7dcd35422c35c01bac56571b0507f3deb AS builder
WORKDIR /app

COPY go.mod go.sum ./
RUN go mod download

COPY main.go .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -o server .

FROM alpine@sha256:be2656061b536c1883c4034790e21d0a09c5aaaf51968b89368a3c0371ea2c9a
RUN apk --no-cache add ca-certificates
WORKDIR /root/
COPY --from=builder /app/server .
EXPOSE 8081
CMD ["./server"]
