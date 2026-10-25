# ADS-B networks: feeding them, and asking them

Two different things, both in **View > Data feeds**, and worth keeping apart in your head:

- **Send to aggregators** — your antenna heard it, you pass it on. This is what feeding airplanes.live, adsb.lol, ADS-B Exchange or
  Flightradar24 means.
- **Bring aircraft in from a service** — you ask a network what is in the air, including where your antenna cannot reach, and show it.

Everything on this page is **off until you turn it on**.

## Sending what you heard

Each target is a host, a port and optionally an id line, with presets as a starting point. Presets are only a starting point: where a network
hands each station its own address, the host is left blank for you to fill in.

| Network | Preset | Notes |
|---|---|---|
| **airplanes.live** | `feed.airplanes.live:30004`, Beast, **with a feeder id** | No account needed. Their own feeder documents this address. For MLAT see below |
| **adsb.lol** | `feed.adsb.lol:30004`, Beast, with a feeder id | No account needed. Their documentation also names `in.adsb.lol`; try that if the first will not connect |
| **adsb.fi** | `feed.adsb.fi:30004`, Beast, with a feeder id | No account needed |
| **TheAirTraffic** | `feed.theairtraffic.com:30004`, Beast, with a feeder id | No account needed |
| **ADS-B Exchange** | `feed.adsbexchange.com:30004`, Beast | Beast only; their MLAT uses a separate client |
| **ADSBHub** | `data.adsbhub.org:5001`, SBS-1 | Register a station first; they identify you by the address you feed from |
| **Flightradar24** | `127.0.0.1:30334`, Beast | **Through the `fr24feed` program you run yourself**, set to `beast-tcp`. Their upload protocol is private and is not reimplemented here |
| **FlightAware** | `127.0.0.1:30005`, Beast | Likewise, through `piaware` or their client |
| **Plane Finder** | `127.0.0.1:30005`, Beast | Likewise, through their client |

Feeding several networks at once is normal and none of them mind.

### The feeder id

The readsb-based networks — airplanes.live, adsb.lol, adsb.fi, TheAirTraffic — tell one feeder from another by a **UUID** that the feeder
sends the moment it connects. Their own feeder software does this (airplanes.live's `configure.sh` runs readsb with
`beast_reduce_plus_out` and `--uuid-file`); without it a feed still arrives, but nothing ties it to your station, so it will not show on their
per-feeder statistics.

When you add one of those presets, a fresh id is made for it. It is sent as readsb's own receiver-id frame — `0x1A 0xE4` and the 36 characters
— ahead of any data, which is how readsb sends it and what readsb's Beast reader expects. That was checked against readsb's source
(`net_io.c`, the `0xE4` case of the reader and `read_uuid`): the test transcribes readsb's acceptance rule and confirms it reads every id this
program sends back as itself.

- **Each target has its own id**, so two networks cannot match you up by it.
- **New id** makes you a new feeder to that network, and your statistics there start again.
- **Already fed with their own client?** Paste the id from that feeder (on a Pi running their image: `/usr/local/share/airplanes/airplanes-uuid`)
  and you carry on as the same feeder.
- Only a canonical UUID (`8-4-4-4-12` hex digits) is sent. Anything else sends no id at all, rather than one the network would read short.
- An SBS target never gets the frame: SBS is text, and a binary frame in it would be garbage to whoever reads it.

Two things their feeder does that this one does not. It sends **reduced** Beast (`beast_reduce`), which is fewer messages for less bandwidth;
this sends every message, which their ingest accepts. And it can switch on their **ping** protocol, which measures network delay; that is
opt-in on their side (readsb only pings a feeder that asks), so not asking means nothing comes back down the connection to handle.

### MLAT

This program does not do multilateration itself, but it does not need to: airplanes.live's own [`mlat-client`](https://github.com/airplanes-live/mlat-client)
reads Beast from a port, and **this program's Beast server is the kind it reads**. `mlat-client` asks for "Beast-format output with a 12 MHz
clock", and this program's Beast time stamp is the sample count of the radio scaled to 12 MHz — the same free-running sample clock readsb and
dump1090 give it, not the wall clock.

**This has not been tried against their server** — nothing here could reach port 31090 — so treat it as expected to work rather than known to.

1. Turn on **Beast binary (TCP server)** in View > Data feeds (port 30005 by default) and keep ADS-B running.
2. Put the **same feeder id** as your airplanes.live target in a file, say `~/airplanes-uuid`. The MLAT server pairs your MLAT with your feed by it.
3. Run their client the way their own feeder does (`airplanes-live/feed`, `scripts/airplanes-mlat.sh`):

   ```
   mlat-client --input-type dump1090 --no-udp --input-connect 127.0.0.1:30005 \
       --server feed.airplanes.live:31090 --user YOUR-NAME \
       --lat 51.50000 --lon -0.12000 --alt 30m \
       --uuid-file ~/airplanes-uuid \
       --results beast,listen,30105
   ```

   `--user` is the name shown on their MLAT map, and the position must be your **antenna's**, to five decimal places: MLAT is a timing
   measurement and a wrong position makes it wrong. Add `--privacy` to keep your station off their public map.
