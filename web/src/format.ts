const nf = new Intl.NumberFormat('pt-BR')
const nf1 = new Intl.NumberFormat('pt-BR', { maximumFractionDigits: 1 })
const nf2 = new Intl.NumberFormat('pt-BR', { maximumFractionDigits: 2 })
const dtf = new Intl.DateTimeFormat('pt-BR', { dateStyle: 'short', timeStyle: 'medium' })
const tf = new Intl.DateTimeFormat('pt-BR', { timeStyle: 'medium' })

export function num(n: number | null | undefined): string {
  if (n === null || n === undefined || Number.isNaN(n)) return '—'
  return nf.format(n)
}

export function dec(n: number | null | undefined, digits = 1): string {
  if (n === null || n === undefined || Number.isNaN(n)) return '—'
  return digits === 2 ? nf2.format(n) : nf1.format(n)
}

export function pct(ratio: number | null | undefined): string {
  if (ratio === null || ratio === undefined || Number.isNaN(ratio)) return '—'
  return nf1.format(ratio * 100) + '%'
}

const units = ['B', 'KB', 'MB', 'GB', 'TB']

export function bytes(n: number | null | undefined): string {
  if (n === null || n === undefined || Number.isNaN(n)) return '—'
  let v = Math.abs(n)
  let i = 0
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024
    i++
  }
  const s = i === 0 ? nf.format(v) : v >= 100 ? nf.format(Math.round(v)) : nf1.format(v)
  return (n < 0 ? '-' : '') + s + ' ' + units[i]
}

export function duration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return '—'
  const s = Math.floor(seconds)
  if (s < 60) return `${s} s`
  const m = Math.floor(s / 60)
  if (m < 60) return `${m} min ${s % 60 ? (s % 60) + ' s' : ''}`.trim()
  const h = Math.floor(m / 60)
  if (h < 48) return `${h} h ${m % 60 ? (m % 60) + ' min' : ''}`.trim()
  const d = Math.floor(h / 24)
  return `${d} d ${h % 24 ? (h % 24) + ' h' : ''}`.trim()
}

export function ttl(ms: number): string {
  if (ms === -1) return 'sem TTL'
  if (ms === -2) return 'expirada'
  if (ms < 1000) return `${ms} ms`
  return duration(ms / 1000)
}

export function dateTime(ms: number): string {
  if (!ms) return '—'
  return dtf.format(new Date(ms))
}

export function time(ms: number): string {
  return tf.format(new Date(ms))
}

export function ago(ms: number): string {
  const diff = (Date.now() - ms) / 1000
  if (diff < 5) return 'agora'
  if (diff < 60) return `há ${Math.floor(diff)} s`
  if (diff < 3600) return `há ${Math.floor(diff / 60)} min`
  if (diff < 86400) return `há ${Math.floor(diff / 3600)} h`
  return dateTime(ms)
}

export function micros(us: number): string {
  if (us < 1000) return `${us} µs`
  if (us < 1_000_000) return `${dec(us / 1000)} ms`
  return `${dec(us / 1_000_000, 2)} s`
}

export function ms(v: number): string {
  if (v < 1) return `${dec(v, 2)} ms`
  if (v < 1000) return `${dec(v)} ms`
  return `${dec(v / 1000, 2)} s`
}

export function cell(v: unknown): string {
  if (v === null || v === undefined) return 'NULL'
  if (typeof v === 'boolean') return v ? 'TRUE' : 'FALSE'
  return String(v)
}

export function looksLikeTimestamp(column: string, type: string, v: unknown): boolean {
  if (type !== 'INT' || typeof v !== 'number') return false
  if (v < 946684800000 || v > 4102444800000) return false
  return /(_em|_at|_ate|_ms|^data|^quando|created|updated|criado|atualizado)/i.test(column)
}

export const roleLabel: Record<string, string> = {
  admin: 'Administrador',
  operator: 'Operador',
  reader: 'Leitura',
}
