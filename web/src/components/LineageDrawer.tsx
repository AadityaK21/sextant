// The lineage drawer. This is the view the whole project exists to make
// possible.
//
// It answers "why does this say Rotterdam?" all the way down:
//
//   the stored value
//     <- the fusion rule that picked it, and what lost, and why
//     <- the transform chain, by pinned id, replayed live
//     <- the exact cell of the exact source row
//     <- the verbatim row, shown as the source wrote it
//
// THE RAW ROW IS PARSED IN THE BROWSER, ON PURPOSE
//
// The server sends the row as bytes, unchanged, because "unchanged" is the
// entire claim being made about it. Splitting it into columns for display is a
// presentation decision and belongs here. If the split is wrong, the raw text
// is still on screen above it and the reader can see that for themselves -
// which would not be true if the server had done the splitting and sent only
// the result.
//
// THE VERDICT IS RECOMPUTED, NOT STORED
//
// `replay.matches` is not a flag someone wrote down at ingest time. The server
// read the provenance, fetched the row, re-applied the chain and compared, on
// this request. A stored flag would be a claim; this is a check.

import { useQuery } from '@tanstack/react-query'
import { api } from '../api/client'
import type { Explanation } from '../api/types'
import { Chip, ErrorBox, Spinner } from './ui'

/**
 * Split a raw row into cells for display.
 *
 * CSV and JSON both arrive here as one string, because RAW stores what the
 * source produced and nothing else. A JSON object is shown as its keys; a CSV
 * line is split on commas, respecting quotes. Neither is authoritative - the
 * verbatim text sits above this table - so a wrong guess is visible rather than
 * misleading.
 */
function parseRow(raw: string): { column: string; value: string }[] | null {
  const text = raw.trim()
  if (text.startsWith('{')) {
    try {
      const parsed = JSON.parse(text) as Record<string, unknown>
      return Object.entries(parsed).map(([column, value]) => ({
        column,
        value: value === null ? '' : String(value),
      }))
    } catch {
      return null
    }
  }

  const cells: string[] = []
  let current = ''
  let inQuotes = false
  for (let i = 0; i < text.length; i++) {
    const c = text[i]!
    if (inQuotes) {
      if (c === '"') {
        if (text[i + 1] === '"') {
          current += '"'
          i++
        } else {
          inQuotes = false
        }
      } else {
        current += c
      }
    } else if (c === '"') {
      inQuotes = true
    } else if (c === ',') {
      cells.push(current)
      current = ''
    } else {
      current += c
    }
  }
  cells.push(current)
  return cells.map((value, i) => ({ column: `col ${i}`, value }))
}

