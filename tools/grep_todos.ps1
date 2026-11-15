$ErrorActionPreference = 'Stop'
Push-Location 'C:\AI_Projektit\F4-Flat-2-VR'
Write-Host '=== TODO/FIXME markers ==='
Get-ChildItem -Recurse -File -Name |
  Where-Object { $_ -match '\.(cpp|h|hlsl|py|ps1)$' } |
  ForEach-Object {
    $content = Get-Content $_ -Raw
    if ($content -match 'TODO|FIXME|XXX|HACK') {
      Write-Host "--- $_"
      $content | Select-String 'TODO|FIXME|XXX|HACK' | ForEach-Object {
        $ln = $_.LineNumber
        $txt = $_.Line.Trim()
        Write-Host "  L$ln: $txt"
      }
    }
  }
Pop-Location

$ErrorActionPreference = 'Stop'
Push-Location 'C:\AI_Projektit\F4-Flat-2-VR'
Write-Host '=== TODO/FIXME markers ==='
Get-ChildItem -Recurse -File -Name |
  Where-Object { $_ -match '\.(cpp|h|hlsl|py|ps1)$' } |
  ForEach-Object {
    $content = Get-Content $_ -Raw
    if ($content -match 'TODO|FIXME|XXX|HACK') {
      Write-Host "--- $_"
      $content | Select-String 'TODO|FIXME|XXX|HACK' | ForEach-Object {
        $ln = $_.LineNumber
        $txt = $_.Line.Trim()
        Write-Host "  L$ln: $txt"
      }
    }
  }
Pop-Location
