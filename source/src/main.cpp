#include "shmem_writer.hpp"
#include <iostream>
#include <vector>
#include <random>
#include <csignal>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int /*signum*/) {
    g_running = 0;
}

int main(int argc, char* argv[]) {
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

    // Optional data_id: --data-id N (argv) or DATA_ID env var; 0 means unused
    uint16_t data_id = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--data-id") {
            data_id = static_cast<uint16_t>(std::stoul(argv[i + 1]));
            break;
        }
    }
    if (data_id == 0) {
        env = std::getenv("DATA_ID");
        if (env) data_id = static_cast<uint16_t>(std::stoul(env));
    }

    std::cout << "C++ Source Container Starting..." << std::endl;
    std::cout << "Configuration:" << std::endl;
    std::cout << "  SHMEM_NAME: " << shmem_name << std::endl;
    std::cout << "  SHMEM_SIZE: " << shmem_size << std::endl;
    std::cout << "  ARRAY_SIZE: " << array_size << std::endl;
    std::cout << "  SEM_NAME: " << sem_name << std::endl;
    std::cout << "  SEM_ACK_NAME: " << sem_ack_name << std::endl;
    std::cout << "  DATA_ID: " << data_id << " (0 = unused)" << std::endl;

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

        if (writer.write_data(data, 2, dims, data_id)) {
            std::cout << "Iteration " << iteration << ": Wrote (" << num_triples << ", " << ncols << ") doubles to shared memory"
                      << (data_id ? " (data_id=" + std::to_string(data_id) + ")" : "") << std::endl;
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
