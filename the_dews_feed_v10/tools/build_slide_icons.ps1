# =====================================================================
# build_slide_icons.ps1 — downloads Twemoji (CC-BY 4.0) colorful emoji
# PNGs and emits ../slide_icons.h with a per-slide anchor-icon set at
# two sizes (16x16 for headers, 24x24 for larger placements).
#
# Twemoji is the art source because it's openly licensed, 3500+ icons,
# colorful and recognizable even at small sizes — perfect detailed
# anchor icons for the data cards.
#
# Run from this folder:
#     powershell -ExecutionPolicy Bypass -File .\build_slide_icons.ps1
# =====================================================================

Add-Type -AssemblyName System.Drawing

# name -> Twemoji codepoint (lowercase hex, no fe0f variation selector).
# One entry per slide / card identity. Add or tweak freely; missing ones
# just fall back to no-icon at runtime.
$Icons = [ordered]@{
    "news"      = "1f4f0"   # newspaper
    "localnews" = "1f5de"   # rolled-up newspaper
    "scores"    = "1f3c6"   # trophy
    "sports"    = "1f3c8"   # american football
    "finance"   = "1f4c8"   # chart increasing
    "money"     = "1f4b0"   # money bag
    "rates"     = "1f3e6"   # bank
    "weather"   = "26c5"    # sun behind cloud
    "traffic"   = "1f697"   # car
    "train"     = "1f686"   # train
    "flights"   = "2708"    # airplane
    "calendar"  = "1f4c5"   # calendar
    "whoop"     = "1f493"   # beating heart
    "workout"   = "1f3cb"   # weightlifter
    "concert"   = "1f3b8"   # guitar
    "lake"      = "1f30a"   # water wave
    "tiki"      = "1f30b"   # volcano
    "moon"      = "1f319"   # crescent moon
    "house"     = "1f3e1"   # house with garden
    "cabin"     = "1f6d6"   # hut
    "chart"     = "2693"    # anchor (nautical charts)
    "compass"   = "1f9ed"   # compass
    "star"      = "2b50"    # star
    "space"     = "1fa90"   # ringed planet
    "golf"      = "26f3"    # flag in hole
    "tennis"    = "1f3be"   # tennis ball
}

$Upstream  = "https://cdn.jsdelivr.net/gh/twitter/twemoji@14.0.2/assets/72x72/{0}.png"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutPath   = Join-Path (Split-Path -Parent $ScriptDir) "slide_icons.h"
$Transparent = 0x0001
$AlphaThreshold = 96

Write-Host "Output: $OutPath"

function ConvertTo-Rgb565([int]$r, [int]$g, [int]$b) {
    $v = ((($r -band 0xF8) -shl 8) -bor (($g -band 0xFC) -shl 3) -bor ($b -shr 3))
    if ($v -eq 0x0001) { $v = 0x0002 }
    return $v
}

function Get-IconPixels {
    param([byte[]] $PngBytes, [int] $Size)
    $ms = New-Object System.IO.MemoryStream(,$PngBytes)
    $src = [System.Drawing.Image]::FromStream($ms)
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $gfx.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $gfx.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $gfx.Clear([System.Drawing.Color]::Transparent)
    $gfx.DrawImage($src, 0, 0, $Size, $Size)
    $gfx.Dispose()
    $out = New-Object 'System.Collections.Generic.List[int]' ($Size * $Size)
    for ($y = 0; $y -lt $Size; $y++) {
        for ($x = 0; $x -lt $Size; $x++) {
            $c = $bmp.GetPixel($x, $y)
            if ($c.A -lt $AlphaThreshold) { $out.Add($Transparent) | Out-Null }
            else { $out.Add((ConvertTo-Rgb565 $c.R $c.G $c.B)) | Out-Null }
        }
    }
    $bmp.Dispose(); $src.Dispose(); $ms.Dispose()
    return ,$out
}

