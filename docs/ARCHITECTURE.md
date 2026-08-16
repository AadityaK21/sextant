# Sextant - Architecture & Design

*A mini-Foundry: ontology, entity resolution, and cell-level lineage over an LSM storage engine.*

**Language:** C++20 · **Domain:** Maritime (vessels, ports, voyages) · **Storage:** custom LSM tree

---

## 0. The one-sentence pitch

> Sextant ingests maritime data from three heterogeneous sources, maps it onto a declarative ontology, resolves duplicate real-world entities across sources, and records - for every single property value - the exact source row and transform chain that produced it. It runs on a log-structured merge-tree storage engine written from scratch.

*A sextant fixes your position by combining several independent observations. That is literally entity resolution.*

---

## 1. Why this design holds together

Most student projects that attempt this become seven disconnected demos. The thing that makes Sextant one system is a single idea:

> **The LSM engine gives you an ordered key-value store with prefix scan. Every layer above it is a key encoding.**

Entities, links, provenance, secondary indexes, blocking indexes, raw records - all of it is bytes in one keyspace, laid out so the access pattern you need is a *sequential range scan*. Graph traversal isn't a separate subsystem; it's a prefix scan. Time-windowed queries aren't a filter; they're a range scan over a big-endian timestamp suffix.

Everything below is an elaboration of this one idea.

---

## 2. System diagram

```
┌───────────────────────────────────────────────────────────────┐
│  web/ - React + Vite + TypeScript                           │
│  Type browser · Entity detail · Link graph · Lineage drawer   │
│  Review queue                                                 │
└──────────────────────────┬────────────────────────────────────┘
                           │  HTTP / JSON
┌──────────────────────────▼────────────────────────────────────┐
│  src/api/ - cpp-httplib                                     │
│  /ontology /entities /links /traverse /lineage /raw /review    │
├───────────────────────────────────────────────────────────────┤
│  src/query/ - planner + executor                            │
│  index selection · frontier expansion · predicate pushdown    │
├───────────────────────────────────────────────────────────────┤
│  src/ontology/ - type registry, validation, link cardinality│
├───────────────────────────────────────────────────────────────┤
│  src/resolve/ - normalize → block → score → cluster → fuse  │
│         ▲                                                     │
│  src/lineage/ - provenance emitted at every fusion decision  │
├───────────────────────────────────────────────────────────────┤
│  src/transform/ - registry of named, pure, versioned fns    │
├───────────────────────────────────────────────────────────────┤
│  src/connectors/ - CSV · HTTP/JSON · Postgres               │
├───────────────────────────────────────────────────────────────┤
│  src/codec/ - key encoding (the glue)                       │
├───────────────────────────────────────────────────────────────┤
│  src/lsm/ - WAL · MemTable · SSTable · Bloom · Compaction   │
│              VersionSet · Snapshots · Block cache             │
└───────────────────────────────────────────────────────────────┘
```

---

## 3. Data sources (all free, all genuinely messy)

You need three *structurally different* connectors. These also give you real, unforced duplication.

### Source A - CSV bulk dump

**NGA World Port Index** (~3,700 ports worldwide) - port names in `ALL CAPS`, an `Alternate Port Name` column, patchy UN/LOCODE coverage, decimal lat/lon.

**UN/LOCODE code list** from UNECE (~110,000 locations) - the canonical code authority. Names *with* diacritics plus a `NameWoDiacritics` column, coordinates in degree-minute format (`5155N 00430E`), a `Function` bitfield telling you whether a location is a seaport, airport, rail terminal, etc.

