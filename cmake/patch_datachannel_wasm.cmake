# Make datachannel-wasm usable from a Web Worker — step 1.24, D-0038.
#
# The library has exactly TWO references to `window`, and both are feature
# guards rather than real uses:
#
#     wasm/js/websocket.js:  if(!window.WebSocket) return 0;
#     wasm/js/webrtc.js:     if(!window.RTCPeerConnection) return 0;
#
# A Web Worker has no `window`, so the first one throws
# `ReferenceError: window is not defined` before the socket is ever created —
# even though the construction below it (`new WebSocket(url)`) resolves fine on
# a worker. The library is one word away from being worker-safe.
#
# Invoked as a FetchContent PATCH_COMMAND. Written as a CMake script rather than
# `sed` because sed's -i flag differs between GNU and BSD, and this must work on
# the developer's Mac and in Linux CI.
#
# IDEMPOTENT: a second run finds no matches and writes nothing. FetchContent may
# re-run a patch step, and a patch that fails on reapplication is a build that
# breaks on the second configure.
#
# FAILS LOUDLY if the upstream text changes on a version bump, rather than
# quietly no-op'ing and leaving a browser-only failure to discover later.

set(_patched_any FALSE)

function(p2pgpu_patch_window path symbol required)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "patch_datachannel_wasm: ${path} does not exist. The upstream layout "
            "changed; re-check D-0038 before bumping the pin.")
    endif()

    file(READ "${path}" contents)

    if(contents MATCHES "globalThis\\.${symbol}")
        message(STATUS "datachannel-wasm: ${symbol} guard already patched")
        return()
    endif()

    if(NOT contents MATCHES "window\\.${symbol}")
        # Neither spelling present. Either upstream fixed it differently or the
        # guard moved — both mean this patch's assumptions no longer hold.
        if(required)
            message(FATAL_ERROR
                "patch_datachannel_wasm: found neither `window.${symbol}` nor "
                "`globalThis.${symbol}` in ${path}. Upstream changed; re-verify "
                "D-0038 before trusting the browser build.")
        endif()
        return()
    endif()

    string(REPLACE "window.${symbol}" "globalThis.${symbol}" contents "${contents}")
    file(WRITE "${path}" "${contents}")
    message(STATUS "datachannel-wasm: patched ${symbol} guard for Web Worker use")
    set(_patched_any TRUE PARENT_SCOPE)
endfunction()

# WebSocket is the one that bites today — the control plane runs on the worker
# thread as of step 1.24.
p2pgpu_patch_window("${SOURCE_DIR}/wasm/js/websocket.js" "WebSocket" TRUE)

# RTCPeerConnection is not used yet. Patched now anyway because Phase 6's data
# plane will use it from this same worker thread, and rediscovering this with
# peer connections to debug on top is a worse afternoon than fixing it here.
p2pgpu_patch_window("${SOURCE_DIR}/wasm/js/webrtc.js" "RTCPeerConnection" TRUE)
