# ── STAGE 1: BUILD THE C++ BINARY ───────────────────────────────────
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Install essential build tools and libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set up the internal build workspace
WORKDIR /app

# Copy your source code into the builder container
# (Assumes your main.cpp, Makefile/CMakeLists, etc., are in your project root)
COPY . .

# Compile your application (Swap this line if you use CMake or standard g++)
RUN g++ -std=c++17 -O2 -pthread -o server server.cpp  


# ── STAGE 2: RUNTIME ENGINE ─────────────────────────────────────────
FROM ubuntu:22.04

WORKDIR /app

# Install runtime dependencies if your server needs them (like OpenSSL)
RUN apt-get update && apt-get install -y \
    ca-certificates \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

# Copy ONLY the compiled executable binary from the builder stage
COPY --from=builder /app/server /app/server

# Copy your frontend web files (HTML, CSS, JS, images) so the server can serve them
COPY ./index.html /app/index.html
COPY ./images /app/images

# Expose the internal networking pipeline port
EXPOSE 8080

# Launch the executable command when the container initializes
CMD ["./server"]
