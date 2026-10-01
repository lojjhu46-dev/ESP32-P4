# compare_img.ps1 —— 比较两张同尺寸图：是同一视角，还是互为镜像
# 用法: powershell -File tools/compare_img.ps1 -PathA a.jpg -PathB b.jpg
param(
    [Parameter(Mandatory = $true)][string]$PathA,
    [Parameter(Mandatory = $true)][string]$PathB
)

Add-Type -AssemblyName System.Drawing

$imgA = [System.Drawing.Image]::FromFile($PathA)
$imgB = [System.Drawing.Image]::FromFile($PathB)
$bmpA = New-Object System.Drawing.Bitmap -ArgumentList (, $imgA)
$bmpB = New-Object System.Drawing.Bitmap -ArgumentList (, $imgB)

if ($bmpA.Width -ne $bmpB.Width -or $bmpA.Height -ne $bmpB.Height) {
    Write-Output ("size mismatch: {0}x{1} vs {2}x{3}" -f $bmpA.Width, $bmpA.Height, $bmpB.Width, $bmpB.Height)
    exit 1
}

$sumSame = 0.0
$sumMirror = 0.0
$count = 0
for ($y = 0; $y -lt $bmpA.Height; $y += 4) {
    for ($x = 0; $x -lt $bmpA.Width; $x += 4) {
        $pa = $bmpA.GetPixel($x, $y)
        $ps = $bmpB.GetPixel($x, $y)
        $pm = $bmpB.GetPixel($bmpA.Width - 1 - $x, $y)
        $sumSame += [Math]::Abs($pa.R - $ps.R) + [Math]::Abs($pa.G - $ps.G) + [Math]::Abs($pa.B - $ps.B)
        $sumMirror += [Math]::Abs($pa.R - $pm.R) + [Math]::Abs($pa.G - $pm.G) + [Math]::Abs($pa.B - $pm.B)
        $count++
    }
}

if ($count -eq 0) {
    Write-Output "no pixels sampled (bad image?)"
    exit 2
}

$same = $sumSame / $count
$mirr = $sumMirror / $count
Write-Output ("sampled {0} px   same-layout diff = {1:N1}   mirror-layout diff = {2:N1}" -f $count, $same, $mirr)
if ($mirr -lt $same * 0.7) {
    Write-Output "=> MIRROR RELATION (B 是 A 的左右镜像)"
} elseif ($same -lt $mirr * 0.7) {
    Write-Output "=> SAME VIEW (视角一致，没有翻转)"
} else {
    Write-Output "=> UNCLEAR (内容/亮度变化太大)"
}

$bmpA.Dispose(); $bmpB.Dispose(); $imgA.Dispose(); $imgB.Dispose()
