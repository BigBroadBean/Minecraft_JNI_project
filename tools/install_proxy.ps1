# install_proxy.ps1: 把状态 DLL 以 glfw.dll 代理形式装进网易客户端的 natives 目录。
# 游戏启动时由自己的加载流程加载它 (System.loadLibrary("glfw")) —— 没有任何外部
# 注入动作、没有运行时内存/模块/线程差异 (启动快照里就有它)。
# 用法:
#   powershell -File tools\install_proxy.ps1                        (默认 1.20 natives)
#   powershell -File tools\install_proxy.ps1 -Natives <natives目录>
# 注意: 游戏更新/启动器修复可能覆盖 natives 文件, 覆盖后需重新运行本脚本。
param([string]$Natives = 'D:\MCLDownload\Game\.minecraft\versions\1.20\natives')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$proxy = Join-Path $root 'glfw_proxy.dll'
$orig  = Join-Path $Natives 'glfw_orig.dll'
$target = Join-Path $Natives 'glfw.dll'

if (-not (Test-Path $Natives)) { Write-Host "[!] natives 目录不存在: $Natives"; exit 1 }
if (-not (Test-Path $proxy))  { Write-Host "[!] 先运行 build.bat 生成 glfw_proxy.dll"; exit 1 }
if (-not (Test-Path $target)) { Write-Host "[!] 找不到原 glfw.dll: $target"; exit 1 }

if (-not (Test-Path $orig)) {
    Copy-Item $target $orig -Force
    Write-Host "[*] 已备份原 glfw.dll -> glfw_orig.dll"
} else {
    Write-Host "[*] glfw_orig.dll 已存在 (保留原备份)"
}
Copy-Item $proxy $target -Force
Write-Host "[+] 代理已安装: $target"
Write-Host "    游戏启动时会自行加载它 (共享内存 Local\MCCombatStatus_<pid> 照常发布)。"
Write-Host "    还原: powershell -File tools\restore_proxy.ps1 -Natives `"$Natives`""
