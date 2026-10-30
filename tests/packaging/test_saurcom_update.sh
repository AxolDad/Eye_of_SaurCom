#!/usr/bin/env bash
# packaging/proxmox/saurcom-update in a temporary directory, with a fake docker, curl, df, systemctl and saurcom-up on the PATH:
#  - an update; a failed download that leaves the running version alone; a failed start that goes back to the image that ran
#  - refused version strings (command line and request file), and requests that are links, hard links or not the app's
#  - the app's request, "latest", a version already running, a downgrade refused from the app but allowed on the command line
#  - a second run blocked by flock, an early exit that still clears the request, a full disk, a stop in the middle
#  - the automatic mode, the units, the read-only mount added to compose.yaml, the clean-up of old images, the log cap
#  - root never writing into the app's directory, whatever links the app puts there; a request that is a directory, socket or worse
#  - the heartbeat of a busy status; an image without a version tag (only "latest" from the app); the mount moved off /run
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
mkdir -p "$BIN" "$STATE" "$D/config" "$T/units" "$T/run" "$T/dockerroot"
cat > "$BIN/docker" <<'EOF'
#!/usr/bin/env bash
# the fake docker. $STATE/running: "<status> <image>" of the container saurcom; $STATE/images: the images it has
case "$1" in
    pull) echo "pull $2" >> "$STATE/calls"; sleep "${FAKE_PULL_SLEEP:-0}"
          for b in ${FAKE_PULL_FAIL:-}; do [ "$2" = "ghcr.io/axoldad/saurcom:$b" ] && { echo "manifest unknown" >&2; exit 1; }; done
          grep -qxF "$2" "$STATE/images" || echo "$2" >> "$STATE/images"
          echo "pulled $2"
          [ -n "${FAKE_PULL_FILL:-}" ] && dd if=/dev/zero of="$FAKE_PULL_FILL/filler" bs=4k 2>/dev/null   # the download fills the disk
          exit 0 ;;
    inspect)
        [ -s "$STATE/running" ] || exit 1
        read -r st img < "$STATE/running"
        case "$3" in
            *State.Status*Config.Image*) echo "$st $img" ;;
            *Config.Image*) echo "$img" ;;
            *State.Status*) echo "$st" ;;
        esac ;;
    image)
        case "$2" in
            inspect) if [ "$3" = -f ]; then grep -qxF "$5" "$STATE/images" || exit 1; echo "${FAKE_LABEL:-}"   # (the version label)
                     else grep -qxF "$3" "$STATE/images"; fi ;;
            ls) cat "$STATE/images" ;;
            rm) grep -vxF "$3" "$STATE/images" > "$STATE/images.new"; mv "$STATE/images.new" "$STATE/images"; echo "rm $3" >> "$STATE/calls" ;;
            prune) echo "prune" >> "$STATE/calls" ;;
        esac ;;
    info) echo "$FAKE_DOCKER_ROOT" ;;
    logs) echo "(the container's last lines)" ;;
    *) echo "fake docker: $*" >&2; exit 1 ;;
esac
EOF
cat > "$BIN/curl" <<'EOF'
#!/usr/bin/env bash
echo "curl $*" >> "$STATE/calls"
printf '{\n  "url": "x",\n  "tag_name": "v%s",\n  "name": "Eye of SaurCom"\n}\n' "$FAKE_LATEST"
EOF
cat > "$BIN/df" <<'EOF'
#!/usr/bin/env bash
echo "df $*" >> "$STATE/calls"
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\n/dev/fake 20000000 1000 %s 1%% /\n' "${FAKE_DF_AVAIL:-10000000}"
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
if [ "$(id -u)" = 0 ]; then APPUID=1000; else APPUID=$(id -u); fi
export PATH="$BIN:$PATH" STATE SAURCOM_DIR=$D SAURCOM_UNIT_DIR=$T/units SAURCOM_UP=$BIN/saurcom-up SAURCOM_SELF=$SCRIPT \
       SAURCOM_LOCK=$T/run/saurcom-update.lock SAURCOM_START_WAIT=0 SAURCOM_SETTLE=0 SAURCOM_APP_UID=$APPUID SAURCOM_BUSY_WAIT=0 \
       FAKE_LATEST=0.3.1 FAKE_DOCKER_ROOT=$T/dockerroot
