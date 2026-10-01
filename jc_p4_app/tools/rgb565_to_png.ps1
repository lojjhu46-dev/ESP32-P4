# rgb565_to_png.ps1 —— 把 0x05 raw 帧（RGB565 裸像素）转成 PNG
# 用法:
#   powershell -File tools/rgb565_to_png.ps1 -In x.rgb565 -Width 640 -Height 480 -Out x_le.png
#   powershell -File tools/rgb565_to_png.ps1 -In x.rgb565 -Width 640 -Height 480 -Out x_be.png -BigEndian
# 说明：V4L2 的 RGB565 是小端（RGBP），如果转出来颜色怪，用 -BigEndian 再试一次即可对比字节序。
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][int]$Width,
    [Parameter(Mandatory = $true)][int]$Height,
    [Parameter(Mandatory = $true)][string]$Out,
    [switch]$BigEndian
)

Add-Type -AssemblyName System.Drawing

$bytes = [IO.File]::ReadAllBytes($In)
$need = $Width * $Height * 2
if ($bytes.Length -lt $need) {
    Write-Output ("file too short: {0} < {1} bytes" -f $bytes.Length, $need)
    exit 1
}

$bmp = [System.Drawing.Bitmap]::new($Width, $Height)
for ($y = 0; $y -lt $Height; $y++) {
    for ($x = 0; $x -lt $Width; $x++) {
        $i = ($y * $Width + $x) * 2
        if ($BigEndian) {
            $v = ([int]$bytes[$i] -shl 8) -bor [int]$bytes[$i + 1]
        } else {
            $v = ([int]$bytes[$i + 1] -shl 8) -bor [int]$bytes[$i]
        }
        $r = [int](((($v -shr 11) -band 0x1F) * 255) / 31)
        $g = [int](((($v -shr 5) -band 0x3F) * 255) / 63)
        $b = [int]((($v -band 0x1F) * 255) / 31)
        $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($r, $g, $b))
    }
}

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("saved {0} ({1}x{2}, {3})" -f $Out, $Width, $Height, $(if ($BigEndian) { "big-endian" } else { "little-endian" }))
