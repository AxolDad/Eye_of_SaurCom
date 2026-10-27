#!/usr/bin/env bash
# Eye of SaurCom in a Proxmox VE container (LXC), used from a browser. Run on the Proxmox node as root:
#
#   bash -c "$(curl -fsSL https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/main/packaging/proxmox/saurcom-lxc.sh)"
#   bash -c "$(curl -fsSL .../saurcom-lxc.sh)" _ --gpu auto --serial --ctid 210      (with options: see --help)
#
# It prepares the node (the radios' udev rules, the kernel's DVB and Mirics drivers kept off the radios), makes an unprivileged Debian 12
# container with the USB bus, the graphics chip and the USB serial ports passed through, installs Docker in it and runs the Eye of SaurCom
# image (the whole desktop app in the browser, picture and sound: packaging/docker). See README.md next to this file.
set -Eeuo pipefail

REPO=AxolDad/Eye_of_SaurCom
ASSET_REF=${SAURCOM_ASSET_REF:-main}   # where the setup script and the udev rules come from when this file runs on its own

# ---------------------------------------------------------------- settings (all of them have an option)
CTID=""
CT_HOSTNAME=saurcom
CORES=4
MEMORY=4096
SWAP=1024
DISK=16
STORAGE=""
TEMPLATE_STORAGE=""
BRIDGE=vmbr0
IP=dhcp
GW=""
VLAN=""
VERSION=""                 # the release (0.3.0); empty: the latest release
WEB_USER=saurcom
WEB_PASSWORD=""            # empty: a random one, printed at the end
GPU=auto                   # auto, none, or a render node (/dev/dri/renderD129)
SERIAL=0                   # pass the USB serial ports (GPS, CAT, rotator) through
PRIVILEGED=0
HOST_SETUP=1
ASSUME_YES=0
DRY_RUN=0

usage() {
    cat <<EOF
Eye of SaurCom in a Proxmox VE container (LXC). Run on the Proxmox node as root.

  --ctid N              container id (default: the next free one)
  --hostname NAME       (default: $CT_HOSTNAME)
  --cores N             (default: $CORES)
  --memory MB           (default: $MEMORY)
  --swap MB             (default: $SWAP)
  --disk GB             root disk; the image takes about 2 GB, recordings take more (default: $DISK)
  --storage NAME        storage for the root disk (default: local-lvm, else the first that holds containers)
  --template-storage N  storage for the Debian template (default: local, else the first that holds templates)
  --bridge NAME         (default: $BRIDGE)
  --ip CIDR|dhcp        (default: dhcp); with a CIDR also --gw
  --gw ADDRESS
  --vlan TAG
  --version X.Y.Z       Eye of SaurCom release to run (default: the latest)
  --user NAME           login of the web page (default: $WEB_USER)
  --password PASS       password of the web page (default: a random one, printed at the end)
  --gpu auto|none|PATH  graphics chip for drawing and video encoding (default: auto: the first Intel or AMD render node)
  --serial              also pass the USB serial ports through (/dev/ttyUSB*, /dev/ttyACM*: GPS, CAT, rotators)
  --privileged          a privileged container (not needed for radios; less isolation)
  --no-host-setup       leave the node's udev rules and kernel modules as they are
  --yes, -y             do not ask
  --dry-run             show what would be done, change nothing
  --help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --ctid) CTID=$2; shift 2 ;;
        --hostname) CT_HOSTNAME=$2; shift 2 ;;
        --cores) CORES=$2; shift 2 ;;
        --memory) MEMORY=$2; shift 2 ;;
        --swap) SWAP=$2; shift 2 ;;
        --disk) DISK=$2; shift 2 ;;
        --storage) STORAGE=$2; shift 2 ;;
        --template-storage) TEMPLATE_STORAGE=$2; shift 2 ;;
        --bridge) BRIDGE=$2; shift 2 ;;
        --ip) IP=$2; shift 2 ;;
        --gw) GW=$2; shift 2 ;;
        --vlan) VLAN=$2; shift 2 ;;
        --version) VERSION=${2#v}; shift 2 ;;
        --user) WEB_USER=$2; shift 2 ;;
        --password) WEB_PASSWORD=$2; shift 2 ;;
        --gpu) GPU=$2; shift 2 ;;
        --serial) SERIAL=1; shift ;;
        --privileged) PRIVILEGED=1; shift ;;
        --no-host-setup) HOST_SETUP=0; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------- helpers
