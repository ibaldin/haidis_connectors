#!/usr/bin/env python3
"""Python destination container that reads data from shared memory."""

import os
import sys
import time
import mmap
import struct
from posix_ipc import SharedMemory, Semaphore, O_CREAT
import numpy as np


class ShmemReader:
    """Reader for POSIX shared memory."""

    def __init__(self, shmem_name: str, size: int, sem_name: str):
        self.shmem_name = shmem_name
        self.size = size
        self.sem_name = sem_name
        self.shm = None
        self.mapfile = None
        self.sem = None

    def initialize(self) -> bool:
        """Initialize shared memory connection."""
        try:
            # Open existing shared memory or create if doesn't exist
            self.shm = SharedMemory(self.shmem_name, flags=O_CREAT, size=self.size)
            self.mapfile = mmap.mmap(self.shm.fd, self.size)
            print(f"Shared memory initialized: {self.shmem_name} ({self.size} bytes)")

            # Open semaphore (will be created by C++ writer)
            self.sem = Semaphore(self.sem_name)
            print(f"Semaphore opened: {self.sem_name}")

            return True
        except Exception as e:
            print(f"Failed to initialize: {e}", file=sys.stderr)
            return False

    def wait_for_data(self):
        """Wait for new data signal from writer."""
        if self.sem is not None:
            self.sem.acquire()

    def read_data(self) -> np.ndarray | None:
        """Read data array from shared memory."""
        if self.mapfile is None:
            print("Shared memory not initialized", file=sys.stderr)
            return None

        try:
            # Read size header
            self.mapfile.seek(0)
            size_bytes = self.mapfile.read(8)
            data_size = struct.unpack('Q', size_bytes)[0]

            if data_size == 0:
                return None

            # Calculate number of doubles
            num_doubles = data_size // 8

            # Read data
            data_bytes = self.mapfile.read(data_size)
            data = np.frombuffer(data_bytes, dtype=np.float64, count=num_doubles)

            return data
        except Exception as e:
            print(f"Error reading data: {e}", file=sys.stderr)
            return None

    def cleanup(self):
        """Clean up shared memory resources."""
        if self.mapfile is not None:
            self.mapfile.close()
            self.mapfile = None

        if self.shm is not None:
            self.shm.close_fd()
            self.shm = None

        if self.sem is not None:
            self.sem.close()
            self.sem = None


def main():
    """Main entry point."""
    shmem_name = os.getenv("SHMEM_NAME", "/haidis_shmem")
    shmem_size = int(os.getenv("SHMEM_SIZE", "10485760"))
    sem_name = os.getenv("SEM_NAME", "/haidis_sem")

    print("Python Destination Container Starting...")
    print("Configuration:")
    print(f"  SHMEM_NAME: {shmem_name}")
    print(f"  SHMEM_SIZE: {shmem_size}")
    print(f"  SEM_NAME: {sem_name}")

    reader = ShmemReader(shmem_name, shmem_size, sem_name)

    if not reader.initialize():
        print("Failed to initialize shared memory reader", file=sys.stderr)
        return 1

    print("Waiting for data from C++ source...")
    iteration = 0

    try:
        while True:
            # Wait for signal that new data is available
            reader.wait_for_data()

            # Read the data
            data = reader.read_data()

            if data is not None and len(data) > 0:
                print(f"Iteration {iteration}: Read {len(data)} doubles from shared memory")
                print(f"  Min: {data.min():.6f}, Max: {data.max():.6f}, Mean: {data.mean():.6f}")
            else:
                print(f"Iteration {iteration}: No data available")

            iteration += 1

    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        reader.cleanup()

    return 0


if __name__ == "__main__":
    sys.exit(main())
