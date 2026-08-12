#!/usr/bin/env bash
#
# Download the source datasets into data/snapshots/.
#
# Everything here is free and open. The point of snapshotting to disk rather
# than fetching at ingest time is reproducibility: your build must not depend
# on a live network, and provenance that points at a response you can no longer
# reproduce is not provenance.
#
# Usage:  ./data/fetch.sh [all|unlocode|wpi|digitraffic|ais]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SNAP="$ROOT/data/snapshots"
UA="sextant/0.1 (student project)"

mkdir -p "$SNAP"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

fetch_unlocode() {
  log "UN/LOCODE (UNECE)"
  mkdir -p "$SNAP/unlocode"
  # The official ZIP lives behind an interstitial at
  #   https://unece.org/trade/cefact/UNLOCODE-Download
  # and its filename changes each release (cut-offs: 31 Mar and 30 Sep).
  # The datasets/un-locode mirror is pre-parsed and stable, which is what we
  # want for a reproducible build. Record which one you used in the batch
  # manifest so lineage stays honest.
  #
  # The mirror also carries a HEADER ROW, which the official distribution does
  # not. That matters more than it sounds: without one, a mapping would have to
  # address columns by position (`from: 5`), and the day the upstream format
  # inserts a column every value silently shifts one place to the left. Naming
  # the column makes that a load error instead.
  curl -fsSL -A "$UA" \
    -o "$SNAP/unlocode/code-list.csv" \
    "https://raw.githubusercontent.com/datasets/un-locode/main/data/code-list.csv"
  curl -fsSL -A "$UA" \
    -o "$SNAP/unlocode/country-codes.csv" \
    "https://raw.githubusercontent.com/datasets/un-locode/main/data/country-codes.csv" || true
  wc -l "$SNAP/unlocode/code-list.csv"
}

fetch_wpi() {
  log "NGA World Port Index (Pub 150)"
  mkdir -p "$SNAP/wpi"
  cat <<'EOF'
  MANUAL STEP.
  The NGA distributes Pub 150 through a portal that does not permit scripted
  download. Fetch it once by hand:

      https://msi.nga.mil/Publications/WPI

  Download the CSV ("World Port Index") and save it as:

      data/snapshots/wpi/UpdatedPub150.csv

  Then re-run this script. Note the download date in
  data/snapshots/wpi/BATCH.txt so the provenance records a real batch.
EOF
}

fetch_digitraffic() {
  log "Fintraffic Digitraffic (marine)"

  # LAYOUT NOTE. The connector reads data/snapshots/digitraffic/<endpoint>.json,
  # flat, because that is what `snapshot_dir` in the mapping file points at. So
  # the live files are written there and a timestamped copy goes into archive/
  # alongside. No symlinks: they do not survive a clone on Windows, and this
  # project is built there.
  local live="$SNAP/digitraffic"
  local batch; batch="$(date -u +%Y%m%dT%H%M%SZ)"
  local archive="$live/archive/$batch"
  mkdir -p "$live" "$archive"

  # Free and keyless. The Digitraffic-User header is requested by their terms.
  local base="https://meri.digitraffic.fi"
  for ep in \
    "port-call/v1/ports:ports" \
    "port-call/v1/vessel-details:vessel_details" \
    "port-call/v1/port-calls:port_calls" \
    "ais/v1/vessels:ais_vessels" \
    "ais/v1/locations:ais_locations"
  do
    local path="${ep%%:*}" name="${ep##*:}"
    log "  GET /api/$path"
    curl -fsSL --compressed \
      -H "Digitraffic-User: $UA" \
      -H "Accept: application/json" \
      -o "$archive/$name.json" \
      "$base/api/$path"
    cp "$archive/$name.json" "$live/$name.json"
    printf '     %s  %s bytes\n' "$name.json" "$(wc -c < "$live/$name.json")"
  done

  echo "$batch" > "$live/LATEST_BATCH"
  log "  live responses in $live, archived copy in $archive"
}

fetch_ais() {
  log "MarineCadastre AIS"
  mkdir -p "$SNAP/marinecadastre"
  cat <<'EOF'
  MANUAL STEP (deliberately).
  The annual bulk files are multi-gigabyte. Use the AccessAIS extractor to pull
  a bounded area and time window instead - a few days of one port approach is
  plenty to exercise the resolver:

      https://marinecadastre.gov/accessais/

  Save the resulting zipped CSV under data/snapshots/marinecadastre/ and load
  it into Postgres:

      docker compose up -d postgres
      ./scripts/load_ais.sh data/snapshots/marinecadastre/<file>.csv

  Background and full dataset docs:
      https://hub.marinecadastre.gov/datasets/vessel-traffic-ais-1
EOF
}

case "${1:-all}" in
  unlocode)    fetch_unlocode ;;
  wpi)         fetch_wpi ;;
  digitraffic) fetch_digitraffic ;;
  ais)         fetch_ais ;;
  all)         fetch_unlocode; fetch_digitraffic; fetch_wpi; fetch_ais ;;
  *)           echo "usage: $0 [all|unlocode|wpi|digitraffic|ais]" >&2; exit 2 ;;
esac

log "done. snapshots in $SNAP"
