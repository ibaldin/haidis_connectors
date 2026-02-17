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
