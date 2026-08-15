#!/usr/bin/env python3
"""Build the committed evaluation corpus and its golden set.

WHY THIS SCRIPT IS IN THE REPOSITORY
------------------------------------
The entity-resolution numbers in the README are measured against data that
ships with the project, so that a reviewer who clones the repository can
reproduce them without downloading anything. That data therefore has to have a
checkable provenance, and this script is it: it says exactly which records
exist, how each source distorts them, and where every golden-set label came
from.

WHAT THE GROUND TRUTH IS, AND WHAT IT PROVES
--------------------------------------------
Each generated record carries a hidden truth id - the UN/LOCODE of the real
port it describes - and the golden set is derived from those ids. That is
ground truth BY CONSTRUCTION, and it is worth being precise about what it does
and does not establish.

It DOES establish that the resolver recovers identity from noisy, disagreeing
attributes, because the resolver never sees the truth id. It has to work from
names in different cases, coordinates at different precisions, missing codes and
conflicting country fields, exactly as it would on real data. This is the same
arrangement as the standard record-linkage benchmarks (Amazon-Google,
DBLP-Scholar): a curated correspondence the matcher must rediscover.

It does NOT establish that the noise model matches reality. The distortions
below were chosen from what the real sources actually do - the World Port Index
writes names in capitals and leaves many UN/LOCODE cells blank, UN/LOCODE keeps
diacritics and degree-minute coordinates - but they are a model, and a model is
always kinder than the world. The number to trust more is the one measured
after `data/fetch.sh` on the full datasets; this one is the number anyone can
check.

The single rule that keeps this honest: **the noise model was never tuned
against the scorer.** It was written first, and the weights were fitted to it
afterwards on a training split, with a held-out split for the reported number.

Usage:
    python3 eval/make_corpus.py
"""

import csv
import json
import os
import random

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SNAP = os.path.join(ROOT, "data", "snapshots")
EVAL = os.path.join(ROOT, "eval")

# A fixed seed. The corpus is committed, so this only matters when someone
# regenerates it - but a corpus that changes under you is a corpus whose
# metrics cannot be compared across commits.
RNG = random.Random(20260813)

