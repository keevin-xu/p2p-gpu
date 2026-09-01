# Coordinator container — step 7.7.
#
# Two stages: a fat builder with the whole toolchain, and a slim runtime that
# carries the binary and nothing else. The builder is ~2 GB and never ships.
#
# ── WHY THE COORDINATOR IS SMALL ENOUGH TO DO THIS ───────────────────────────
# It links `p2pgpu-coordinator-core`, `p2pgpu-scene-hash`, CLI11 and
# uWebSockets. **No wgpu-native, no libdatachannel** — scheduling and signalling
# need neither a GPU nor a WebRTC stack, because the coordinator relays
# signalling without reading it (D-0085). So the runtime image needs no GPU driver,
# no Vulkan loader, and no X libraries.
#
# ── RELEASE, NEVER SANITIZERS (CONVENTIONS.md §2/§8) ─────────────────────────
# `native-release` carries the hardening flags. A sanitizer build would be
# 2-10x slower and would turn a hostile input into an abort — which on a public
# endpoint is a denial of service handed to anyone who can connect.

# ─────────────────────────────── builder ────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config \
        ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src
# Manifest first, so a source-only change does not re-resolve dependencies.
COPY vcpkg.json CMakePresets.json ./

# ── A SHALLOW CLONE SILENTLY BREAKS THE VERSION PIN ──────────────────────────
# `builtin-baseline` (D-0013) pins the whole dependency graph to one vcpkg
# commit, and vcpkg resolves it by running `git show <sha>:versions/baseline.json`.
# `--depth 1` fetches only the tip, so that commit is not in the clone and EVERY
# dependency fails to resolve — with an error that names the packages
# ("while loading baseline version for spdlog") and never mentions the clone.
#
# Fetching exactly the pinned commit keeps the clone small AND makes the pin
# work. Cloning full history would also work and costs ~1 GB for one file.
#
# The baseline is read FROM the manifest rather than repeated here: a second
# copy of a pin is a second thing to forget to update.
RUN BASELINE="$(python3 -c "import json;print(json.load(open('vcpkg.json'))['builtin-baseline'])")" \
    && git -C "$VCPKG_ROOT" fetch --depth 1 origin "$BASELINE" \
    && echo "vcpkg baseline $BASELINE fetched"
COPY protocol/ protocol/
COPY CMakeLists.txt ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
COPY kernels/ kernels/
COPY tools/ tools/

# COORDINATOR_ONLY skips wgpu-native and the worker targets. Without it, the
# build downloads a GPU DRIVER to compile a server — and fails outright on
# arm64, where no wgpu archive is pinned, for a library the coordinator never
# links. Tests off for the same reason: this image is a server.
# `|| cat` because vcpkg fails with "see /src/build/vcpkg-manifest-install.log"
# and that layer is discarded on failure — the one file that explains the error
# is the one a container build throws away.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON \
        -DVCPKG_MANIFEST_FEATURES=native \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DP2PGPU_BUILD_TESTS=OFF \
        -DP2PGPU_BUILD_FUZZ=OFF \
        -DP2PGPU_COORDINATOR_ONLY=ON \
      || { echo "=== vcpkg-manifest-install.log ==="; \
           cat /src/build/vcpkg-manifest-install.log 2>/dev/null | tail -60; \
           exit 1; } \
    && cmake --build build --target coordinator -j"$(nproc)"

# ─────────────────────────────── runtime ────────────────────────────────────
FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Unprivileged. The whole input surface is attacker-controlled (R11), so the
# process that parses it should not be able to write its own binary.
RUN useradd --system --uid 10001 --no-create-home p2pgpu

WORKDIR /app
COPY --from=builder /src/build/coordinator /app/coordinator
# WGSL and the manifest are read at runtime and served to workers over HTTP.
COPY --from=builder /src/kernels/ /app/kernels/
COPY deploy/entrypoint.sh /app/entrypoint.sh
COPY scenes/ /app/scenes/

# SQLite lives on a mounted volume; the image itself stays stateless so a
# redeploy cannot silently discard durable state.
RUN mkdir -p /data && chown p2pgpu:p2pgpu /data
VOLUME ["/data"]

USER p2pgpu
EXPOSE 8080

# The wrapper appends --ice-server flags from $P2PGPU_ICE_SERVERS and then
# `exec`s the coordinator, so it is still PID 1 and still receives SIGTERM
# directly. Set the variable with `flyctl secrets set`; leave it unset and the
# coordinator runs exactly as before.
ENTRYPOINT ["/app/entrypoint.sh"]
# A seeded render, so the deployment has work the moment a worker connects.
#
# `dense.scene` for the relay measurement: a 256 KB BVH, big enough that a
# peer-to-peer transfer is doing visible work. (`default.scene` is ~6 KB, which
# was right for the first real-network test — small enough that a failure could
# not be confused with a timeout — but too small to measure a transport with.)
#
# 262144 spp keeps the render alive long enough for two machines to join,
# fetch, and peer with each other.
#
# ICE SERVERS ARE NOT LISTED HERE. They come from $P2PGPU_ICE_SERVERS via the
# entrypoint, because a Cloudflare TURN credential expires and one baked into a
# layer would outlive its own validity while still looking configured.
CMD ["--port", "8080", \
     "--kernel-dir", "/app/kernels", \
     "--manifest", "/app/kernels/manifest.toml", \
     "--store", "/data/p2pgpu.db", \
     "--seed-render", "/app/scenes/dense.scene", \
     "--render-size", "384x288", \
     "--render-spp", "262144", \
     "--log-level", "info"]
