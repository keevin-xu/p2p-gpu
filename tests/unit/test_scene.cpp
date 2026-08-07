// Scene parsing, BVH build, and the hostile loader — steps 5.2 / 5.3 / 5.5.
//
// ── THE POINT OF MOST OF THIS FILE ───────────────────────────────────────
// `LoadBvh` is the function that stands in for `flatbuffers::Verifier` on the
// asset path (D-0069), and R11's whole content is that it must REJECT things.
// A loader tested only on valid input is the same shape as a CI step that
// cannot fail (D-0067): it passes whatever you feed it and proves nothing.
//
// So each rejection case below starts from a VALID blob and breaks exactly one
// thing. If a check is ever deleted, its case starts passing and the test fails.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "p2pgpu/scene/bvh.hpp"
#include "p2pgpu/scene/scene.hpp"

using namespace p2pgpu::scene;

namespace {

constexpr const char* kMinimal = R"(version 1
camera origin 0 1 4 target 0 0 0 up 0 1 0 vfov_deg 40
material 0 lambertian 0.5 0.5 0.5
material 1 metal 0.9 0.9 0.9 0.1
material 2 emissive 5 5 5
sphere 0 1 0 1.0 0
sphere 3 1 0 1.0 1
sphere -3 1 0 1.0 2
tri -9 0 -9  9 0 -9  9 0 9  0
tri -9 0 -9  9 0 9  -9 0 9  0
)";

Scene GoodScene() {
    auto s = ParseScene(kMinimal);
    REQUIRE(s.has_value());
    return *s;
}

std::vector<std::byte> GoodBlob() {
    return SerializeBvh(BuildBvh(GoodScene()));
}

/// Read/write a u32 at a byte offset without aliasing games.
std::uint32_t GetU32(const std::vector<std::byte>& b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, sizeof(v));
    return v;
}
void SetU32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    std::memcpy(b.data() + at, &v, sizeof(v));
}

constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffNodeCount = 8;
constexpr std::size_t kOffPrimCount = 12;
constexpr std::size_t kOffMaterialCount = 16;
constexpr std::size_t kOffNodeOffset = 20;
constexpr std::size_t kOffPrimOffset = 24;
constexpr std::size_t kOffTotalBytes = 32;
constexpr std::size_t kOffReserved0 = 36;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// 5.2 — the trusted parser
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("a well-formed scene parses", "[scene]") {
    const Scene s = GoodScene();
    CHECK(s.spheres.size() == 3);
    CHECK(s.triangles.size() == 2);
    CHECK(s.materials.size() == 3);
    CHECK(s.primitive_count() == 5);
}

TEST_CASE("version must come first, so a future format is rejected not misread",
          "[scene]") {
    // The failure this prevents: a v1 parser reading a v2 file, ignoring what
    // it does not understand, and rendering a scene missing half its geometry
    // while reporting success.
    CHECK_FALSE(ParseScene("sphere 0 1 0 1.0 0\nversion 1\n").has_value());
    CHECK_FALSE(ParseScene("version 2\nmaterial 0 lambertian 1 1 1\n").has_value());
}

TEST_CASE("an unknown directive is an error, never ignored", "[scene]") {
    std::string text = kMinimal;
    text += "portal 1 2 3\n";
    CHECK_FALSE(ParseScene(text).has_value());
}

TEST_CASE("a primitive cannot reference an undefined material", "[scene]") {
    // Sparse indices are legal, so "material 5 exists" and "index 5 is defined"
    // are different questions — the second is the one that matters.
    const char* text = R"(version 1
material 0 lambertian 1 1 1
sphere 0 1 0 1.0 4
)";
    CHECK_FALSE(ParseScene(text).has_value());
}