# --- the real ports -----------------------------------------------------------
# (locode, name, country, lat, lon). Coordinates are the port area to about a
# hundredth of a degree, which is roughly a kilometre - finer than any of the
# sources this stands in for.
PORTS = [
    ("NLRTM", "Rotterdam", "NL", 51.92, 4.48),
    ("NLAMS", "Amsterdam", "NL", 52.38, 4.90),
    ("BEANR", "Antwerpen", "BE", 51.22, 4.40),
    ("BEZEE", "Zeebrugge", "BE", 51.33, 3.20),
    ("DEHAM", "Hamburg", "DE", 53.55, 9.97),
    ("DEBRV", "Bremerhaven", "DE", 53.55, 8.58),
    ("DEWVN", "Wilhelmshaven", "DE", 53.51, 8.14),
    ("DEKEL", "Kiel", "DE", 54.32, 10.14),
    ("DERSK", "Rostock", "DE", 54.09, 12.10),
    ("FRLEH", "Le Havre", "FR", 49.48, 0.12),
    ("FRMRS", "Marseille", "FR", 43.30, 5.37),
    ("FRDKK", "Dunkerque", "FR", 51.03, 2.37),
    ("FRNTE", "Nantes", "FR", 47.22, -1.55),
    ("FRBOD", "Bordeaux", "FR", 44.84, -0.58),
    ("GBFXT", "Felixstowe", "GB", 51.96, 1.35),
    ("GBSOU", "Southampton", "GB", 50.90, -1.40),
    ("GBLON", "London", "GB", 51.51, -0.13),
    ("GBLIV", "Liverpool", "GB", 53.41, -3.00),
    ("GBGSY", "Grimsby", "GB", 53.57, -0.08),
    ("GBIMM", "Immingham", "GB", 53.63, -0.19),
    ("IEDUB", "Dublin", "IE", 53.35, -6.22),
    ("IEORK", "Cork", "IE", 51.90, -8.47),
    ("ESBIO", "Bilbao", "ES", 43.26, -2.93),
    ("ESVLC", "Valencia", "ES", 39.45, -0.33),
    ("ESBCN", "Barcelona", "ES", 41.35, 2.17),
    ("ESALG", "Algeciras", "ES", 36.13, -5.44),
    ("PTLIS", "Lisboa", "PT", 38.72, -9.14),
    ("PTSIN", "Sines", "PT", 37.95, -8.87),
    ("PTLEI", "Leixoes", "PT", 41.18, -8.70),
    ("ITGOA", "Genova", "IT", 44.41, 8.93),
    ("ITSPE", "La Spezia", "IT", 44.10, 9.83),
    ("ITGIT", "Gioia Tauro", "IT", 38.43, 15.90),
    ("ITTRS", "Trieste", "IT", 45.65, 13.77),
    ("ITLIV", "Livorno", "IT", 43.55, 10.30),
    ("ITNAP", "Napoli", "IT", 40.84, 14.25),
    ("GRPIR", "Piraeus", "GR", 37.94, 23.64),
    ("GRSKG", "Thessaloniki", "GR", 40.63, 22.93),
    ("TRIST", "Istanbul", "TR", 41.01, 28.98),
    ("TRIZM", "Izmir", "TR", 38.42, 27.14),
    ("TRMER", "Mersin", "TR", 36.80, 34.63),
    ("ROCND", "Constanta", "RO", 44.17, 28.65),
    ("BGVAR", "Varna", "BG", 43.20, 27.92),
    ("UAODS", "Odesa", "UA", 46.48, 30.73),
    ("RUNVS", "Novorossiysk", "RU", 44.72, 37.77),
    ("RULED", "Sankt-Peterburg", "RU", 59.93, 30.31),
    ("RUKGD", "Kaliningrad", "RU", 54.71, 20.51),
    ("PLGDN", "Gdansk", "PL", 54.35, 18.65),
    ("PLGDY", "Gdynia", "PL", 54.52, 18.53),
    ("PLSZZ", "Szczecin", "PL", 53.43, 14.55),
    ("LTKLJ", "Klaipeda", "LT", 55.70, 21.14),
    ("LVRIX", "Riga", "LV", 56.95, 24.11),
    ("LVVNT", "Ventspils", "LV", 57.39, 21.56),
    ("EETLL", "Tallinn", "EE", 59.44, 24.75),
    ("FIHEL", "Helsinki", "FI", 60.17, 24.94),
    ("FIKTK", "Kotka", "FI", 60.47, 26.95),
    ("FITKU", "Turku", "FI", 60.45, 22.27),
    ("FIRAU", "Rauma", "FI", 61.13, 21.50),
    ("FIPOR", "Pori", "FI", 61.49, 21.80),
    ("FIOUL", "Oulu", "FI", 65.01, 25.47),
    ("FIHKO", "Hanko", "FI", 59.83, 22.97),
    ("FIKEM", "Kemi", "FI", 65.74, 24.56),
    ("SESTO", "Stockholm", "SE", 59.33, 18.07),
    ("SEGOT", "Goteborg", "SE", 57.71, 11.97),
    ("SEMMA", "Malmo", "SE", 55.61, 13.00),
    ("SEHEL", "Helsingborg", "SE", 56.05, 12.69),
    ("NOOSL", "Oslo", "NO", 59.91, 10.75),
    ("NOBGO", "Bergen", "NO", 60.39, 5.32),
    ("NOSVG", "Stavanger", "NO", 58.97, 5.73),
    ("NOTRD", "Trondheim", "NO", 63.43, 10.40),
    ("DKCPH", "Kobenhavn", "DK", 55.68, 12.57),
    ("DKAAR", "Aarhus", "DK", 56.16, 10.20),
    ("DKAAL", "Aalborg", "DK", 57.05, 9.92),
    ("DKEBJ", "Esbjerg", "DK", 55.47, 8.45),
    ("ISREY", "Reykjavik", "IS", 64.15, -21.94),
    ("SGSIN", "Singapore", "SG", 1.29, 103.85),
    ("HKHKG", "Hong Kong", "HK", 22.32, 114.17),
    ("CNSHA", "Shanghai", "CN", 31.23, 121.47),
    ("CNSZX", "Shenzhen", "CN", 22.54, 114.06),
    ("CNNGB", "Ningbo", "CN", 29.87, 121.55),
    ("CNTAO", "Qingdao", "CN", 36.07, 120.38),
    ("CNTSN", "Tianjin", "CN", 39.13, 117.20),
    ("CNCAN", "Guangzhou", "CN", 23.13, 113.26),
    ("KRPUS", "Busan", "KR", 35.18, 129.08),
    ("KRINC", "Incheon", "KR", 37.46, 126.63),
    ("JPTYO", "Tokyo", "JP", 35.65, 139.76),
    ("JPYOK", "Yokohama", "JP", 35.45, 139.64),
    ("JPUKB", "Kobe", "JP", 34.68, 135.19),
    ("JPNGO", "Nagoya", "JP", 35.09, 136.88),
    ("JPOSA", "Osaka", "JP", 34.65, 135.43),
    ("TWKHH", "Kaohsiung", "TW", 22.61, 120.28),
    ("MYPKG", "Port Klang", "MY", 3.00, 101.39),
    ("MYTPP", "Tanjung Pelepas", "MY", 1.36, 103.55),
    ("THLCH", "Laem Chabang", "TH", 13.08, 100.88),
    ("THBKK", "Bangkok", "TH", 13.71, 100.57),
    ("VNSGN", "Ho Chi Minh City", "VN", 10.77, 106.70),
    ("VNHPH", "Haiphong", "VN", 20.86, 106.68),
    ("IDJKT", "Jakarta", "ID", -6.11, 106.88),
    ("PHMNL", "Manila", "PH", 14.58, 120.97),
    ("LKCMB", "Colombo", "LK", 6.94, 79.84),
    ("INBOM", "Mumbai", "IN", 18.95, 72.84),
    ("INMAA", "Chennai", "IN", 13.10, 80.30),
    ("INNSA", "Nhava Sheva", "IN", 18.95, 72.95),
    ("AEJEA", "Jebel Ali", "AE", 25.01, 55.06),
    ("AEDXB", "Dubai", "AE", 25.27, 55.30),
    ("SAJED", "Jeddah", "SA", 21.48, 39.19),
    ("OMSLL", "Salalah", "OM", 16.94, 54.01),
    ("EGPSD", "Port Said", "EG", 31.26, 32.30),
    ("EGALY", "Alexandria", "EG", 31.20, 29.92),
    ("ZADUR", "Durban", "ZA", -29.87, 31.03),
    ("ZACPT", "Cape Town", "ZA", -33.92, 18.42),
    ("NGLOS", "Lagos", "NG", 6.45, 3.39),
    ("GHTEM", "Tema", "GH", 5.62, 0.00),
    ("CIABJ", "Abidjan", "CI", 5.32, -4.02),
    ("USNYC", "New York", "US", 40.71, -74.01),
    ("USLAX", "Los Angeles", "US", 33.74, -118.27),
    ("USLGB", "Long Beach", "US", 33.75, -118.19),
    ("USSAV", "Savannah", "US", 32.08, -81.09),
    ("USHOU", "Houston", "US", 29.76, -95.37),
    ("USSEA", "Seattle", "US", 47.61, -122.33),
    ("USOAK", "Oakland", "US", 37.80, -122.28),
    ("USORF", "Norfolk", "US", 36.85, -76.29),
    ("USCHS", "Charleston", "US", 32.78, -79.93),
    ("USMIA", "Miami", "US", 25.77, -80.19),
    ("USBAL", "Baltimore", "US", 39.29, -76.61),
    ("CAVAN", "Vancouver", "CA", 49.28, -123.12),
    ("CAMTR", "Montreal", "CA", 45.50, -73.57),
    ("CAHAL", "Halifax", "CA", 44.65, -63.57),
    ("BRSSZ", "Santos", "BR", -23.96, -46.33),
    ("BRRIO", "Rio de Janeiro", "BR", -22.91, -43.17),
    ("ARBUE", "Buenos Aires", "AR", -34.60, -58.38),
    ("CLVAP", "Valparaiso", "CL", -33.05, -71.62),
    ("PECLL", "Callao", "PE", -12.05, -77.15),
    ("COCTG", "Cartagena", "CO", 10.39, -75.51),
    ("MXZLO", "Manzanillo", "MX", 19.05, -104.32),
    ("MXVER", "Veracruz", "MX", 19.19, -96.14),
    ("PACLN", "Colon", "PA", 9.36, -79.90),
    ("PABLB", "Balboa", "PA", 8.95, -79.57),
    ("JMKIN", "Kingston", "JM", 17.97, -76.79),
    ("AUSYD", "Sydney", "AU", -33.87, 151.21),
    ("AUMEL", "Melbourne", "AU", -37.81, 144.96),
    ("AUBNE", "Brisbane", "AU", -27.47, 153.03),
    ("AUFRE", "Fremantle", "AU", -32.06, 115.74),
    ("NZAKL", "Auckland", "NZ", -36.85, 174.76),
    ("NZTRG", "Tauranga", "NZ", -37.69, 176.17),
]