U=$D/config/.saurcom-update S=$D/update
printf 'SAURCOM_IMAGE=ghcr.io/axoldad/saurcom:0.3.0\nSAURCOM_USER=saurcom\nSAURCOM_PASSWORD=secret12\nTZ=Etc/UTC\n' > "$D/.env"
chmod 600 "$D/.env"
cat > "$D/compose.yaml" <<'EOF'
services:
  saurcom:
    image: ${SAURCOM_IMAGE}
    volumes:
      - ./config:/config      # settings, recordings and saurcom.log
      - /dev/bus/usb:/dev/bus/usb   # the radios
EOF
echo "running ghcr.io/axoldad/saurcom:0.3.0" > "$STATE/running"
echo "ghcr.io/axoldad/saurcom:0.3.0" > "$STATE/images"
mkdir -p "$U"
[ "$(id -u)" = 0 ] && chown -R "$APPUID:$APPUID" "$D/config"

image() { sed -n 's/^SAURCOM_IMAGE=//p' "$D/.env"; }
st() { sed -nE "s/.*\"$1\":\"?([^\",}]*)\"?.*/\1/p" "$S/status"; }
show() { local l; while IFS= read -r l; do printf '       | %s\n' "$l"; done <<<"$1"; }
run() { out=$("$@" 2>&1); rc=$?; show "$out"; }
# a file of the app's in its directory, owned by the app's user
app_put() { printf '%s\n' "$2" > "$U/$1"; [ "$(id -u)" = 0 ] && chown "$APPUID:$APPUID" "$U/$1"; return 0; }
# nothing in the app's directory belongs to anyone but the app
app_dir_clean() { [ -z "$(find "$D/config" ! -user "$APPUID" -print -quit)" ]; }

echo "== 1. update to the newest release"
run bash "$SCRIPT"
[ "$rc" = 0 ]; check "exit 0"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env names 0.3.1"
[ "$(cat "$STATE/running")" = "running ghcr.io/axoldad/saurcom:0.3.1" ]; check "the container runs 0.3.1"
[ "$(st state)" = "done" ] && [ "$(st version)" = 0.3.1 ]; check "status done 0.3.1, in root's directory"
[ $(( $(date +%s) - $(st time) )) -lt 10 ]; check "status time is now"
grep -qx 'SAURCOM_PREVIOUS_IMAGE=ghcr.io/axoldad/saurcom:0.3.0' "$S/previous-image"; check "the previous image is recorded"
[ "$(grep -n 'pull ghcr.io/axoldad/saurcom:0.3.1' "$STATE/calls" | cut -d: -f1)" -lt "$(grep -n 'up ghcr.io/axoldad/saurcom:0.3.1' "$STATE/calls" | cut -d: -f1)" ]; check "pulled before starting"
grep -q '^df ' "$STATE/calls"; check "the free disk space was checked"
[ "$(stat -c %a "$D/.env")" = 600 ] && grep -qx SAURCOM_PASSWORD=secret12 "$D/.env" && grep -qx TZ=Etc/UTC "$D/.env"; check ".env keeps its mode and the rest"
grep -q '\[done\] Eye of SaurCom 0.3.1 is running.' "$S/log"; check "the log is written"
if [ "$(id -u)" = 0 ]; then
    [ "$(stat -c '%u:%g %a' "$S")" = "0:0 755" ] && [ "$(stat -c '%u %a' "$S/status")" = "0 644" ]; check "the status directory is root's, 0755: the app cannot write there"
    if command -v setpriv >/dev/null 2>&1; then
        ! setpriv --reuid="$APPUID" --regid="$APPUID" --clear-groups sh -c "echo x > '$S/status' || exit 1; :" 2>/dev/null \
            && ! setpriv --reuid="$APPUID" --regid="$APPUID" --clear-groups sh -c "ln -s /etc '$S/planted'" 2>/dev/null && [ ! -e "$S/planted" ]
        check "...tried as the app's user: no write, no link"
    fi
