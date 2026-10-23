import { useCallback, useEffect, useMemo, useState } from 'react'
import { api, getToken, onUnauthorized, setToken } from './api'
import { Icon } from './components/Icons'
import type { IconName } from './components/Icons'
import { Logo } from './components/Logo'
import { IconButton, Loading } from './components/ui'
import { roleLabel } from './format'
import { Account } from './pages/Account'
import { Clients } from './pages/Clients'
import { Commands } from './pages/Commands'
import { Config } from './pages/Config'
import { Keys } from './pages/Keys'
import { Login } from './pages/Login'
import { Overview } from './pages/Overview'
import { SqlConsole } from './pages/SqlConsole'
import { Storage } from './pages/Storage'
import { Tables } from './pages/Tables'
import { Users } from './pages/Users'
import { MetricsProvider, SessionContext, canRole, useMetrics, useTheme } from './state'
import type { Theme } from './state'
import type { Role, User } from './types'

type PageId = 'visao-geral' | 'chaves' | 'tabelas' | 'sql' | 'armazenamento' | 'clientes' | 'comandos' | 'configuracao' | 'usuarios' | 'conta'

interface PageDef {
  id: PageId
  label: string
  title: string
  description: string
  icon: IconName
  role: Role
  group: 'Dados' | 'Motor' | 'Administração' | 'Conta'
}

const pages: PageDef[] = [
  { id: 'visao-geral', label: 'Visão geral', title: 'Visão geral', description: 'Tráfego, memória e níveis em tempo real.', icon: 'overview', role: 'reader', group: 'Dados' },
  { id: 'chaves', label: 'Chaves', title: 'Chaves', description: 'Valores do espaço chave-valor, com TTL.', icon: 'keys', role: 'reader', group: 'Dados' },
  { id: 'tabelas', label: 'Tabelas', title: 'Tabelas', description: 'Linhas, colunas e índices das tabelas SQL.', icon: 'tables', role: 'reader', group: 'Dados' },
  { id: 'sql', label: 'Console SQL', title: 'Console SQL', description: 'Consultas e comandos no dialeto SQL do Arca.', icon: 'sql', role: 'reader', group: 'Dados' },
  { id: 'armazenamento', label: 'Armazenamento', title: 'Armazenamento', description: 'Memtable, SSTables por nível e compactações.', icon: 'layers', role: 'reader', group: 'Motor' },
  { id: 'clientes', label: 'Clientes', title: 'Clientes e comandos lentos', description: 'Conexões abertas e o slowlog do servidor.', icon: 'clients', role: 'reader', group: 'Motor' },
  { id: 'comandos', label: 'Comandos', title: 'Comandos', description: 'Console direto do protocolo RESP.', icon: 'terminal', role: 'admin', group: 'Administração' },
  { id: 'configuracao', label: 'Configuração', title: 'Configuração', description: 'Parâmetros do servidor em tempo de execução.', icon: 'settings', role: 'admin', group: 'Administração' },
  { id: 'usuarios', label: 'Usuários', title: 'Usuários e auditoria', description: 'Contas do painel e tudo o que foi feito nelas.', icon: 'users', role: 'admin', group: 'Administração' },
  { id: 'conta', label: 'Minha conta', title: 'Minha conta', description: 'Seus dados, permissões e senha.', icon: 'account', role: 'reader', group: 'Conta' },
]

