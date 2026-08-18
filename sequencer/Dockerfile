FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

# Install build and development tools
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    clangd \
    clang-format \
    libboost-all-dev \
    libgtest-dev \
    libquickfix-dev \
    wget \
    gdb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy essential project files for building
COPY CMakeLists.txt .
COPY core/ ./core/
COPY sequencer/ ./sequencer/
COPY matching_engine/ ./matching_engine/

# Pre-build the application
RUN cmake -B build -S .
RUN cmake --build build --parallel

# Default command remains as sleep for development containers
# This allows the user to exec into it or run commands via docker-compose
CMD ["sleep", "infinity"]
