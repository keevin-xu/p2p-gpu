// p2pgpu coordinator.
//
// The only decision-maker in the system (R1): scheduling, sizing, validation,
// reputation, retry. Workers execute and report facts.
//
// Two rules that bite early:
//   - No exceptions, no crash paths on worker input. A hostile worker taking
//     down the coordinator is total system failure (CONVENTIONS.md §1, R11).
//   - Never trust worker clocks or TaskStats. Lease expiry is decided here
//     (PROTOCOL.md §5); stats are telemetry only (invariant 8).
//
// Steps 1.11-1.12: server skeleton + kernel registry.

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>

#include "p2pgpu/coordinator/net.hpp"

int main(int argc, char** argv) {
    p2pgpu::coordinator::Config cfg;

    CLI::App app{"p2pgpu coordinator"};
    app.add_option("--port", cfg.port, "WebSocket / HTTP port")->capture_default_str();
    app.add_option("--manifest", cfg.manifest, "kernel manifest")->capture_default_str();
    app.add_option("--kernel-dir", cfg.kernel_dir, "WGSL directory")->capture_default_str();
    app.add_option("--log-level", cfg.log_level,
                   "trace|debug|info|warn|error")->capture_default_str();
    // Exceptions are permitted in startup/config code, before serving begins
    // (CONVENTIONS.md §1). CLI11 throws for --help and parse errors.
    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(spdlog::level::from_str(cfg.log_level));
    // Correlation fields go in the message body (CONVENTIONS.md §6); the JSON
    // sink for production arrives with deployment in Phase 7.
    spdlog::set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%^%l%$] %v");

    // Load the registry BEFORE binding a port. A coordinator that accepts
    // workers and then discovers it has no kernels to give them is worse than
    // one that refuses to start — the failure would surface as UnknownKernel
    // errors at the far end, nowhere near the cause.
    const auto registry = p2pgpu::coordinator::KernelRegistry::Load(
        std::filesystem::path(cfg.manifest), std::filesystem::path(cfg.kernel_dir));
    if (!registry) {
        spdlog::error("kernel registry failed to load: {}", registry.error().message);
        return 1;
    }

    for (const auto* spec : registry->All()) {
        spdlog::info("kernel id={} entry={} wg=[{},{},{}] out={}B det={} r5_ratio={:.3g}",
                     spec->id, spec->entry_point, spec->workgroup_size[0],
                     spec->workgroup_size[1], spec->workgroup_size[2],
                     spec->output_bytes, static_cast<int>(spec->determinism),
                     spec->r5_ratio);
    }

    // One JobManager for the process, owned here and outliving the server.
    // Empty at startup: jobs arrive over the control API in Phase 2 (2.16).
    p2pgpu::coordinator::JobManager jobs;

    p2pgpu::coordinator::Server server(cfg, *registry, jobs);
    server.Run();
    return 0;
}
