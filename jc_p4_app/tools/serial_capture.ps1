param(
    [string]$PortName = 'COM4',
    [int]$Baud = 115200,
    [int]$Seconds = 30,
    [switch]$NoReset
)

$port = New-Object System.IO.Ports.SerialPort $PortName, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$port.ReadTimeout = 500
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.Open()
Start-Sleep -Milliseconds 300

if (-not $NoReset) {
    # classic esptool reset: RTS -> EN low, IO0 kept high by DTR=false
    $port.RtsEnable = $true
    Start-Sleep -Milliseconds 150
    $port.RtsEnable = $false
    Write-Output "--- reset via RTS, capturing $Seconds s ---"
}

$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try {
        $line = $port.ReadLine()
        Write-Output $line
    } catch [TimeoutException] {
    } catch {
        Start-Sleep -Milliseconds 50
    }
}
$port.Close()
Write-Output "--- capture done ---"
