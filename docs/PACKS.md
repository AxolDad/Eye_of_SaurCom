# Data packs (work without internet)

View > Data packs lists data you can download once and then use offline: map tiles (a region or the area around your station), satellite elements (CelesTrak)
and airports and their frequencies (OurAirports). Each row shows size, source, licence (hover for the credit that has to
stay visible) and the date of the last download.

- Nothing is downloaded by itself. A pack starts only when you click Download (or Update) on it.
- Stop keeps what is already in place; starting the pack again continues with the missing files (files that are there and correct are skipped).
- Check compares every file with the checksum recorded at the download (or the pinned one, for packs that list one). Delete removes the pack's files.
- The disk guard refuses a pack that would leave less than 200 MB free.
- Tile packs are limited to 4000 tiles (the OpenStreetMap tile policy forbids bulk downloads); the deepest zoom is lowered to fit.
- Where files go: tiles in `maps/osm` (the map reads them), satellite elements in `tle/`, airports are converted into the airport table, DAB lists in the cache folder.

## Your own packs

`<data folder>/packs/catalog.tsv`, tab separated, one file per line (lines with the same id form one pack; `#` starts a comment):

    id  title  category  source  licence  credit  https-address  path-in-data-folder  [size-in-bytes  [sha256]]

The address must be https, the path must stay inside the data folder. A pinned sha256 makes the download fail when the file differs.
