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
    unit/test_session.cpp          # 1.15-1.16 — handshake + result ingestion

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
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain p2pgpu-coordinator-core)
p2pgpu_harden(tests)

include(Catch)
catch_discover_tests(tests)

# T3 — kernel tests. Headless via worker-native, because CI has no browser.
# Golden · chunk-invariance · cross-implementation · limits (KERNELS.md §5).
# add_executable(kernel_tests
#     kernels/test_golden.cpp
#     kernels/test_chunk_invariance.cpp
#     kernels/test_limits.cpp
# )
# target_link_libraries(kernel_tests PRIVATE Catch2::Catch2WithMain p2pgpu-worker-core)

# Sanitizer self-check (step 0.4). OFF by default so the deliberate bugs can
# never ship. Turn on in a throwaway build dir to prove the sanitizers are
# genuinely instrumenting our code — see the header of selfcheck.cpp.
option(P2PGPU_SELFCHECK "Compile the deliberate sanitizer self-check bugs" OFF)
if(P2PGPU_SELFCHECK)
    add_executable(selfcheck placeholder.cpp)
    target_compile_definitions(selfcheck PRIVATE P2PGPU_SELFCHECK)
    message(WARNING "p2pgpu: P2PGPU_SELFCHECK is ON — `selfcheck` contains deliberate bugs")
endif()
