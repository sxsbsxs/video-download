# ---- Build stage: compilers and headers stay out of the runtime image ----
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    g++ \
    libcpp-httplib-dev \
    nlohmann-json3-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 2

# ---- Runtime stage ----
# Node 22 is supported by yt-dlp's JavaScript challenge solver.
FROM node:22-bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    PORT=10000 \
    PYTHON_BIN=/opt/venv/bin/python \
    WORKER_SCRIPT=/app/worker/extract.py \
    PUBLIC_DIR=/app/public \
    PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    ffmpeg \
    libcpp-httplib0.11 \
    python3 \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/videodl /usr/local/bin/videodl
COPY worker ./worker
COPY public ./public
COPY requirements.txt ./

RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir --upgrade pip \
    && /opt/venv/bin/pip install --no-cache-dir -r requirements.txt \
    && useradd --system --uid 10001 --create-home app \
    && chown -R app:app /app /opt/venv

USER app
EXPOSE 10000
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:10000/api/health', timeout=3).read()" || exit 1

CMD ["/usr/local/bin/videodl"]