# The diacritics UN/LOCODE actually carries, keyed by locode. This is the source
# of the strip_diacritics workload and of a genuine cross-source disagreement:
# the World Port Index writes ASCII, UN/LOCODE does not.
DIACRITICS = {
    "SEGOT": "Göteborg",
    "SEMMA": "Malmö",
    "DKAAR": "Århus",
    "DKCPH": "København",
    "DKAAL": "Ålborg",
    "PLGDN": "Gdańsk",
    "LTKLJ": "Klaipėda",
    "ROCND": "Constanța",
    "CLVAP": "Valparaíso",
    "PTLEI": "Leixões",
    "TRIZM": "İzmir",
    "NOTRD": "Trondheim",
    "FITKU": "Turku (Åbo)",
    "FIHEL": "Helsinki (Helsingfors)",
    "EETLL": "Tallinn",
}

# Sub-locations: a second entry in the same source for the same port complex.
# These are the hardest true positives in the set, because the names differ and
# only one of the two carries the code.
SUBLOCATIONS = {
    "NLRTM": ["Rotterdam Botlek", "Europoort"],
    "DEHAM": ["Hamburg Waltershof"],
    "BEANR": ["Antwerpen Deurganck"],
    "USNYC": ["New York and New Jersey"],
    "SGSIN": ["Singapore Jurong"],
    "CNSHA": ["Shanghai Yangshan"],
    "GBLON": ["London Gateway"],
    "USLGB": ["Long Beach Pier T"],
    "FRLEH": ["Le Havre Port 2000"],
    "MYPKG": ["Port Klang Westport"],
}

