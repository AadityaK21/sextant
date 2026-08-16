#!/usr/bin/env node
// Check the wire types against a running server.
//
// WHAT THIS CATCHES THAT tsc CANNOT
//
// TypeScript checks that the app uses `types.ts` consistently. It cannot check
// that `types.ts` describes what the C++ actually sends - those two are joined
// by an assertion (`as T`) and nothing verifies it. So the failure mode is a
// field renamed in src/api/json.cpp, a frontend that still compiles, and
// `undefined` appearing in the UI at runtime.
//
// This walks a live response and asserts every field the frontend relies on is
// present and of the declared kind. It is deliberately a runtime check against
// a real server rather than a generated client: the thing being verified is the
// agreement between two codebases, and only a real response can settle it.
//
//   node scripts/check-api-contract.mjs [base-url]
//
// Exits non-zero on the first disagreement, naming the route and the field.

const base = process.argv[2] ?? 'http://127.0.0.1:8080'

let failures = 0
let checks = 0

function fail(route, message) {
  failures++
  console.error(`  FAIL ${route}: ${message}`)
}

/** `spec` maps a field name to 'string' | 'number' | 'boolean' | 'array' | 'object'. */
function check(route, value, spec, path = '') {
  for (const [field, kind] of Object.entries(spec)) {
    checks++
    const here = path ? `${path}.${field}` : field
    if (!(field in value)) {
      fail(route, `missing field ${here}`)
      continue
    }
    const actual = value[field]
    const ok =
      kind === 'array'
        ? Array.isArray(actual)
        : kind === 'object'
          ? actual !== null && typeof actual === 'object' && !Array.isArray(actual)
          : typeof actual === kind
    if (!ok) {
      fail(
        route,
        `${here} should be ${kind}, got ${Array.isArray(actual) ? 'array' : typeof actual}`,
      )
    }
  }
}

async function get(path) {
  const response = await fetch(`${base}${path}`)
  if (!response.ok) throw new Error(`${path} -> HTTP ${response.status}`)
  return response.json()
}

const STATS = {
  keys_scanned: 'number',
  blocks_read: 'number',
  block_cache_hits: 'number',
  bloom_rejections: 'number',
  range_rejections: 'number',
  sstables_probed: 'number',
  memtable_hits: 'number',
  entities_materialised: 'number',
  index_used: 'string',
  elapsed_us: 'number',
}

