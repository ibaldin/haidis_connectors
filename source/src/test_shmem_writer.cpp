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
    CHECK("write: fails when dims.size() < ndim", !w.write_data(data, 2, dims, 0));
}

// -------------------------------------------------------------------------
// Test 4: write_data() fails when product(dims) != data.size()
// -------------------------------------------------------------------------
static void test_write_dims_product_mismatch() {
    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    w.initialize();
    std::vector<double>   data = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};  // 6 elements
    std::vector<uint32_t> dims = {2, 4};    // product=8, mismatch
    CHECK("write: fails when product(dims) != data.size()", !w.write_data(data, 2, dims, 0));
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

    bool first = w.write_data(data, 1, dims, 0);
    CHECK("write: returns false for oversized data", !first);

    // If sem_ack_ was NOT restored, this second call would block forever.
    // Run it in a thread with a timeout to detect deadlock.
    std::atomic<bool> second_returned{false};
    std::thread t([&]() {
        w.write_data(data, 1, dims, 0);
        second_returned = true;
    });
    // Give it 2 seconds – far more than needed for a non-blocking path
    t.detach();
    sleep(2);
    CHECK("write: sem_ack_ restored – second call returns without deadlock",
          second_returned.load());
}

// -------------------------------------------------------------------------
// Test 6: initialize() is idempotent – calling it twice should not leak
//         resources or break subsequent writes
// -------------------------------------------------------------------------
static void test_init_idempotent() {
    ShmemWriter w(SHM, SZ, SEM, SEM_ACK);
    bool first  = w.initialize();
    bool second = w.initialize();
    CHECK("init idempotent: first  initialize() succeeds", first);
    CHECK("init idempotent: second initialize() succeeds", second);

    // Round-trip after re-init to confirm semaphores/shmem are wired up correctly
    std::atomic<bool> consumer_got_signal{false};
    std::thread consumer([&]() {
        sem_t* sem     = sem_open(SEM.c_str(), 0);
        sem_t* sem_ack = sem_open(SEM_ACK.c_str(), 0);
        if (sem == SEM_FAILED || sem_ack == SEM_FAILED) {
            if (sem     != SEM_FAILED) sem_close(sem);
            if (sem_ack != SEM_FAILED) sem_close(sem_ack);
            return;
        }
        if (sem_wait(sem) == 0) {
            consumer_got_signal = true;
            sem_post(sem_ack);
        }
        sem_close(sem);
        sem_close(sem_ack);
    });

    std::vector<double>   data = {1.0, 2.0};
    std::vector<uint32_t> dims = {2};
    bool write_ok = w.write_data(data, 1, dims, 0);
    consumer.join();

    CHECK("init idempotent: write after re-init succeeds", write_ok);
    CHECK("init idempotent: consumer received signal after re-init",
          consumer_got_signal.load());
}

// -------------------------------------------------------------------------
// Test 7: when initialize() fails partway through, the destructor must NOT
//         double-close the fd / double-munmap / double-close the semaphores
//         it already released on the error path. Detect by forcing a partial
//         failure (mmap with length=0 → EINVAL on Linux) and then constructing
//         a fresh writer on the same names: if the destructor of the failed
//         writer corrupted state, this re-init would misbehave.
// -------------------------------------------------------------------------
static void test_init_partial_failure_safe_destruction() {
    {
        // size=0 → ftruncate to 0 succeeds, mmap with length=0 fails (EINVAL),
        // exercising the partial-init cleanup path.
        ShmemWriter w(SHM, 0, SEM, SEM_ACK);
        bool ok = w.initialize();
        CHECK("partial-init: initialize() returns false on mmap failure", !ok);
        // Destructor runs here. With the fix, sentinels were reset on the
        // error path, so cleanup() is a true no-op. Without the fix, we'd
        // double-close fd_ and double-munmap a stale ptr_.
    }
    // If the partial-init destructor misbehaved (e.g. closed an unrelated fd
    // that got recycled), the next ShmemWriter on the same names should still
    // come up cleanly.
    ShmemWriter w2(SHM, SZ, SEM, SEM_ACK);
    bool reinit = w2.initialize();
    CHECK("partial-init: fresh ShmemWriter on same names still initializes",
          reinit);
}

// -------------------------------------------------------------------------
// Test 8: successful round-trip – write produces a signal the consumer reads
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
    bool write_ok = w.write_data(data, 1, dims, 42);
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

    test_init_idempotent();
    std::cout << std::endl;

    test_init_partial_failure_safe_destruction();
    std::cout << std::endl;

    test_write_roundtrip();
    std::cout << std::endl;

    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===" << std::endl;
    return failed > 0 ? 1 : 0;
}