HARBOR_SIZE = ["Large", "Medium", "Small", "Very Small"]


def ddmm(lat, lon):
    """Degree-and-whole-minute text, the way UN/LOCODE writes coordinates.

    The rounding to whole minutes is not cosmetic - it discards about 1.8 km of
    precision, which is why the scorer cannot treat a coordinate match as
    decisive.
    """
    def one(value, is_lat):
        hemi = ("N" if value >= 0 else "S") if is_lat else ("E" if value >= 0 else "W")
        value = abs(value)
        deg = int(value)
        minutes = int(round((value - deg) * 60))
        if minutes == 60:
            deg += 1
            minutes = 0
        width = 2 if is_lat else 3
        return f"{deg:0{width}d}{minutes:02d}{hemi}"

    return f"{one(lat, True)} {one(lon, False)}"


def jitter(value, km):
    """Move a coordinate by up to `km` kilometres, roughly."""
    return value + RNG.uniform(-km, km) / 111.0


def write_wpi(records):
    """The World Port Index: capitals, patchy codes, semicolon alternates."""
    path = os.path.join(SNAP, "wpi", "UpdatedPub150.csv")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow([
            "World Port Index Number", "Region Name", "Main Port Name",
            "Alternate Port Name", "UN/LOCODE", "Country Code",
            "World Water Body", "Harbor Size", "Harbor Type",
            "Latitude", "Longitude",
        ])
        for r in records:
            w.writerow([
                r["wpi_number"], r["region"], r["name"].upper(),
                "; ".join(r["alternates"]), r["locode"], r["country"],
                r["water_body"], r["harbor_size"], r["harbor_type"],
                f"{r['lat']:.4f}", f"{r['lon']:.4f}",
            ])
    return path


def write_unlocode(rows):
    path = os.path.join(SNAP, "unlocode", "code-list.csv")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow([
            "Change", "Country", "Location", "Name", "NameWoDiacritics",
            "Subdivision", "Status", "Function", "Date", "IATA",
            "Coordinates", "Remarks",
        ])
        for r in rows:
            w.writerow([
                "", r["country"], r["location"], r["name"], r["plain"],
                "", "AI", r["function"], "0401", "", r["coordinates"], "",
            ])
    return path


