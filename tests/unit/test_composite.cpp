// Tile decomposition and compositing — step 5.15.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <array>
#include <limits>
#include <set>
#include <vector>

#include "p2pgpu/coordinator/composite.hpp"

using namespace p2pgpu::coordinator;

namespace {

/// A tile accumulator: `samples` samples of constant radiance `value`.
std::vector<std::byte> Tile(std::uint32_t pixels, float value, float samples) {
    std::vector<std::byte> out(static_cast<std::size_t>(pixels) * 16);
    for (std::uint32_t p = 0; p < pixels; ++p) {
        const float px[4]{value * samples, value * samples, value * samples, samples};
        std::memcpy(out.data() + p * 16, px, 16);
    }
    return out;
}

}  // namespace

TEST_CASE("the decomposition is a PARTITION: every pixel in exactly one tile",
          "[composite]") {
    // E3 audited the keyspace for gaps and overlaps against the database rather
    // than trusting a log line. Same question here, and it has a visible answer
    // — a gap is a black stripe — but only if someone looks at the picture.
    for (const auto& [w, h, tw, th] : std::vector<std::array<std::uint32_t, 4>>{
             {64, 64, 64, 64},      // exactly one tile
             {128, 128, 64, 64},    // exact multiple
             {100, 70, 64, 64},     // BOTH dimensions ragged
             {65, 65, 64, 64},      // one-pixel overhang
             {7, 3, 64, 64},        // image smaller than a tile
             {192, 64, 32, 16},     // non-square tiles
         }) {
        const TileGrid grid{w, h, tw, th};
        INFO(w << "x" << h << " in " << tw << "x" << th);

        std::set<std::pair<std::uint32_t, std::uint32_t>> covered;
        std::uint64_t counted = 0;
        for (std::uint32_t i = 0; i < grid.tile_count(); ++i) {
            const auto t = grid.TileAt(i);
            REQUIRE(t.has_value());
            // No tile may extend past the image.
            CHECK(t->x + t->w <= w);
            CHECK(t->y + t->h <= h);
            CHECK(t->w > 0);
            CHECK(t->h > 0);
            for (std::uint32_t y = 0; y < t->h; ++y) {
                for (std::uint32_t x = 0; x < t->w; ++x) {
                    covered.insert({t->x + x, t->y + y});
                    ++counted;
                }
            }
        }
        // Set size == count proves NO OVERLAP; both equal to w*h proves NO GAP.
        // Either check alone would pass one of the two failures.
        CHECK(covered.size() == static_cast<std::size_t>(w) * h);
        CHECK(counted == static_cast<std::uint64_t>(w) * h);
    }
}

TEST_CASE("an out-of-range tile index is refused", "[composite]") {
    const TileGrid grid{128, 128, 64, 64};
    CHECK(grid.tile_count() == 4);
    CHECK_FALSE(grid.TileAt(4).has_value());
    CHECK_FALSE(grid.TileAt(0xFFFFFFFF).has_value());

    Compositor comp(grid);
    CHECK_FALSE(comp.AcceptTile(4, Tile(64 * 64, 0.5F, 16.0F)));
}

TEST_CASE("a payload of the wrong size is refused", "[composite]") {
    // Post-validation data, and the check is still here: "the kernel always
    // sends that size" is how a buffer overrun gets written.
    const TileGrid grid{64, 64, 64, 64};
    Compositor comp(grid);
    CHECK_FALSE(comp.AcceptTile(0, Tile(64 * 64 - 1, 0.5F, 16.0F)));
    CHECK_FALSE(comp.AcceptTile(0, Tile(64 * 64 + 1, 0.5F, 16.0F)));
    CHECK(comp.AcceptTile(0, Tile(64 * 64, 0.5F, 16.0F)));
}

TEST_CASE("an edge tile's payload is sized to the CLIPPED tile", "[composite]") {
    // 100x70 in 64x64 tiles: the right column is 36 wide, the bottom row 6
    // tall. Accepting a full-size payload here would composite pixels that do
    // not exist.
    const TileGrid grid{100, 70, 64, 64};
    REQUIRE(grid.tile_count() == 4);
    const auto corner = grid.TileAt(3);
    REQUIRE(corner.has_value());
    CHECK(corner->w == 36);
    CHECK(corner->h == 6);

    Compositor comp(grid);
    CHECK_FALSE(comp.AcceptTile(3, Tile(64 * 64, 0.5F, 16.0F)));
    CHECK(comp.AcceptTile(3, Tile(36 * 6, 0.5F, 16.0F)));
}

