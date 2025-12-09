# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a demonstration project showing uni-directional, high-performance data transfer between two Docker containers using POSIX shared memory (shmem) with semaphore-based synchronization.

**Architecture:**
- **Source container (C++)**: Writes large arrays to shared memory and signals availability via semaphore
- **Destination container (Python)**: Blocks on semaphore, reads data when signaled
- **IPC mechanism**: Both containers share `ipc: host` namespace for access to shared memory and semaphores

## Key Commands

### Running the System
```bash
# Build and start both containers
docker compose up --build

# Start without rebuilding
docker compose up

# Stop all containers
docker compose down

# View logs from specific container
docker logs haidis-source
docker logs haidis-destination
```

### Development - C++ Source Container

```bash
# Build manually (inside container or with Docker)
cd source
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run standalone
./build/shmem_source
```

### Development - Python Destination Container

```bash
# Install dependencies with uv
cd destination
uv pip install -e .

# Run standalone
python src/main.py
```

## Architecture Details

### Synchronization Mechanism

The system uses **POSIX named semaphores** for producer-consumer synchronization:

1. **C++ writer** (source/src/main.cpp):
   - Creates shared memory segment and semaphore (initial value 0)
   - Writes data to shmem
   - Calls `sem_post()` to signal data ready

2. **Python reader** (destination/src/main.py):
   - Opens existing shared memory and semaphore
   - Calls `sem.acquire()` which **blocks** until C++ signals
   - Reads data immediately when woken up

This eliminates polling and ensures the reader only processes new data.

### Shared Memory Layout

Memory structure in the shared memory segment:
```
[0-7 bytes]: size_t data_size (number of bytes in array)
[8-N bytes]: double array data (data_size bytes)
```

Both C++ and Python must agree on this layout.

### IPC Namespace Sharing

Critical configuration in docker-compose.yml:
```yaml
services:
  source:
    ipc: host  # Makes IPC objects accessible
  destination:
    ipc: host  # Shares same namespace
```

Both containers access the same kernel IPC resources:
- **Shared memory**: `/haidis_shmem`
- **Semaphore**: `/haidis_sem`

### Configuration

All IPC parameters are centralized in `shared/config.env`:
- `SHMEM_NAME`: Name of shared memory object
- `SHMEM_SIZE`: Size in bytes (must accommodate data + size header)
- `ARRAY_SIZE`: Number of doubles to transfer
- `SEM_NAME`: Name of semaphore for synchronization

## Code Organization

### C++ Source Container (source/)
- **include/shmem_writer.hpp**: Class interface for shared memory operations
- **src/main.cpp**: Implementation of ShmemWriter + main loop that generates sine wave data
- **CMakeLists.txt**: Links against `rt` (POSIX realtime) and `pthread` for semaphore support

### Python Destination Container (destination/)
- **src/main.py**: ShmemReader class using `posix_ipc` and `numpy`
- **pyproject.toml**: Dependencies managed by `uv` package manager

## Modifying Data Transfer

To change what data is transferred:

1. **Update ARRAY_SIZE** in `shared/config.env`
2. **Ensure SHMEM_SIZE** is large enough: `SHMEM_SIZE >= (ARRAY_SIZE * 8) + 8`
3. **Modify data generation** in C++ main loop (source/src/main.cpp:~110)
4. **Modify data processing** in Python main loop (destination/src/main.py:~118)

## Important Implementation Notes

- **Semaphore semantics**: Initial value is 0 (no data ready). Each `sem_post()` increments, each `acquire()` decrements and blocks if zero.
- **No mutex needed**: Single producer, single consumer with one-way data flow
- **Data format**: Currently doubles (8 bytes), but any binary format works if both sides agree
- **Cleanup**: C++ source owns semaphore/shmem creation and unlinking on shutdown
- **Python unbuffering**: `PYTHONUNBUFFERED=1` set in Dockerfile for immediate log output