if [ -t 1 ]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B=""; G=""; Y=""; R=""; N=""; fi
msg()  { echo "${G}==>${N} ${B}$*${N}"; }
note() { echo "${Y} ! ${N} $*"; }
die()  { echo "${R}error:${N} $*" >&2; exit 1; }
trap 'die "stopped at line $LINENO: $BASH_COMMAND"' ERR

# a command that changes something: shown, and in a dry run not done
run() {
    if [ "$DRY_RUN" = 1 ]; then echo "    [dry-run] $*"; else "$@"; fi
}
# writes stdin to a file (shown instead in a dry run)
put() {
    local path=$1 mode=${2:-644}
    if [ "$DRY_RUN" = 1 ]; then echo "    [dry-run] write $path:"; sed 's/^/        /'; return; fi
    install -d "$(dirname "$path")"
    cat > "$path"
    chmod "$mode" "$path"
}
# the file next to this script, else the same file from GitHub
asset() {
    local rel=$1 here=""
    if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); fi
    if [ -n "$here" ] && [ -f "$here/$rel" ]; then cat "$here/$rel"; return; fi
    curl -fsSL "https://raw.githubusercontent.com/$REPO/$ASSET_REF/packaging/proxmox/$rel"
}

# ---------------------------------------------------------------- checks
[ "$(id -u)" = 0 ] || die "run this as root on the Proxmox node"
for c in pct pveam pvesm pvesh pveversion curl; do command -v "$c" >/dev/null 2>&1 || die "$c not found: run this on a Proxmox VE node"; done
PVE_VER=$(pveversion | sed -nE 's#^pve-manager/([0-9]+\.[0-9]+).*#\1#p')
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }
ver_ge "${PVE_VER:-0}" 7.0 || die "Proxmox VE 7 or newer is needed (this is ${PVE_VER:-unknown})"
DEVN=0; ver_ge "$PVE_VER" 8.1 && DEVN=1   # devN: device passthrough with its own mode (8.1 and newer)

[ -n "$CTID" ] || CTID=$(pvesh get /cluster/nextid)
if pct status "$CTID" >/dev/null 2>&1; then die "container $CTID exists already (choose another with --ctid)"; fi
case "$IP" in dhcp) ;; */*) [ -n "$GW" ] || die "--ip $IP needs --gw" ;; *) die "--ip takes dhcp or an address with its prefix (192.168.1.50/24)" ;; esac

# storages: the one named, else local-lvm / local, else the first that takes containers / templates
pick_storage() {
    local content=$1 prefer=$2
    if pvesm status --content "$content" 2>/dev/null | awk 'NR>1 {print $1}' | grep -qx "$prefer"; then echo "$prefer"; return; fi
    pvesm status --content "$content" 2>/dev/null | awk 'NR>1 && $3=="active" {print $1; exit}'
}
[ -n "$STORAGE" ] || STORAGE=$(pick_storage rootdir local-lvm)
[ -n "$TEMPLATE_STORAGE" ] || TEMPLATE_STORAGE=$(pick_storage vztmpl local)
[ -n "$STORAGE" ] || die "no storage for containers found (give one with --storage)"
[ -n "$TEMPLATE_STORAGE" ] || die "no storage for templates found (give one with --template-storage)"

# the release
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | sed -nE 's/.*"tag_name": *"v?([^"]+)".*/\1/p' | head -1) || true
    [ -n "$VERSION" ] || die "could not find the latest release on GitHub (give one with --version)"
fi
IMAGE="ghcr.io/axoldad/saurcom:$VERSION"

