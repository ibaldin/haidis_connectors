# Haidis Connectors - Shared Memory Data Transfer

This project demonstrates uni-directional data transfer of large arrays between two Docker containers using shared memory (shmem).

## Architecture

- **Source Container**: C++ application that writes data to shared memory
- **Destination Container**: Python application that reads data from shared memory
- **Transport**: POSIX shared memory for high-performance data transfer

## Directory Structure

```
haidis-connectors/
├── source/          # C++ source container (CMake)
├── destination/     # Python destination container (uv)
└── shared/          # Shared configuration
```

## Prerequisites

- Docker
- Docker Compose

## Quick Start

```bash
# Build and start both containers
docker-compose up --build

# Stop containers
docker-compose down
```

## Development

### C++ Source Container

Built with CMake. The build produces two artifacts:

- **`libshmem_writer.a`** — a standalone static library containing the `ShmemWriter` class, which can be linked into other applications
- **`shmem_source`** — the demo executable that links against the library

```bash
cd source
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To install the library and headers to a prefix (e.g., `/usr/local`):

```bash
cmake --install build --prefix /usr/local
```

This installs:
- `lib/libshmem_writer.a` — the static library
- `include/shmem_writer.hpp` — the header
- `lib/cmake/shmem_writer/` — CMake config files for `find_package(shmem_writer)`
- `bin/shmem_source` — the demo executable

To use the library from another CMake project:

```cmake
find_package(shmem_writer REQUIRED)
target_link_libraries(my_app PRIVATE shmem_writer::shmem_writer)
```

### Python Destination Container

Built with uv. See `destination/` directory for details.

## Testing

### Unit tests — C++ ShmemWriter

`source/src/test_shmem_writer.cpp` exercises initialization, input validation, and deadlock-prevention on the C++ shared memory writer. Run inside a temporary Docker container (POSIX shmem requires Linux):

```bash
DOCKER_API_VERSION=1.43 docker run --rm --ipc=host \
  -v "$(pwd)/source":/app -w /app ubuntu:22.04 \
  bash -c "cmake -B /tmp/build -DCMAKE_BUILD_TYPE=Release && \
           cmake --build /tmp/build --target test_shmem_writer && \
           /tmp/build/test_shmem_writer"
```

A successful run prints `9 passed, 0 failed`. Any `[FAIL]` line indicates the test name and the broken invariant.

### Integration test — C++ source ↔ Python destination

`scripts/integration_test.sh` builds both container images, runs them together, and validates the end-to-end data handoff. Run from the repo root:

```bash
DOCKER_API_VERSION=1.43 ./scripts/integration_test.sh [RUN_SECONDS]
# default run window: 20 seconds
```

The script validates:
- Both containers initialized shared memory and semaphores successfully
- Each side completed at least 5 read/write iterations
- The transferred array shape matches `ARRAY_SIZE` from `shared/config.env`
- All `Min`/`Max` values reported by the destination are within `[-1, 1]`
- Neither container produced any error messages

**Successful output:**
```
[PASS] source:    shmem + semaphores initialized
[PASS] destination: shmem + semaphores opened
[PASS] source:      >= 5 iterations written (NNNN)
[PASS] destination: >= 5 iterations read (NNNN)
[PASS] destination: array shape is (1000000, 3)
[PASS] destination: all Min/Max values within [-1, 1]
[PASS] source:      no error messages in log
[PASS] destination: no error messages in log
── Results: 8 passed, 0 failed ──
```

**When failures occur**, each failing check prints `[FAIL]` with a description:

| Failure message | Likely cause |
|---|---|
| `destination: >= 5 iterations read (0)` | Deadlock from stale IPC objects — run the cleanup command below, then retry |
| `destination: array shape is (...)` | `ARRAY_SIZE` mismatch between `config.env` and the running container |
| `source: no error messages in log` | C++ writer error — check the inline error text for details |
| `destination: no error messages in log` | Python reader error — check the inline error text for details |

The script tears down containers unconditionally on exit, so failed runs leave no orphaned containers or stale IPC objects from that run.

## Cleaning Up Stale IPC Objects

When containers are killed (e.g., via `docker compose down`, `SIGKILL`, or a crash), the POSIX shared memory segment and semaphores may persist in the Docker VM's `/dev/shm`. Because the containers use `ipc: host`, these objects live in the host's (or Docker VM's) IPC namespace and survive container restarts. Stale semaphores with incorrect values will cause the system to deadlock on the next run.

To clean up stale IPC objects, run a temporary container with host IPC access:

```bash
docker run --rm --ipc=host ubuntu:22.04 \
  rm -f /dev/shm/sem.haidis_sem /dev/shm/sem.haidis_sem_ack /dev/shm/haidis_shmem
```

On macOS, `/dev/shm` does not exist on the host filesystem — it only exists inside the Docker VM, so cleanup must be done from within a container as shown above.

**Symptoms of stale IPC:** Both containers start and initialize successfully, but no data is transferred (writer blocks on `sem_wait`, reader blocks on `sem.acquire`).

## Configuration

Shared memory configuration parameters are defined in `shared/config.env`.
