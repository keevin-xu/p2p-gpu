// Seed corpus for the asset fuzzer — step 4.13.
//
// Regenerate:  ./build/native-debug/make_asset_corpus fuzz/corpus/asset
//
// The asset path is NOT framed: `VerifyAssetMsg` takes a bare FlatBuffer,
// because a second `root_type` is impossible in one schema and wrapping both
// planes in a shared root would put data-plane bytes through the control-plane
// parser (see the note in p2pgpu.fbs).
//
// The seeds that matter are the boundary ones. A fuzzer starting from random
// bytes finds `index = 0xFFFFFFFF` eventually; starting from a valid AssetChunk
// with that index in it, every mutation nearby is a variation on the
// integer-overflow site invariant 10 exists for.

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "p2pgpu/p2pgpu_generated.h"
#include "p2pgpu/protocol/limits.hpp"

namespace {

namespace wire = p2pgpu::wire;
std::filesystem::path g_dir;

void Emit(const char* name, const std::vector<std::byte>& bytes) {
    const auto path = g_dir / name;
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(static_cast<const void*>(bytes.data())),
              static_cast<std::streamsize>(bytes.size()));
    std::printf("  %-28s %zu bytes\n", name, bytes.size());
}

std::vector<std::byte> Bytes(const flatbuffers::FlatBufferBuilder& fbb) {
    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    return std::vector<std::byte>(p, p + fbb.GetSize());
}

wire::Hash32 SomeHash() { return wire::Hash32(1, 2, 3, 4); }

std::vector<std::byte> Chunk(std::uint32_t index, std::uint32_t total,
                             std::size_t payload_len) {
    flatbuffers::FlatBufferBuilder fbb;
    std::vector<std::uint8_t> payload(payload_len, 0xAB);
    auto data = fbb.CreateVector(payload);
    const wire::Hash32 h = SomeHash();
    wire::AssetChunkBuilder cb(fbb);
    cb.add_hash(&h);
    cb.add_index(index);
    cb.add_total(total);
    cb.add_bytes(data);
    const auto body = cb.Finish();
    wire::AssetMsgBuilder mb(fbb);
    mb.add_body_type(wire::AssetBody::AssetChunk);
    mb.add_body(body.Union());
    fbb.Finish(mb.Finish());
    return Bytes(fbb);
}

std::vector<std::byte> Request(std::uint32_t from, std::uint32_t to) {
    flatbuffers::FlatBufferBuilder fbb;
    const wire::Hash32 h = SomeHash();
    wire::AssetRequestBuilder rb(fbb);
    rb.add_hash(&h);
    rb.add_chunk_from(from);
    rb.add_chunk_to(to);
    const auto body = rb.Finish();
    wire::AssetMsgBuilder mb(fbb);
    mb.add_body_type(wire::AssetBody::AssetRequest);
    mb.add_body(body.Union());
    fbb.Finish(mb.Finish());
    return Bytes(fbb);
}

}  // namespace

int main(int argc, char** argv) {
    g_dir = argc > 1 ? argv[1] : "fuzz/corpus/asset";
    std::filesystem::create_directories(g_dir);
    std::printf("writing asset seeds to %s\n\n", g_dir.string().c_str());

    std::printf("valid:\n");
    Emit("valid_chunk_first", Chunk(0, 4, p2pgpu::protocol::kChunkBytes));
    Emit("valid_chunk_last", Chunk(3, 4, 100));
    Emit("valid_request", Request(0, 4));

    // ── the integer-overflow site (invariant 10) ───────────────────────
    // `index * kChunkBytes` wraps for index >= 2^32 / 16384 = 262144. Seeded
    // exactly at and around that boundary, because a mutation-only fuzzer
    // spends a long time finding a 32-bit constant.
    std::printf("\noverflow boundary:\n");
    Emit("index_at_wrap", Chunk(262144, 262145, 16));
    Emit("index_past_wrap", Chunk(262145, 262146, 16));
    Emit("index_max", Chunk(0xFFFFFFFFu, 0xFFFFFFFFu, 16));
    Emit("index_max_total_one", Chunk(0xFFFFFFFFu, 1, 16));

    // ── inconsistent metadata ──────────────────────────────────────────
    // index >= total, and a payload larger than one chunk. Both must be
    // rejected before any offset is computed.
    std::printf("\ninconsistent:\n");
    Emit("index_equals_total", Chunk(4, 4, 16));
    Emit("index_beyond_total", Chunk(99, 4, 16));
    Emit("zero_total", Chunk(0, 0, 16));
    Emit("oversized_payload", Chunk(0, 4, p2pgpu::protocol::kChunkBytes + 1));
    Emit("empty_payload", Chunk(0, 4, 0));

    // A range that runs backwards, which must not become a loop bound.
    Emit("request_reversed", Request(10, 2));
    Emit("request_max", Request(0, 0xFFFFFFFFu));

    std::printf("\ndone\n");
    return 0;
}
