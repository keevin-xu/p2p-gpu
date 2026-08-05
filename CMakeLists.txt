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

# NOTE: sqlite3 is NOT looked up here. It lives in the vcpkg `native` feature,
# which the fuzz presets disable, so its find_package must sit BELOW the
# P2PGPU_FUZZ_ONLY early-return with the other native deps (the 1.29 CI break).
# find_package(stduuid     CONFIG REQUIRED)

# NOTE: the coordinator's dependencies (spdlog, CLI11, uWebSockets) are found
# further down, inside the coordinator target block — NOT here. See the comment
# there; the placement is load-bearing for the fuzz preset.

# NOTE — libdatachannel and BLAKE3 are found FURTHER DOWN, below the
# P2PGPU_FUZZ_ONLY early-return, alongside the coordinator's dependencies. Both
# live in the vcpkg `native` feature, which the fuzz presets disable. See the
# comment at that point; the placement is load-bearing and has broken the fuzz
# build twice.

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
    elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Added for CI (step 1.29 / D-0036). Note this only makes the build
        # CONFIGURE and LINK on Linux — CI runners have no GPU, so nothing that
        # acquires a device runs there. It also unblocks step 4.5's run on
        # non-Apple hardware, which does have one.
        set(P2PGPU_WGPU_ARCHIVE "wgpu-linux-x86_64-release.zip")
        set(P2PGPU_WGPU_SHA256  "95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a")
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
# vcpkg `native`-feature dependencies. EVERY ONE OF THEM BELONGS HERE.
#
# ┌─ THIS HAS BROKEN THE FUZZ BUILD TWICE. READ BEFORE ADDING ONE. ────────┐
# │ The fuzz presets set VCPKG_MANIFEST_FEATURES="" (D-0015), so NOTHING   │
# │ in the `native` feature is installed for them. A find_package for one  │
# │ of those, placed above the early-return, fails the fuzz configure      │
# │ outright — even though the fuzz build never uses the library.          │
# │                                                                       │
# │ It broke once with uwebsockets, and again with libdatachannel and      │
# │ blake3 at step 1.18/1.20 — where a comment above the return literally  │
# │ claimed to be below it. **A warm build directory hides this**: the     │
# │ local preset kept configuring from a cached LibDataChannel_DIR, and    │
# │ only CI's clean checkout surfaced it (D-0036). If you touch this,      │
# │ verify in a FRESH build dir, not the one you have been using.          │
# └───────────────────────────────────────────────────────────────────────┘
#
# The rule is NOT "coordinator-only deps go here" — libdatachannel is a
# worker-core dep and still belongs here. The rule is: **anything from the
# vcpkg `native` feature.**
if(NOT P2PGPU_WASM)
    # Transport, step 1.18. libdatachannel natively, datachannel-wasm in the
    # browser — they expose the SAME `rtc::WebSocket`, which is what lets
    # src/worker-core/transport.cpp stay portable with no platform seam. That
    # is the property D-0008's language choice rests on, and D-0032 tabulates
    # the shared subset rather than assuming it.
    find_package(LibDataChannel CONFIG REQUIRED)

    # BLAKE3 — invariant 9's checksum. Needed by BOTH the coordinator (which
    # verifies it) and worker-core (which computes it), so it cannot live in
    # the coordinator block below.
    #
    # The browser worker cannot use this port at all: it hardcodes x86 SIMD and
    # does not build for wasm32-emscripten, so wasm fetches upstream (D-0034).
    find_package(BLAKE3 CONFIG REQUIRED)
endif()

