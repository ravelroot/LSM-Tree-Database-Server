import { api } from '../api'
import { Badge, Empty, ErrorBox, Loading, Panel } from '../components/ui'
import { dateTime, duration, micros, num } from '../format'
import { useAsync } from '../state'

export function Clients() {
  const clients = useAsync(() => api.clients(), [], 3000)
  const slow = useAsync(() => api.slowlog(50), [], 10000)

  return (
    <div className="stack">
      <Panel
        flush
        title={
          <>
            Conexões abertas <span className="muted">· {clients.data ? num(clients.data.length) : '…'}</span>
          </>
        }
      >
        {clients.error && !clients.data ? (
          <ErrorBox message={clients.error} onRetry={clients.reload} />
        ) : !clients.data ? (
          <Loading />
        ) : clients.data.length === 0 ? (
          <Empty title="Nenhum cliente conectado" />
        ) : (
          <div className="table-wrap">
            <table className="grid">
              <thead>
                <tr>
                  <th className="num">ID</th>
                  <th>Endereço</th>
                  <th>Nome</th>
                  <th className="num">Conectado há</th>
                  <th className="num">Ocioso</th>
                  <th>Último comando</th>
                </tr>
              </thead>
              <tbody>
                {clients.data.map((c) => (
                  <tr key={c.id}>
                    <td className="num">{c.id}</td>
                    <td className="mono">{c.addr}</td>
                    <td>{c.name ? <Badge tone="accent">{c.name}</Badge> : <span className="muted">—</span>}</td>
                    <td className="num">{duration(c.age_s)}</td>
                    <td className="num">{c.idle_s === 0 ? <Badge tone="ok">ativo</Badge> : duration(c.idle_s)}</td>
                    <td className="mono">{c.cmd || '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      <Panel flush title="Comandos lentos">
        {slow.error && !slow.data ? (
          <ErrorBox message={slow.error} onRetry={slow.reload} />
        ) : !slow.data ? (
          <Loading />
        ) : slow.data.length === 0 ? (
          <Empty title="Nenhum comando lento registrado">O limite fica no parâmetro slowlog-log-slower-than (em microssegundos).</Empty>
        ) : (
          <div className="table-wrap">
            <table className="grid">
              <thead>
                <tr>
                  <th className="num">#</th>
                  <th>Quando</th>
                  <th className="num">Duração</th>
                  <th>Comando</th>
                  <th>Cliente</th>
                </tr>
              </thead>
              <tbody>
                {slow.data.map((s) => (
                  <tr key={s.id}>
                    <td className="num">{s.id}</td>
                    <td>{dateTime(s.at)}</td>
                    <td className="num">
                      <Badge tone={s.micros > 100000 ? 'error' : s.micros > 20000 ? 'warn' : 'neutral'}>{micros(s.micros)}</Badge>
                    </td>
                    <td className="mono cmd-cell" title={s.args.join(' ')}>
                      {s.args.join(' ')}
                    </td>
                    <td className="mono">{s.addr || '—'}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>
    </div>
  )
}
