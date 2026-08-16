// The query plan and what it cost, rendered next to the results.
//
// WHY THIS IS ON SCREEN AND NOT IN A LOG
//
// The failure mode of a system like this is silent degradation. A query that
// used to hit TIDX starts doing a full scan because someone removed a
// time_index from the schema, and nothing anywhere says so. It still returns
// the right answer, just a hundred times slower, and you find out in
// production.
//
// With the plan next to the results, "full scan of Port: no usable index for
// name" is visible the moment it happens. That is not a hypothetical: the
// planner said exactly that on its first run against the real schema, and the
// search box got an index before it ever shipped slow.
//
// The numbers are measured, not estimated - a ReadStats the request owns,
// incremented inside the storage engine. See src/api/json.h.

import type { QueryPlan, QueryStats } from '../api/types'
import { Chip, Panel, PathBadge, formatMicros } from './ui'

function Stat({
  label,
  value,
  hint,
  tone,
}: {
  label: string
  value: string | number
  hint?: string
  tone?: 'good' | 'warn'
}) {
  const colour =
    tone === 'good' ? 'text-good' : tone === 'warn' ? 'text-warn' : 'text-body'
  return (
    <div title={hint} className="min-w-0">
      <div className="text-[10px] uppercase tracking-wider text-dim">{label}</div>
      <div className={`mono text-sm ${colour}`}>{value}</div>
    </div>
  )
}

export function PlanPanel({
  plan,
  stats,
  resultCount,
}: {
  plan?: QueryPlan
  stats?: QueryStats
  resultCount?: number
}) {
  if (!plan && !stats) return null

  // The claim worth making visible: a range scan touches about one key per
  // result. A scan-and-filter touches far more. Showing the ratio makes the
  // difference legible without the reader doing arithmetic.
  const ratio =
    stats && resultCount && resultCount > 0
      ? stats.keys_scanned / resultCount
      : undefined

  return (
    <Panel
      title="query plan"
      right={stats && <PathBadge path={stats.index_used} />}
      className="text-sm"
    >
      {plan && (
        <ol className="divide-y divide-edge">
          {plan.steps.map((step) => (
            <li key={step.ordinal} className="px-4 py-3">
              <div className="flex flex-wrap items-center gap-2">
                <span className="mono text-xs text-dim">{step.ordinal}.</span>
                <span className="font-medium">{step.description}</span>
                <PathBadge path={step.access_path} />
                <Chip>{step.keyspace}</Chip>
                {step.property && <Chip tone="accent">{step.property}</Chip>}
              </div>
              <p className="mt-1.5 pl-5 text-[13px] leading-relaxed text-dim">
                {step.reason}
              </p>
              {step.window && (
                <p className="mono mt-1 pl-5 text-[11px] text-dim">
                  window [{step.window.from}, {step.window.to})
                </p>
              )}
              {step.residuals.length > 0 && (
                <div className="mt-1.5 flex flex-wrap items-center gap-1.5 pl-5">
                  <span className="text-[11px] text-dim">then filter:</span>
                  {step.residuals.map((r, i) => (
                    <Chip key={i} tone="warn">
                      {r.property} {r.op} {String(r.value)}
                    </Chip>
                  ))}
                </div>
              )}
            </li>
          ))}
        </ol>
      )}

      {plan && plan.warnings.length > 0 && (
        <div className="border-t border-edge bg-warn/5 px-4 py-3">
          {plan.warnings.map((w, i) => (
            <div key={i} className="flex gap-2 text-[13px] text-warn">
              <span aria-hidden>!</span>
              <span>{w}</span>
            </div>
          ))}
        </div>
      )}

      {stats && (
        <div className="grid grid-cols-3 gap-x-4 gap-y-3 border-t border-edge px-4 py-3 sm:grid-cols-4 lg:grid-cols-6">
          <Stat
            label="keys scanned"
            value={stats.keys_scanned}
            tone={ratio !== undefined && ratio <= 3 ? 'good' : undefined}
            hint={
              ratio !== undefined
                ? `${ratio.toFixed(1)} keys per result. Close to 1 means the index turned the question into a range rather than a scan and filter.`
                : 'Entries the storage engine actually stepped over.'
            }
          />
          <Stat
            label="materialised"
            value={stats.entities_materialised}
            hint="Entities decoded into memory. A wide gap from keys scanned is the index doing its job."
          />
          <Stat
            label="blocks read"
            value={stats.blocks_read}
            hint="Data blocks pulled off disk. Cache hits are counted separately so a warm run and a cold run are distinguishable."
          />
          <Stat
            label="cache hits"
            value={stats.block_cache_hits}
            hint="Blocks that were already in memory."
          />
          <Stat
            label="bloom rejects"
            value={stats.bloom_rejections}
            hint="Lookups a filter answered with no I/O at all."
          />
          <Stat
            label="elapsed"
            value={formatMicros(stats.elapsed_us)}
            hint="Measured across the whole request, including materialising entities."
          />
        </div>
      )}

      {stats?.truncated && (
        <div className="border-t border-edge bg-warn/10 px-4 py-2 text-[13px] text-warn">
          Traversal stopped early at {stats.truncated_at}. Results are
          incomplete, not merely limited.
        </div>
      )}
    </Panel>
  )
}
