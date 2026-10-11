# gen_ascii_js.ps1 - export main/display/font8x16.h (96 glyphs, 16 bytes each)
# to tools/ascii_font.js for the OLED UI simulator.
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content (Join-Path $root "main\display\font8x16.h") -Raw
$matches = [regex]::Matches($h, '\{((?:0x[0-9A-Fa-f]{2},){15}0x[0-9A-Fa-f]{2})\}')
if ($matches.Count -ne 96) { throw "expected 96 glyphs, got $($matches.Count)" }

$l = New-Object System.Collections.Generic.List[string]
$l.Add("// Auto-generated from main/display/font8x16.h (IBM PC VGA 8x16).")
$l.Add("var ASCIIFONT = [")
foreach ($m in $matches) {
    $l.Add('  [' + $m.Groups[1].Value + '],')
}
$l.Add("];")
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot "ascii_font.js"),
    ($l -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote tools/ascii_font.js ($($matches.Count) glyphs)"
