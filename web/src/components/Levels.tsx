import type { Level, SSTable } from '../types'
import { bytes, dec, num } from '../format'

export function LevelBars({ levels, live }: { levels: Level[]; live?: number[] }) {
  const max = Math.max(1, ...levels.map((l) => l.bytes))
  return (
    <div className="level-bars">
      {levels.map((l) => {
        const files = live?.[l.level] ?? l.files
        const ratio = l.bytes / max
        return (
          <div key={l.level} className={`level-bar${files === 0 ? ' level-empty' : ''}`}>
            <span className="level-name">L{l.level}</span>
            <span className="level-track">
              <span className="level-fill" style={{ width: files === 0 ? '0%' : `${Math.max(1.5, ratio * 100).toFixed(1)}%` }} />
            </span>
            <span className="level-meta">
              <b>{num(files)}</b> {files === 1 ? 'arquivo' : 'arquivos'} · {bytes(l.bytes)}
            </span>
          </div>
        )
      })}
    </div>
  )
}

interface StrataProps {
  levels: Level[]
  selected: number | null
  onSelect: (t: SSTable, level: number) => void
}

export function Strata({ levels, selected, onSelect }: StrataProps) {
  const max = Math.max(1, ...levels.map((l) => l.bytes))
  return (
    <div className="strata">
      {levels.map((l) => {
        const share = l.bytes / max
        const target = l.max_bytes > 0 ? l.max_bytes : 0
        return (
          <div key={l.level} className="stratum">
            <div className="stratum-head">
              <span className="stratum-name">L{l.level}</span>
              <span className="stratum-info">
                {num(l.files)} {l.files === 1 ? 'SSTable' : 'SSTables'} · {bytes(l.bytes)}
                {target ? <> de {bytes(target)}</> : l.level === 0 ? <> · gatilho por quantidade</> : null}
              </span>
              <span className={`stratum-score${l.score >= 1 ? ' hot' : ''}`} title="Pontuação de compactação: a partir de 1 o nível é compactado">
                {dec(l.score, 2)}
              </span>
            </div>
            <div className="stratum-band" style={{ width: l.files === 0 ? '100%' : `${Math.max(8, share * 100).toFixed(1)}%` }}>
              {l.tables.length === 0 ? (
                <span className="stratum-void">{l.files > 0 ? `${num(l.files)} ${l.files === 1 ? 'arquivo' : 'arquivos'}` : 'vazio'}</span>
              ) : (
                l.tables.map((t) => (
                  <button
                    key={t.number}
                    type="button"
                    className={`sst${selected === t.number ? ' sst-on' : ''}${t.deletions > 0 && t.deletions / Math.max(1, t.entries) > 0.3 ? ' sst-tomb' : ''}`}
                    style={{ flexGrow: Math.max(1, t.bytes) }}
                    title={`#${t.number} · ${bytes(t.bytes)} · ${t.smallest} → ${t.largest}`}
                    onClick={() => onSelect(t, l.level)}
                  >
                    <span>{t.number}</span>
                  </button>
                ))
              )}
            </div>
          </div>
        )
      })}
    </div>
  )
}