# the graphics chip: the first Intel or AMD render node (NVIDIA needs its driver in the container as well: see README.md)
GPU_NODE=""
case "$GPU" in
    none) ;;
    auto)
        for n in /dev/dri/renderD*; do
            [ -c "$n" ] || continue
            v=$(cat "/sys/class/drm/$(basename "$n")/device/vendor" 2>/dev/null || true)
            if [ "$v" = 0x8086 ] || [ "$v" = 0x1002 ]; then GPU_NODE=$n; break; fi
        done ;;
    *) [ -c "$GPU" ] || die "$GPU is not a device"; GPU_NODE=$GPU ;;
esac

[ -z "$WEB_PASSWORD" ] && WEB_PASSWORD=$(tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 20 || true)
[ "${#WEB_PASSWORD}" -ge 8 ] || die "the password needs 8 characters or more"
# it goes into a .env file that both the shell and Docker Compose read: no spaces, quotes, $, ` or backslashes
bad_chars='[[:space:]'\''"`$\\]'
if [[ "$WEB_PASSWORD$WEB_USER" =~ $bad_chars ]]; then die "the user and password may not hold spaces, quotes, \$, \` or backslashes"; fi
TZ_HOST=$(timedatectl show -p Timezone --value 2>/dev/null || cat /etc/timezone 2>/dev/null || echo Etc/UTC)

# ---------------------------------------------------------------- summary
cat <<EOF

${B}Eye of SaurCom in a Proxmox container${N}
  container      $CTID ($CT_HOSTNAME), $([ "$PRIVILEGED" = 1 ] && echo privileged || echo unprivileged), Debian 12
  resources      $CORES cores, $MEMORY MB memory, $SWAP MB swap, $DISK GB on $STORAGE
  network        $BRIDGE, $IP${GW:+ via $GW}${VLAN:+, VLAN $VLAN}
  Eye of SaurCom $VERSION ($IMAGE), web login $WEB_USER
  USB radios     the whole USB bus (radios plugged in later appear without a restart)
  graphics chip  ${GPU_NODE:-none (the processor draws and encodes)}
  serial ports   $([ "$SERIAL" = 1 ] && echo "/dev/ttyUSB0-7, /dev/ttyACM0-7" || echo "no (--serial)")
  node setup     $([ "$HOST_SETUP" = 1 ] && echo "udev rules for the radios, kernel DVB/Mirics drivers kept off them" || echo "skipped")

EOF
if [ "$ASSUME_YES" != 1 ] && [ "$DRY_RUN" != 1 ]; then
    [ -t 0 ] || die "no terminal to ask: add --yes"
    read -r -p "Go ahead? [y/N] " a
    case "$a" in y|Y|yes|YES) ;; *) echo "Nothing was changed."; exit 0 ;; esac
fi

# ---------------------------------------------------------------- the node
if [ "$HOST_SETUP" = 1 ]; then
    msg "Preparing the node: the radios' udev rules and kernel modules"
    # The project's rules (packaging/linux/60-saurcom-sdr.rules) give the radios to the group plugdev. A container's users are not in the
    # node's groups (an unprivileged container's root is an unknown user on the node), so here the radios are opened to everyone: mode 0666.
    rules=$(curl -fsSL "https://raw.githubusercontent.com/$REPO/v$VERSION/packaging/linux/60-saurcom-sdr.rules" 2>/dev/null \
            || curl -fsSL "https://raw.githubusercontent.com/$REPO/main/packaging/linux/60-saurcom-sdr.rules") \
        || die "could not fetch the radios' udev rules from GitHub"
    {
        echo "# Eye of SaurCom radios for Proxmox containers (written by saurcom-lxc.sh): every radio the project knows, readable and writable by all,"
        echo "# so that the containers' users can open them through the USB bus passed in. From packaging/linux/60-saurcom-sdr.rules."
        echo
        # (the project file's own header, up to its first empty line, speaks of desktop users: left out)
        sed -E '1,/^$/d; s/MODE="0660"/MODE="0666"/; s/, *GROUP="plugdev"//; s/, *TAG\+="uaccess"//' <<<"$rules"
        if [ "$SERIAL" = 1 ]; then
            echo
            echo "# USB serial ports (GPS receivers, radios' CAT ports, rotators) for the containers: --serial"
            echo 'SUBSYSTEM=="tty", KERNEL=="ttyUSB[0-9]*|ttyACM[0-9]*", MODE="0666"'
        fi
    } | put /etc/udev/rules.d/60-saurcom-lxc.rules
    put /etc/modprobe.d/saurcom-sdr-blacklist.conf <<'EOF'