async function main() {
  console.log(`checking ${base}\n`)

  // --- /api/ontology --------------------------------------------------------
  const ontology = await get('/api/ontology')
  check('/api/ontology', ontology, {
    version: 'number',
    namespace: 'string',
    types: 'array',
    links: 'array',
  })
  if (ontology.types?.[0]) {
    check('/api/ontology', ontology.types[0], {
      id: 'number',
      name: 'string',
      description: 'string',
      display: 'string',
      properties: 'array',
    })
    check('/api/ontology', ontology.types[0].properties[0], {
      id: 'number',
      name: 'string',
      type: 'string',
      title: 'boolean',
      indexed: 'boolean',
      unique_hint: 'boolean',
      fuse: 'string',
    })
  }
  const timed = ontology.links?.find((l) => l.time_indexed)
  if (!timed) {
    fail('/api/ontology', 'no link reports time_indexed; the UI cannot show TIDX')
  } else {
    check('/api/ontology', timed, {
      id: 'number',
      name: 'string',
      from: 'string',
      to: 'string',
      cardinality: 'string',
      inverse: 'string',
      time_indexed: 'boolean',
      time_index: 'string',
    })
  }

  // The whole generic-UI claim rests on this: a title property to search by.
  const withTitle = ontology.types.find((t) => t.properties.some((p) => p.title))
  if (!withTitle) fail('/api/ontology', 'no type declares a title property')

  // --- /api/entities --------------------------------------------------------
  const type = withTitle?.name ?? ontology.types[0].name
  const list = await get(`/api/entities?type=${type}&limit=5`)
  check('/api/entities', list, {
    entities: 'array',
    edges: 'array',
    count: 'number',
    total_before_limit: 'number',
    _plan: 'object',
    _stats: 'object',
  })
  check('/api/entities', list._stats, STATS)
  check('/api/entities', list._plan, {
    steps: 'array',
    warnings: 'array',
    index_used: 'string',
  })
  if (list._plan.steps[0]) {
    check('/api/entities', list._plan.steps[0], {
      ordinal: 'number',
      description: 'string',
      access_path: 'string',
      keyspace: 'string',
      reason: 'string',
      residuals: 'array',
    })
    if (!list._plan.steps[0].reason) {
      fail('/api/entities', 'a plan step has an empty reason, which is the field the UI renders')
    }
  }

  if (!list.entities.length) {
    fail('/api/entities', `no ${type} entities; run ingest and resolve first`)
    return
  }
  const first = list.entities[0]
  check('/api/entities', first, {
    id: 'string',
    type: 'string',
    display: 'string',
    depth: 'number',
    properties: 'object',
  })

  // --- /api/entities/{id} ---------------------------------------------------
  const entity = await get(`/api/entities/${first.id}`)
  check('/api/entities/{id}', entity, {
    id: 'string',
    type: 'string',
    display: 'string',
    properties: 'object',
    _provenance: 'object',
    _stats: 'object',
  })
  const provEntries = Object.entries(entity._provenance ?? {})
  if (!provEntries.length) {
    fail('/api/entities/{id}', 'no provenance summary; the detail view would show none')
  } else {
    const [propName, prov] = provEntries[0]
    check('/api/entities/{id}', prov, {
      source: 'string',
      column: 'string',
      rule: 'string',
      confidence: 'number',
      rejected: 'number',
      cluster_size: 'number',
      verified: 'boolean',
      check: 'string',
    })
    if (prov.check !== 'equality' && prov.check !== 'containment') {
      fail('/api/entities/{id}', `check should be equality or containment, got ${prov.check}`)
    }

    // --- /api/lineage/{id}/{prop} -------------------------------------------
    const lineage = await get(`/api/lineage/${first.id}/${propName}`)
    check('/api/lineage', lineage, {
      entity: 'string',
      property: 'string',
      stored_value: 'string',
      found: 'boolean',
      origin: 'object',
      raw_row_found: 'boolean',
      raw_row: 'string',
      raw_cell: 'string',
      transforms: 'array',
      chain_fingerprint: 'number',
      chain_changed: 'boolean',
      fusion: 'object',
      rejected: 'array',
      cluster_size: 'number',
      merge_evidence: 'array',
      replay: 'object',
    })
    check('/api/lineage', lineage.origin, {
      source: 'string',
      batch: 'number',
      row: 'number',
      column: 'string',
    })
    check('/api/lineage', lineage.replay, {
      value: 'string',
      matches: 'boolean',
      check: 'string',
      error: 'string',
    })
    check('/api/lineage', lineage.fusion, { rule: 'string', confidence: 'number' })
    if (lineage.transforms[0]) {
      check('/api/lineage', lineage.transforms[0], { id: 'number', name: 'string' })
    }
    if (lineage.rejected[0]) {
      check('/api/lineage', lineage.rejected[0], {
        source: 'number',
        batch: 'number',
        row: 'number',
        column: 'string',
        value: 'string',
        reason: 'string',
      })
    }

    // The claim the whole project rests on, asserted on live data.
    if (!lineage.replay.matches) {
      fail(
        '/api/lineage',
        `${propName} does not replay: stored "${lineage.stored_value}" vs replayed "${lineage.replay.value}"`,
      )
    }

    // --- /api/raw ----------------------------------------------------------
    const raw = await get(
      `/api/raw/${lineage.origin.source}/${lineage.origin.batch}/${lineage.origin.row}`,
    )
    check('/api/raw', raw, {
      source: 'string',
      batch: 'number',
      row: 'number',
      raw: 'string',
    })
    if (raw.raw !== lineage.raw_row) {
      fail('/api/raw', 'the raw row does not match the one lineage reported')
    }
  }

  // --- /api/entities/{id}/links --------------------------------------------
  const linkFromHere = ontology.links.find(
    (l) => l.to === entity.type && l.inverse,
  )
  if (linkFromHere) {
    const links = await get(
      `/api/entities/${entity.id}/links?type=${linkFromHere.inverse}&limit=3`,
    )
    check('/api/links', links, { entities: 'array', count: 'number', _stats: 'object' })
    check('/api/links', links._stats, STATS)
  }

  // --- /api/review ---------------------------------------------------------
  const review = await get('/api/review?limit=5')
  check('/api/review', review, { pairs: 'array', count: 'number', _stats: 'object' })
  if (review.pairs[0]) {
    check('/api/review', review.pairs[0], {
      pair_id: 'string',
      score: 'number',
      features: 'array',
      explanation: 'string',
    })
    if (review.pairs[0].legacy_format) {
      fail(
        '/api/review',
        'candidates are in the pre-structured format; re-run `sextant resolve`',
      )
    } else if (!review.pairs[0].features.length) {
      fail('/api/review', 'no features on a candidate; the review UI would show nothing')
    } else {
      check('/api/review', review.pairs[0].features[0], {
        name: 'string',
        value: 'number',
        weight: 'number',
        contribution: 'number',
        detail: 'string',
      })
    }
  }

  // --- /api/stats ----------------------------------------------------------
  const stats = await get('/api/stats')
  check('/api/stats', stats, {
    entities: 'object',
    total_entities: 'number',
    source_records: 'number',
    dedup_ratio: 'number',
    engine: 'object',
  })
  check('/api/stats', stats.engine, {
    writes: 'number',
    reads: 'number',
    sstables: 'number',
    bytes_on_disk: 'number',
    compactions: 'number',
    trivial_moves: 'number',
    keys_dropped: 'number',
    write_stalls: 'number',
    cache_hits: 'number',
    cache_misses: 'number',
    filter_rejections: 'number',
    range_rejections: 'number',
    levels: 'array',
  })

  // The CLI and the API used to report complementary numbers under this name.
  if (stats.dedup_ratio < 0 || stats.dedup_ratio >= 1) {
    fail('/api/stats', `dedup_ratio ${stats.dedup_ratio} is not a fraction removed`)
  }

  // --- /api/traverse -------------------------------------------------------
  if (timed) {
    const anchorType = timed.to
    const anchors = await get(`/api/entities?type=${anchorType}&limit=1`)
    if (anchors.entities[0]) {
      const response = await fetch(`${base}/api/traverse`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          start: { type: anchorType, ids: [anchors.entities[0].id] },
          hops: [{ link: timed.inverse }],
          limit: 10,
        }),
      })
      if (!response.ok) {
        fail('/api/traverse', `HTTP ${response.status}`)
      } else {
        const result = await response.json()
        check('/api/traverse', result, {
          entities: 'array',
          edges: 'array',
          count: 'number',
          _plan: 'object',
          _stats: 'object',
        })
        check('/api/traverse', result._stats, STATS)
        if (result.edges[0]) {
          check('/api/traverse', result.edges[0], {
            from: 'string',
            to: 'string',
            link: 'string',
            reverse: 'boolean',
          })
        }
      }
    }
  }

  console.log(
    `\n${checks} field checks, ${failures} disagreement${failures === 1 ? '' : 's'}`,
  )
  if (failures > 0) {
    console.error('\nweb/src/api/types.ts does not match what the server sends.')
    process.exit(1)
  }
  console.log('web/src/api/types.ts matches the running server')
}

main().catch((error) => {
  console.error(`\ncould not complete: ${error.message}`)
  console.error(`Is \`sextant serve\` running on ${base}?`)
  process.exit(1)
})
