// The link explorer: a force-directed graph in plain SVG, no dependency.
//
// WHY NOT react-force-graph
//
// It would have been faster to write and it renders on canvas, which handles
// far more nodes. It also pulls in three.js and d3-force - tens of megabytes of
// node_modules for one view, in a project whose own architecture note says
// "keep it thin, the backend is the project".
//
// At the scale this explores - a neighbourhood of a few dozen nodes, expanded
// by clicking - a simple simulation is enough, and it is code that can be
// explained rather than a library that was configured.
//
// THE SIMULATION
//
// Three forces, integrated with velocity Verlet and a decaying alpha:
//
//   repulsion   every pair pushes apart, O(n^2). A Barnes-Hut quadtree would
//               make that O(n log n) and is the right answer at thousands of
//               nodes; at fifty it is a slower way to get the same picture.
//   springs     each edge pulls toward a rest length
//   centring    a weak pull to the middle, so disconnected pieces do not drift
//
// Alpha decays geometrically and the loop stops when it is small. Without a
// stopping rule this is an animation that never ends and a laptop fan that
// never stops.

import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'

import { api } from '../api/client'
import type { Ontology } from '../api/types'
import { linksFor } from './EntityDetail'
import { Chip, ErrorBox, Panel, Spinner } from './ui'

interface Node {
  id: string
  label: string
  type: string
  x: number
  y: number
  vx: number
  vy: number
  depth: number
  fixed?: boolean
}

interface Edge {
  source: string
  target: string
  link: string
}

const WIDTH = 900
const HEIGHT = 560

/** Distinct, readable, and stable per type name rather than per index. */
function colourFor(type: string): string {
  const palette = ['#4aa8ff', '#3ecf8e', '#f0b429', '#f2617a', '#b48ead', '#88c0d0']
  let hash = 0
  for (let i = 0; i < type.length; i++) hash = (hash * 31 + type.charCodeAt(i)) | 0
  return palette[Math.abs(hash) % palette.length]!
}

function simulate(nodes: Node[], edges: Edge[], iterations: number) {
  const index = new Map(nodes.map((n) => [n.id, n]))
  let alpha = 1

  for (let step = 0; step < iterations && alpha > 0.005; step++) {
    // Repulsion. The epsilon floor on distance keeps two nodes that land on
    // exactly the same point from producing a division by zero and flinging
    // each other to infinity - which is what happens the first time you write
    // this and seed every node at the centre.
    for (let i = 0; i < nodes.length; i++) {
      const a = nodes[i]!
      for (let j = i + 1; j < nodes.length; j++) {
        const b = nodes[j]!
        let dx = b.x - a.x
        let dy = b.y - a.y
        let d2 = dx * dx + dy * dy
        if (d2 < 1) {
          dx = (Math.random() - 0.5) * 2
          dy = (Math.random() - 0.5) * 2
          d2 = dx * dx + dy * dy + 0.01
        }
        const force = 6000 / d2
        const d = Math.sqrt(d2)
        const fx = (dx / d) * force
        const fy = (dy / d) * force
        a.vx -= fx
        a.vy -= fy
        b.vx += fx
        b.vy += fy
      }
    }

    // Springs.
    for (const edge of edges) {
      const a = index.get(edge.source)
      const b = index.get(edge.target)
      if (!a || !b) continue
      const dx = b.x - a.x
      const dy = b.y - a.y
      const d = Math.sqrt(dx * dx + dy * dy) || 1
      const force = (d - 110) * 0.02
      const fx = (dx / d) * force
      const fy = (dy / d) * force
      a.vx += fx
      a.vy += fy
      b.vx -= fx
      b.vy -= fy
    }

    // Centring and integration.
    for (const node of nodes) {
      if (node.fixed) {
        node.vx = 0
        node.vy = 0
        continue
      }
      node.vx += (WIDTH / 2 - node.x) * 0.005
      node.vy += (HEIGHT / 2 - node.y) * 0.005
      node.vx *= 0.82
      node.vy *= 0.82
      node.x += node.vx * alpha
      node.y += node.vy * alpha
      node.x = Math.max(40, Math.min(WIDTH - 40, node.x))
      node.y = Math.max(30, Math.min(HEIGHT - 30, node.y))
    }

    alpha *= 0.985
  }
}

