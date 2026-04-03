FROM ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    libquickfix-dev \
    wget \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt .
COPY core/ ./core/
COPY sequencer/ ./sequencer/
RUN cmake -B build -S .
RUN cmake --build build --target sequencer_app -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    libboost-system1.74.0 \
    libquickfix17 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /app/build/sequencer/sequencer_app .
CMD ["./sequencer_app"]