# Eye of SaurCom (saurcom-lxc.sh): the kernel's TV driver must not take RTL-SDR dongles, nor its Mirics driver SDRplay RSPs,
# or the radio cannot be opened in the container.
blacklist dvb_usb_rtl28xxu
blacklist rtl2832_sdr
blacklist rtl2832
blacklist rtl2830
blacklist msi2500
blacklist msi001
blacklist sdr_msi3101
EOF
    for m in dvb_usb_rtl28xxu rtl2832_sdr rtl2832 rtl2830 msi2500 msi001 sdr_msi3101; do
        if grep -q "^$m " /proc/modules 2>/dev/null; then run modprobe -r "$m" || note "the kernel module $m is in use: replug the radio after a reboot"; fi
    done
    run udevadm control --reload-rules
    run udevadm trigger --subsystem-match=usb
    [ "$SERIAL" = 1 ] && run udevadm trigger --subsystem-match=tty
    # the render node: devN sets its mode for the container; on Proxmox older than 8.1 a udev rule does it on the node
    if [ -n "$GPU_NODE" ] && [ "$DEVN" = 0 ]; then
        echo "KERNEL==\"$(basename "$GPU_NODE")\", SUBSYSTEM==\"drm\", MODE=\"0666\"" | put /etc/udev/rules.d/61-saurcom-lxc-gpu.rules
        run udevadm trigger --subsystem-match=drm
    fi
fi

# ---------------------------------------------------------------- the template
msg "Debian 12 template"
run pveam update >/dev/null || note "pveam update failed: using the template list as it is"
ARCH=$(dpkg --print-architecture 2>/dev/null || echo amd64)
TEMPLATE=$(pveam available --section system 2>/dev/null | awk '{print $2}' | grep -E "^debian-12-standard_.*_${ARCH}\.tar\.(zst|gz|xz)\$" | sort -V | tail -1)
[ -n "$TEMPLATE" ] || die "no Debian 12 template in the list (pveam available --section system)"
if ! pveam list "$TEMPLATE_STORAGE" 2>/dev/null | grep -q "$TEMPLATE"; then run pveam download "$TEMPLATE_STORAGE" "$TEMPLATE"; fi

# ---------------------------------------------------------------- the container
msg "Creating container $CTID"
net="name=eth0,bridge=$BRIDGE,ip=$IP${GW:+,gw=$GW}${VLAN:+,tag=$VLAN}"
run pct create "$CTID" "$TEMPLATE_STORAGE:vztmpl/$TEMPLATE" \
    --hostname "$CT_HOSTNAME" --cores "$CORES" --memory "$MEMORY" --swap "$SWAP" \
    --rootfs "$STORAGE:$DISK" --net0 "$net" --ostype debian --timezone host \
    --unprivileged "$([ "$PRIVILEGED" = 1 ] && echo 0 || echo 1)" --features nesting=1,keyctl=1 \
    --onboot 1 --tags saurcom \
    --description "Eye of SaurCom $VERSION: https://<this container>:3001 (web login $WEB_USER). Made by saurcom-lxc.sh; update with: pct exec $CTID -- saurcom-update"

