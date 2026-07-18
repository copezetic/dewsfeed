# =====================================================================
# build_team_logos.ps1 — PowerShell port of build_team_logos.py.
# Uses .NET System.Drawing (built into Windows) so NOTHING needs installing.
#
# What it does:
#   1. Downloads team logos for NFL/NHL/MLB/NBA from the ChuckBuilds repo
#   2. Resizes each PNG to 20×20 and 48×48 using HighQualityBicubic
#   3. Quantizes RGBA → RGB565 (alpha<128 = transparent, sentinel 0x0001)
#   4. Writes ..\team_logos.h next to the .ino
#
# Run from the tools/ folder:
#     powershell -ExecutionPolicy Bypass -File .\build_team_logos.ps1
# =====================================================================

Add-Type -AssemblyName System.Drawing

# --- Teams ----------------------------------------------------------------
$NFL = @("ARI","ATL","BAL","BUF","CAR","CHI","CIN","CLE","DAL","DEN","DET","GB",
         "HOU","IND","JAX","KC","LAC","LAR","LV","MIA","MIN","NE","NO","NYG",
         "NYJ","PHI","PIT","SEA","SF","TB","TEN","WSH")
$NHL = @("ANA","BOS","BUF","CAR","CBJ","CGY","CHI","COL","DAL","DET","EDM","FLA",
         "LA","MIN","MTL","NJ","NSH","NYI","NYR","OTT","PHI","PIT","SEA","SJ",
         "STL","TB","TOR","UTA","VAN","VGK","WPG","WSH")
$MLB = @("ARI","ATL","BAL","BOS","CHC","CWS","CIN","CLE","COL","DET","HOU","KC",
         "LAA","LAD","MIA","MIL","MIN","NYM","NYY","OAK","PHI","PIT","SD","SF",
         "SEA","STL","TB","TEX","TOR","WSH")
$NBA = @("ATL","BOS","BKN","CHA","CHI","CLE","DAL","DEN","DET","GS","HOU","IND",
         "LAC","LAL","MEM","MIA","MIL","MIN","NO","NYK","OKC","ORL","PHI","PHX",
         "POR","SAC","SA","TOR","UTAH","WAS")
$Leagues = @(
    @{ Name = "NFL"; Teams = $NFL },
    @{ Name = "NHL"; Teams = $NHL },
    @{ Name = "MLB"; Teams = $MLB },
    @{ Name = "NBA"; Teams = $NBA }
)

# Aliases: ESPN-API team abbreviation → filename used in the ChuckBuilds repo.
# We always WRITE the entry under the ESPN abbr (so runtime lookup works),
# but DOWNLOAD using the upstream filename. Add more here if any future
# fetches fail (e.g. relocated franchises).
$UpstreamAliases = @{
    "MLB/CWS" = "CHW"   # White Sox: ESPN says CWS, repo file is CHW
    "MLB/OAK" = "ATH"   # A's: now filed as ATH after relocation
    "NBA/NYK" = "NY"    # Knicks: repo uses NY
    "NBA/WAS" = "WSH"   # Wizards: repo uses WSH
}

$Upstream = "https://raw.githubusercontent.com/ChuckBuilds/LEDMatrix/main/assets/sports/{0}_logos/{1}.png"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutPath = Join-Path (Split-Path -Parent $ScriptDir) "team_logos.h"
$Transparent = 0x0001
$AlphaThreshold = 128

Write-Host "Output: $OutPath"

# --- Helpers --------------------------------------------------------------
function ConvertTo-Rgb565([int]$r, [int]$g, [int]$b) {
    $v = ((($r -band 0xF8) -shl 8) -bor (($g -band 0xFC) -shl 3) -bor ($b -shr 3))
    if ($v -eq 0x0001) { $v = 0x0002 }  # avoid collision with transparency sentinel
    return $v
}

function Get-LogoPixels {
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
            if ($c.A -lt $AlphaThreshold) {
                $out.Add($Transparent) | Out-Null
            } else {
                $out.Add((ConvertTo-Rgb565 $c.R $c.G $c.B)) | Out-Null
            }
        }
    }
    $bmp.Dispose()
    $src.Dispose()
    $ms.Dispose()
    return ,$out
}

function Get-PngBytes {
    param([string] $Url)
    try {
        $wc = New-Object System.Net.WebClient
        return ,($wc.DownloadData($Url))
    } catch {
        Write-Host "  ! fetch failed: $Url"
        return $null
    }
}

# --- Main loop ------------------------------------------------------------
$arrays = New-Object 'System.Collections.Generic.List[object]'
$index  = New-Object 'System.Collections.Generic.List[object]'
$fetched = 0
$skipped = New-Object 'System.Collections.Generic.List[string]'

