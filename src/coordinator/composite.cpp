// Tile decomposition and compositing — step 5.15.

#include "p2pgpu/coordinator/composite.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace p2pgpu::coordinator {
namespace {

/// sRGB-ish transfer. 1/2.2 rather than the exact piecewise sRGB curve: this is
/// a preview for a dashboard and a demo capture, not a colour-managed pipeline,
/// and the difference is invisible next to Monte Carlo noise.
float EncodeGamma(float linear) {
    if (!(linear > 0.0F)) {
        return 0.0F;   // also catches NaN, which `<= 0` would not
    }
    return std::pow(std::min(linear, 1.0F), 1.0F / 2.2F);
}

}  // namespace

std::optional<TileGrid::Tile> TileGrid::TileAt(std::uint32_t index) const noexcept {
    if (tile_w == 0 || tile_h == 0 || index >= tile_count()) {
        return std::nullopt;
    }
    const std::uint32_t tx = index % tiles_x();
    const std::uint32_t ty = index / tiles_x();
    Tile t;
    t.x = tx * tile_w;
    t.y = ty * tile_h;
    // CLIPPED, not overhanging. An image 100 wide in 64-wide tiles has a second
    // column 36 wide; rounding it up to 64 would composite 28 columns of pixels
    // that do not exist.
    t.w = std::min(tile_w, image_w - t.x);
    t.h = std::min(tile_h, image_h - t.y);
    return t;
}

bool Compositor::AcceptTile(std::uint32_t index, std::span<const std::byte> payload) {
    const auto tile = grid_.TileAt(index);
    if (!tile) {
        return false;
    }
    constexpr std::size_t kStride = 16;   // {r,g,b,samples} f32
    const std::size_t expected = static_cast<std::size_t>(tile->pixels()) * kStride;
    if (payload.size() != expected) {
        // "The kernel always sends that size" is how a buffer overrun gets
        // written. This is post-validation data and the check is still here.
        return false;
    }

    TileData data;
    data.rgb.resize(static_cast<std::size_t>(tile->pixels()) * 3);
    double samples = 0.0;
    bool samples_set = false;

    for (std::uint32_t p = 0; p < tile->pixels(); ++p) {
        std::array<float, 4> px{};
        for (std::size_t k = 0; k < 4; ++k) {
            std::array<std::byte, 4> raw{};
            for (std::size_t j = 0; j < 4; ++j) {
                raw[j] = payload[p * kStride + k * 4 + j];
            }
            px[k] = std::bit_cast<float>(raw);
        }
        for (std::size_t k = 0; k < 3; ++k) {
            data.rgb[p * 3 + k] = std::isfinite(px[k]) ? px[k] : 0.0F;
        }
        // Every pixel of a tile is rendered by the same dispatches, so the
        // sample count is uniform across it. Taking the FIRST rather than the
        // max or the mean: a payload where they disagree is malformed, and
        // silently averaging would hide that.
        if (!samples_set) {
            samples = std::isfinite(px[3]) && px[3] > 0.0F ? px[3] : 0.0;
            samples_set = true;
        }
    }
    data.samples = samples;

    // REPLACE, never merge. A worker's upload already contains everything it has
    // computed for this tile (D-0074), so adding two of them double-counts.
    tiles_[index] = std::move(data);
    return true;
}

double Compositor::TileSamples(std::uint32_t index) const {
    const auto it = tiles_.find(index);
    return it == tiles_.end() ? 0.0 : it->second.samples;
}

double Compositor::MinSamples() const {
    if (grid_.tile_count() == 0) {
        return 0.0;
    }
    // Counts tiles that have NEVER been rendered as 0 rather than skipping
    // them. Skipping would report a fully-converged minimum while most of the
    // image was still blank — and 5.17 schedules on this number.
    double lowest = std::numeric_limits<double>::infinity();
    for (std::uint32_t i = 0; i < grid_.tile_count(); ++i) {
        lowest = std::min(lowest, TileSamples(i));
    }
    return std::isfinite(lowest) ? lowest : 0.0;
}

double Compositor::MaxSamples() const {
    double highest = 0.0;
    for (const auto& [index, data] : tiles_) {
        highest = std::max(highest, data.samples);
    }
    return highest;
}

std::uint32_t Compositor::LeastSampledTile() const {
    std::uint32_t best = 0;
    double best_samples = std::numeric_limits<double>::infinity();
    for (std::uint32_t i = 0; i < grid_.tile_count(); ++i) {
        const double s = TileSamples(i);
        // Strictly less, so ties keep the LOWEST index — the choice has to be
        // deterministic or an experiment cannot be replayed (2.3's rule).
        if (s < best_samples) {
            best_samples = s;
            best = i;
        }
    }
    return best;
}

std::vector<std::uint8_t> Compositor::RenderRgba() const {
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(grid_.image_w) * grid_.image_h * 4, 0);
    if (out.empty()) {
        return out;
    }
    // Opaque everywhere, including tiles that have not been rendered. They come
    // out black, which is honestly what "no samples yet" looks like; a
    // transparent hole would let the page background show through and read as a
    // rendered white patch.
    for (std::size_t i = 3; i < out.size(); i += 4) {
        out[i] = 255;
    }

    for (std::uint32_t index = 0; index < grid_.tile_count(); ++index) {
        const auto it = tiles_.find(index);
        if (it == tiles_.end() || !(it->second.samples > 0.0)) {
            continue;
        }
        const auto tile = grid_.TileAt(index);
        if (!tile) {
            continue;
        }
        const double inv = 1.0 / it->second.samples;
        for (std::uint32_t ty = 0; ty < tile->h; ++ty) {
            for (std::uint32_t tx = 0; tx < tile->w; ++tx) {
                const std::size_t src = (static_cast<std::size_t>(ty) * tile->w + tx) * 3;
                const std::size_t dst =
                    ((static_cast<std::size_t>(tile->y + ty) * grid_.image_w) +
                     (tile->x + tx)) * 4;
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto mean =
                        static_cast<float>(it->second.rgb[src + k] * inv) * exposure_;
                    out[dst + k] = static_cast<std::uint8_t>(
                        std::lround(EncodeGamma(mean) * 255.0F));
                }
            }
        }
    }
    return out;
}

}  // namespace p2pgpu::coordinator
