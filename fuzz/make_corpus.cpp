// Corpus generator for fuzz_protocol (step 1.5).
//
// Seeds come from the REAL encoder, not by hand. A hand-written "valid frame"
// that is subtly wrong wastes the fuzzer's time on inputs the parser rejects at
// byte 0 — it would never reach the Verifier, and coverage would look fine
// while testing almost nothing.
//
// Regenerate:  ./build/native-debug/make_corpus fuzz/corpus/protocol
//
// Seeds are committed evidence (CONVENTIONS.md §7 T4, EVALUATION.md E8), so
// this must stay reproducible rather than being a one-off script.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "p2pgpu/protocol/frame.hpp"
#include "p2pgpu/protocol/verify.hpp"

using namespace p2pgpu::protocol;
namespace wire = p2pgpu::wire;

namespace {

std::filesystem::path g_dir;
int g_count = 0;

void Emit(const std::string& name, std::span<const std::byte> bytes) {
    const auto path = g_dir / (name + ".bin");
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(static_cast<const void*>(bytes.data())),
            static_cast<std::streamsize>(bytes.size()));
    std::printf("  %-34s %zu bytes\n", name.c_str(), bytes.size());
    ++g_count;
}

std::vector<std::byte> Frame(std::span<const std::byte> fb,
                             std::span<const std::byte> payload = {},
                             std::uint32_t magic = kFrameMagic,
                             std::uint16_t ver = kProtocolVersion,
                             std::optional<std::uint32_t> fb_len = std::nullopt) {
    Header h;
    h.magic = magic;
    h.protocol_ver = ver;
    h.flags = payload.empty() ? 0U : static_cast<std::uint16_t>(FrameFlags::kPayloadFollows);
    h.fb_len = fb_len.value_or(static_cast<std::uint32_t>(fb.size()));
    std::vector<std::byte> out(kHeaderBytes);
    EncodeHeader(h, std::span<std::byte, kHeaderBytes>(out.data(), kHeaderBytes));
    out.insert(out.end(), fb.begin(), fb.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::byte> Bytes(const flatbuffers::FlatBufferBuilder& fbb) {
    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    return std::vector<std::byte>(p, p + fbb.GetSize());
}

template <typename Build>
std::vector<std::byte> Envelope(wire::Body type, Build build) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto body = build(fbb);
    wire::EnvelopeBuilder eb(fbb);
    eb.add_body_type(type);
    eb.add_body(body.Union());
    fbb.Finish(eb.Finish());
    return Bytes(fbb);
}

}  // namespace

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : "fuzz/corpus/protocol";
    std::filesystem::create_directories(g_dir);
    std::printf("writing seeds to %s\n\n", g_dir.string().c_str());

    // ── VALID frames, one per message shape ────────────────────────────
    // One per Body variant so the fuzzer starts with a reachable example of
    // every branch it needs to explore, rather than having to discover 17
    // union tags by mutation.
    std::printf("valid:\n");

    Emit("valid_hello", Frame(Envelope(wire::Body::Hello, [](auto& fbb) {
        auto ua = fbb.CreateString("p2pgpu-test/1.0");
        auto vendor = fbb.CreateString("apple");
        auto arch = fbb.CreateString("metal-3");
        auto dev = fbb.CreateString("");
        auto backend = fbb.CreateString("webgpu");
        std::vector<flatbuffers::Offset<flatbuffers::String>> feats{
            fbb.CreateString("timestamp-query"), fbb.CreateString("shader-f16")};
        auto fv = fbb.CreateVector(feats);
        wire::AdapterInfoBuilder ab(fbb);
        ab.add_vendor(vendor); ab.add_architecture(arch);
        ab.add_device(dev); ab.add_backend(backend);
        auto adapter = ab.Finish();
        wire::GpuLimitsBuilder gb(fbb);
        gb.add_max_buffer_size(268435456);
        gb.add_max_storage_buffer_binding_size(134217728);
        gb.add_max_compute_workgroups_per_dim(65535);
        auto limits = gb.Finish();
        wire::WorkerCapabilitiesBuilder cb(fbb);
        cb.add_kind(wire::WorkerKind::Browser);
        cb.add_user_agent(ua); cb.add_adapter(adapter);
        cb.add_features(fv); cb.add_limits(limits);
        cb.add_supports_webrtc(true);
        auto caps = cb.Finish();
        wire::HelloBuilder hb(fbb);
        hb.add_protocol_version(kProtocolVersion);
        hb.add_capabilities(caps);
        return hb.Finish();
    })));

    Emit("valid_lease_request", Frame(Envelope(wire::Body::LeaseRequest, [](auto& fbb) {
        wire::LeaseRequestBuilder b(fbb); b.add_max_tasks(4); return b.Finish();
    })));

    Emit("valid_task_grant", Frame(Envelope(wire::Body::TaskGrant, [](auto& fbb) {
        auto kid = fbb.CreateString("brute_search_v1");
        const std::vector<std::uint8_t> params(32, 0xAB);
        auto pv = fbb.CreateVector(params);
        const wire::Uuid task{1, 2}, job{3, 4};
        const wire::Hash32 href{5, 6, 7, 8};
        wire::OutputSpecBuilder ob(fbb);
        ob.add_bytes(32); ob.add_dtype(wire::DType::U32);
        auto os = ob.Finish();
        wire::AccumulationSpecBuilder ab(fbb);
        ab.add_upload_interval_ms(500); ab.add_min_iterations(1000);
        auto acc = ab.Finish();
        wire::TaskEnvelopeBuilder tb(fbb);
        tb.add_task_id(&task); tb.add_job_id(&job); tb.add_kernel_id(kid);
        tb.add_seed(0xDEADBEEF); tb.add_params(pv);
        tb.add_work_units(12500000000ULL);
        tb.add_input_ref(&href); tb.add_output_spec(os);
        tb.add_lease_ms(30000); tb.add_accumulate(acc);
        auto env = tb.Finish();
        wire::TaskGrantBuilder gb(fbb); gb.add_envelope(env); return gb.Finish();
    })));

    // The only shape that carries a trailing payload — so the only seed that
    // exercises SplitFrame's payload arithmetic on a valid input.
    {
        const std::vector<std::byte> payload(256, std::byte{0x5A});
        Emit("valid_result_header_with_payload",
             Frame(Envelope(wire::Body::ResultHeader, [](auto& fbb) {
                 const wire::Uuid task{9, 10};
                 wire::TaskStatsBuilder sb(fbb);
                 sb.add_gpu_ms(123.4); sb.add_transfer_ms(5.6); sb.add_idle_ms(0.1);
                 sb.add_dispatches(8); sb.add_iterations(2048);
                 sb.add_asset_source(wire::AssetSource::Peer);
                 auto stats = sb.Finish();
                 wire::ResultHeaderBuilder rb(fbb);
                 rb.add_task_id(&task); rb.add_payload_bytes(256);
                 rb.add_checksum(0xCAFEBABEDEADBEEFULL); rb.add_stats(stats);
                 return rb.Finish();
             }), payload));
    }

    Emit("valid_progress", Frame(Envelope(wire::Body::Progress, [](auto& fbb) {
        const wire::Uuid t{11, 12};
        wire::ProgressBuilder b(fbb);
        b.add_task_id(&t); b.add_fraction_done(0.5F); b.add_request_renew(true);
        return b.Finish();
    })));

    Emit("valid_error", Frame(Envelope(wire::Body::Error, [](auto& fbb) {
        auto msg = fbb.CreateString("blacklisted");
        wire::ErrorBuilder b(fbb);
        b.add_code(wire::ErrorCode::Blacklisted); b.add_message(msg); b.add_fatal(true);
        return b.Finish();
    })));

    Emit("valid_peer_list", Frame(Envelope(wire::Body::PeerList, [](auto& fbb) {
        std::vector<flatbuffers::Offset<wire::PeerInfo>> peers;
        for (std::uint64_t i = 0; i < 3; ++i) {
            const wire::Uuid w{i, i};
            wire::PeerInfoBuilder pb(fbb);
            pb.add_worker_id(&w);
            peers.push_back(pb.Finish());
        }
        auto pv = fbb.CreateVector(peers);
        wire::PeerListBuilder b(fbb); b.add_peers(pv); return b.Finish();
    })));

    // ── MALFORMED, per the step 1.5 list ───────────────────────────────
    std::printf("\nmalformed:\n");

    const auto good_fb = Envelope(wire::Body::LeaseRequest, [](auto& fbb) {
        wire::LeaseRequestBuilder b(fbb); b.add_max_tasks(1); return b.Finish();
    });

    // Truncated header, every length below the minimum.
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{11},
                          std::size_t{15}}) {
        Emit("truncated_header_" + std::to_string(n), std::vector<std::byte>(n, std::byte{0}));
    }

    Emit("bad_magic", Frame(good_fb, {}, 0xDEADBEEF));
    Emit("bad_version", Frame(good_fb, {}, kFrameMagic,
                              static_cast<std::uint16_t>(kProtocolVersion + 1)));

    // fb_len at UINT32_MAX — the overflow probe. If the size check were written
    // without bounding fb_len first, this is the input that wraps it.
    Emit("fb_len_uint32_max", Frame(good_fb, {}, kFrameMagic, kProtocolVersion, 0xFFFFFFFFU));
    Emit("fb_len_exceeds_frame", Frame(good_fb, {}, kFrameMagic, kProtocolVersion, 65000));
    Emit("fb_len_over_cap", Frame(good_fb, {}, kFrameMagic, kProtocolVersion,
                                  kMaxEnvelopeBytes + 1));
    Emit("fb_len_zero", Frame({}, {}, kFrameMagic, kProtocolVersion, 0));

    // Valid header, garbage where the FlatBuffer should be.
    {
        const std::vector<std::byte> junk(64, std::byte{0xFF});
        Emit("valid_header_garbage_fb", Frame(junk));
        std::vector<std::byte> zeros(64, std::byte{0x00});
        Emit("valid_header_zero_fb", Frame(zeros));
    }

    // Orphan payload, both directions.
    {
        auto f = Frame(good_fb);
        Header h{kFrameMagic, kProtocolVersion,
                 static_cast<std::uint16_t>(FrameFlags::kPayloadFollows),
                 static_cast<std::uint32_t>(good_fb.size())};
        EncodeHeader(h, std::span<std::byte, kHeaderBytes>(f.data(), kHeaderBytes));
        Emit("orphan_flag_no_payload", f);
    }
    {
        auto f = Frame(good_fb, std::vector<std::byte>(8, std::byte{1}));
        Header h{kFrameMagic, kProtocolVersion, 0,
                 static_cast<std::uint32_t>(good_fb.size())};
        EncodeHeader(h, std::span<std::byte, kHeaderBytes>(f.data(), kHeaderBytes));
        Emit("orphan_payload_no_flag", f);
    }

    // Deeply nested tables — the verifier-depth probe. PeerList nests
    // vector -> table -> vector -> struct, which is the deepest shape the
    // schema allows; kMaxVerifyDepth is what stops a crafted buffer from
    // exhausting the stack here.
    Emit("deep_peer_list_under_cap", Frame(Envelope(wire::Body::PeerList, [](auto& fbb) {
        std::vector<flatbuffers::Offset<wire::PeerInfo>> peers;
        // Sized to stay UNDER kMaxEnvelopeBytes on purpose. A first attempt used
        // 200x10 and came out at 71 KB, which the fb_len cap rejects before the
        // Verifier ever runs — a seed that tests the cap, not the depth bound.
        // Worth knowing: the 64 KiB envelope cap already bounds nesting
        // implicitly, so kMaxVerifyDepth is the SECOND line of defence here,
        // not the first. This seed exists to exercise it anyway.
        for (std::uint64_t i = 0; i < 100; ++i) {
            const wire::Uuid w{i, i};
            wire::PeerInfoBuilder pb(fbb);
            pb.add_worker_id(&w);
            peers.push_back(pb.Finish());
        }
        auto pv = fbb.CreateVector(peers);
        wire::PeerListBuilder b(fbb); b.add_peers(pv); return b.Finish();
    })));

    // Same shape but deliberately OVER the 64 KiB cap: proves the length check
    // fires before the Verifier is even constructed.
    Emit("deep_peer_list_over_cap", Frame(Envelope(wire::Body::PeerList, [](auto& fbb) {
        std::vector<flatbuffers::Offset<wire::PeerInfo>> peers;
        for (std::uint64_t i = 0; i < 200; ++i) {
            const wire::Uuid w{i, i};
            wire::PeerInfoBuilder pb(fbb);
            pb.add_worker_id(&w);
            peers.push_back(pb.Finish());
        }
        auto pv = fbb.CreateVector(peers);
        wire::PeerListBuilder b(fbb); b.add_peers(pv); return b.Finish();
    })));

    // Envelope with body_type set but no body — a shape the Verifier accepts
    // structurally and VerifyFrame must still refuse.
    {
        flatbuffers::FlatBufferBuilder fbb;
        wire::EnvelopeBuilder eb(fbb);
        fbb.Finish(eb.Finish());
        Emit("envelope_no_body", Frame(Bytes(fbb)));
    }

    // Semantically hostile but structurally valid: limits declared far over the
    // kMax* constants. Verification passes; the INVARIANTS are what must catch
    // these, which is the distinction the whole boundary rests on.
    Emit("oversized_declared_output", Frame(Envelope(wire::Body::TaskGrant, [](auto& fbb) {
        wire::OutputSpecBuilder ob(fbb);
        ob.add_bytes(0xFFFFFFFFU);            // >> kMaxOutputBytes
        auto os = ob.Finish();
        wire::TaskEnvelopeBuilder tb(fbb);
        tb.add_output_spec(os);
        tb.add_work_units(0xFFFFFFFFFFFFFFFFULL);
        auto env = tb.Finish();
        wire::TaskGrantBuilder gb(fbb); gb.add_envelope(env); return gb.Finish();
    })));

    Emit("upload_interval_zero_trap", Frame(Envelope(wire::Body::TaskGrant, [](auto& fbb) {
        wire::AccumulationSpecBuilder ab(fbb);
        ab.add_upload_interval_ms(1);         // below kMinUploadIntervalMs
        auto acc = ab.Finish();
        wire::TaskEnvelopeBuilder tb(fbb);
        tb.add_accumulate(acc);
        auto env = tb.Finish();
        wire::TaskGrantBuilder gb(fbb); gb.add_envelope(env); return gb.Finish();
    })));

    // Non-zero reserved word (D-0027). Structurally fine, rejected on purpose
    // so the field stays available for a future extension.
    {
        auto f = Frame(good_fb);
        Header h{kFrameMagic, kProtocolVersion, 0,
                 static_cast<std::uint32_t>(good_fb.size()), 0xFFFFFFFFU};
        EncodeHeader(h, std::span<std::byte, kHeaderBytes>(f.data(), kHeaderBytes));
        Emit("reserved_nonzero", f);
    }

    // Data-plane seeds: the same bytes feed VerifyAssetMsg, and the chunk index
    // is the integer-overflow site (invariant 10).
    {
        flatbuffers::FlatBufferBuilder fbb;
        const wire::Hash32 h{1, 2, 3, 4};
        const std::vector<std::uint8_t> data(64, 0x11);
        auto dv = fbb.CreateVector(data);
        wire::AssetChunkBuilder cb(fbb);
        cb.add_hash(&h); cb.add_index(0xFFFFFFFFU); cb.add_total(0xFFFFFFFFU); cb.add_bytes(dv);
        auto chunk = cb.Finish();
        wire::AssetMsgBuilder mb(fbb);
        mb.add_body_type(wire::AssetBody::AssetChunk); mb.add_body(chunk.Union());
        fbb.Finish(mb.Finish());
        Emit("asset_chunk_index_uint32_max", Bytes(fbb));
    }

    std::printf("\n%d seeds written\n", g_count);
    return 0;
}
