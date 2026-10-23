import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { errorMessage, getToken } from './api'
import type { Metrics, Role, User } from './types'

const rank: Record<Role, number> = { reader: 1, operator: 2, admin: 3 }

interface SessionValue {
  user: User
  can: (role: Role) => boolean
  logout: () => void
  refreshUser: (u: User) => void
}

export const SessionContext = createContext<SessionValue | null>(null)

export function useSession(): SessionValue {
  const v = useContext(SessionContext)
  if (!v) throw new Error('sessão ausente')
  return v
}

export function canRole(user: User | null, role: Role): boolean {
  if (!user) return false
  return rank[user.role] >= rank[role]
}

type ToastKind = 'ok' | 'error' | 'info'

interface Toast {
  id: number
  kind: ToastKind
  text: string
}

interface ToastValue {
  push: (kind: ToastKind, text: string) => void
  ok: (text: string) => void
  error: (err: unknown) => void
}

const ToastContext = createContext<ToastValue | null>(null)

export function useToast(): ToastValue {
  const v = useContext(ToastContext)
  if (!v) throw new Error('toast ausente')
  return v
}

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([])
  const seq = useRef(0)
  const push = useCallback((kind: ToastKind, text: string) => {
    const id = ++seq.current
    setToasts((t) => [...t.slice(-3), { id, kind, text }])
    window.setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), kind === 'error' ? 7000 : 4000)
  }, [])
  const value = useMemo<ToastValue>(
    () => ({
      push,
      ok: (text) => push('ok', text),
      error: (err) => push('error', errorMessage(err)),
    }),
    [push],
  )
  return (
    <ToastContext.Provider value={value}>
      {children}
      <div className="toasts" role="status" aria-live="polite">
        {toasts.map((t) => (
          <div key={t.id} className={`toast toast-${t.kind}`}>
            {t.text}
          </div>
        ))}
      </div>
    </ToastContext.Provider>
  )
}

export type StreamState = 'connecting' | 'live' | 'down' | 'offline'

interface MetricsValue {
  history: Metrics[]
  latest: Metrics | null
  state: StreamState
}

const MetricsContext = createContext<MetricsValue>({ history: [], latest: null, state: 'connecting' })

export function useMetrics(): MetricsValue {
  return useContext(MetricsContext)
}

const HISTORY = 120

export function MetricsProvider({ children, onExpired }: { children: ReactNode; onExpired: () => void }) {
  const [history, setHistory] = useState<Metrics[]>([])
  const [state, setState] = useState<StreamState>('connecting')
  const expiredRef = useRef(onExpired)
  expiredRef.current = onExpired

  useEffect(() => {
    const token = getToken()
    if (!token) return
    let closed = false
    const es = new EventSource('/api/events?token=' + encodeURIComponent(token))
    es.addEventListener('metrics', (ev) => {
      try {
        const m = JSON.parse((ev as MessageEvent<string>).data) as Metrics
        setHistory((h) => (h.length >= HISTORY ? [...h.slice(h.length - HISTORY + 1), m] : [...h, m]))
        setState('live')
      } catch {
        return
      }
    })
    es.addEventListener('unavailable', () => setState('down'))
    es.addEventListener('expired', () => {
      es.close()
      if (!closed) expiredRef.current()
    })
    es.onerror = () => {
      if (!closed) setState(es.readyState === EventSource.CLOSED ? 'offline' : 'connecting')
    }
    return () => {
      closed = true
      es.close()
    }
  }, [])

  const value = useMemo(() => ({ history, latest: history.length ? history[history.length - 1] : null, state }), [history, state])
  return <MetricsContext.Provider value={value}>{children}</MetricsContext.Provider>
}

export function useAsync<T>(fn: () => Promise<T>, deps: unknown[], intervalMs?: number) {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const fnRef = useRef(fn)
  fnRef.current = fn
  const alive = useRef(true)

  const reload = useCallback(async () => {
    setLoading(true)
    try {
      const v = await fnRef.current()
      if (alive.current) {
        setData(v)
        setError(null)
      }
    } catch (e) {
      if (alive.current) setError(errorMessage(e))
    } finally {
      if (alive.current) setLoading(false)
    }
  }, [])

  useEffect(() => {
    alive.current = true
    reload()
    let timer: number | undefined
    if (intervalMs) {
      timer = window.setInterval(() => {
        if (document.visibilityState === 'visible') reload()
      }, intervalMs)
    }
    return () => {
      alive.current = false
      if (timer) window.clearInterval(timer)
    }
  }, deps)

  return { data, error, loading, reload, setData }
}

const THEME_KEY = 'arca.tema'

export type Theme = 'light' | 'dark'

function systemTheme(): Theme {
  return window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function initialTheme(): Theme {
  try {
    const saved = window.localStorage.getItem(THEME_KEY)
    if (saved === 'light' || saved === 'dark') return saved
  } catch {
    return systemTheme()
  }
  return systemTheme()
}

export function applyTheme(theme: Theme): void {
  document.documentElement.setAttribute('data-theme', theme)
}

export function useTheme(): [Theme, () => void] {
  const [theme, setTheme] = useState<Theme>(() => initialTheme())
  useEffect(() => applyTheme(theme), [theme])
  const toggle = useCallback(() => {
    setTheme((t) => {
      const next = t === 'dark' ? 'light' : 'dark'
      try {
        window.localStorage.setItem(THEME_KEY, next)
      } catch {
        return next
      }
      return next
    })
  }, [])
  return [theme, toggle]
}

export function useElementWidth<T extends HTMLElement>(): [React.RefObject<T>, number] {
  const ref = useRef<T>(null)
  const [width, setWidth] = useState(0)
  useEffect(() => {
    const el = ref.current
    if (!el) return
    setWidth(el.clientWidth)
    const ro = new ResizeObserver((entries) => {
      for (const e of entries) setWidth(Math.floor(e.contentRect.width))
    })
    ro.observe(el)
    return () => ro.disconnect()
  }, [])
  return [ref, width]
}

export function useLocalList(key: string, max: number): [string[], (v: string) => void] {
  const [list, setList] = useState<string[]>(() => {
    try {
      const raw = window.localStorage.getItem(key)
      const parsed = raw ? (JSON.parse(raw) as unknown) : []
      return Array.isArray(parsed) ? parsed.filter((x): x is string => typeof x === 'string').slice(0, max) : []
    } catch {
      return []
    }
  })
  const add = useCallback(
    (v: string) => {
      setList((l) => {
        const next = [v, ...l.filter((x) => x !== v)].slice(0, max)
        try {
          window.localStorage.setItem(key, JSON.stringify(next))
        } catch {
          return next
        }
        return next
      })
    },
    [key, max],
  )
  return [list, add]
}
