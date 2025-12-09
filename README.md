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

Built with CMake. See `source/` directory for details.

### Python Destination Container

Built with uv. See `destination/` directory for details.

## Configuration

Shared memory configuration parameters are defined in `shared/config.env`.