# ─────────────────────────────────────────────────────────────────────────
# p2pgpu-transport — the WebSocket control plane, GPU-free (D-0042)
#
# Split out of worker-core so `mock-worker` can be a full protocol client with
# NO GPU runtime linked in — the property that lets 200 virtual workers share
# one process. worker-core links wgpu-native; this deliberately links nothing
# but libdatachannel (or datachannel-wasm) and the protocol.
#
# One WebSocket wrapper, two consumers. The alternative was the same ~90 lines
# twice, and the second copy is the one that stops getting fixed.
# ─────────────────────────────────────────────────────────────────────────
add_library(p2pgpu-transport STATIC src/transport/transport.cpp)
target_include_directories(p2pgpu-transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(p2pgpu-transport PUBLIC p2pgpu-protocol)
if(NOT P2PGPU_WASM)
    target_link_libraries(p2pgpu-transport PUBLIC LibDataChannel::LibDataChannel)
endif()
p2pgpu_harden(p2pgpu-transport)

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
    src/worker-core/checksum.cpp           # portable: BLAKE3-64, invariant 9
)
target_include_directories(p2pgpu-worker-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(p2pgpu-worker-core PUBLIC p2pgpu-protocol p2pgpu-transport)

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
        GIT_SHALLOW    FALSE
        # Two `window.X` feature guards prevent the library working on a Web
        # Worker, which is where the task loop lives as of step 1.24 (D-0038).
        # A Worker has no `window`, so the WebSocket throws before it is ever
        # created — although `new WebSocket(url)` right below the guard would
        # have worked fine. Two words, patched to `globalThis.`.
        #
        # RE-CHECK THIS ON EVERY PIN BUMP. The patch script fails loudly rather
        # than silently no-op'ing if the upstream text moves.
        PATCH_COMMAND ${CMAKE_COMMAND}
                      -DSOURCE_DIR=<SOURCE_DIR>
                      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_datachannel_wasm.cmake)
    FetchContent_MakeAvailable(datachannel_wasm)
    target_link_libraries(p2pgpu-transport PUBLIC datachannel-wasm)
