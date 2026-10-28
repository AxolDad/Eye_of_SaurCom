# Station network

Run Eye of SaurCom on more than one computer, and see what all of them hear in one Eye. Each computer is a **station**. One of them (or any other) adds the rest as **sites**, and their decoded events show up in its Eye, marked with the station's name.

## What it does

- Every station you add is read over its remote control API (a WebSocket). The events it decodes (aircraft, ships, APRS, pagers and so on) arrive here and are treated like local ones: the Eye, the message database and the watchlists see them.
- In the Eye, a row of chips appears once an event from another station has been heard: **All**, **This station**, and one chip per station. A chip shows only that station's contacts and timeline lines. Rows from another station carry its name in brackets. A contact heard by several stations lists all of them in its tooltip.
- If the link drops, it is retried after 2, 4, 8 seconds and so on, up to 60 s. After a reconnect the station sends what it decoded in the meantime, and nothing is shown twice.

## What crosses the link

Decoded events: type, time, frequency, mode, and the decoded fields (call sign, position, text). Direction-finding bearings cross too.

Not audio, not IQ, and not your settings. The other station keeps its receivers to itself.

## Setting it up

1. **On each station you want to read** (the remote side): open **View > Data feeds > Remote control API**. Turn on **Let scripts and other programs control this receiver**, then **Let other computers connect (LAN, needs the token)**. The app makes a token of at least 16 characters. Press **Copy token**. Note the port (8765 unless you changed it) and the computer's address.
2. **Here:** open **Tools > Stations**, press **Add...**, and fill in the name (anything short; it is shown in the Eye), the address, the port and the token. Press **Save**.
3. The state column goes Connecting, then Live. If it says Retrying, read the Error column: a wrong token, a closed port or a wrong address are the usual causes.

A station on the same computer (a second copy with another data folder and port) needs no token, since local programs are trusted.

The sites, with their tokens, are kept in `stations/sites.json` in your data folder. The file is readable by you only (mode 0600 on Linux and macOS). The token is shown as dots in the form.

## Clock skew

The table shows the clock skew of each station: how far its clock is behind or ahead of this one, judged from the time of its events. Over 30 seconds the value turns red with the hint *this station's clock is off*.

This matters for locating. Bearings from stations whose clocks differ by more than a few seconds do not combine into a fix, because they are not bearings of the same moment. Keep every station on NTP (or another time sync) when you want fixes.

## Located: fixes from several stations

The **Located** chip in the Eye holds signals that were placed on the map from bearings of two or more stations. It needs direction-finding bearings from at least two stations (a KrakenSDR or another array on each, running the Direction finder), heard within 5 seconds of each other and within 25 kHz of each other. Each bearing must carry the station's position, so set your location on every station. One station alone gives a line, not a point, so it never makes a Located contact. The fix has an error figure (the 95 % ellipse); a poor geometry, for example two stations in line with the signal, gives a large one.

## Limits

- **No encryption yet.** The link is plain HTTP and WebSocket, and the token travels in clear. Use it on your own network or a VPN. Do not put the port on the internet.
- **No TDOA.** Fixes are crossed bearings only; arrival time differences between stations are not used.
- A bearing without the station's position (no location set there) is ignored.
- Text from another station is shown as plain text and never interpreted, but you still choose whom to trust: a station you add can put any contact on your Eye.
