# Eye of SaurCom on Proxmox VE

`saurcom-lxc.sh` sets Eye of SaurCom up on a Proxmox VE node in one go. It creates a container (LXC) and passes the radios, the graphics chip and,
if you want, the USB serial ports through to it. It then runs the [Docker image](../docker/README.md) inside, so you use the whole desktop app
from a browser, with picture and sound.

## Install

You type one command on the Proxmox **node** (the server itself), not inside a container.

1. In the Proxmox web interface, click the node's name in the tree on the left: the line above the numbered containers (for example
   `raptor`).
2. Click **>_ Shell** in the column next to it. A black terminal opens, logged in as root on the node.
3. Paste this and press Enter:

   ```sh
   bash -c "$(curl -fsSL https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/main/packaging/proxmox/saurcom-lxc.sh)"
   ```

4. It shows what it will do and asks `Go ahead? [y/N]`. Type `y`.
5. Wait a few minutes: it downloads about 2 GB. At the end it prints the address and the login. **Write the password down**: it is random
   unless you gave one with `--password`.

## Logging in

1. On any computer on your network, open the address the installer printed in a web browser: `https://<container address>:3001`. It
   must start with `https://` and end with `:3001`. The address is also in the container's **Notes**, on its **Summary** page in Proxmox.
2. The browser warns that the connection is not private, because the certificate is self-made. Choose **Advanced**, then **Continue**. It
   asks only once.
3. The browser asks for a user name and password. The user is `saurcom` (unless you gave `--user`); the password is the one the installer
   printed.
4. Click into the page once to turn the sound on.

**Lost the password?** In the node's **Shell**, with your container's number in place of `120`:

```sh
pct exec 120 -- grep SAURCOM_PASSWORD /opt/saurcom/.env
```

**Not the Proxmox Console.** The container's **Console** in Proxmox asks for the container's own Linux login (`saurcom login:`). That is a
different login and the web password does not work there. For a shell inside the container, run `pct enter 120` in the node's Shell instead
(`exit` leaves it).

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

## Updates

Update from inside the app, in the browser. When a newer release is out, the version button in the top bar turns yellow
(`update 0.3.1`). Click it, then **Update now**. The window shows the progress: "Downloading the new version…" while Eye of SaurCom keeps
running, then "Restarting…". The page drops for about half a minute while the app's container is recreated, and reconnects by itself.
Afterwards the window says "Updated to 0.3.1".

- **Automatic updates:** tick **Install updates automatically (at 04:00)** in the same window. Every night at 04:00 (the node's time zone,
  plus up to 30 minutes) a newer release is installed by itself. It is off until you tick it.
- **If something goes wrong:** the new image is downloaded before anything is stopped, so a failed download leaves the running version
  alone. If the new version does not start, the previous one is started again. Either way the window shows what happened, and the log is
  in `/opt/saurcom/config/.saurcom-update/log` (`/config/.saurcom-update/log` inside the app).
- **From a shell** it still works as before: `pct exec <ctid> -- saurcom-update`, or `saurcom-update 0.3.1` for a given release.

How it works: the button writes the wanted version into `/opt/saurcom/config/.saurcom-update/request`. The systemd unit
`saurcom-update.path` in the Proxmox container sees the file and starts `saurcom-update.service`, which runs `saurcom-update --request`. It
reads only a version from the file (digits and dots), never runs what is in it, writes each step to `status` next to it and removes the
request. `saurcom-update-auto.timer` runs `saurcom-update --auto`, which updates only when the app's `auto` file holds `1` and a newer
release exists. A lock keeps two updates from running at once. The app's container gets no network port and no Docker socket for this.

### Containers set up before the update button

A container made by an older `saurcom-lxc.sh` has an older `saurcom-update` that knows nothing of the button. There, **Update now** waits 30
seconds, then shows the command below. Run it once in the node's **Shell**, with your container's number in place of `120`:

```sh
pct exec 120 -- bash -c 'curl -fsSL https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/main/packaging/proxmox/saurcom-update -o /usr/local/bin/saurcom-update && chmod 755 /usr/local/bin/saurcom-update && saurcom-update'
```

It replaces `saurcom-update` with the new one and runs it. That updates Eye of SaurCom to the newest release and installs the units behind
the button. From then on, the button and the automatic updates work, and every later `saurcom-update` refreshes those units. The old command
alone (`pct exec 120 -- saurcom-update`) still updates, but it runs the old script, so it does not add the button's units.

## Afterwards

```sh
pct exec <ctid> -- saurcom-update              # the newest release (the app's Update now button does the same)
pct exec <ctid> -- saurcom-update 0.3.1        # a given one
pct exec <ctid> -- cat /opt/saurcom/config/.saurcom-update/log   # what the updates did
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
- **The page does not open:** use the `192.168.x.x` style address from the installer or the container's Notes, with `https://` and `:3001`.
  `172.17.0.1` is Docker's address inside the container and does not work from outside. To check the app, run these in the node's Shell
  (container 120 here):
  - `pct exec 120 -- ip -4 -o addr show eth0`: the address to use.
  - `pct exec 120 -- docker ps`: should list `saurcom` as `Up`. The first start after the install can take a few minutes.
  - `pct exec 120 -- curl -sk -o /dev/null -w '%{http_code}\n' https://localhost:3001`: `401` means the page is up and waiting for the
    login; `000` means it is not up yet (see `pct exec 120 -- journalctl -u saurcom -n 30`).
- **"Login incorrect" at `saurcom login:`:** that is the Proxmox Console, not the web page. See [Logging in](#logging-in).
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

## Radios added later

`/dev/bus/usb` is bound into the container as a directory, so a radio you plug into the node after the container has started shows up inside it without a restart. Eye of SaurCom then offers the new-radio notice (**Name it**, **Use in new receiver tab**, **Dismiss**). Radios plugged in before the program starts do not trigger it. Names and RTL-SDR serial backups are kept in `radios/` under `/config/.local/share/saurcom`, in the `/config` volume. See [docs/radios.md](../../docs/radios.md).