fi
app_dir_clean; check "root wrote nothing into the app's directory"
grep -qE '^      - \./update:/saurcom-update:ro' "$D/compose.yaml"; check "compose.yaml mounts the status directory read-only"
[ -f "$T/units/saurcom-update.path" ] && [ -f "$T/units/saurcom-update.service" ] && [ -f "$T/units/saurcom-update-auto.service" ] && [ -f "$T/units/saurcom-update-auto.timer" ]; check "the four units are installed"
grep -qx "PathExists=$U/request" "$T/units/saurcom-update.path"; check "the path unit watches the request file"
grep -qx 'StartLimitIntervalSec=0' "$T/units/saurcom-update.service" && grep -qx 'Restart=no' "$T/units/saurcom-update.service"; check "the service has no start limit and no restart"
grep -q 'reset-failed saurcom-update.path' "$STATE/systemctl" && grep -q 'enable --quiet saurcom-update.path saurcom-update-auto.timer' "$STATE/systemctl"; check "units reset, enabled and started"
[ "$(wc -l < "$S/status")" = 1 ] && head -c1 "$S/status" | grep -q '{'; check "the status is a JSON object on one line"
reloads=$(grep -c daemon-reload "$STATE/systemctl")

echo "== 2. a failed download leaves the running version alone"
: > "$STATE/calls"
FAKE_PULL_FAIL=0.3.2 run bash "$SCRIPT" 0.3.2
[ "$rc" = 1 ]; check "exit 1"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env still names 0.3.1"
! grep -q '^up ' "$STATE/calls"; check "nothing was restarted"
[ "$(st state)" = failed ] && grep -q 'Eye of SaurCom 0.3.1 still runs' "$S/status"; check "status failed, still runs 0.3.1"
[ "$(grep -c daemon-reload "$STATE/systemctl")" = "$reloads" ]; check "the units are not rewritten when unchanged"
[ "$(grep -c ':/saurcom-update' "$D/compose.yaml")" = 1 ]; check "the mount is added once"

echo "== 3. a failed start goes back to the image that ran (not what .env said)"
: > "$STATE/calls"
sed -i 's#^SAURCOM_IMAGE=.*#SAURCOM_IMAGE="ghcr.io/axoldad/saurcom:0.2.9"#' "$D/.env"   # edited by hand, quoted, never started
FAKE_BAD=0.3.3 run bash "$SCRIPT" v0.3.3
[ "$rc" = 1 ]; check "exit 1"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env names 0.3.1, the image that ran"
[ "$(grep '^up ' "$STATE/calls" | tr '\n' ' ')" = "up ghcr.io/axoldad/saurcom:0.3.3 up ghcr.io/axoldad/saurcom:0.3.1 " ]; check "0.3.3 was tried, then 0.3.1 started"
[ "$(cat "$STATE/running")" = "running ghcr.io/axoldad/saurcom:0.3.1" ]; check "the container runs 0.3.1"
[ "$(st state)" = failed ] && grep -q 'did not start. Went back to 0.3.1' "$S/status" && [ "$(st version)" = 0.3.3 ]; check "status failed: went back"
grep -q "the container's last lines" "$S/log"; check "the container's last lines are in the log"
: > "$STATE/calls"
grep -vxF ghcr.io/axoldad/saurcom:0.3.1 "$STATE/images" > "$STATE/i"; mv "$STATE/i" "$STATE/images"   # the running image is gone
FAKE_BAD=0.3.3 run bash "$SCRIPT" 0.3.3
[ "$rc" = 1 ] && [ "$(grep -c '^up ' "$STATE/calls")" = 1 ] && grep -q 'no way back' "$S/log"; check "the running image is not here: no switch back to a missing image"
echo "running ghcr.io/axoldad/saurcom:0.3.1" > "$STATE/running"; echo ghcr.io/axoldad/saurcom:0.3.1 >> "$STATE/images"
sed -i 's#^SAURCOM_IMAGE=.*#SAURCOM_IMAGE=ghcr.io/axoldad/saurcom:0.3.1#' "$D/.env"

echo "== 4. bad version strings and bad requests are refused"
: > "$STATE/calls"
# shellcheck disable=SC2016  # (the texts are meant literally)
for bad in '0.3;id' '$(id)' '-rf' '0.3.1-beta.1' '../../x' '1.2.3.4.5' ' 0.3.1'; do
    run bash "$SCRIPT" "$bad"
    [ "$rc" = 2 ]; check "refused: \"$bad\""
