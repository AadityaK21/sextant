// The wire types.
//
// WRITTEN FROM CAPTURED RESPONSES, NOT FROM THE C++ HEADERS
//
// Every shape here was taken from an actual response of a running server, not
// read off `src/api/json.cpp`. The difference matters: a serialiser and its
// header can disagree, and the type that describes the header would then be
// wrong in exactly the place nothing checks.
//
// WHY THERE IS NO CODE GENERATOR
//
// A generated client would need an OpenAPI document, which would be a third
// artifact to keep in step with the other two. With eleven routes, hand-written
// types that are exercised by the app are cheaper and no less honest. If this
// grew to fifty routes the answer would change.
//
// TIMESTAMPS ARE STRINGS. The server sends ISO 8601 rather than epoch
// milliseconds, because an epoch in milliseconds is past the exact-integer
// range of a JavaScript number and would silently lose precision here, in the
// browser, and nowhere else. See src/api/json.h.

export type ValueType =
  | 'null'
  | 'string'
  | 'int'
  | 'double'
  | 'bool'
  | 'timestamp'
  | 'string[]'

/** A property value as it arrives. `string` covers timestamps too. */
export type PropertyValue = string | number | boolean | string[] | null

export interface PropertyDef {
  id: number
  name: string
  type: ValueType
  title: boolean
  indexed: boolean
  unique_hint: boolean
  fuse: string
  enum?: string[]
}

export interface EntityTypeDef {
  id: number
  name: string
  description: string
  display: string
  properties: PropertyDef[]
}

export interface LinkTypeDef {
  id: number
  name: string
  from: string
  to: string
  cardinality: string
  inverse: string
  time_indexed: boolean
  /** Present only when time_indexed. */
  time_index?: string
}

export interface Ontology {
  version: number
  namespace: string
  types: EntityTypeDef[]
  links: LinkTypeDef[]
}

// --- query plan and cost ----------------------------------------------------

export type AccessPath =
  | 'POINT'
  | 'IDX'
  | 'IDX_RANGE'
  | 'IDX_PREFIX'
  | 'SCAN'
  | 'LINKOUT'
  | 'LINKIN'
  | 'TIDX'

export interface PlanStep {
  ordinal: number
  description: string
  access_path: AccessPath
  keyspace: string
  reason: string
  residuals: { property: string; op: string; value: PropertyValue }[]
  type?: string
  link?: string
  property?: string
  window?: { from: string; to: string }
}

export interface QueryPlan {
  steps: PlanStep[]
  warnings: string[]
  index_used: AccessPath
}

export interface QueryStats {
  keys_scanned: number
  blocks_read: number
  block_cache_hits: number
  bloom_rejections: number
  range_rejections: number
  sstables_probed: number
  memtable_hits: number
  entities_materialised: number
  index_used: AccessPath
  elapsed_us: number
  /** Present only when a hop hit its expansion bound. */
  truncated?: boolean
  truncated_at?: string
}

// --- entities ---------------------------------------------------------------

export interface ProvenanceSummary {
  source: string
  column: string
  rule: string
  confidence: number
  rejected: number
  cluster_size: number
  verified: boolean
  /** Which check `verified` reports: union properties pass by containment. */
  check: 'equality' | 'containment'
}

export interface Entity {
  id: string
  type: string
  display: string
  depth: number
  properties: Record<string, PropertyValue>
  /** Only on the single-entity route. */
  _provenance?: Record<string, ProvenanceSummary>
  _stats?: QueryStats
}

export interface ResultEdge {
  from: string
  to: string
  link: string
  reverse: boolean
}

export interface QueryResult {
  entities: Entity[]
  edges: ResultEdge[]
  count: number
  total_before_limit: number
  _plan: QueryPlan
  _stats: QueryStats
}

// --- lineage ----------------------------------------------------------------

export interface RejectedValue {
  source: number
  batch: number
  row: number
  column: string
  value: string
  reason: string
}

export interface Explanation {
  entity: string
  property: string
  stored_value: string
  found: boolean
  origin: { source: string; batch: number; row: number; column: string }
  raw_row_found: boolean
  raw_row: string
  raw_cell: string
  transforms: { id: number; name: string }[]
  chain_fingerprint: number
  chain_changed: boolean
  fusion: { rule: string; confidence: number }
  rejected: RejectedValue[]
  cluster_size: number
  merge_evidence: string[]
  replay: {
    value: string
    matches: boolean
    check: 'equality' | 'containment'
    error: string
  }
}

export interface RawRow {
  source: string
  batch: number
  row: number
  raw: string
}

// --- review -----------------------------------------------------------------

export interface ScoreFeature {
  name: string
  value: number
  weight: number
  contribution: number
  detail: string
}

export interface ReviewPair {
  pair_id: string
  score: number
  a?: { source: number; label: string }
  b?: { source: number; label: string }
  features: ScoreFeature[]
  explanation: string
  vetoed?: boolean
  veto_reason?: string
  decision?: string
  reviewer?: string
  /** Written before the structured payload existed. */
  legacy_format?: boolean
}

export interface ReviewQueue {
  pairs: ReviewPair[]
  count: number
  _stats: QueryStats
}

// --- stats ------------------------------------------------------------------

export interface Stats {
  entities: Record<string, number>
  total_entities: number
  source_records: number
  /** The fraction of records removed by merging, matching `sextant resolve`. */
  dedup_ratio: number
  engine: {
    writes: number
    reads: number
    sstables: number
    bytes_on_disk: number
    compactions: number
    trivial_moves: number
    keys_dropped: number
    write_stalls: number
    cache_hits: number
    cache_misses: number
    filter_rejections: number
    range_rejections: number
    levels: { level: number; files: number; bytes: number }[]
  }
}

// --- traverse request -------------------------------------------------------

export interface TraverseHop {
  link: string
  reverse?: boolean
  where?: Record<string, Record<string, string | number>>
  max_expand?: number
}

export interface TraverseRequest {
  start: { type: string; ids?: string[]; filter?: Record<string, unknown> }
  hops?: TraverseHop[]
  select?: string[]
  limit?: number
  include_path?: boolean
}

export interface ApiErrorBody {
  error: { code: string; message: string }
}
