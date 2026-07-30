// Frame header encode/decode. See docs/PROTOCOL.md §1.
// R11 applies: no pointer arithmetic, no memcpy. std::span only.
// Implement in Phase 1 step 1.2.
#include "p2pgpu/protocol/limits.hpp"
