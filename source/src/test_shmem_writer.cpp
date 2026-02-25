#include "shmem_writer.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

static int passed = 0;
static int failed = 0;

#define CHECK(label, cond) \
    do { \
        if (cond) { std::cout << "[PASS] " << (label) << std::endl; ++passed; } \
        else      { std::cout << "[FAIL] " << (label) << std::endl; ++failed; } \
    } while (0)

// Unique names to avoid conflict with production objects
static const std::string SHM     = "/tw_shmem";
static const std::string SEM     = "/tw_sem";
static const std::string SEM_ACK = "/tw_sem_ack";
static const size_t      SZ      = 65536;

// -------------------------------------------------------------------------
// Test 1: basic initialize() succeeds
// -------------------------------------------------------------------------
static void test_init_success() {
    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    CHECK("init: basic initialize() succeeds", w.initialize());
}

// -------------------------------------------------------------------------
// Test 2: initialize() succeeds even with stale semaphores left at wrong
//         values (validates sem_unlink at top of initialize())
// -------------------------------------------------------------------------
static void test_init_stale_semaphores() {
    // Plant stale semaphores with wrong values (sem=5, sem_ack=0)
    sem_t* s = sem_open(SEM.c_str(), O_CREAT, 0666, 5);
    if (s != SEM_FAILED) sem_close(s);
    sem_t* sa = sem_open(SEM_ACK.c_str(), O_CREAT, 0666, 0);
    if (sa != SEM_FAILED) sem_close(sa);

    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    bool ok = w.initialize();
    CHECK("init: succeeds with stale semaphores (sem_unlink at start)", ok);

    if (ok) {
        // Verify sem_ack_ was reset to 1 (not left at stale 0).
        // Open the live sem_ack and do a non-blocking wait – should succeed.
        sem_t* live_ack = sem_open(SEM_ACK.c_str(), 0);
        bool ack_available = false;
        if (live_ack != SEM_FAILED) {
            ack_available = (sem_trywait(live_ack) == 0);
            if (ack_available) sem_post(live_ack);  // put it back
            sem_close(live_ack);
        }
        CHECK("init: sem_ack value reset to 1 (not stale 0)", ack_available);
    }
}

// -------------------------------------------------------------------------
// Test 3: write_data() fails early when dims.size() < ndim
//         (validation happens before sem_wait, so sem_ack must stay intact)
// -------------------------------------------------------------------------
static void test_write_dims_too_few() {
    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    w.initialize();
    std::vector<double>   data = {1.0, 2.0, 3.0, 4.0};
    std::vector<uint32_t> dims = {2};       // ndim=2 but only 1 dim value
    CHECK("write: fails when dims.size() < ndim", !w.write_data(data, 2, dims));
}

// -------------------------------------------------------------------------
// Test 4: write_data() fails when product(dims) != data.size()
// -------------------------------------------------------------------------
static void test_write_dims_product_mismatch() {
    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    w.initialize();
    std::vector<double>   data = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};  // 6 elements
    std::vector<uint32_t> dims = {2, 4};    // product=8, mismatch
    CHECK("write: fails when product(dims) != data.size()", !w.write_data(data, 2, dims));
}

// -------------------------------------------------------------------------
// Test 5: write_data() returns false for oversized data AND restores sem_ack_
//         so subsequent calls don't deadlock (key fix in commit)
// -------------------------------------------------------------------------
static void test_write_too_large_restores_sem_ack() {
    const size_t small = 128;   // tiny shared memory
    ShmemWriter w(SHM, small, SEM, SEM_ACK);
    w.initialize();

    std::vector<double>   data(100, 1.0);   // 800 bytes >> 128
    std::vector<uint32_t> dims = {100};

    bool first = w.write_data(data, 1, dims);
    CHECK("write: returns false for oversized data", !first);

    // If sem_ack_ was NOT restored, this second call would block forever.
    // Run it in a thread with a timeout to detect deadlock.
    std::atomic<bool> second_returned{false};
    std::thread t([&]() {
        w.write_data(data, 1, dims);
        second_returned = true;
    });
    // Give it 2 seconds – far more than needed for a non-blocking path
    t.detach();
    sleep(2);
    CHECK("write: sem_ack_ restored – second call returns without deadlock",
          second_returned.load());
}

// -------------------------------------------------------------------------
// Test 6: successful round-trip – write produces a signal the consumer reads
// -------------------------------------------------------------------------
static void test_write_roundtrip() {
    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    w.initialize();

    std::atomic<bool> consumer_got_signal{false};
    std::thread consumer([&]() {
        sem_t* sem     = sem_open(SEM.c_str(), 0);
        sem_t* sem_ack = sem_open(SEM_ACK.c_str(), 0);
        if (sem == SEM_FAILED || sem_ack == SEM_FAILED) return;
        if (sem_wait(sem) == 0) {
            consumer_got_signal = true;
            sem_post(sem_ack);
        }
        sem_close(sem);
        sem_close(sem_ack);
    });

    std::vector<double>   data = {1.0, 2.0, 3.0};
    std::vector<uint32_t> dims = {3};
    bool write_ok = w.write_data(data, 1, dims);
    consumer.join();

    CHECK("write: write_data() succeeds for valid data", write_ok);
    CHECK("write: consumer received the data-ready signal", consumer_got_signal.load());
}

// -------------------------------------------------------------------------
int main() {
    std::cout << "=== ShmemWriter unit tests ===" << std::endl << std::endl;

    test_init_success();
    std::cout << std::endl;

    test_init_stale_semaphores();
    std::cout << std::endl;

    test_write_dims_too_few();
    std::cout << std::endl;

    test_write_dims_product_mismatch();
    std::cout << std::endl;

    test_write_too_large_restores_sem_ack();
    std::cout << std::endl;

    test_write_roundtrip();
    std::cout << std::endl;

    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;
    return failed > 0 ? 1 : 0;
}