export function LinkGraph({
  rootId,
  ontology,
  onOpen,
}: {
  rootId: string
  ontology: Ontology
  onOpen: (id: string) => void
}) {
  const [nodes, setNodes] = useState<Node[]>([])
  const [edges, setEdges] = useState<Edge[]>([])
  const [expanding, setExpanding] = useState<string | null>(null)
  const [hovered, setHovered] = useState<string | null>(null)
  const expanded = useRef(new Set<string>())

  const { data: root, isLoading, error } = useQuery({
    queryKey: ['entity', rootId],
    queryFn: () => api.entity(rootId),
  })

  // Reset when the root changes, so the graph is about the entity you are
  // looking at rather than an accumulation of everywhere you have been.
  useEffect(() => {
    expanded.current = new Set()
    setEdges([])
    setNodes([])
  }, [rootId])

  useEffect(() => {
    if (!root || nodes.length > 0) return
    setNodes([
      {
        id: root.id,
        label: root.display || root.id,
        type: root.type,
        x: WIDTH / 2,
        y: HEIGHT / 2,
        vx: 0,
        vy: 0,
        depth: 0,
        fixed: true,
      },
    ])
  }, [root, nodes.length])

  const expand = useCallback(
    async (nodeId: string) => {
      if (expanded.current.has(nodeId)) {
        onOpen(nodeId)
        return
      }
      const node = nodes.find((n) => n.id === nodeId)
      if (!node) return

      expanded.current.add(nodeId)
      setExpanding(nodeId)
      try {
        const relations = linksFor(ontology, node.type)
        const nextNodes = [...nodes]
        const nextEdges = [...edges]
        const known = new Set(nextNodes.map((n) => n.id))

        for (const { label } of relations) {
          // A cap per link type, because a hub port has hundreds of arrivals
          // and a graph with hundreds of nodes in one burst is not a picture of
          // anything.
          const result = await api.links(nodeId, label, 8)
          for (const neighbour of result.entities) {
            if (!known.has(neighbour.id)) {
              known.add(neighbour.id)
              nextNodes.push({
                id: neighbour.id,
                label: neighbour.display || neighbour.id,
                type: neighbour.type,
                x: node.x + (Math.random() - 0.5) * 160,
                y: node.y + (Math.random() - 0.5) * 160,
                vx: 0,
                vy: 0,
                depth: node.depth + 1,
              })
            }
            nextEdges.push({ source: nodeId, target: neighbour.id, link: label })
          }
        }

        simulate(nextNodes, nextEdges, 320)
        setNodes(nextNodes)
        setEdges(nextEdges)
      } finally {
        setExpanding(null)
      }
    },
    [nodes, edges, ontology, onOpen],
  )

  // Expand the root once it exists, so the view is never an empty box.
  useEffect(() => {
    if (nodes.length === 1 && !expanded.current.has(rootId)) void expand(rootId)
  }, [nodes.length, rootId, expand])

  const byId = useMemo(() => new Map(nodes.map((n) => [n.id, n])), [nodes])
  const types = useMemo(
    () => Array.from(new Set(nodes.map((n) => n.type))),
    [nodes],
  )

  if (isLoading) return <Spinner label="loading the root entity" />
  if (error) return <ErrorBox error={error} />

  return (
    <Panel
      title="link explorer"
      right={
        <div className="flex items-center gap-2">
          {types.map((type) => (
            <span key={type} className="flex items-center gap-1 text-[11px]">
              <span
                className="inline-block h-2 w-2 rounded-full"
                style={{ background: colourFor(type) }}
              />
              <span className="text-dim">{type}</span>
            </span>
          ))}
          <Chip>{nodes.length} nodes</Chip>
        </div>
      }
    >
      <div className="scroll-x">
        <svg
          viewBox={`0 0 ${WIDTH} ${HEIGHT}`}
          className="block w-full"
          style={{ minWidth: 640 }}
        >
          <defs>
            <marker
              id="arrow"
              viewBox="0 0 10 10"
              refX="18"
              refY="5"
              markerWidth="5"
              markerHeight="5"
              orient="auto-start-reverse"
            >
              <path d="M 0 0 L 10 5 L 0 10 z" fill="#3a4657" />
            </marker>
          </defs>

          {edges.map((edge, i) => {
            const a = byId.get(edge.source)
            const b = byId.get(edge.target)
            if (!a || !b) return null
            const lit = hovered === edge.source || hovered === edge.target
            return (
              <line
                key={i}
                x1={a.x}
                y1={a.y}
                x2={b.x}
                y2={b.y}
                stroke={lit ? '#4aa8ff' : '#2a3444'}
                strokeWidth={lit ? 1.6 : 1}
                markerEnd="url(#arrow)"
              />
            )
          })}

          {nodes.map((node) => {
            const isRoot = node.id === rootId
            const isExpanded = expanded.current.has(node.id)
            return (
              <g
                key={node.id}
                transform={`translate(${node.x},${node.y})`}
                onMouseEnter={() => setHovered(node.id)}
                onMouseLeave={() => setHovered(null)}
                onClick={() => void expand(node.id)}
                style={{ cursor: 'pointer' }}
              >
                <circle
                  r={isRoot ? 13 : 9}
                  fill={colourFor(node.type)}
                  fillOpacity={isExpanded ? 0.95 : 0.35}
                  stroke={colourFor(node.type)}
                  strokeWidth={hovered === node.id ? 3 : 1.5}
                />
                {expanding === node.id && (
                  <circle
                    r={18}
                    fill="none"
                    stroke={colourFor(node.type)}
                    strokeWidth={1}
                    strokeDasharray="4 4"
                  />
                )}
                <text
                  y={isRoot ? 28 : 22}
                  textAnchor="middle"
                  className="mono"
                  fontSize={isRoot ? 12 : 10}
                  fill={hovered === node.id ? '#d7e0ea' : '#7d8ba1'}
                >
                  {node.label.length > 22
                    ? `${node.label.slice(0, 21)}...`
                    : node.label}
                </text>
              </g>
            )
          })}
        </svg>
      </div>

      <p className="border-t border-edge px-4 py-2 text-[12px] text-dim">
        Click an unexplored node to expand its neighbourhood, or an explored one
        to open it. Up to 8 neighbours per link type, because a hub port has
        hundreds of arrivals and all of them at once is not a picture of
        anything.
      </p>
    </Panel>
  )
}
