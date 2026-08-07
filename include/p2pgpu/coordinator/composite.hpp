#pragma once
//
// Tile decomposition and image compositing — step 5.15.
//
// ── THE DECOMPOSITION IS A PARTITION, AND THAT IS CHECKED ────────────────
// Every pixel of the image belongs to EXACTLY ONE tile. E3 audited the
// keyspace the same way and found it contiguous with no gaps and no overlaps;
// the same question here has a visible answer — a gap is a black stripe and an
// overlap is a tile rendered twice — but only if someone looks at the picture.
// `tests/unit/test_composite.cpp` asserts it instead.
//
// Edge tiles are SMALLER when the image is not a multiple of the tile size.
// Rounding up and letting the last tile overhang would make the coordinator
// composite pixels that do not exist, and the kernel already takes tile_w/tile_h
// per task precisely so it can render a partial tile.
//
// ── THE COORDINATOR OWNS THE IMAGE, THE WORKERS OWN NONE OF IT ───────────
// A worker returns an accumulator for one tile: a running SUM of radiance plus
// the sample count that produced it (D-0074). Normalisation, tone mapping and
// assembly all happen here (R1). A worker that pre-divided would destroy the
// weight needed to merge its tile with a longer render of the same tile.

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace p2pgpu::coordinator {

/// The image, cut into tiles. Pure geometry — no state, no pixels.
struct TileGrid {
    std::uint32_t image_w = 0;
    std::uint32_t image_h = 0;
    std::uint32_t tile_w = 64;
    std::uint32_t tile_h = 64;

    [[nodiscard]] std::uint32_t tiles_x() const noexcept {
        return tile_w == 0 ? 0 : (image_w + tile_w - 1) / tile_w;
    }
    [[nodiscard]] std::uint32_t tiles_y() const noexcept {
        return tile_h == 0 ? 0 : (image_h + tile_h - 1) / tile_h;
    }
    [[nodiscard]] std::uint32_t tile_count() const noexcept {
        return tiles_x() * tiles_y();
    }

    /// Where a tile starts and how big it actually is. The last column and row
    /// are CLIPPED to the image rather than overhanging it.
    struct Tile {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t w = 0;
        std::uint32_t h = 0;
        [[nodiscard]] std::uint32_t pixels() const noexcept { return w * h; }
    };
    [[nodiscard]] std::optional<Tile> TileAt(std::uint32_t index) const noexcept;
};

/// Assembles tiles into an image as results arrive.
///
/// Holds the LATEST accumulator per tile, never a merge of several. A worker's
/// upload already contains everything it has computed for that tile (D-0074), so
/// adding two uploads from the same worker would double-count. Two DIFFERENT
/// workers rendering the same tile is a replication question the validator
/// answers before anything reaches here.
class Compositor {
public:
    explicit Compositor(TileGrid grid) : grid_(grid) {}

    /// Accept an accumulator for a tile.
    ///
    /// `payload` is `{r,g,b,samples}` f32 quadruples, `tile.pixels()` of them.
    /// Returns false and stores nothing if the tile index is out of range or
    /// the payload is the wrong size — this is post-validation data, but a
    /// size assumption that holds "because the kernel always sends that" is how
    /// a buffer overrun gets written.
    [[nodiscard]] bool AcceptTile(std::uint32_t index,
                                  std::span<const std::byte> payload);

    /// Samples accumulated in a tile, 0 if nothing has arrived. **This is what
    /// 5.17 schedules on** — prefer the tile with the fewest, so the image
    /// converges evenly rather than in patches.
    [[nodiscard]] double TileSamples(std::uint32_t index) const;

    /// Fewest and most samples across ALL tiles, counting never-rendered ones
    /// as 0. The gap between them is how uneven the image currently is.
    [[nodiscard]] double MinSamples() const;
    [[nodiscard]] double MaxSamples() const;
    [[nodiscard]] std::uint32_t TilesWithData() const noexcept {
        return static_cast<std::uint32_t>(tiles_.size());
    }

    /// The tile with the fewest samples (5.17). Ties break on the lowest index
    /// so the choice is deterministic and an experiment can be replayed.
    [[nodiscard]] std::uint32_t LeastSampledTile() const;

    /// Composite to 8-bit RGBA, row-major from the top-left.
    ///
    /// GAMMA IS APPLIED HERE. Radiance accumulates linearly, and writing linear
    /// values into an 8-bit buffer produces an image that looks almost black —
    /// which reads as a broken renderer rather than as a missing transfer
    /// function, and would have sent someone hunting through the kernel.
    [[nodiscard]] std::vector<std::uint8_t> RenderRgba() const;

    /// Linear exposure multiplier applied before gamma. 1.0 is "show the
    /// radiance as computed".
    ///
    /// An EXPLICIT knob rather than an automatic tone map, because auto-exposure
    /// would make the preview's brightness depend on which tiles have arrived —
    /// the image would breathe as it converges, and nobody watching could tell
    /// that from the render actually changing. `scenes/default.scene` needs
    /// ~0.45: its sky is near-white, so at 1.0 most of the frame clips and the
    /// picture reads as washed out rather than as correctly bright.
    void SetExposure(float e) noexcept { exposure_ = e > 0.0F ? e : 1.0F; }
    [[nodiscard]] float exposure() const noexcept { return exposure_; }

    [[nodiscard]] const TileGrid& grid() const noexcept { return grid_; }

private:
    struct TileData {
        std::vector<float> rgb;   ///< 3 per pixel, the running SUM
        double samples = 0.0;
    };
    TileGrid grid_;
    float exposure_ = 1.0F;
    std::unordered_map<std::uint32_t, TileData> tiles_;
};

}  // namespace p2pgpu::coordinator
