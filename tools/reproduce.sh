#!/usr/bin/env bash
# Reproduce the evaluation — step 7.6.
#
#   tools/reproduce.sh charts      regenerate every chart from committed CSVs
#   tools/reproduce.sh experiments re-run the experiments, overwriting the CSVs
#   tools/reproduce.sh all         experiments, then charts
#
# ── TWO COMMANDS, DELIBERATELY ───────────────────────────────────────────────
# `charts` reads committed data and starts nothing. `experiments` overwrites
# that data by running the fleet for ~30 minutes. Anyone who wants a picture
# wants the first, and if one command did both, "let me just regenerate the
# chart" would silently destroy the numbers it was drawing — which is the
# accident this split exists to make impossible.
#
# ── WHAT THIS CANNOT REPRODUCE ───────────────────────────────────────────────
# E7 needs >=3 GPU vendors and E1's REAL overlay needs up to 5 physical GPUs
# (docs/EVALUATION.md). This machine has one, so both are BLOCKED, not missing:
# the script says so at the end rather than producing a quieter set of outputs
# than the evaluation asks for.

set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-charts}"
# Overridable so the sanitizer guard below can be TESTED. A guard that has
# never been observed to fire is an assumption with a shell function around it.
BUILD="${P2PGPU_BUILD:-build/native-release}"

die() { echo "error: $*" >&2; exit 1; }

need_build() {
    [ -x "$BUILD/coordinator" ] || die "no $BUILD/coordinator — run: cmake --preset native-release && cmake --build $BUILD"
    # Sanitizer builds cost 2-10x and would be measuring the sanitizer, not the
    # system (CONVENTIONS.md §8). Refused rather than warned about: a warning
    # scrolls past and the numbers still land in results/.
    if grep -qs "fsanitize" "$BUILD/CMakeCache.txt"; then
        die "$BUILD looks like a sanitizer build; measurements must use native-release"
    fi
}

run_experiments() {
    need_build
    echo "==> experiments (~30 min; overwrites results/*.csv)"
    python3 tools/experiment.py all
    python3 tools/experiment_e6.py --repeats 3 --out results/E6_egress.csv
    python3 tools/experiment_sources.py --out results/6.14-sources.csv
    python3 tools/experiment_ready.py  --out results/6.16-time-to-ready.csv
    echo "==> E2 (utilization, with and without accumulation)"
    for spp in 512 2048 8192; do
        "$BUILD/render-preview" scenes/default.scene 256 192 "$spp" /tmp/e2_on_$spp.ppm  | grep '^E2' || true
        "$BUILD/render-preview" scenes/default.scene 256 192 "$spp" /tmp/e2_off_$spp.ppm --no-accumulation | grep '^E2' || true
    done
    echo "    (E2 rows are transcribed into results/E2_utilization.csv by hand —"
    echo "     render-preview prints them; it does not own the CSV)"
    echo "==> E7 (this device's row only)"
    "$BUILD/worker-native" bench | tail -4
}

run_charts() {
    echo "==> charts from committed CSVs"
    python3 tools/make_charts.py
}

case "$MODE" in
    charts)      run_charts ;;
    experiments) run_experiments ;;
    all)         run_experiments; run_charts ;;
    *)           die "usage: $0 {charts|experiments|all}" ;;
esac

cat <<'EOF'

BLOCKED, not missing — both need hardware this machine does not have:
  7.3  E7 across >=3 vendors        (one vendor measured; pct_of_peak needs a
                                     published spec, which is not a measurement)
  7.2  E1 REAL overlay, 1..5 GPUs   (one GPU; N processes sharing it measures
                                     GPU contention, not fleet scaling)
EOF
