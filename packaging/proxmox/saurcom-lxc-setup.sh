#!/usr/bin/env bash
# Inside the Proxmox container that saurcom-lxc.sh made: Docker, and Eye of SaurCom's image (packaging/docker) as a service that starts
# with the container. saurcom-lxc.sh runs it with these in the environment:
#   SAURCOM_IMAGE      ghcr.io/axoldad/saurcom:<version>
#   SAURCOM_USER, SAURCOM_PASSWORD   the login of the web page
#   TZ                 the node's time zone
#   SAURCOM_GPU        /dev/dri/renderD128 when a graphics chip is passed through, else empty
#   SAURCOM_SERIAL     1: pass the container's USB serial ports on to the app
#   SAURCOM_ASSET_REF  the branch or tag saurcom-update comes from if saurcom-lxc.sh did not put it in place (default main)
#
# The devices are bound into the app's container as volumes, not with --device: Docker in an unprivileged container cannot make device
# files (mknod), and a bound directory also shows a radio plugged in later.
set -Eeuo pipefail
: "${SAURCOM_IMAGE:?}" "${SAURCOM_USER:?}" "${SAURCOM_PASSWORD:?}"
TZ=${TZ:-Etc/UTC}
SAURCOM_GPU=${SAURCOM_GPU:-}
SAURCOM_SERIAL=${SAURCOM_SERIAL:-0}
export DEBIAN_FRONTEND=noninteractive
D=/opt/saurcom

echo "--- packages"
apt-get update -q
apt-get -y -q -o Dpkg::Options::=--force-confold dist-upgrade
apt-get install -y -q --no-install-recommends ca-certificates curl gnupg usbutils

echo "--- Docker (from Docker's own repository)"
if ! command -v docker >/dev/null 2>&1; then
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/debian/gpg | gpg --dearmor --yes -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg
    # shellcheck source=/dev/null
    . /etc/os-release
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/debian $VERSION_CODENAME stable" \
        > /etc/apt/sources.list.d/docker.list
    apt-get update -q
    apt-get install -y -q docker-ce docker-ce-cli containerd.io docker-compose-plugin
fi
systemctl enable --now docker
drv=$(docker info --format '{{.Driver}}' 2>/dev/null || true)
[ "$drv" = vfs ] && echo "!!! Docker uses the vfs storage driver here (no overlay support on this storage): every layer is a full copy. It works, but takes more disk."

echo "--- Eye of SaurCom"
install -d -m 0755 "$D" "$D/config"
chown 1000:1000 "$D/config"
install -d -m 0755 -o 0 -g 0 "$D/update"   # saurcom-update's status and log: root's, read-only in the app's container
umask 077
cat > "$D/.env" <<EOF
# the login of the web page, and what the service runs (saurcom-update changes SAURCOM_IMAGE)
SAURCOM_IMAGE=$SAURCOM_IMAGE
SAURCOM_USER=$SAURCOM_USER
SAURCOM_PASSWORD=$SAURCOM_PASSWORD
TZ=$TZ
SAURCOM_GPU=$SAURCOM_GPU
SAURCOM_SERIAL=$SAURCOM_SERIAL
EOF
umask 022

cat > "$D/compose.yaml" <<'EOF'
# Eye of SaurCom in a browser (packaging/docker in the project), in a Proxmox container. Made by saurcom-lxc.sh; the service saurcom
# starts it (saurcom-up writes compose.devices.yaml first: the graphics chip and the serial ports present now).
services:
  saurcom:
    image: ${SAURCOM_IMAGE}
    container_name: saurcom
    restart: unless-stopped
    environment:
      PUID: "1000"
      PGID: "1000"
      TZ: ${TZ}
      CUSTOM_USER: ${SAURCOM_USER}
      PASSWORD: ${SAURCOM_PASSWORD}
    ports:
      - "3001:3001"           # HTTPS (self-signed): browsers refuse the stream over plain HTTP
    shm_size: "1gb"           # the virtual display and the browser stream
    volumes:
      - ./config:/config      # settings, recordings and saurcom.log
      - ./update:/saurcom-update:ro   # the updater's status and log, read-only (saurcom-update)
      - /dev/bus/usb:/dev/bus/usb   # the radios, also those plugged in later
    device_cgroup_rules:      # only matters in a privileged container (in an unprivileged one the kernel's own rules apply)
      - "c 189:* rwm"         # USB devices
      - "c 188:* rwm"         # ttyUSB
      - "c 166:* rwm"         # ttyACM
      - "c 226:* rwm"         # the graphics chip
