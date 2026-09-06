[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '../out/brand'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
[xml]$svg = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../assets/nexplay.svg') -Raw
$ns = [Xml.XmlNamespaceManager]::new($svg.NameTable)
$ns.AddNamespace('s', 'http://www.w3.org/2000/svg')

function Render-Icon([int]$size) {
    $large = [Drawing.Bitmap]::new($size*4, $size*4, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($large)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size*4/256.0, $size*4/256.0)
    try {
        $rect = $svg.SelectSingleNode('//s:rect', $ns)
        $x=[single]$rect.x; $y=[single]$rect.y; $w=[single]$rect.width; $h=[single]$rect.height; $d=2*[single]$rect.rx
        $path = [Drawing.Drawing2D.GraphicsPath]::new()
        $path.AddArc($x,$y,$d,$d,180,90)
        $path.AddArc($x+$w-$d,$y,$d,$d,270,90)
        $path.AddArc($x+$w-$d,$y+$h-$d,$d,$d,0,90)
        $path.AddArc($x,$y+$h-$d,$d,$d,90,90)
        $path.CloseFigure()
        $stops=$svg.SelectNodes('//s:stop',$ns)
        $gradient = [Drawing.Drawing2D.LinearGradientBrush]::new(
            [Drawing.PointF]::new($x,$y),[Drawing.PointF]::new($x+$w,$y+$h),
            [Drawing.ColorTranslator]::FromHtml($stops[0].GetAttribute('stop-color')),
            [Drawing.ColorTranslator]::FromHtml($stops[1].GetAttribute('stop-color')))
        $graphics.FillPath($gradient,$path)
        $gradient.Dispose(); $path.Dispose()
        foreach ($polygon in $svg.SelectNodes('//s:polygon',$ns)) {
            [Drawing.PointF[]]$points = @($polygon.points -split '\s+' | ForEach-Object {
                $pair=$_ -split ','
                [Drawing.PointF]::new([single]$pair[0],[single]$pair[1])
            })
            $brush=[Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($polygon.fill))
            $graphics.FillPolygon($brush,$points)
            $brush.Dispose()
        }
        $result = [Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $output = [Drawing.Graphics]::FromImage($result)
        $output.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
        $output.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $output.DrawImage($large,0,0,$size,$size)
        $output.Dispose()
        return $result
    } finally { $graphics.Dispose(); $large.Dispose() }
}

$sizes=@(16,20,24,32,40,48,64,128,256)
$images=[Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $bitmap=Render-Icon $size
    $memory=[IO.MemoryStream]::new()
    $bitmap.Save($memory,[Drawing.Imaging.ImageFormat]::Png)
    $images.Add($memory.ToArray())
    if ($size -eq 256) { $bitmap.Save((Join-Path $OutputDirectory 'nexplay.png'),[Drawing.Imaging.ImageFormat]::Png) }
    $memory.Dispose(); $bitmap.Dispose()
}
$file=[IO.File]::Create((Join-Path $OutputDirectory 'nexplay.ico'))
$writer=[IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset=6+16*$sizes.Count
    for ($i=0;$i -lt $sizes.Count;$i++) {
        $side=if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$side); $writer.Write([byte]$side)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset+=$images[$i].Length
    }
    foreach ($image in $images) { $writer.Write($image) }
} finally { $writer.Dispose() }
Write-Output "Brand assets: $OutputDirectory"
