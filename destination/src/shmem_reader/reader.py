"""POSIX shared memory reader."""

import sys
import time
import mmap
import struct
from posix_ipc import SharedMemory, Semaphore, ExistentialError, BusyError
import numpy as np


class ShmemReader:
    """Reader for POSIX shared memory produced by a C++ ShmemWriter."""

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
            print(f"Waiting for shared memory '{self.shmem_name}' to be created by writer...")
            while True:
                try:
                    self.shm = SharedMemory(self.shmem_name)
                    break
                except ExistentialError:
                    time.sleep(0.5)

            self.mapfile = mmap.mmap(self.shm.fd, self.size)
            print(f"Shared memory opened: {self.shmem_name} ({self.size} bytes)")

            print("Waiting for semaphores...")
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

            # Drain any stale data-ready signals left by a prior run or an
            # eager writer that posted before the reader was ready.  Each
            # drained signal must be matched with an ack so the writer can
            # proceed.
            drained = 0
            while True:
                try:
                    self.sem.acquire(timeout=0)
                    drained += 1
                    self.sem_ack.release()
                except BusyError:
                    break
            if drained > 0:
                print(
                    f"Drained {drained} stale signal(s) from {self.sem_name} (acks sent)",
                    file=sys.stderr,
                )

            return True
        except Exception as e:
            print(f"Failed to initialize: {e}", file=sys.stderr)
            return False

    def wait_for_data(self):
        """Block until the writer signals new data is ready."""
        if self.sem is not None:
            self.sem.acquire()

    def acknowledge_data(self):
        """Signal that the reader has finished consuming the data."""
        if self.sem_ack is not None:
            self.sem_ack.release()

    def read_data(self) -> np.ndarray | tuple[np.ndarray, int] | None:
        """Read data array from shared memory.

        Returns the array directly when data_id is 0 (default/unused),
        or (array, data_id) when the writer set a non-zero data_id.
        Returns None on error.
        """
        if self.mapfile is None:
            print("Shared memory not initialized", file=sys.stderr)
            return None

        try:
            self.mapfile.seek(0)

            data_size = struct.unpack('Q', self.mapfile.read(8))[0]
            if data_size == 0:
                return None

            data_id = struct.unpack('H', self.mapfile.read(2))[0]
            ndim = struct.unpack('I', self.mapfile.read(4))[0]
            dims = struct.unpack(f'{ndim}I', self.mapfile.read(ndim * 4))

            num_doubles = data_size // 8
            data_bytes = self.mapfile.read(data_size)
            data = np.frombuffer(data_bytes, dtype=np.float64, count=num_doubles)
            array = data.reshape(dims)
            return array if data_id == 0 else (array, data_id)
        except Exception as e:
            print(f"Error reading data: {e}", file=sys.stderr)
            return None

    def cleanup(self):
        """Release all shared memory resources."""
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