function pageFromHash(): PageId {
  const h = window.location.hash.replace(/^#\/?/, '')
  return (pages.find((p) => p.id === h)?.id ?? 'visao-geral') as PageId
}

function StreamPill() {
  const { state, latest } = useMetrics()
  const label = state === 'live' ? (latest?.compacting ? 'ao vivo · compactando' : 'ao vivo') : state === 'down' ? 'Arca fora do ar' : state === 'offline' ? 'desconectado' : 'conectando'
  return (
    <span className={`pill pill-${state}`} title="Métricas em tempo real">
      <i />
      {label}
    </span>
  )
}

function Shell({ user, theme, toggleTheme, onLogout, onUser }: { user: User; theme: Theme; toggleTheme: () => void; onLogout: () => void; onUser: (u: User) => void }) {
  const [page, setPage] = useState<PageId>(pageFromHash)
  const [menu, setMenu] = useState(false)

  useEffect(() => {
    const onHash = () => setPage(pageFromHash())
    window.addEventListener('hashchange', onHash)
    return () => window.removeEventListener('hashchange', onHash)
  }, [])

  const allowed = pages.filter((p) => canRole(user, p.role))
  const def = allowed.find((p) => p.id === page) ?? allowed[0]

  useEffect(() => {
    document.title = `${def.title} · Arca`
  }, [def])

  const go = (id: PageId) => {
    window.location.hash = '/' + id
    setMenu(false)
  }

  const session = useMemo(
    () => ({
      user,
      can: (r: Role) => canRole(user, r),
      logout: onLogout,
      refreshUser: onUser,
    }),
    [user, onLogout, onUser],
  )

  const groups = ['Dados', 'Motor', 'Administração', 'Conta'] as const

  return (
    <SessionContext.Provider value={session}>
      <div className={`shell${menu ? ' menu-open' : ''}`}>
        <aside className="side">
          <div className="brand">
            <Logo size={30} />
            <div>
              <strong>Arca</strong>
              <span>LSM-Tree em C++</span>
            </div>
          </div>
          <nav aria-label="Navegação principal">
            {groups.map((g) => {
              const items = allowed.filter((p) => p.group === g)
              if (!items.length) return null
              return (
                <div key={g} className="nav-group">
                  <span className="nav-title">{g}</span>
                  {items.map((p) => (
                    <a key={p.id} href={`#/${p.id}`} className={p.id === def.id ? 'nav-item active' : 'nav-item'} aria-current={p.id === def.id ? 'page' : undefined} onClick={(e) => { e.preventDefault(); go(p.id) }}>
                      <Icon name={p.icon} />
                      <span>{p.label}</span>
                    </a>
                  ))}
                </div>
              )
            })}
          </nav>
          <div className="side-foot">
            <div className="who">
              <span className="avatar" aria-hidden="true">{user.name.trim().charAt(0).toUpperCase() || '?'}</span>
              <div>
                <strong>{user.name}</strong>
                <span>{roleLabel[user.role]}</span>
              </div>
            </div>
            <IconButton icon="logout" label="Sair" onClick={onLogout} />
          </div>
        </aside>
        <div className="side-scrim" onClick={() => setMenu(false)} />
        <div className="main">
          <header className="top">
            <button type="button" className="icon-btn menu-btn" aria-label="Abrir menu" onClick={() => setMenu(true)}>
              <Icon name="menu" />
            </button>
            <div className="top-title">
              <h1>{def.title}</h1>
              <p>{def.description}</p>
            </div>
            <span className="spacer" />
            <StreamPill />
            <IconButton icon={theme === 'dark' ? 'sun' : 'moon'} label={theme === 'dark' ? 'Tema claro' : 'Tema escuro'} onClick={toggleTheme} />
          </header>
          <main className="content" key={def.id}>
            {def.id === 'visao-geral' ? <Overview /> : null}
            {def.id === 'chaves' ? <Keys /> : null}
            {def.id === 'tabelas' ? <Tables /> : null}
            {def.id === 'sql' ? <SqlConsole /> : null}
            {def.id === 'armazenamento' ? <Storage /> : null}
            {def.id === 'clientes' ? <Clients /> : null}
            {def.id === 'comandos' ? <Commands /> : null}
            {def.id === 'configuracao' ? <Config /> : null}
            {def.id === 'usuarios' ? <Users /> : null}
            {def.id === 'conta' ? <Account theme={theme} toggleTheme={toggleTheme} /> : null}
          </main>
        </div>
      </div>
    </SessionContext.Provider>
  )
}

export function App() {
  const [theme, toggleTheme] = useTheme()
  const [user, setUser] = useState<User | null>(null)
  const [checking, setChecking] = useState(() => getToken() !== null)

  const clear = useCallback(() => {
    setToken(null)
    setUser(null)
  }, [])

  useEffect(() => {
    onUnauthorized(clear)
    if (!getToken()) return
    api
      .me()
      .then(setUser)
      .catch(() => clear())
      .finally(() => setChecking(false))
  }, [clear])

  const logout = useCallback(() => {
    api.logout().catch(() => undefined)
    clear()
  }, [clear])

  if (checking) {
    return (
      <div className="boot">
        <Logo size={40} />
        <Loading label="Abrindo o painel…" />
      </div>
    )
  }

  if (!user) return <Login onLogin={setUser} />

  return (
    <MetricsProvider onExpired={clear}>
      <Shell user={user} theme={theme} toggleTheme={toggleTheme} onLogout={logout} onUser={setUser} />
    </MetricsProvider>
  )
}
