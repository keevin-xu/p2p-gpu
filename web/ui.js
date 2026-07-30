/*
 * THE ONLY JAVASCRIPT IN THIS PROJECT.
 *
 * It exists for exactly one reason: WebAssembly cannot touch the DOM directly.
 * This file is the DOM surface for the R7 opt-in controls.
 *
 * ── KEEP IT DUMB ─────────────────────────────────────────────────────────
 * No logic. No state machine. No decisions. Read a slider, set some text,
 * fire a callback into C++. Anything more violates R1 (dumb worker) and R2
 * (one worker-core, never forked) — and reintroduces the second-language
 * boundary that the C++ pivot deleted (D-0008).
 *
 * If you catch yourself wanting to compute something here, the answer belongs
 * in worker-core, or in the coordinator.
 *
 * Note what is deliberately NOT here: verifying the kernel result. That check
 * lives in C++ (worker-browser/main.cpp) so the browser and native targets
 * validate correctness with the SAME code. This file only prints what C++ said.
 */

// Surface required by R7:
//   - start button        -> calls into C++ to begin contributing        [0.6]
//   - contributing badge  -> visible whenever GPU work is running        [0.6]
//   - throttle slider     -> 0.0-1.0, sent as Throttle{level}            [1.23]
//   - stop button         -> instant, releases leases cleanly (Draining) [1.23]

(function () {
  "use strict";

  var startBtn = document.getElementById("start");
  var chunkBtn = document.getElementById("chunk");
  var benchBtn = document.getElementById("bench");
  var statusEl = document.getElementById("status");
  var logEl = document.getElementById("log");
  var heartbeatEl = document.getElementById("heartbeat");
  var lines = [];

  // Responsiveness probe for step 0.15. A plain main-thread timer: if the
  // worker ever blocks rather than yielding, the browser cannot run this and
  // the number stops. Objective evidence, not a subjective impression.
  var ticks = 0;
  var stalls = 0;
  var lastTick = Date.now();
  setInterval(function () {
    var now = Date.now();
    var gap = now - lastTick;
    lastTick = now;
    ticks++;
    // Expected gap is 100 ms. Anything past 500 ms means the main thread was
    // held hostage — exactly what yielding is supposed to prevent.
    if (gap > 500) { stalls++; }
    heartbeatEl.textContent =
      "heartbeat: " + ticks + " ticks, " + stalls + " stalls (gap " + gap + "ms)";
  }, 100);

  function setStatus(text, cls) {
    statusEl.textContent = text;
    statusEl.className = cls || "";
  }

  // Ship the canonical report to the dev server, which tags it by browser and
  // writes it under results/. Development convenience only — nothing in the
  // real system posts anything this way, and a failure here must never affect
  // the PASS/FAIL reported above.
  //
  // Only called for SUCCESSFUL runs: a failed run has no meaningful report, and
  // posting an empty one just stacks a second, misleading error on top of the
  // real failure (which is exactly what happened diagnosing Safari).
  function post(name) {
    var report = worker.ccall("p2pgpu_report", "string", [], []);
    if (!report) { return; }
    fetch("/report?name=" + encodeURIComponent(name),
          { method: "POST", body: report })
      .then(function () { append("[dev] posted results/ (" + name + ", browser-tagged)"); })
      .catch(function () { append("[dev] POST failed — copy the text above"); });
  }

  function append(text) {
    lines.push(text);
    logEl.textContent = lines.join("\n");
  }

  // Mirror C++ console output into the page so the result is visible without
  // opening devtools. The console remains the source of truth.
  ["log", "warn", "error"].forEach(function (level) {
    var original = console[level].bind(console);
    console[level] = function () {
      append(Array.prototype.join.call(arguments, " "));
      original.apply(null, arguments);
    };
  });

  // The module is MODULARIZE'd, so there is no global Module — the build emits
  // a createP2pgpuWorker() factory returning a promise. Its resolution IS the
  // readiness signal; the button stays disabled until then, because there is
  // nothing to opt in to yet.
  var worker = null;

  createP2pgpuWorker().then(function (mod) {
    worker = mod;
    startBtn.disabled = false;
    chunkBtn.disabled = false;
    benchBtn.disabled = false;
    startBtn.textContent = "Start contributing";
    setStatus("ready — click to start. No GPU work has run yet.");
  }).catch(function (err) {
    setStatus("failed to load worker module: " + err, "fail");
  });

  benchBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    // R7: GPU work only ever starts from a user click.
    benchBtn.disabled = true;
    startBtn.disabled = true;
    chunkBtn.disabled = true;
    benchBtn.textContent = "Running…";
    setStatus("● contributing — throughput benchmark running (~30s)", "running");

    worker
      .ccall("p2pgpu_run_bench", "number", [], [], { async: true })
      .then(function (rc) {
        setStatus(rc === 0 ? "✓ throughput measured — see output"
                           : "✗ benchmark failed — see output",
                  rc === 0 ? "pass" : "fail");
        benchBtn.disabled = false;
        startBtn.disabled = false;
        chunkBtn.disabled = false;
        benchBtn.textContent = "Throughput (0.11)";

        if (rc === 0) { post("0.11-throughput.csv"); }
      });
  });

  chunkBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    // R7 applies here too: GPU work only ever starts from a user click.
    chunkBtn.disabled = true;
    startBtn.disabled = true;
    chunkBtn.textContent = "Running…";
    setStatus("● contributing — chunking spike running (watch the heartbeat)",
              "running");
    var stallsBefore = stalls;

    worker
      .ccall("p2pgpu_run_chunking", "number", [], [], { async: true })
      .then(function (rc) {
        var stalled = stalls - stallsBefore;
        if (rc === 0 && stalled === 0) {
          setStatus("✓ chunking done — tab stayed responsive (0 stalls)", "pass");
        } else if (rc === 0) {
          setStatus("⚠ chunking done but the main thread stalled " + stalled +
                    "x — yielding is not working", "fail");
        } else {
          setStatus("✗ chunking spike failed — see output", "fail");
        }
        append("[dev] main-thread stalls during run: " + stalled);
        chunkBtn.disabled = false;
        startBtn.disabled = false;
        chunkBtn.textContent = "Chunking spike (0.15)";

        if (rc === 0) { post("0.15-chunking-browser.txt"); }
      });
  });

  startBtn.addEventListener("click", function () {
    if (worker === null) {
      return;
    }
    // R7: this click is the ONLY path to GPU work.
    startBtn.disabled = true;
    startBtn.textContent = "Running…";
    setStatus("● contributing — GPU work in progress", "running");

    // async: true is REQUIRED, not optional. The C++ side yields to the event
    // loop while waiting on WebGPU (ASYNCIFY), so the call unwinds back to JS
    // and resumes later. A synchronous ccall would return before the kernel
    // finished and report a meaningless result.
    worker
      .ccall("p2pgpu_run_smoke_test", "number", [], [], { async: true })
      .then(function (rc) {
        if (rc === 0) {
          setStatus("✓ PASS — kernel output verified", "pass");
        } else {
          setStatus("✗ FAIL — see output below", "fail");
        }
        startBtn.disabled = false;
        startBtn.textContent = "Run again";

        if (rc === 0) { post("0.9-browser.txt"); }
      });
  });
})();