done
[ ! -s "$STATE/calls" ]; check "nothing was pulled"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.1 ]; check ".env unchanged"
# shellcheck disable=SC2016
app_put request "$(printf '$(touch %s/pwned)' "$T")"
run bash "$SCRIPT" --request
[ "$rc" = 2 ]; check "a request that is not a version: exit 2"
[ ! -e "$T/pwned" ]; check "...is not run"
[ ! -e "$U/request" ]; check "...is removed"
[ "$(st state)" = failed ] && grep -q 'not a version' "$S/status"; check "...status failed"
ln -s "$D/.env" "$U/request"
run bash "$SCRIPT" --request
[ "$rc" = 2 ] && [ ! -e "$U/request" ] && [ ! -L "$U/request" ] && grep -q 'not a plain file' "$S/status" && ! grep -q secret12 "$S/log"; check "a request that is a link: not read, removed, refused"
app_put other "0.3.9"; ln "$U/other" "$U/request"
run bash "$SCRIPT" --request
[ "$rc" = 2 ] && [ ! -e "$U/request" ] && [ -f "$U/other" ] && grep -q 'not a plain file' "$S/status"; check "a request that is a hard link: refused, removed (the other name kept)"
rm -f "$U/other"
if [ "$(id -u)" = 0 ]; then
    printf '0.3.9\n' > "$U/request"; chown 0:0 "$U/request"
    run bash "$SCRIPT" --request
    [ "$rc" = 2 ] && [ ! -e "$U/request" ]; check "a request not owned by the app: refused, removed"
fi
head -c 100 /dev/zero | tr '\0' '1' > "$U/request"; [ "$(id -u)" = 0 ] && chown "$APPUID" "$U/request"
run bash "$SCRIPT" --request
[ "$rc" = 2 ] && [ ! -e "$U/request" ]; check "a request over 64 bytes: refused, removed"
mkfifo "$U/request"; [ "$(id -u)" = 0 ] && chown "$APPUID" "$U/request"
run timeout 20 bash "$SCRIPT" --request
[ "$rc" = 2 ] && [ ! -e "$U/request" ]; check "a request that is a pipe: no wait, refused, removed"
[ ! -s "$STATE/calls" ]; check "...nothing pulled for any of them"

echo "== 5. the app's request"
app_put request 0.3.4
run bash "$SCRIPT" --request
[ "$rc" = 0 ]; check "exit 0"
[ "$(st state)" = "done" ] && [ "$(st version)" = 0.3.4 ] && [ ! -e "$U/request" ]; check "status done 0.3.4, request removed"
[ "$(image)" = ghcr.io/axoldad/saurcom:0.3.4 ]; check ".env names 0.3.4"
app_put request latest
FAKE_LATEST=0.3.5 run bash "$SCRIPT" --request
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.5 ] && [ "$(st version)" = 0.3.5 ]; check "\"latest\" in the request: 0.3.5"
: > "$STATE/calls"
app_put request 0.3.5
run bash "$SCRIPT" --request
[ "$rc" = 0 ] && [ "$(st state)" = "done" ] && grep -q 'running already' "$S/status" && ! grep -q '^pull' "$STATE/calls"; check "the running version again: done at once, nothing pulled"
app_dir_clean; check "root wrote nothing into the app's directory"

echo "== 6. going back: refused from the app, allowed on the command line"
: > "$STATE/calls"
app_put request 0.3.4
run bash "$SCRIPT" --request
[ "$rc" = 1 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.5 ] && grep -q 'Use the command line to go back to an older version' "$S/status" && [ ! -e "$U/request" ]; check "a request for an older version: refused"
! grep -q '^pull' "$STATE/calls"; check "...nothing pulled"
run bash "$SCRIPT" 0.3.4
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.4 ]; check "saurcom-update 0.3.4 in a shell: allowed"
run bash "$SCRIPT" 0.3.5
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.5 ]; check "(and forward again)"

