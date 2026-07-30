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
  var statusEl = document.getElementById("status");
  var logEl = document.getElementById("log");
  var lines = [];

  function setStatus(text, cls) {
    statusEl.textContent = text;
    statusEl.className = cls || "";
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
    startBtn.textContent = "Start contributing";
    setStatus("ready — click to start. No GPU work has run yet.");
  }).catch(function (err) {
    setStatus("failed to load worker module: " + err, "fail");
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
      });
  });
})();
