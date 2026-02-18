cmake_minimum_required(VERSION 3.15)
project(shmem_source VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library: libshmem_writer (shared)
add_library(shmem_writer SHARED
    src/shmem_writer.cpp
)
target_include_directories(shmem_writer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(shmem_writer PUBLIC rt pthread)

# Executable: shmem_source
add_executable(shmem_source
    src/main.cpp
)
target_link_libraries(shmem_source PRIVATE shmem_writer)

# Install targets
install(TARGETS shmem_writer
    EXPORT shmem_writer-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT shmem_writer-targets
    FILE shmem_writer-config.cmake
    NAMESPACE shmem_writer::
    DESTINATION lib/cmake/shmem_writer
)

install(TARGETS shmem_source RUNTIME DESTINATION bin)
