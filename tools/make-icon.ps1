# Gera src\app.ico (três barrinhas sobre fundo escuro). Rode de novo só se quiser mudar o ícone.
Add-Type -AssemblyName System.Drawing

function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Fundo: quadrado arredondado escuro
    $r = [Math]::Max(2, [int]($s * 0.22)); $d = $r * 2; $w = $s
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $w - $d, $d, $d, 0, 90)
    $path.AddArc(0, $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0x16, 0x18, 0x1D))
    $g.FillPath($bg, $path)

    # Três barras alinhadas à grade de pixels (ficam nítidas em 16 px)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::None
    $bw = [Math]::Max(2, [int][Math]::Round($s * 0.16))
    $gap = [Math]::Max(1, [int][Math]::Round($s * 0.09))
    $x = [int](($s - (3 * $bw + 2 * $gap)) / 2)
    $base = [int][Math]::Round($s * 0.77)
    $heights = @(0.27, 0.54, 0.39)
    $colors = @('#4C9FFF', '#3DDC84', '#F5B83D')
    for ($i = 0; $i -lt 3; $i++) {
        $h = [Math]::Max(2, [int][Math]::Round($s * $heights[$i]))
        $brush = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml($colors[$i]))
        $g.FillRectangle($brush, $x, $base - $h, $bw, $h)
        $brush.Dispose()
        $x += $bw + $gap
    }
    $bg.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# Entrada BMP de 32 bits (formato clássico de ícone): cabeçalho + pixels de baixo para cima + máscara AND zerada.
function Get-DibBytes([System.Drawing.Bitmap]$bmp) {
    $s = $bmp.Width
    $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = New-Object byte[] ($s * $s * 4)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $bmp.UnlockBits($data)

    $maskRow = [int]([Math]::Floor(($s + 31) / 32) * 4)
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int]40); $bw.Write([int]$s); $bw.Write([int]($s * 2)); $bw.Write([int16]1); $bw.Write([int16]32)
    $bw.Write([int]0); $bw.Write([int]($s * $s * 4)); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0)
    for ($y = $s - 1; $y -ge 0; $y--) { $bw.Write($pixels, $y * $s * 4, $s * 4) }
    $bw.Write((New-Object byte[] ($maskRow * $s)))
    $bw.Flush()
    return ,$ms.ToArray()
}

$sizes = @(16, 20, 24, 32, 48, 256)
$images = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    if ($s -ge 256) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $images += ,($ms.ToArray())
    } else {
        $images += ,(Get-DibBytes $bmp)
    }
    $bmp.Dispose()
}

$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'src\app.ico'
$fs = [System.IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]$images[$i].Length); $w.Write([int]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush(); $fs.Close()
"Icone gerado: $out ($((Get-Item $out).Length) bytes)"