TEST_CASE("malformed numbers and degenerate geometry are rejected", "[scene]") {
    const auto bad = [](const std::string& body) {
        return ParseScene("version 1\nmaterial 0 lambertian 1 1 1\n" + body);
    };
    CHECK_FALSE(bad("sphere 0 1 0 0.0 0\n").has_value());    // zero radius
    CHECK_FALSE(bad("sphere 0 1 0 -1.0 0\n").has_value());   // negative radius
    CHECK_FALSE(bad("sphere 0 1 0 1.5x 0\n").has_value());   // trailing junk
    CHECK_FALSE(bad("sphere 0 1 0 1.0\n").has_value());      // missing material
    CHECK_FALSE(bad("tri 0 0 0 1 0 0 0 1 0\n").has_value()); // missing material
}

TEST_CASE("the reported error line points at the offending directive", "[scene]") {
    std::size_t line = 0;
    const auto r = ParseScene("version 1\nmaterial 0 lambertian 1 1 1\nbogus\n", &line);
    REQUIRE_FALSE(r.has_value());
    CHECK(line == 3);
}

// ─────────────────────────────────────────────────────────────────────────
// 5.3 — build and round-trip
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("a built BVH round-trips through serialization", "[bvh]") {
    const Bvh built = BuildBvh(GoodScene());
    const auto blob = SerializeBvh(built);
    const auto back = LoadBvh(blob);
    REQUIRE(back.has_value());
    CHECK(back->nodes.size() == built.nodes.size());
    CHECK(back->prims.size() == built.prims.size());
    CHECK(back->materials.size() == built.materials.size());
    CHECK(back->max_leaf_prims == built.max_leaf_prims);
}

TEST_CASE("builder depth and independently recomputed depth agree", "[bvh]") {
    // THIS TEST FOUND A REAL BUG and is kept for that reason.
    //
    // `BvhNode` stores one child index because the right child is DEFINED as
    // `left + 1`. The first builder recursed into the left subtree before
    // allocating the right child, so the right child actually landed at
    // `left + sizeof(left subtree)` — every reader following `left + 1` walked
    // into the middle of the left subtree.
    //
    // Nothing else caught it. The image would have rendered; it would just have
    // been of a tree nobody built. The two depths disagreed (13 vs 11) because
    // the loader recomputes depth by walking `left`/`left+1` while the builder
    // counts its own recursion — two independent derivations of one number,
    // which is exactly why this cross-check is worth keeping.
    const Bvh built = BuildBvh(GoodScene());
    const auto back = LoadBvh(SerializeBvh(built));
    REQUIRE(back.has_value());
    CHECK(back->max_depth == built.max_depth);
}

TEST_CASE("every leaf range and child index is inside its array", "[bvh]") {
    const Bvh b = BuildBvh(GoodScene());
    std::size_t leaf_prims = 0;
    for (std::size_t i = 0; i < b.nodes.size(); ++i) {
        const auto& n = b.nodes[i];
        if ((n.count_and_leaf & kLeafFlag) != 0) {
            const std::uint32_t count = n.count_and_leaf & ~kLeafFlag;
            CHECK(n.left_or_first + count <= b.prims.size());
            leaf_prims += count;
        } else {
            CHECK(n.left_or_first > i);
            CHECK(n.left_or_first + 1 < b.nodes.size());
        }
    }
    // Every primitive lands in exactly one leaf: a partition that dropped or
    // duplicated one would still render, slightly wrong.
    CHECK(leaf_prims == b.prims.size());
}

TEST_CASE("the content address changes with content and not with rebuilds",
          "[bvh]") {
    const auto blob = GoodBlob();
    CHECK(ContentAddress(blob) == ContentAddress(GoodBlob()));
    CHECK(ContentAddress(blob).size() == 64);

    std::string moved = kMinimal;
    moved.replace(moved.find("sphere 3 1 0"), 12, "sphere 4 1 0");
    const auto other = ParseScene(moved);
    REQUIRE(other.has_value());
    CHECK(ContentAddress(SerializeBvh(BuildBvh(*other))) != ContentAddress(blob));
}

// ─────────────────────────────────────────────────────────────────────────
// 5.5 — the hostile loader. Each case breaks exactly one thing.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("the valid blob loads, so the rejections below mean something",
          "[bvh][hostile]") {
    // The control. Without it every REQUIRE_FALSE beneath could be passing for
    // the wrong reason.
    REQUIRE(LoadBvh(GoodBlob()).has_value());
}