def write_digitraffic_ports(rows):
    path = os.path.join(SNAP, "digitraffic", "ports.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1, ensure_ascii=False)
        f.write("\n")
    return path


def build_ports():
    """Produce the three port sources plus the truth map."""
    wpi_rows, unlocode_rows, digitraffic_rows = [], [], []
    # record id -> truth id, where a record id is "<source>:<natural key>"
    truth = {}

    water_bodies = ["North Sea", "Baltic Sea", "Mediterranean Sea",
                    "North Atlantic Ocean", "South China Sea", "Pacific Ocean",
                    "Indian Ocean", "Caribbean Sea"]

    for index, (locode, name, country, lat, lon) in enumerate(PORTS):
        wpi_number = 10000 + index * 10

        # --- World Port Index -------------------------------------------------
        # Roughly a quarter of real Pub 150 rows carry no UN/LOCODE at all,
        # which is the single most important gap in this dataset: it is what
        # forces the resolver to work from name and geography.
        has_code = RNG.random() > 0.25
        alternates = list(SUBLOCATIONS.get(locode, []))[:1]
        wpi_rows.append({
            "wpi_number": wpi_number,
            "region": water_bodies[index % len(water_bodies)],
            "name": name,
            "alternates": alternates,
            "locode": locode if has_code else "",
            "country": country,
            "water_body": water_bodies[index % len(water_bodies)],
            "harbor_size": HARBOR_SIZE[index % len(HARBOR_SIZE)],
            "harbor_type": "Coastal Natural",
            "lat": jitter(lat, 3.0),
            "lon": jitter(lon, 3.0),
        })
        truth[f"wpi:{wpi_number}"] = locode

        # A sub-location as its own World Port Index row, with no code. Same
        # port complex, different name, 10 km away.
        for offset, sub in enumerate(SUBLOCATIONS.get(locode, [])):
            sub_number = wpi_number + offset + 1
            wpi_rows.append({
                "wpi_number": sub_number,
                "region": water_bodies[index % len(water_bodies)],
                "name": sub,
                "alternates": [],
                "locode": "",
                "country": country,
                "water_body": water_bodies[index % len(water_bodies)],
                "harbor_size": "Medium",
                "harbor_type": "River Basin",
                "lat": jitter(lat, 12.0),
                "lon": jitter(lon, 12.0),
            })
            truth[f"wpi:{sub_number}"] = locode

        # --- UN/LOCODE --------------------------------------------------------
        display = DIACRITICS.get(locode, name)
        unlocode_rows.append({
            "country": country,
            "location": locode[2:],
            "name": display,
            "plain": name,
            "function": "1-3-----" if index % 3 else "12345---",
            "coordinates": ddmm(lat, lon),
        })
        truth[f"unlocode:{country}|{locode[2:]}"] = locode

        # --- Digitraffic ------------------------------------------------------
        # A metadata endpoint covering about half the ports, in its own casing.
        if index % 2 == 0:
            digitraffic_rows.append({
                "locode": locode,
                "portName": name.upper(),
                "country": country,
            })
            truth[f"digitraffic:{locode}"] = locode

    # Non-seaport rows, so the Function bitfield filter has something to reject.
    for code, place, country in [("CDG", "Paris Charles de Gaulle Apt", "FR"),
                                 ("AMF", "Amersfoort", "NL"),
                                 ("MUC", "Munchen Apt", "DE"),
                                 ("ZRH", "Zurich Apt", "CH"),
                                 ("DEN", "Denver Apt", "US")]:
        unlocode_rows.append({
            "country": country, "location": code, "name": place,
            "plain": place, "function": "-2------",
            "coordinates": ddmm(50.0, 5.0),
        })

    return wpi_rows, unlocode_rows, digitraffic_rows, truth


# --- vessels ------------------------------------------------------------------

VESSEL_NAMES = [
    "AURORA BOREALIS", "BALTIC TRADER", "MAAS EXPRESS", "ELBE PIONEER",
    "GOTA LINK", "MONROVIA STAR", "NORDIC SPIRIT", "ATLANTIC DAWN",
    "PACIFIC HORIZON", "CORAL VOYAGER", "IRON DUCHESS", "SILVER MERIDIAN",
    "CAPE FINISTERRE", "GULF SENTINEL", "ARCTIC RESOLVE", "SOUTHERN CROSS",
    "HANSEATIC BAY", "IBERIAN TRADER", "ADRIATIC PEARL", "AEGEAN WIND",
    "BOSPHORUS QUEEN", "CASPIAN GATE", "DANUBE RIVER", "EMERALD ISLE",
    "FJORD RUNNER", "GRAND BANKS", "HELLENIC SUN", "INDIAN OCEAN",
    "JADE HARMONY", "KATTEGAT STAR", "LOIRE VALLEY", "MEDITERRANEAN SKY",
    "NORTH CAPE", "ORION TRADER", "PANAMA EXPRESS", "QUANTUM LEAP",
    "RHINE CARRIER", "SUEZ PIONEER", "THAMES SPIRIT", "URAL MOUNTAIN",
]

# MIDs whose flag state the transform registry knows, so the derived flag can
# be checked against the declared one.
MIDS = [230, 244, 211, 265, 636, 353, 232, 366, 563, 477, 249, 538]


def imo_check_digit(six):
    total = sum(int(d) * w for d, w in zip(six, [7, 6, 5, 4, 3, 2]))
    return str(total % 10)


def build_vessels():
    """Two sources for vessels, so hulls have cross-source duplicates offline.

    Both are Digitraffic endpoints - `vessel_details` from the port-call API and
    `ais_vessels` from the AIS API. They genuinely disagree: the port-call feed
    carries a registry name and an IMO, while the AIS feed carries whatever the
    transmitter broadcasts, which is frequently truncated, misspelled or absent.

    They are declared as two SOURCES rather than two endpoints of one source,
    which is not bookkeeping. SRCREC is keyed by (source_id, natural_key_hash)
    with no endpoint in the key, so two feeds sharing a source id and keying on
    MMSI would overwrite each other - the second ingest would silently replace
    the first and the vessels would have no cross-source duplicates at all.
    Separate sources also let them carry separate trust weights, which is
    correct: a registry name outranks a broadcast one.
    """
    details, ais, truth = [], [], {}

    for index, name in enumerate(VESSEL_NAMES):
        # Six digits then the check digit. Written as an explicit six-character
        # string rather than a formatted expression that gets truncated: an
        # earlier version of this line produced ten hulls sharing one IMO, and
        # since the IMO is the truth id, that quietly turned ten distinct
        # vessels into one and inflated the match count fourfold.
        six = str(900000 + index)
        assert len(six) == 6
        imo = six + imo_check_digit(six)
        mid = MIDS[index % len(MIDS)]
        mmsi = mid * 1000000 + 100000 + index * 7
        call_sign = f"{chr(ord('A') + index % 26)}{chr(ord('A') + (index * 3) % 26)}{1000 + index}"

        details.append({
            "name": name,
            "imo": int(imo),
            "mmsi": mmsi,
            "callSign": call_sign,
            "vesselTypeCode": [70, 80, 60, 52, 30][index % 5],
            "grossTonnage": 8000 + index * 1300,
        })
        truth[f"digitraffic:{mmsi}"] = imo

        # The AIS view of the same hull. About a fifth broadcast no IMO at all,
        # which is the case that forces the resolver onto weak identifiers.
        broadcast = name
        if index % 4 == 0:
            broadcast = name[:12].strip()          # truncated by the transmitter
        elif index % 4 == 1:
            broadcast = "MV " + name.title()        # type prefix, mixed case
        elif index % 4 == 2:
            broadcast = name.replace("A", "4", 1)   # a transcription slip

        ais_mmsi = mmsi
        ais_imo = 0 if index % 5 == 0 else int(imo)
        ais_truth = imo

        # --- the two cases that make this set worth measuring ----------------
        #
        # Without these, every vessel pair shares an identical MMSI and the
        # resolver only has to compare one integer. That would produce a
        # flattering F1 that proves nothing.

        # 1. A TRANSCRIPTION SLIP IN THE MMSI. Same hull, one digit different,
        #    so the strongest weak identifier disagrees and the match has to be
        #    carried by the IMO and the name. MMSIs here are spaced seven apart,
        #    so shifting by one cannot collide with another record.
        if index % 7 == 3:
            ais_mmsi = mmsi + 1

        # 2. MMSI REASSIGNMENT. An MMSI belongs to the radio licence, not the
        #    hull, and is reissued when a vessel reflags - which the ontology
        #    says out loud and which nothing has tested until now. Here the AIS
        #    feed reports a DIFFERENT hull under an MMSI the registry feed
        #    still associates with the original one.
        #
        #    This is the pair that breaks naive resolution: the single most
        #    identifying-looking field matches exactly, and the answer is still
        #    no. It is also precisely the veto edge that day 10's
        #    veto-constrained clustering exists to respect - two records with
        #    conflicting IMO numbers must never be merged, however much else
        #    they agree on.
        elif index in (6, 18, 30):
            other = (index + 13) % len(VESSEL_NAMES)
            other_six = str(900000 + 500 + other)
            other_imo = other_six + imo_check_digit(other_six)
            broadcast = VESSEL_NAMES[other]
            ais_imo = int(other_imo)
            ais_truth = other_imo  # a different hull, so a different truth id

        ais.append({
            "name": broadcast,
            "imo": ais_imo,
            "mmsi": ais_mmsi,
            "callSign": call_sign.lower() if index % 3 else "",
            "shipType": [70, 80, 60, 52, 30][index % 5],
        })
        truth[f"digitraffic_ais:{ais_mmsi}"] = ais_truth

    return details, ais, truth


def build_port_calls(port_locodes, vessel_mmsis, count=1000):
    """Port calls, which ARE the Voyage entity.

    These exist to give the graph something to traverse and the time index
    something to select. Each call references a real vessel MMSI and two real
    UN/LOCODEs, because a link to an identifier that is not in the data resolves
    to nothing - which is exactly the bug that made this function necessary:
    the hand-written port_calls.json survived a corpus regeneration and went on
    naming MMSIs that no longer existed.

    Arrivals are spread across a year so that "everything through this port last
    quarter" selects a genuine subset rather than everything or nothing.
    """
    calls = []
    for index in range(count):
        arrival_day = index % 365
        # Finnish local time with an offset, the way Digitraffic reports it.
        offset = "+03:00" if 90 <= arrival_day <= 300 else "+02:00"
        month = arrival_day // 31 + 1
        day = arrival_day % 28 + 1
        hour = (index * 7) % 24

        # Traffic is skewed, because real traffic is: a handful of hub ports
        # handle most calls and the long tail handles a few each. A uniform
        # spread would put one arrival per port per quarter, which makes the
        # time-range query technically correct and useless as a demonstration -
        # a range scan returning one row proves nothing about range scans.
        hubs = max(1, len(port_locodes) // 8)
        if index % 5 != 0:
            to_port = port_locodes[index % hubs]
        else:
            to_port = port_locodes[index % len(port_locodes)]
        from_port = port_locodes[(index * 13 + 5) % len(port_locodes)]
        if from_port == to_port:
            from_port = port_locodes[(index + 1) % len(port_locodes)]

        detail = {
            "ata": f"2026-{month:02d}-{day:02d}T{hour:02d}:15:00{offset}",
            "atd": f"2026-{month:02d}-{day:02d}T{(hour + 9) % 24:02d}:40:00{offset}",
        }
        # A few calls with no arrival recorded at all, and a few with a
        # malformed one, because both occur in the real feed.
        #
        # Offset off zero deliberately: the first record in a sample file is the
        # one people read, and starting with a degenerate case makes the format
        # look wrong. It also makes any test that grabs the first record
        # accidentally test the exception rather than the rule.
        if index % 40 == 13:
            details = []
        elif index % 37 == 21:
            details = [{"ata": "not a timestamp", "atd": None}]
        else:
            details = [detail]

        calls.append({
            "portCallId": 3120000 + index,
            "portToVisit": to_port,
            "prevPort": from_port,
            "mmsi": vessel_mmsis[index % len(vessel_mmsis)],
            "portAreaDetails": details,
        })
    return {"portCalls": calls}


def write_json(path, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1, ensure_ascii=False)
        f.write("\n")
    return path


# --- the golden set -----------------------------------------------------------

def build_golden(truth, path, negatives_per_record=3):
    """Every cross-source pair sharing a truth id, plus hard negatives.

    The negatives are the part that decides whether the number means anything.
    Random pairs are trivially separable and would inflate precision to
    meaninglessness, so negatives are drawn from records that a blocking key
    would plausibly put together: same country, or a shared leading character.
    """
    by_truth = {}
    for record, truth_id in truth.items():
        by_truth.setdefault(truth_id, []).append(record)

    pairs = []
    for truth_id, records in sorted(by_truth.items()):
        records = sorted(records)
        for i in range(len(records)):
            for j in range(i + 1, len(records)):
                pairs.append((records[i], records[j], "match", truth_id))

    all_records = sorted(truth.keys())
    seen = {(a, b) for a, b, _, _ in pairs}

    # Forced hard negatives: two records that share a natural key across sources
    # but describe different things. For vessels that is MMSI reassignment - the
    # single most identifying-looking field agrees exactly and the answer is
    # still no.
    #
    # These are added explicitly rather than left to the random sampler below,
    # because they are the pairs the whole veto mechanism exists for and there
    # are only a handful of them. Sampling would have included them by luck or
    # not at all, and "or not at all" is how a test quietly stops testing the
    # thing it was written for.
    by_key = {}
    for record in all_records:
        _, natural_key = record.split(":", 1)
        by_key.setdefault(natural_key, []).append(record)
    for natural_key, records in sorted(by_key.items()):
        for i in range(len(records)):
            for j in range(i + 1, len(records)):
                a, b = records[i], records[j]
                if truth[a] == truth[b]:
                    continue
                key = tuple(sorted((a, b)))
                if key in seen:
                    continue
                seen.add(key)
                pairs.append((key[0], key[1], "non_match", ""))

    for record in all_records:
        candidates = [
            other for other in all_records
            if other != record
            and truth[other] != truth[record]
            # Same country, or the same first letter of the location part:
            # exactly the sort of pair a cheap blocking key hands to the scorer.
            and (truth[other][:2] == truth[record][:2]
                 or truth[other][2:3] == truth[record][2:3])
        ]
        RNG.shuffle(candidates)
        for other in candidates[:negatives_per_record]:
            key = tuple(sorted((record, other)))
            if key in seen:
                continue
            seen.add(key)
            pairs.append((key[0], key[1], "non_match", ""))

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["record_a", "record_b", "label", "truth_id"])
        for a, b, label, truth_id in pairs:
            w.writerow([a, b, label, truth_id])

    matches = sum(1 for p in pairs if p[2] == "match")
    return path, len(pairs), matches


def main():
    wpi_rows, unlocode_rows, digitraffic_rows, port_truth = build_ports()
    details, ais, vessel_truth = build_vessels()

    port_calls = build_port_calls([row["locode"] for row in digitraffic_rows],
                                  [row["mmsi"] for row in details])

    written = [
        write_wpi(wpi_rows),
        write_unlocode(unlocode_rows),
        write_digitraffic_ports(digitraffic_rows),
        write_json(os.path.join(SNAP, "digitraffic", "vessel_details.json"), details),
        write_json(os.path.join(SNAP, "digitraffic", "ais_vessels.json"), ais),
        write_json(os.path.join(SNAP, "digitraffic", "port_calls.json"), port_calls),
    ]
    for path in written:
        print(f"  {os.path.relpath(path, ROOT)}")

    ports_path, ports_pairs, ports_matches = build_golden(
        port_truth, os.path.join(EVAL, "golden_ports.csv"))
    vessels_path, vessel_pairs, vessel_matches = build_golden(
        vessel_truth, os.path.join(EVAL, "golden_vessels.csv"))

    print(f"\n  {os.path.relpath(ports_path, ROOT)}: "
          f"{ports_pairs} pairs, {ports_matches} matches")
    print(f"  {os.path.relpath(vessels_path, ROOT)}: "
          f"{vessel_pairs} pairs, {vessel_matches} matches")
    print(f"\n  {len(wpi_rows)} wpi, {len(unlocode_rows)} unlocode, "
          f"{len(digitraffic_rows)} digitraffic ports")
    print(f"  {len(details)} vessel_details, {len(ais)} ais_vessels, "
          f"{len(port_calls['portCalls'])} port_calls")


if __name__ == "__main__":
    main()
