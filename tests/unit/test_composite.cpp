// Tile decomposition and compositing — step 5.15.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <array>
#include <limits>
#include <algorithm>
#include <map>
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

// ─────────────────────────────────────────────────────────────────────────
// 5.17 — sample-budget scheduling, in the JobManager
// ─────────────────────────────────────────────────────────────────────────

#include "p2pgpu/coordinator/job.hpp"

namespace {

/// A render job of `tiles` tiles, `spp` samples each.
JobManager RenderJob(std::uint32_t tiles_x, std::uint32_t tiles_y,
                     std::uint64_t spp, JobId& out) {
    JobManager jobs;
    const TileGrid grid{tiles_x * 64, tiles_y * 64, 64, 64};
    out = jobs.CreateJob("pathtrace_tile_v1", grid.tile_count() * spp, 42, spp);
    auto* job = jobs.MutableJob(out);
    RenderConfig rc;
    rc.grid = grid;
    rc.samples_per_tile = spp;
    job->render = rc;
    job->tile_granted.assign(grid.tile_count(), 0);
    return jobs;
}

}  // namespace

TEST_CASE("grants go to the LEAST-granted tile, round-robin at equal counts",
          "[composite][schedule]") {
    JobId id;
    auto jobs = RenderJob(2, 2, 1000, id);   // 4 tiles, 1000 spp each

    // Tasks of 250 samples: four grants should touch four DIFFERENT tiles
    // rather than filling tile 0 first, which is what a cursor would do.
    std::vector<std::uint64_t> tiles;
    for (int i = 0; i < 4; ++i) {
        const auto t = jobs.Grant(WorkerId{1, static_cast<std::uint64_t>(i + 1)},
                                  1000, 5000, 250, {});
        REQUIRE(t.has_value());
        tiles.push_back(t->start_unit / 1000);
    }
    std::ranges::sort(tiles);
    CHECK(tiles == std::vector<std::uint64_t>{0, 1, 2, 3});
}

TEST_CASE("an in-flight grant is visible to the scheduler", "[composite][schedule]") {
    // THE POINT OF COUNTING AT THE GRANT (D-0078). If the counter only moved on
    // acceptance, every idle worker asking at once would be handed tile 0 —
    // nine of ten redundantly — and the fleet would oscillate tile by tile.
    JobId id;
    auto jobs = RenderJob(2, 1, 1000, id);   // 2 tiles

    const auto a = jobs.Grant(WorkerId{1, 1}, 1000, 5000, 500, {});
    const auto b = jobs.Grant(WorkerId{1, 2}, 1000, 5000, 500, {});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    // Nothing has been ACCEPTED, yet the second grant avoids the first's tile.
    CHECK(a->start_unit / 1000 != b->start_unit / 1000);
}

TEST_CASE("a task never spans two tiles", "[composite][schedule]") {
    // A task carries ONE set of tile coordinates. Spanning two would render
    // half its range into the wrong place and still produce a well-formed
    // accumulator of the right size — undetectable downstream (the D-0040
    // shape).
    JobId id;
    auto jobs = RenderJob(2, 2, 100, id);

    for (int i = 0; i < 8; ++i) {
        // Ask for far more than a tile holds.
        const auto t = jobs.Grant(WorkerId{1, static_cast<std::uint64_t>(i + 1)},
                                  1000, 5000, 10000, {});
        if (!t) { break; }
        const std::uint64_t first = t->start_unit / 100;
        const std::uint64_t last = (t->start_unit + t->unit_count - 1) / 100;
        INFO("task " << i << " start=" << t->start_unit << " count=" << t->unit_count);
        CHECK(first == last);
        CHECK(t->unit_count <= 100);
    }
}

