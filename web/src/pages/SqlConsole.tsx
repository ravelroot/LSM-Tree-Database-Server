import { useState } from 'react'
import type { KeyboardEvent } from 'react'
import { api, errorMessage } from '../api'
import { Badge, Button, Empty, Panel } from '../components/ui'
import { cell, ms, num } from '../format'
import { useLocalList, useSession } from '../state'
import type { SQLResult } from '../types'

const examples: [string, string][] = [
  ['Tabelas', 'SHOW TABLES'],
  ['Estrutura', 'DESCRIBE pedidos'],
  ['Pedidos por status', 'SELECT status, COUNT(*) AS pedidos, SUM(total) AS receita FROM pedidos GROUP BY status ORDER BY receita DESC'],
  ['Clientes por cidade', "SELECT cidade, COUNT(*) AS clientes FROM clientes WHERE ativo = TRUE GROUP BY cidade ORDER BY clientes DESC LIMIT 5"],
  ['Busca por índice', "EXPLAIN SELECT * FROM pedidos WHERE status = 'pago'"],
  ['Produtos com pouco estoque', 'SELECT sku, nome, estoque FROM produtos WHERE estoque < 20 ORDER BY estoque'],
]

export function SqlConsole() {
  const { can } = useSession()
  const [query, setQuery] = useState(examples[2][1])
  const [result, setResult] = useState<SQLResult | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [history, remember] = useLocalList('arca.sql.historico', 30)

  const run = async (q = query) => {
    const text = q.trim()
    if (!text) return
    setBusy(true)
    setError(null)
    try {
      const r = await api.sql(text)
      setResult(r)
      remember(text)
    } catch (e) {
      setError(errorMessage(e))
      setResult(null)
    } finally {
      setBusy(false)
    }
  }

  const onKey = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      e.preventDefault()
      run()
    }
  }

  return (
    <div className="sql-layout">
      <div className="stack">
        <Panel
          title="Consulta"
          actions={
            <>
              {!can('operator') ? <Badge>somente leitura</Badge> : null}
              <Button variant="primary" icon="play" busy={busy} onClick={() => run()}>
                Executar
              </Button>
            </>
          }
        >
          <textarea
            className="editor mono"
            aria-label="Consulta SQL"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={onKey}
            rows={7}
            spellCheck={false}
          />
          <div className="editor-foot">
            <span>Ctrl + Enter executa</span>
            <span className="spacer" />
            {examples.map(([label, q]) => (
              <button key={label} type="button" className="chip" onClick={() => setQuery(q)}>
                {label}
              </button>
            ))}
          </div>
        </Panel>

        {error ? (
          <div className="error-box sql-error" role="alert">
            <strong>Erro</strong>
            <span>{error}</span>
          </div>
        ) : null}

        {result ? (
          result.kind === 'rows' ? (
            <Panel
              flush
              title={
                <>
                  Resultado <span className="muted">· {num(result.rows.length)} {result.rows.length === 1 ? 'linha' : 'linhas'} em {ms(result.ms)}</span>
                </>
              }
              actions={result.plan ? <span className="plan mono" title="Plano de execução">{result.plan}</span> : undefined}
            >
              {result.rows.length === 0 ? (
                <Empty title="A consulta não retornou linhas" />
              ) : (
                <div className="table-wrap table-tall">
                  <table className="grid">
                    <thead>
                      <tr>
                        {result.columns.map((c, i) => (
                          <th key={i} className={result.types[i] === 'INT' || result.types[i] === 'REAL' ? 'num' : undefined}>
                            {c}
                            <small>{result.types[i]}</small>
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {result.rows.map((r, ri) => (
                        <tr key={ri}>
                          {r.map((v, i) => (
                            <td key={i} className={`${result.types[i] === 'INT' || result.types[i] === 'REAL' ? 'num' : ''}${v === null ? ' null' : ''}`}>
                              {cell(v)}
                            </td>
                          ))}
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </Panel>
          ) : (
            <Panel>
              <div className="sql-done">
                <Badge tone="ok">{result.tag}</Badge>
                <span>
                  {num(result.affected)} {result.affected === 1 ? 'linha afetada' : 'linhas afetadas'} em {ms(result.ms)}
                </span>
              </div>
            </Panel>
          )
        ) : null}
      </div>

      <Panel title="Histórico" className="sql-history" flush>
        {history.length === 0 ? (
          <Empty title="Nada executado ainda" />
        ) : (
          <ul className="history">
            {history.map((h) => (
              <li key={h}>
                <button type="button" className="mono" onClick={() => setQuery(h)} title={h}>
                  {h}
                </button>
              </li>
            ))}
          </ul>
        )}
      </Panel>
    </div>
  )
}
