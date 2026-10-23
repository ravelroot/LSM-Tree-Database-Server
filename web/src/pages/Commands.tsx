import { useEffect, useRef, useState } from 'react'
import type { FormEvent, KeyboardEvent } from 'react'
import { api, errorMessage } from '../api'
import { Button, Panel } from '../components/ui'
import { ms } from '../format'
import type { Reply } from '../types'

interface Entry {
  id: number
  input: string
  output: string
  error: boolean
  ms?: number
}

export function splitArgs(line: string): string[] {
  const args: string[] = []
  let cur = ''
  let quote: string | null = null
  let has = false
  for (let i = 0; i < line.length; i++) {
    const c = line[i]
    if (quote) {
      if (c === '\\' && i + 1 < line.length) {
        const n = line[++i]
        cur += n === 'n' ? '\n' : n === 't' ? '\t' : n
      } else if (c === quote) {
        quote = null
      } else {
        cur += c
      }
    } else if (c === '"' || c === "'") {
      quote = c
      has = true
    } else if (c === ' ' || c === '\t') {
      if (has || cur) args.push(cur)
      cur = ''
      has = false
    } else {
      cur += c
    }
  }
  if (quote) throw new Error('Aspas sem fechamento.')
  if (has || cur) args.push(cur)
  return args
}

export function renderReply(r: Reply, indent = ''): string {
  if (r === null) return '(nil)'
  if (typeof r === 'number') return `(integer) ${r}`
  if (typeof r === 'string') return JSON.stringify(r)
  if (Array.isArray(r)) {
    if (r.length === 0) return '(empty array)'
    const pad = String(r.length).length
    return r
      .map((item, i) => {
        const label = `${String(i + 1).padStart(pad)}) `
        const body = renderReply(item, indent + ' '.repeat(label.length))
        return (i === 0 ? '' : indent) + label + body
      })
      .join('\n')
  }
  return `(error) ${r.error}`
}

const tips = ['PING', 'INFO', 'DBSIZE', 'GET contador:pedidos', 'SCAN 0 MATCH cache:* COUNT 10', 'SLOWLOG GET 5', 'ARCA.STATS']

export function Commands() {
  const [line, setLine] = useState('')
  const [entries, setEntries] = useState<Entry[]>([])
  const [history, setHistory] = useState<string[]>([])
  const [pos, setPos] = useState(-1)
  const [busy, setBusy] = useState(false)
  const seq = useRef(0)
  const end = useRef<HTMLDivElement>(null)

  useEffect(() => {
    end.current?.scrollIntoView({ block: 'end' })
  }, [entries])

  const run = async (e?: FormEvent) => {
    e?.preventDefault()
    const input = line.trim()
    if (!input) return
    if (input.toLowerCase() === 'clear') {
      setEntries([])
      setLine('')
      return
    }
    let args: string[]
    try {
      args = splitArgs(input)
    } catch (err) {
      setEntries((x) => [...x, { id: ++seq.current, input, output: errorMessage(err), error: true }])
      return
    }
    setHistory((h) => [input, ...h.filter((x) => x !== input)].slice(0, 100))
    setPos(-1)
    setLine('')
    setBusy(true)
    try {
      const res = await api.command(args)
      const isErr = typeof res.reply === 'object' && res.reply !== null && !Array.isArray(res.reply)
      setEntries((x) => [...x, { id: ++seq.current, input, output: renderReply(res.reply), error: isErr, ms: res.ms }].slice(-200))
    } catch (err) {
      setEntries((x) => [...x, { id: ++seq.current, input, output: errorMessage(err), error: true }])
    } finally {
      setBusy(false)
    }
  }

  const onKey = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'ArrowUp') {
      e.preventDefault()
      const next = Math.min(history.length - 1, pos + 1)
      if (next >= 0) {
        setPos(next)
        setLine(history[next])
      }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault()
      const next = pos - 1
      setPos(next)
      setLine(next >= 0 ? history[next] : '')
    }
  }

  return (
    <div className="stack">
      <Panel
        title="Console do servidor"
        actions={
          <Button small onClick={() => setEntries([])}>
            Limpar
          </Button>
        }
      >
        <p className="note">
          Comandos vão direto ao Arca pelo protocolo RESP, como no <code>redis-cli</code>. Tudo fica registrado na auditoria. Use aspas para argumentos com espaço e ↑ ↓ para o histórico.
        </p>
        <div className="terminal" aria-live="polite">
          {entries.length === 0 ? (
            <div className="term-tips">
              {tips.map((t) => (
                <button key={t} type="button" className="chip" onClick={() => setLine(t)}>
                  {t}
                </button>
              ))}
            </div>
          ) : null}
          {entries.map((en) => (
            <div key={en.id} className="term-entry">
              <div className="term-in">
                <span className="term-prompt">arca&gt;</span> {en.input}
                {en.ms !== undefined ? <span className="term-ms">{ms(en.ms)}</span> : null}
              </div>
              <pre className={en.error ? 'term-out term-err' : 'term-out'}>{en.output}</pre>
            </div>
          ))}
          <div ref={end} />
        </div>
        <form className="term-line" onSubmit={run}>
          <span className="term-prompt mono">arca&gt;</span>
          <input aria-label="Comando" className="mono" value={line} onChange={(e) => setLine(e.target.value)} onKeyDown={onKey} spellCheck={false} autoComplete="off" />
          <Button type="submit" variant="primary" busy={busy}>
            Enviar
          </Button>
        </form>
      </Panel>
    </div>
  )
}
