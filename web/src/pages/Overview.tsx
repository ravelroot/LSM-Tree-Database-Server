import { api } from '../api'
import { LineChart, Meter, Sparkline } from '../components/Charts'
import { LevelBars } from '../components/Levels'
import { Badge, Empty, ErrorBox, KV, Loading, Panel, Stat } from '../components/ui'
import { bytes, dec, duration, ms, num, pct, ago } from '../format'
import { useAsync, useMetrics } from '../state'

export function Overview() {
  const { data, error, reload } = useAsync(() => api.overview(), [], 5000)
  const { history, latest } = useMetrics()

  if (error && !data) return <ErrorBox message={error} onRetry={reload} />
  if (!data) return <Loading />

  const e = data.engine
  const ops = latest?.ops_per_sec ?? data.commands.ops_per_sec
  const hitRatio = latest ? latest.cache_hit_ratio : e.block_cache_hits + e.block_cache_misses > 0 ? e.block_cache_hits / (e.block_cache_hits + e.block_cache_misses) : 0
  const bloomRatio = e.bloom_checks > 0 ? e.bloom_useful / e.bloom_checks : 0
  const keyspaceRatio = data.commands.keyspace_hits + data.commands.keyspace_misses > 0 ? data.commands.keyspace_hits / (data.commands.keyspace_hits + data.commands.keyspace_misses) : 0

  return (
    <div className="page-grid">
      <div className="server-strip">
        <span>
          <Badge tone="accent">v{data.server.version}</Badge>
        </span>
        <span>porta <b>{data.server.port}</b></span>
        <span>no ar há <b>{duration(data.server.uptime_s)}</b></span>
        <span>
          fsync <b>{data.server.appendfsync}</b>
        </span>
        <span>
          threads <b>{data.server.io_threads}</b> de E/S · <b>{data.server.worker_threads}</b> de trabalho
        </span>
        <span className="mono strip-dir" title={data.server.dir}>{data.server.dir}</span>
        {e.compacting ? <Badge tone="warn">compactando</Badge> : null}
      </div>

      <div className="stats-row">
        <Stat label="Operações por segundo" value={dec(ops)} hint={<Sparkline values={history.map((m) => m.ops_per_sec)} />} />
        <Stat label="Clientes conectados" value={num(latest?.clients ?? data.clients.connected)} hint={`${num(data.clients.total)} conexões desde a partida`} />
        <Stat label="Chaves e linhas (estimativa)" value={num(e.estimated_keys)} hint={`${num(data.tables)} ${data.tables === 1 ? 'tabela' : 'tabelas'}`} />
        <Stat label="Dados em SSTables" value={bytes(latest?.sst_bytes ?? e.sst_bytes)} hint={`${num(latest?.sst_files ?? e.sst_files)} arquivos`} />
        <Stat
          label="Amplificação de escrita"
          value={`${dec(latest?.write_amplification ?? e.write_amplification, 2)}×`}
          hint="bytes gravados em disco por byte do usuário"
          tone={(latest?.write_amplification ?? e.write_amplification) > 10 ? 'warn' : undefined}
        />
        <Stat label="Acerto do cache de blocos" value={pct(hitRatio)} hint={<Meter value={e.block_cache_bytes} max={e.block_cache_capacity} label="Uso do cache" />} />
      </div>

      <Panel title="Leituras e escritas por segundo" className="span-2">
        <LineChart
          series={[
            { name: 'Leituras', values: history.map((m) => m.reads_per_sec), tone: 'a' },
            { name: 'Escritas', values: history.map((m) => m.writes_per_sec), tone: 'b' },
          ]}
          format={(v) => dec(v)}
        />
      </Panel>

      <Panel title="Memtable e memtable imutável">
        <LineChart
          height={150}
          series={[
            { name: 'Memtable', values: history.map((m) => m.memtable_bytes), tone: 'c' },
            { name: 'Imutável', values: history.map((m) => m.immutable_bytes), tone: 'b' },
          ]}
          format={(v) => bytes(v)}
        />
        <KV
          items={[
            ['Entradas na memtable', num(e.memtable_entries)],
            ['Log de escrita (WAL)', `#${e.wal_number} · ${bytes(e.wal_bytes)}`],
            ['Sequência atual', num(e.last_sequence)],
          ]}
        />
      </Panel>

      <Panel title="Níveis da LSM-Tree">
        <LevelBars levels={data.levels} live={latest?.level_files} />
      </Panel>

      <Panel title="Motor de armazenamento">
        <KV
          items={[
            ['Flushes de memtable', num(e.flushes)],
            ['Compactações', num(e.compactions)],
            ['Lido / gravado em compactações', `${bytes(e.compaction_bytes_read)} / ${bytes(e.compaction_bytes_written)}`],
            ['Bytes do usuário', bytes(e.user_bytes_written)],
            ['Filtro de Bloom evitou leituras', `${pct(bloomRatio)} de ${num(e.bloom_checks)}`],
            ['Travamentos de escrita', num(e.write_stalls)],
            ['Snapshots abertos', num(e.snapshots)],
          ]}
        />
      </Panel>

      <Panel title="Comandos">
        <KV
          items={[
            ['Total processado', num(data.commands.total)],
            ['Leituras / escritas', `${num(data.commands.reads)} / ${num(data.commands.writes)}`],
            ['Acertos no keyspace', `${pct(keyspaceRatio)} (${num(data.commands.keyspace_hits)})`],
            ['Chaves expiradas', num(data.commands.expired_keys)],
            ['Rede (entrada / saída)', `${bytes(data.commands.net_input_bytes)} / ${bytes(data.commands.net_output_bytes)}`],
            ['Conexões recusadas', num(data.clients.rejected)],
          ]}
        />
      </Panel>

      <Panel title="Compactações recentes" className="span-2" flush>
        {data.recent_compactions.length === 0 ? (
          <Empty title="Nenhuma compactação ainda">Elas aparecem aqui quando um nível passa do limite ou quando você compacta pela tela Armazenamento.</Empty>
        ) : (
          <div className="table-wrap">
            <table className="grid">
              <thead>
                <tr>
                  <th>Quando</th>
                  <th>Níveis</th>
                  <th>Motivo</th>
                  <th className="num">Entrada</th>
                  <th className="num">Saída</th>
                  <th className="num">Lido</th>
                  <th className="num">Gravado</th>
                  <th className="num">Duração</th>
                </tr>
              </thead>
              <tbody>
                {data.recent_compactions.slice(0, 8).map((c, i) => (
                  <tr key={i}>
                    <td>{ago(c.at)}</td>
                    <td className="mono">
                      L{c.from_level} → L{c.to_level}
                    </td>
                    <td>
                      <Badge tone={c.reason === 'manual' ? 'accent' : 'neutral'}>{c.reason === 'size' ? 'tamanho' : c.reason}</Badge>
                    </td>
                    <td className="num">{num(c.inputs)}</td>
                    <td className="num">{num(c.outputs)}</td>
                    <td className="num">{bytes(c.bytes_read)}</td>
                    <td className="num">{bytes(c.bytes_written)}</td>
                    <td className="num">{ms(c.ms)}</td>
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
