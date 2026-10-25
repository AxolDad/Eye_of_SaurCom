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
| **airplanes.live** | `feed.airplanes.live:30004`, Beast | No account needed. Their own feeder documents this address; MLAT (port 31090) needs their separate client and is not implemented here |
| **adsb.lol** | `feed.adsb.lol:30004`, Beast | No account needed. Their documentation also names `in.adsb.lol`; try that if the first will not connect |
| **adsb.fi** | `feed.adsb.fi:30004`, Beast | No account needed |
| **TheAirTraffic** | `feed.theairtraffic.com:30004`, Beast | No account needed |
| **ADS-B Exchange** | `feed.adsbexchange.com:30004`, Beast | Beast only; their MLAT uses a separate client |
| **ADSBHub** | `data.adsbhub.org:5001`, SBS-1 | Register a station first; they identify you by the address you feed from |
| **Flightradar24** | `127.0.0.1:30334`, Beast | **Through the `fr24feed` program you run yourself**, set to `beast-tcp`. Their upload protocol is private and is not reimplemented here |
| **FlightAware** | `127.0.0.1:30005`, Beast | Likewise, through `piaware` or their client |
| **Plane Finder** | `127.0.0.1:30005`, Beast | Likewise, through their client |

Feeding several networks at once is normal and none of them mind.

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
