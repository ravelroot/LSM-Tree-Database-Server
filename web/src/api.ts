import type {
  AuditEntry,
  ClientInfo,
  CommandResult,
  ConfigEntry,
  Health,
  KeyPage,
  KeyValue,
  LoginResponse,
  Overview,
  Role,
  RowPage,
  SQLResult,
  SlowEntry,
  Storage,
  Table,
  User,
} from './types'

export class ApiError extends Error {
  status: number
  code: string
  fields: Record<string, string>

  constructor(status: number, code: string, message: string, fields?: Record<string, string>) {
    super(message)
    this.status = status
    this.code = code
    this.fields = fields ?? {}
  }
}

const TOKEN_KEY = 'arca.sessao'

function readToken(): string | null {
  try {
    return window.localStorage.getItem(TOKEN_KEY)
  } catch {
    return null
  }
}

let token: string | null = readToken()
let unauthorizedHandler: (() => void) | null = null

export function getToken(): string | null {
  return token
}

export function setToken(value: string | null): void {
  token = value
  try {
    if (value) {
      window.localStorage.setItem(TOKEN_KEY, value)
    } else {
      window.localStorage.removeItem(TOKEN_KEY)
    }
  } catch {
    return
  }
}

export function onUnauthorized(handler: () => void): void {
  unauthorizedHandler = handler
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { Accept: 'application/json' }
  if (token) headers.Authorization = `Bearer ${token}`
  if (body !== undefined) headers['Content-Type'] = 'application/json'
  let res: Response
  try {
    res = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    })
  } catch {
    throw new ApiError(0, 'network', 'Sem conexão com o gateway. Confira se ele está no ar.')
  }
  if (res.status === 204) return undefined as T
  const text = await res.text()
  let data: unknown = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      data = null
    }
  }
  if (!res.ok) {
    const err = (data ?? {}) as { code?: string; message?: string; fields?: Record<string, string> }
    const apiErr = new ApiError(res.status, err.code ?? 'http_' + res.status, err.message ?? `Erro ${res.status}`, err.fields)
    if (res.status === 401 && path !== '/api/auth/login' && unauthorizedHandler) unauthorizedHandler()
    throw apiErr
  }
  return data as T
}

function qs(params: Record<string, string | number | undefined>): string {
  const sp = new URLSearchParams()
  for (const [k, v] of Object.entries(params)) {
    if (v !== undefined && v !== '') sp.set(k, String(v))
  }
  const s = sp.toString()
  return s ? '?' + s : ''
}

export const api = {
  login: (email: string, password: string) => request<LoginResponse>('POST', '/api/auth/login', { email, password }),
  logout: () => request<void>('POST', '/api/auth/logout'),
  me: () => request<User>('GET', '/api/auth/me'),
  changePassword: (current: string, next: string) => request<void>('PUT', '/api/auth/me/password', { current, new: next }),
  overview: () => request<Overview>('GET', '/api/overview'),
  keys: (match: string, cursor: string, count: number) => request<KeyPage>('GET', '/api/keys' + qs({ match, cursor, count })),
  keyGet: (key: string) => request<KeyValue>('GET', '/api/keys/value' + qs({ key })),
  keyPut: (key: string, value: string, ttl_s?: number) => request<void>('PUT', '/api/keys/value', { key, value, ttl_s }),
  keyDelete: (key: string) => request<void>('DELETE', '/api/keys/value' + qs({ key })),
  keyExpire: (key: string, ttl_s: number | null) => request<void>('POST', '/api/keys/expire', { key, ttl_s }),
  tables: () => request<Table[]>('GET', '/api/tables'),
  rows: (name: string, p: { limit: number; offset: number; order?: string; dir?: string; filter?: string }) =>
    request<RowPage>('GET', `/api/tables/${encodeURIComponent(name)}/rows` + qs(p)),
  rowInsert: (name: string, values: Record<string, unknown>) =>
    request<{ message: string }>('POST', `/api/tables/${encodeURIComponent(name)}/rows`, { values }),
  rowUpdate: (name: string, key: unknown, values: Record<string, unknown>) =>
    request<void>('PUT', `/api/tables/${encodeURIComponent(name)}/rows`, { key, values }),
  rowDelete: (name: string, key: unknown) =>
    request<void>('DELETE', `/api/tables/${encodeURIComponent(name)}/rows` + qs({ key: String(key) })),
  sql: (query: string) => request<SQLResult>('POST', '/api/sql', { query }),
  command: (args: string[]) => request<CommandResult>('POST', '/api/command', { args }),
  storage: () => request<Storage>('GET', '/api/storage'),
  flush: () => request<{ message: string }>('POST', '/api/storage/flush'),
  compact: () => request<{ message: string }>('POST', '/api/storage/compact'),
  checkpoint: () => request<{ message: string }>('POST', '/api/storage/checkpoint'),
  clients: () => request<ClientInfo[]>('GET', '/api/clients'),
  slowlog: (n = 50) => request<SlowEntry[]>('GET', '/api/slowlog' + qs({ n })),
  config: () => request<ConfigEntry[]>('GET', '/api/config'),
  configSet: (name: string, value: string) => request<void>('PUT', '/api/config', { name, value }),
  users: () => request<User[]>('GET', '/api/users'),
  userCreate: (u: { email: string; name: string; role: Role; password: string }) => request<User>('POST', '/api/users', u),
  userUpdate: (email: string, patch: { name?: string; role?: Role; password?: string; unlock?: boolean }) =>
    request<User>('PATCH', `/api/users/${encodeURIComponent(email)}`, patch),
  userDelete: (email: string) => request<void>('DELETE', `/api/users/${encodeURIComponent(email)}`),
  audit: (limit = 200) => request<AuditEntry[]>('GET', '/api/audit' + qs({ limit })),
  health: () => request<Health>('GET', '/api/health'),
}

export function errorMessage(err: unknown): string {
  if (err instanceof ApiError) return err.message
  if (err instanceof Error) return err.message
  return 'Algo deu errado.'
}
