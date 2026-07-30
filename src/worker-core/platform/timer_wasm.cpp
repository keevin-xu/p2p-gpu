// PLATFORM SEAM (browser). Clock, and Yield via the Emscripten main loop.
// NEVER busy-wait here — it freezes the tab.
#include "p2pgpu/worker/platform.hpp"
