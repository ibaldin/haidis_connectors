# C++ Source Container
FROM ubuntu:22.04

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY CMakeLists.txt .
COPY src/ src/
COPY include/ include/

# Build application
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

# Run the application
CMD ["./build/shmem_source"]