# The passthrough. The USB bus as a directory: radios plugged in later appear in the container at once (major 189: USB devices).
# Serial ports: each name is bound when the container starts, if it exists then (majors 188: ttyUSB, 166: ttyACM).
{
    echo "# Eye of SaurCom (saurcom-lxc.sh): the USB radios"
    echo "lxc.cgroup2.devices.allow: c 189:* rwm"
    echo "lxc.mount.entry: /dev/bus/usb dev/bus/usb none bind,optional,create=dir"
    if [ "$SERIAL" = 1 ]; then
        echo "# USB serial ports (GPS, CAT, rotators): those present when the container starts"
        echo "lxc.cgroup2.devices.allow: c 188:* rwm"
        echo "lxc.cgroup2.devices.allow: c 166:* rwm"
        for i in 0 1 2 3 4 5 6 7; do
            echo "lxc.mount.entry: /dev/ttyUSB$i dev/ttyUSB$i none bind,optional,create=file"
            echo "lxc.mount.entry: /dev/ttyACM$i dev/ttyACM$i none bind,optional,create=file"
        done
    fi
    if [ -n "$GPU_NODE" ] && [ "$DEVN" = 0 ]; then
        echo "# the graphics chip (Proxmox before 8.1: no devN)"
        echo "lxc.cgroup2.devices.allow: c 226:* rwm"
        echo "lxc.mount.entry: $GPU_NODE dev/dri/$(basename "$GPU_NODE") none bind,optional,create=file"
    fi
} > "/tmp/saurcom-lxc-$CTID.conf"
if [ "$DRY_RUN" = 1 ]; then
    echo "    [dry-run] append to /etc/pve/lxc/$CTID.conf:"; sed 's/^/        /' "/tmp/saurcom-lxc-$CTID.conf"
else
    cat "/tmp/saurcom-lxc-$CTID.conf" >> "/etc/pve/lxc/$CTID.conf"
fi
rm -f "/tmp/saurcom-lxc-$CTID.conf"
# the graphics chip, open to the container's users whatever their groups
if [ -n "$GPU_NODE" ] && [ "$DEVN" = 1 ]; then run pct set "$CTID" --dev0 "$GPU_NODE,mode=0666"; fi

msg "Starting container $CTID"
run pct start "$CTID"
if [ "$DRY_RUN" != 1 ]; then
    for _ in $(seq 1 60); do
        if pct exec "$CTID" -- getent hosts deb.debian.org >/dev/null 2>&1; then break; fi
        sleep 2
    done
    pct exec "$CTID" -- getent hosts deb.debian.org >/dev/null 2>&1 || die "the container has no network (check $BRIDGE and --ip)"
fi

# ---------------------------------------------------------------- inside: Docker and Eye of SaurCom
msg "Installing Docker and Eye of SaurCom $VERSION in the container (the image is about 2 GB)"
setup=$(asset saurcom-lxc-setup.sh) || die "could not get saurcom-lxc-setup.sh"
if [ "$DRY_RUN" = 1 ]; then
    echo "    [dry-run] pct exec $CTID -- env SAURCOM_IMAGE=$IMAGE ... bash -s  < saurcom-lxc-setup.sh ($(wc -l <<<"$setup") lines)"
else
    pct exec "$CTID" -- env \
        SAURCOM_IMAGE="$IMAGE" SAURCOM_USER="$WEB_USER" SAURCOM_PASSWORD="$WEB_PASSWORD" TZ="$TZ_HOST" \
        SAURCOM_GPU="${GPU_NODE:+/dev/dri/$(basename "$GPU_NODE")}" SAURCOM_SERIAL="$SERIAL" \
        bash -s <<<"$setup"
fi

# ---------------------------------------------------------------- done
addr=""
if [ "$DRY_RUN" != 1 ]; then
    addr=$(pct exec "$CTID" -- hostname -I 2>/dev/null | awk '{print $1}' || true)
fi
cat <<EOF

${G}${B}Eye of SaurCom is running in container $CTID.${N}

  Open      ${B}https://${addr:-<container address>}:3001${N}   (self-signed certificate: the browser warns once)
  Login     ${B}$WEB_USER${N} / ${B}$WEB_PASSWORD${N}   (kept in /opt/saurcom/.env in the container)
  Sound     click into the page once; the sound button is in the sidebar on the left

  Update    pct exec $CTID -- saurcom-update            (the newest release)
            pct exec $CTID -- saurcom-update 0.3.1      (a given one)
  Log       pct exec $CTID -- cat /opt/saurcom/config/saurcom.log
  Radios    plug them into this node; they appear in the app at once (on the next "Refresh" of the radio list)
EOF
[ "$SERIAL" = 1 ] && echo "  Serial    a serial port plugged in later appears after: pct reboot $CTID"
[ -z "$GPU_NODE" ] && [ "$GPU" != none ] && echo "  Graphics  no Intel or AMD chip found: the processor draws the app and encodes the stream"
echo