TEST_CASE("compositing normalizes by sample count", "[composite]") {
    // The property that makes partial results mergeable at all: a tile with
    // 4 samples and one with 400 must render to the SAME colour if the
    // underlying radiance is the same. A worker that pre-averaged would have
    // destroyed the weight this relies on (D-0074).
    const TileGrid grid{64, 64, 32, 32};
    Compositor comp(grid);
    REQUIRE(comp.AcceptTile(0, Tile(32 * 32, 0.5F, 4.0F)));
    REQUIRE(comp.AcceptTile(1, Tile(32 * 32, 0.5F, 400.0F)));

    const auto img = comp.RenderRgba();
    REQUIRE(img.size() == 64ULL * 64 * 4);
    const auto at = [&](std::uint32_t x, std::uint32_t y, std::size_t k) {
        return img[((static_cast<std::size_t>(y) * 64) + x) * 4 + k];
    };
    CHECK(at(0, 0, 0) == at(32, 0, 0));
    CHECK(at(0, 0, 0) > 0);
}

TEST_CASE("gamma is applied, so mid-grey is not near-black", "[composite]") {
    // Linear 0.5 written straight into 8 bits is 128; gamma-encoded it is ~186.
    // Without the transfer function the whole image looks almost black, which
    // reads as a broken renderer and sends someone into the kernel.
    const TileGrid grid{16, 16, 16, 16};
    Compositor comp(grid);
    REQUIRE(comp.AcceptTile(0, Tile(16 * 16, 0.5F, 8.0F)));
    const auto img = comp.RenderRgba();
    CHECK(img[0] > 180);
    CHECK(img[0] < 195);
}

TEST_CASE("an unrendered tile is opaque black, not transparent", "[composite]") {
    // Transparent would let the page background show through and read as a
    // rendered white patch. Black is honestly what "no samples yet" looks like.
    const TileGrid grid{32, 32, 16, 16};
    Compositor comp(grid);
    REQUIRE(comp.AcceptTile(0, Tile(16 * 16, 1.0F, 8.0F)));
    const auto img = comp.RenderRgba();
    CHECK(img[3] == 255);              // rendered pixel, opaque
    const std::size_t blank = ((0ULL * 32) + 16) * 4;
    CHECK(img[blank + 0] == 0);
    CHECK(img[blank + 3] == 255);      // unrendered pixel, still opaque
}

TEST_CASE("a later upload REPLACES rather than accumulating", "[composite]") {
    // A worker's upload already contains everything it has computed for the
    // tile (D-0074). Adding two of them double-counts.
    const TileGrid grid{16, 16, 16, 16};
    Compositor comp(grid);
    REQUIRE(comp.AcceptTile(0, Tile(16 * 16, 0.25F, 10.0F)));
    CHECK(comp.TileSamples(0) == 10.0);
    REQUIRE(comp.AcceptTile(0, Tile(16 * 16, 0.25F, 50.0F)));
    CHECK(comp.TileSamples(0) == 50.0);   // not 60
}

TEST_CASE("sample tracking drives the least-sampled choice (5.17)",
          "[composite]") {
    const TileGrid grid{64, 64, 32, 32};
    Compositor comp(grid);
    CHECK(comp.MinSamples() == 0.0);
    CHECK(comp.LeastSampledTile() == 0);   // nothing rendered: lowest index

    REQUIRE(comp.AcceptTile(0, Tile(32 * 32, 0.5F, 100.0F)));
    REQUIRE(comp.AcceptTile(1, Tile(32 * 32, 0.5F, 50.0F)));
    REQUIRE(comp.AcceptTile(2, Tile(32 * 32, 0.5F, 75.0F)));

    // MinSamples counts tile 3 — never rendered — as 0. Skipping it would
    // report a converged minimum while a quarter of the image was blank, and
    // 5.17 schedules on this number.
    CHECK(comp.MinSamples() == 0.0);
    CHECK(comp.MaxSamples() == 100.0);
    CHECK(comp.LeastSampledTile() == 3);

    REQUIRE(comp.AcceptTile(3, Tile(32 * 32, 0.5F, 200.0F)));
    CHECK(comp.MinSamples() == 50.0);
    CHECK(comp.LeastSampledTile() == 1);
}

TEST_CASE("non-finite radiance is zeroed rather than propagated", "[composite]") {
    const TileGrid grid{16, 16, 16, 16};
    Compositor comp(grid);
    auto payload = Tile(16 * 16, 0.5F, 8.0F);
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(payload.data(), &nan_value, 4);
    REQUIRE(comp.AcceptTile(0, payload));
    const auto img = comp.RenderRgba();
    CHECK(img[0] == 0);        // the NaN channel
    CHECK(img[1] > 180);       // its neighbours are unaffected
}
