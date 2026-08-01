// libFuzzer harness for the frame parser — the project's primary attack surface.
//
// Every byte here is what an anonymous worker can send to the coordinator.
// This harness is the evidence behind rule R11 and decision D-0010; its exec
// count and corpus get reported in EVALUATION.md E8.
//
// ── WHY IT READS FIELDS RATHER THAN JUST CHECKING THE RETURN ─────────────
// A Verifier that accepts a buffer whose fields then fault is exactly the bug
// this exists to find. Checking `if (result)` and stopping would exercise the
// verifier and nothing downstream — so every accepted message has every field
// touched below, including vector CONTENTS, which is where an under-verified
// buffer actually bites.
//
// Implemented in Phase 1 step 1.5, before anything was built on the protocol.

#include <cstddef>
#include <cstdint>
#include <span>

#include "p2pgpu/protocol/invariants.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace {

using namespace p2pgpu::protocol;
namespace wire = p2pgpu::wire;

/// Force the optimiser to keep a read. Without this the compiler is entitled to
/// delete field accesses whose results are unused — leaving a harness that
/// verifies buffers and never actually reads them. It would look identical in
/// the exec counter and find nothing.
template <typename T>
void Consume(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

void TouchUuid(const wire::Uuid* u) {
    if (u != nullptr) { Consume(u->hi()); Consume(u->lo()); }
}

void TouchHash(const wire::Hash32* h) {
    if (h != nullptr) { Consume(h->a()); Consume(h->b()); Consume(h->c()); Consume(h->d()); }
}

void TouchString(const flatbuffers::String* s) {
    if (s == nullptr) { return; }
    Consume(s->size());
    // Walk the bytes. A string whose length survived verification but whose
    // storage did not is precisely an out-of-bounds read, and only touching the
    // contents can expose it.
    for (const char c : s->string_view()) { Consume(c); }
}

void TouchEnvelopeFields(const wire::TaskEnvelope* e) {
    if (e == nullptr) { return; }
    TouchUuid(e->task_id());
    TouchUuid(e->job_id());
    TouchUuid(e->replica_of());
    TouchHash(e->input_ref());
    TouchString(e->kernel_id());
    Consume(e->seed());
    Consume(e->work_units());
    Consume(e->lease_ms());

    if (const auto* p = e->params()) {
        Consume(p->size());
        for (std::uint32_t i = 0; i < p->size(); ++i) { Consume(p->Get(i)); }
    }
    if (const auto* os = e->output_spec()) { Consume(os->bytes()); Consume(os->dtype()); }
    if (const auto* acc = e->accumulate()) {
        Consume(acc->upload_interval_ms());
        Consume(acc->min_iterations());
    }
    // The invariants are part of the boundary. A length check that itself
    // faulted on a crafted envelope would be a fine irony.
    Consume(CheckTaskEnvelope(*e).ok());
}

void TouchCapabilities(const wire::WorkerCapabilities* c) {
    if (c == nullptr) { return; }
    Consume(c->kind());
    Consume(c->supports_webrtc());
    TouchString(c->user_agent());
    if (const auto* a = c->adapter()) {
        TouchString(a->vendor()); TouchString(a->architecture());
        TouchString(a->device());  TouchString(a->backend());
    }
    if (const auto* f = c->features()) {
        for (std::uint32_t i = 0; i < f->size(); ++i) { TouchString(f->Get(i)); }
    }
    if (const auto* l = c->limits()) {
        Consume(l->max_buffer_size());
        Consume(l->max_storage_buffer_binding_size());
        Consume(l->max_compute_workgroups_per_dim());
        Consume(l->max_compute_invocations_per_group());
        Consume(l->max_compute_workgroup_storage_size());
    }
}

void TouchBody(const wire::Envelope& env) {
    // NO `default:` ARM. -Werror=switch then makes adding a Body variant a
    // BUILD failure here, which is what keeps the fuzzer's coverage honest as
    // the protocol grows (ARCHITECTURE.md §5).
    switch (env.body_type()) {
        case wire::Body::NONE: break;
        case wire::Body::Hello:
            if (const auto* m = env.body_as_Hello()) {
                Consume(m->protocol_version());
                TouchString(m->resume_token());
                TouchCapabilities(m->capabilities());
            }
            break;
        case wire::Body::Welcome:
            if (const auto* m = env.body_as_Welcome()) {
                TouchUuid(m->worker_id());
                TouchString(m->session_token());
                Consume(m->heartbeat_ms());
                if (const auto* ks = m->kernels()) {
                    for (std::uint32_t i = 0; i < ks->size(); ++i) {
                        const auto* k = ks->Get(i);
                        TouchString(k->kernel_id());
                        TouchString(k->entry_point());
                        Consume(k->accumulates());
                        Consume(k->min_iterations());
                        Consume(k->flop_per_unit());
                        Consume(k->determinism_type());
                    }
                }
            }
            break;
        case wire::Body::BenchmarkRequest:
            if (const auto* m = env.body_as_BenchmarkRequest()) {
                TouchString(m->kernel_id()); Consume(m->target_ms());
            }
            break;
        case wire::Body::BenchmarkResult:
            if (const auto* m = env.body_as_BenchmarkResult()) {
                TouchString(m->kernel_id()); Consume(m->score()); Consume(m->samples());
            }
            break;
        case wire::Body::LeaseRequest:
            if (const auto* m = env.body_as_LeaseRequest()) { Consume(m->max_tasks()); }
            break;
        case wire::Body::TaskGrant:
            if (const auto* m = env.body_as_TaskGrant()) { TouchEnvelopeFields(m->envelope()); }
            break;
        case wire::Body::LeaseAck:
            if (const auto* m = env.body_as_LeaseAck()) {
                TouchUuid(m->task_id()); Consume(m->expires_at_ms());
            }
            break;
        case wire::Body::Progress:
            if (const auto* m = env.body_as_Progress()) {
                TouchUuid(m->task_id()); Consume(m->fraction_done()); Consume(m->request_renew());
            }
            break;
        case wire::Body::ResultHeader:
            if (const auto* m = env.body_as_ResultHeader()) {
                TouchUuid(m->task_id());
                Consume(m->payload_bytes());
                Consume(m->checksum());
                if (const auto* s = m->stats()) {
                    Consume(s->gpu_ms()); Consume(s->transfer_ms()); Consume(s->idle_ms());
                    Consume(s->dispatches()); Consume(s->iterations()); Consume(s->asset_source());
                }
            }
            break;
        case wire::Body::Release:
            if (const auto* m = env.body_as_Release()) {
                TouchUuid(m->task_id()); Consume(m->reason());
            }
            break;
        case wire::Body::Revoke:
            if (const auto* m = env.body_as_Revoke()) {
                TouchUuid(m->task_id()); Consume(m->reason());
            }
            break;
        case wire::Body::Throttle:
            if (const auto* m = env.body_as_Throttle()) { Consume(m->level()); }
            break;
        case wire::Body::PeerList:
            if (const auto* m = env.body_as_PeerList()) {
                if (const auto* ps = m->peers()) {
                    for (std::uint32_t i = 0; i < ps->size(); ++i) {
                        const auto* p = ps->Get(i);
                        TouchUuid(p->worker_id());
                    }
                }
            }
            break;
        case wire::Body::Signal:
            if (const auto* m = env.body_as_Signal()) {
                TouchUuid(m->peer());
                if (const auto* p = m->payload()) {
                    for (std::uint32_t i = 0; i < p->size(); ++i) { Consume(p->Get(i)); }
                }
            }
            break;
        case wire::Body::Goodbye:
            if (const auto* m = env.body_as_Goodbye()) { Consume(m->reason()); }
            break;
        case wire::Body::Shutdown:
            if (const auto* m = env.body_as_Shutdown()) { TouchString(m->reason()); }
            break;
        case wire::Body::Error:
            if (const auto* m = env.body_as_Error()) {
                Consume(m->code()); Consume(m->fatal()); TouchString(m->message());
            }
            break;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The one permitted cast in this file: adapting libFuzzer's C entry
    // signature. It is not parsing, and nothing downstream of the span sees a
    // raw pointer (CONVENTIONS.md §2).
    const std::span<const std::byte> frame{
        static_cast<const std::byte*>(static_cast<const void*>(data)), size};

    // Control plane: the full docs/PROTOCOL.md §1 sequence.
    if (const auto verified = VerifyFrame(frame)) {
        TouchBody(*verified->envelope());
        // The payload sits OUTSIDE the verified region (D-0009), so it gets its
        // own bounds walk — SplitFrame's arithmetic is what keeps it in range,
        // and that arithmetic is exactly what is worth fuzzing.
        for (const std::byte b : verified->payload()) { Consume(b); }
    }

    // Data plane: peer-supplied bytes, no coordinator mediation at all. Fed the
    // same input, because a hostile peer is not obliged to send well-formed
    // frames and this path must survive anything.
    if (const auto asset = VerifyAssetMsg(frame)) {
        const wire::AssetMsg* msg = *asset;
        switch (msg->body_type()) {
            case wire::AssetBody::NONE: break;
            case wire::AssetBody::AssetRequest:
                if (const auto* m = msg->body_as_AssetRequest()) {
                    TouchHash(m->hash()); Consume(m->chunk_from()); Consume(m->chunk_to());
                }
                break;
            case wire::AssetBody::AssetChunk:
                if (const auto* m = msg->body_as_AssetChunk()) {
                    TouchHash(m->hash());
                    const std::uint32_t idx = m->index();
                    const std::uint32_t total = m->total();
                    const std::size_t n = m->bytes() != nullptr ? m->bytes()->size() : 0;
                    // The integer-overflow site (invariant 10). Fuzzing the
                    // CHECK matters as much as fuzzing the parser: an offset
                    // computed from an attacker's index is the classic wrap.
                    Consume(CheckAssetChunk(idx, total, total, n).ok());
                    Consume(ChunkOffset(idx).has_value());
                    if (const auto* b = m->bytes()) {
                        for (std::uint32_t i = 0; i < b->size(); ++i) { Consume(b->Get(i)); }
                    }
                }
                break;
            case wire::AssetBody::AssetMiss:
                if (const auto* m = msg->body_as_AssetMiss()) { TouchHash(m->hash()); }
                break;
        }
    }
    return 0;
}
