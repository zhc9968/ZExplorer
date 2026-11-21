# Generate app.ico (multi-size, 32bpp DIB entries) for ZExplorer.
# Pure ASCII. Requires .NET Framework System.Drawing.
Add-Type -AssemblyName System.Drawing

function New-LogoBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = [Math]::Max(0, [int]($size * 0.02))
    $rect = New-Object System.Drawing.Rectangle $pad, $pad, ($size - 2 * $pad), ($size - 2 * $pad)
    $radius = [single]([Math]::Max(2, $size * 0.18))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc(($rect.Right - $d), $rect.Y, $d, $d, 270, 90)
    $path.AddArc(($rect.Right - $d), ($rect.Bottom - $d), $d, $d, 0, 90)
    $path.AddArc($rect.X, ($rect.Bottom - $d), $d, $d, 90, 90)
    $path.CloseFigure()

    $c1 = [System.Drawing.Color]::FromArgb(255, 26, 108, 204)
    $c2 = [System.Drawing.Color]::FromArgb(255, 12, 68, 140)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $c1, $c2, 90.0
    $g.FillPath($grad, $path)

    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)

    if ($size -le 20) {
        $zw = [single]($size * 0.54)
        $zh = [single]($size * 0.50)
        $zx = [single](($size - $zw) / 2)
        $zy = [single](($size - $zh) / 2)
        $barH = [single]([Math]::Max(2.0, $zh * 0.26))
        $g.FillRectangle($white, $zx, $zy, $zw, $barH)
        $g.FillRectangle($white, $zx, ($zy + $zh - $barH), $zw, $barH)
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([single]([Math]::Max(2.0, $barH)))
        $g.DrawLine($pen, ($zx + $zw - $barH * 0.3), ($zy + $barH), ($zx + $barH * 0.3), ($zy + $zh - $barH))
        $g.Dispose()
        return $bmp
    }

    $fx = [single]($size * 0.16)
    $fw = [single]($size * 0.68)
    $fy = [single]($size * 0.30)
    $fh = [single]($size * 0.44)
    $tabW = [single]($fw * 0.46)
    $tabH = [single]($fh * 0.24)
    $g.FillRectangle($white, $fx, ($fy - $tabH), $tabW, ($tabH * 1.2))
    $g.FillRectangle($white, $fx, ($fy + $tabH * 0.4), $fw, ($fh - $tabH * 0.4))

    $blue = New-Object System.Drawing.SolidBrush $c1
    $bodyTop = [single]($fy + $tabH * 0.4)
    $bodyH = [single]($fh - $tabH * 0.4)
    $zx = [single]($fx + $fw * 0.30)
    $zy = [single]($bodyTop + $bodyH * 0.22)
    $zw = [single]($fw * 0.40)
    $zh = [single]($bodyH * 0.56)
    $barH = [single]([Math]::Max(1.5, $zh * 0.20))
    $g.FillRectangle($blue, $zx, $zy, $zw, $barH)
    $g.FillRectangle($blue, $zx, ($zy + $zh - $barH), $zw, $barH)
    $diag = New-Object System.Drawing.Pen $c1, ([single]([Math]::Max(1.5, $barH)))
    $g.DrawLine($diag, ($zx + $zw), ($zy + $barH), $zx, ($zy + $zh - $barH))

    $g.Dispose()
    return $bmp
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$images = @()

foreach ($s in $sizes) {
    $bmp = New-LogoBitmap $s
    $rect = New-Object System.Drawing.Rectangle 0, 0, $s, $s
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $bytes = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $bmp.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint32]40)
    $bw.Write([int32]$s)
    $bw.Write([int32]($s * 2))
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]0)
    $bw.Write([uint32]($s * $s * 4))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    for ($y = $s - 1; $y -ge 0; $y--) {
        $bw.Write($bytes, ($y * $stride), ($s * 4))
    }
    $maskStride = [int]([Math]::Floor((($s + 31) / 32)) * 4)
    $mask = New-Object byte[] ($maskStride * $s)
    $bw.Write($mask)
    $bw.Flush()
    $images += , ($ms.ToArray())
    $bw.Dispose(); $ms.Dispose()
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$images[$i].Length)
    $bw.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()

$target = Join-Path $PSScriptRoot '..\app.ico'
[System.IO.File]::WriteAllBytes($target, $out.ToArray())
$bw.Dispose(); $out.Dispose()
Write-Host ("wrote " + (Resolve-Path $target) + " (" + (Get-Item $target).Length + " bytes)")
