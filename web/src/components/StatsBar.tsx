// The header: what is in the store, and what the engine underneath is doing.
//
// The LSM numbers are here because they are the part of this project that is
// usually invisible. Seeing files per level and compaction counts next to the
// entity counts is what makes it read as an engine rather than a database
// wrapper.

import { useQuery } from '@tanstack/react-query'

import { api } from '../api/client'
import { Chip, formatBytes } from './ui'

function Metric({
  label,
  value,
  hint,
}: {
  label: string
  value: string | number
  hint?: string
}) {
  return (
    <div title={hint} className="px-3">
      <div className="text-[10px] uppercase tracking-wider text-dim">{label}</div>
      <div className="mono text-sm">{value}</div>
    </div>
  )
}

export function StatsBar() {
  const { data } = useQuery({ queryKey: ['stats'], queryFn: api.stats })
  if (!data) return null

  return (
    <div className="flex flex-wrap items-center gap-y-3 divide-x divide-edge border-b border-edge bg-panel py-2">
      {Object.entries(data.entities).map(([type, count]) => (
        <Metric key={type} label={type} value={count} />
      ))}
      <Metric
        label="source records"
        value={data.source_records}
        hint="Normalised rows read from the connectors, before resolution."
      />
      <Metric
        label="merged away"
        value={`${(data.dedup_ratio * 100).toFixed(1)}%`}
        hint="The fraction of source records that turned out to be duplicates. Same definition `sextant resolve` prints."
      />
      <Metric
        label="sstables"
        value={data.engine.sstables}
        hint="Files on disk across all levels."
      />
      <Metric label="on disk" value={formatBytes(data.engine.bytes_on_disk)} />
      <Metric
        label="compactions"
        value={`${data.engine.compactions} (${data.engine.trivial_moves} trivial)`}
        hint="A trivial move relabels a file to the next level without rewriting its bytes."
      />
      <Metric
        label="bloom rejects"
        value={data.engine.filter_rejections}
        hint="Lookups answered 'definitely absent' without reading a data block."
      />
      <div className="flex items-center gap-1.5 px-3">
        {data.engine.levels.map((level) => (
          <Chip key={level.level} title={`L${level.level}: ${formatBytes(level.bytes)}`}>
            L{level.level}:{level.files}
          </Chip>
        ))}
      </div>
    </div>
  )
}
