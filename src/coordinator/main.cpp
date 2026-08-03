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
#include <functional>

#include "p2pgpu/coordinator/net.hpp"

int main(int argc, char** argv) {
    p2pgpu::coordinator::Config cfg;

    CLI::App app{"p2pgpu coordinator"};
    app.add_option("--port", cfg.port, "WebSocket / HTTP port")->capture_default_str();
    app.add_option("--manifest", cfg.manifest, "kernel manifest")->capture_default_str();
    app.add_option("--kernel-dir", cfg.kernel_dir, "WGSL directory")->capture_default_str();
    app.add_option("--log-level", cfg.log_level,
                   "trace|debug|info|warn|error")->capture_default_str();

    // DEVELOPMENT AFFORDANCE, replaced by the job submission API in step 2.16.
    // Phase 1's objective is "one worker completes a real job end to end", and
    // without this there is no way to put a job in the queue at all. Kept
    // deliberately crude so nobody mistakes it for the real interface.
    std::string seed_kernel;
    std::uint64_t seed_units = 4'000'000;
    std::uint32_t seed_tasks = 4;
    app.add_option("--seed-job", seed_kernel,
                   "DEV ONLY: queue one job for this kernel id at startup");
    app.add_option("--seed-units", seed_units, "units in the seeded job")
        ->capture_default_str();
    app.add_option("--seed-tasks", seed_tasks, "tasks to split the seeded job into")
        ->capture_default_str();

    // DEV ONLY, step 1.26. Recomputes every accepted result on the CPU and
    // compares. A TEST HARNESS, NOT VALIDATION — if the coordinator could
    // afford to compute every answer there would be no reason to distribute
    // the work at all. Affordable only with the tiny tasks 1.26 uses; at the
    // sizer's real operating point it would take longer than the whole grid.
    // Real validation is replication + reputation, in Phase 3.
    bool verify_reference = false;
    app.add_flag("--verify-reference", verify_reference,
                 "DEV ONLY: recompute each result on the CPU and compare "
                 "(a test harness, never a validation strategy)");
    app.add_flag("--exit-when-complete", cfg.exit_when_complete,
                 "DEV ONLY: stop once every seeded task is terminal");
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
    // Empty at startup unless --seed-job is given; the real submission API is
    // step 2.16.
    p2pgpu::coordinator::JobManager jobs;

    if (!seed_kernel.empty()) {
        if (registry->Find(seed_kernel) == nullptr) {
            // Refuse rather than queue work no worker can run — the failure
            // would otherwise surface as every worker releasing every task with
            // KernelUnavailable, nowhere near the cause.
            spdlog::error("--seed-job names an unknown kernel: {}", seed_kernel);
            return 1;
        }
        const auto job = jobs.CreateJob(seed_kernel, seed_units, seed_tasks, /*seed=*/42);
        spdlog::warn("DEV: seeded job kernel={} units={} tasks={} job_lo={}",
                     seed_kernel, seed_units, seed_tasks, job.lo());
    }

    p2pgpu::coordinator::ReferenceStats ref_stats;
    if (verify_reference) {
        spdlog::warn("DEV: --verify-reference is ON. This recomputes every result on "
                     "the CPU. It is a TEST HARNESS, not validation — never quote it "
                     "as evidence that results are validated (Phase 3 does that).");
    }

    p2pgpu::coordinator::Server server(cfg, *registry, jobs,
                                       verify_reference ? &ref_stats : nullptr);

    // Runs from inside the event loop when every task is terminal, under
    // --exit-when-complete. The summary has to be printed there rather than
    // after Run(), because Run() does not return while a worker is attached.
    const auto report = [&]() -> int {
        if (!verify_reference) {
            return 0;
        }
        // "Nothing was verified" and "everything verified clean" must never
        // look alike, so `checked` is reported even when there are no
        // mismatches — "0 mismatches" over 0 results would read as a pass.
        spdlog::info("reference check: checked={} matched={} mismatched={} unsupported={}",
                     ref_stats.checked, ref_stats.matched, ref_stats.mismatched,
                     ref_stats.unsupported);
        if (ref_stats.mismatched > 0) {
            spdlog::error("{} RESULT(S) DID NOT MATCH THE CPU REFERENCE",
                          ref_stats.mismatched);
            return 1;
        }
        return 0;
    };
    server.SetOnComplete(report);
    server.Run();

    if (verify_reference) {
        // "Nothing was verified" and "everything verified clean" must never
        // look alike, so `checked` is reported even when there are no
        // mismatches — a summary of "0 mismatches" over 0 results would
        // otherwise read as a pass.
        return report();   // reached only if Run() returned on its own
    }
    return 0;
}
