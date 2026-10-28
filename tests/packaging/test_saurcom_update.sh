#!/usr/bin/env bash
# packaging/proxmox/saurcom-update in a temporary directory, with a fake docker, curl, systemctl and saurcom-up on the PATH: an update,
# a failed download that leaves the running version alone, a failed start that goes back to the previous image, refused version strings
# (on the command line and in the app's request file), a second run blocked by flock, the automatic mode, the units it installs, and links
# the app could put into its directory.
#   bash tests/packaging/test_saurcom_update.sh [path/to/saurcom-update]
set -uo pipefail
SCRIPT=$(cd "$(dirname "$0")/../.." && pwd)/packaging/proxmox/saurcom-update
[ $# -ge 1 ] && SCRIPT=$1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
pass=0
check() {   # condition; check "what": the exit status of the condition just before
    local r=$? what=$1
    if [ "$r" = 0 ]; then pass=$((pass + 1)); echo "ok   $what"; else fails=$((fails + 1)); echo "FAIL $what"; fi
}

BIN=$T/bin STATE=$T/state D=$T/opt/saurcom
mkdir -p "$BIN" "$STATE" "$D/config" "$T/units" "$T/run"
cat > "$BIN/docker" <<'EOF'
#!/usr/bin/env bash
# the fake docker: pull, inspect, image prune, logs
case "$1" in
    pull) echo "pull $2" >> "$STATE/calls"; sleep "${FAKE_PULL_SLEEP:-0}"
          for b in ${FAKE_PULL_FAIL:-}; do [ "$2" = "ghcr.io/axoldad/saurcom:$b" ] && { echo "manifest unknown" >&2; exit 1; }; done
          echo "pulled $2" ;;
    inspect) cat "$STATE/running" 2>/dev/null || exit 1 ;;
    image) echo "prune" >> "$STATE/calls" ;;
    logs) echo "(the container's last lines)" ;;
    *) echo "fake docker: $*" >&2; exit 1 ;;
esac
EOF
cat > "$BIN/curl" <<'EOF'
#!/usr/bin/env bash
echo "curl $*" >> "$STATE/calls"
printf '{\n  "url": "x",\n  "tag_name": "v%s",\n  "name": "Eye of SaurCom"\n}\n' "$FAKE_LATEST"
EOF
cat > "$BIN/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "systemctl $*" >> "$STATE/systemctl"
EOF
cat > "$BIN/saurcom-up" <<'EOF'
#!/usr/bin/env bash
# the fake saurcom-up: "starts" the image .env names; the images in FAKE_BAD exit at once
img=$(sed -n 's/^SAURCOM_IMAGE=//p' "$SAURCOM_DIR/.env")
echo "up $img" >> "$STATE/calls"
for b in ${FAKE_BAD:-}; do [ "$img" = "ghcr.io/axoldad/saurcom:$b" ] && { echo "exited $img" > "$STATE/running"; exit 0; }; done
echo "running $img" > "$STATE/running"
EOF
chmod 755 "$BIN"/*
export PATH="$BIN:$PATH" STATE SAURCOM_DIR=$D SAURCOM_UNIT_DIR=$T/units SAURCOM_UP=$BIN/saurcom-up SAURCOM_SELF=$SCRIPT \
       SAURCOM_LOCK=$T/run/saurcom-update.lock SAURCOM_START_WAIT=0 SAURCOM_SETTLE=0 FAKE_LATEST=0.3.1
U=$D/config/.saurcom-update
printf 'SAURCOM_IMAGE=ghcr.io/axoldad/saurcom:0.3.0\nSAURCOM_USER=saurcom\nSAURCOM_PASSWORD=secret12\n' > "$D/.env"
chmod 600 "$D/.env"
echo "running ghcr.io/axoldad/saurcom:0.3.0" > "$STATE/running"

image() { sed -n 's/^SAURCOM_IMAGE=//p' "$D/.env"; }
st() { sed -nE "s/.*\"$1\":\"?([^\",}]*)\"?.*/\1/p" "$U/status"; }
show() { local l; while IFS= read -r l; do printf '       | %s\n' "$l"; done <<<"$1"; }
run() { out=$("$@" 2>&1); rc=$?; show "$out"; }

