# Alpine Linux version (smaller image)
FROM alpine:latest AS builder

# Install build dependencies
RUN apk add --no-cache \
    build-base \
    cmake \
    ninja \
    git \
    linux-headers

WORKDIR /workspace
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cmake --build . --parallel $(nproc)

RUN ls -R /workspace

# Runtime stage with Alpine
FROM alpine:latest

# Install minimal runtime dependencies
RUN apk add --no-cache libstdc++ && \
    adduser -D -u 1000 appuser

WORKDIR /app
COPY --from=builder /workspace/build/lab1 .
COPY --from=builder /workspace/config.txt .

USER appuser
CMD ["./lab1"]