function Get-PngBytes {
    param([string] $Url)
    try { $wc = New-Object System.Net.WebClient; return ,($wc.DownloadData($Url)) }
    catch { return $null }
}

$arrays  = New-Object 'System.Collections.Generic.List[object]'
$index   = New-Object 'System.Collections.Generic.List[object]'
$fetched = 0
$skipped = New-Object 'System.Collections.Generic.List[string]'

Write-Host "`n== Slide icons ($($Icons.Count) total) =="
foreach ($name in $Icons.Keys) {
    $cp = $Icons[$name]
    $url = ($Upstream -f $cp)
    $png = Get-PngBytes $url
    if ($null -eq $png) { $skipped.Add("$name ($cp)") | Out-Null; Write-Host "  ??  $name ($cp)"; continue }
    try {
        $px16 = Get-IconPixels $png 16
        $px24 = Get-IconPixels $png 24
    } catch { Write-Host "  ! decode failed for $name"; $skipped.Add("$name ($cp)") | Out-Null; continue }
    $safe = ($name.ToUpper() -replace '[^A-Z0-9]', '_')
    $n16 = "ICON_${safe}_16"
    $n24 = "ICON_${safe}_24"
    $arrays.Add(@{ Name = $n16; Pixels = $px16; LineW = 16 }) | Out-Null
    $arrays.Add(@{ Name = $n24; Pixels = $px24; LineW = 24 }) | Out-Null
    $index.Add(@{ Key = $name; N16 = $n16; N24 = $n24 }) | Out-Null
    $fetched++
    Write-Host "  ok  $name"
}

Write-Host "`nFetched $fetched icons, skipped $($skipped.Count)"
if ($skipped.Count -gt 0) { foreach ($s in $skipped) { Write-Host "  skip $s" } }

Write-Host "`nWriting $OutPath ..."
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// AUTO-GENERATED by tools/build_slide_icons.ps1 - do not edit by hand.")
[void]$sb.AppendLine("// Art source: Twemoji (https://github.com/twitter/twemoji), CC-BY 4.0.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <Arduino.h>")
[void]$sb.AppendLine("#include <pgmspace.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("#define SLIDE_ICON_TRANSPARENT 0x{0:X4}" -f $Transparent))
[void]$sb.AppendLine("")
foreach ($arr in $arrays) {
    $name = $arr.Name; $pixels = $arr.Pixels; $lineW = $arr.LineW
    [void]$sb.Append("const uint16_t $name[$($pixels.Count)] PROGMEM = {")
    [void]$sb.AppendLine(); [void]$sb.Append("  ")
    for ($i = 0; $i -lt $pixels.Count; $i++) {
        [void]$sb.Append(("0x{0:X4}" -f $pixels[$i]))
        if ($i -ne $pixels.Count - 1) {
            [void]$sb.Append(",")
            if ((($i + 1) % $lineW) -eq 0) { [void]$sb.AppendLine(); [void]$sb.Append("  ") } else { [void]$sb.Append(" ") }
        }
    }
    [void]$sb.AppendLine(); [void]$sb.AppendLine("};"); [void]$sb.AppendLine()
}
[void]$sb.AppendLine("struct SlideIconEntry { const char* key; const uint16_t* px16; const uint16_t* px24; };")
[void]$sb.AppendLine(("const int SLIDE_ICON_COUNT = {0};" -f $index.Count))
[void]$sb.AppendLine("const SlideIconEntry SLIDE_ICONS[SLIDE_ICON_COUNT] = {")
foreach ($e in $index) { [void]$sb.AppendLine(('  {{ "{0}", {1}, {2} }},' -f $e.Key, $e.N16, $e.N24)) }
[void]$sb.AppendLine("};")
[System.IO.File]::WriteAllText($OutPath, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))

$flashKb = ($fetched * (16*16 + 24*24) * 2) / 1024
Write-Host ("`nDone. Approx flash use: {0:N1} KB in PROGMEM." -f $flashKb)
Write-Host "Now re-flash the .ino from the Arduino IDE."
