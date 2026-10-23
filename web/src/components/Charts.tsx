import { useMemo, useState } from 'react'
import { useElementWidth } from '../state'

export interface Series {
  name: string
  values: number[]
  tone: 'a' | 'b' | 'c'
}

interface LineChartProps {
  series: Series[]
  height?: number
  format: (v: number) => string
  windowLabel?: string
  empty?: string
}

function niceMax(v: number): number {
  if (v <= 0) return 1
  const exp = Math.pow(10, Math.floor(Math.log10(v)))
  const f = v / exp
  const nice = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10
  return nice * exp
}

export function LineChart({ series, height = 180, format, windowLabel = '2 min', empty = 'Aguardando dados…' }: LineChartProps) {
  const [ref, width] = useElementWidth<HTMLDivElement>()
  const [hover, setHover] = useState<number | null>(null)
  const count = Math.max(0, ...series.map((s) => s.values.length))
  const left = 56
  const right = 12
  const top = 10
  const bottom = 22
  const w = Math.max(width, 200)
  const innerW = w - left - right
  const innerH = height - top - bottom
  const max = useMemo(() => niceMax(Math.max(0, ...series.flatMap((s) => s.values))), [series])
  const slots = 120
  const x = (i: number) => left + innerW - ((count - 1 - i) / (slots - 1)) * innerW
  const y = (v: number) => top + innerH - (v / max) * innerH
  const ticks = [0, 0.25, 0.5, 0.75, 1].map((f) => f * max)

  const onMove = (e: React.MouseEvent<SVGSVGElement>) => {
    if (!count) return
    const rect = e.currentTarget.getBoundingClientRect()
    const px = e.clientX - rect.left
    const idx = Math.round(count - 1 - ((left + innerW - px) / innerW) * (slots - 1))
    setHover(idx >= 0 && idx < count ? idx : null)
  }

  return (
    <div className="chart" ref={ref}>
      {width > 0 ? (
        <svg width={w} height={height} role="img" aria-label={series.map((s) => s.name).join(', ')} onMouseMove={onMove} onMouseLeave={() => setHover(null)}>
          {ticks.map((t, i) => (
            <g key={i}>
              <line className="chart-grid" x1={left} x2={w - right} y1={y(t)} y2={y(t)} />
              <text className="chart-label" x={left - 8} y={y(t) + 4} textAnchor="end">
                {format(t)}
              </text>
            </g>
          ))}
          <text className="chart-label" x={left} y={height - 4}>
            −{windowLabel}
          </text>
          <text className="chart-label" x={w - right} y={height - 4} textAnchor="end">
            agora
          </text>
          {count > 1
            ? series.map((s, si) => {
                const pts = s.values.map((v, i) => `${x(i).toFixed(1)},${y(v).toFixed(1)}`)
                const line = 'M' + pts.join('L')
                const area = `${line}L${x(s.values.length - 1).toFixed(1)},${y(0)}L${x(0).toFixed(1)},${y(0)}Z`
                const last = s.values[s.values.length - 1]
                return (
                  <g key={s.name} className={`series series-${s.tone}`}>
                    {si === 0 ? <path className="series-area" d={area} /> : null}
                    <path className="series-line" d={line} />
                    <circle className="series-dot" cx={x(s.values.length - 1)} cy={y(last)} r={3.2} />
                  </g>
                )
              })
            : null}
          {hover !== null ? (
            <g>
              <line className="chart-cursor" x1={x(hover)} x2={x(hover)} y1={top} y2={top + innerH} />
            </g>
          ) : null}
          {count <= 1 ? (
            <text className="chart-empty" x={left + innerW / 2} y={top + innerH / 2} textAnchor="middle">
              {empty}
            </text>
          ) : null}
        </svg>
      ) : null}
      <div className="chart-legend">
        {series.map((s) => {
          const v = hover !== null ? s.values[hover] : s.values[s.values.length - 1]
          return (
            <span key={s.name} className={`legend legend-${s.tone}`}>
              <i />
              {s.name}
              <b>{v === undefined ? '—' : format(v)}</b>
            </span>
          )
        })}
      </div>
    </div>
  )
}

export function Sparkline({ values, height = 34 }: { values: number[]; height?: number }) {
  const [ref, width] = useElementWidth<HTMLDivElement>()
  const w = Math.max(width, 60)
  const max = Math.max(1e-9, ...values) * 1.25
  const pts = values.map((v, i) => [values.length < 2 ? w : (i / (values.length - 1)) * w, height - 3 - (v / max) * (height - 6)])
  const d = pts.length ? 'M' + pts.map((p) => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join('L') : ''
  return (
    <div className="spark" ref={ref}>
      {width > 0 && pts.length > 1 ? (
        <svg width={w} height={height} aria-hidden="true">
          <path className="spark-area" d={`${d}L${w},${height}L0,${height}Z`} />
          <path className="spark-line" d={d} />
        </svg>
      ) : (
        <div style={{ height }} />
      )}
    </div>
  )
}

export function Meter({ value, max, label }: { value: number; max: number; label?: string }) {
  const ratio = max > 0 ? Math.min(1, value / max) : 0
  return (
    <div className="meter" role="meter" aria-valuemin={0} aria-valuemax={max} aria-valuenow={value} aria-label={label}>
      <span style={{ width: `${(ratio * 100).toFixed(1)}%` }} className={ratio > 0.9 ? 'meter-hot' : ratio > 0.7 ? 'meter-warm' : ''} />
    </div>
  )
}