echo "== 7. a second update while one runs is blocked by flock"
: > "$STATE/calls"
FAKE_PULL_SLEEP=3 bash "$SCRIPT" 0.3.6 > "$T/first.out" 2>&1 &
first=$!
for _ in $(seq 1 50); do grep -q 'pull ' "$STATE/calls" 2>/dev/null && break; sleep 0.1; done
sleep 1
run bash "$SCRIPT" 0.3.7
[ "$rc" = 1 ] && grep -q 'another update is running' <<<"$out"; check "the second run: exit 1, says another update is running"
before=$(st time)
sleep 1
app_put request 0.3.7
run bash "$SCRIPT" --request
[ "$rc" = 1 ] && [ ! -e "$U/request" ]; check "a request meanwhile: removed (no service loop)"
[ "$(st state)" = pulling ] && [ "$(st version)" = 0.3.6 ] && [ "$(st time)" -gt "$before" ]; check "...answered with the running update's progress, time renewed"
wait "$first"; frc=$?
[ "$frc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ]; check "the first run finished: 0.3.6"
! grep -q 0.3.7 "$STATE/calls"; check "0.3.7 was never pulled"
exec 7>"$T/run/saurcom-update.lock"; flock 7      # an update that has written nothing yet
app_put request 0.3.7
run bash "$SCRIPT" --request
exec 7>&-
[ "$(st state)" = failed ] && grep -q 'An update is already running' "$S/status"; check "...or, before it wrote anything: \"An update is already running\""

echo "== 8. automatic updates"
: > "$STATE/calls"
before=$(cat "$S/status")
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ] && [ ! -s "$STATE/calls" ]; check "no auto file: nothing"
app_put auto 0
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ] && [ ! -s "$STATE/calls" ]; check "auto 0: nothing"
rm -f "$U/auto"; ln -s "$T/one" "$U/auto"; echo 1 > "$T/one"
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ]; check "auto as a link to a file holding 1: not read"
rm -f "$U/auto"; app_put auto 1
FAKE_LATEST=0.3.6 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && ! grep -q '^pull' "$STATE/calls" && [ "$(cat "$S/status")" = "$before" ]; check "auto 1, nothing newer: nothing, status untouched"
FAKE_LATEST=0.3.5 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.3.6 ]; check "auto 1, an older latest: no downgrade"
FAKE_LATEST=0.4.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.0 ] && [ "$(st state)" = "done" ]; check "auto 1, 0.4.0 is newer: installed"
echo "running saurcom:mine" > "$STATE/running"
FAKE_LATEST=0.5.0 run bash "$SCRIPT" --auto
[ "$rc" = 0 ] && grep -q 'not a release' <<<"$out" && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.0 ]; check "auto with an image of one's own: left alone"
echo "running ghcr.io/axoldad/saurcom:0.4.0" > "$STATE/running"

echo "== 9. a full disk"
: > "$STATE/calls"
FAKE_DF_AVAIL=500000 run bash "$SCRIPT" 0.4.1
[ "$rc" = 1 ] && [ "$(st state)" = failed ] && grep -q 'Not enough disk space' "$S/status"; check "refused: not enough disk space"
! grep -q '^pull' "$STATE/calls" && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.0 ]; check "...nothing pulled, .env unchanged"

echo "== 10. an early exit still clears the request"
mv "$D/.env" "$D/.env.away"
app_put request 0.4.1
run bash "$SCRIPT" --request
[ "$rc" = 1 ] && [ ! -e "$U/request" ] && grep -q '.env not found' "$S/status"; check "no .env: request removed, status failed"
mv "$D/.env.away" "$D/.env"

echo "== 11. links the app could put into its directory"
mkdir -p "$T/victimdir"; echo victim > "$T/victim"
mv "$U" "$D/config/.old"; mkdir "$U"
ln -s "$T/victimdir" "$U/status"; ln -s "$T/victim" "$U/log"
[ "$(id -u)" = 0 ] && chown -h "$APPUID:$APPUID" "$U" "$U/status" "$U/log"
run bash "$SCRIPT" 0.4.1
[ -z "$(ls -A "$T/victimdir")" ] && [ "$(cat "$T/victim")" = victim ]; check "status and log as links (also to a directory): nothing written through them"
app_dir_clean; check "root wrote nothing into the app's directory"
rm -rf "$U" "$D/config/.old"; ln -s "$T/victimdir" "$U"
app_put_link() { printf '0.4.2\n' > "$T/victimdir/request"; [ "$(id -u)" = 0 ] && chown "$APPUID" "$T/victimdir/request"; }
app_put_link
run bash "$SCRIPT" --request
[ ! -L "$U" ] && [ -f "$T/victimdir/request" ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.1 ]; check "a link in place of the directory: removed, the request behind it not read"
mkdir -p "$U"; [ "$(id -u)" = 0 ] && chown "$APPUID:$APPUID" "$U"

echo "== 12. old images are removed: the current one and the one before are kept"
grep -c . "$STATE/images" > /dev/null
[ "$(sort "$STATE/images" | tr '\n' ' ')" = "ghcr.io/axoldad/saurcom:0.4.0 ghcr.io/axoldad/saurcom:0.4.1 " ]; check "left: 0.4.0 and 0.4.1"
grep -q '^rm ghcr.io/axoldad/saurcom:0.3.6' "$STATE/calls"; check "...0.3.6 was removed"

