# Eye of SaurCom on Proxmox VE

`saurcom-lxc.sh` sets Eye of SaurCom up on a Proxmox VE node in one go. It creates a container (LXC) and passes the radios, the graphics chip and,
if you want, the USB serial ports through to it. It then runs the [Docker image](../docker/README.md) inside, so you use the whole desktop app
from a browser, with picture and sound.

## Install

On the Proxmox node, as root (the node's **Shell** in the web interface):

```sh
bash -c "$(curl -fsSL https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/main/packaging/proxmox/saurcom-lxc.sh)"
```

The script shows what it will do and asks before changing anything. When it is done, it prints the address (`https://<container>:3001`) and
the login. The password is random unless you give one. The certificate is self-signed, so the browser warns once. Browsers keep the sound off
until you click the page once.

With options, put `_` before them:

```sh
bash -c "$(curl -fsSL https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/main/packaging/proxmox/saurcom-lxc.sh)" _ \
  --ctid 210 --ip 192.168.1.50/24 --gw 192.168.1.1 --serial --password 'a-long-password'
```

To look first without changing anything, add `--dry-run`. From a clone of the repository: `bash packaging/proxmox/saurcom-lxc.sh --help`.

| Option | Default | |
|---|---|---|
| `--ctid N` | the next free id | |
| `--hostname NAME` | `saurcom` | |
| `--cores N` / `--memory MB` / `--swap MB` | 4 / 4096 / 1024 | Decoding wide signals (ATSC, DVB-T, several modes at once) wants more cores. |
| `--disk GB` | 16 | The image takes about 2 GB. Recordings and the databases take the rest. |
| `--storage NAME` | `local-lvm`, else the first storage that holds containers | |
| `--template-storage NAME` | `local`, else the first storage that holds templates | |
| `--bridge NAME` / `--vlan TAG` | `vmbr0` / none | |
| `--ip CIDR` + `--gw ADDR` | `dhcp` | |
| `--version X.Y.Z` | the latest release | |
| `--user NAME` / `--password PASS` | `saurcom` / random | The web page's login. At least 8 characters. No spaces, quotes, `$`, backtick or backslash. |
| `--gpu auto\|none\|/dev/dri/renderDN` | `auto` | `auto` takes the first Intel or AMD render node. |
| `--serial` | off | Also passes `/dev/ttyUSB*` and `/dev/ttyACM*` through: GPS receivers, CAT ports, rotators. |
| `--privileged` | off | Not needed for radios. It weakens the isolation. |
| `--no-host-setup` | off | Leaves the node's udev rules and kernel modules alone. |
| `--yes` | off | Doesn't ask before going ahead. |

## What is passed through, and how

The container is unprivileged, with Debian 12 and the `nesting` and `keyctl` features that Docker needs. It starts with the node.

| Device | In the container's config | On the node |
|---|---|---|
| **USB radios**: every radio Eye of SaurCom drives | `lxc.cgroup2.devices.allow: c 189:* rwm` and `/dev/bus/usb` bound as a directory | `/etc/udev/rules.d/60-saurcom-lxc.rules`: the project's radio list (`packaging/linux/60-saurcom-sdr.rules`) with mode 0666 |
| **Graphics chip** (Intel or AMD) | `dev0: /dev/dri/renderD128,mode=0666`. Before Proxmox 8.1: a cgroup line for major 226 and a bind mount. | Before 8.1: `61-saurcom-lxc-gpu.rules` sets the mode |
| **USB serial ports** (`--serial`) | Majors 188 (ttyUSB) and 166 (ttyACM); `ttyUSB0-7` and `ttyACM0-7` bound when present | A mode 0666 line in `60-saurcom-lxc.rules` |

Notes:

- **USB radios:** the whole bus is bound as a directory, so a radio plugged in later shows up without a restart. Press Refresh in the radio
  list.
- **Mode 0666:** an unprivileged container's root is an unknown user on the node and belongs to none of the node's groups. The radios'
  device files are therefore opened to everyone.
- **Kernel drivers:** the node also keeps its kernel TV driver off RTL-SDR dongles and its Mirics driver off SDRplay RSPs
  (`/etc/modprobe.d/saurcom-sdr-blacklist.conf`). If one of them was already loaded and in use, unplug the radio and plug it back in, or
  reboot the node.
- **Graphics chip:** the app draws on it and the browser stream is encoded on it (VA-API). Without one, the processor does both, which works
  but costs more CPU.
- **Serial ports:** a port is bound only if it existed when the container started. After plugging in a new one, run `pct reboot <ctid>`.

Inside the container, Docker runs the image from `/opt/saurcom`. The devices are bound into Docker as volumes, not with `--device`. Docker in
an unprivileged container cannot create device files, and a bound `/dev/bus/usb` also sees radios plugged in later.
`saurcom-up` writes `compose.devices.yaml` with the graphics chip and serial ports present at each start. The service `saurcom` runs it when
the container boots.

## Afterwards

```sh
pct exec <ctid> -- saurcom-update              # the newest release
pct exec <ctid> -- saurcom-update 0.3.1        # a given one
pct exec <ctid> -- cat /opt/saurcom/config/saurcom.log
pct exec <ctid> -- lsusb                       # the radios the container sees
pct exec <ctid> -- systemctl restart saurcom   # e.g. after changing /opt/saurcom/.env (login, graphics chip, serial ports)
```

Settings, recordings and the databases are in `/opt/saurcom/config` inside the container and are kept across updates. To change the login,
edit `SAURCOM_USER` and `SAURCOM_PASSWORD` in `/opt/saurcom/.env` and restart the service.

**Remove:** `pct stop <ctid> && pct destroy <ctid>`. On the node, delete `/etc/udev/rules.d/60-saurcom-lxc.rules`,
`61-saurcom-lxc-gpu.rules` (if present) and `/etc/modprobe.d/saurcom-sdr-blacklist.conf`, then run `udevadm control --reload-rules`.

## NVIDIA

`--gpu auto` leaves NVIDIA cards out. The container would need the same driver version as the node, plus the NVIDIA container toolkit inside
for Docker. If you set that up yourself, pass the card with `--gpu /dev/dri/renderDN`. Without it, Eye of SaurCom runs on the processor.

## Troubleshooting

- **"the container has no network":** check the bridge, VLAN and `--ip`. With DHCP, the container needs a lease from your router.
- **A radio is not listed:**
  - Run `pct exec <ctid> -- lsusb`. If the radio is missing there, it is not plugged into this node, or a VM has it through USB
    passthrough.
  - If it is listed but does not open, check `ls -l /dev/bus/usb/*/*` on the node: the radio should show `crw-rw-rw-`. If not, unplug it and
    plug it back in.
  - A radio Eye of SaurCom's udev rules don't know stays at mode 0664. Add a line for it to `60-saurcom-lxc.rules`.
- **RTL-SDR: "usb_claim_interface error -6":** the kernel's TV driver has the dongle. Run `lsmod | grep rtl` on the node and reboot it once
  after the install.
- **SDRplay RSPs** are passed through, but the image does not include SDRplay's API service (it may not be redistributed), so they don't
  open yet. Use the other radios, or SoapySDR's `mirisdr` module for a Mirics radio.
- **"vfs storage driver" during the install:** the storage under the container doesn't support overlay. ZFS before 2.2 is one example. Docker
  still works, but every image layer is a full copy and takes more disk. LVM-thin, directory storage and ZFS 2.2 or newer don't have this
  problem.
- **Anything about the page itself** (login, picture, sound): see the [Docker README](../docker/README.md). The container runs the same image.
