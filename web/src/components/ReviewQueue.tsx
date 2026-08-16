// The entity-resolution review queue.
//
// THE ORDERING IS FREE, AND THAT IS THE POINT
//
// CAND is keyed by inverted score, so scanning it forward already yields the
// most uncertain pairs first. No sort, no secondary structure - the pairs
// closest to the decision boundary come out first because of how the key was
// laid out.
//
// WHY THE FEATURE BREAKDOWN AND NOT A SCORE
//
// "5.23" tells a reviewer nothing. The question they are actually answering is
// which evidence carried it: a +5.2 made mostly of a name similarity between
// two different Italian ports is a very different case from a +5.2 carrying a
// locode match. The bars below are the scorer's own feature vector, stored
// structured in the candidate record rather than rendered to prose at write
// time and re-parsed here.
//
// ACCEPTING DOES NOT MERGE
//
// It records the answer. The merge happens on the next `sextant resolve`,
// because the veto-constrained clustering that makes the result trustworthy is
// a whole-graph operation and running it inside a request handler would be a
// bad trade. The UI says so rather than implying an instant effect.

import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'

import { api } from '../api/client'
import type { ReviewPair } from '../api/types'
import { Chip, Empty, ErrorBox, Panel, Spinner } from './ui'

function FeatureBars({ pair }: { pair: ReviewPair }) {
  const max = Math.max(
    1,
    ...pair.features.map((f) => Math.abs(f.contribution)),
  )
  return (
    <div className="space-y-1.5">
      {pair.features.map((feature) => {
        const width = (Math.abs(feature.contribution) / max) * 100
        const positive = feature.contribution >= 0
        return (
          <div key={feature.name} className="grid grid-cols-[9rem_1fr_3.5rem] items-center gap-2">
            <span className="mono truncate text-[11px] text-dim" title={feature.name}>
              {feature.name}
            </span>
            <div className="h-3 overflow-hidden rounded-sm bg-raised">
              <div
                className={`h-full ${positive ? 'bg-accent/70' : 'bg-bad/70'}`}
                style={{ width: `${width}%` }}
                title={`value ${feature.value.toFixed(3)} x weight ${feature.weight.toFixed(2)}`}
              />
            </div>
            <span
              className={`mono text-right text-[11px] ${positive ? 'text-accent' : 'text-bad'}`}
            >
              {feature.contribution >= 0 ? '+' : ''}
              {feature.contribution.toFixed(2)}
            </span>
            {feature.detail && (
              <span className="col-span-3 -mt-0.5 pl-[9.5rem] text-[11px] text-dim">
                {feature.detail}
              </span>
            )}
          </div>
        )
      })}
    </div>
  )
}

function PairCard({ pair }: { pair: ReviewPair }) {
  const queryClient = useQueryClient()
  const [reviewer, setReviewer] = useState('analyst')

  const decide = useMutation({
    mutationFn: (decision: 'accept' | 'reject') =>
      api.decide(pair.pair_id, decision, reviewer),
    onSuccess: () => queryClient.invalidateQueries({ queryKey: ['review'] }),
  })

  return (
    <div className="border-t border-edge px-4 py-4 first:border-t-0">
      <div className="mb-3 flex flex-wrap items-center gap-2">
        <span className="mono text-sm text-accent">
          {pair.score.toFixed(2)}
        </span>
        {pair.a && pair.b ? (
          <span className="text-sm">
            <span className="mono">{pair.a.label}</span>
            <span className="mx-2 text-dim">vs</span>
            <span className="mono">{pair.b.label}</span>
          </span>
        ) : (
          <span className="mono text-[12px] text-dim">
            pair {pair.pair_id.slice(0, 12)}
          </span>
        )}
        {pair.vetoed && <Chip tone="bad">vetoed: {pair.veto_reason}</Chip>}
        {pair.decision && (
          <Chip tone={pair.decision === 'accept' ? 'good' : 'bad'}>
            {pair.decision} by {pair.reviewer}
          </Chip>
        )}
        {pair.legacy_format && (
          <Chip tone="warn" title="Written before the structured candidate record existed. Re-run `sextant resolve` to refresh.">
            legacy record
          </Chip>
        )}
      </div>

      {pair.features.length > 0 ? (
        <FeatureBars pair={pair} />
      ) : (
        <p className="mono text-[12px] text-dim">{pair.explanation}</p>
      )}

      <div className="mt-3 flex flex-wrap items-center gap-2">
        <input
          value={reviewer}
          onChange={(e) => setReviewer(e.target.value)}
          className="w-28 rounded border border-edge bg-ink px-2 py-1 text-[12px] outline-none focus:border-accent/60"
          aria-label="reviewer"
        />
        <button
          onClick={() => decide.mutate('accept')}
          disabled={decide.isPending}
          className="rounded border border-good/50 bg-good/10 px-3 py-1 text-[12px] text-good hover:bg-good/20 disabled:opacity-50"
        >
          same entity
        </button>
        <button
          onClick={() => decide.mutate('reject')}
          disabled={decide.isPending}
          className="rounded border border-bad/50 bg-bad/10 px-3 py-1 text-[12px] text-bad hover:bg-bad/20 disabled:opacity-50"
        >
          different
        </button>
        {decide.isSuccess && (
          <span className="text-[11px] text-dim">
            recorded. The merge happens on the next `sextant resolve`.
          </span>
        )}
        {decide.error && (
          <span className="text-[11px] text-bad">
            {(decide.error as Error).message}
          </span>
        )}
      </div>
    </div>
  )
}

export function ReviewQueue() {
  const { data, isLoading, error } = useQuery({
    queryKey: ['review'],
    queryFn: () => api.review(50),
  })

  return (
    <Panel
      title="review queue"
      right={data && <Chip>{data.count} pairs</Chip>}
    >
      <p className="border-b border-edge px-4 py-2.5 text-[12px] leading-relaxed text-dim">
        Pairs the scorer placed between the two thresholds: not confident enough
        to merge, not clearly separate either. Most uncertain first, which is
        free - the candidate keyspace is ordered by inverted score, so a forward
        scan is already sorted by how close the pair sat to the boundary.
      </p>

      {isLoading && <Spinner label="scanning the candidate keyspace" />}
      {error && <ErrorBox error={error} />}
      {data && data.pairs.length === 0 && (
        <Empty>
          Nothing awaiting review. Every pair scored either above the match
          threshold or below the review one.
        </Empty>
      )}
      {data?.pairs.map((pair) => <PairCard key={pair.pair_id} pair={pair} />)}
    </Panel>
  )
}
