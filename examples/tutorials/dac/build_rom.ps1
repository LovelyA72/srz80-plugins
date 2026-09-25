param(
    [string]$Z88dkRoot = 'C:\Software\z88dk'
)

$assembler = Join-Path $Z88dkRoot 'bin\z80asm.exe'
if (-not (Test-Path -LiteralPath $assembler)) {
    throw "z88dk z80asm was not found at $assembler. Pass -Z88dkRoot with your installation path."
}

$here = $PSScriptRoot
& $assembler -mz80 -b "-o$here\dac.rom" "$here\dac.asm"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
