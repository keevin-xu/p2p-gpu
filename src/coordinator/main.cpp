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

#include <memory>

#include "p2pgpu/coordinator/net.hpp"
#include "p2pgpu/scene/bvh.hpp"
#include "p2pgpu/scene/scene.hpp"

int main(int argc, char** argv) {
    p2pgpu::coordinator::Config cfg;

    CLI::App app{"p2pgpu coordinator"};
    app.add_option("--port", cfg.port, "WebSocket / HTTP port")->capture_default_str();
    app.add_option("--manifest", cfg.manifest, "kernel manifest")->capture_default_str();
    app.add_option("--kernel-dir", cfg.kernel_dir, "WGSL directory")->capture_default_str();
    app.add_option("--replication", cfg.replication,
                   "none | fixed2x | adaptive (steps 3.6/3.8)")
        ->capture_default_str();
    app.add_flag("--spot-checks", cfg.spot_checks,
                 "inject known-answer tasks (3.9). Strongly recommended with "
                 "--replication adaptive");
    app.add_option("--events-csv", cfg.events_csv,
                   "DEV: per-event CSV for the 2.23-2.26 experiments");
    app.add_flag("!--no-speculation", cfg.speculation,
                 "disable speculative re-execution (E5's control condition)");
    app.add_option("--store", cfg.store_path,
                   "SQLite file for durable state; empty (default) disables it");
    app.add_option("--log-level", cfg.log_level,
                   "trace|debug|info|warn|error")->capture_default_str();

    // DEVELOPMENT AFFORDANCE, replaced by the job submission API in step 2.16.
    // Phase 1's objective is "one worker completes a real job end to end", and
    // without this there is no way to put a job in the queue at all. Kept
    // deliberately crude so nobody mistakes it for the real interface.
    std::string seed_kernel;
    std::string seed_render_scene;
    std::string render_size = "384x256";
    std::uint64_t render_spp = 512;
    p2pgpu::coordinator::TileGrid render_grid{};
    std::vector<std::byte> render_asset;
    std::uint64_t seed_units = 4'000'000;
    std::uint32_t seed_tasks = 4;
    app.add_option("--seed-render", seed_render_scene,
                   "DEV ONLY: queue a path-trace render of this .scene file")
        ->check(CLI::ExistingFile);
    app.add_option("--render-size", render_size,
                   "DEV ONLY: WxH for --seed-render")->capture_default_str();
    app.add_option("--render-spp", render_spp,
                   "DEV ONLY: samples per pixel for --seed-render")->capture_default_str();
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
    app.add_option("--lease-ms", cfg.lease_ms,
                   "lease duration on the coordinator clock")->capture_default_str();
    app.add_option("--worker-timeout-ms", cfg.worker_timeout_ms,
                   "silence after which a worker is declared lost")->capture_default_str();
    app.add_option("--sweep-ms", cfg.sweep_interval_ms,
                   "how often the expiry/loss sweep runs")->capture_default_str();
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
    // Owns who is connected, separately from who owns work — the two have
    // genuinely different lifetimes (fleet.hpp).
    p2pgpu::coordinator::Fleet fleet;

    // 2.19/2.20 — open the store and recover BEFORE seeding.
    //
    // Order matters: recovery replaces all state (`AdoptRecovered`), so a job
    // seeded first would be silently discarded. And --seed-job after a
    // successful recovery would create a SECOND job over the same keyspace,
    // which presents as the fleet doing everything twice.
    std::unique_ptr<p2pgpu::coordinator::Store> store;
    bool recovered_any = false;
    if (!cfg.store_path.empty()) {
        auto opened = p2pgpu::coordinator::Store::Open(cfg.store_path);
        if (!opened) {
            spdlog::error("could not open store: {}", opened.error().message);
            return 1;
        }
        store = *std::move(opened);

        auto state = store->LoadAll();
        if (!state) {
            spdlog::error("could not read store: {}", state.error().message);
            return 1;
        }
        if (!state->jobs.empty()) {
            p2pgpu::coordinator::RecoverInto(jobs, *state);
            recovered_any = true;
            spdlog::info("recovered jobs={} tasks={} requeued={} from {}",
                         state->jobs.size(), state->tasks.size(), jobs.queued(),
                         cfg.store_path);
        } else {
            spdlog::info("store {} is empty; starting fresh", cfg.store_path);
        }
    }

    if (recovered_any && !seed_kernel.empty()) {
        // Refuse rather than guess. Seeding on top of a recovered job would
        // double the keyspace, and silently ignoring --seed-job would make a
        // restart quietly do something different from what was asked.
        spdlog::error("--seed-job with a non-empty store: refusing to seed a second "
                      "job over recovered state. Delete {} or drop --seed-job.",
                      cfg.store_path);
        return 1;
    }

    // ── DEV: queue a render (5.15/5.16) ──────────────────────────────────
    if (!seed_render_scene.empty()) {
        // Refuse rather than queue work no worker can run — the SAME guard
        // --seed-job has, and its absence here produced a job that carved tasks
        // forever while every grant died with "kernel is not in the registry".
        // The 5.15 bring-up hit exactly that; D-0066's stall detector named the
        // symptom correctly, but the coordinator should never have started.
        if (registry->Find("pathtrace_tile_v1") == nullptr) {
            spdlog::error("--seed-render needs pathtrace_tile_v1 in "
                          "kernels/manifest.toml, and it is not there");
            return 1;
        }
        std::size_t bad_line = 0;
        auto scene = p2pgpu::scene::LoadSceneFile(seed_render_scene, &bad_line);
        if (!scene) {
            spdlog::error("scene {}: {} (line {})", seed_render_scene,
                          scene.error().message, bad_line);
            return 1;
        }
        const auto blob = p2pgpu::scene::SerializeBvh(p2pgpu::scene::BuildBvh(*scene));
        auto bvh = p2pgpu::scene::LoadBvh(blob);
        if (!bvh) {
            spdlog::error("bvh: {}", bvh.error().message);
            return 1;
        }

        std::uint32_t w = 384;
        std::uint32_t h = 256;
        if (const auto x = render_size.find('x'); x != std::string::npos) {
            w = static_cast<std::uint32_t>(std::stoul(render_size.substr(0, x)));
            h = static_cast<std::uint32_t>(std::stoul(render_size.substr(x + 1)));
        }

        p2pgpu::coordinator::RenderConfig rc;
        rc.grid = p2pgpu::coordinator::TileGrid{w, h, 64, 64};
        rc.samples_per_tile = render_spp;
        const float aspect = static_cast<float>(w) / static_cast<float>(h);
        rc.cam_origin[0] = 0.0F; rc.cam_origin[1] = 1.4F; rc.cam_origin[2] = 4.0F;
        rc.cam_lower_left[0] = -aspect; rc.cam_lower_left[1] = 0.35F;
        rc.cam_lower_left[2] = 2.6F;
        rc.cam_horizontal[0] = 2.0F * aspect;
        rc.cam_vertical[1] = 2.0F;
        rc.node_count = static_cast<std::uint32_t>(bvh->nodes.size());
        rc.prim_count = static_cast<std::uint32_t>(bvh->prims.size());
        rc.material_count = static_cast<std::uint32_t>(bvh->materials.size());

        // `units_per_group` is what stops a task spanning two tiles. Passed
        // separately from `RenderConfig` on purpose: the carve rule must not
        // need to know what a tile is (R1).
        const std::uint64_t total = static_cast<std::uint64_t>(rc.grid.tile_count()) *
                                    rc.samples_per_tile;
        const auto job = jobs.CreateJob("pathtrace_tile_v1", total, /*seed=*/42,
                                        rc.samples_per_tile);
        if (auto* mutable_job = jobs.MutableJob(job); mutable_job != nullptr) {
            mutable_job->render = rc;
            // Per-tile granted counters (5.17). Sized here rather than lazily,
            // so `Grant` never has to decide whether the vector is ready.
            mutable_job->tile_granted.assign(rc.grid.tile_count(), 0);
            // DEV ONLY (5.20). The coordinator has no business owning geometry
            // in normal operation — it publishes the asset by hash and holds
            // nothing else. Attached only when the reference check is on, so a
            // production coordinator never carries a copy.
            if (verify_reference) {
                mutable_job->render->reference_bvh =
                    std::make_shared<const p2pgpu::scene::Bvh>(std::move(*bvh));
            }
            // THE JOB MUST NAME ITS ASSET, or the grant carries no `input_ref`
            // and the worker has no way to know it needs one. Setting the
            // render config without this is what made the 5.16 bring-up crash:
            // the worker ran a kernel whose storage bindings nothing supplied.
            const std::string address = p2pgpu::scene::ContentAddress(blob);
            p2pgpu::coordinator::AssetId id{};
            for (std::size_t i = 0; i < id.size(); ++i) {
                id[i] = static_cast<std::byte>(
                    std::stoul(address.substr(i * 2, 2), nullptr, 16));
            }
            mutable_job->input_ref = id;
        }
        render_grid = rc.grid;
        render_asset = std::move(blob);
        spdlog::warn("DEV: seeded RENDER {}x{} tiles={} spp={} prims={} depth={} "
                     "job_lo={}",
                     w, h, rc.grid.tile_count(), rc.samples_per_tile,
                     bvh->prims.size(), bvh->max_depth, job.lo());
    }

    if (!seed_kernel.empty()) {
        if (registry->Find(seed_kernel) == nullptr) {
            // Refuse rather than queue work no worker can run — the failure
            // would otherwise surface as every worker releasing every task with
            // KernelUnavailable, nowhere near the cause.
            spdlog::error("--seed-job names an unknown kernel: {}", seed_kernel);
            return 1;
        }
        // Tasks are carved on demand now (D-0043), so --seed-tasks no longer
        // sets a count. Kept as a knob only for tests that want a specific
        // shape; the sizer decides the real thing.
        const auto job = jobs.CreateJob(seed_kernel, seed_units, /*seed=*/42);
        spdlog::warn("DEV: seeded job kernel={} units={} job_lo={} (tasks carved on demand)",
                     seed_kernel, seed_units, job.lo());
    }

    p2pgpu::coordinator::ReferenceStats ref_stats;
    if (verify_reference) {
        spdlog::warn("DEV: --verify-reference is ON. This recomputes every result on "
                     "the CPU. It is a TEST HARNESS, not validation — never quote it "
                     "as evidence that results are validated (Phase 3 does that).");
    }

    std::unique_ptr<p2pgpu::coordinator::EventLog> events;
    if (!cfg.events_csv.empty()) {
        auto opened = p2pgpu::coordinator::EventLog::Open(cfg.events_csv);
        if (!opened) {
            spdlog::error("{}", opened.error().message);
            return 1;
        }
        events = *std::move(opened);
        spdlog::warn("DEV: writing per-event CSV to {}", cfg.events_csv);
    }
    if (!cfg.speculation) {
        spdlog::warn("speculation DISABLED (E5 control condition)");
    }

    p2pgpu::coordinator::Server server(cfg, *registry, jobs, fleet,
                                       verify_reference ? &ref_stats : nullptr,
                                       store.get(), events.get());
    if (render_grid.tile_count() > 0) {
        server.SetRenderGrid(render_grid);
        // 0.45 for scenes/default.scene, whose sky is near-white: at 1.0 most
        // of the frame clips and the picture reads as washed out rather than as
        // correctly bright. An explicit constant, not auto-exposure — see
        // composite.hpp.
        server.SetRenderExposure(0.45F);
        const std::string address = server.PublishAsset(std::move(render_asset));
        spdlog::warn("DEV: render asset published address={} (GET /asset/{})",
                     address, address);
    }

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
        spdlog::info("reference check: checked={} matched={} mismatched={} unsupported={}"
                     " | ACCEPTED: checked={} wrong={}",
                     ref_stats.checked, ref_stats.matched, ref_stats.mismatched,
                     ref_stats.unsupported, ref_stats.accepted_checked,
                     ref_stats.accepted_wrong);
        // The VERDICT keys off accepted_wrong, not mismatched. Under
        // replication a caught liar submits a wrong answer that is then
        // outvoted — counting that as a failure would make a working validator
        // report a failing run, which is exactly what it did before this split
        // existed (7 mismatches on a run that caught all 7 lies).
        if (ref_stats.accepted_wrong > 0) {
            spdlog::error("{} ACCEPTED RESULT(S) DID NOT MATCH THE CPU REFERENCE",
                          ref_stats.accepted_wrong);
            return 1;
        }
        if (ref_stats.mismatched > 0) {
            spdlog::warn("{} wrong result(s) were SUBMITTED and caught by validation "
                         "(not a failure)", ref_stats.mismatched);
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
