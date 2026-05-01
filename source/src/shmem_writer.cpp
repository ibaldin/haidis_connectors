#include "shmem_writer.hpp"
#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

ShmemWriter::ShmemWriter(const std::string& shmem_name, size_t size,
                         const std::string& sem_name, const std::string& sem_ack_name)
    : shmem_name_(shmem_name), sem_name_(sem_name), sem_ack_name_(sem_ack_name),
      shmem_size_(size), fd_(-1), ptr_(nullptr), sem_(SEM_FAILED), sem_ack_(SEM_FAILED),
      initialized_(false) {}

ShmemWriter::~ShmemWriter() {
    cleanup();
}

bool ShmemWriter::initialize() {
    // Idempotent: release any prior resources before re-initializing
    if (initialized_ || fd_ != -1 || ptr_ != nullptr || sem_ != SEM_FAILED || sem_ack_ != SEM_FAILED) {
        cleanup();
    }

    // Unlink any existing semaphores to ensure clean state
    // (Ignore errors - they may not exist)
    sem_unlink(sem_name_.c_str());
    sem_unlink(sem_ack_name_.c_str());

    // Create shared memory object
    fd_ = shm_open(shmem_name_.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ == -1) {
        std::cerr << "Failed to create shared memory: " << strerror(errno) << std::endl;
        return false;
    }

    // Set size
    if (ftruncate(fd_, shmem_size_) == -1) {
        std::cerr << "Failed to set shared memory size: " << strerror(errno) << std::endl;
        close(fd_);
        fd_ = -1;
        shm_unlink(shmem_name_.c_str());
        return false;
    }

    // Map to memory
    ptr_ = mmap(nullptr, shmem_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
        std::cerr << "Failed to map shared memory: " << strerror(errno) << std::endl;
        ptr_ = nullptr;
        close(fd_);
        fd_ = -1;
        shm_unlink(shmem_name_.c_str());
        return false;
    }

    // mmap holds its own reference; the fd is no longer needed
    close(fd_);
    fd_ = -1;

    // Create data-ready semaphore (initial value 0 - no data ready)
    sem_ = sem_open(sem_name_.c_str(), O_CREAT, 0666, 0);
    if (sem_ == SEM_FAILED) {
        std::cerr << "Failed to create semaphore: " << strerror(errno) << std::endl;
        munmap(ptr_, shmem_size_);
        ptr_ = nullptr;
        shm_unlink(shmem_name_.c_str());
        return false;
    }

    // Create acknowledgment semaphore (initial value 1 - buffer available)
    sem_ack_ = sem_open(sem_ack_name_.c_str(), O_CREAT, 0666, 1);
    if (sem_ack_ == SEM_FAILED) {
        std::cerr << "Failed to create ack semaphore: " << strerror(errno) << std::endl;
        sem_close(sem_);
        sem_ = SEM_FAILED;
        sem_unlink(sem_name_.c_str());
        munmap(ptr_, shmem_size_);
        ptr_ = nullptr;
        shm_unlink(shmem_name_.c_str());
        return false;
    }

    initialized_ = true;
    std::cout << "Shared memory initialized: " << shmem_name_ << " (" << shmem_size_ << " bytes)" << std::endl;
    std::cout << "Semaphores initialized: " << sem_name_ << ", " << sem_ack_name_ << std::endl;
    return true;
}

bool ShmemWriter::write_data(const std::vector<double>& data, uint32_t ndim, const std::vector<uint32_t>& dims, uint16_t data_id) {
    if (!initialized_) {
        std::cerr << "Shared memory not initialized" << std::endl;
        return false;
    }

    // Validate dims vector size
    if (dims.size() < ndim) {
        std::cerr << "Invalid dims: dims.size() (" << dims.size()
                  << ") must be >= ndim (" << ndim << ")" << std::endl;
        return false;
    }

    // Validate that product of dimensions matches data size
    size_t expected_elements = 1;
    for (uint32_t i = 0; i < ndim; ++i) {
        expected_elements *= dims[i];
    }
    if (expected_elements != data.size()) {
        std::cerr << "Invalid dimensions: product of dims (" << expected_elements
                  << ") != data.size() (" << data.size() << ")" << std::endl;
        return false;
    }

    // Wait until the reader has consumed the previous data
    if (sem_wait(sem_ack_) == -1) {
        if (errno == EINTR) return false;  // interrupted by signal
        std::cerr << "Failed to wait on ack semaphore: " << strerror(errno) << std::endl;
        return false;
    }

    size_t data_size = data.size() * sizeof(double);
    size_t header_size = sizeof(size_t) + sizeof(uint16_t) + sizeof(uint32_t) + ndim * sizeof(uint32_t);
    if (header_size + data_size > shmem_size_) {
        std::cerr << "Data too large for shared memory (need " << (header_size + data_size)
                  << " bytes, have " << shmem_size_ << " bytes)" << std::endl;
        // Restore semaphore before returning
        sem_post(sem_ack_);
        return false;
    }

    char* dest = static_cast<char*>(ptr_);

    // Write data_size (8 bytes)
    std::memcpy(dest, &data_size, sizeof(size_t));
    dest += sizeof(size_t);

    // Write data_id (2 bytes)
    std::memcpy(dest, &data_id, sizeof(uint16_t));
    dest += sizeof(uint16_t);

    // Write ndim (4 bytes)
    std::memcpy(dest, &ndim, sizeof(uint32_t));
    dest += sizeof(uint32_t);

    // Write dimension values (ndim * 4 bytes)
    std::memcpy(dest, dims.data(), ndim * sizeof(uint32_t));
    dest += ndim * sizeof(uint32_t);

    // Write array data
    std::memcpy(dest, data.data(), data_size);

    // Signal that new data is ready
    if (sem_post(sem_) == -1) {
        std::cerr << "Failed to post semaphore: " << strerror(errno) << std::endl;
        // Restore ack semaphore to prevent deadlock
        sem_post(sem_ack_);
        return false;
    }

    return true;
}

void ShmemWriter::cleanup() {
    if (ptr_ != nullptr && ptr_ != MAP_FAILED) {
        munmap(ptr_, shmem_size_);
        ptr_ = nullptr;
    }

    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }

    if (sem_ != SEM_FAILED) {
        sem_close(sem_);
        sem_unlink(sem_name_.c_str());
        sem_ = SEM_FAILED;
    }

    if (sem_ack_ != SEM_FAILED) {
        sem_close(sem_ack_);
        sem_unlink(sem_ack_name_.c_str());
        sem_ack_ = SEM_FAILED;
    }

    if (initialized_) {
        shm_unlink(shmem_name_.c_str());
        initialized_ = false;
    }
}
