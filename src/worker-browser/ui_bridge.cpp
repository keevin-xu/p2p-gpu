// EM_JS glue to web/ui.js — the ONLY JavaScript boundary in the project.
//
// Exists because WASM cannot touch the DOM directly. Carries the R7 opt-in
// surface: start button, contributing indicator, throttle slider, stop.
//
// Keep it dumb. Any logic on the JS side violates R1 and R2.
// Implement in Phase 1 step 1.23.
