# Guard emdawnwebgpu's `fillAdapterInfoStruct` against absent subgroup fields.
#
# THE BUG. The glue writes the adapter's subgroup sizes into the heap with no
# fallback:
#
#     HEAPU32[(infoStruct + 52) >> 2] = info.subgroupMinSize;
#     checkInt32(info.subgroupMinSize);
#
# Safari's `GPUAdapterInfo` does not expose `subgroupMinSize` /
# `subgroupMaxSize` — subgroups are a newer feature — so the value is
# `undefined`. Every other field in this file uses `?? fallbackValue`; these two
# do not.
#
# HOW IT PRESENTS. In a release build the write silently coerces to 0. With
# assertions it aborts inside a pthread, and Emscripten rethrows across the
# worker boundary, discarding the stack — the page shows
# `Aborted(Assertion failed: attempt to write non-integer (undefined) into
# integer heap)` and nothing pointing at WebGPU, adapters, or Safari.
#
# It fires on ANY call to wgpuAdapterGetInfo, which the worker makes once at
# startup to report its device identity.
#
# FAILS LOUDLY if the upstream text moves, rather than silently not applying —
# the same rule as the datachannel-wasm patch. A patch that quietly stops
# matching is worse than no patch, because the build still succeeds.
if(NOT EXISTS "${JS_FILE}")
    message(FATAL_ERROR "patch_emdawn_adapterinfo: ${JS_FILE} does not exist")
endif()

file(READ "${JS_FILE}" JS)

set(BEFORE_MIN "= info.subgroupMinSize;")
set(BEFORE_MAX "= info.subgroupMaxSize;")
set(AFTER_MIN  "= (info.subgroupMinSize ?? 0);")
set(AFTER_MAX  "= (info.subgroupMaxSize ?? 0);")

string(FIND "${JS}" "${BEFORE_MIN}" FOUND_MIN)
string(FIND "${JS}" "${BEFORE_MAX}" FOUND_MAX)

if(FOUND_MIN EQUAL -1 AND FOUND_MAX EQUAL -1)
    string(FIND "${JS}" "${AFTER_MIN}" ALREADY)
    if(NOT ALREADY EQUAL -1)
        return()   # already patched by an earlier build of the same file
    endif()
    message(FATAL_ERROR
        "patch_emdawn_adapterinfo: neither subgroup write found in ${JS_FILE}. "
        "The upstream glue changed shape — re-check whether the unguarded "
        "write still exists before removing this patch.")
endif()

string(REPLACE "${BEFORE_MIN}" "${AFTER_MIN}" JS "${JS}")
string(REPLACE "${BEFORE_MAX}" "${AFTER_MAX}" JS "${JS}")

# Also drop the assertions that fire on the same values.
string(REPLACE "checkInt32(info.subgroupMinSize);" "" JS "${JS}")
string(REPLACE "checkInt32(info.subgroupMaxSize);" "" JS "${JS}")

file(WRITE "${JS_FILE}" "${JS}")
message(STATUS "p2pgpu: guarded emdawnwebgpu subgroup adapter-info writes")
