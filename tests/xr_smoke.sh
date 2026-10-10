#!/usr/bin/env bash
# Full-pipeline smoke test against Monado's simulated HMD + null compositor:
#   monado-service -> vrlayer-compositor <- vrlayer_test_game (+ capture layer)
#
# usage: xr_smoke.sh <build dir>
# Exits 77 (skipped) when monado-service is not installed.
set -u
B="$(cd "$1" && pwd)"
command -v monado-service >/dev/null || { echo "SKIP: monado-service not installed"; exit 77; }

T="$(mktemp -d)"
export XDG_RUNTIME_DIR="$T/run"
mkdir -m 700 "$XDG_RUNTIME_DIR"
export XDG_CONFIG_HOME="$T/config"
export VRLAYER_SOCKET="vrlayer-smoke-$$"
# Load the shipped profiles from this checkout, exactly like an install would.
export VRLAYER_DATADIR="$(cd "$(dirname "$0")/.." && pwd)"
CTL="$B/vrlayer-ctl"
pids=()
fail=0
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    exec 9>&- 2>/dev/null
    wait 2>/dev/null
    [ "$fail" -ne 0 ] && { echo "---- compositor log"; cat "$T/comp.log"; echo "---- game log"; cat "$T"/game*.log; }
    rm -rf "$T"
}
trap cleanup EXIT

expect() {  # expect <description> <regex> [timeout s]
    local deadline=$((SECONDS + ${3:-10})) out
    while :; do
        out="$("$CTL" status 2>&1)"
        if grep -qE "$2" <<<"$out"; then echo "ok   $1"; return 0; fi
        if [ $SECONDS -ge $deadline ]; then
            echo "FAIL $1 (wanted /$2/)"; echo "$out" | sed 's/^/     /'; fail=1; return 1
        fi
        sleep 0.2
    done
}

game() {  # game <log> <test game mode> [env...]
    local log="$1" mode="$2"; shift 2
    env ENABLE_VRLAYER=1 XDG_DATA_HOME="$B/devroot" SteamAppId=4242 VRLAYER_DEPTH_MIN_DRAWS=1 "$@" \
        "$B/vrlayer_test_game" renderpass $mode >"$T/$log" 2>&1 &
    pids+=($!)
    echo $!
}

# monado-service insists on polling stdin: give it a fifo we hold open.
mkfifo "$T/monado.in"
exec 9<>"$T/monado.in"
XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1 monado-service <"$T/monado.in" >"$T/monado.log" 2>&1 &
pids+=($!)
sleep 2

# 1. Game first: the layer must keep retrying and connect once the compositor is up.
#    The game opens with a main menu that has its own 3D camera, then loads the level.
g1=$(game game1.log menu)
sleep 1.5
"$B/vrlayer-compositor" >"$T/comp.log" 2>&1 &
pids+=($!)
expect "compositor session running" "session running" 15
expect "late compositor picks up running game (gpu)" "frame: 160x120 via gpu, depth yes"
expect "VR loading screen shown" "phase: loading"

# 2. Loading screen: the gameplay camera (not the menu's) is found.
expect "gameplay camera detected (not the menu camera)" "camera: fov 75.* view found" 20
expect "loading screen recommends vr" "recommended vr"
# No /dev/uinput here, so gameplay cannot be probed: it must wait for START
# instead of starting VR on what could be a menu camera.
sleep 7
expect "no auto-start without a gameplay check" "phase: loading \\(analyzing" 2
"$CTL" continue >/dev/null && expect "START: into VR mode" "phase: playing" 10
expect "6DOF mode active" "mode: vr"
expect "head pose injected into the game camera" "injecting \\([0-9]+/frame\\)" 10
"$CTL" set vr.camera alternate >/dev/null && expect "alternate-eye rendering" "camera alternate.*" 5
expect "alternate-eye injection" "injecting \\([0-9]+/frame\\)" 10
"$CTL" set vr.camera head >/dev/null
grep -q "game_fov = 75" "$XDG_CONFIG_HOME/vrlayer/games/4242.auto.ini" 2>/dev/null && echo "ok   detection saved to auto profile" ||
    { echo "FAIL auto profile missing"; fail=1; }
"$CTL" rescan >/dev/null && expect "rescan finds the camera again" "camera: fov 75.* view found" 20

# 3. Back to the loading screen and continue in theater.
"$CTL" loading >/dev/null && expect "loading screen on request" "phase: loading"
expect "injection stops while loading" "view found\$" 10
"$CTL" continue theater >/dev/null && expect "continue in theater" "phase: playing"
expect "theater mode" "mode: theater"

# 2. Live control.
"$CTL" mode immersive >/dev/null && expect "switch to immersive" "mode: immersive"
"$CTL" 3d off >/dev/null && expect "stereo off" "stereo: off"
"$CTL" set display.curved false >/dev/null
"$CTL" mode theater >/dev/null && "$CTL" 3d on >/dev/null && expect "back to theater + 3d" "mode: theater, stereo: on"
if "$CTL" set nope 1 >/dev/null 2>&1; then echo "FAIL bad setting accepted"; fail=1; else echo "ok   bad setting rejected"; fi
"$CTL" save >/dev/null && [ -f "$XDG_CONFIG_HOME/vrlayer/games/4242.ini" ] && echo "ok   profile saved" ||
    { echo "FAIL profile not saved"; fail=1; }

# 3. Game crash: compositor must survive and accept the next game.
kill -9 "$g1"
expect "crashed game dropped" "game: none connected"

# 4. Cpu transport, and the saved profile is applied (curved=false was saved).
game game2.log "" VRLAYER_TRANSPORT=cpu >/dev/null
expect "cpu transport game" "frame: 160x120 via cpu, depth yes"
[ "$("$CTL" get display.curved)" = "display.curved = false" ] && echo "ok   saved profile loaded" ||
    { echo "FAIL saved profile not loaded"; fail=1; }

"$CTL" quit >/dev/null
sleep 1
if grep -q "XR_ERROR" "$T/comp.log"; then echo "FAIL OpenXR errors in compositor log"; fail=1; fi
[ "$fail" -eq 0 ] && echo "PASSED" || echo "FAILED"
exit "$fail"
