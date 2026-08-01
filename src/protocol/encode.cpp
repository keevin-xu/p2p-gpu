// Frame encoder. See encode.hpp.

#include "p2pgpu/protocol/encode.hpp"

#include <algorithm>

namespace p2pgpu::protocol {

std::vector<std::byte> EncodeFrame(std::span<const std::byte> envelope,
                                   std::span<const std::byte> payload) {
    Header h;
    h.magic = kFrameMagic;
    h.protocol_ver = kProtocolVersion;
    h.flags = payload.empty()
                  ? 0U
                  : static_cast<std::uint16_t>(FrameFlags::kPayloadFollows);
    h.fb_len = static_cast<std::uint32_t>(envelope.size());
    // Reserved stays zero: SplitFrame rejects non-zero, which is what keeps the
    // field genuinely free for a future extension (D-0027).
    h.reserved = 0;

    std::vector<std::byte> out(kHeaderBytes + envelope.size() + payload.size());
    EncodeHeader(h, std::span<std::byte, kHeaderBytes>(out.data(), kHeaderBytes));
    std::ranges::copy(envelope, out.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
    std::ranges::copy(payload, out.begin() +
                                   static_cast<std::ptrdiff_t>(kHeaderBytes + envelope.size()));
    return out;
}

}  // namespace p2pgpu::protocol