else()
    target_sources(p2pgpu-worker-core PRIVATE
        src/worker-core/platform/gpu_native.cpp
        src/worker-core/platform/timer_native.cpp)
    target_link_libraries(p2pgpu-worker-core PUBLIC wgpu-native BLAKE3::blake3)
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
    find_package(unofficial-sqlite3 CONFIG REQUIRED)   # step 2.19

    # The coordinator's DECISION logic, split out from its transport so the
    # tests can link it. Everything here is socket-free by construction — if a
    # file in this list ever needs uWebSockets, it belongs on the other side of
    # the line (session.hpp explains why that boundary is worth keeping).
    add_library(p2pgpu-coordinator-core STATIC
        src/coordinator/affinity.cpp
        src/coordinator/event_log.cpp
        src/coordinator/quorum.cpp
        src/coordinator/reputation.cpp
        src/coordinator/spot_check.cpp
        src/coordinator/store.cpp
        src/coordinator/job.cpp
        src/coordinator/session.cpp
        src/coordinator/kernels.cpp
        src/coordinator/fleet.cpp
        src/coordinator/sizer.cpp
        src/coordinator/params.cpp
        src/coordinator/reference_check.cpp
        src/coordinator/queue.cpp
        src/coordinator/lease.cpp
        src/coordinator/validator.cpp
        src/coordinator/signaling.cpp
        src/coordinator/metrics.cpp
        src/coordinator/store.cpp
    )
    target_link_libraries(p2pgpu-coordinator-core PUBLIC
        p2pgpu-protocol
        spdlog::spdlog
        BLAKE3::blake3
        unofficial::sqlite3::sqlite3)
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
        src/mock-worker/profiles.cpp
    )
    target_link_libraries(mock-worker PRIVATE
        p2pgpu-transport CLI11::CLI11 spdlog::spdlog
        # Invariant 9's checksum. BLAKE3 is native-only and already in the
        # vcpkg `native` feature — D-0042's constraint was NO GPU, not no
        # dependencies. Without it every mock result fails the checksum and is
        # requeued, and the harness measures nothing.
        BLAKE3::blake3)
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
    #
    # A real OUTPUT rule with DEPENDS, not a POST_BUILD hook. POST_BUILD only
    # fires when the executable RELINKS, so editing index.html or ui.js alone
    # left the served copies stale — you then test the previous version and
    # conclude the fix did not work. Painful enough once with browser caching
    # on top; not worth risking twice.
    set(P2PGPU_WEB_ASSETS index.html ui.js)
    set(P2PGPU_WEB_COPIED)
    foreach(asset IN LISTS P2PGPU_WEB_ASSETS)
        add_custom_command(
            OUTPUT  "${CMAKE_BINARY_DIR}/web/${asset}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/web/${asset}"
                    "${CMAKE_BINARY_DIR}/web/${asset}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/${asset}"
            COMMENT "Copying web/${asset}")
        list(APPEND P2PGPU_WEB_COPIED "${CMAKE_BINARY_DIR}/web/${asset}")
    endforeach()
    add_custom_target(worker-browser-assets ALL DEPENDS ${P2PGPU_WEB_COPIED})
    add_dependencies(worker-browser worker-browser-assets)

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
        # Everything ui.js may call. _p2pgpu_start / _stop / _set_throttle are
        # the R7 surface added in step 1.23 — the only three things a user can
        # do, and every one reachable ONLY from a click or a slider. The rest
        # are the Phase 0 diagnostic buttons.
        -sEXPORTED_FUNCTIONS=_main,_p2pgpu_start,_p2pgpu_stop,_p2pgpu_set_throttle,_p2pgpu_run_smoke_test,_p2pgpu_run_chunking,_p2pgpu_run_bench,_p2pgpu_run_tdr_probe,_p2pgpu_report
        # HEAPU8 is REQUIRED BY datachannel-wasm, not by us (step 1.23).
        #
        # Its websocket.js does `new Uint8Array(Module['HEAPU8'].buffer, ...)`
        # to read the bytes we hand to rtc::WebSocket::send. Emscripten stopped
        # attaching the heap views to `Module` automatically, so without this
        # the library throws
        #     Cannot read properties of undefined (reading 'buffer')
        # from inside _wsSendMessage — a JS TypeError with a C++ stack above it,
        # on the very first Hello.
        #
        # The library is v0.4.0 (D-0032) and predates that change. Exactly the
        # class of mismatch D-0032 warned about: the two libdatachannel APIs
        # agree, but their RUNTIME INTEGRATION with the host does not, and only
        # the browser build can tell you.
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8
        # ASYNCIFY yields between dispatches (R4) without blocking. NOTE its
        # tension with step 1.24's -sPROXY_TO_PTHREAD: with the task loop on its
        # own thread, blocking is fine and ASYNCIFY's size/speed cost buys
        # nothing. Revisit — and log it — when 1.24 lands.
        -sASYNCIFY
        -sALLOW_MEMORY_GROWTH=1
        -sEXIT_RUNTIME=0
        -sMODULARIZE=1
        -sEXPORT_NAME=createP2pgpuWorker

        # ── Step 1.24: the task loop runs on a Web Worker ────────────────
        #
        # -pthread, NOT -sPROXY_TO_PTHREAD (D-0037). Proxying main() would also
        # move the five Phase 0 diagnostic entry points, which JS calls by
        # ccall and which currently produce the EVALUATION.md numbers — a
        # whole-module blast radius for a benefit confined to one loop. An
        # explicit std::thread inside p2pgpu_start moves exactly the loop.
        #
        # The pool is sized 1: the loop is the only thread we ever spawn.
        # Sizing it here rather than letting the thread be created on demand
        # matters, because creating a pthread on demand needs the MAIN THREAD
        # to service the request — and blocking the main thread to start the
        # thread that exists to escape the main thread is a deadlock waiting
        # for a slow frame.
        #
        # -pthread requires SharedArrayBuffer, which requires the COOP/COEP
        # headers RISKS.md §1 already mandates and tools/serve.py already
        # sets. Deployment MUST set them: without them the worker does not
        # start at all, rather than merely running slower.
        #
        # emcc warns: "-pthread + ALLOW_MEMORY_GROWTH may run non-wasm code
        # slowly". Accepted knowingly. With a growable SharedArrayBuffer the
        # JS-side heap views must be refreshed after growth, which taxes JS
        # that reads wasm memory — for us that is datachannel-wasm's send path
        # (Module['HEAPU8'], see above) and nothing else hot. Dropping
        # ALLOW_MEMORY_GROWTH instead would mean choosing a fixed heap large
        # enough for an 8 MiB payload on every device, which is worse.
        # Revisit if E2 ever shows send cost mattering.
        -pthread
        -sPTHREAD_POOL_SIZE=1
    )
    # -pthread has to be on the COMPILE line too, and on worker-core rather
    # than just the executable: atomics and TLS codegen differ, and mixing
    # objects built with and without it is an ODR violation the linker will not
    # catch.
    target_compile_options(p2pgpu-worker-core PUBLIC -pthread)

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
