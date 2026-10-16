# _ab_test.ps1 -- USB / TCP dual-interface jc_test suite (2026-10-16)
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File pc_host\_ab_test.ps1
#
# Runs the same functional set twice: first over USB (COM5), then over TCP
# (192.168.33.2:3333), and prints the result frames for each step:
#   list / stream on-off / 0x01 PC frame push / 0x06 F10 export+import / list again
# "link" (the active TX backend) is read from the result frames themselves
# (field `link`), so no raw JSON command is needed.
#
# NOTE (keep this file ASCII-only): Windows PowerShell 5.1 reads .ps1 as ANSI,
#       so UTF-8 Chinese comments break parsing (documented in HANDOVER).
# NOTE (why no `jc_test.py cmd '{"...":...}'` here): PowerShell 5.1 mangles JSON
#       arguments passed to native executables (looks like `{cmd:link}` ->
#       json.decoder.JSONDecodeError). Raw JSON commands work when typed in a
#       real shell (or via bash); the subcommands used below do not need JSON.
# NOTE: the board serves one TCP client at a time -> run this script serially.
$ErrorActionPreference = 'Continue'
Set-Location (Join-Path $PSScriptRoot '..')
$img = 'esp-dl\examples\human_face_recognition\faces\musk1.jpg'

function Section($t) {
    Write-Output ''
    Write-Output ('===== ' + $t + ' =====')
}

$ifaces = @(
    @{ n = 'USB'; a = @('--serial', 'COM5') },
    @{ n = 'TCP'; a = @('--tcp', '192.168.33.2:3333') }
)

foreach ($i in $ifaces) {
    $n = $i.n
    $a = $i.a

    Section "[$n] 0x03/0x02 list (link field = active TX backend)"
    python pc_host/jc_test.py @a list 2>&1 | Select-String 'lib"' | Select-Object -Last 1 -ExpandProperty Line

    Section "[$n] 0x05 stream on 10fps / off"
    python pc_host/jc_test.py @a stream on --fps 10 2>&1 | Select-Object -Last 1
    Start-Sleep -Seconds 3
    python pc_host/jc_test.py @a stream off 2>&1 | Select-Object -Last 1

    Section "[$n] 0x01 PC frame push"
    python pc_host/jc_test.py @a frame $img 2>&1 | Select-Object -Last 1

    Section "[$n] 0x06 F10 export"
    python pc_host/jc_test.py @a db_backup _dev\ab_bak.bin 2>&1 | Select-String 'db_export|person|FAILED' | Select-Object -First 3 -ExpandProperty Line

    Section "[$n] 0x06 F10 import (restore)"
    python pc_host/jc_test.py @a db_restore _dev\ab_bak.bin 2>&1 | Select-String 'db_import ok|FAILED' | Select-Object -Last 1 -ExpandProperty Line

    Section "[$n] list after restore"
    python pc_host/jc_test.py @a list 2>&1 | Select-String 'lib"' | Select-Object -Last 1 -ExpandProperty Line
}

Write-Output ''
Write-Output '===== cleanup: stream off + src=cam + final list ====='
python pc_host/jc_test.py --serial COM5 stream off 2>&1 | Select-Object -Last 1
python pc_host/jc_test.py --serial COM5 src cam 2>&1 | Select-Object -Last 1
python pc_host/jc_test.py --serial COM5 list 2>&1 | Select-String 'lib"' | Select-Object -Last 1 -ExpandProperty Line
