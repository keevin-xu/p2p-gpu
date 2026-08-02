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
 * Note what is deliberately NOT here:
 *   - Verifying kernel results. That is C++, so both targets check with the
 *     same code; this file only prints what C++ said.
 *   - Tracking whether we are contributing. C++ owns that and writes the
 *     indicator every frame through ui_bridge.cpp. Two sources of truth about
 *     the one fact R7 requires be honest is exactly one too many.
 *
 * ── THE R7 SURFACE (step 1.23) ───────────────────────────────────────────
 *   start button        -> p2pgpu_start(url)          begins contributing
 *   stop button         -> p2pgpu_stop()              instant, releases leases
 *   throttle slider     -> p2pgpu_set_throttle(0..1)  0 pauses, stays connected
 *   contributing badge  <- written by C++ every frame
 *
 * Those three calls are the only things a user can do, and every one of them
 * is reachable ONLY from a real click or drag.
 */

(function () {
  "use strict";

  var startBtn = document.getElementById("start");
  var stopBtn = document.getElementById("stop");
  var urlEl = document.getElementById("url");
  var throttleEl = document.getElementById("throttle");
  var throttleVal = document.getElementById("throttleVal");

  var smokeBtn = document.getElementById("smoke");
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

  // Presentation only: the diagnostics are separate GPU work, and running one
  // while the grid loop is live would have two things fighting over the device
  // and over #status. Disabling them is not a decision about the system, it is
  // a decision about which buttons are clickable.
  function setDiagnosticsEnabled(on) {
    smokeBtn.disabled = !on;
    chunkBtn.disabled = !on;
    benchBtn.disabled = !on;
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
  // readiness signal; the buttons stay disabled until then, because there is
  // nothing to opt in to yet.
  var worker = null;

  createP2pgpuWorker().then(function (mod) {
    worker = mod;
    startBtn.disabled = false;
    startBtn.textContent = "Start contributing";
    setDiagnosticsEnabled(true);
    setStatus("ready — click Start. No GPU work has run yet.");
  }).catch(function (err) {
    setStatus("failed to load worker module: " + err, "fail");
  });

  // ── R7: START ──────────────────────────────────────────────────────────
  // The affirmative click. This is the ONLY path into the task loop; there is
  // no autostart, no query parameter, and no timer that reaches it.
  startBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    setDiagnosticsEnabled(false);
    setStatus("connecting…", "running");

    // async: true is REQUIRED, not optional — same as the diagnostics below.
    // p2pgpu_start acquires the GPU device, which waits through
    // platform::WaitUntil -> emscripten_sleep, and ASYNCIFY unwinds the stack
    // back to JS there.
    //
    // Getting this wrong does NOT fail loudly. The socket still opens, because
    // the coroutine gets that far — but with an async operation still in
    // flight, the transport's onOpen callback cannot re-enter WASM, so Hello is
    // never sent. The coordinator logs conn_open and then nothing, forever.
    // (Observed exactly that on the first browser run of step 1.23.)
    //
    // C++ takes it from here, including enabling/disabling these very buttons
    // through ui_bridge.cpp — the page does not track whether we are running.
    worker.ccall("p2pgpu_start", "number", ["string"], [urlEl.value],
                 { async: true });
  });

  // ── R7: INSTANT STOP ───────────────────────────────────────────────────
  // "Instant" is a promise to the user. C++ cannot cancel a dispatch already on
  // the GPU — nothing can — but chunking bounds that to one chunk and nothing
  // further is submitted.
  stopBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    worker.ccall("p2pgpu_stop", null, [], []);
    setDiagnosticsEnabled(true);
  });

  // ── R7: THROTTLE ───────────────────────────────────────────────────────
  // Read the slider, convert to 0..1, hand it over. The meaning of the number
  // is C++'s business; note in particular that 0 does not disconnect.
  throttleEl.addEventListener("input", function () {
    var pct = parseInt(throttleEl.value, 10);
    throttleVal.textContent = pct + "%";
    if (worker !== null) {
      worker.ccall("p2pgpu_set_throttle", null, ["number"], [pct / 100]);
    }
  });

  // ── Phase 0 diagnostics ────────────────────────────────────────────────
  // Measurement harnesses. R7 applies to these too: GPU work only ever starts
  // from a user click.

  smokeBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    setDiagnosticsEnabled(false);
    startBtn.disabled = true;
    smokeBtn.textContent = "Running…";
    setStatus("● contributing — GPU work in progress", "running");

    // async: true is REQUIRED, not optional. The C++ side yields to the event
    // loop while waiting on WebGPU (ASYNCIFY), so the call unwinds back to JS
    // and resumes later. A synchronous ccall would return before the kernel
    // finished and report a meaningless result.
    worker
      .ccall("p2pgpu_run_smoke_test", "number", [], [], { async: true })
      .then(function (rc) {
        setStatus(rc === 0 ? "✓ PASS — kernel output verified"
                           : "✗ FAIL — see output below",
                  rc === 0 ? "pass" : "fail");
        setDiagnosticsEnabled(true);
        startBtn.disabled = false;
        smokeBtn.textContent = "Smoke test (0.6)";
        if (rc === 0) { post("0.9-browser.txt"); }
      });
  });

  chunkBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    setDiagnosticsEnabled(false);
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
        setDiagnosticsEnabled(true);
        startBtn.disabled = false;
        chunkBtn.textContent = "Chunking spike (0.15)";
        if (rc === 0) { post("0.15-chunking-browser.txt"); }
      });
  });

  benchBtn.addEventListener("click", function () {
    if (worker === null) { return; }
    setDiagnosticsEnabled(false);
    startBtn.disabled = true;
    benchBtn.textContent = "Running…";
    setStatus("● contributing — throughput benchmark running (~30s)", "running");

    worker
      .ccall("p2pgpu_run_bench", "number", [], [], { async: true })
      .then(function (rc) {
        setStatus(rc === 0 ? "✓ throughput measured — see output"
                           : "✗ benchmark failed — see output",
                  rc === 0 ? "pass" : "fail");
        setDiagnosticsEnabled(true);
        startBtn.disabled = false;
        benchBtn.textContent = "Throughput (0.11)";
        if (rc === 0) { post("0.11-throughput.csv"); }
      });
  });
})();