echo "== 1. update to the newest release"
run bash "$SCRIPT"
[ "$rc" = 0 ]; check "exit 0"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env names 0.3.1"
[ "$(cat "$STATE/running")" = "running ghcr.io/axoldad/saurcom:0.3.1" ]; check "the container runs 0.3.1"
[ "$(st state)" = "done" ] && [ "$(st version)" = 0.3.1 ]; check "status done 0.3.1"
[ $(( $(date +%s) - $(st time) )) -lt 10 ]; check "status time is now"
grep -qx 'SAURCOM_PREVIOUS_IMAGE=ghcr.io/axoldad/saurcom:0.3.0' "$D/previous-image"; check "the previous image is recorded"
[ "$(grep -n 'pull ghcr.io/axoldad/saurcom:0.3.1' "$STATE/calls" | cut -d: -f1)" -lt "$(grep -n 'up ghcr.io/axoldad/saurcom:0.3.1' "$STATE/calls" | cut -d: -f1)" ]; check "pulled before starting"
[ "$(stat -c %a "$D/.env")" = 600 ] && grep -qx SAURCOM_PASSWORD=secret12 "$D/.env"; check ".env keeps its mode and the rest"
grep -q '\[done\] Eye of SaurCom 0.3.1 is running.' "$U/log"; check "the log is written"
[ -f "$T/units/saurcom-update.path" ] && [ -f "$T/units/saurcom-update.service" ] && [ -f "$T/units/saurcom-update-auto.service" ] && [ -f "$T/units/saurcom-update-auto.timer" ]; check "the four units are installed"
grep -qx "PathExists=$U/request" "$T/units/saurcom-update.path"; check "the path unit watches the request file"
grep -q 'enable --quiet saurcom-update.path saurcom-update-auto.timer' "$STATE/systemctl" && grep -q 'start saurcom-update.path saurcom-update-auto.timer' "$STATE/systemctl"; check "the units are enabled and started"
if [ "$(id -u)" = 0 ]; then
    [ "$(stat -c '%u:%g %a' "$U")" = "0:1000 1775" ]; check "the directory is root's, group 1000, sticky"
fi
bash -c "head -c1 '$U/status' | grep -q '{' && [ \$(wc -l < '$U/status') = 1 ]"; check "the status is a JSON object on one line"
reloads=$(grep -c daemon-reload "$STATE/systemctl")

echo "== 2. a failed download leaves the running version alone"
: > "$STATE/calls"
FAKE_PULL_FAIL=0.3.2 run bash "$SCRIPT" 0.3.2
[ "$rc" = 1 ]; check "exit 1"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env still names 0.3.1"
! grep -q '^up ' "$STATE/calls"; check "nothing was restarted"
[ "$(st state)" = failed ] && grep -q 'Eye of SaurCom 0.3.1 still runs' "$U/status"; check "status failed, still runs 0.3.1"
[ "$(grep -c daemon-reload "$STATE/systemctl")" = "$reloads" ]; check "the units are not rewritten when unchanged"

echo "== 3. a failed start goes back to the previous image"
: > "$STATE/calls"
FAKE_BAD=0.3.3 run bash "$SCRIPT" v0.3.3
[ "$rc" = 1 ]; check "exit 1"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env names 0.3.1 again"
[ "$(grep '^up ' "$STATE/calls" | tr '\n' ' ')" = "up ghcr.io/axoldad/saurcom:0.3.3 up ghcr.io/axoldad/saurcom:0.3.1 " ]; check "0.3.3 was tried, then 0.3.1 started"
[ "$(cat "$STATE/running")" = "running ghcr.io/axoldad/saurcom:0.3.1" ]; check "the container runs 0.3.1"
[ "$(st state)" = failed ] && grep -q 'did not start. Went back to 0.3.1' "$U/status" && [ "$(st version)" = 0.3.3 ]; check "status failed: went back"
grep -q "the container's last lines" "$U/log"; check "the container's last lines are in the log"

echo "== 4. bad version strings are refused"
: > "$STATE/calls"
# shellcheck disable=SC2016  # (the texts are meant literally)
for bad in '0.3;id' '$(id)' '-rf' '0.3.1-beta.1' '../../x' '1.2.3.4.5' ' 0.3.1'; do
    run bash "$SCRIPT" "$bad"
    [ "$rc" = 2 ]; check "refused: \"$bad\""
done
[ ! -s "$STATE/calls" ]; check "nothing was pulled"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env unchanged"
# shellcheck disable=SC2016
printf '$(touch %s/pwned)\n' "$T" > "$U/request"
run bash "$SCRIPT" --request
[ "$rc" = 2 ]; check "a request that is not a version: exit 2"
[ ! -e "$T/pwned" ]; check "...is not run"
[ ! -e "$U/request" ]; check "...is removed"
[ "$(st state)" = failed ] && grep -q 'not a version' "$U/status"; check "...status failed"
[ ! -s "$STATE/calls" ]; check "...nothing pulled"
ln -s "$D/.env" "$U/request"
run bash "$SCRIPT" --request
[ "$rc" = 0 ] && [ ! -e "$U/request" ] && [ ! -s "$STATE/calls" ] && ! grep -q secret12 "$U/log"; check "a request that is a link is not read"