TEST_CASE("the whole sample budget is carved exactly once", "[composite][schedule]") {
    // The E3 audit question, applied to a render: no gaps (a tile short of
    // samples renders noisier than asked) and no overlaps (a tile double-counted
    // renders brighter, because the compositor divides by the count it is told).
    JobId id;
    auto jobs = RenderJob(2, 2, 300, id);

    std::map<std::uint64_t, std::uint64_t> per_tile;
    std::uint64_t total = 0;
    for (int i = 0; i < 200; ++i) {
        const auto t = jobs.Grant(WorkerId{1, static_cast<std::uint64_t>(i + 1)},
                                  1000, 5000, 70, {});
        if (!t) { break; }
        per_tile[t->start_unit / 300] += t->unit_count;
        total += t->unit_count;
    }
    CHECK(total == 4 * 300);
    REQUIRE(per_tile.size() == 4);
    for (const auto& [tile, samples] : per_tile) {
        INFO("tile " << tile);
        CHECK(samples == 300);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 5.18 — cache affinity, finally exercisable now that assets exist
// ─────────────────────────────────────────────────────────────────────────

namespace {

AssetId MakeAsset(std::uint8_t fill) {
    AssetId id{};
    id.fill(static_cast<std::byte>(fill));
    return id;
}

}  // namespace

TEST_CASE("a fresh carve prefers a job whose asset the worker holds",
          "[affinity]") {
    // 2.16's affinity has been WIRED AND NEVER TAKEN since Phase 2 (D-0047),
    // because `cached_assets` was always empty. This is the first check that
    // could have failed.
    //
    // EIGHT jobs, and the assertion is made once per job. The first version
    // used two, and it PASSED with the affinity pass stubbed out — `jobs_` is
    // an unordered_map, so the wanted job was simply first often enough. A
    // coin-flip test is not a test; asking all eight makes passing by luck a
    // 1-in-8^8 event.
    constexpr int kJobs = 8;
    for (int wanted = 0; wanted < kJobs; ++wanted) {
        JobManager jobs;
        std::vector<JobId> ids;
        for (int i = 0; i < kJobs; ++i) {
            const auto id = jobs.CreateJob("k", 1000, static_cast<std::uint64_t>(i));
            jobs.MutableJob(id)->input_ref = MakeAsset(static_cast<std::uint8_t>(i));
            ids.push_back(id);
        }
        const std::vector<AssetId> holds{MakeAsset(static_cast<std::uint8_t>(wanted))};
        const auto t = jobs.Grant(WorkerId{1, 1}, 1000, 5000, 100, holds);
        REQUIRE(t.has_value());
        INFO("wanted job index " << wanted);
        CHECK(t->job == ids[static_cast<std::size_t>(wanted)]);
        CHECK(jobs.asset_grants_hit() == 1);
    }
}

TEST_CASE("no affinity NEVER means no work", "[affinity]") {
    // The D-0047 failure in a new place: a preferring pass that does not fall
    // through starves every worker holding nothing, which is every worker at
    // the start of a render.
    JobManager jobs;
    const auto job = jobs.CreateJob("k", 1000, 1);
    jobs.MutableJob(job)->input_ref = MakeAsset(0xAA);

    const auto t = jobs.Grant(WorkerId{1, 1}, 1000, 5000, 100, {});
    REQUIRE(t.has_value());
    CHECK(t->job == job);
    // Counted as a MISS, which is what makes the rate meaningful.
    CHECK(jobs.asset_grants() == 1);
    CHECK(jobs.asset_grants_hit() == 0);
}

TEST_CASE("a worker holding an unrelated asset still gets work", "[affinity]") {
    JobManager jobs;
    const auto job = jobs.CreateJob("k", 1000, 1);
    jobs.MutableJob(job)->input_ref = MakeAsset(0xAA);

    const std::vector<AssetId> unrelated{MakeAsset(0x11)};
    const auto t = jobs.Grant(WorkerId{1, 1}, 1000, 5000, 100, unrelated);
    REQUIRE(t.has_value());
    CHECK(jobs.asset_grants_hit() == 0);
}

TEST_CASE("assetless grants are not counted as affinity misses", "[affinity]") {
    // The denominator is "grants that COULD have hit". Counting plain keyspace
    // work as a miss would make the rate fall whenever the fleet does anything
    // other than render — a number that moves for reasons unrelated to what it
    // claims to measure.
    JobManager jobs;
    (void)jobs.CreateJob("k", 1000, 1);   // no input_ref
    const auto t = jobs.Grant(WorkerId{1, 1}, 1000, 5000, 100, {});
    REQUIRE(t.has_value());
    CHECK(jobs.asset_grants() == 0);
    CHECK(jobs.asset_grants_hit() == 0);
}

TEST_CASE("affinity holds across repeated grants to the same worker",
          "[affinity]") {
    // The steady state a render actually runs in: once a worker has fetched the
    // asset, every subsequent grant should hit.
    JobManager jobs;
    const auto job = jobs.CreateJob("k", 10000, 1);
    jobs.MutableJob(job)->input_ref = MakeAsset(0xAA);
    const std::vector<AssetId> holds{MakeAsset(0xAA)};

    for (int i = 0; i < 10; ++i) {
        const auto t = jobs.Grant(WorkerId{1, static_cast<std::uint64_t>(i + 1)},
                                  1000, 5000, 100, holds);
        REQUIRE(t.has_value());
    }
    CHECK(jobs.asset_grants() == 10);
    CHECK(jobs.asset_grants_hit() == 10);
}
