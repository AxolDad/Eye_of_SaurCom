# Flight history: what flies over, and what it does

View > Flight history keeps every flight the ADS-B receiver heard, so the question stops being "what is overhead now" and becomes "what is
usually overhead, where does it go, and is today different". It is a SQLite file of its own, `<data folder>/flights/flights.db`, separate from
the message store: the two hold nothing in common, and a user who wants years of flights while pruning messages weekly can have both.

Nothing here touches the network. The dossier you build is your own file.

## What gets recorded

Recording runs whichever screen you are looking at, as long as ADS-B is the active mode and "Record flights" is on (it is by default). Each
aircraft's positions are collected until the flight ends, then the finished flight and its track are written in one go.

A flight ends when the aircraft goes quiet for a few minutes, or when it has clearly been on the ground long enough to be a separate trip. An
aircraft that only transits the edge of coverage still makes a flight: the figures then describe the part that was heard, which is what the
range columns are for.

Only an aircraft whose message count has moved is looked at again, and each position is stamped with when the aircraft was actually heard (its
own age), not when the pass happened.

## What is worked out per flight

- **Phases** — ground, takeoff, climb, cruise, descent, approach, landing, from the altitude and vertical rate, against the field elevation
  where an airport is known. A level pass at cruise is never mistaken for a ground run: with no field elevation, the track's own floor only
  counts when the flight's vertical extent is large enough to mean something.
- **Where it began and ended** — the airport, or "near" an airport, or just overhead, from the runway database (see below) and the track's own
  ends. The runway is named where the direction at the end matches one.
- **The approach**, where there is one: which runway, the glide path flown against it, how far off the centreline and off the glide path on
  average and at worst, the height at the threshold, and how far out it lined up. This is a measurement of the track, not of the aircraft.
- **The kind of flying** — airline, cargo, private, medical, military, survey, skydive or training — from the call sign, the address block, the
  registration, the type, the pattern of the flight itself (a climb-and-drop cycle is a skydive run, circuits are training) and the airports at
  each end. It comes with how sure the program is and what it based that on; you can overrule it on the aircraft's own screen and your answer
  wins everywhere, including the filters.
- The altitudes, speeds, distance along the track, range from the receiver, squawk, and whether that squawk was an emergency.

## The four screens

**Overview** answers what is in your airspace the most, with how many flights, how long it was airborne and the altitude it usually cruises at;
what kind of flying it was; the busiest airports and operators; and what stood out in the period.

**Flights** is the log, filtered by period, kind, airport, call sign or emergencies only. Picking one shows its figures, the share of time in
each phase, the approach measurement, and buttons to copy the track as CSV or save it as GPX.

**Aircraft** is the dossier, and the one screen you write on rather than read:

- what the program knows: registration, type, operator, how often it was heard, hours airborne over the receiver, usual cruise altitude and
  speed, the airport it begins or ends at most often, average time on the ground between trips, how many different paths it has flown and how
  much of its flying is on the commonest one, and the hour-of-day and day-of-week it is heard
- what you add: free notes, tags, **facts** as key and value pairs (`built`, `owner`, anything you looked up elsewhere), pictures attached from
  your own disk, a timeline you can add sightings to, and the kind-of-flying override
- the "Photo / info" button opens a public aircraft database in your browser, so the research you do elsewhere comes back here as facts

Everything the program inferred is kept in different columns from everything you entered, so a later recording never overwrites your work.

**Patterns** is the aggregate view: flights by hour and by weekday, how each kind of flying compares with the same window over the weeks before
(and whether a kind is new rather than merely quiet), the routes most flown, the aircraft types, and for medical, military, skydive and survey
flights, which airports they come from and go to.

## Runways

The airport database carries runways as well: 11,861 runways at 8,666 airports, from OurAirports (public domain), built into the binary. Each
end has its threshold position, elevation, identifier and true heading; a missing heading is computed from the two thresholds, or from the
runway number. This is what lets a flight end be named with a runway and an approach be measured against one.

Rebuild it with `python3 tools/gen_runways.py` after refreshing `runways.csv`; it keeps only the airports already in `airports.tsv`.

## Retention

The store keeps 2 million flights and 2 GB by default, with no age limit; the oldest flights go first, and a flight's track goes with it.
Tracks can be dropped sooner than their flights, or not kept at all, which makes the file very much smaller while leaving every figure and every
answer above intact.

## Limits worth knowing

- "Hours airborne" is time heard over this receiver, never the airframe's total. The screens say so where it matters.
- The age of an airframe is not in any offline database here. It is a fact you research and enter, which is why facts are free text.
- The kind of flying is a reading of the evidence, not a fact. Where it matters, overrule it.
