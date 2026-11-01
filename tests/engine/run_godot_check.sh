#!/usr/bin/env bash
# Real-engine check of the Godot exporter.
#   tests/engine/run_godot_check.sh <godot binary> <build dir> <work dir>
# Builds a throwaway Godot 4 project from the fixture, imports it headless,
# loads every exported resource, paints the fixture map with Godot's terrain
# tools and compares the tiles Godot chooses with AceStudio's placement.
set -euo pipefail
GODOT="$1"; BUILD="$2"; WORK="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$WORK"; mkdir -p "$WORK"
cp "$HERE/godot/project.godot" "$HERE/godot/check.gd" "$WORK/"
"$BUILD/tests/engine_fixture" "$WORK/fx" "$HERE/textures/water.png" "$HERE/textures/sand.png" "$HERE/textures/grass.png"
"$GODOT" --headless --path "$WORK" --import > "$WORK/import.log" 2>&1 || { cat "$WORK/import.log"; exit 1; }
"$GODOT" --headless --path "$WORK" --script res://check.gd -- "$WORK/result.json" > "$WORK/check.log" 2>&1 || { cat "$WORK/check.log"; exit 1; }
if grep -q "ERROR" "$WORK/check.log"; then cat "$WORK/check.log"; exit 1; fi
python3 "$HERE/compare_godot.py" "$WORK/fx" "$WORK/result.json"