echo "== 13. stopped in the middle"
FAKE_PULL_SLEEP=5 bash "$SCRIPT" 0.4.2 > "$T/stop.out" 2>&1 &
p=$!
for _ in $(seq 1 50); do grep -q 'pull ghcr.io/axoldad/saurcom:0.4.2' "$STATE/calls" 2>/dev/null && break; sleep 0.1; done
kill -TERM "$p"; wait "$p"; src=$?
[ "$src" != 0 ] && [ "$(st state)" = failed ] && grep -q 'stopped during the download' "$S/status" && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.1 ]; check "a stop during the download: status failed, nothing changed"
sleep 5

echo "== 14. the log is capped"
head -c 400000 /dev/zero | tr '\0' 'x' > "$S/log"
run bash "$SCRIPT" 0.4.1
[ "$(stat -c %s "$S/log")" -lt 262144 ]; check "capped below 256 KiB"

echo "== 15. --install-units alone"
rm -f "$T/units/"*
: > "$STATE/calls"
run bash "$SCRIPT" --install-units
[ "$rc" = 0 ] && [ "$(find "$T/units" -type f | wc -l)" = 4 ] && [ ! -s "$STATE/calls" ]; check "the units, and no update"
[ -z "$(find "$T/units" -name '.*')" ]; check "no temporary unit files left"
if command -v systemd-analyze >/dev/null 2>&1; then
    # (the units run SAURCOM_SELF: here the script itself, so that verify finds it)
    v=$(cd "$T/units" && SYSTEMD_LOG_LEVEL=err systemd-analyze verify --man=no ./saurcom-update.path ./saurcom-update.service ./saurcom-update-auto.service ./saurcom-update-auto.timer 2>&1); vrc=$?
    show "$v"
    [ -z "$v" ] && [ "$vrc" = 0 ]; check "systemd-analyze verify: no complaints"
fi

echo "== 16. the installer's compose.yaml already has the read-only mount"
SETUP=$(dirname "$SCRIPT")/saurcom-lxc-setup.sh
if [ -f "$SETUP" ]; then
    cp "$D/compose.yaml" "$T/compose.keep"
    # shellcheck disable=SC2016  # (the texts are meant literally)
    sed -n '/^cat > "$D\/compose.yaml" <</,/^EOF$/p' "$SETUP" | sed '1d;$d' > "$D/compose.yaml"
    # shellcheck disable=SC2016
    grep -qE '^      - \./update:/saurcom-update:ro' "$D/compose.yaml" && grep -q 'install -d -m 0755 -o 0 -g 0 "$D/update"' "$SETUP"
    check "saurcom-lxc-setup.sh: the mount, and update/ made as root's"
    run bash "$SCRIPT" --install-units
    [ "$rc" = 0 ] && [ "$(grep -c ':/saurcom-update' "$D/compose.yaml")" = 1 ]; check "...not added a second time"
    [ "$(grep -cE '^      SAURCOM_(LOCATOR|LAT|LON|ALT): \$\{SAURCOM_' "$D/compose.yaml")" = 4 ]; check "saurcom-lxc-setup.sh passes the station location on, and it is not added again"
    cp "$T/compose.keep" "$D/compose.yaml"
fi

echo "== 17. a disk that fills up while .env is rewritten"
# (a tiny tmpfs for the whole install: only as root, where mount works)
F=$T/full
if [ "$(id -u)" = 0 ] && mkdir -p "$F" && mount -t tmpfs -o size=256k tmpfs "$F" 2>/dev/null; then
    trap 'umount "$F" 2>/dev/null; rm -rf "$T"' EXIT
    mkdir -p "$F/config/.saurcom-update"; cp -p "$D/.env" "$D/compose.yaml" "$F/"; chown -R "$APPUID:$APPUID" "$F/config"
    before=$(cat "$F/.env")
    : > "$STATE/calls"
    FAKE_PULL_FILL=$F SAURCOM_DIR=$F run bash "$SCRIPT" 0.4.2
    [ "$rc" = 1 ] && [ "$(cat "$F/.env")" = "$before" ] && ! grep -q '^up ' "$STATE/calls" && grep -q 'Could not write .*\.env' <<<"$out"
    check "the new .env cannot be written: exit 1, the old .env kept whole, nothing restarted"
    [ -z "$(find "$F" -maxdepth 1 -name '.env.*')" ]; check "...no temporary .env left"
    umount "$F"
