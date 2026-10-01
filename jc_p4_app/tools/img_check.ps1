# img_check.ps1 —— 检查 JPEG 是否可解码 + 抽样平均亮度（验证 ISP 自动曝光有没有生效）
# 用法: powershell -File tools/img_check.ps1 -Path D:\...\preview.jpg
param(
    [Parameter(Mandatory = $true)][string]$Path
)

Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $Path)) {
    Write-Output "missing $Path"
    exit 1
}

$bytes = [IO.File]::ReadAllBytes($Path)
Write-Output ("file {0} bytes  head={1}  tail={2}" -f $bytes.Length,
    (($bytes[0..3] | ForEach-Object { $_.ToString('X2') }) -join ' '),
    (($bytes[($bytes.Length - 2)..($bytes.Length - 1)] | ForEach-Object { $_.ToString('X2') }) -join ' '))

try {
    $img = [System.Drawing.Image]::FromFile($Path)
} catch {
    Write-Output ("DECODE FAILED: " + $_.Exception.Message)
    exit 2
}

$bmp = New-Object System.Drawing.Bitmap($img)
$sum = 0.0
$n = 0
$maxl = 0
for ($y = 0; $y -lt $bmp.Height; $y += 8) {
    for ($x = 0; $x -lt $bmp.Width; $x += 8) {
        $c = $bmp.GetPixel($x, $y)
        $l = ($c.R + $c.G + $c.B) / 3.0
        $sum += $l
        if ($l -gt $maxl) { $maxl = [int]$l }
        $n++
    }
}
Write-Output ("decoded {0}x{1}  avg_luma={2}  max_luma={3}  sampled={4}" -f $bmp.Width, $bmp.Height,
    [math]::Round($sum / $n, 1), $maxl, $n)
$bmp.Dispose()
$img.Dispose()
