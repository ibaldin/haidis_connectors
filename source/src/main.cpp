#include "shmem_writer.hpp"
#include <iostream>
#include <vector>
#include <random>
#include <cstring>
#include <csignal>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int /*signum*/) {
    g_running = 0;
}

ShmemWriter::ShmemWriter(const std::string& shmem_name, size_t size,
                         const std::string& sem_name, const std::string& sem_ack_name)
    : shmem_name_(shmem_name), sem_name_(sem_name), sem_ack_name_(sem_ack_name),
      shmem_size_(size), fd_(-1), ptr_(nullptr), sem_(SEM_FAILED), sem_ack_(SEM_FAILED),
      initialized_(false) {}

ShmemWriter::~ShmemWriter() {
    cleanup();
}

bool ShmemWriter::initialize() {
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
        return false;
    }

    // Map to memory
    ptr_ = mmap(nullptr, shmem_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
        std::cerr << "Failed to map shared memory: " << strerror(errno) << std::endl;
        close(fd_);
        return false;
    }

    // Create data-ready semaphore (initial value 0 - no data ready)
    sem_ = sem_open(sem_name_.c_str(), O_CREAT, 0666, 0);
    if (sem_ == SEM_FAILED) {
        std::cerr << "Failed to create semaphore: " << strerror(errno) << std::endl;
        munmap(ptr_, shmem_size_);
        close(fd_);
        return false;
    }

    // Create acknowledgment semaphore (initial value 1 - buffer available)
    sem_ack_ = sem_open(sem_ack_name_.c_str(), O_CREAT, 0666, 1);
    if (sem_ack_ == SEM_FAILED) {
        std::cerr << "Failed to create ack semaphore: " << strerror(errno) << std::endl;
        sem_close(sem_);
        sem_unlink(sem_name_.c_str());
        munmap(ptr_, shmem_size_);
        close(fd_);
        return false;
    }

    initialized_ = true;
    std::cout << "Shared memory initialized: " << shmem_name_ << " (" << shmem_size_ << " bytes)" << std::endl;
    std::cout << "Semaphores initialized: " << sem_name_ << ", " << sem_ack_name_ << std::endl;
    return true;
}

bool ShmemWriter::write_data(const std::vector<double>& data, uint32_t ndim, const std::vector<uint32_t>& dims) {
    if (!initialized_) {
        std::cerr << "Shared memory not initialized" << std::endl;
        return false;
    }

    // Wait until the reader has consumed the previous data
    if (sem_wait(sem_ack_) == -1) {
        if (errno == EINTR) return false;  // interrupted by signal
        std::cerr << "Failed to wait on ack semaphore: " << strerror(errno) << std::endl;
        return false;
    }

    size_t data_size = data.size() * sizeof(double);
    size_t header_size = sizeof(size_t) + sizeof(uint32_t) + ndim * sizeof(uint32_t);
    if (header_size + data_size > shmem_size_) {
        std::cerr << "Data too large for shared memory" << std::endl;
        return false;
    }

    char* dest = static_cast<char*>(ptr_);

    // Write data_size (8 bytes)
    std::memcpy(dest, &data_size, sizeof(size_t));
    dest += sizeof(size_t);

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

int main() {
    // Register signal handlers for graceful shutdown
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    const char* env;

    env = std::getenv("SHMEM_NAME");
    const std::string shmem_name = env ? env : "/haidis_shmem";

    env = std::getenv("SHMEM_SIZE");
    const size_t shmem_size = env ? std::stoul(env) : 10485760;

    env = std::getenv("ARRAY_SIZE");
    const size_t array_size = env ? std::stoul(env) : 1000000;

    env = std::getenv("SEM_NAME");
    const std::string sem_name = env ? env : "/haidis_sem";

    env = std::getenv("SEM_ACK_NAME");
    const std::string sem_ack_name = env ? env : "/haidis_sem_ack";

    std::cout << "C++ Source Container Starting..." << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  SHMEM_NAME: " << shmem_name << std::endl;
    std::cout << "  SHMEM_SIZE: " << shmem_size << std::endl;
    std::cout << "  ARRAY_SIZE: " << array_size << std::endl;
    std::cout << "  SEM_NAME: " << sem_name << std::endl;
    std::cout << "  SEM_ACK_NAME: " << sem_ack_name << std::endl;

    ShmemWriter writer(shmem_name, shmem_size, sem_name, sem_ack_name);

    if (!writer.initialize()) {
        std::cerr << "Failed to initialize shared memory writer" << std::endl;
        return 1;
    }

    // Generate and write 2D array of random triples
    const size_t num_triples = array_size;
    const uint32_t ncols = 3;
    std::vector<double> data(num_triples * ncols);
    std::vector<uint32_t> dims = {static_cast<uint32_t>(num_triples), ncols};

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    int iteration = 0;

    while (g_running) {
        // Generate random triples
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = dist(rng);
        }

        if (writer.write_data(data, 2, dims)) {
            std::cout << "Iteration " << iteration << ": Wrote (" << num_triples << ", " << ncols << ") doubles to shared memory" << std::endl;
        } else {
            if (!g_running) break;
            std::cerr << "Failed to write data" << std::endl;
        }

        iteration++;
    }

    std::cout << "Shutting down gracefully..." << std::endl;
    // Destructor calls cleanup(), unlinking shmem and semaphores
    return 0;
}