else
    echo "(skipped: needs root and tmpfs)"
fi

echo "== 18. a request that is not a file: a directory, a socket, one that cannot be removed"
mkdir -p "$T/victim2"; echo keep > "$T/victim2/file"
mkdir -p "$U/request/sub"; echo 0.4.2 > "$U/request/sub/x"; ln -s "$T/victim2" "$U/request/link"; ln -s "$T/victim2/file" "$U/request/sub/flink"
[ "$(id -u)" = 0 ] && chown -hR "$APPUID:$APPUID" "$U/request"
: > "$STATE/calls"
run bash "$SCRIPT" --request
[ "$rc" = 2 ] && [ ! -e "$U/request" ] && grep -q 'not a plain file' "$S/status"; check "a directory: removed, refused (no loop)"
[ "$(cat "$T/victim2/file")" = keep ] && [ -d "$T/victim2" ] && ! grep -q '^pull' "$STATE/calls"; check "...nothing behind its links removed, nothing pulled"
if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$U/request"
    [ "$(id -u)" = 0 ] && chown -h "$APPUID" "$U/request"
    run bash "$SCRIPT" --request
    [ "$rc" = 2 ] && [ ! -e "$U/request" ]; check "a socket: removed, refused"
fi
if [ "$(id -u)" = 0 ] && mkdir -p "$U/request/m" && mount -t tmpfs tmpfs "$U/request/m" 2>/dev/null; then
    t0=$(date +%s)
    SAURCOM_BUSY_WAIT=2 run bash "$SCRIPT" --request
    el=$(( $(date +%s) - t0 ))
    umount "$U/request/m"
    [ "$rc" = 1 ] && grep -q 'could not be removed' "$S/status" && [ "$el" -ge 2 ]; check "a directory that cannot be removed (a mount in it): status, then a pause before the exit ($el s)"
    rm -rf "$U/request"
fi
rm -rf "$U/request"

echo "== 19. the heartbeat: a long download keeps its status fresh, and stops with the run"
: > "$STATE/calls"
SAURCOM_HEARTBEAT=1 FAKE_PULL_SLEEP=4 bash "$SCRIPT" 0.4.2 > "$T/hb.out" 2>&1 &
p=$!
for _ in $(seq 1 50); do grep -q 'pull ghcr.io/axoldad/saurcom:0.4.2' "$STATE/calls" 2>/dev/null && break; sleep 0.1; done
t1=$(st time)
sleep 2.5
[ "$(st state)" = pulling ] && [ "$(st time)" -gt "$t1" ] && grep -q 'Downloading Eye of SaurCom 0.4.2' "$S/status"; check "while pulling: the same status, a newer time"
wait "$p"; prc=$?
final=$(cat "$S/status")
sleep 2
[ "$prc" = 0 ] && [ "$(st state)" = "done" ] && [ "$(cat "$S/status")" = "$final" ]; check "after the run: done, and nothing renews it any more"
[ "$(grep -c 'Downloading' "$S/log")" -le "$(grep -c '\[pulling\]' "$S/log")" ]; check "the heartbeat does not fill the log"
: > "$STATE/calls"
SAURCOM_HEARTBEAT=1 FAKE_PULL_SLEEP=5 bash "$SCRIPT" 0.4.3 > "$T/hb2.out" 2>&1 &
p=$!
for _ in $(seq 1 50); do grep -q 'pull ghcr.io/axoldad/saurcom:0.4.3' "$STATE/calls" 2>/dev/null && break; sleep 0.1; done
kill -TERM "$p"; wait "$p"
stopped=$(cat "$S/status")
sleep 2
[ "$(st state)" = failed ] && [ "$(cat "$S/status")" = "$stopped" ]; check "stopped: failed, and the heartbeat stopped with it"
[ -z "$(find "$S" -name '.status.??????')" ]; check "...no temporary status files left"
sleep 4   # (the fake pull's sleep)

