param(
    [string]$PortName = "COM4",
    [int]$Seconds = 12,
    [string]$Match = "wifi|got ip|disconnect|handshake|sta|AP"
)
# Passive serial read: DTR/RTS kept low (no board reset), capture log lines
$sp = New-Object System.IO.Ports.SerialPort -ArgumentList $PortName, 115200, 0, 8, 1
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 400
try {
    $sp.Open()
    Write-Host "PASSIVE READ $PortName for ${Seconds}s"
    $deadline = (Get-Date).AddSeconds($Seconds)
    $n = 0
    while ((Get-Date) -lt $deadline) {
        try {
            $line = $sp.ReadLine()
            $n++
            if ($Match -eq "" -or $line -match $Match) { Write-Host $line }
        } catch { }
    }
    Write-Host "--- done, $n lines total ---"
} catch {
    Write-Host "ERR: $($_.Exception.Message)"
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