foreach ($league in $Leagues) {
    $lname = $league.Name
    Write-Host "`n== $lname ($($league.Teams.Count) teams) =="
    foreach ($abbr in $league.Teams) {
        # Use upstream alias if defined for this team, else use the abbr directly.
        $aliasKey = "$lname/$abbr"
        $upstreamAbbr = if ($UpstreamAliases.ContainsKey($aliasKey)) {
            $UpstreamAliases[$aliasKey]
        } else { $abbr }
        $url = ($Upstream -f $lname.ToLower(), $upstreamAbbr)
        $png = Get-PngBytes $url
        if ($null -eq $png) { $skipped.Add("$lname $abbr") | Out-Null; continue }
        try {
            $px20 = Get-LogoPixels $png 20
            $px48 = Get-LogoPixels $png 48
        } catch {
            Write-Host "  ! decode failed for $lname $abbr"
            $skipped.Add("$lname $abbr") | Out-Null
            continue
        }
        $safe = $abbr -replace '[^A-Z0-9]', '_'
        $n20 = "LOGO_${lname}_${safe}_20"
        $n48 = "LOGO_${lname}_${safe}_48"
        $arrays.Add(@{ Name = $n20; Pixels = $px20; LineW = 20 }) | Out-Null
        $arrays.Add(@{ Name = $n48; Pixels = $px48; LineW = 24 }) | Out-Null
        $index.Add(@{ League = $lname; Abbr = $abbr; N20 = $n20; N48 = $n48 }) | Out-Null
        $fetched++
        Write-Host "  ok  $lname $abbr"
    }
}

Write-Host "`nFetched $fetched teams, skipped $($skipped.Count)"
if ($skipped.Count -gt 0) {
    Write-Host "Skipped (will fall back to monogram at runtime):"
    foreach ($s in $skipped) { Write-Host "  $s" }
}

# --- Write header ---------------------------------------------------------
Write-Host "`nWriting $OutPath ..."
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// AUTO-GENERATED by tools/build_team_logos.ps1 - do not edit by hand.")
[void]$sb.AppendLine("// Sourced from https://github.com/ChuckBuilds/LEDMatrix (team logos).")
[void]$sb.AppendLine("// Personal-use only; team marks are property of their respective leagues.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <Arduino.h>")
[void]$sb.AppendLine("#include <pgmspace.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("#define LOGO_TRANSPARENT 0x{0:X4}" -f $Transparent))
[void]$sb.AppendLine("#define LOGO_SIZE_20 20")
[void]$sb.AppendLine("#define LOGO_SIZE_48 48")
[void]$sb.AppendLine("")

foreach ($arr in $arrays) {
    $name = $arr.Name
    $pixels = $arr.Pixels
    $lineW = $arr.LineW
    [void]$sb.Append("const uint16_t $name[$($pixels.Count)] PROGMEM = {")
    [void]$sb.AppendLine()
    [void]$sb.Append("  ")
    for ($i = 0; $i -lt $pixels.Count; $i++) {
        [void]$sb.Append(("0x{0:X4}" -f $pixels[$i]))
        if ($i -ne $pixels.Count - 1) {
            [void]$sb.Append(",")
            if ((($i + 1) % $lineW) -eq 0) {
                [void]$sb.AppendLine()
                [void]$sb.Append("  ")
            } else {
                [void]$sb.Append(" ")
            }
        }
    }
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine()
}

[void]$sb.AppendLine("struct TeamLogoEntry {")
[void]$sb.AppendLine("  const char* league;")
[void]$sb.AppendLine("  const char* abbr;")
[void]$sb.AppendLine("  const uint16_t* px20;")
[void]$sb.AppendLine("  const uint16_t* px48;")
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("const int TEAM_LOGO_COUNT = {0};" -f $index.Count))
[void]$sb.AppendLine("const TeamLogoEntry TEAM_LOGOS[TEAM_LOGO_COUNT] = {")
foreach ($e in $index) {
    [void]$sb.AppendLine(('  {{ "{0}", "{1}", {2}, {3} }},' -f $e.League, $e.Abbr, $e.N20, $e.N48))
}
[void]$sb.AppendLine("};")

[System.IO.File]::WriteAllText($OutPath, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))

$flashKb = ($fetched * (400 + 2304) * 2) / 1024
Write-Host ("`nDone. Approx flash use: {0:N0} KB in PROGMEM (you have 4 MB)." -f $flashKb)
Write-Host "Now re-flash the .ino from the Arduino IDE."
