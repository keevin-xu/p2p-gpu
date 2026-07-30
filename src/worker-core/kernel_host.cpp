// Portable. Fetch WGSL by kernel id, compile, allocate against QUERIED limits
// (K4 — never hardcode), dispatch in <=250ms chunks with yields (R4/K1),
// read back, populate every TaskStats field.
// Written once, compiled to both targets — browser and native cannot drift.
// Implement in Phase 1 step 1.19.
