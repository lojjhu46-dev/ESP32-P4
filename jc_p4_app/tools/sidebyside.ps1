# sidebyside.ps1 —— 把两张图缩放后左右并排拼成一张（对比用）
# 用法: powershell -File tools/sidebyside.ps1 -PathA a.jpg -PathB b.jpg -Out ab.png
param(
    [Parameter(Mandatory = $true)][string]$PathA,
    [Parameter(Mandatory = $true)][string]$PathB,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Width = 480,
    [string]$LabelA = "A",
    [string]$LabelB = "B"
)

Add-Type -AssemblyName System.Drawing

$imgA = [System.Drawing.Image]::FromFile($PathA)
$imgB = [System.Drawing.Image]::FromFile($PathB)

$h = [int]($Width * $imgA.Height / $imgA.Width)
$canvas = [System.Drawing.Bitmap]::new($Width * 2, $h + 24)
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.Clear([System.Drawing.Color]::Black)
$g.DrawImage($imgA, 0, 24, $Width, $h)
$g.DrawImage($imgB, $Width, 24, $Width, $h)

$font = New-Object System.Drawing.Font -ArgumentList "Consolas", 14
$brush = [System.Drawing.Brushes]::White
$g.DrawString($LabelA, $font, $brush, 6, 2)
$g.DrawString($LabelB, $font, $brush, ($Width + 6), 2)

$canvas.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $canvas.Dispose(); $imgA.Dispose(); $imgB.Dispose()
Write-Output ("saved {0} ({1}x{2})" -f $Out, ($Width * 2), ($h + 24))
