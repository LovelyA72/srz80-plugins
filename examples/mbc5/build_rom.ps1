param(
    [string]$Z88dkRoot = 'C:\Software\z88dk'
)

$assembler = Join-Path $Z88dkRoot 'bin\z80asm.exe'
if (-not (Test-Path -LiteralPath $assembler)) {
    throw "z88dk z80asm was not found at $assembler. Pass -Z88dkRoot with your installation path."
}

$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ('srz80-mbc5-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $banks = @()
    foreach ($name in @('bank0', 'bank1', 'bank2')) {
        $output = Join-Path $temporary ($name + '.bin')
        & $assembler -mz80 -b "-o$output" (Join-Path $PSScriptRoot ($name + '.asm'))
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $banks += [System.IO.File]::ReadAllBytes($output)
    }
    [System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'mbc5.rom'), $banks)

    $staticOutput = Join-Path $temporary 'static.bin'
    & $assembler -mz80 -b "-o$staticOutput" (Join-Path $PSScriptRoot 'static.asm')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    [System.IO.File]::Copy($staticOutput, (Join-Path $PSScriptRoot 'static.rom'), $true)
}
finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
