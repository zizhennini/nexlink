# commit-push.ps1 - 提交并推送，提交信息文件始终放在仓库之外。
#
# 用法:  pwsh -File .tools\commit-push.ps1 -MessageFile <本仓库外的路径> -Force
#
# 为什么需要它: 反复用 `git add -A` + 工作区里的信息文件提交时，信息文件会被
# 一并 stage（本项目已经犯过两次）。这里把信息文件放到 $env:TEMP，并在提交后
# 断言仓库里没有临时文件残留。
param(
    [Parameter(Mandatory = $true)][string]$MessageFile,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$env:GIT_TERMINAL_PROMPT = '0'

if (-not (Test-Path -LiteralPath $MessageFile)) { throw "message file not found: $MessageFile" }
$resolved = (Resolve-Path -LiteralPath $MessageFile).Path
if ($resolved -like "$PWD*") { throw "message file must live OUTSIDE the repo: $resolved" }

# 去掉可能的 BOM，否则提交标题会带一个不可见字符
$b = [System.IO.File]::ReadAllBytes($resolved)
if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) {
    [System.IO.File]::WriteAllBytes($resolved, $b[3..($b.Length - 1)])
}

git add -A

$staged = git diff --cached --name-only
if (-not $staged) { Write-Output 'nothing staged'; exit 0 }
Write-Output "staged ($($staged.Count) files):"
$staged | ForEach-Object { "  $_" }

git commit -F $resolved --quiet

$junk = git ls-files | Select-String 'nxcmsg|commitmsg|\.tmp$'
if ($junk) { throw "temp file leaked into the commit: $junk" }

Write-Output "commit: $(git log -1 --format='%h %s')"

if ($Force) { git push --force-with-lease origin main } else { git push origin main }
if ($LASTEXITCODE -ne 0) { throw "push failed ($LASTEXITCODE)" }

git fetch origin --quiet
$local  = git rev-parse HEAD
$remote = git rev-parse origin/main
Write-Output "local : $local"
Write-Output "remote: $remote"
if ($local -ne $remote) { throw 'local and remote diverge after push' }
Write-Output 'OK: pushed and verified'
