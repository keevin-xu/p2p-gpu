# libFuzzer harnesses. Clang only; built when P2PGPU_BUILD_FUZZ=ON.
#
# These are not optional extras — they are the evidence behind rule R11 and
# decision D-0010, and they get reported in EVALUATION.md E8.
#
# Run:
#   ./build/native-fuzz/fuzz_protocol fuzz/corpus/protocol -max_total_time=60
#
# ── STANDING CONSTRAINT (D-0015) ─────────────────────────────────────────
# This preset compiles with Homebrew LLVM (Apple Clang has no libFuzzer
# runtime), which uses its own libc++ — while vcpkg builds ports with Apple
# Clang. So p2pgpu-protocol MUST NOT link any compiled vcpkg library, or every
# std:: type crossing that boundary becomes an ABI mismatch.
#
# In particular: do not link spdlog into p2pgpu-protocol. Logging a rejected
# frame is the CALLER's job (ARCHITECTURE.md §4 — the protocol layer has "no
# I/O, no policy"). flatbuffers::Verifier is header-only, which is what keeps
# this build honest.
#
# Any crash found: minimize it, fix the bug, and COMMIT THE INPUT to the
# corpus as a regression seed (docs/CONVENTIONS.md §7 T4).

function(p2pgpu_add_fuzzer name source)
    add_executable(${name} ${source})
    # Land next to the other binaries, so the CLAUDE.md commands table is true.
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    target_link_libraries(${name} PRIVATE p2pgpu-protocol)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${name}    PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

# Phase 1 step 1.5 — the frame parser. Do this before building anything on top
# of the protocol, not after.
p2pgpu_add_fuzzer(fuzz_protocol fuzz_protocol.cpp)

# Phase 4 step 4.13 — asset chunk reassembly. Needed BEFORE Phase 6 makes that
# path live, since peer-supplied bytes are the least trusted input in the system.
# p2pgpu_add_fuzzer(fuzz_asset fuzz_asset.cpp)
