// The type browser and entity list.
//
// THE TEST THIS VIEW HAS TO PASS
//
// Grep this file for "Port", "Vessel" or "Voyage" and you will not find them.
// The type tabs come from /api/ontology, the search field targets whichever
// property the schema marked `title`, and the result rows render the display
// template the schema declares.
//
// That is not tidiness for its own sake. It is the demo: add an entity type to
// schema/ontology.yaml, restart the binary, and the type appears in the UI with
// its properties, its links and its lineage, having touched no frontend code.
//
// THE SEARCH BOX IS DEBOUNCED, AND THE PLAN SAYS WHY IT IS FAST
//
// Each keystroke is a prefix range scan over IDX. The plan panel underneath
// shows IDX_PREFIX, and if someone removes `indexed: true` from the title
// property it will say SCAN instead - visibly, on screen, rather than as a
// slowdown nobody attributes correctly.

import { useQuery } from '@tanstack/react-query'
import { useEffect, useState } from 'react'

import { api } from '../api/client'
import type { Ontology } from '../api/types'
import { PlanPanel } from './PlanPanel'
import { Chip, Empty, ErrorBox, Panel, Spinner } from './ui'

function useDebounced<T>(value: T, ms: number): T {
  const [debounced, setDebounced] = useState(value)
  useEffect(() => {
    const timer = setTimeout(() => setDebounced(value), ms)
    return () => clearTimeout(timer)
  }, [value, ms])
  return debounced
}

export function EntityBrowser({
  ontology,
  selectedType,
  onSelectType,
  onOpen,
  selectedId,
}: {
  ontology: Ontology
  selectedType: string
  onSelectType: (type: string) => void
  onOpen: (id: string) => void
  selectedId: string | null
}) {
  const [search, setSearch] = useState('')
  const debounced = useDebounced(search, 200)

  const typeDef = ontology.types.find((t) => t.name === selectedType)
  const titleProp = typeDef?.properties.find((p) => p.title)

  const { data, isLoading, error } = useQuery({
    queryKey: ['entities', selectedType, debounced],
    queryFn: () =>
      api.entities({ type: selectedType, q: debounced || undefined, limit: 200 }),
    enabled: Boolean(selectedType),
  })

  return (
    <div className="space-y-4">
      <Panel title="entity types">
        <div className="flex flex-wrap gap-2 p-3">
          {ontology.types.map((type) => (
            <button
              key={type.id}
              onClick={() => {
                onSelectType(type.name)
                setSearch('')
              }}
              title={type.description}
              className={`rounded-md border px-3 py-1.5 text-sm transition ${
                type.name === selectedType
                  ? 'border-accent/60 bg-accent/10 text-accent'
                  : 'border-edge bg-raised text-body/80 hover:border-accent/40'
              }`}
            >
              {type.name}
              <span className="mono ml-2 text-[10px] text-dim">
                {type.properties.length}p
              </span>
            </button>
          ))}
        </div>
        {typeDef?.description && (
          <p className="border-t border-edge px-4 py-2.5 text-[13px] leading-relaxed text-dim">
            {typeDef.description}
          </p>
        )}
      </Panel>

      <Panel
        title={`${selectedType} entities`}
        right={
          data && (
            <span className="mono text-xs text-dim">
              {data.count}
              {data.total_before_limit > data.count
                ? ` of ${data.total_before_limit}`
                : ''}
            </span>
          )
        }
      >
        <div className="border-b border-edge p-3">
          <input
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            placeholder={
              titleProp
                ? `search ${selectedType} by ${titleProp.name}...`
                : `${selectedType} has no title property to search`
            }
            disabled={!titleProp}
            className="w-full rounded-md border border-edge bg-ink px-3 py-2 text-sm outline-none placeholder:text-dim focus:border-accent/60 disabled:opacity-50"
          />
          {titleProp && !titleProp.indexed && (
            <p className="mt-1.5 text-[11px] text-warn">
              {selectedType}.{titleProp.name} is not indexed, so every keystroke
              is a full scan. The plan below says so too.
            </p>
          )}
        </div>

        {isLoading && <Spinner label="searching" />}
        {error && <ErrorBox error={error} />}
        {data && data.entities.length === 0 && (
          <Empty>
            no {selectedType} matches {debounced ? `"${debounced}"` : 'this query'}
          </Empty>
        )}
        {data && data.entities.length > 0 && (
          <ul className="max-h-[26rem] overflow-y-auto">
            {data.entities.map((entity) => (
              <li key={entity.id}>
                <button
                  onClick={() => onOpen(entity.id)}
                  className={`flex w-full items-baseline gap-3 border-t border-edge px-4 py-2 text-left first:border-t-0 hover:bg-raised ${
                    entity.id === selectedId ? 'bg-accent/10' : ''
                  }`}
                >
                  <span className="truncate text-sm">
                    {entity.display || entity.id}
                  </span>
                  <Chip>{entity.id.slice(0, 8)}</Chip>
                </button>
              </li>
            ))}
          </ul>
        )}
      </Panel>

      <PlanPanel
        plan={data?._plan}
        stats={data?._stats}
        resultCount={data?.count}
      />
    </div>
  )
}
