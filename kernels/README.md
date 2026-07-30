# kernels/

WGSL compute kernels, loaded **verbatim** by every worker target — `worker-browser` (WASM), `worker-native`, and the CI kernel tests. All three run the same `worker-core` host, so a kernel that behaves differently between them means either a real vendor/driver difference (see R6) or a bug in the platform seam.

Authoring rules K1–K8, the manifest format, and the R5 gate: **`docs/KERNELS.md`**.

## The short version

1. **Do the arithmetic-intensity calculation before writing any WGSL.** >10⁶ FLOP per output byte, or redesign with accumulation. Log it in `DECISIONS.md` first (R9).
2. **Chunkable.** Accept a `(start_unit, unit_count)` range so the host can split a task into ≤250 ms dispatches (R4/K1). Windows TDR kills anything blocking ~2 s.
3. **Deterministic given `(seed, unit_range)`.** Counter-based RNG (PCG/Philox), never stateful. This is what makes replication and speculative re-execution sound.
4. **Declare `determinism` honestly.** A false `exact` claim blacklists honest workers.
5. **No fp64.** WebGPU has none.
6. **Query limits at runtime, never hardcode.** `maxStorageBufferBindingSize` is commonly ~128 MB; `maxComputeWorkgroupsPerDimension` is 65535.
7. **Optional features need fallbacks.** `shader-f16` and `subgroups` are wins where present, but a worker lacking them must still contribute.
8. **Mirror the params struct in C++ with a `static_assert`.** Silent layout drift produces garbage that looks like a kernel bug and costs hours.

## Testing

Every kernel needs golden, chunk-invariance, cross-implementation, and limits tests under `tests/kernels/` (`docs/KERNELS.md` §5). They run headless through `worker-native`, because CI has no browser.
