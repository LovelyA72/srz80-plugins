param(
    [string]$RiscvBin = $env:RISCV_BIN
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RiscvBin)) {
    $RiscvBin = 'C:\msys64\ucrt64\bin'
}

$gcc = Join-Path $RiscvBin 'riscv64-unknown-elf-gcc.exe'
$objcopy = Join-Path $RiscvBin 'riscv64-unknown-elf-objcopy.exe'
if (-not (Test-Path -LiteralPath $gcc) -or -not (Test-Path -LiteralPath $objcopy)) {
    throw "RISC-V compiler not found in $RiscvBin"
}

$example = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$elf = Join-Path $example 'demo.elf.tmp'
$wav = Join-Path $example 'pcm.wav'
$pcm = Join-Path $example 'pcm.bin'
$raw = Join-Path $example 'pcm.raw.tmp'
$rom = Join-Path $example 'demo.rom'

try {
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    $sox = Get-Command sox -ErrorAction SilentlyContinue
    if ($ffmpeg) {
        # X1-010 PCM is signed 8-bit. Keep the source's 48 kHz rate, downmix to
        # mono, and retain the first 0x80000 samples of the demo.
        & $ffmpeg.Source '-y' '-v' 'error' '-i' $wav '-ac' '1' '-ar' '48000' `
            '-c:a' 'pcm_s8' '-frames:a' '524288' '-f' 's8' $raw
    } elseif ($sox) {
        & $sox.Source $wav '-t' 'raw' '-c' '1' '-e' 'signed-integer' '-b' '8' '-r' '48000' `
            $raw 'trim' '0' '10.922667'
    } else {
        throw 'Neither ffmpeg nor sox is available for pcm.wav conversion'
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $raw)) {
        throw 'PCM conversion failed'
    }

    # The RAM card is always a full 1 MiB linear sample window. Pad the source
    # ROM to that size; the firmware plays the first 0x80000 bytes.
    $converted = [System.IO.File]::ReadAllBytes($raw)
    $output = [byte[]]::new(0x100000)
    [Array]::Copy($converted, 0, $output, 0, [Math]::Min($converted.Length, 0x80000))
    [System.IO.File]::WriteAllBytes($pcm, $output)

    & $gcc '-march=rv32imf' '-mabi=ilp32' '-nostdlib' '-nostartfiles' '-Os' '-ffreestanding' `
        '-fno-builtin' '-fno-stack-protector' '-fomit-frame-pointer' '-msmall-data-limit=0' `
        '-Wl,-T,linker.ld' '-Wl,--no-relax' '-Wl,--build-id=none' '-o' $elf 'demo.c'
    if ($LASTEXITCODE -ne 0) { throw 'RISC-V firmware compilation failed' }
    & $objcopy '-O' 'binary' $elf $rom
    if ($LASTEXITCODE -ne 0) { throw 'RISC-V ROM extraction failed' }
    Write-Host "Wrote $rom and $pcm"
}
finally {
    if (Test-Path -LiteralPath $elf) {
        Remove-Item -LiteralPath $elf -Force
    }
    if (Test-Path -LiteralPath $raw) {
        Remove-Item -LiteralPath $raw -Force
    }
}
