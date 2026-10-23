import { useState } from 'react'
import type { FormEvent } from 'react'
import { ApiError, api } from '../api'
import { Badge, Button, Field, KV, Panel } from '../components/ui'
import { dateTime, roleLabel } from '../format'
import { useSession, useToast } from '../state'
import type { Theme } from '../state'

const abilities: Record<string, string[]> = {
  reader: ['Ver visão geral, armazenamento e clientes', 'Listar chaves e tabelas', 'Executar SELECT, EXPLAIN, SHOW e DESCRIBE'],
  operator: ['Tudo de Leitura', 'Criar, alterar e apagar chaves e linhas', 'Executar qualquer SQL', 'Gravar memtable, compactar e criar checkpoints'],
  admin: ['Tudo de Operador', 'Console de comandos crus', 'Configuração em tempo de execução', 'Usuários e auditoria'],
}

export function Account({ theme, toggleTheme }: { theme: Theme; toggleTheme: () => void }) {
  const { user } = useSession()
  const toast = useToast()
  const [current, setCurrent] = useState('')
  const [next, setNext] = useState('')
  const [confirm, setConfirm] = useState('')
  const [busy, setBusy] = useState(false)
  const [fieldErr, setFieldErr] = useState<Record<string, string>>({})

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (next !== confirm) {
      setFieldErr({ confirm: 'As senhas não conferem.' })
      return
    }
    setBusy(true)
    setFieldErr({})
    try {
      await api.changePassword(current, next)
      toast.ok('Senha alterada.')
      setCurrent('')
      setNext('')
      setConfirm('')
    } catch (err) {
      if (err instanceof ApiError) setFieldErr(err.fields)
      toast.error(err)
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="account-layout">
      <Panel title="Seus dados">
        <KV
          items={[
            ['Nome', user.name],
            ['E-mail', <span className="mono">{user.email}</span>],
            ['Papel', <Badge tone="accent">{roleLabel[user.role]}</Badge>],
            ['Conta criada em', dateTime(user.created_at ?? 0)],
          ]}
        />
        <h3 className="sub">O que você pode fazer</h3>
        <ul className="ticks">
          {abilities[user.role].map((a) => (
            <li key={a}>{a}</li>
          ))}
        </ul>
        <h3 className="sub">Aparência</h3>
        <Button icon={theme === 'dark' ? 'sun' : 'moon'} onClick={toggleTheme}>
          {theme === 'dark' ? 'Usar tema claro' : 'Usar tema escuro'}
        </Button>
      </Panel>
      <Panel title="Trocar senha">
        <form className="form" onSubmit={submit}>
          <Field label="Senha atual" htmlFor="pw-current" error={fieldErr.current}>
            <input id="pw-current" type="password" autoComplete="current-password" value={current} onChange={(e) => setCurrent(e.target.value)} required />
          </Field>
          <Field label="Nova senha" htmlFor="pw-new" error={fieldErr.new} hint="Mínimo de 8 caracteres, com letras e números.">
            <input id="pw-new" type="password" autoComplete="new-password" value={next} onChange={(e) => setNext(e.target.value)} required />
          </Field>
          <Field label="Repita a nova senha" htmlFor="pw-confirm" error={fieldErr.confirm}>
            <input id="pw-confirm" type="password" autoComplete="new-password" value={confirm} onChange={(e) => setConfirm(e.target.value)} required />
          </Field>
          <div>
            <Button type="submit" variant="primary" busy={busy}>
              Salvar nova senha
            </Button>
          </div>
        </form>
      </Panel>
    </div>
  )
}
