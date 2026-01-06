# -------- Stage 1: build --------
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash build-essential cmake ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN chmod +x build.sh && ./build.sh

# -------- Stage 2: runtime --------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /app/run /app/run
RUN mkdir -p /etc/logs && chmod 0777 /etc/logs
EXPOSE 5555
CMD ["/bin/sh","-lc","ls -l /app/run && echo 'Use these binaries with docker exec or override CMD'"]
