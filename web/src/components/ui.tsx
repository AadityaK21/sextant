// Small shared pieces. Deliberately not a component library.
//
// The plan says "don't spend design time here", and the way to honour that
// without the app looking thrown together is a handful of primitives used
// everywhere rather than bespoke markup per view.

import type { ReactNode } from 'react'
import type { AccessPath, PropertyValue } from '../api/types'
import { ApiError } from '../api/client'

export function Panel({
  title,
  right,
  children,
  className = '',
}: {
  title?: ReactNode
  right?: ReactNode
  children: ReactNode
  className?: string
}) {
  return (
    <section
      className={`rounded-lg border border-edge bg-panel overflow-hidden ${className}`}
    >
      {title !== undefined && (
        <header className="flex items-center justify-between gap-3 border-b border-edge px-4 py-2.5">
          <h2 className="text-xs font-semibold uppercase tracking-wider text-dim">
            {title}
          </h2>
          {right}
        </header>
      )}
      {children}
    </section>
  )
}

export function Spinner({ label = 'loading' }: { label?: string }) {
  return (
    <div className="flex items-center gap-2 px-4 py-6 text-sm text-dim">
      <span className="inline-block h-3 w-3 animate-spin rounded-full border-2 border-edge border-t-accent" />
      {label}
    </div>
  )
}

/**
 * Shows the server's own message.
 *
 * The API separates a client mistake from a server fault and explains which in
 * the body; collapsing that into "something went wrong" throws away the one
 * part that lets a person fix their own problem.
 */
export function ErrorBox({ error }: { error: unknown }) {
  const api = error instanceof ApiError ? error : null
  const code = api?.code ?? 'error'
  const message = api?.message ?? String(error)
  return (
    <div className="m-4 rounded-md border border-bad/40 bg-bad/10 px-4 py-3 text-sm">
      <div className="mb-1 font-semibold text-bad">
        {code}
        {api && api.status > 0 ? ` (HTTP ${api.status})` : ''}
      </div>
      <div className="text-body/90">{message}</div>
    </div>
  )
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="px-4 py-8 text-center text-sm text-dim">{children}</div>
}

/** A monospace chip. Used for ids, columns, transforms and keyspaces. */
export function Chip({
  children,
  tone = 'default',
  title,
}: {
  children: ReactNode
  tone?: 'default' | 'good' | 'warn' | 'bad' | 'accent'
  title?: string
}) {
  const tones = {
    default: 'border-edge bg-raised text-body/80',
    good: 'border-good/40 bg-good/10 text-good',
    warn: 'border-warn/40 bg-warn/10 text-warn',
    bad: 'border-bad/40 bg-bad/10 text-bad',
    accent: 'border-accent/40 bg-accent/10 text-accent',
  }
  return (
    <span
      title={title}
      className={`mono inline-flex items-center rounded border px-1.5 py-0.5 text-[11px] leading-tight ${tones[tone]}`}
    >
      {children}
    </span>
  )
}

/**
 * The access path badge, coloured by how good the news is.
 *
 * TIDX and a point lookup are green because they are ranges; a full scan is
 * amber. This is the single glance that tells you whether the query you just
 * ran is one you would be happy to run against a hundred times more data.
 */
export function PathBadge({ path }: { path: AccessPath }) {
  const tone =
    path === 'TIDX' || path === 'POINT' || path === 'IDX'
      ? 'good'
      : path === 'SCAN'
        ? 'warn'
        : 'accent'
  return <Chip tone={tone}>{path}</Chip>
}

export function formatValue(value: PropertyValue): string {
  if (value === null || value === undefined) return '-'
  if (Array.isArray(value)) return value.join(', ')
  if (typeof value === 'boolean') return value ? 'true' : 'false'
  if (typeof value === 'number') {
    return Number.isInteger(value) ? String(value) : value.toFixed(4)
  }
  return value
}

/** 3105 -> "3.1 ms". Microseconds are the unit; nobody reads them as one. */
export function formatMicros(us: number): string {
  if (us < 1000) return `${us} us`
  if (us < 1_000_000) return `${(us / 1000).toFixed(1)} ms`
  return `${(us / 1_000_000).toFixed(2)} s`
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 ** 2) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 ** 3) return `${(bytes / 1024 ** 2).toFixed(1)} MB`
  return `${(bytes / 1024 ** 3).toFixed(2)} GB`
}