Downloads: [UNECE UN/LOCODE](https://unece.org/trade/cefact/UNLOCODE-Download) (last updated 27 May 2026; releases cut off 31 March and 30 September). A pre-parsed mirror lives at [datasets/un-locode](https://github.com/datasets/un-locode) if the official ZIP fights you.

### Source B - JSON HTTP API

**Fintraffic Digitraffic marine APIs** - free, no API key, open licence. Four endpoints matter:

| Endpoint | Gives you |
|---|---|
| `https://meri.digitraffic.fi/api/port-call/v1/port-calls` | **Voyages** - vessel arriving/departing a port with timestamps |
| `https://meri.digitraffic.fi/api/port-call/v1/ports` | Port metadata (Finnish LOCODEs + free-text names) |
| `https://meri.digitraffic.fi/api/port-call/v1/vessel-details` | Vessel registry - IMO, MMSI, name, tonnage |
| `https://meri.digitraffic.fi/api/ais/v1/locations` | Live AIS positions |
| `https://meri.digitraffic.fi/api/ais/v1/vessels` | AIS-derived vessel metadata |

Send a `Digitraffic-User: <yourname>/sextant` header - it's requested by their terms and some endpoints behave badly without it.

The port-calls endpoint is the gift here: it *is* your `Voyage` entity, with `departs_from` and `arrives_at` links already implied.

### Source C - Postgres

Load a slice of **MarineCadastre.gov AIS** (NOAA + BOEM + USCG) into Postgres and read it through `libpqxx`. Data since 2015 is distributed as Zstd-compressed daily CSVs; use the AccessAIS extractor to pull a bounded area/timeframe rather than the multi-GB annual files.

This is not cheating. In the real world your third source *is* someone's operational warehouse. It gives you: a different vessel-type taxonomy (USCG codes vs. AIS type codes), MMSI-keyed records with no IMO, and US port naming conventions. All three are excellent duplication generators.

> **Reproducibility discipline:** snapshot every API response and CSV to `data/snapshots/<source>/<batch_id>/` on first fetch, and replay from disk during development. Your build must not depend on a live network. Mention this in your README - it reads as professional.

---

## 4. Storage: the LSM engine

Build this first, freeze it fast, come back to polish. It is the highest-risk component.

### 4.1 Components

**WAL (write-ahead log).** Append-only. Record framing:

```
[ crc32c : 4 ][ length : 4 ][ type : 1 ][ payload : length ]
```

`type` handles records that span block boundaries (`FULL`/`FIRST`/`MIDDLE`/`LAST`) - this is the LevelDB log format and it means a torn tail is detectable, not fatal. Group-commit: batch writers share one `fsync`.

**MemTable - skiplist.** Not a red-black tree. Reasons you should be able to give: no rebalancing, lock-free concurrent readers with a single writer (readers only follow `atomic` forward pointers, and a node is fully constructed before it's linked in), and probabilistic *O(log n)* with far simpler code.

**Internal key encoding** - the trick that buys you MVCC, deletes, and snapshots in one move:

```
internal_key = user_key || sequence(7 bytes, big-endian) || value_type(1 byte)
```

The comparator sorts `user_key` ascending, then `sequence` **descending**. So a forward scan sees the newest version of a key first - the read path just takes the first hit and skips the rest. `value_type` is `kTypeValue` or `kTypeDeletion`; a delete is a *tombstone*, not a removal.

**SSTable format:**

```
┌──────────────┬──────────────┬─────┬──────────────┐
│ data block 0 │ data block 1 │ ... │ data block N │
├──────────────┴──────────────┴─────┴──────────────┤
│ filter block   (bloom filter per data block)     │
├──────────────────────────────────────────────────┤
│ index block    (last key of block → BlockHandle) │
├──────────────────────────────────────────────────┤
│ footer  (index handle, filter handle, magic)     │
└──────────────────────────────────────────────────┘
```

Data blocks use **prefix-compressed keys with restart points** every 16 entries:

```
[ shared_prefix_len ][ unshared_len ][ value_len ][ unshared_key_bytes ][ value_bytes ]
...
[ restart_offset_0 ][ restart_offset_1 ]...[ num_restarts ]
```

Restart points let you binary-search *within* a block. Prefix compression matters a lot for your workload because your keys are structured and share long prefixes by construction (`LINKOUT | src_eid | link_type | …`). Being able to say "my key design makes my block compression effective" is a strong, specific observation.

**Bloom filters.** 10 bits per key → false-positive rate ≈ 1%. Optimal hash count `k = (m/n)·ln2 ≈ 7`. Don't compute 7 hashes; use Kirsch-Mitzenmacher double hashing: `g_i(x) = h1(x) + i·h2(x)`, from one 64-bit hash split in half. The false-positive formula, which you should have memorised:

```
p ≈ (1 − e^(−kn/m))^k
```

**Block cache.** Sharded LRU, 16 shards keyed by `hash(file_id, block_offset) & 15`, each with its own mutex. Sharding is the whole point - a single global mutex on the cache is the classic bottleneck.

**VersionSet + MANIFEST.** The set of live SSTables per level is *state*. Persist it as an edit log (`VersionEdit`: files added, files deleted, new log number, last sequence). Recovery = replay MANIFEST to rebuild the file set, then replay the WAL from the last persisted sequence. A `Version` is refcounted and immutable, so an open iterator pins the files it needs against concurrent compaction.

**Compaction - leveled.** L0 files overlap (they're just flushed memtables). L1+ are non-overlapping and globally sorted within a level. Target size ratio 10× per level. Pick the compaction with the highest score (`level_bytes / target_bytes`, or file *count* for L0). Write stalls when L0 file count crosses a threshold - this is the well-known LSM pathology and you should be able to describe it.

**Snapshots.** A snapshot is just a sequence number. Reads skip entries with `seq > snapshot_seq`. This is nearly free and it's *load-bearing* for you: a multi-hop graph traversal touches thousands of keys, and without a snapshot a concurrent ingest could show you a half-merged entity. It also gives you time travel over the ontology for free - "show me this entity as of last Tuesday's batch" is a snapshot read.

**Iterators.** A merging iterator over: memtable, immutable memtable, each L0 file individually (they overlap), and one *concatenating* iterator per level ≥1 (they don't overlap, so you can binary-search the file list then walk). K-way merge with a small heap.

### 4.2 Read path

```
Get(key, snapshot):
  memtable        → hit? return
  imm memtable    → hit? return
  L0 files (newest first, only those whose [smallest,largest] contains key):
      bloom.MayContain? no → skip file entirely     ← the whole point of bloom filters
      index block binary search → block handle
      block cache lookup / read + decompress
      binary search restarts → linear scan to key
  L1..LN: binary search the file list (non-overlapping), same as above
```

### 4.3 Write path

```
Put(batch):
  assign sequence range
  WAL append (one fsync per group commit)
  memtable insert
  if memtable.size > threshold:
      switch to immutable, signal background thread
      background: flush imm → L0 SSTable, then maybe compact
```

### 4.4 Public interface

Keep it deliberately small - this is what the rest of the system codes against.

```cpp
namespace lsm {

class DB {
public:
  static Status Open(const Options&, std::string_view path, std::unique_ptr<DB>*);

  Status Put(const WriteOptions&, Slice key, Slice value);
  Status Delete(const WriteOptions&, Slice key);
  Status Write(const WriteOptions&, WriteBatch*);          // atomic multi-key
  Status Get(const ReadOptions&, Slice key, std::string* out);

  std::unique_ptr<Iterator> NewIterator(const ReadOptions&);  // supports Seek(prefix)

  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot*);

  Stats GetStats() const;   // level sizes, compaction bytes, cache hit rate, stalls
};

}  // namespace lsm
```

`WriteBatch` matters more than it looks: **an entity merge must be atomic**. Writing the entity, its links, its provenance, and its cross-reference entries as one batch is what keeps the ontology internally consistent.

---

## 5. Key encoding - the glue layer

One byte of keyspace prefix, then a fixed layout. All integers big-endian so byte order equals numeric order.

| Prefix | Name | Key layout | Value |
|---|---|---|---|
| `0x01` | `RAW` | `src(4) ‖ batch(8) ‖ row(8)` | original record bytes, verbatim |
| `0x02` | `SRCREC` | `src(4) ‖ natural_key_hash(8)` | normalized `SourceRecord` |
| `0x03` | `ENTITY` | `type(2) ‖ eid(16)` | `ResolvedEntity` |
| `0x04` | `LINKOUT` | `src_eid(16) ‖ link_type(2) ‖ dst_eid(16)` | edge payload |
| `0x05` | `LINKIN` | `dst_eid(16) ‖ link_type(2) ‖ src_eid(16)` | edge payload (mirror) |
| `0x06` | `PROV` | `eid(16) ‖ prop(2) ‖ version(8)` | `Provenance` |
| `0x07` | `XREF` | `src(4) ‖ src_pk_hash(8)` | `eid(16)` - where did this row land? |
| `0x08` | `BLOCK` | `block_key_hash(8) ‖ src(4) ‖ rec(8)` | ∅ (blocking index) |
| `0x09` | `IDX` | `type(2) ‖ prop(2) ‖ value ‖ 0x00 ‖ eid(16)` | ∅ (secondary index) |
| `0x0A` | `TIDX` | `link_type(2) ‖ anchor_eid(16) ‖ ts_be(8) ‖ eid(16)` | ∅ (time-ordered traversal) |
| `0x0B` | `CAND` | `score_inv(4) ‖ pair_hash(8)` | ER candidate pair awaiting review |
| `0x0C` | `INGEST` | `src(4) ‖ batch(8)` | `BatchManifest` - what one ingest run loaded |

**Entity IDs are ULIDs** (16 bytes: 48-bit timestamp + 80 bits random). Lexicographically sortable, time-ordered, no coordination needed. Better than UUIDv4 here because sorted IDs mean entities created together sit together on disk.

### Why this specific layout wins

- **`LINKOUT` prefix scan** `0x04 ‖ eid ‖ link_type` returns every outgoing edge of one type as a contiguous range. That is your entire graph traversal engine, and it's *one sequential read*.
- **`LINKIN` is a deliberate denormalisation.** You store every edge twice. Cost: 2× link storage. Benefit: reverse traversal ("which voyages arrived *at* this port") is as cheap as forward. Be ready to defend this trade - it's a real one.
- **`TIDX` is why the quarter-query is fast.** `0x0A ‖ arrives_at ‖ rotterdam_eid ‖ ts` with a big-endian timestamp means "all voyages into Rotterdam between April and July" is a *range scan over a contiguous byte range*, not a scan-and-filter.
- **`CAND` uses inverted score** (`UINT32_MAX − score`) as the key prefix, so scanning the review queue naturally returns the most-uncertain pairs first.
- **`PROV` versioned by LSM sequence number** means provenance history is append-only and ordered for free.
- **`INGEST` exists because `RAW` is append-only.** That immutability is what makes lineage permanent, and it is also why the archive has no opinion about duplication: run the same ingest twice and you get two copies of every row with no way to tell afterwards. A manifest per batch - including a fingerprint of the input bytes - makes a repeated ingest of an unchanged file a no-op, while a *changed* file still gets a new batch and never overwrites the old one.

### `RAW` and `SRCREC` behave in opposite ways, on purpose

`RAW` is keyed by `(source, batch, row)` and is never overwritten. `SRCREC` is keyed by `(source, natural_key_hash)` with **no batch in the key**, so a new batch replaces the normalised view of each row.

That asymmetry is the whole design:

> `RAW` answers *"what did the source say on the 3rd of April"*.
> `SRCREC` answers *"what does the source say now"*.

Entity resolution wants the second - it should compare current records, not every historical version of them. Lineage wants the first, permanently. Conflating them would cost one or the other.

---

## 6. The ontology layer

Declarative. YAML. The whole point is that adding an entity type requires **zero code changes** - including in the frontend, which reads the schema from `/api/ontology` and renders generically.

### `schema/ontology.yaml`

```yaml
version: 1
namespace: maritime

entity_types:
  Port:
    properties:
      name:        { type: string,  title: true, fuse: most_trusted }
      locode:      { type: string,  indexed: true, unique_hint: true, fuse: most_trusted }
      alt_names:   { type: string[], fuse: union }
      country:     { type: string,  indexed: true, fuse: most_frequent }
      lat:         { type: double,  fuse: numeric_median }
      lon:         { type: double,  fuse: numeric_median }
      harbor_size: { type: enum, values: [V, L, M, S], fuse: most_trusted }
    display: "{name} ({locode})"

  Vessel:
    properties:
      name:          { type: string, title: true, fuse: most_recent }
      imo:           { type: string, indexed: true, unique_hint: true, validate: imo_checksum }
      mmsi:          { type: string, indexed: true }
      call_sign:     { type: string }
      flag:          { type: string, indexed: true, fuse: most_recent }
      ship_type:     { type: int }
      gross_tonnage: { type: int, fuse: most_trusted }
    display: "{name} [IMO {imo}]"

  Voyage:
    properties:
      voyage_ref:  { type: string, title: true }
      departed_at: { type: timestamp, indexed: true }
      arrived_at:  { type: timestamp, indexed: true }
      cargo:       { type: string }

link_types:
  departs_from: { from: Voyage, to: Port,   cardinality: many_to_one, inverse: departures, time_index: departed_at }
  arrives_at:   { from: Voyage, to: Port,   cardinality: many_to_one, inverse: arrivals,   time_index: arrived_at }
  operated_by:  { from: Voyage, to: Vessel, cardinality: many_to_one, inverse: voyages }
```

Note `time_index: arrived_at` - that declaration is what causes the `TIDX` keyspace to be populated. Declarative indexing.

### `schema/mappings/wpi.yaml`

```yaml
source:
  id: wpi
  name: NGA World Port Index
  connector: csv
  uri: data/snapshots/wpi/UpdatedPub150.csv
  natural_key: ["World Port Index Number"]
  trust: 0.80                     # source-level weight, used by fuse: most_trusted

mappings:
  - target_type: Port
    properties:
      name:        { from: "Main Port Name",      transform: [trim, collapse_ws, title_case] }
      alt_names:   { from: "Alternate Port Name", transform: [trim, split_semicolon] }
      locode:      { from: "UN/LOCODE",           transform: [trim, upper, validate_locode] }
      country:     { from: "Country Code",        transform: [trim, upper] }
      lat:         { from: "Latitude",            transform: [to_double] }
      lon:         { from: "Longitude",           transform: [to_double] }
      harbor_size: { from: "Harbor Size",         transform: [trim, upper, first_char] }
```

### The transform registry - non-negotiable design rule

```cpp
// Every transform is a pure function with a stable, versioned identifier.
using TransformFn = std::function<TValue(const TValue&, const TransformCtx&)>;

struct Transform {
  TransformId id;          // e.g. 0x0107
  std::string name;        // "title_case"
  uint16_t    version;     // bump when behaviour changes - old lineage stays valid
  TransformFn fn;
};
```

Lineage stores `std::vector<TransformId>`, not names, not code. That is what makes lineage **replayable**: given the raw value and the transform chain, you can recompute the output and assert it matches. Section 9 turns this into a test that proves your lineage isn't decorative.

---

## 7. Entity resolution

This is the intellectual centre of the project. Five stages.

```
   SourceRecords
        │
   ┌────▼─────┐
   │ Normalize│   Unicode-fold, strip noise tokens, validate identifiers
   └────┬─────┘
   ┌────▼─────┐
   │  Block   │   O(n²) → O(n·b).  Emit candidate pairs.
   └────┬─────┘
   ┌────▼─────┐
   │  Score   │   Weighted feature vector → real-valued score
   └────┬─────┘
   ┌────▼─────┐
   │ Cluster  │   Veto-constrained union-find
   └────┬─────┘
   ┌────▼─────┐
   │   Fuse   │   One value per property + provenance for every decision
   └────┬─────┘
   ResolvedEntities
```

### 7.1 Normalization

Port names:

```
"Port of Rotterdam"  ─┐
"ROTTERDAM"          ─┼─→  NFKD → strip diacritics → uppercase
"Rotterdam, NL"      ─┤     → drop noise tokens {PORT OF, HARBOUR, HARBOR,
"NLRTM"              ─┘        TERMINAL, ANCHORAGE, ROADS, PT, ST→SAINT}
                            → strip punctuation → collapse whitespace
                     →  "ROTTERDAM"
```

Identifiers:

- **LOCODE** - uppercase, strip spaces. 5 chars: 2 country + 3 location.
- **IMO number** - 7 digits, and it has a **check digit**. Multiply digits 1-6 by weights 7,6,5,4,3,2; sum; the last digit of the sum must equal digit 7. Validating this catches transcription errors *and* gives you a nice small piece of domain code.
- **MMSI** - 9 digits. The first three are the **MID** (Maritime Identification Digits) and encode the flag state. Cross-checking MID against the declared flag is a free extra feature.

### 7.2 Blocking - why, and how to prove it worked

Naive comparison is *O(n²)*: 3,700 WPI ports × 110,000 UN/LOCODE rows ≈ **400 million pairs**. Blocking cuts this to a few hundred thousand.

Use **multiple blocking keys** with disjunctive semantics - two records are candidates if they share *any* block. Multiple weak keys beat one strong key because each one covers a different failure mode.

| Key | Definition | Catches |
|---|---|---|
| `bk_locode` | exact normalized LOCODE | trivial exact matches |
| `bk_country_prefix` | `country ‖ first 4 chars of normalized name` | typos after char 4 |
| `bk_geo` | geohash(lat, lon, precision 4) + its 8 neighbours | name mismatches, exonyms |
| `bk_phonetic` | Double Metaphone of normalized name | spelling variants |
| `bk_sorted_tokens` | first 2 name tokens, sorted | word-order differences |

**Block purging:** skip any block with more than ~200 members. A block that large isn't discriminative (it's usually `country=US`) and it will dominate your runtime.

**Report two metrics - this is what a serious ER writeup looks like:**

- **Reduction Ratio** `RR = 1 − (candidate pairs / all pairs)` - how much work you saved. Target > 0.999.
- **Pair Completeness** `PC = (true matches surviving blocking) / (all true matches)` - how much recall you sacrificed. Target > 0.98.

The tension between those two is the entire art of blocking. Put both numbers in your README.

### 7.3 Scoring

A weighted linear feature vector - interpretable, debuggable, and tunable against a labeled set. No black box.

**Port pair features:**

| Feature | Computation | Weight |
|---|---|---|
| `locode_exact` | both present and equal | **+6.0** |
| `locode_conflict` | both present and different | **−8.0** (veto) |
| `name_jaro_winkler` | JW on normalized names | ×3.0 |
| `name_token_jaccard` | token set intersection / union | ×1.5 |
| `geo_proximity` | `exp(−haversine_km / 15)` | ×4.0 |
| `geo_conflict` | distance > 100 km | **−6.0** (veto) |
| `country_match` | ISO-2 equal | +1.0, else −4.0 |

**Vessel pair features** - note the asymmetry, this is the domain-knowledge answer:

| Feature | Weight | Why |
|---|---|---|
| `imo_exact` (both checksum-valid) | **+10.0** | IMO is assigned to a *hull* and never changes, even through renaming, reflagging, or resale. Near-decisive. |
| `imo_conflict` | **−20.0** | Different hulls. Hard veto, overrides everything. |
| `mmsi_exact` | +4.0 | Strong but *not* decisive - MMSI is tied to the radio licence and is **reassigned when a vessel changes flag**. Two different ships can hold the same MMSI at different times. |
| `callsign_exact` | +3.0 | Same reassignment caveat as MMSI. |
| `name_jaro_winkler` | ×2.0 | Vessels get renamed constantly. Weak signal. |
| `mid_flag_agreement` | +1.0 | MMSI prefix consistent with declared flag. |

Thresholds:

```
score > 5.0    →  MATCH
2.0 … 5.0      →  REVIEW   ← write to the CAND keyspace, surface in the UI
score < 2.0    →  NO MATCH
```

**Do not skip the review band.** A system that admits uncertainty and routes it to a human is what a production data platform actually does. It's also the cheapest way to look mature.

> **Where this sits in the literature.** The weighted-sum scorer is a hand-tuned approximation of the **Fellegi-Sunter** probabilistic record linkage model (1969), in which each feature contributes `log(m_i/u_i)` - the log-likelihood ratio of agreement given a match against agreement given a non-match. Fitting `m` and `u` from the labeled set with EM would make that correspondence exact rather than approximate, and is the natural next step if the golden set grows enough to support it.

### 7.4 Clustering - and the trap

You have scored pairs. You need *clusters*. The obvious move is union-find on all MATCH edges. The obvious move is wrong.

**The transitive chaining problem:**

```
"Rotterdam"        ~  "Rotterdam Botlek"     score 5.4  ✓
"Rotterdam Botlek" ~  "Botlek"               score 5.8  ✓
"Rotterdam"        ~  "Botlek"               score 1.1  ✗   ← and geo says 12km apart
```

Naive union-find merges all three into one entity. You've just silently destroyed a distinct port.

**Fix - veto-constrained union-find.** Process MATCH edges in *descending score order*. Before each union, check whether any pair `(a ∈ cluster_A, b ∈ cluster_B)` carries a **veto** (a hard-negative feature like `locode_conflict` or `geo_conflict`). If so, refuse the merge and record it as a *blocked merge* - which is itself great lineage.

Keep a per-cluster veto set so the check is cheap. This is a lightweight approximation of **correlation clustering**, which is NP-hard in general.

**Implement both, measure both, put the table in your README.** "Union-find gave F1 0.86 with 14 chained over-merges; veto-constrained gave F1 0.94 with 1" is a sentence that gets you hired.

### 7.5 Fusion

A cluster holds N source records. You must emit **one** entity with **one** value per property. The `fuse:` key in the ontology declares the rule:

| Rule | Behaviour | Good for |
|---|---|---|
| `most_trusted` | highest source `trust` wins | `locode` - UN/LOCODE is the authority |
| `most_recent` | highest source timestamp | `name`, `flag` - vessels get renamed and reflagged |
| `most_frequent` | plurality vote across sources | `country` |
| `numeric_median` | median of numeric values | `lat`/`lon` - robust to one bad source |
| `longest` | longest non-empty string | free-text descriptions |
| `union` | set union | `alt_names` |

**Every fusion decision writes a provenance record that names the losers.** That is the difference between a lineage panel and a *good* lineage panel.

---

## 8. Lineage

Two granularities, both needed.

**Coarse (dataset-level):** source → batch → transform job → keyspace. This is the DAG you draw on a slide.

**Fine (cell-level):** every property value on every entity. This is the one that's hard and the one they asked for.

```cpp
struct SourceRef {
  uint32_t    source_id;
  uint64_t    batch_id;
  uint64_t    row_seq;
  std::string column;      // the exact source column
};

struct RejectedCandidate {
  SourceRef              origin;
  std::string            value;
  std::string            reason;      // "lower source trust (0.60 < 0.95)"
};

struct MergeEvidence {
  std::string feature;                // "locode_exact"
  double      contribution;           // +6.0
  std::string detail;                 // "both = NLRTM"
};

struct Provenance {
  EntityId    entity_id;
  PropertyId  property_id;
  uint64_t    version;                // LSM sequence number at write time

  // the winner
  SourceRef                origin;
  std::vector<TransformId> transform_chain;
  std::string              raw_value;      // literal bytes from the source
  std::string              emitted_value;  // after transforms

  // the decision
  FusionRule                     rule;
  double                         confidence;
  std::vector<RejectedCandidate> rejected;

  // how this entity came to exist
  ClusterId                  cluster_id;
  std::vector<MergeEvidence> merge_evidence;
  std::vector<SourceRef>     cluster_members;
};
```

**"Why does this say X?" resolves in two point lookups**, both *O(log n)*:

1. `PROV ‖ eid ‖ prop` → the provenance record (transform chain, rule, rejected alternatives, merge evidence)
2. `RAW ‖ source_id ‖ batch_id ‖ row_seq` → the verbatim original row, rendered as a table with the source column highlighted

**Immutability is what makes this true rather than aspirational.** Batches are append-only; re-ingesting a source creates a *new* `batch_id` and never overwrites `RAW`. Combined with LSM snapshots, you get time travel over the whole ontology for free.

---

## 9. The test that makes this project credible

Everything above is a design. This is the proof.

> **Lineage round-trip property test.** For *every* property of *every* entity: read the provenance, fetch the raw row it names, re-apply the transform chain by ID, and assert the result equals the stored value.

```cpp
TEST(Lineage, RoundTripsForEveryProperty) {
  for (const auto& entity : db.ScanEntities()) {
    for (const auto& [prop_id, value] : entity.properties) {
      auto prov = db.GetProvenance(entity.id, prop_id);
      auto raw  = db.GetRawRecord(prov.origin);          // RAW keyspace
      auto cell = raw.Column(prov.origin.column);
      auto replayed = transforms.Apply(prov.transform_chain, cell);
      EXPECT_EQ(replayed, value)
        << "Lineage broken for " << entity.id << "." << prop_id;
    }
  }
}
```

Passing this over the full dataset is what separates lineage from decoration: it makes it a **verified invariant** rather than a record nobody has checked.

### The rest of the test suite

1. **LSM differential test** - run 10⁶ random ops (put/delete/get/scan/snapshot) against your DB and a reference `std::map`, assert identical results at every step.
2. **Crash recovery** - `kill -9` mid-write in a loop; assert every acknowledged write survives and no torn record is accepted (CRC catches the tail).
3. **Compaction invariants** - after every compaction: no key appears in two files at the same level ≥ 1; level sizes within bounds; a full scan still matches the reference map.
4. **ER golden set** - hand-label ~300 port pairs and ~200 vessel pairs. Report **precision / recall / F1**, hold out 20% for validation. *Accuracy is a meaningless metric here* - 99.99% of pairs are non-matches, so a model that says "never match" scores 99.99%. Know why you report F1.
5. **Benchmarks** - writes/sec, point-read p50/p99, scan throughput, space amplification, compaction bytes written. Chart them.

---

## 10. Query API

```
GET  /api/ontology                      → the schema; drives the UI generically
GET  /api/entities?type=Port&q=rott     → search via IDX keyspace
GET  /api/entities/{id}                 → properties + provenance summary
GET  /api/entities/{id}/links?type=arrivals
POST /api/traverse                      → multi-hop
GET  /api/lineage/{id}/{prop}           → full provenance chain
GET  /api/raw/{src}/{batch}/{row}       → verbatim original record
GET  /api/review?limit=50               → uncertain ER pairs, most-uncertain first
POST /api/review/{pair_id}              → accept / reject; persists as a decision edge
GET  /api/stats                         → entity counts, dedup ratio, LSM stats
```

**Traverse request:**

```json
{
  "start": { "type": "Port", "filter": { "locode": "NLRTM" } },
  "hops": [
    { "link": "arrivals",
      "where": { "arrived_at": { "gte": "2026-04-01", "lt": "2026-07-01" } } },
    { "link": "operated_by" }
  ],
  "select": ["Vessel.name", "Vessel.imo", "Voyage.arrived_at"],
  "limit": 200
}
```

**Execution plan:**

1. Resolve the start set via `IDX ‖ Port ‖ locode ‖ "NLRTM"` → one entity ID.
2. Hop 1 has a time predicate on a `time_index`ed link → planner chooses `TIDX` over `LINKIN`. Range scan `0x0A ‖ arrives_at ‖ port_eid ‖ ts_2026_04_01` to `…ts_2026_07_01`. **One contiguous sequential scan.** No filtering.
3. Hop 2 → `LINKOUT ‖ voyage_eid ‖ operated_by` prefix scan per voyage.
4. Project, dedupe, limit.

Return a `_stats` block on every response:

```json
"_stats": { "keys_scanned": 1284, "blocks_read": 19, "bloom_rejections": 7,
            "index_used": "TIDX", "elapsed_us": 3105 }
```

Exposing the cost accounting is what makes a performance regression visible at the moment it happens rather than months later in a profiler. It is also the difference between claiming a query is a range scan and being able to show it.

---

## 11. Frontend

Vite + React + TypeScript. Keep it thin - the backend is the project.

| View | Notes |
|---|---|
| **Type browser** | Rendered entirely from `/api/ontology`. Add an entity type to the YAML → it appears in the UI with zero frontend changes. |
| **Entity detail** | Property table; each value has an ⓘ affordance opening the lineage drawer. |
| **Lineage drawer** | Raw row as a table with the source column highlighted → transform chain as named pills → fusion rule → **rejected alternatives** → merge evidence with per-feature contributions. |
| **Link explorer** | Force-directed graph (`react-force-graph` or Cytoscape). Click a node to expand its neighbourhood. |
| **Review queue** | Uncertain pairs side-by-side with a feature-contribution breakdown. Accept / reject. |

State: TanStack Query. Styling: Tailwind. Don't spend design time here.

---

## 12. Repository layout

> **This section is the target layout, not the current one.** Directories
> marked `[planned]` do not exist in the repository yet - see the status table
> in the README for what is actually built. A design document that reads as a
> description of existing code is a document that will eventually mislead
> somebody, including you.

```
sextant/
├── CMakeLists.txt
├── cmake/                       # FetchContent declarations
├── include/sextant/             # public headers
├── src/
│   ├── lsm/                     # STANDALONE - own tests, own benchmarks
│   │   ├── skiplist.h  memtable.{h,cpp}  wal.{h,cpp}
│   │   ├── sstable_builder.{h,cpp}  sstable_reader.{h,cpp}  block.{h,cpp}
│   │   ├── bloom.{h,cpp}  block_cache.{h,cpp}  comparator.h
│   │   ├── version_set.{h,cpp}  compaction.{h,cpp}
│   │   └── db.{h,cpp}  iterator.{h,cpp}  write_batch.{h,cpp}
│   ├── codec/                   # key encoding, varint, big-endian, ULID, hash
│   ├── ontology/                # schema loader, type registry, transforms, mapping
│   ├── connectors/              # csv, json_source, postgres, ingest
│   ├── resolve/                 # [planned] normalize, blocking, scorer, clusterer, fuser
│   ├── lineage/                 # [planned] provenance writer / reader
│   ├── query/                   # [planned] planner, executor, index selection
│   ├── api/                     # [planned] cpp-httplib routes + JSON serialization
│   └── cli/                     # sextant ingest | stats | lineage | schema
│                                #   (resolve | query | serve are [planned])
├── tests/
├── bench/
├── schema/
│   ├── ontology.yaml
│   └── mappings/{wpi,unlocode,digitraffic,marinecadastre}.yaml
├── data/
│   ├── snapshots/               # committed small samples
│   └── fetch.sh                 # downloads the full datasets
├── eval/
│   ├── golden_ports.csv         # hand-labeled pairs
│   └── golden_vessels.csv
├── web/                         # React app
├── docs/
│   ├── adr/                     # architecture decision records - 1 page each
│   ├── BUGS.md                  # every bug worth writing down, as it was fixed
│   └── diagrams/
└── docker-compose.yml           # Postgres for source C
```

---

## 13. Dependencies

| Need | Library | Why |
|---|---|---|
| Build | CMake ≥ 3.20 + `FetchContent` | pins versions, no system installs, CI-friendly |
| JSON | [`nlohmann/json`](https://github.com/nlohmann/json) | the de-facto standard; C++20-clean; FetchContent is the documented path |
| HTTP client + server | `yhirose/cpp-httplib` | header-only, does both sides, zero ceremony |
| Postgres | `libpqxx` | the official C++ client |
| YAML | `yaml-cpp` | schema + mapping files |
| Testing | GoogleTest **or** Catch2 | pick one, don't mix |
| Logging | `spdlog` | structured, fast |
| Compression | `zstd` (or `snappy`) | SSTable block compression |
| Hashing | `xxHash` | bloom filters, block cache keys |
| CLI | `CLI11` | subcommands for free |
| Frontend | Vite, React, TS, TanStack Query, Tailwind, react-force-graph | |

Standard: **C++20**. You want `std::span`, designated initialisers, concepts for the comparator, and `<bit>` for byte-order helpers. Don't reach for C++23 - compiler support will bite you.

---

## 14. Deliberate non-goals

Each of these is a real capability the system does not have. They are listed
here, and in the README, because a boundary that is written down is a decision
and a boundary that is not is an oversight.

- **No distribution.** Single node. Sharding the LSM and doing distributed ER is a different project.
- **No ML-based ER.** A hand-tuned Fellegi-Sunter-style scorer is interpretable and tunable on 500 labels. A learned model needs 10⁵ and gives you worse lineage.
- **No transactions across entities.** `WriteBatch` gives per-merge atomicity. Full serializable multi-entity transactions are out of scope.
- **No incremental re-resolution.** Re-ingest triggers a full re-resolve. Incremental ER is a genuinely hard research problem; know that, and be able to sketch how you'd approach it (dirty-cluster tracking).
- **No auth / multi-tenancy.**

---

## 15. What it measures

Every number here is produced by a command, not recorded by hand. The full
conditions - hardware, configuration, value sizes - are stated next to each one
in [`BENCH.md`](BENCH.md) and [`ER.md`](ER.md), because a throughput figure
without its configuration is not a measurement.

| | |
|---|---|
| **Lineage** | 4,201 of 4,201 resolved properties pass the round-trip test. 100%. |
| **Entity resolution** | F1 **0.9912** on a held-out split the weights were never fitted to; 1.0000 on vessels |
| **Blocking** | RR **0.999**, PC **0.997** - 1.05M possible pairs reduced to a few hundred, losing one true pair in a thousand |
| **Clustering** | Veto-constrained precision **1.000** against plain transitive **0.973**, with 10 merges refused |
| **Storage** | 1.53M batched writes/sec · 494k random writes/sec · 3.7M misses/sec · write amplification **1.20x** · 655 writes/sec with `sync=true` |
| **Query** | The quarter query: 24 arrivals, **24 keys scanned**, `index_used: "TIDX"`, 230 µs end to end over HTTP |
| **Resolution** | 1,451 source records → 1,187 entities; 451 port and vessel records → 187, so 58% of them were duplicates |

The first row is the one that is genuinely uncommon. Most systems that claim
lineage record it; this one checks it, and the check can fail.

---

## Sources

- [UN/LOCODE - UNECE](https://unece.org/trade/uncefact/unlocode) · [download page](https://unece.org/trade/cefact/unlocode-code-list-country-and-territory) · [column descriptions](https://service.unece.org/trade/locode/Service/LocodeColumn.htm) · [parsed mirror](https://github.com/datasets/un-locode)
- [Digitraffic marine traffic APIs - Fintraffic](https://www.digitraffic.fi/en/marine-traffic/) · [Swagger](https://meri.digitraffic.fi/swagger/)
- [MarineCadastre.gov vessel traffic (AIS)](https://hub.marinecadastre.gov/datasets/vessel-traffic-ais-1) · [AccessAIS extractor](https://marinecadastre.gov/accessais/) · [NOAA Digital Coast](https://coast.noaa.gov/digitalcoast/data/vesseltraffic.html)
- [nlohmann/json](https://github.com/nlohmann/json) · [CMake integration](https://json.nlohmann.me/integration/cmake/)