EOF

cat > /usr/local/sbin/saurcom-up <<'EOF'
#!/usr/bin/env bash
# Starts Eye of SaurCom with the devices this container has now (the service saurcom runs it).
set -euo pipefail
cd /opt/saurcom
set -a; . ./.env; set +a
{
    echo "# written by saurcom-up at each start: the devices present then"
    echo "services:"
    echo "  saurcom:"
    if [ -n "${SAURCOM_GPU:-}" ] && [ -c "$SAURCOM_GPU" ]; then
        echo "    environment:"
        echo "      DRINODE: $SAURCOM_GPU      # the app draws on the graphics chip"
        echo "      DRI_NODE: $SAURCOM_GPU     # Selkies encodes the video on it (VA-API)"
    fi
    vols=()
    [ -n "${SAURCOM_GPU:-}" ] && [ -c "$SAURCOM_GPU" ] && vols+=("$SAURCOM_GPU:$SAURCOM_GPU")
    if [ "${SAURCOM_SERIAL:-0}" = 1 ]; then
        for t in /dev/ttyUSB* /dev/ttyACM*; do [ -c "$t" ] && vols+=("$t:$t"); done
    fi
    if [ ${#vols[@]} -gt 0 ]; then
        echo "    volumes:"
        for v in "${vols[@]}"; do echo "      - $v"; done
    fi
} > compose.devices.yaml
exec docker compose -f compose.yaml -f compose.devices.yaml up -d --remove-orphans
EOF
chmod 755 /usr/local/sbin/saurcom-up

# saurcom-update (packaging/proxmox/saurcom-update): saurcom-lxc.sh puts it in place before this runs (pct push); else it comes from GitHub.
# It also installs the units behind the app's update button (saurcom-update.path) and the automatic updates (saurcom-update-auto.timer).
# (downloaded to a temporary file and installed only when complete)
if [ ! -x /usr/local/bin/saurcom-update ]; then
    su_tmp=$(mktemp)
    curl -fsSL -o "$su_tmp" "https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/${SAURCOM_ASSET_REF:-main}/packaging/proxmox/saurcom-update"
    if [ ! -s "$su_tmp" ] || ! head -n 1 "$su_tmp" | grep -qx '#!/usr/bin/env bash'; then
        rm -f "$su_tmp"; echo "!!! saurcom-update did not download completely" >&2; exit 1
    fi
    install -m 755 "$su_tmp" /usr/local/bin/saurcom-update
    rm -f "$su_tmp"
fi

cat > /etc/systemd/system/saurcom.service <<'EOF'
[Unit]
Description=Eye of SaurCom in the browser (https://<this container>:3001)
After=docker.service network-online.target
Requires=docker.service
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/saurcom-up
ExecStop=/usr/bin/docker compose -f /opt/saurcom/compose.yaml -f /opt/saurcom/compose.devices.yaml stop
WorkingDirectory=/opt/saurcom
TimeoutStartSec=1800

[Install]
WantedBy=multi-user.target
EOF

echo "--- pulling $SAURCOM_IMAGE (about 2 GB)"
set -a; # shellcheck source=/dev/null
. "$D/.env"; set +a
docker pull "$SAURCOM_IMAGE"
systemctl daemon-reload
systemctl enable --now saurcom
/usr/local/bin/saurcom-update --install-units

echo "--- radios on the USB bus now"
lsusb 2>/dev/null || true
echo "--- done"
