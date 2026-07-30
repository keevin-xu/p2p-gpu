// Portable — and this is the load-bearing one.
//
// libdatachannel (native) and datachannel-wasm (browser) expose the SAME API,
// so WebSocket control plane and WebRTC data plane are written once here for
// both targets. No platform seam needed.
//
// This property is why the single-language architecture works at all; if you
// find yourself adding an #ifdef here, re-read docs/DECISIONS.md D-0008 first.
// Implement in Phase 1 step 1.18; data plane in Phase 6.
