param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$profilePath = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (Test-Path $profilePath) { . $profilePath | Out-Null }
Write-Host "A=[$env:IDF_PATH] procA=[$([Environment]::GetEnvironmentVariable('IDF_PATH'))]"
$env:IDF_EXTRA_ACTIONS_PATH = (Resolve-Path "$PSScriptRoot\..\managed_components\espressif__esp_board_manager").Path
Write-Host "B=[$env:IDF_PATH] procB=[$([Environment]::GetEnvironmentVariable('IDF_PATH'))]"
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
Write-Host "C=[$idfPy]"
