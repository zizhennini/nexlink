# release_github.ps1 - 把当前固件发布为 GitHub Release 并上传 nexlink.bin
# 用法:
#   $env:GITHUB_TOKEN = "<token>"   # 不设则自动从 Windows 凭据管理器(GCM)读取
#   powershell -File tools\release_github.ps1 -Tag v1.0.0 -NotesFile <notes.md> [-BinPath build\nexlink.bin]
# 说明: 脚本强制直连 GitHub(禁系统代理), 与 git push 的 -c http.proxy= 行为一致。
param(
    [Parameter(Mandatory=$true)][string]$Tag,
    [Parameter(Mandatory=$true)][string]$NotesFile,
    [string]$Repo = "zizhennini/nexlink",
    [string]$BinPath = "",
    [switch]$Prerelease
)
$ErrorActionPreference = "Stop"

# 0) token: 优先环境变量, 否则从 Windows 凭据管理器(GCM / CredRead)读取(不回显)
function Get-CredentialManagerToken {
    param([string]$Target = "git:https://github.com")
    $src = @"
using System;
using System.Runtime.InteropServices;
public class CredMan {
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool CredRead(string target, int type, int flags, out IntPtr cred);
  [DllImport("advapi32.dll")]
  static extern void CredFree(IntPtr cred);
  [StructLayout(LayoutKind.Sequential)]
  public struct CREDENTIAL {
    public int Flags; public int Type;
    public IntPtr TargetName; public IntPtr Comment;
    public long LastWritten; public int CredentialBlobSize;
    public IntPtr CredentialBlob; public int Persist;
    public int AttributeCount; public IntPtr Attributes;
    public IntPtr TargetAlias; public IntPtr UserName;
  }
  public static string Get(string target) {
    IntPtr p;
    if (!CredRead(target, 1, 0, out p)) return null;
    try {
      CREDENTIAL c = (CREDENTIAL)Marshal.PtrToStructure(p, typeof(CREDENTIAL));
      if (c.CredentialBlobSize <= 0) return null;
      byte[] b = new byte[c.CredentialBlobSize];
      Marshal.Copy(c.CredentialBlob, b, 0, c.CredentialBlobSize);
      return System.Text.Encoding.Unicode.GetString(b);
    } finally { CredFree(p); }
  }
}
"@
    Add-Type -TypeDefinition $src -ErrorAction SilentlyContinue
    return [CredMan]::Get($Target)
}
if (-not $env:GITHUB_TOKEN) {
    $env:GITHUB_TOKEN = Get-CredentialManagerToken
}
if (-not $env:GITHUB_TOKEN) {
    $c = "protocol=https`nhost=github.com`n" | git credential fill
    $m = [regex]::Match($c, '(?m)^password=(.+)$')
    if ($m.Success) { $env:GITHUB_TOKEN = $m.Groups[1].Value }
}
if (-not $env:GITHUB_TOKEN) { throw "no github token (set env GITHUB_TOKEN)" }

# 1) 禁系统代理(Windows .NET 默认走 IE 代理, 会撞上没开的 127.0.0.1:7897)
[System.Net.WebRequest]::DefaultWebProxy = $null
$headers = @{ Authorization = "token $env:GITHUB_TOKEN"; Accept = "application/vnd.github+json" }

# 2) 创建 release
$body = Get-Content -Raw -Encoding UTF8 $NotesFile
$json = @{
    tag_name = $Tag
    name     = "NexLink $Tag"
    body     = $body
    draft    = $false
    prerelease = [bool]$Prerelease
} | ConvertTo-Json
$rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases" -Method Post -Headers $headers -ContentType "application/json" -Body $json
Write-Host "RELEASE_CREATED id=$($rel.id) tag=$($rel.tag_name)"
Write-Host "URL=$($rel.html_url)"

# 3) 上传固件资产(可选)
if ($BinPath) {
    if (-not (Test-Path $BinPath)) { throw "bin not found: $BinPath" }
    $name = [uri]::EscapeDataString((Split-Path $BinPath -Leaf))
    $upload = ($rel.upload_url -replace '\{\?name,label\}', "?name=$name")
    $asset = Invoke-RestMethod -Uri $upload -Method Post -Headers $headers -ContentType "application/octet-stream" -InFile $BinPath
    Write-Host "ASSET_UPLOADED name=$($asset.name) size=$($asset.size) url=$($asset.browser_download_url)"
}
