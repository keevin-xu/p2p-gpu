# Test suite. Tiers T1–T3 and T5 per docs/CONVENTIONS.md §7.
#
# Runs under the native-debug preset, which has ASan + UBSan on. A sanitizer
# finding is a test failure, not a warning.

# find_package(Catch2 3 REQUIRED)

add_executable(tests
    # T1 — unit: sizer math, reputation, tolerance comparison, state machine
    # unit/test_frame.cpp
    # unit/test_invariants.cpp
    # unit/test_task_state.cpp        exhaustive transitions, incl. every ILLEGAL one
    # unit/test_sizer.cpp
    # unit/test_reputation.cpp
    # unit/test_comparators.cpp

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
# target_link_libraries(tests PRIVATE Catch2::Catch2WithMain p2pgpu-protocol)
# include(Catch)
# catch_discover_tests(tests)

# T3 — kernel tests. Headless via worker-native, because CI has no browser.
# Golden · chunk-invariance · cross-implementation · limits (KERNELS.md §5).
# add_executable(kernel_tests
#     kernels/test_golden.cpp
#     kernels/test_chunk_invariance.cpp
#     kernels/test_limits.cpp
# )
# target_link_libraries(kernel_tests PRIVATE Catch2::Catch2WithMain p2pgpu-worker-core)

# Placeholder so the target exists before any tests are written.
target_sources(tests PRIVATE placeholder.cpp)

# Sanitizer self-check (step 0.4). OFF by default so the deliberate bugs can
# never ship. Turn on in a throwaway build dir to prove the sanitizers are
# genuinely instrumenting our code — see the header of placeholder.cpp.
option(P2PGPU_SELFCHECK "Compile the deliberate sanitizer self-check bugs" OFF)
if(P2PGPU_SELFCHECK)
    target_compile_definitions(tests PRIVATE P2PGPU_SELFCHECK)
    message(WARNING "p2pgpu: P2PGPU_SELFCHECK is ON — `tests` contains deliberate bugs")
endif()
