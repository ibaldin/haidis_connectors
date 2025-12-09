#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <semaphore.h>

class ShmemWriter {
public:
    ShmemWriter(const std::string& shmem_name, size_t size, const std::string& sem_name);
    ~ShmemWriter();

    bool initialize();
    bool write_data(const std::vector<double>& data);
    void cleanup();

private:
    std::string shmem_name_;
    std::string sem_name_;
    size_t shmem_size_;
    int fd_;
    void* ptr_;
    sem_t* sem_;
    bool initialized_;
};
