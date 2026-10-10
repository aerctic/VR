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

game() {  # game <log> [env...]
    local log="$1"; shift
    env ENABLE_VRLAYER=1 XDG_DATA_HOME="$B/devroot" SteamAppId=4242 VRLAYER_DEPTH_MIN_DRAWS=1 "$@" \
        "$B/vrlayer_test_game" renderpass >"$T/$log" 2>&1 &
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
g1=$(game game1.log)
sleep 1.5
"$B/vrlayer-compositor" >"$T/comp.log" 2>&1 &
pids+=($!)
expect "compositor session running" "session running" 15
expect "late compositor picks up running game (gpu)" "frame: 160x120 via gpu, depth yes"
expect "stereo active" "stereo: on"

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
game game2.log VRLAYER_TRANSPORT=cpu >/dev/null
expect "cpu transport game" "frame: 160x120 via cpu, depth yes"
[ "$("$CTL" get display.curved)" = "display.curved = false" ] && echo "ok   saved profile loaded" ||
    { echo "FAIL saved profile not loaded"; fail=1; }

"$CTL" quit >/dev/null
sleep 1
if grep -q "XR_ERROR" "$T/comp.log"; then echo "FAIL OpenXR errors in compositor log"; fail=1; fi
[ "$fail" -eq 0 ] && echo "PASSED" || echo "FAILED"
exit "$fail"
