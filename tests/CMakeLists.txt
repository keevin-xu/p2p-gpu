# Test suite. Tiers T1–T3 and T5 per docs/CONVENTIONS.md §7.
#
# Runs under the native-debug preset, which has ASan + UBSan on. A sanitizer
# finding is a test failure, not a warning.

find_package(Catch2 3 REQUIRED)

add_executable(tests
    # T1 — unit: pure logic, no I/O.
    unit/test_frame.cpp            # 1.2 — frame header codec + the §1 sequence
    unit/test_ids.cpp              # 1.6 — strong IDs; mostly static_assert by design
    unit/test_invariants.cpp       # 1.4 — all ten, every failure case
    unit/test_verify.cpp           # 1.3 — the sanctioned bytes->typed path

    unit/test_task_state.cpp       # 1.13 — all 54 pairs, incl. every ILLEGAL one
    unit/test_job.cpp              # 1.14 — job/task queue + lease bookkeeping
    unit/test_leases.cpp           # 2.6-2.10 — renewal, expiry, loss, drain
    unit/test_store.cpp            # 2.19/2.20 — durable state + crash recovery
    unit/test_metrics.cpp          # 2.21 — derived metrics + SSE payload
    unit/test_validator.cpp        # 3.1-3.3 — determinism-class comparators
    unit/test_quorum.cpp           # 3.4-3.6 — replication policy + quorum
    unit/test_reputation.cpp       # 3.7-3.11 — reputation, adaptive, fault classes
    unit/test_spot_check.cpp       # 3.9 — known-answer injection
    unit/test_sizer.cpp            # 2.12-2.14 — sizing rules, in order
    unit/test_session.cpp          # 1.15-1.16 — handshake + result ingestion
    unit/test_checksum.cpp         # 1.20 — invariant 9, pinned to BLAKE3's vectors
    unit/test_reference.cpp        # 1.25 — the CPU ground truth, no GPU needed

    # Still to come:
    # unit/test_sizer.cpp          # 2.12
    # unit/test_reputation.cpp     # 3.7
    # unit/test_comparators.cpp    # 3.1

    # T2 — integration: coordinator + N mock workers, in-process.
    # The workhorse tier. Every failure path in ARCHITECTURE.md §6 lives here.
    # integration/test_lease_expiry.cpp
    # integration/test_device_loss.cpp
    # integration/test_speculation.cpp
    # integration/test_restart_recovery.cpp
    # integration/test_duplicate_submit.cpp
    # integration/test_lease_not_held.cpp
    # integration/test_malformed_frame.cpp
)
# p2pgpu-coordinator-core, not the `coordinator` executable: the decision logic
# is deliberately transport-free (session.hpp), so these tests need no socket.
#
# worker-core is here for ONE reason — test_checksum.cpp asserts that the
# worker's BLAKE3 and the coordinator's produce the same number (D-0034). It
# links GPU and transport libraries but calls neither; nothing in this binary
# touches a device, which is what keeps `tests` runnable on a machine with no
# GPU. GPU tests live in `kernel_tests`.
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain
    p2pgpu-coordinator-core p2pgpu-worker-core)
# test_session.cpp loads the REAL kernel manifest — granting a task now needs
# the job's kernel to be in the registry, so a stub registry would exercise a
# path production never takes.
target_compile_definitions(tests PRIVATE
    P2PGPU_KERNEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../kernels")
p2pgpu_harden(tests)

include(Catch)
catch_discover_tests(tests)

# R2 as a TEST rather than a review note (step 1.17). Nothing breaks the day a
# portable file grows an `#ifdef __EMSCRIPTEN__` — it compiles and runs, and the
# two targets quietly stop being the same program. A rule enforced by
# remembering to look is a rule that holds until the session that forgets.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME r2_platform_seam
         COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_seam.py)

# NO protocol-doc test here, deliberately. `tools/check_protocol_doc.py` reads
# docs/PROTOCOL.md, and **`*.md` is gitignored in this repo** — so the file does
# not exist on a fresh clone and the test failed in CI with FileNotFoundError
# while passing locally.
#
# STANDING RULE: a ctest may only read files that are actually committed.
# Anything under docs/, or any other .md, is local-only. Run
# `python3 tools/check_protocol_doc.py` by hand before a gate instead.

# T3 — kernel tests on a REAL GPU. Headless via worker-core, because CI has no
# browser. Golden · chunk-invariance · cross-implementation · limits
# (KERNELS.md §5).
#
# A SEPARATE BINARY from `tests` on purpose: these need a working adapter, and a
# unit suite that silently skips itself on a machine without one is worse than
# no suite. Keeping them apart means `tests` stays a pure T1 tier that runs
# anywhere, and a missing GPU fails loudly here instead of quietly there.
add_executable(kernel_tests
    kernels/test_brute_search.cpp        # 1.19 — chunk invariance + the D-0033 window
)
target_link_libraries(kernel_tests PRIVATE Catch2::Catch2WithMain p2pgpu-worker-core)
target_compile_definitions(kernel_tests PRIVATE
    P2PGPU_KERNEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../kernels")
p2pgpu_harden(kernel_tests)
catch_discover_tests(kernel_tests TEST_PREFIX "kernel_")

# Sanitizer self-check (step 0.4). OFF by default so the deliberate bugs can
# never ship. Turn on in a throwaway build dir to prove the sanitizers are
# genuinely instrumenting our code — see the header of selfcheck.cpp.
option(P2PGPU_SELFCHECK "Compile the deliberate sanitizer self-check bugs" OFF)
if(P2PGPU_SELFCHECK)
    add_executable(selfcheck placeholder.cpp)
    target_compile_definitions(selfcheck PRIVATE P2PGPU_SELFCHECK)
    message(WARNING "p2pgpu: P2PGPU_SELFCHECK is ON — `selfcheck` contains deliberate bugs")
endif()