TEST_CASE("a truncated buffer is rejected", "[bvh][hostile]") {
    auto blob = GoodBlob();
    blob.resize(blob.size() - 1);
    CHECK_FALSE(LoadBvh(blob).has_value());
    CHECK_FALSE(LoadBvh(std::vector<std::byte>(8)).has_value());
    CHECK_FALSE(LoadBvh({}).has_value());
}

TEST_CASE("bad magic and unknown version are rejected", "[bvh][hostile]") {
    auto blob = GoodBlob();
    SetU32(blob, kOffMagic, 0xDEADBEEF);
    CHECK_FALSE(LoadBvh(blob).has_value());

    blob = GoodBlob();
    SetU32(blob, kOffVersion, 2);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("a non-zero reserved field is rejected", "[bvh][hostile]") {
    // Keeps the extension point genuinely free: if a non-zero value were
    // tolerated now, a future version could not give it a meaning (D-0027).
    auto blob = GoodBlob();
    SetU32(blob, kOffReserved0, 1);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("declared counts over the limit are rejected before allocating",
          "[bvh][hostile]") {
    // R11: nothing here prevents an attacker declaring gigabytes. The check
    // must precede the allocation, not follow it.
    auto blob = GoodBlob();
    SetU32(blob, kOffNodeCount, 0xFFFF'FFFFU);
    CHECK_FALSE(LoadBvh(blob).has_value());

    blob = GoodBlob();
    SetU32(blob, kOffPrimCount, 0xFFFF'FFFFU);
    CHECK_FALSE(LoadBvh(blob).has_value());

    blob = GoodBlob();
    SetU32(blob, kOffMaterialCount, 0xFFFF'FFFFU);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("a count whose byte size wraps 32 bits is rejected", "[bvh][hostile]") {
    // The `fuzz_asset` lesson (4.13): unsigned multiplication WRAPS by
    // definition, so no sanitizer fires and the result is a wrong value rather
    // than a crash. 2^26 nodes x 32 bytes = 2^31, and the section arithmetic
    // must be done in 64-bit for this to be caught.
    auto blob = GoodBlob();
    SetU32(blob, kOffNodeCount, 1U << 27);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("section offsets disagreeing with the counts are rejected",
          "[bvh][hostile]") {
    auto blob = GoodBlob();
    SetU32(blob, kOffNodeOffset, GetU32(blob, kOffNodeOffset) + 16);
    CHECK_FALSE(LoadBvh(blob).has_value());

    blob = GoodBlob();
    SetU32(blob, kOffTotalBytes, GetU32(blob, kOffTotalBytes) + 64);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("a leaf pointing outside the primitive array is rejected",
          "[bvh][hostile]") {
    auto blob = GoodBlob();
    const std::uint32_t node_off = GetU32(blob, kOffNodeOffset);
    const std::uint32_t prim_count = GetU32(blob, kOffPrimCount);
    // Find a leaf and push its first-primitive index past the end.
    bool corrupted = false;
    for (std::uint32_t i = 0; i < GetU32(blob, kOffNodeCount); ++i) {
        const std::size_t base = node_off + std::size_t{i} * 32;
        if ((GetU32(blob, base + 28) & kLeafFlag) != 0) {
            SetU32(blob, base + 12, prim_count);
            corrupted = true;
            break;
        }
    }
    REQUIRE(corrupted);  // see the note in the cycle case below
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("a child index that does not increase is rejected, so cycles cannot exist",
          "[bvh][hostile]") {
    // A cycle would spin the GPU traversal loop forever, and R4's chunking
    // cannot interrupt a running shader — the worker would hang with no way
    // back. Acyclicity is structural: a child must be strictly greater than its
    // parent, so a back-edge is unrepresentable rather than merely unlikely.
    auto blob = GoodBlob();
    const std::uint32_t node_off = GetU32(blob, kOffNodeOffset);
    bool corrupted = false;
    for (std::uint32_t i = 0; i < GetU32(blob, kOffNodeCount); ++i) {
        const std::size_t base = node_off + std::size_t{i} * 32;
        if ((GetU32(blob, base + 28) & kLeafFlag) == 0) {
            SetU32(blob, base + 12, i);  // child points at its own parent
            corrupted = true;
            break;
        }
    }
    // ASSERT THE MUTATION HAPPENED. Written first as `i = 1`, which found no
    // internal node in this small tree, changed nothing, and left the test
    // passing a perfectly valid blob to a function it was supposed to be
    // breaking. A negative test that never applies its negation is the same
    // shape as D-0067's CI step: it cannot fail, so it proves nothing.
    REQUIRE(corrupted);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("an out-of-range material reference is rejected", "[bvh][hostile]") {
    auto blob = GoodBlob();
    const std::uint32_t prim_off = GetU32(blob, kOffPrimOffset);
    SetU32(blob, prim_off + 4, GetU32(blob, kOffMaterialCount));
    CHECK_FALSE(LoadBvh(blob).has_value());
}

TEST_CASE("an unknown primitive kind is rejected", "[bvh][hostile]") {
    auto blob = GoodBlob();
    const std::uint32_t prim_off = GetU32(blob, kOffPrimOffset);
    SetU32(blob, prim_off, 99);
    CHECK_FALSE(LoadBvh(blob).has_value());
}

// ─────────────────────────────────────────────────────────────────────────
// 5.4 — asset store and the worker-side verify-before-use cache
// ─────────────────────────────────────────────────────────────────────────

#include "p2pgpu/coordinator/assets.hpp"
#include "p2pgpu/worker/asset_cache.hpp"
#include "p2pgpu/worker/checksum.hpp"

TEST_CASE("the coordinator and the worker agree on a content address",
          "[bvh][asset]") {
    // THE D-0034 PIN, for the same reason and against a different pair of
    // functions. `scene::ContentAddress` and `worker::Blake3Hex` hash the same
    // bytes in two libraries that cannot share a dependency: p2pgpu-scene must
    // stay free of vcpkg `native` packages so `LoadBvh` is fuzzable (D-0069),
    // while the browser worker links BLAKE3 from upstream entirely (D-0034).
    //
    // If they ever disagree the symptom is "every asset fetch is corrupt", with
    // nothing in any log pointing at the hash.
    const auto blob = GoodBlob();
    CHECK(ContentAddress(blob) == p2pgpu::worker::Blake3Hex(blob));

    const std::vector<std::byte> empty;
    CHECK(ContentAddress(empty) == p2pgpu::worker::Blake3Hex(empty));
}

TEST_CASE("an address must be exactly 64 lowercase hex characters",
          "[asset][hostile]") {
    using p2pgpu::coordinator::AssetStore;
    const std::string good(64, 'a');
    CHECK(AssetStore::IsWellFormedAddress(good));

    CHECK_FALSE(AssetStore::IsWellFormedAddress(""));
    CHECK_FALSE(AssetStore::IsWellFormedAddress(std::string(63, 'a')));
    CHECK_FALSE(AssetStore::IsWellFormedAddress(std::string(65, 'a')));
    CHECK_FALSE(AssetStore::IsWellFormedAddress(std::string(64, 'g')));
    // Uppercase is REJECTED deliberately: one function emits addresses and it
    // emits lowercase, so accepting both spellings would create two names for
    // one asset — two cache entries per worker and a 5.18 hit rate measuring
    // the wrong thing.
    CHECK_FALSE(AssetStore::IsWellFormedAddress(std::string(64, 'A')));
    // Path traversal is not expressible, and cannot become expressible: the
    // store is in-memory and the key never reaches a filesystem.
    CHECK_FALSE(AssetStore::IsWellFormedAddress("../../etc/passwd"));
    CHECK_FALSE(AssetStore::IsWellFormedAddress(std::string(64, '\0')));
}

TEST_CASE("the store round-trips a blob and is idempotent", "[asset]") {
    p2pgpu::coordinator::AssetStore store;
    const auto blob = GoodBlob();
    const std::string address = store.Put(blob);

    CHECK(address == ContentAddress(blob));
    REQUIRE(store.Find(address) != nullptr);
    CHECK(*store.Find(address) == blob);
    CHECK(store.count() == 1);

    // Same bytes, same address, one entry — the defining property of content
    // addressing rather than a collision.
    CHECK(store.Put(blob) == address);
    CHECK(store.count() == 1);

    CHECK(store.Find(std::string(64, 'b')) == nullptr);
    CHECK(store.Find("not-an-address") == nullptr);
}

TEST_CASE("the worker cache REJECTS bytes that do not hash to their name",
          "[asset][hostile]") {
    // The check the whole class exists for, and the one thing standing between
    // a hostile Phase 6 peer and the GPU traversal loop.
    using p2pgpu::worker::AssetCache;
    auto blob = GoodBlob();
    const std::string address = ContentAddress(blob);

    int fetches = 0;
    auto corrupt = blob;
    corrupt[corrupt.size() / 2] = static_cast<std::byte>(
        static_cast<unsigned char>(corrupt[corrupt.size() / 2]) ^ 0x01U);

    AssetCache cache(
        [&](std::string_view) -> std::optional<std::vector<std::byte>> {
            ++fetches;
            return corrupt;
        },
        1U << 20);

    CHECK(cache.Get(address) == nullptr);
    CHECK(cache.rejected_count() == 1);
    CHECK(cache.resident_bytes() == 0);
    CHECK_FALSE(cache.Has(address));
    // A single flipped bit, in a blob that is otherwise a perfectly valid BVH:
    // `LoadBvh` would have accepted it, because it is structurally sound and
    // only the GEOMETRY is wrong. Verification is what catches that class, and
    // structural validation cannot.
    CHECK(LoadBvh(corrupt).has_value());
}

TEST_CASE("the worker cache accepts, caches, and does not refetch", "[asset]") {
    using p2pgpu::worker::AssetCache;
    const auto blob = GoodBlob();
    const std::string address = ContentAddress(blob);

    int fetches = 0;
    AssetCache cache(
        [&](std::string_view) -> std::optional<std::vector<std::byte>> {
            ++fetches;
            return blob;
        },
        1U << 20);

    const auto* first = cache.Get(address);
    REQUIRE(first != nullptr);
    CHECK(*first == blob);
    CHECK(cache.Has(address));
    CHECK(cache.rejected_count() == 0);
    CHECK(cache.cached() == std::vector<std::string>{address});

    CHECK(cache.Get(address) != nullptr);
    CHECK(fetches == 1);  // served from cache the second time

    cache.Clear();
    CHECK_FALSE(cache.Has(address));
    CHECK(cache.resident_bytes() == 0);
}

TEST_CASE("the cache bounds its memory against an unbounded asset namer",
          "[asset][hostile]") {
    // R11: a coordinator or peer that can name unlimited assets must not be
    // able to grow a worker's memory without limit.
    using p2pgpu::worker::AssetCache;
    const auto blob = GoodBlob();
    const std::string address = ContentAddress(blob);

    AssetCache tiny(
        [&](std::string_view) -> std::optional<std::vector<std::byte>> {
            return blob;
        },
        blob.size() / 2);
    CHECK(tiny.Get(address) == nullptr);       // larger than the whole budget
    CHECK(tiny.resident_bytes() == 0);

    AssetCache bounded(
        [&](std::string_view) -> std::optional<std::vector<std::byte>> {
            return blob;
        },
        blob.size() + 8);
    REQUIRE(bounded.Get(address) != nullptr);
    CHECK(bounded.resident_bytes() == blob.size());
}

TEST_CASE("a fetch failure is not a verification failure", "[asset]") {
    // Distinct counters on purpose: "the network is down" and "a peer served
    // corrupt bytes" call for completely different responses in Phase 6, and a
    // single failure count would conflate them.
    using p2pgpu::worker::AssetCache;
    AssetCache cache(
        [](std::string_view) -> std::optional<std::vector<std::byte>> {
            return std::nullopt;
        },
        1U << 20);
    CHECK(cache.Get(std::string(64, 'a')) == nullptr);
    CHECK(cache.rejected_count() == 0);
}
