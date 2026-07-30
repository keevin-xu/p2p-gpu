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

# find_package(Flatbuffers CONFIG REQUIRED)
# find_package(spdlog      CONFIG REQUIRED)
# find_package(CLI11       CONFIG REQUIRED)
# find_package(unofficial-sqlite3 CONFIG REQUIRED)
# find_package(stduuid     CONFIG REQUIRED)

# Native-only deps
# if(NOT P2PGPU_WASM)
#     find_package(LibDataChannel CONFIG REQUIRED)
#     find_package(unofficial-uwebsockets CONFIG REQUIRED)
# else()
#     # datachannel-wasm: FetchContent. Exposes the same API as libdatachannel,
#     # which is what lets src/worker-core/transport.cpp stay portable (R2).
# endif()

# ── WebGPU ───────────────────────────────────────────────────────────────
# Native: wgpu-native, prebuilt. Not a vcpkg port, so it carries its own pin
# (D-0016) rather than riding the vcpkg baseline.
# Browser: the emdawnwebgpu port, added to worker-core further down (D-0014).
#
# These two implement the SAME webgpu.h revision — 199/202 shared functions and
# all 86 shared structs field-for-field identical, measured in D-0016. That is
# what lets worker-core call webgpu.h directly and stay portable (R2), instead
# of needing the platform seam widened to cover the whole compute API.
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
)
target_include_directories(p2pgpu-protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated      # flatc output lands here
)
p2pgpu_harden(p2pgpu-protocol)

# Phase 1 step 1.1: wire up flatc.
#   flatbuffers_generate_headers(TARGET p2pgpu-schema
#       SCHEMAS protocol/p2pgpu.fbs
#       INCLUDE_PREFIX p2pgpu)
#   target_link_libraries(p2pgpu-protocol PUBLIC p2pgpu-schema flatbuffers::flatbuffers)

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
    src/worker-core/task_loop.cpp
    src/worker-core/kernel_host.cpp
    src/worker-core/accumulator.cpp
    src/worker-core/transport.cpp          # portable: libdatachannel API on both
)
target_include_directories(p2pgpu-worker-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(p2pgpu-worker-core PUBLIC p2pgpu-protocol)

if(P2PGPU_WASM)
    target_sources(p2pgpu-worker-core PRIVATE
        src/worker-core/platform/gpu_wasm.cpp
        src/worker-core/platform/timer_wasm.cpp)
else()
    target_sources(p2pgpu-worker-core PRIVATE
        src/worker-core/platform/gpu_native.cpp
        src/worker-core/platform/timer_native.cpp)
    target_link_libraries(p2pgpu-worker-core PUBLIC wgpu-native)
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

    add_executable(coordinator
        src/coordinator/main.cpp
        src/coordinator/job.cpp
        src/coordinator/queue.cpp
        src/coordinator/lease.cpp
        src/coordinator/sizer.cpp
        src/coordinator/validator.cpp
        src/coordinator/signaling.cpp
        src/coordinator/metrics.cpp
        src/coordinator/store.cpp
        src/coordinator/net.cpp
        src/coordinator/kernels.cpp
    )
    target_link_libraries(coordinator PRIVATE p2pgpu-protocol)
    p2pgpu_harden(coordinator)

    # Thin main() over worker-core. If this grows past ~100 lines,
    # logic is leaking out of worker-core (R1).
    add_executable(worker-native src/worker-native/main.cpp)
    target_link_libraries(worker-native PRIVATE p2pgpu-worker-core)
    # Dev convenience so the binary finds kernels/ without an argument. The
    # deployed worker gets its WGSL from the coordinator, not from a build path.
    target_compile_definitions(worker-native PRIVATE
        P2PGPU_KERNEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/kernels")
    p2pgpu_harden(worker-native)

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
    set_target_properties(worker-browser PROPERTIES OUTPUT_NAME "p2pgpu-worker" SUFFIX ".js")

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
