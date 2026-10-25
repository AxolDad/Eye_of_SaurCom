#!/usr/bin/env python3
"""Build data/airports/runways.tsv, the runway table, from the OurAirports runways.csv (public domain).

  python3 -I tools/gen_runways.py --runways RUNWAYS.csv [--airports data/airports/airports.tsv] [--out data/airports/runways.tsv]

The input is https://davidmegginson.github.io/ourairports-data/runways.csv; put it in an empty folder of its own (never run anything
from there). Only runways of the airports already in airports.tsv are kept, because an approach can only be measured against an airport
the program knows; a closed runway, and one whose two ends both lack a position, are dropped.

Output, one text file, tab separated, one line per runway:
  # comment lines
  R  airport  ident_le  lat_le  lon_le  elev_ft_le  hdg_le  ident_he  lat_he  lon_he  elev_ft_he  hdg_he  length_ft  width_ft  hard  lit

`hdg` is the true heading of that end in degrees (the direction an aircraft faces when landing on it), empty when OurAirports does not
give one and it cannot be computed from the two threshold positions. An end without a position has empty lat/lon. `hard` is 1 for a
paved surface (asphalt, concrete, bitumen, tarmac, paved), `lit` 1 when the runway is lighted. Elevations are feet, empty when unknown.
"""
import argparse, csv, hashlib, math, os, sys

HARD = ("ASP", "CON", "PEM", "BIT", "TAR", "PAV", "COP", "ASPH", "CONC")   # OurAirports writes these many ways


def clean(s):
    return " ".join(s.replace("\t", " ").replace("\r", " ").replace("\n", " ").split())


def num(s):
    """A number as OurAirports writes it, or None."""
    s = (s or "").strip()
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def is_hard(surface):
    u = (surface or "").upper()
    return int(any(h in u for h in HARD))


def bearing(lat1, lon1, lat2, lon2):
    """True bearing from one threshold to the other, degrees 0 - 360."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360


def ident_heading(ident):
    """The heading a runway number implies (09 -> 90 degrees); None when the ident is not a number."""
    d = "".join(c for c in (ident or "") if c.isdigit())
    if not d:
        return None
    n = int(d)
    if not (1 <= n <= 36):
        return None
    return n * 10.0


def fmt(v, nd):
    return "" if v is None else f"{v:.{nd}f}".rstrip("0").rstrip(".") if nd else str(int(round(v)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runways", required=True, help="OurAirports runways.csv")
    ap.add_argument("--airports", default="data/airports/airports.tsv", help="the built-in airport table, to filter by")
    ap.add_argument("--out", default="data/airports/runways.tsv")
    a = ap.parse_args()

    known = set()
    with open(a.airports, encoding="utf-8") as f:
        for line in f:
            if line.startswith("A\t"):
                known.add(line.split("\t")[1])
    if not known:
        sys.exit(f"{a.airports}: no airports read")

    raw = open(a.runways, "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    rows, kept, closed, unknown_ap, no_pos = [], 0, 0, 0, 0
    rd = csv.DictReader(raw.decode("utf-8", "replace").splitlines())
    for r in rd:
        apt = clean(r.get("airport_ident", ""))
        if apt not in known:
            unknown_ap += 1
            continue
        if (r.get("closed") or "").strip() in ("1", "yes", "true"):
            closed += 1
            continue
        le_lat, le_lon = num(r.get("le_latitude_deg")), num(r.get("le_longitude_deg"))
        he_lat, he_lon = num(r.get("he_latitude_deg")), num(r.get("he_longitude_deg"))
        if le_lat is None and he_lat is None:
            no_pos += 1
            continue
        le_id, he_id = clean(r.get("le_ident", "")), clean(r.get("he_ident", ""))
        le_hdg, he_hdg = num(r.get("le_heading_degT")), num(r.get("he_heading_degT"))
        # a missing heading: from the two thresholds when both are known, else from the runway number
        if le_hdg is None and None not in (le_lat, le_lon, he_lat, he_lon):
            le_hdg = bearing(le_lat, le_lon, he_lat, he_lon)
        if he_hdg is None and None not in (le_lat, le_lon, he_lat, he_lon):
            he_hdg = bearing(he_lat, he_lon, le_lat, le_lon)
        if le_hdg is None:
            le_hdg = ident_heading(le_id)
        if he_hdg is None:
            he_hdg = ident_heading(he_id)
        rows.append("\t".join(["R", apt,
                               le_id, fmt(le_lat, 6), fmt(le_lon, 6), fmt(num(r.get("le_elevation_ft")), 0), fmt(le_hdg, 1),
                               he_id, fmt(he_lat, 6), fmt(he_lon, 6), fmt(num(r.get("he_elevation_ft")), 0), fmt(he_hdg, 1),
                               fmt(num(r.get("length_ft")), 0), fmt(num(r.get("width_ft")), 0),
                               str(is_hard(r.get("surface"))), "1" if (r.get("lighted") or "").strip() == "1" else "0"]))
        kept += 1

    rows.sort(key=lambda s: (s.split("\t")[1], s.split("\t")[2]))
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w", encoding="utf-8", newline="\n") as o:
        o.write("# Runways of the airports in airports.tsv: built by tools/gen_runways.py from OurAirports (public domain, https://ourairports.com/data/).\n")
        o.write("# R airport ident_le lat_le lon_le elev_ft_le hdg_le ident_he lat_he lon_he elev_ft_he hdg_he length_ft width_ft hard lit\n")
        o.write(f"# runways.csv sha256 {sha}\n")
        o.write("\n".join(rows) + "\n")
    print(f"{kept} runways at {len(set(r.split(chr(9))[1] for r in rows))} of {len(known)} airports "
          f"({closed} closed, {no_pos} without a position, {unknown_ap} at airports not in the table)")


if __name__ == "__main__":
    main()
