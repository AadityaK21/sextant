// One entity: its properties, its provenance summary, and its links.
//
// EVERY PROPERTY ROW IS A LINEAGE AFFORDANCE
//
// The provenance summary is inline - which source won, which rule, how many
// alternatives lost - and the row opens the full chain. That inline summary is
// what makes lineage feel like a property of the data rather than a feature
// you have to go and find.
//
// NOTHING HERE NAMES A TYPE OR A PROPERTY
//
// The property table is built from the ontology's declaration order, the link
// sections from the ontology's link list. Add `harbor_depth` to the YAML and it
// appears here, with its lineage, having touched no frontend code.

import { useQuery } from '@tanstack/react-query'
import { useState } from 'react'

import { api } from '../api/client'
import type { Entity, LinkTypeDef, Ontology } from '../api/types'
import { LineageDrawer } from './LineageDrawer'
import {
  Chip,
  Empty,
  ErrorBox,
  Panel,
  Spinner,
  formatValue,
} from './ui'

/** Which links touch this type, and which way they run from here. */
export function linksFor(ontology: Ontology, typeName: string) {
  const out: { link: LinkTypeDef; label: string; other: string }[] = []
  for (const link of ontology.links) {
    if (link.from === typeName) {
      out.push({ link, label: link.name, other: link.to })
    }
    if (link.to === typeName && link.inverse) {
      out.push({ link, label: link.inverse, other: link.from })
    }
  }
  return out
}

function LinkSection({
  entityId,
  label,
  link,
  other,
  onOpen,
}: {
  entityId: string
  label: string
  link: LinkTypeDef
  other: string
  onOpen: (id: string) => void
}) {
  const [expanded, setExpanded] = useState(false)
  const { data, isLoading, error } = useQuery({
    queryKey: ['links', entityId, label],
    queryFn: () => api.links(entityId, label, 50),
    enabled: expanded,
  })

  return (
    <div className="border-t border-edge first:border-t-0">
      <button
        onClick={() => setExpanded((e) => !e)}
        className="flex w-full items-center gap-2 px-4 py-2.5 text-left hover:bg-raised/50"
      >
        <span className="mono text-xs text-dim">{expanded ? '-' : '+'}</span>
        <span className="text-sm font-medium">{label}</span>
        <span className="text-xs text-dim">-&gt; {other}</span>
        {link.time_indexed && (
          <Chip
            tone="good"
            title={`time-indexed on ${link.time_index}: a date filter on this link is a range scan, not a filter`}
          >
            TIDX
          </Chip>
        )}
        {data && (
          <span className="ml-auto mono text-xs text-dim">
            {data.count}
            {data.total_before_limit > data.count
              ? ` of ${data.total_before_limit}`
              : ''}
          </span>
        )}
      </button>

      {expanded && (
        <div className="pb-1">
          {isLoading && <Spinner label="following the edge" />}
          {error && <ErrorBox error={error} />}
          {data && data.entities.length === 0 && <Empty>no links</Empty>}
          {data && data.entities.length > 0 && (
            <ul className="max-h-72 overflow-y-auto">
              {data.entities.map((e) => (
                <li key={e.id}>
                  <button
                    onClick={() => onOpen(e.id)}
                    className="flex w-full items-baseline gap-3 px-4 py-1.5 pl-9 text-left hover:bg-raised"
                  >
                    <span className="truncate text-[13px]">
                      {e.display || e.id}
                    </span>
                    <span className="mono ml-auto shrink-0 text-[10px] text-dim">
                      {e.type}
                    </span>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </div>
      )}
    </div>
  )
}

export function EntityDetail({
  entityId,
  ontology,
  onOpen,
}: {
  entityId: string
  ontology: Ontology
  onOpen: (id: string) => void
}) {
  const [lineageProperty, setLineageProperty] = useState<string | null>(null)

  const { data, isLoading, error } = useQuery<Entity>({
    queryKey: ['entity', entityId],
    queryFn: () => api.entity(entityId),
  })

  if (isLoading) return <Spinner label="reading the entity" />
  if (error) return <ErrorBox error={error} />
  if (!data) return null

  const typeDef = ontology.types.find((t) => t.name === data.type)
  // Declaration order, not JSON key order. The schema author chose an order and
  // it is more meaningful than alphabetical.
  const ordered = typeDef
    ? typeDef.properties
        .filter((p) => p.name in data.properties)
        .map((p) => ({ def: p, value: data.properties[p.name]! }))
    : Object.entries(data.properties).map(([name, value]) => ({
        def: undefined,
        value,
        name,
      }))

  return (
    <div className="space-y-4">
      <Panel
        title={data.type}
        right={<Chip title="entity id (ULID)">{data.id}</Chip>}
      >
        <div className="border-b border-edge px-4 py-3">
          <div className="text-lg">{data.display || data.id}</div>
        </div>

        <table className="w-full border-collapse text-sm">
          <tbody>
            {ordered.map((row, i) => {
              const name = row.def?.name ?? (row as { name: string }).name
              const prov = data._provenance?.[name]
              return (
                <tr
                  key={name}
                  className={`border-t border-edge ${i % 2 ? 'bg-raised/30' : ''}`}
                >
                  <td className="w-44 px-4 py-2 align-top">
                    <div className="mono text-[13px] text-dim">{name}</div>
                    {row.def && (
                      <div className="mt-0.5 flex flex-wrap gap-1">
                        <Chip>{row.def.type}</Chip>
                        {row.def.indexed && <Chip tone="accent">indexed</Chip>}
                      </div>
                    )}
                  </td>
                  <td className="px-4 py-2 align-top">
                    <div className="break-words">{formatValue(row.value)}</div>
                    {prov && (
                      <div className="mt-1 flex flex-wrap items-center gap-x-2 gap-y-1 text-[11px] text-dim">
                        <span>from</span>
                        <Chip tone="accent">{prov.source}</Chip>
                        <Chip>{prov.column}</Chip>
                        <span>by {prov.rule}</span>
                        {prov.rejected > 0 && (
                          <span className="text-warn">
                            {prov.rejected} rejected
                          </span>
                        )}
                        <Chip
                          tone={prov.verified ? 'good' : 'bad'}
                          title={
                            prov.verified
                              ? `Replayed from the raw source cell on this request and checked by ${prov.check}.`
                              : 'The stored value does not match a replay of its own transform chain.'
                          }
                        >
                          {prov.verified ? 'verified' : 'MISMATCH'}
                        </Chip>
                      </div>
                    )}
                  </td>
                  <td className="w-24 px-4 py-2 text-right align-top">
                    <button
                      onClick={() => setLineageProperty(name)}
                      className="rounded border border-edge px-2 py-1 text-[11px] text-dim hover:border-accent/50 hover:text-accent"
                    >
                      lineage
                    </button>
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </Panel>

      <Panel title="links">
        {linksFor(ontology, data.type).map(({ link, label, other }) => (
          <LinkSection
            key={label}
            entityId={data.id}
            label={label}
            link={link}
            other={other}
            onOpen={onOpen}
          />
        ))}
        {linksFor(ontology, data.type).length === 0 && (
          <Empty>no link types touch {data.type}</Empty>
        )}
      </Panel>

      {lineageProperty && (
        <LineageDrawer
          entityId={data.id}
          property={lineageProperty}
          onClose={() => setLineageProperty(null)}
        />
      )}
    </div>
  )
}
