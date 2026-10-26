# NOAA Weather Radio SAME (EAS alerts, US and Canada)

Receive only. The text of an alert is a public broadcast. Eye of ZorCom is not an alerting system: do not rely on it for your safety.

## What is decoded

- Seven narrow-FM channels, 162.400 / .425 / .450 / .475 / .500 / .525 / .550 MHz (WX1 - WX7).
- SAME data: AFSK at 520.83 baud, mark 2083.3 Hz, space 1562.5 Hz, bytes least significant bit first after 16 x 0xAB.
  `ZCZC-ORG-EEE-PSSCCC[-PSSCCC...]+TTTT-JJJHHMM-LLLLLLLL-` is sent three times, then `NNNN` three times.
- The repeats of one message are voted bit by bit (equal lengths; with only two repeats the character the format allows wins), then every
  field is checked: 3 capital letters each for originator and event, 1 - 31 six-digit locations, purge time (minutes below 60), day of year
  1 - 366, hour, minute, call sign. A message that fails is listed as damaged with the reason; a repeat that is whole by itself is used when the vote fails.
- Partial County Alerting: the first digit of a location code, 0 whole county, 1 - 9 northwest, north, northeast, west, central, east, southwest, south, southeast.
- The 1050 Hz attention tone (Goertzel detector, 0.5 s or longer) and which alert it followed.
- Event names: the 47 CFR 11.31 table; an event that is not in it is shown by its code (and its warning / watch / statement kind from the last letter).

## Names that are built in, and those that are not

- State, territory and DC names (FIPS PUB 5-2 codes), the NWS marine area codes and Great Lakes, and the Canadian provinces (Statistics Canada codes).
  The US and Canadian codes overlap (35 is New Mexico or Ontario) and the message does not say which: the "Country" setting is yours to choose.
  The Canadian table names only the province part; how Weatheradio Canada lays out the digits of its location codes has not been checked against a source here.
- **County names are not built in.** A location shows `county FIPS 113, Texas`. To name counties, load a table in the Receiver tab ("County CSV", Load) or
  from code with `dect2::same::loadCountyFile(path)`. Accepted lines (a header and blank lines are skipped, names may be quoted):
  - `SSCCC,Name` (for example `48113,Dallas County`)
  - `SS,CCC,Name`
  - the Census file `national_county.txt`: `TX,48,113,Dallas County,H1`
  The path is remembered. Nothing is downloaded.
- The map puts a marker at the middle of the state or province (moved a little toward the partial-county direction). It is an approximation, not the area of the alert.

## Test signal

`dect2cli --standard same --synthetic` (or the Synthetic source in the app): a tornado warning for three locations with partial county codes, the tone and
the ends, then a required weekly test. `tests/test_same_*.cpp` cover the parser, the vote and the receiver (carrier offsets, noise, a damaged repeat, a missing repeat, 31 locations, noise only).
