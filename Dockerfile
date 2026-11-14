# Multi-stage Dockerfile for Multithreaded Bank Transaction Management System
#
# Stage 1: Build the C++20 Linux ELF binaries inside Ubuntu
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libmysqlclient-dev \
    ca-certificates \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Copy source tree and configuration
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/
COPY tests/ tests/

# Compile bank and bank_tests in Release mode
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --parallel

# Stage 2: Minimal runtime image
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

# Install runtime MySQL client library
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmysqlclient21 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create a dedicated non-root user
RUN groupadd -r bankuser && useradd -r -g bankuser -m -s /sbin/nologin bankuser

WORKDIR /workspace

# Copy compiled Linux binaries from builder
COPY --from=builder /workspace/build/bank /usr/local/bin/bank
COPY --from=builder /workspace/build/bank_tests /usr/local/bin/bank_tests

# Copy assets and schemas
COPY sql/ sql/
COPY results/ results/
COPY web/ web/
COPY docs/ docs/
COPY .env.example .env.example

RUN chmod +x /usr/local/bin/bank /usr/local/bin/bank_tests && \
    chown -R bankuser:bankuser /workspace

# Default database connection parameters
ENV BANK_DB_HOST=mysql \
    BANK_DB_PORT=3306 \
    BANK_DB_USER=root \
    BANK_DB_PASS=root \
    BANK_DB_NAME=bank

USER bankuser

ENTRYPOINT ["/usr/local/bin/bank"]
CMD ["--accounts", "10", "--clients", "5", "--ops", "100", "--workers", "4", "--seed", "42", "--verbose"]
