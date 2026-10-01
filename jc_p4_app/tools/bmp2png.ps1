# bmp2png.ps1 —— 把 SDL 截图（BMP）转成 PNG，便于查看/归档
# 用法: powershell -File tools/bmp2png.ps1 -In D:\...\sim_004.bmp -Out D:\...\sim.png
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out
)

Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $In)) {
    Write-Output "missing $In"
    exit 1
}

try {
    $img = [System.Drawing.Image]::FromFile($In)
    $img.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output ("saved {0} ({1}x{2})" -f $Out, $img.Width, $img.Height)
    $img.Dispose()
} catch {
    Write-Output ("convert failed: " + $_.Exception.Message)
    exit 2
}
