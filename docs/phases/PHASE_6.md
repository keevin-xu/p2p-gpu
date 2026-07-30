# Phase 6 — P2P Data Plane

**Objective:** distribute the BVH asset peer-to-peer over WebRTC so coordinator egress stays flat as the fleet grows — and measure it.

**Why now:** needs a large real asset (Phase 5's BVH) to justify existing. Building P2P before there is anything worth distributing produces an unmeasurable feature.

**Entry criteria:** G5 approved.

> **This is the only honest justification for "P2P" in the project name.** The measurement in E6 is what makes the claim defensible rather than decorative. Verify TURN access before starting (`RISKS.md` R-G).
>
> **Structural advantage here:** libdatachannel (native) and datachannel-wasm (browser) expose the same API, so the peer transport is written **once** in `worker-core` and both targets get it (R2, D-0008). Resist any temptation to special-case the browser.

---

## Steps

### Signaling

- [ ] **6.1 — Signaling relay in the coordinator.**
  Relay `Signal{to, payload}` between workers. The coordinator brokers; it never joins the media path.
  Signal payloads are relayed attacker-controlled bytes — bound their size and never parse SDP coordinator-side. Relay opaquely.

- [ ] **6.2 — Peer list distribution.**
  `PeerList{peers}` with candidates. Prefer peers known to hold the needed content hash; cap simultaneous connections per worker (a full mesh at N=50 is 1225 connections — do not build one).

- [ ] **6.3 — Coordinator-brokered discovery, not a DHT.**
  Per `ARCHITECTURE.md` §9 — roughly 10× less code and adequate at this scale. If it becomes limiting, that is a finding worth a `DECISIONS.md` entry, not an assumption to pre-optimize against.

### WebRTC transport

- [ ] **6.4 — Peer connection + data channel in `worker-core`.**
  libdatachannel API, one implementation, both targets. Offer/answer over the signaling relay.

- [ ] **6.5 — ICE with STUN and TURN.**
  Public STUN plus a configured TURN server. Instrument which path each connection took — direct vs. relayed. That ratio is E6 data.

- [ ] **6.6 — Chunked transfer at 16 KiB.**
  Safe cross-browser floor; do not assume 256 KiB EOR support (`PROTOCOL.md` §1). Implement backpressure — flooding a data channel stalls it.

- [ ] **6.7 — Asset protocol with hostile-input discipline.**
  `AssetRequest` / `AssetChunk` / `AssetMiss` as verified FlatBuffers. Then, before any offset is computed:
  - `index < total`, `total == expected_chunks_for(hash)`
  - `index * kChunkBytes` via **checked arithmetic**; overflow ⇒ reject the peer
  - reassembly buffer preallocated to the known asset size; a chunk that would write past it is rejected, not clamped
  - BLAKE3 verify the reassembled asset before use

  Peer-supplied bytes are the least trustworthy input in the system — they come from an arbitrary participant with no coordinator mediation at all. `fuzz_asset` (step 4.13) covers this path; run it against any changes here.

- [ ] **6.8 — Connection lifecycle.**
  Peers disappear constantly. Handle ICE failure, channel close, and partial transfers by falling back cleanly. No retry storms.

### Fallback & correctness

- [ ] **6.9 — Coordinator fallback (D-0007).**
  Any failed or slow peer fetch falls back to `GET /asset/{hash}`. Timeout-based, not failure-only — a merely slow peer should not block a task.

- [ ] **6.10 — Correctness with the data plane fully disabled.**
  A config flag disables P2P entirely; the full job must still complete correctly. Both a hard requirement and the control condition for E6.

- [ ] **6.11 — Record `asset_source` in `TaskStats`.**
  `Cached` / `Peer` / `Coordinator` / `None`. Raw data for the E6 fetch-source breakdown.

- [ ] **6.12 — Swarm behavior.**
  A worker holding the asset serves it to others. Track which peers hold which hashes so `PeerList` selection is informed. Seed from the coordinator only as needed.

### Experiments

- [ ] **6.13 — E6 egress experiment.**
  Total coordinator egress vs. worker count (10 / 25 / 50 / 100), with and without P2P. Expect flat vs. linear. Explain the flat line's residual — seed copies plus fallbacks.

- [ ] **6.14 — Fetch-source breakdown.**
  Fraction served by peer vs. coordinator vs. cache, over time. Should shift toward peers as the swarm warms.

- [ ] **6.15 — NAT/TURN measurement.**
  What fraction of peer connections needed TURN relay. Literature says 10–20%; **your measured number is a real datapoint** worth reporting.

- [ ] **6.16 — Time-to-ready.**
  Worker join to first task started, with and without P2P. Should improve at scale (more sources) and may worsen at very small N (connection setup overhead). Report both honestly — a result that is not uniformly favorable is more credible, not less.

- [ ] **6.17 — Malicious-peer experiment.**
  A mock worker that serves *wrong bytes* for a valid hash. Confirm the receiving worker rejects on hash mismatch, falls back to the coordinator, and completes the task correctly. Report detection.
  Cheap to run and it closes the obvious hole in the P2P story before anyone asks.

---

## Deliverables

- Signaling relay + peer selection
- Chunked, hash-verified, bounds-checked, backpressure-aware asset transfer written once for both targets
- Coordinator fallback with timeout
- P2P-disabled mode proven correct
- E6 charts: egress, fetch-source breakdown, TURN rate, time-to-ready
- Malicious-peer rejection result

## Exit criteria

1. Coordinator egress measurably flat vs. worker count with P2P; linear without
2. Full job completes correctly with the data plane entirely disabled
3. Peer-fetch success rate and coordinator-fallback rate measured
4. TURN relay fraction measured and reported
5. All peer-supplied assets hash-verified and bounds-checked before use
6. Malicious peer serving wrong bytes is detected and recovered from
7. No retry storms or connection leaks under the `flaps` chaos profile
8. `fuzz_asset` clean against the live reassembly code

---

## → HUMAN GATE G6

Produce for review: egress chart (with/without P2P); fetch-source breakdown over time; TURN relay fraction; proof of correctness with P2P disabled; malicious-peer result.

**The question being answered:** is the P2P claim backed by a real measurement, or is it decoration?

If egress does not flatten, say so and diagnose — an honest negative here is far better than a chart that does not survive scrutiny.

**Stop here.**