function RawRowTable({ explanation }: { explanation: Explanation }) {
  const cells = parseRow(explanation.raw_row)
  const needle = explanation.raw_cell

  return (
    <div className="space-y-2">
      <div className="scroll-x rounded border border-edge bg-ink p-2">
        <pre className="mono whitespace-pre text-[11px] leading-relaxed text-dim">
          {explanation.raw_row}
        </pre>
      </div>

      {cells && (
        <div className="scroll-x rounded border border-edge">
          <table className="w-full border-collapse text-[12px]">
            <tbody>
              {cells.map((cell, i) => {
                // Highlight by VALUE, not by index. The provenance names a
                // column by name for JSON and by header lookup for CSV, and
                // this component has neither header available - but it does
                // have the cell the server extracted, which is the thing the
                // reader is looking for.
                const isSource =
                  needle !== '' &&
                  (cell.value === needle ||
                    cell.column === explanation.origin.column)
                return (
                  <tr
                    key={i}
                    className={
                      isSource
                        ? 'bg-accent/15 outline outline-1 outline-accent/40'
                        : i % 2
                          ? 'bg-raised/40'
                          : ''
                    }
                  >
                    <td className="mono w-40 border-r border-edge px-2 py-1 align-top text-dim">
                      {cell.column}
                    </td>
                    <td className="mono px-2 py-1 align-top">
                      {cell.value || <span className="text-dim">-</span>}
                      {isSource && (
                        <span className="ml-2 text-[10px] uppercase tracking-wide text-accent">
                          source cell
                        </span>
                      )}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

function Section({
  step,
  title,
  children,
}: {
  step: number
  title: string
  children: React.ReactNode
}) {
  return (
    <div className="border-t border-edge px-4 py-4 first:border-t-0">
      <div className="mb-2.5 flex items-center gap-2">
        <span className="mono flex h-5 w-5 items-center justify-center rounded-full border border-edge bg-raised text-[10px] text-dim">
          {step}
        </span>
        <h3 className="text-xs font-semibold uppercase tracking-wider text-dim">
          {title}
        </h3>
      </div>
      {children}
    </div>
  )
}

export function LineageDrawer({
  entityId,
  property,
  onClose,
}: {
  entityId: string
  property: string
  onClose: () => void
}) {
  const { data, isLoading, error } = useQuery({
    queryKey: ['lineage', entityId, property],
    queryFn: () => api.lineage(entityId, property),
  })

  return (
    <div className="fixed inset-0 z-40 flex justify-end">
      <div
        className="absolute inset-0 bg-black/60"
        onClick={onClose}
        aria-hidden
      />
      <aside className="relative z-10 flex h-full w-full max-w-3xl flex-col border-l border-edge bg-panel shadow-2xl">
        <header className="flex items-start justify-between gap-4 border-b border-edge px-4 py-3">
          <div className="min-w-0">
            <div className="text-[10px] uppercase tracking-wider text-dim">
              lineage
            </div>
            <div className="mono truncate text-sm">
              {property}
              {data && (
                <>
                  <span className="text-dim"> = </span>
                  <span className="text-accent">{data.stored_value}</span>
                </>
              )}
            </div>
          </div>
          <button
            onClick={onClose}
            className="rounded border border-edge px-2 py-1 text-xs text-dim hover:bg-raised hover:text-body"
          >
            close
          </button>
        </header>

        <div className="flex-1 overflow-y-auto">
          {isLoading && <Spinner label="reading the provenance" />}
          {error && <ErrorBox error={error} />}
          {data && !data.found && (
            <div className="m-4 rounded border border-warn/40 bg-warn/10 px-4 py-3 text-sm text-warn">
              No provenance record for this property. Every fused value should
              have one, so this is a gap rather than an expected state.
            </div>
          )}

          {data && data.found && (
            <>
              <Section step={1} title="the value, and why this one">
                <div className="flex flex-wrap items-center gap-2">
                  <Chip tone="accent">{data.fusion.rule}</Chip>
                  <span className="text-[13px] text-dim">
                    confidence {data.fusion.confidence.toFixed(2)}
                  </span>
                  <span className="text-[13px] text-dim">
                    merged from {data.cluster_size} source record
                    {data.cluster_size === 1 ? '' : 's'}
                  </span>
                </div>
                {data.merge_evidence.length > 0 && (
                  <ul className="mt-2 space-y-1">
                    {data.merge_evidence.map((e, i) => (
                      <li key={i} className="text-[13px] text-dim">
                        {e}
                      </li>
                    ))}
                  </ul>
                )}
              </Section>

              {data.rejected.length > 0 && (
                <Section step={2} title={`what lost (${data.rejected.length})`}>
                  <div className="space-y-2">
                    {data.rejected.map((r, i) => (
                      <div
                        key={i}
                        className="rounded border border-edge bg-raised/50 px-3 py-2"
                      >
                        <div className="mono text-[13px] text-body/90">
                          "{r.value}"
                        </div>
                        <div className="mt-1 flex flex-wrap items-center gap-x-3 gap-y-1 text-[11px] text-dim">
                          <span>
                            source {r.source} batch {r.batch} row {r.row}
                          </span>
                          <Chip>{r.column}</Chip>
                        </div>
                        <div className="mt-1 text-[12px] text-warn/90">
                          {r.reason}
                        </div>
                      </div>
                    ))}
                  </div>
                </Section>
              )}

              <Section
                step={data.rejected.length > 0 ? 3 : 2}
                title="the transform chain"
              >
                <div className="flex flex-wrap items-center gap-1.5">
                  <Chip>"{data.raw_cell}"</Chip>
                  {data.transforms.map((t) => (
                    <span key={t.id} className="flex items-center gap-1.5">
                      <span className="text-dim">-&gt;</span>
                      <Chip
                        tone="accent"
                        title={`transform id ${t.id}, pinned so provenance written today still replays years from now`}
                      >
                        {t.name}
                      </Chip>
                    </span>
                  ))}
                  <span className="text-dim">-&gt;</span>
                  <Chip tone={data.replay.matches ? 'good' : 'bad'}>
                    "{data.replay.value}"
                  </Chip>
                </div>

                <div className="mt-3 flex flex-wrap items-center gap-2">
                  {data.replay.matches ? (
                    <Chip tone="good">
                      verified by {data.replay.check}
                    </Chip>
                  ) : (
                    <Chip tone="bad">replay does not match</Chip>
                  )}
                  {data.replay.check === 'containment' && (
                    <span className="text-[12px] text-dim">
                      A union property is the merge of several sources, so no
                      single cell reproduces the whole list. The check is that
                      what this row contributed is present in it.
                    </span>
                  )}
                  {data.chain_changed && (
                    <Chip tone="warn">
                      a transform has been re-versioned since this was written
                    </Chip>
                  )}
                  {data.replay.error && (
                    <span className="text-[12px] text-bad">
                      {data.replay.error}
                    </span>
                  )}
                </div>
              </Section>

              <Section
                step={data.rejected.length > 0 ? 4 : 3}
                title="the raw source row"
              >
                <div className="mb-2 flex flex-wrap items-center gap-2 text-[12px] text-dim">
                  <Chip tone="accent">{data.origin.source}</Chip>
                  <span>batch {data.origin.batch}</span>
                  <span>row {data.origin.row}</span>
                  <span>column</span>
                  <Chip>{data.origin.column}</Chip>
                </div>
                {data.raw_row_found ? (
                  <RawRowTable explanation={data} />
                ) : (
                  <div className="rounded border border-bad/40 bg-bad/10 px-3 py-2 text-[13px] text-bad">
                    The provenance points at a row that is not in the store.
                    This is the failure the round-trip test exists to catch.
                  </div>
                )}
              </Section>
            </>
          )}
        </div>
      </aside>
    </div>
  )
}
