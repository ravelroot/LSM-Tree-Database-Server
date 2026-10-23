$ErrorActionPreference = 'Stop'
$raiz = Split-Path -Parent $PSScriptRoot
$alvo = Join-Path $raiz '.env'
$modelo = Join-Path $raiz '.env.example'
$letras = [char[]]'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'
$bytes = New-Object byte[] 40
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$senha = -join ($bytes | ForEach-Object { $letras[$_ % $letras.Length] })
(Get-Content $modelo) -replace '^ARCA_PASSWORD=.*', "ARCA_PASSWORD=$senha" | Set-Content -Encoding ascii $alvo
Write-Host 'Arquivo .env criado com uma senha aleatória para o servidor Arca.'
