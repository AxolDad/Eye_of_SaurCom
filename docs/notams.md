# NOTAMs and TFRs

Tools > **NOTAMs and TFRs** lists the notices to air missions and the temporary flight restrictions around your station. TFR airspace is drawn
on the ADS-B map (the **TFRs** switch on the map's control line), and the airport window shows each airport's NOTAMs.

## Where they come from

Four sources, most accurate first:

| Source | What it gives | Needs |
|---|---|---|
| **FAA NOTAM Management Service (NMS)** (`api-nms.aim.faa.gov`) | The FAA's new NOTAM system: every NOTAM within the circle you choose (up to 100 NM), with text, times and airspace | Credentials from the FAA, by request to notams@faa.gov (issued for one environment: production, staging or fit), and the receiver position set |
| **FAA NOTAM API, older** (`external-api.faa.gov`) | The same, from the system NMS replaces, while it lasts | A key (client id and secret) from [api.faa.gov](https://api.faa.gov/s/), and the receiver position set |
| **tfr.faa.gov** | Every TFR in the country, its airspace drawn exactly, its times | Internet |
| **FIS-B** (UAT, 978 MHz) | The NOTAMs, TFRs and special-use airspace the ground stations near you broadcast | A UAT receiver tuned to 978 MHz. No internet |

FIS-B is always used when a UAT receiver is running. The internet sources are **off until you turn them on**. Your FAA keys are kept in the
program's settings and are sent only to the FAA, in request headers, never on a command line. NMS uses OAuth2 client credentials: the id and
secret are exchanged for a short-lived token, which is kept in memory only and renewed when it runs out.

## Failing back to the best information there is

Every NOTAM is shown once, from the best source that has it **now**:

- **Each part from the most accurate source.** The text from NMS, else the older API, else tfr.faa.gov, else FIS-B. The airspace from
  tfr.faa.gov (an exact outline), else NMS, else the older API, else FIS-B (circles and outlines, simplified for the radio link).
- **Live before stale.** A source is live for 30 minutes after its last good fetch (FIS-B: 15 minutes after the last frame). When a fetch
  fails, the last answer is still used, but only where no live source has the same thing. The last good answers of tfr.faa.gov are kept
  in the data folder, so a start without internet still shows them.
- **Stale says so.** A NOTAM no live source has is marked *stale*, with its age. One whose airspace or times come from a stale source is
  flagged too. Nothing out of date is shown as current.
- **Gone is gone.** tfr.faa.gov lists every TFR, so a TFR it no longer lists is gone, unless another source heard it *after* that list was
  fetched (a new TFR). NMS and the older API each speak for everything within their circle in the same way. FIS-B never makes anything disappear: a ground
  station sends what is near it, so what it skips says nothing.
- A NOTAM whose end time has passed, or whose newest source says it was cancelled, is not shown.

In the list, *From* is where the text came from, and *Age* is how long ago the freshest source had it. Select a row to see the whole
text, every source that has it, and where its airspace and times came from.

## On the map

Red: in force now. Amber: coming. Grey: stale. Circles are shaded, and outlines are drawn as lines.

## What has been checked

- The FIS-B decoder against the DO-358B test groups of [fisb-decode](https://github.com/rand-projects/fisb-decode): METARs, a NOTAM-D, and a
  TFR sent in three segments, put back together and paired with its airspace from another frame.
- The tfr.faa.gov readers against the list and all 79 TFR detail files the site served on 25 October 2026: every one read, with its number,
  times (UTC, or local zones such as EDT and Guam) and limits. 15 of them have no airspace at all, such as the nationwide UAS notices, and
  are listed without a distance.
- The older FAA NOTAM API reader against the API's documented geoJson layout only. No key was available to record a real answer.
- NMS against how an open-source client of it ([faa-nms-api](https://www.npmjs.com/package/faa-nms-api)) calls it: the token exchange, the
  `/nmsapi/v1/notams` query by latitude, longitude and radius with the `nmsResponseFormat: GEOJSON` header, and answers of the form
  `{"data": {"geojson": [...]}}`, each NOTAM read as the older API's. The FAA hands out the full documentation with the credentials, so
  this is untested against the service itself. If the first fetch fails to read the answer, the window shows the reason.

Not for navigation. Brief with an official source before you fly.