echo "== 5. the app's request"
printf '0.3.4\n' > "$U/request"
run bash "$SCRIPT" --request
[ "$rc" = 0 ]; check "exit 0"
[ "$(st state)" = "done" ] && [ "$(st version)" = 0.3.4 ] && [ ! -e "$U/request" ]; check "status done 0.3.4, request removed"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.4 ]; check ".env names 0.3.4"
printf 'latest\n' > "$U/request"
FAKE_LATEST=0.3.5 run bash "$SCRIPT" --request
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.5 ] && [ "$(st version)" = 0.3.5 ]; check "\"latest\" in the request: 0.3.5"

echo "== 6. a second update while one runs is blocked by flock"
: > "$STATE/calls"
FAKE_PULL_SLEEP=3 bash "$SCRIPT" 0.3.6 > "$T/first.out" 2>&1 &
first=$!
for _ in $(seq 1 50); do grep -q 'pull ' "$STATE/calls" 2>/dev/null && break; sleep 0.1; done
printf '0.3.7\n' > "$U/request"
run bash "$SCRIPT" 0.3.7
[ "$rc" = 1 ]; check "the second run: exit 1"
grep -q 'another update is running' <<<"$out"; check "...says another update is running"
run bash "$SCRIPT" --request
[ "$rc" = 1 ] && [ ! -e "$U/request" ]; check "a request meanwhile: exit 1, request removed (no service loop)"
[ "$(st state)" = pulling ] && [ "$(st version)" = 0.3.6 ]; check "...and the running update's status stays"
wait "$first"; frc=$?
[ "$frc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ]; check "the first run finished: 0.3.6"
! grep -q 0.3.7 "$STATE/calls"; check "0.3.7 was never pulled"

echo "== 7. automatic updates"
: > "$STATE/calls"
before=$(cat "$U/status")
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ] && [ ! -s "$STATE/calls" ]; check "no auto file: nothing"
echo 0 > "$U/auto"
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ] && [ ! -s "$STATE/calls" ]; check "auto 0: nothing"
echo 1 > "$U/auto"
FAKE_LATEST=0.3.6 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && ! grep -q '^pull' "$STATE/calls" && [ "$(cat "$U/status")" = "$before" ]; check "auto 1, nothing newer: nothing, status untouched"
FAKE_LATEST=0.3.5 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ]; check "auto 1, an older latest: no downgrade"
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.0 ] && [ "$(st state)" = "done" ]; check "auto 1, 0.4.0 is newer: installed"

echo "== 8. links the app could put into its directory"
echo "victim" > "$T/victim"
rm -f "$U/status"; ln -s "$T/victim" "$U/status"
rm -f "$U/log"; ln -s "$T/victim" "$U/log"
run bash "$SCRIPT" 0.4.0
[ "$(cat "$T/victim")" = victim ]; check "the linked file is untouched"
[ -f "$U/status" ] && [ ! -L "$U/status" ] && [ -f "$U/log" ] && [ ! -L "$U/log" ]; check "status and log are plain files again"
mv "$U" "$T/moved"; ln -s "$T/elsewhere" "$U"; mkdir -p "$T/elsewhere"
run bash "$SCRIPT" 0.4.0
[ -z "$(ls -A "$T/elsewhere")" ] && [ -d "$U" ] && [ ! -L "$U" ]; check "a link in place of the directory: nothing written through it"

echo "== 9. the log is capped"
head -c 400000 /dev/zero | tr '\0' 'x' > "$U/log"
run bash "$SCRIPT" 0.4.0
[ "$(stat -c %s "$U/log")" -lt 262144 ]; check "capped below 256 KiB"

echo "== 10. --install-units alone"
rm -f "$T/units/"*
: > "$STATE/calls"
run bash "$SCRIPT" --install-units
[ "$rc" = 0 ] && [ "$(find "$T/units" -type f | wc -l)" = 4 ] && [ ! -s "$STATE/calls" ]; check "the units, and no update"
if command -v systemd-analyze >/dev/null 2>&1; then
    # (the units run SAURCOM_SELF: here the script itself, so that verify finds it)
    v=$(cd "$T/units" && SYSTEMD_LOG_LEVEL=err systemd-analyze verify --man=no ./saurcom-update.path ./saurcom-update.service ./saurcom-update-auto.service ./saurcom-update-auto.timer 2>&1 || true)
    show "$v"
    [ -z "$v" ]; check "systemd-analyze verify: no complaints"
fi

echo
echo "$pass passed, $fails failed"
[ "$fails" = 0 ]
