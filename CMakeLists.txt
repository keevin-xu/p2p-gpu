cmake_minimum_required(VERSION 3.25)
project(p2pgpu LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clang-tidy and editors

option(P2PGPU_BUILD_TESTS "Build the test suite" ON)
option(P2PGPU_BUILD_FUZZ  "Build libFuzzer harnesses (needs real libFuzzer — NOT Apple Clang)" OFF)

# D-0015: the fuzz preset uses Homebrew LLVM because Apple Clang ships no
# libFuzzer runtime. That compiler brings its own libc++, while vcpkg builds
# ports with Apple Clang — so the fuzz build must link NOTHING compiled by
# vcpkg. This option restricts it to p2pgpu-protocol + the harnesses, whose
# only dependency (flatbuffers::Verifier) is header-only.
option(P2PGPU_FUZZ_ONLY   "Build only p2pgpu-protocol and the fuzz harnesses" OFF)

if(EMSCRIPTEN)
    set(P2PGPU_WASM ON)
    message(STATUS "p2pgpu: configuring for WebAssembly")
else()
    set(P2PGPU_WASM OFF)
endif()

include(cmake/Hardening.cmake)

# ─────────────────────────────────────────────────────────────────────────
# Dependencies
#
# Managed by vcpkg manifest mode (vcpkg.json) except where noted.
# Phase 0 step 0.2: uncomment as each is actually needed, and pin the exact
# version in vcpkg.json. Do NOT add anything not listed in
# docs/CONVENTIONS.md §10 without a DECISIONS.md entry (DL5).
# ─────────────────────────────────────────────────────────────────────────

# find_package(unofficial-sqlite3 CONFIG REQUIRED)   # step 2.19
# find_package(stduuid     CONFIG REQUIRED)

# NOTE: the coordinator's dependencies (spdlog, CLI11, uWebSockets) are found
# further down, inside the coordinator target block — NOT here. See the comment
# there; the placement is load-bearing for the fuzz preset.

# ── Transport (step 1.18) ────────────────────────────────────────────────
# libdatachannel natively, datachannel-wasm in the browser. They expose the SAME
# `rtc::WebSocket`, which is what lets src/worker-core/transport.cpp stay
# portable with no platform seam — the property D-0008's language choice rests
# on. The shared subset is MEASURED, not assumed: D-0032 tabulates it.
#
# datachannel-wasm has no vcpkg port, so like wgpu-native it carries its own pin
# rather than riding the vcpkg baseline (D-0013 / D-0016). Its CMakeLists
# FATAL_ERRORs outside Emscripten, so it may only be fetched in the wasm branch
# — declaring it unconditionally breaks every native preset.
if(NOT P2PGPU_WASM)
    find_package(LibDataChannel CONFIG REQUIRED)
endif()

# ── WebGPU ───────────────────────────────────────────────────────────────
# Native: wgpu-native, prebuilt. Not a vcpkg port, so it carries its own pin
# (D-0016) rather than riding the vcpkg baseline.
# Browser: the emdawnwebgpu port, added to worker-core further down (D-0014).
#
# These two implement the SAME webgpu.h revision — 199/202 shared functions and
# all 86 shared structs field-for-field identical, measured in D-0016. That is
# what lets worker-core call webgpu.h directly and stay portable (R2), instead
# of needing the platform seam widened to cover the whole compute API.
# BLAKE3 — invariant 9's checksum. Needed by BOTH the coordinator (verifies it)
# and worker-core (computes it), so it is found here rather than in the
# coordinator block. Still below the P2PGPU_FUZZ_ONLY early-return, because
# p2pgpu-protocol must link nothing vcpkg compiled (D-0015).
#
# The browser worker cannot use this port at all — it hardcodes x86 SIMD and
# does not build for wasm32-emscripten. It fetches upstream instead (D-0034).
if(NOT P2PGPU_WASM)
    find_package(BLAKE3 CONFIG REQUIRED)
endif()

if(NOT P2PGPU_WASM)
    include(FetchContent)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(P2PGPU_WGPU_ARCHIVE "wgpu-macos-aarch64-release.zip")
        set(P2PGPU_WGPU_SHA256  "a5797a37b1adf720bcd5dcffb291edbbd5b7b14be0a3874c28e6393a655a7a3e")
    else()
        # Add the platform's archive + hash from the v29.0.1.1 release page.
        message(FATAL_ERROR
            "p2pgpu: no wgpu-native archive pinned for this platform yet. "
            "Add it to CMakeLists.txt with its SHA256 (see D-0016).")
    endif()

    FetchContent_Declare(wgpu_native
        URL      "https://github.com/gfx-rs/wgpu-native/releases/download/v29.0.1.1/${P2PGPU_WGPU_ARCHIVE}"
        URL_HASH "SHA256=${P2PGPU_WGPU_SHA256}"
    )
    FetchContent_MakeAvailable(wgpu_native)

    # Prebuilt archive, so hand-roll the imported target. Static, to keep
    # worker-native a single self-contained binary — step 4.19 wants the
    # borrowed machine to join with one command and no dylib hunting.
    add_library(wgpu-native STATIC IMPORTED GLOBAL)
    set_target_properties(wgpu-native PROPERTIES
        IMPORTED_LOCATION             "${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.a"
        INTERFACE_INCLUDE_DIRECTORIES "${wgpu_native_SOURCE_DIR}/include")
    if(APPLE)
        # wgpu-native's Metal backend needs these at link time.
        target_link_libraries(wgpu-native INTERFACE
            "-framework Metal" "-framework QuartzCore" "-framework Foundation"
            "-framework IOKit" "-framework IOSurface")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────
# p2pgpu-protocol — framing, Verifier wrappers, invariants
#
# R3: protocol/p2pgpu.fbs is the single source of truth. flatc generates
# headers into the BUILD TREE at build time. Never committed, never edited.
# ─────────────────────────────────────────────────────────────────────────
add_library(p2pgpu-protocol STATIC
    src/protocol/frame.cpp
    src/protocol/verify.cpp
    src/protocol/invariants.cpp
    src/protocol/encode.cpp
)
target_include_directories(p2pgpu-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# SYSTEM: generated code and third-party headers are not ours to fix, and our
# warning set (-Wconversion et al) would otherwise fire on every build.
target_include_directories(p2pgpu-protocol SYSTEM PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}/generated      # flatc output lands here
)
p2pgpu_harden(p2pgpu-protocol)

# ─────────────────────────────────────────────────────────────────────────
# flatc — schema to C++ headers, as a build step (step 0.5, rule R3)
#
# Output goes into the BUILD TREE only. The schema is the sole versioned
# artifact; generated headers are never committed and never hand-edited, so
# they cannot drift from protocol/p2pgpu.fbs. Editing the schema and
# rebuilding is the ONLY way to change the wire format.
#
# NOTE we take the flatbuffers HEADERS but deliberately do NOT link
# flatbuffers::flatbuffers. That is a STATIC imported target, and libflatbuffers
# .a holds only the schema-parsing side (idl_parser, reflection, util) which we
# never use — `flatbuffers::Verifier` is header-only. Linking it would break
# D-0015: the fuzz preset builds p2pgpu-protocol with Homebrew LLVM while vcpkg
# compiled that archive with Apple Clang, and mixing the two libc++ ABIs is
# exactly the corruption D-0015 exists to prevent.
# ─────────────────────────────────────────────────────────────────────────
find_package(flatbuffers CONFIG REQUIRED)

set(P2PGPU_SCHEMA           ${CMAKE_CURRENT_SOURCE_DIR}/protocol/p2pgpu.fbs)
set(P2PGPU_GENERATED_DIR    ${CMAKE_CURRENT_BINARY_DIR}/generated/p2pgpu)
set(P2PGPU_GENERATED_HEADER ${P2PGPU_GENERATED_DIR}/p2pgpu_generated.h)

add_custom_command(
    OUTPUT  ${P2PGPU_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${P2PGPU_GENERATED_DIR}
    # --scoped-enums so generated enums are `enum class`, matching the
    # -Werror=switch discipline the task state machine relies on.
    COMMAND flatbuffers::flatc --cpp --scoped-enums
            -o ${P2PGPU_GENERATED_DIR} ${P2PGPU_SCHEMA}
    DEPENDS ${P2PGPU_SCHEMA} flatbuffers::flatc
    COMMENT "flatc: protocol/p2pgpu.fbs -> build tree"
    VERBATIM)

add_custom_target(p2pgpu-schema DEPENDS ${P2PGPU_GENERATED_HEADER})
add_dependencies(p2pgpu-protocol p2pgpu-schema)

# Headers only — see the note above on why the library is not linked.
target_include_directories(p2pgpu-protocol SYSTEM PUBLIC
    $<TARGET_PROPERTY:flatbuffers::flatbuffers,INTERFACE_INCLUDE_DIRECTORIES>)

if(P2PGPU_FUZZ_ONLY)
    # Everything below this point is deliberately skipped — see D-0015.
    add_subdirectory(fuzz)
    return()
endif()

# ─────────────────────────────────────────────────────────────────────────
# p2pgpu-worker-core — THE dual-target library (R2)
#
# Identical source compiled for native and WASM. Platform differences live
# ONLY in src/worker-core/platform/. A #ifdef __EMSCRIPTEN__ anywhere else
# in this target is a defect.
# ─────────────────────────────────────────────────────────────────────────
add_library(p2pgpu-worker-core STATIC
    src/worker-core/task_loop.cpp         # 1.20 lease->execute->report; 1.21 loss
    src/worker-core/kernel_host.cpp
    src/worker-core/accumulator.cpp
    src/worker-core/smoke.cpp              # portable: shared 0.6/0.8/0.9 checks
    src/worker-core/gpu_info.cpp           # portable: adapter info, features, limits
    src/worker-core/bench.cpp              # portable: throughput + overhead (0.10-0.12)
    src/worker-core/recovery.cpp           # portable: device-loss recovery (0.14)
    src/worker-core/transport.cpp          # portable: libdatachannel API on both
    src/worker-core/checksum.cpp           # portable: BLAKE3-64, invariant 9
)
target_include_directories(p2pgpu-worker-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(p2pgpu-worker-core PUBLIC p2pgpu-protocol)

if(P2PGPU_WASM)
    target_sources(p2pgpu-worker-core PRIVATE
        src/worker-core/platform/gpu_wasm.cpp
        src/worker-core/platform/timer_wasm.cpp)

    # datachannel-wasm — the browser twin of libdatachannel (D-0032). Pinned by
    # SHA, not by tag: a tag can be moved, and "reproducible" has to mean the
    # bytes, not the name.
    # BLAKE3 for the browser worker. vcpkg's port hardcodes x86 SIMD and does
    # NOT build for wasm32-emscripten — measured, not assumed (D-0034). Native
    # keeps the vcpkg port; only this target fetches upstream, with the SIMD
    # path forced to the portable one.
    include(FetchContent)
    set(BLAKE3_SIMD_TYPE "none" CACHE STRING "" FORCE)
    FetchContent_Declare(blake3_upstream
        GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
        GIT_TAG        df610ddc3b93841ffc59a87e3da659a15910eb46  # 1.8.2
        SOURCE_SUBDIR  c
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(blake3_upstream)
    target_link_libraries(p2pgpu-worker-core PUBLIC blake3)

    FetchContent_Declare(datachannel_wasm
        GIT_REPOSITORY https://github.com/paullouisageneau/datachannel-wasm.git
        GIT_TAG        3d88eb88fb7d6729e498079477dd3ae46d30ac38  # v0.4.0, 2025-09-13
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(datachannel_wasm)
    target_link_libraries(p2pgpu-worker-core PUBLIC datachannel-wasm)
else()
    target_sources(p2pgpu-worker-core PRIVATE
        src/worker-core/platform/gpu_native.cpp
        src/worker-core/platform/timer_native.cpp)
    target_link_libraries(p2pgpu-worker-core PUBLIC
        wgpu-native LibDataChannel::LibDataChannel BLAKE3::blake3)
endif()

# NOTE: worker-core deliberately does NOT know where kernels live. Native reads
# them from disk, the browser fetches them over HTTP from the coordinator (step
# 1.12) — so the WGSL text is passed IN. Keeping acquisition out of worker-core
# is what stops a `#ifdef __EMSCRIPTEN__` appearing in portable code (R2).
p2pgpu_harden(p2pgpu-worker-core)

# ─────────────────────────────────────────────────────────────────────────
# Executables
# ─────────────────────────────────────────────────────────────────────────
if(NOT P2PGPU_WASM)

    # Coordinator dependencies (steps 1.11-1.12).
    #
    # Declared HERE rather than beside the other find_package calls at the top,
    # and the placement is load-bearing: `uwebsockets` lives in the `native`
    # vcpkg feature, which the fuzz preset deliberately does not enable
    # (D-0015). A find_package above the P2PGPU_FUZZ_ONLY early-return breaks
    # that preset outright — which it did, once. Anything only the coordinator
    # needs belongs below the return.
    find_package(spdlog CONFIG REQUIRED)
    find_package(CLI11  CONFIG REQUIRED)
    find_package(unofficial-uwebsockets CONFIG REQUIRED)

    # The coordinator's DECISION logic, split out from its transport so the
    # tests can link it. Everything here is socket-free by construction — if a
    # file in this list ever needs uWebSockets, it belongs on the other side of
    # the line (session.hpp explains why that boundary is worth keeping).
    add_library(p2pgpu-coordinator-core STATIC
        src/coordinator/job.cpp
        src/coordinator/session.cpp
        src/coordinator/kernels.cpp
        src/coordinator/params.cpp
        src/coordinator/queue.cpp
        src/coordinator/lease.cpp
        src/coordinator/sizer.cpp
        src/coordinator/validator.cpp
        src/coordinator/signaling.cpp
        src/coordinator/metrics.cpp
        src/coordinator/store.cpp
    )
    target_link_libraries(p2pgpu-coordinator-core PUBLIC
        p2pgpu-protocol
        spdlog::spdlog
        BLAKE3::blake3)
    p2pgpu_harden(p2pgpu-coordinator-core)

    add_executable(coordinator
        src/coordinator/main.cpp
        src/coordinator/net.cpp
    )
    target_link_libraries(coordinator PRIVATE
        p2pgpu-coordinator-core
        CLI11::CLI11
        unofficial::uwebsockets::uwebsockets)
    p2pgpu_harden(coordinator)

    # Thin main() over worker-core. If this grows past ~100 lines,
    # logic is leaking out of worker-core (R1).
    add_executable(worker-native src/worker-native/main.cpp)
    target_link_libraries(worker-native PRIVATE p2pgpu-worker-core CLI11::CLI11)
    # Dev convenience so the binary finds kernels/ without an argument. The
    # deployed worker gets its WGSL from the coordinator, not from a build path.
    target_compile_definitions(worker-native PRIVATE
        P2PGPU_KERNEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/kernels")
    p2pgpu_harden(worker-native)

    # Corpus generator for fuzz_protocol (step 1.5). Lives here rather than in
    # fuzz/ because that subdirectory is only added for the fuzz preset, and the
    # seeds must be reproducible from an ordinary build.
    add_executable(make_corpus fuzz/make_corpus.cpp)
    target_link_libraries(make_corpus PRIVATE p2pgpu-protocol)
    p2pgpu_harden(make_corpus)

    add_executable(mock-worker
        src/mock-worker/main.cpp
        src/mock-worker/virtual_worker.cpp
        src/mock-worker/behaviors.cpp
        src/mock-worker/profiles.cpp
    )
    target_link_libraries(mock-worker PRIVATE p2pgpu-protocol)
    p2pgpu_harden(mock-worker)

else()

    # Browser worker. The only JavaScript in the project is web/ui.js, reached
    # from ui_bridge.cpp via EM_JS — DOM glue for the R7 opt-in surface, no logic.
    add_executable(worker-browser
        src/worker-browser/main.cpp
        src/worker-browser/ui_bridge.cpp
    )
    target_link_libraries(worker-browser PRIVATE p2pgpu-worker-core)
    # Emit into build/wasm/web alongside the static assets, so the whole
    # directory is what gets served (and later deployed).
    set_target_properties(worker-browser PROPERTIES
        OUTPUT_NAME "p2pgpu-worker"
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web")

    # Copy the static R7 opt-in surface next to the module. No build step, no
    # bundler — web/ is plain files by design (ARCHITECTURE.md §4).
    add_custom_command(TARGET worker-browser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
                "${CMAKE_CURRENT_SOURCE_DIR}/web/ui.js"
                "${CMAKE_BINARY_DIR}/web/"
        COMMENT "Copying web/ static assets")

    # WebGPU in the browser (D-0014). -sUSE_WEBGPU=1 was REMOVED in Emscripten
    # 6.0.5 — it is a hard configure error, not a warning. The replacement is
    # Dawn's emdawnwebgpu port, which implements a NEWER AND INCOMPATIBLE
    # revision of webgpu.h. Whether wgpu-native matches that revision is the
    # open question step 0.8 exists to answer; if it does not, the platform
    # seam widens to cover the difference (pre-authorized in D-0014).
    # The port must be on BOTH compile and link lines to get headers + JS glue.
    target_compile_options(p2pgpu-worker-core PUBLIC --use-port=emdawnwebgpu)
    target_link_options(worker-browser PRIVATE
        --bind
        --use-port=emdawnwebgpu
        # Embed the WGSL so the browser reads the SAME BYTES as worker-native,
        # which is what makes step 0.9's cross-target comparison mean anything.
        # Step 1.12 replaces this with an HTTP fetch from the coordinator.
        # SHELL: is required, not decoration. CMake de-duplicates repeated link
        # options, so a bare second `--embed-file` gets dropped and its path is
        # left orphaned — emcc then reports it as a missing input file. SHELL:
        # keeps each flag glued to its own argument.
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/kernels/smoke_double.wgsl@/kernels/smoke_double.wgsl"
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/kernels/smoke_hash.wgsl@/kernels/smoke_hash.wgsl"
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/kernels/calibrate.wgsl@/kernels/calibrate.wgsl"
        "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/kernels/divergence_probe.wgsl@/kernels/divergence_probe.wgsl"
        # p2pgpu_run_smoke_test is called from ui.js on the R7 opt-in click.
        -sEXPORTED_FUNCTIONS=_main,_p2pgpu_run_smoke_test,_p2pgpu_run_chunking,_p2pgpu_run_bench,_p2pgpu_report
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
        # ASYNCIFY yields between dispatches (R4) without blocking. NOTE its
        # tension with step 1.24's -sPROXY_TO_PTHREAD: with the task loop on its
        # own thread, blocking is fine and ASYNCIFY's size/speed cost buys
        # nothing. Revisit — and log it — when 1.24 lands.
        -sASYNCIFY
        -sALLOW_MEMORY_GROWTH=1
        -sEXIT_RUNTIME=0
        -sMODULARIZE=1
        -sEXPORT_NAME=createP2pgpuWorker
        # Phase 1 step 1.24: move the task loop off the browser main thread.
        #   -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=2
        # Requires COOP/COEP headers in dev and prod (docs/RISKS.md §1).
    )

endif()

# ─────────────────────────────────────────────────────────────────────────
# Tests and fuzzing
# ─────────────────────────────────────────────────────────────────────────
if(P2PGPU_BUILD_TESTS AND NOT P2PGPU_WASM)
    enable_testing()
    add_subdirectory(tests)
endif()

if(P2PGPU_BUILD_FUZZ AND NOT P2PGPU_WASM)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_subdirectory(fuzz)
    else()
        message(WARNING "p2pgpu: P2PGPU_BUILD_FUZZ needs Clang; skipping fuzz targets")
    endif()
endif()