4. To see the MLAT positions in this program too, add `127.0.0.1:30105` as a feed from another receiver on the ADS-B screen. Note `listen`
   rather than their feeder's `connect`: their feeder connects out to a listening readsb, but this program's feed-in connects out itself, so
   `mlat-client` has to be the one listening.

Those MLAT positions arrive like any other receiver's feed: shown, marked with where they came from, and **not** recorded in the flight history.
That is the conservative choice — the aircraft genuinely were in earshot, but the position was worked out by the network, and the flight history
only keeps what this receiver measured.

A retune, a reset or dropped USB samples restart or jump the sample clock. `mlat-client` notices and resynchronises, but a receiver that does
that often will rarely stay in sync.

## Asking a service what is in the air

Pick a service, a radius around your receiver and how often to ask. You need a receiver position set on the ADS-B screen first, because what
gets asked for is a circle around you.

| Service | Works | Notes |
|---|---|---|
| **adsb.lol** | yes, no key | Community network run by volunteers. Be gentle with it |
| **adsb.fi** | yes, no key | Community network |
| **OpenSky Network** | yes, no key | Small daily allowance anonymously; a free account raises it, entered as `user:password` |
| **airplanes.live** | **needs their permission** | Their API refuses callers it does not know, and answers with "contact us at contact@airplanes.live … include a description of the project". Feeding **to** them needs no permission at all |
| **Flightradar24** | **no** | Their API is a paid subscription and wants the token in a request header, which this program cannot send yet |
| **ADS-B Exchange** | **no** | Paid, through RapidAPI, and also header-only |

Why airplanes.live refuses: their [`api-archive`](https://github.com/airplanes-live/api-archive) is the API as it was when they were **ADSB One**
— a thin layer over readsb's `re-api`, rate-limited to one request a second, with no access check at all. The check came later, in front of
the live service, and the old `api.adsb.one` address is now behind it too.

That archive also shows the full set of questions these readsb-based services answer, beyond the circle around you that this program asks:
`/v2/hex/…`, `/v2/callsign/…`, `/v2/reg/…`, `/v2/type/…`, `/v2/sqk/…` (a squawk: `7700` for emergencies), and `/v2/mil` (aircraft flagged
military). adsb.lol and adsb.fi both answer those today. This program does not ask them yet.

## No LADD

This program does not take part in LADD, the FAA's Limiting Aircraft Data Displayed program. LADD governs how the FAA hands out its own
data; it has nothing to do with what a receiver hears over the air. Nothing here is filtered by it: every aircraft received is shown, kept and
fed on in exactly the same way, whatever list it is on. An aircraft broadcasting on a PIA address is shown on that address, as broadcast.

airplanes.live works the same way. In their readsb, LADD appears only as a selection filter in the API (`filter_ladd`, "give me the aircraft
that are flagged"), never on any path that hides an aircraft from the feed, the data or the map.

Treating every aircraft alike cuts both ways, so nothing singles them out either: there is no LADD view or LADD list, and the `/v2/ladd` and
`/v2/pia` questions are not asked. Aircraft from a service come through whatever flags the service attaches to them — the reader does not even
look — and a test holds that in place.

The radius is capped per service and the interval can never go below the service's own rate limit, whatever you set — these are free networks
run by people, and hammering them is both rude and the fastest way to get blocked.

## What happens to aircraft that arrive this way

They are **shown, not kept, and not passed on**. Three separate rules, each enforced in a different place:

1. They go into the aircraft table as **a source of their own**, so the table's *source* column and the aircraft detail say which service an
   aircraft came from rather than implying your antenna heard it.
2. They are **never re-sent to an aggregator**. `AdsbTracker::applyExternal` has no path to the outbound feeds at all, so a position you did
   not receive cannot be passed on. Re-feeding other people's positions pollutes a network and breaks its terms.
3. They are **never recorded in the flight history** (`docs/flight-history.md`). The recorder skips every aircraft whose source is not this
   receiver. The flight history answers "what flew over this antenna", "how far does my receiver reach", "how long was it in earshot" — and a
   service covering a whole region would not add coverage to those answers, it would quietly make them wrong, with no way to tell afterwards
   which rows were real.

Rule 3 also fixed a real bug: aircraft fed in over Beast or SBS **from another receiver** were being recorded as though this one heard them.

One small thing to know: an aircraft is labelled with its service from its own source slot, and those slots are handed out after the TCP
feed-in sources. Adding or removing a TCP source can therefore mislabel which service an aircraft came from for a second or two, until its
next update re-stamps it.

## Keys

No key, account or token is built into this program; a service that needs one gets it from you, and it is kept in your preferences. A key goes
into a web address, and that address is handed to `curl`, so only the characters a URL token may hold are accepted: letters, digits and
`- _ . ~ :`. Anything else is refused rather than quoted and hoped for.
