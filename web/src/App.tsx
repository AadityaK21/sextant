// The shell.
//
// STATE LIVES IN THE URL HASH
//
// #/Port/01M02TVE9E2CDS4ZT8EQ3NGVQ4 is a shareable link to an entity, the back
// button works, and a reload lands where you were. That costs about fifteen
// lines and is the difference between a demo you can send someone a link into
// and one where you say "now click through to...".
//
// THE ONTOLOGY IS FETCHED ONCE AND PASSED DOWN
//
// Everything generic about this UI depends on it, so it is fetched at the root
// and nothing renders until it arrives. That also makes the failure mode clean:
// if the API is unreachable there is one error on screen explaining it, rather
// than six components each failing separately.

import { useEffect, useState } from 'react'
import { useQuery } from '@tanstack/react-query'

import { api } from './api/client'
import { EntityBrowser } from './components/EntityBrowser'
import { EntityDetail } from './components/EntityDetail'
import { LinkGraph } from './components/LinkGraph'
import { ReviewQueue } from './components/ReviewQueue'
import { StatsBar } from './components/StatsBar'
import { ErrorBox, Panel, Spinner } from './components/ui'

type Tab = 'browse' | 'graph' | 'review'

function readHash(): { type?: string; id?: string; tab?: Tab } {
  const raw = window.location.hash.replace(/^#\/?/, '')
  if (!raw) return {}
  const [type, id, tab] = raw.split('/')
  return {
    type: type || undefined,
    id: id || undefined,
    tab: (tab as Tab) || undefined,
  }
}

export default function App() {
  const {
    data: ontology,
    isLoading,
    error,
  } = useQuery({ queryKey: ['ontology'], queryFn: api.ontology })

  const initial = readHash()
  const [selectedType, setSelectedType] = useState(initial.type ?? '')
  const [selectedId, setSelectedId] = useState<string | null>(initial.id ?? null)
  const [tab, setTab] = useState<Tab>(initial.tab ?? 'browse')

  // The first type in the ontology, not a name written here. See the note in
  // EntityBrowser: no entity type is named anywhere in this frontend.
  useEffect(() => {
    if (!selectedType && ontology?.types[0]) {
      setSelectedType(ontology.types[0].name)
    }
  }, [ontology, selectedType])

  useEffect(() => {
    const next = `#/${selectedType}/${selectedId ?? ''}/${tab}`
    if (window.location.hash !== next) {
      window.history.replaceState(null, '', next)
    }
  }, [selectedType, selectedId, tab])

  useEffect(() => {
    const onPop = () => {
      const state = readHash()
      if (state.type) setSelectedType(state.type)
      setSelectedId(state.id ?? null)
      if (state.tab) setTab(state.tab)
    }
    window.addEventListener('hashchange', onPop)
    return () => window.removeEventListener('hashchange', onPop)
  }, [])

  if (isLoading) {
    return (
      <div className="p-8">
        <Spinner label="loading the ontology" />
      </div>
    )
  }
  if (error) {
    return (
      <div className="mx-auto max-w-2xl p-8">
        <ErrorBox error={error} />
      </div>
    )
  }
  if (!ontology) return null

  const tabs: { key: Tab; label: string; enabled: boolean }[] = [
    { key: 'browse', label: 'browse', enabled: true },
    { key: 'graph', label: 'links', enabled: Boolean(selectedId) },
    { key: 'review', label: 'review', enabled: true },
  ]

  return (
    <div className="min-h-screen">
      <header className="border-b border-edge bg-panel">
        <div className="flex items-baseline gap-4 px-4 py-3">
          <h1 className="text-lg font-semibold tracking-tight">sextant</h1>
          <span className="text-[12px] text-dim">
            {ontology.namespace} ontology v{ontology.version} ·{' '}
            {ontology.types.length} types · {ontology.links.length} link types
          </span>
          <nav className="ml-auto flex gap-1">
            {tabs.map((t) => (
              <button
                key={t.key}
                onClick={() => setTab(t.key)}
                disabled={!t.enabled}
                title={
                  t.enabled ? undefined : 'select an entity first'
                }
                className={`rounded px-3 py-1.5 text-sm transition ${
                  tab === t.key
                    ? 'bg-accent/15 text-accent'
                    : 'text-dim hover:bg-raised hover:text-body'
                } disabled:cursor-not-allowed disabled:opacity-40`}
              >
                {t.label}
              </button>
            ))}
          </nav>
        </div>
      </header>

      <StatsBar />

      <main className="mx-auto max-w-[110rem] p-4">
        {tab === 'browse' && (
          <div className="grid gap-4 lg:grid-cols-[minmax(0,26rem)_minmax(0,1fr)]">
            <EntityBrowser
              ontology={ontology}
              selectedType={selectedType}
              onSelectType={(type) => {
                setSelectedType(type)
                setSelectedId(null)
              }}
              onOpen={setSelectedId}
              selectedId={selectedId}
            />
            <div>
              {selectedId ? (
                <EntityDetail
                  entityId={selectedId}
                  ontology={ontology}
                  onOpen={setSelectedId}
                />
              ) : (
                <Panel title="entity">
                  <div className="px-4 py-16 text-center text-sm text-dim">
                    Select an entity to see its properties, where every value
                    came from, and what it links to.
                  </div>
                </Panel>
              )}
            </div>
          </div>
        )}

        {tab === 'graph' && selectedId && (
          <LinkGraph
            rootId={selectedId}
            ontology={ontology}
            onOpen={(id) => {
              setSelectedId(id)
              setTab('browse')
            }}
          />
        )}

        {tab === 'review' && <ReviewQueue />}
      </main>
    </div>
  )
}
