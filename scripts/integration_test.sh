#!/usr/bin/env bash
# Integration test: C++ source -> Python destination via POSIX shared memory.
#
# Builds both containers, runs them for a configurable duration, then validates:
#   - both containers completed at least MIN_ITERATIONS handoffs
#   - the transferred array shape matches the configured dimensions
#   - all reported Min/Max values are within the expected [-1, 1] range
#
# Usage:  ./scripts/integration_test.sh [RUN_SECONDS]
#   RUN_SECONDS  seconds to let the containers run (default: 20)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

# ── configuration ────────────────────────────────────────────────────────────
RUN_SECONDS="${1:-20}"
MIN_ITERATIONS=5          # minimum successful iterations to consider the test passing
DOCKER_API_VERSION="${DOCKER_API_VERSION:-1.43}"
export DOCKER_API_VERSION

# ── helpers ──────────────────────────────────────────────────────────────────
PASS=0
FAIL=0

pass() { echo "[PASS] $*"; PASS=$(( PASS + 1 )); }
fail() { echo "[FAIL] $*"; FAIL=$(( FAIL + 1 )); }

# cond is truthy when non-empty and not "0" (grep -c returns "0" on no match)
check() {
    local label="$1"
    local cond="$2"
    if [[ -n "${cond}" && "${cond}" != "0" ]]; then
        pass "${label}"
    else
        fail "${label}"
    fi
}

cleanup() {
    echo ""
    echo "── Tearing down containers ──────────────────────────────────────────"
    docker compose down 2>&1 || true
}
trap cleanup EXIT

# ── step 1: clean stale IPC objects ─────────────────────────────────────────
echo "── Cleaning stale IPC objects ───────────────────────────────────────"
docker run --rm --ipc=host ubuntu:22.04 \
    rm -f /dev/shm/sem.haidis_sem /dev/shm/sem.haidis_sem_ack /dev/shm/haidis_shmem
echo "   done."
echo ""

# ── step 2: build ────────────────────────────────────────────────────────────
echo "── Building containers ──────────────────────────────────────────────"
docker compose build
echo ""

# ── step 3: start detached ───────────────────────────────────────────────────
echo "── Starting containers (detached) ───────────────────────────────────"
docker compose up -d
echo "   running for ${RUN_SECONDS}s ..."
sleep "${RUN_SECONDS}"
echo ""

# ── step 4: capture logs ─────────────────────────────────────────────────────
echo "── Capturing logs ───────────────────────────────────────────────────"
SRC_LOG="$(docker logs haidis-source  2>&1)"
DST_LOG="$(docker logs haidis-destination 2>&1)"

echo "--- Source (last 5 lines) ---"
echo "${SRC_LOG}" | tail -5
echo ""
echo "--- Destination (last 5 lines) ---"
echo "${DST_LOG}" | tail -5
echo ""

# ── step 5: validate ─────────────────────────────────────────────────────────
echo "── Validation ───────────────────────────────────────────────────────"

# 5a. Initialization
check "source:    shmem + semaphores initialized" \
    "$(echo "${SRC_LOG}" | grep -c 'Shared memory initialized')"

check "destination: shmem + semaphores opened" \
    "$(echo "${DST_LOG}" | grep -c 'Shared memory opened')"

# 5b. Iteration counts
SRC_ITERS="$(echo "${SRC_LOG}" | grep -c '^Iteration' || true)"
DST_ITERS="$(echo "${DST_LOG}" | grep -c 'Iteration' || true)"
echo "   Source iterations:      ${SRC_ITERS}"
echo "   Destination iterations: ${DST_ITERS}"

[[ "${SRC_ITERS}" -ge "${MIN_ITERATIONS}" ]] \
    && pass "source:      >= ${MIN_ITERATIONS} iterations written (${SRC_ITERS})" \
    || fail "source:      only ${SRC_ITERS} iterations written (need ${MIN_ITERATIONS})"

[[ "${DST_ITERS}" -ge "${MIN_ITERATIONS}" ]] \
    && pass "destination: >= ${MIN_ITERATIONS} iterations read (${DST_ITERS})" \
    || fail "destination: only ${DST_ITERS} iterations read (need ${MIN_ITERATIONS})"

# 5c. Array shape - destination must report the shape from config.env
ARRAY_SIZE="$(grep ARRAY_SIZE shared/config.env | cut -d= -f2)"
EXPECTED_SHAPE="(${ARRAY_SIZE}, 3)"
check "destination: array shape is ${EXPECTED_SHAPE}" \
    "$(echo "${DST_LOG}" | grep -F "Read array ${EXPECTED_SHAPE}")"

# 5d. Value range - source writes uniform_real_distribution(-1, 1)
#     every "Min:" line from destination must be >= -1 and every "Max:" <= 1
BAD_RANGE="$(echo "${DST_LOG}" | grep 'Min:' | awk '
    {
        for (i=1;i<=NF;i++) {
            if ($i=="Min:") min=$(i+1)+0
            if ($i=="Max:") max=$(i+1)+0
        }
        if (min < -1.0001 || max > 1.0001) print "out of range: min=" min " max=" max
    }
')"
[[ -z "${BAD_RANGE}" ]] \
    && pass "destination: all Min/Max values within [-1, 1]" \
    || fail "destination: out-of-range values detected: ${BAD_RANGE}"

# 5e. No errors on either side
SRC_ERRORS="$(echo "${SRC_LOG}"  | grep -i 'error\|failed\|fatal' || true)"
DST_ERRORS="$(echo "${DST_LOG}"  | grep -i 'error\|failed\|fatal' || true)"
[[ -z "${SRC_ERRORS}" ]] \
    && pass "source:      no error messages in log" \
    || fail "source:      error messages found: ${SRC_ERRORS}"

[[ -z "${DST_ERRORS}" ]] \
    && pass "destination: no error messages in log" \
    || fail "destination: error messages found: ${DST_ERRORS}"

# ── results ──────────────────────────────────────────────────────────────────
echo ""
echo "── Results: ${PASS} passed, ${FAIL} failed ──────────────────────────────────────"
[[ "${FAIL}" -eq 0 ]]   # exit 0 on all-pass, non-zero otherwise
