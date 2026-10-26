# GPS interference map

ADS-B aircraft broadcast how good their own GNSS position is. When a jammer or spoofer is near, many aircraft in one area report a collapsed accuracy or an impossible position jump. Eye of ZorCom counts this per grid cell, shows it on its ADS-B map, and can share the counts (and only the counts) with the public map.

## What is measured

For every aircraft heard with a fresh position (not on the ground), Eye of ZorCom looks at:

| Field | Source | Used |
|---|---|---|
| NACp (0 - 11) | operational status message (type code 31) | degraded when below 7 (position error over 0.1 nm) |
| NIC (0 - 11) | airborne position type code 9 - 18 (supplement bits not applied) | degraded when below 6 (containment over 1 nm) |
| Position jump | two fixes of one aircraft that imply more than 2000 kt within 5 minutes | degraded |
| SIL | operational status | recorded, not used in the verdict |

NACv and SDA are not decoded by the receiver yet and are not used. An aircraft whose transponder reports no accuracy (old, version 0) is neither good nor bad and is not counted.

The grid is 0.5 degree squares (about 55 km north-south). Per square and over a sliding hour, an aircraft counts once, and as degraded if it was degraded at any time in that hour. A square needs at least 3 aircraft to be shown. Colour by share of degraded aircraft: under 10 % green, 10 - 30 % yellow, 30 - 60 % orange, over 60 % red.

## Local layer

ADS-B map, "GPS interference" checkbox: the squares around your receiver, from what your own receiver heard in the last hour. Hover a square for its counts. It is off by default and costs nothing when off (the counting runs while the ADS-B screen is open).

## Limits: this is not proof of jamming

- Some aircraft always report poor accuracy: old transponders, certain GNSS receivers, aircraft on approach with a shaded antenna, and aircraft during GNSS outages unrelated to jamming (space weather, receiver faults).
- A square's share depends on who flies there: three aircraft are a small sample, and a square near an airport mixes in aircraft on the ground that are filtered out only by the ground flag.
- A receiver that decodes position wrongly (a CPR error) produces a jump that is not the aircraft's.
- A jammer's effect depends on altitude and line of sight: an aircraft at 35,000 ft sees much further than one at 3,000 ft, so squares are not a map of the jammer's position.
- Only aircraft that your receiver (or the stations contributing) can hear are counted; empty squares mean no data, not no jamming.

Treat a red square as "worth a look", compare it with other sources, and do not draw conclusions about who or what causes it.
