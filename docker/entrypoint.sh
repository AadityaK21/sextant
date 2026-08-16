#!/bin/sh
# Ingest, resolve and serve - but only ingest and resolve if there is nothing
# there yet.
#
# WHY THE GUARD
#
# The database is a volume, so a container restart finds the previous run's
# work. Re-resolving every start would add a minute to every restart for no
# benefit, and on a large corpus it would be much worse.
#
# The guard is a marker file rather than "does the directory exist", because the
# directory exists the moment Docker creates the volume, and the interesting
# question is whether a resolve ever COMPLETED. A container killed midway leaves
# no marker and the next start redoes the work, which is the right way round.
#
# SEXTANT_FORCE_RESOLVE=1 redoes it anyway. Resolution clears its own derived
# keyspaces first, so that is safe rather than additive.

set -e

DB="${SEXTANT_DB:-/app/sextant-db}"
PORT="${SEXTANT_PORT:-8080}"
MARKER="$DB/.resolved"

if [ ! -f "$MARKER" ] || [ "${SEXTANT_FORCE_RESOLVE:-0}" = "1" ]; then
  echo "==> first run: ingesting the committed corpus"
  sextant schema --schema /app/schema

  for source in wpi unlocode digitraffic digitraffic_ais; do
    sextant ingest --source "$source" --db "$DB" --schema /app/schema
  done

  echo "==> resolving"
  sextant block   --db "$DB" --schema /app/schema --eval /app/eval
  sextant eval    --db "$DB" --schema /app/schema --eval /app/eval
  sextant resolve --db "$DB" --schema /app/schema --eval /app/eval

  echo "==> verifying lineage"
  # This is the headline result, and it runs on every fresh database rather than
  # being a number quoted from a README. If the round trip fails, the container
  # fails to start - which is the correct behaviour for a system whose whole
  # claim is that its lineage is verified.
  sextant explain --db "$DB" --schema /app/schema

  touch "$MARKER"
  echo "==> ready"
else
  echo "==> existing database found; skipping ingest and resolve"
  echo "    (set SEXTANT_FORCE_RESOLVE=1 to redo it)"
fi

exec sextant serve \
  --db "$DB" \
  --schema /app/schema \
  --data-root /app \
  --port "$PORT" \
  --static /app/web/dist \
  --no-cors
