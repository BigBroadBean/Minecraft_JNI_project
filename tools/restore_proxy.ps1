# restore_proxy.ps1: 还原 natives 目录下的原始 glfw.dll (卸载代理)。
param([string]$Natives = 'D:\MCLDownload\Game\.minecraft\versions\1.20\natives')
$ErrorActionPreference = 'Stop'
$orig  = Join-Path $Natives 'glfw_orig.dll'
$target = Join-Path $Natives 'glfw.dll'
if (-not (Test-Path $orig)) { Write-Host "[!] 未找到备份 glfw_orig.dll"; exit 1 }
Copy-Item $orig $target -Force
Write-Host "[+] 已还原: $target"
