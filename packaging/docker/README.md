# Eye of SaurCom in Docker, in a browser

The image in `packaging/docker` runs the whole Eye of SaurCom desktop app on a server without a screen (a NAS, a home server)
and shows it in any browser: picture, sound, mouse and keyboard. It is the same program as the desktop download, nothing is cut
out. The browser part is [Selkies](https://github.com/selkies-project/selkies) from the
[linuxserver.io base image](https://docs.linuxserver.io/images/docker-baseimage-selkies/), which runs Eye of SaurCom on a virtual display and
streams it as H.264 video with Opus sound.

## Start it

```sh
cd packaging/docker
echo "SAURCOM_PASSWORD=<a password>" > .env      # the login of the page (user saurcom); compose will not start without it
docker compose up -d --build
```

Then open `https://<server>:3001` and log in. The certificate is self-signed, so the browser warns once. Click into the page once:
browsers keep the sound off until the page has been clicked. The sound button in the sidebar on the left turns it on and off.

Without compose:

```sh
docker build -t saurcom packaging/docker
docker run -d --name saurcom -p 3001:3001 -e CUSTOM_USER=saurcom -e PASSWORD=<a password> \
  --device /dev/bus/usb -v saurcom-config:/config saurcom
```

Every release is also published ready-made, for x86-64 and arm64: use `ghcr.io/axoldad/saurcom:latest` (or `:<version>`) in place of
`saurcom` above and skip the build.

The image installs the release `.deb` of the version named in the `Dockerfile` (`SAURCOM_VERSION`). For another release:
`docker build --build-arg SAURCOM_VERSION=0.2.7 -t saurcom packaging/docker`.

## Updates

The app cannot replace its own image, so in the container (`SAURCOM_CONTAINER=docker` in the `Dockerfile`) the version button in the top
bar does not download anything itself. When a newer release is out it shows **Update now**, and that writes a request file into the config
volume, `/config/.saurcom-update/request`. A helper outside the container, `saurcom-update`, reads it (it only reads in the app's
volume), pulls the new image, recreates the container and writes its progress to a folder of its own that the app's container sees
read-only, `/saurcom-update/status`. The app shows it ("Downloading the new version…", "Restarting…", then "Updated to 0.3.1" or the
error). The app needs no network port and no Docker socket for this.
**Install updates automatically (at 04:00)** in the same window writes `/config/.saurcom-update/auto`; the helper's daily timer reads it.

The app asks GitHub for the newest release a few seconds after every start (only github.com is contacted). To switch that off, untick
**Check for updates automatically** in the same window, or set `SAURCOM_NO_UPDATE_CHECK: "1"` under `environment:` in `compose.yaml`.

- **Proxmox** ([../proxmox](../proxmox/README.md)): the helper is installed there. Nothing to do.
- **Plain Docker**: there is no helper, so after 30 seconds the button shows how to update by hand. With the ready-made image
  (`image: ghcr.io/axoldad/saurcom:latest`), that is `docker compose pull && docker compose up -d` next to `compose.yaml`; with a build of
  your own, build the new version. To get the button working on a Linux host with systemd, set the host up like the Proxmox container:
  `/opt/saurcom/.env` with `SAURCOM_IMAGE=ghcr.io/axoldad/saurcom:<version>`, a `compose.yaml` there with `image: ${SAURCOM_IMAGE}`,
  `./config:/config` (owned by uid 1000) and `./update:/saurcom-update:ro`, and `/usr/local/sbin/saurcom-up` running `docker compose up -d` in that folder. Then copy
  `packaging/proxmox/saurcom-update` to `/usr/local/bin/` and run it once as root: it installs its units and updates.

An update installed by hand inside a running container is lost when the container is recreated: change the image instead.

## Radios

- **USB radios** (HackRF, RTL-SDR, Airspy, ...): pass `/dev/bus/usb` in, as above. At the start the container gives Eye of SaurCom's user
  access to the radios listed in Eye of SaurCom's udev rules; only the container's copies of the device files change, nothing on the host.
  Docker copies the device files when the container starts, so **restart the container after plugging a radio in**. The host
  still needs the kernel's DVB driver kept off RTL-SDR dongles (`packaging/linux/saurcom-rtlsdr-blacklist.conf`).
- **Network radios** (rtl_tcp, SoapyRemote, SpyServer, openHPSDR, ...) need nothing extra.
- **SDRplay** needs SDRplay's own API service, which may not be shipped; it is not in the image.

## Graphics card

Without one, the processor draws the GUI and encodes the stream. With the synthetic 20 Msps signal running and a 1920 x 1040 browser
window, the container used 3.8 cores on an i7-12700K, and 1.8 with its Intel UHD 770 doing that work. Pass the chip's render node in
and name it (the `DRINODE` lines of `compose.yaml`):

```sh
docker run ... --device /dev/dri/renderD128 -e DRINODE=/dev/dri/renderD128 -e DRI_NODE=/dev/dri/renderD128 saurcom
```

`DRINODE` lets Eye of SaurCom draw on the chip, `DRI_NODE` lets Selkies encode the video with it (VA-API). With several chips,
`/sys/class/drm/renderD*/device/vendor` tells them apart (0x8086 Intel, 0x1002 AMD). NVIDIA cards work through the NVIDIA Container
Toolkit (`--gpus all`) as the base image describes; that path is not tested with Eye of SaurCom.

## Settings

| Variable | Default | |
|---|---|---|
| `PUID`, `PGID` | 1000 | owner of the files in `/config` |
| `TZ` | UTC | time zone |
| `CUSTOM_USER`, `PASSWORD` | none (`compose.yaml` requires one) | the login of the page. The page is the whole desktop, so keep a login whenever other computers can reach the port |
| `LANG` | `en_US.UTF-8` | system language; any other locale is made at the start (Eye of SaurCom's own language is chosen in the app) |
| `MAX_RES` | `3840x2160` | the largest screen a browser can ask for. The virtual display reserves memory for it (4K: 33 MB; the base image's 15360x8640: 530 MB) |
| `DRINODE`, `DRI_NODE` | none | graphics chip, see above |

The base image has more (`SELKIES_*`, see its documentation). Everything Eye of SaurCom keeps is in the `/config` volume: settings in
`/config/.config/saurcom`, recordings and data in `/config/.local/share/saurcom`, and Eye of SaurCom's log in `/config/saurcom.log`, which is
what a bug report needs.

## Notes

- The image is the base image plus Eye of SaurCom, `libfaad2` (DAB+ HE-AAC), SoapySDR modules and fonts, minus what Eye of SaurCom does not use
  (the Docker Engine inside the base, the compiler tool chain, `locales-all`, Noto CJK). It is flattened into one layer so that the
  removed files are really gone: about 1.7 GB instead of 2.9 GB. The base image's settings are therefore repeated in the
  `Dockerfile`; check them when the base changes.
- **HTTPS is required.** Browsers offer WebCodecs (the video and sound decoders the page uses) only on HTTPS or `localhost`; over plain
  HTTP from another computer the page stops with "This application requires a secure connection (HTTPS)". Use 3001 (HTTPS, self-signed),
  or a reverse proxy that serves HTTPS with your own certificate and passes on to 3000 (plain HTTP, WebSockets on). Publish 3000 only
  to the proxy, for example `127.0.0.1:3000:3000`.
- Selkies loads a stand-in `libudev` into the desktop session for its virtual gamepads. With it, libusb finds no USB devices, so
  the start script (`root/defaults/autostart`) starts Eye of SaurCom without it.
- Tested on x86-64 (Docker 27, Intel UHD 770, HackRF One). The arm64 image is built from the same file and the arm64 `.deb`, but
  has not been run on a radio.