echo "== 20. an image whose version is not known (:latest): only \"latest\", and only if newer than its label"
cp "$D/.env" "$T/env.keep"
echo "running ghcr.io/axoldad/saurcom:latest" > "$STATE/running"; echo ghcr.io/axoldad/saurcom:latest >> "$STATE/images"
sed -i 's#^SAURCOM_IMAGE=.*#SAURCOM_IMAGE=ghcr.io/axoldad/saurcom:latest#' "$D/.env"
: > "$STATE/calls"
app_put request 0.1.0
run bash "$SCRIPT" --request
[ "$rc" = 1 ] && grep -q 'has no version tag' "$S/status" && ! grep -q '^pull' "$STATE/calls" && [ "$(image)" = ghcr.io/axoldad/saurcom:latest ]; check "a given version: refused, nothing pulled"
app_put request latest
FAKE_LABEL=0.4.5 FAKE_LATEST=0.4.5 run bash "$SCRIPT" --request
[ "$rc" = 1 ] && grep -q 'not newer than the running image (0.4.5)' "$S/status" && ! grep -q '^pull' "$STATE/calls"; check "latest, not newer than the label: refused"
app_put request latest
FAKE_LABEL=0.4.5 FAKE_LATEST=0.4.6 run bash "$SCRIPT" --request
[ "$rc" = 0 ] && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.6 ]; check "latest, newer than the label: installed"
cp "$T/env.keep" "$D/.env"; echo "running $(image)" > "$STATE/running"

echo "== 21. the mount where a test build put it (/run) is moved"
sed -i 's#\./update:/saurcom-update:ro#./update:/run/saurcom-update:ro#' "$D/compose.yaml"
chmod 600 "$D/compose.yaml"
run bash "$SCRIPT" --install-units
grep -q '\./update:/saurcom-update:ro' "$D/compose.yaml" && ! grep -q '/run/saurcom-update' "$D/compose.yaml" && [ "$(stat -c %a "$D/compose.yaml")" = 600 ]; check "moved to /saurcom-update, the file's mode kept"

echo "== 22. an older compose.yaml gets the station location pass-through; .env keeps the location through an update"
cp "$D/compose.yaml" "$T/compose.keep22"; cp "$D/.env" "$T/env.keep22"
cat > "$D/compose.yaml" <<'EOF'
services:
  saurcom:
    image: ${SAURCOM_IMAGE}
    environment:
      CUSTOM_USER: ${SAURCOM_USER}
      PASSWORD: ${SAURCOM_PASSWORD}
    volumes:
      - ./config:/config      # settings, recordings and saurcom.log
EOF
chmod 640 "$D/compose.yaml"
printf '%s\n' "SAURCOM_LAT=52.100000" "SAURCOM_LON=-1.300000" >> "$D/.env"
run bash "$SCRIPT" --install-units
[ "$rc" = 0 ]; check "exit 0"
# shellcheck disable=SC2016  # (the texts are meant literally)
grep -A5 '^      PASSWORD: ' "$D/compose.yaml" | grep -qx '      SAURCOM_LAT: ${SAURCOM_LAT:-}' && grep -qx '      SAURCOM_LOCATOR: ${SAURCOM_LOCATOR:-}' "$D/compose.yaml" \
    && grep -qx '      SAURCOM_LON: ${SAURCOM_LON:-}' "$D/compose.yaml" && grep -qx '      SAURCOM_ALT: ${SAURCOM_ALT:-}' "$D/compose.yaml"
check "the four location variables are passed to the app, under PASSWORD"
[ "$(stat -c %a "$D/compose.yaml")" = 640 ]; check "compose.yaml keeps its mode"
run bash "$SCRIPT" --install-units
[ "$(grep -c '^      SAURCOM_LAT:' "$D/compose.yaml")" = 1 ]; check "added once"
echo "running $(image)" > "$STATE/running"
FAKE_LATEST=0.4.7 run bash "$SCRIPT"
[ "$rc" = 0 ] && grep -qx SAURCOM_LAT=52.100000 "$D/.env" && grep -qx SAURCOM_LON=-1.300000 "$D/.env" && [ "$(image)" = ghcr.io/axoldad/saurcom:0.4.7 ]
check "an update keeps SAURCOM_LAT / SAURCOM_LON in .env"
cp "$T/compose.keep22" "$D/compose.yaml"; cp "$T/env.keep22" "$D/.env"; echo "running $(image)" > "$STATE/running"

echo
echo "$pass passed, $fails failed"
[ "$fails" = 0 ]
