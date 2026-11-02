#!/usr/bin/env bash
# The CI gate, run locally (GitHub Actions is manual-only now): build every
# target, then the unit tests, the FX plugin tests, the UI test, the preset
# fingerprints against the Linux baseline and pluginval at strictness 10 on
# both plugins. Stops at the first failure.
#
#   tools/verify.sh              everything
#   tools/verify.sh --quick      skip pluginval
#   ILANA_JOBS=2 tools/verify.sh build parallelism (default: every core)
#
# Needs: cmake, ninja, python3; on a headless Linux box xvfb-run; pluginval
# is fetched once into build/pluginval (set PLUGINVAL=/path to use another;
# on macOS an installed /Applications/pluginval.app is used as it is).
set -euo pipefail

cd "$(dirname "$0")/.."
quick=0
[ "${1:-}" = "--quick" ] && quick=1

run_gui() { if command -v xvfb-run > /dev/null && [ -z "${DISPLAY:-}" ]; then xvfb-run -a "$@"; else "$@"; fi; }
step() { printf '\n=== %s\n' "$*"; }

step "Build"
if [ ! -f build/CMakeCache.txt ]; then
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake --build build --parallel "${ILANA_JOBS:-$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)}"

step "Unit tests"
ILANA_CI=1 build/ilanaTableTest_artefacts/Release/ilanaTableTest | tee build/table-test.txt | grep -E "^FAIL|TESTS" || true
grep -q "ALL TESTS PASSED" build/table-test.txt

step "FX plugin tests"
build/ilanaFxTest_artefacts/Release/ilanaFxTest | tail -1

step "UI tests"
run_gui build/ilanaSnapshot_artefacts/Release/ilanaSnapshot --uitest | tail -1

step "Preset fingerprints"
build/ilanaFingerprint_artefacts/Release/ilanaFingerprint build/fingerprints.csv > /dev/null
python3 tools/compare_fingerprints.py tests/fingerprints-linux.csv build/fingerprints.csv --fail | tail -3

if [ "$quick" = 0 ]; then
    step "pluginval (strictness 10)"
    if [ "$(uname)" = Darwin ]; then
        PV="${PLUGINVAL:-/Applications/pluginval.app/Contents/MacOS/pluginval}"
        if [ ! -x "$PV" ]; then
            PV=build/pluginval/pluginval.app/Contents/MacOS/pluginval
            if [ ! -x "$PV" ]; then
                mkdir -p build/pluginval
                curl -sSL -o build/pluginval/pluginval.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_macOS.zip
                unzip -q -o build/pluginval/pluginval.zip -d build/pluginval
            fi
        fi
    else
        PV="${PLUGINVAL:-build/pluginval/pluginval}"
        if [ ! -x "$PV" ]; then
            mkdir -p build/pluginval
            curl -sSL -o build/pluginval/pluginval.zip https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip
            unzip -q -o build/pluginval/pluginval.zip -d build/pluginval
        fi
    fi
    for plugin in "build/ilanaSynth_artefacts/Release/VST3/ilanaSynth.vst3" "build/ilanaSynthFX_artefacts/Release/VST3/ilanaSynth FX.vst3"; do
        run_gui "$PV" --strictness-level 10 --validate-in-process --timeout-ms 600000 "$plugin" > build/pluginval.txt 2>&1 \
            || { tail -30 build/pluginval.txt; exit 1; }
        echo "$plugin: $(grep -E 'SUCCESS|FAILED' build/pluginval.txt | tail -1)"
    done
fi

step "All checks passed"
