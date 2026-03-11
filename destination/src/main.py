#!/usr/bin/env python3
"""Python destination container that reads data from shared memory."""

import os
import sys
import time
import mmap
import struct
import random
import itertools
import multiprocessing
from posix_ipc import SharedMemory, Semaphore, ExistentialError
import numpy as np


class ShmemReader:
    """Reader for POSIX shared memory."""

    def __init__(self, shmem_name: str, size: int, sem_name: str, sem_ack_name: str):
        self.shmem_name = shmem_name
        self.size = size
        self.sem_name = sem_name
        self.sem_ack_name = sem_ack_name
        self.shm = None
        self.mapfile = None
        self.sem = None
        self.sem_ack = None

    def initialize(self) -> bool:
        """Initialize shared memory connection, retrying until the writer creates resources."""
        try:
            # Wait for the C++ writer to create shared memory
            print(f"Waiting for shared memory '{self.shmem_name}' to be created by writer...")
            while True:
                try:
                    self.shm = SharedMemory(self.shmem_name)
                    break
                except ExistentialError:
                    time.sleep(0.5)

            self.mapfile = mmap.mmap(self.shm.fd, self.size)
            print(f"Shared memory opened: {self.shmem_name} ({self.size} bytes)")

            # Wait for the C++ writer to create the semaphores
            print(f"Waiting for semaphores...")
            while True:
                try:
                    self.sem = Semaphore(self.sem_name)
                    break
                except ExistentialError:
                    time.sleep(0.5)

            while True:
                try:
                    self.sem_ack = Semaphore(self.sem_ack_name)
                    break
                except ExistentialError:
                    time.sleep(0.5)

            print(f"Semaphores opened: {self.sem_name}, {self.sem_ack_name}")

            return True
        except Exception as e:
            print(f"Failed to initialize: {e}", file=sys.stderr)
            return False

    def wait_for_data(self):
        """Wait for new data signal from writer."""
        if self.sem is not None:
            self.sem.acquire()

    def acknowledge_data(self):
        """Signal that the reader has finished consuming the data."""
        if self.sem_ack is not None:
            self.sem_ack.release()

    def read_data(self) -> np.ndarray | None:
        """Read data array from shared memory."""
        if self.mapfile is None:
            print("Shared memory not initialized", file=sys.stderr)
            return None

        try:
            self.mapfile.seek(0)

            # Read data_size (8 bytes, size_t)
            data_size = struct.unpack('Q', self.mapfile.read(8))[0]
            if data_size == 0:
                return None

            # Read ndim (4 bytes, uint32_t)
            ndim = struct.unpack('I', self.mapfile.read(4))[0]

            # Read dimension values (ndim * 4 bytes)
            dims = struct.unpack(f'{ndim}I', self.mapfile.read(ndim * 4))

            # Read array data and reshape
            num_doubles = data_size // 8
            data_bytes = self.mapfile.read(data_size)
            data = np.frombuffer(data_bytes, dtype=np.float64, count=num_doubles)

            return data.reshape(dims)
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

        if self.sem_ack is not None:
            self.sem_ack.close()
            self.sem_ack = None


def reader_worker(reader_id: int, shmem_name: str, shmem_size: int,
                  sem_name: str, sem_ack_name: str):
    reader = ShmemReader(shmem_name, shmem_size, sem_name, sem_ack_name)
    if not reader.initialize():
        print(f"[Reader {reader_id}] Failed to initialize", file=sys.stderr)
        return
    print(f"[Reader {reader_id}] Waiting for data...")
    try:
        for iteration in itertools.count():
            reader.wait_for_data()
            data = reader.read_data()
            reader.acknowledge_data()
            if data is not None and data.size > 0:
                print(f"[Reader {reader_id}] Iteration {iteration}: "
                      f"Read array {data.shape} Min: {data.min():.6f}, "
                      f"Max: {data.max():.6f}, Mean: {data.mean():.6f}")
            else:
                print(f"[Reader {reader_id}] Iteration {iteration}: No data")
            time.sleep(random.uniform(0, 0.1))   # random 0–100 ms wait
    except KeyboardInterrupt:
        pass
    finally:
        reader.cleanup()


def main():
    """Main entry point."""
    shmem_name   = os.getenv("SHMEM_NAME",    "/haidis_shmem")
    shmem_size   = int(os.getenv("SHMEM_SIZE", "10485760"))
    sem_name     = os.getenv("SEM_NAME",      "/haidis_sem")
    sem_ack_name = os.getenv("SEM_ACK_NAME",  "/haidis_sem_ack")
    num_readers  = int(os.getenv("NUM_READERS", "1"))

    print("Python Destination Container Starting...")
    print(f"  SHMEM_NAME: {shmem_name}, SHMEM_SIZE: {shmem_size}")
    print(f"  SEM_NAME: {sem_name}, SEM_ACK_NAME: {sem_ack_name}")
    print(f"  NUM_READERS: {num_readers}")

    worker_args = (shmem_name, shmem_size, sem_name, sem_ack_name)

    if num_readers == 1:
        reader_worker(0, *worker_args)
    else:
        procs = [
            multiprocessing.Process(target=reader_worker, args=(i, *worker_args))
            for i in range(num_readers)
        ]
        for p in procs:
            p.start()
        try:
            for p in procs:
                p.join()
        except KeyboardInterrupt:
            for p in procs:
                p.terminate()
            for p in procs:
                p.join()
    return 0


if __name__ == "__main__":
    sys.exit(main())
