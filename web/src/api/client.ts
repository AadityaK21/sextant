// The fetch layer.
//
// ERRORS CARRY THE SERVER'S MESSAGE, NOT "Request failed"
//
// The API distinguishes 400 from 500 deliberately - an unknown property is the
// client's mistake, a decode failure is the server's - and it puts a sentence
// in the body explaining which. Throwing a generic Error would discard exactly
// the information that was worth sending, so `ApiError` keeps the status and
// the server's own message, and the UI shows it verbatim.
//
// A user seeing "unknown parameter or property: locoode" can fix their own
// problem. A user seeing "Request failed" opens a ticket.

import type {
  Entity,
  Explanation,
  Ontology,
  QueryResult,
  RawRow,
  ReviewQueue,
  Stats,
  TraverseRequest,
  ApiErrorBody,
} from './types'

export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
  ) {
    super(message)
    this.name = 'ApiError'
  }

  /** A 404 is an empty state, not a failure worth a red banner. */
  get isNotFound() {
    return this.status === 404
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let response: Response
  try {
    response = await fetch(path, {
      ...init,
      headers: { 'Content-Type': 'application/json', ...(init?.headers ?? {}) },
    })
  } catch (cause) {
    // A network-level failure, which in development almost always means the
    // C++ binary is not running. Say that rather than "Failed to fetch".
    throw new ApiError(
      0,
      'unreachable',
      'Cannot reach the Sextant API. Is `sextant serve` running on port 8080?',
    )
  }

  const text = await response.text()
  if (!response.ok) {
    let code = 'internal'
    let message = text || response.statusText
    try {
      const body = JSON.parse(text) as ApiErrorBody
      if (body.error) {
        code = body.error.code
        message = body.error.message
      }
    } catch {
      // Not JSON. Keep the raw body; it is more useful than discarding it.
    }
    throw new ApiError(response.status, code, message)
  }

  return JSON.parse(text) as T
}

const qs = (params: Record<string, string | number | undefined>) => {
  const search = new URLSearchParams()
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== '') search.set(key, String(value))
  }
  const s = search.toString()
  return s ? `?${s}` : ''
}

export const api = {
  ontology: () => request<Ontology>('/api/ontology'),

  entities: (params: { type: string; q?: string; limit?: number }) =>
    request<QueryResult>(`/api/entities${qs(params)}`),

  entity: (id: string) => request<Entity>(`/api/entities/${id}`),

  links: (id: string, linkType: string, limit = 200) =>
    request<QueryResult>(
      `/api/entities/${id}/links${qs({ type: linkType, limit })}`,
    ),

  traverse: (body: TraverseRequest) =>
    request<QueryResult>('/api/traverse', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  explainQuery: (body: TraverseRequest) =>
    request<{ query: string; _plan: QueryResult['_plan'] }>('/api/explain', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  lineage: (id: string, property: string) =>
    request<Explanation>(`/api/lineage/${id}/${property}`),

  raw: (source: string, batch: number, row: number) =>
    request<RawRow>(`/api/raw/${source}/${batch}/${row}`),

  review: (limit = 50) => request<ReviewQueue>(`/api/review${qs({ limit })}`),

  decide: (pairId: string, decision: 'accept' | 'reject', reviewer: string) =>
    request<{ pair_id: string; decision: string; applied: boolean; note: string }>(
      `/api/review/${pairId}`,
      { method: 'POST', body: JSON.stringify({ decision, reviewer }) },
    ),

  stats: () => request<Stats>('/api/stats'),
}
