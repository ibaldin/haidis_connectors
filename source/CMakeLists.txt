cmake_minimum_required(VERSION 3.15)
project(shmem_source VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(shmem_source
    src/main.cpp
)

target_link_libraries(shmem_source rt pthread)
