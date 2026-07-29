# kernels/

WGSL compute kernels. **The language-neutral shared artifact** — loaded verbatim by the browser worker (TypeScript), `worker-native` (Rust), and the CI kernel tests. A kernel that only works in one worker implementation is a bug.

Authoring rules K1–K8, the manifest format, and the R5 gate: **`docs/KERNELS.md`**.

## The short version

1. **Do the arithmetic-intensity calculation before writing any WGSL.** >10⁶ FLOP per output byte, or redesign with accumulation. Log it in `DECISIONS.md` first (R9).
2. **Chunkable.** Accept a `(start_unit, unit_count)` range so the host can split a task into ≤250 ms dispatches (R4/K1). Windows TDR kills anything blocking ~2 s.
3. **Deterministic given `(seed, unit_range)`.** Counter-based RNG (PCG/Philox), never stateful. This is what makes replication and speculative re-execution sound.
4. **Declare `determinism` honestly.** A false `exact` claim blacklists honest workers.
5. **No fp64.** WebGPU has none.
6. **Query limits at runtime, never hardcode.** `maxStorageBufferBindingSize` is commonly ~128 MB; `maxComputeWorkgroupsPerDimension` is 65535.
7. **Optional features need fallbacks.** `shader-f16` and `subgroups` are wins where present, but a worker lacking them must still contribute.

## Testing

Every kernel needs golden, chunk-invariance, cross-implementation, and limits tests in `crates/worker-native/tests/` (`docs/KERNELS.md` §5). They run through `worker-native` because CI has no browser.
