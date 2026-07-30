// Portable. On-node sample accumulation — the mechanism that makes arithmetic
// intensity a tuning knob instead of a fixed property (D-0001, rule R5).
// Persistent GPU buffer per task; upload the running average on
// upload_interval_ms, never per dispatch.
// Implement in Phase 5 step 5.12.
