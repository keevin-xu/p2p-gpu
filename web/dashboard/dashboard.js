/*
 * Dashboard. Consumes the coordinator SSE feed; writes nothing.
 *
 * Panels: fleet table, task-state breakdown, throughput chart, queue depth,
 * validation stats, rejected-frame count, and (from Phase 5) the live image.
 *
 * Keep it simple. Resist turning it into an application.
 * Implemented in Phase 2 step 2.22.
 */

// Colours follow the ORDER of TaskState in task_state.hpp. The *names* arrive
// with the data (`state_names`) instead of being hardcoded here, so adding a
// state cannot leave this page silently mislabelling every column after it.
const COLORS = [
  "#8b93a7", // Queued
  "#58a6ff", // Leased
  "#a371f7", // Validating
  "#d29922", // NeedsReplica
  "#3fb950", // Accepted
  "#f85149", // Rejected
  "#484f58", // Cancelled
];

const fmt = (n) =>
  n === null || n === undefined ? "–"
  : n >= 1e12 ? (n / 1e12).toFixed(2) + "T"
  : n >= 1e9  ? (n / 1e9).toFixed(2) + "G"
  : n >= 1e6  ? (n / 1e6).toFixed(2) + "M"
  : n >= 1e3  ? (n / 1e3).toFixed(1) + "k"
  : String(Math.round(n * 100) / 100);

const set = (id, v) => { document.getElementById(id).textContent = v; };

function renderStates(d) {
  const total = d.tasks_by_state.reduce((a, b) => a + b, 0);
  const bar = document.getElementById("statebar");
  const legend = document.getElementById("legend");
  bar.replaceChildren();
  legend.replaceChildren();

  d.tasks_by_state.forEach((count, i) => {
    const color = COLORS[i % COLORS.length];
    if (count > 0 && total > 0) {
      const seg = document.createElement("div");
      seg.style.width = (100 * count / total) + "%";
      seg.style.background = color;
      seg.title = d.state_names[i] + ": " + count;
      bar.appendChild(seg);
    }
    const item = document.createElement("span");
    const swatch = document.createElement("i");
    swatch.style.background = color;
    item.appendChild(swatch);
    item.appendChild(document.createTextNode(d.state_names[i] + " " + count));
    legend.appendChild(item);
  });
}

function renderFleet(d) {
  const tbody = document.getElementById("fleet");
  tbody.replaceChildren();

  // Sorted by observed throughput, so the interesting end of a heterogeneous
  // fleet is at the top rather than wherever the hash map happened to put it.
  const rows = d.fleet
    .slice()
    .sort((a, b) => (b.observed_units_per_sec || 0) - (a.observed_units_per_sec || 0));

  for (const w of rows) {
    const tr = document.createElement("tr");
    const cells = [
      "#" + w.worker_id,
      w.tasks_completed,
      fmt(w.score_ops_per_sec),
      fmt(w.observed_units_per_sec),
      w.correction === null ? "–" : w.correction.toFixed(2),
      w.throttle === null ? "–" : w.throttle.toFixed(2),
    ];
    for (const c of cells) {
      const td = document.createElement("td");
      // textContent, NOT innerHTML. Every field here is a number today, but
      // this table shows data derived from connections nobody controls, and the
      // day a string field is added is the day innerHTML becomes an injection.
      // Build the habit while it costs nothing.
      td.textContent = c;
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
}

function render(d) {
  set("workers", d.workers);
  set("queue", d.queue_depth);
  set("tasks", d.total_tasks);
  set("wasted", fmt(d.wasted_units));
  set("rejected", d.rejected_frames);
  set("progress", d.units_total > 0
    ? (100 * (d.units_total - d.units_remaining) / d.units_total).toFixed(1) + "%"
    : "–");

  renderStates(d);
  renderFleet(d);
}

// Same-origin by default; `?coordinator=http://host:port` when the dashboard is
// served from somewhere else — which is the usual case, since tools/serve.py is
// on :8000 and the coordinator is on :8080. That cross-origin split is why
// /metrics/stream sends Access-Control-Allow-Origin (the same trap as the
// kernel fetch in 1.23, where a missing header presented as an unrelated bug).
const base = new URLSearchParams(location.search).get("coordinator") || "";
const status = document.getElementById("status");
const src = new EventSource(base + "/metrics/stream");

src.onopen = () => { status.textContent = "live"; status.className = "up"; };

src.onmessage = (e) => {
  // A malformed frame must not kill the stream. EventSource reconnects on
  // transport errors, but NOT on an exception thrown inside this handler — so
  // one bad parse would silently freeze the page on stale numbers, which is
  // worse than showing nothing.
  try {
    render(JSON.parse(e.data));
  } catch (err) {
    console.error("dashboard: bad frame", err);
  }
};

src.onerror = () => { status.textContent = "reconnecting"; status.className = "down"; };

// ── 5.16 — the progressive render ────────────────────────────────────────
//
// Polled rather than pushed. The SSE feed carries numbers, and a 300 KB image
// on that channel every sweep would swamp it — and the two have genuinely
// different natural rates: metrics matter every tick, the picture only needs to
// look alive.
//
// Nothing here decides anything. The coordinator composites (R1); this blits
// bytes to a canvas. If that ever stops being true, the page has grown a second
// opinion about the image.
const renderPanel = document.getElementById("render-panel");
const renderCanvas = document.getElementById("render");
const renderInfo = document.getElementById("render-info");
const renderCtx = renderCanvas.getContext("2d");

const RENDER_MAGIC = 0x50324752;   // "RG2P" little-endian
let renderInFlight = false;

async function pollRender() {
  // Skip rather than queue. A slow fetch must not pile up requests behind it —
  // that turns a briefly busy coordinator into an unbounded backlog.
  if (renderInFlight) { return; }
  renderInFlight = true;
  try {
    const res = await fetch(base + "/render", { cache: "no-store" });
    if (!res.ok) { return; }
    const buf = await res.arrayBuffer();
    if (buf.byteLength < 16) { return; }
    const head = new DataView(buf, 0, 16);
    if (head.getUint32(0, true) !== RENDER_MAGIC) { return; }
    const w = head.getUint32(4, true);
    const h = head.getUint32(8, true);
    if (w === 0 || h === 0) { return; }
    // The declared size must match the bytes that arrived. Trusting the header
    // and constructing ImageData from a short buffer throws, which would kill
    // the interval and freeze the picture on a stale frame.
    if (buf.byteLength !== 16 + w * h * 4) { return; }

    if (renderCanvas.width !== w || renderCanvas.height !== h) {
      renderCanvas.width = w;
      renderCanvas.height = h;
    }
    const pixels = new Uint8ClampedArray(buf, 16, w * h * 4);
    renderCtx.putImageData(new ImageData(pixels, w, h), 0, 0);
    renderPanel.hidden = false;
    renderInfo.textContent = w + "x" + h;
  } catch (err) {
    // Same reasoning as the SSE handler: a transient failure must not stop the
    // loop, or the image silently stops updating while everything else looks fine.
    console.error("dashboard: render fetch failed", err);
  } finally {
    renderInFlight = false;
  }
}

// 1 Hz. Fast enough to look live as tiles land, slow enough that the image is
// not the dominant load on a coordinator whose actual job is scheduling.
pollRender();
setInterval(pollRender, 1000);
