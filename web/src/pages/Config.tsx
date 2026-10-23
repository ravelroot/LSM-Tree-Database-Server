import { useMemo, useState } from 'react'
import { api } from '../api'
import { Button, Empty, ErrorBox, Loading, Panel } from '../components/ui'
import { useAsync, useToast } from '../state'

export function Config() {
  const toast = useToast()
  const { data, error, reload, setData } = useAsync(() => api.config(), [])
  const [edits, setEdits] = useState<Record<string, string>>({})
  const [saving, setSaving] = useState<string | null>(null)
  const [filter, setFilter] = useState('')

  const rows = useMemo(() => (data ?? []).filter((c) => c.name.includes(filter.trim().toLowerCase())), [data, filter])

  const save = async (name: string) => {
    const value = edits[name]
    if (value === undefined) return
    setSaving(name)
    try {
      await api.configSet(name, value)
      toast.ok(`${name} atualizado.`)
      setData((d) => (d ? d.map((c) => (c.name === name ? { ...c, value } : c)) : d))
      setEdits((e) => {
        const n = { ...e }
        delete n[name]
        return n
      })
    } catch (e) {
      toast.error(e)
    } finally {
      setSaving(null)
    }
  }

  if (error && !data) return <ErrorBox message={error} onRetry={reload} />
  if (!data) return <Loading />

  return (
    <div className="stack">
      <div className="toolbar">
        <div className="search">
          <input aria-label="Filtrar parâmetros" placeholder="Filtrar parâmetros" value={filter} onChange={(e) => setFilter(e.target.value)} />
        </div>
        <span className="toolbar-hint">Alterações valem na hora, mas não são gravadas no arquivo de configuração. Parâmetros que só mudam com reinício são recusados pelo servidor.</span>
      </div>
      <Panel flush>
        {rows.length === 0 ? (
          <Empty title="Nenhum parâmetro encontrado" />
        ) : (
          <div className="table-wrap">
            <table className="grid config-grid">
              <thead>
                <tr>
                  <th>Parâmetro</th>
                  <th>Valor</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {rows.map((c) => {
                  const dirty = edits[c.name] !== undefined && edits[c.name] !== c.value
                  return (
                    <tr key={c.name}>
                      <td className="mono">{c.name}</td>
                      <td>
                        <input
                          aria-label={c.name}
                          className="mono cell-input"
                          value={edits[c.name] ?? c.value}
                          onChange={(e) => setEdits({ ...edits, [c.name]: e.target.value })}
                          onKeyDown={(e) => e.key === 'Enter' && dirty && save(c.name)}
                        />
                      </td>
                      <td className="actions">
                        {dirty ? (
                          <Button small variant="primary" busy={saving === c.name} onClick={() => save(c.name)}>
                            Aplicar
                          </Button>
                        ) : null}
                      </td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
        )}
      </Panel>
    </div>
  )
}
