<#
.SYNOPSIS
  检测并自动安装策划 Windows 本地启动 GameServer 所需的环境。

.DESCRIPTION
  逐项检测：git / Python / PyYAML / Go / protoc / protoc-gen-go /
  .NET runtime。缺失的自动通过 winget / pip /
  go install 安装。远端 etcd/redis/mongo 复用 tencent.codepeak.cn，无需本地装。
  protoc-gen-svcstub 是仓库内构建产物，不在本脚本检测范围。

  检测原则：一律实跑验退出码，不看 Get-Command 就下结论。
  微软商店的"应用执行别名"存根（python.exe -> AppInstallerPythonRedirector.exe）
  会被 Get-Command 命中，但实跑必然失败（退出码非 0、无输出）；只信
  Get-Command 会把它误判成"已安装"，再让后续所有依赖它的检测连锁误报。

.PARAMETER DryRun
  只检测不安装。

.EXAMPLE
  # 双击 setup_windows_env.bat，或命令行：
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup_windows_env.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup_windows_env.ps1 -DryRun
#>
param([switch]$DryRun)

$ErrorActionPreference = "Continue"

function Say-Ok([string]$n)      { Write-Host "  [OK] $n" -ForegroundColor Green }
function Say-Missing([string]$n) { Write-Host "  [缺失] $n" -ForegroundColor Yellow }

function Install-Winget([string]$id) {
    winget install --silent --accept-package-agreements --accept-source-agreements --source winget --id $id -e
}

# 实跑一条命令，返回 @{ Ok = 退出码是否为 0; Out = 合并后的输出 }
# 命令不存在时 cmd 返回非 0，不会抛异常。
function Invoke-Probe([string]$cmdLine) {
    $out = cmd /c "$cmdLine 2>&1"
    return [pscustomobject]@{
        Ok  = ($LASTEXITCODE -eq 0)
        Out = (($out | Out-String).Trim())
    }
}

# 命令是否指向微软商店的"应用执行别名"存根。这类存根在 PATH 里是一个
# 重定向器，实跑只会失败——必须专门识别，否则用户会以为装好了。
function Test-StoreAlias([string]$exe) {
    $c = Get-Command $exe -ErrorAction SilentlyContinue
    if (-not $c) { return $false }
    $path = if ($c.Source) { $c.Source } else { $c.Path }
    return ($path -like "*\WindowsApps\*")
}

# 按【持久化】PATH（机器 + 用户）算第一个存在的 python.exe 是不是存根。
# 官方安装器会把自身目录插到用户 PATH 前面，通常能盖过存根——所以不能凭
# "存在别名" 就断言装完还得手动关，必须实算一遍顺序。
function Test-AliasShadowsPython {
    $eff = @()
    $eff += ([Environment]::GetEnvironmentVariable("Path", "Machine") -split ";")
    $eff += ([Environment]::GetEnvironmentVariable("Path", "User")    -split ";")
    $win = $eff | Where-Object { $_ } |
           ForEach-Object { Join-Path $_ "python.exe" -ErrorAction SilentlyContinue } |
           Where-Object { $_ -and (Test-Path $_ -ErrorAction SilentlyContinue) } |
           Select-Object -First 1
    if (-not $win) { return $false }
    return ($win -like "*\WindowsApps\*")
}

Write-Host ""
Write-Host "=== GameServer Windows 环境检测 ===" -ForegroundColor Cyan
if ($DryRun) { Write-Host "  （DryRun：只检测不安装）" -ForegroundColor Cyan }

# 0. winget（安装器本身，缺失则无法自动装）
if ((Invoke-Probe "winget --version").Ok) {
    Say-Ok "winget（安装器）"
} else {
    Write-Host "  [!!] 未检测到 winget，无法自动安装（Windows 11 一般自带 App Installer）" -ForegroundColor Red
    exit 1
}

# 1. 简单命令行工具：name / 探针命令 / winget id
$tools = @(
    @("git",    "git --version",    "Git.Git"),
    @("Go",     "go version",       "GoLang.Go"),
    @("protoc", "protoc --version", "Google.Protobuf")
)
foreach ($t in $tools) {
    $name = $t[0]; $probeCmd = $t[1]; $wid = $t[2]
    $p = Invoke-Probe $probeCmd
    if ($p.Ok) {
        Say-Ok "$name（$($p.Out)）"
    } else {
        Say-Missing $name
        if (-not $DryRun) {
            Write-Host "        安装: winget install $wid"
            Install-Winget $wid
        }
    }
}

# 2. Python —— 必须实跑验证，不能只看 Get-Command（见文件头说明）
$pyCmd = Get-Command python -ErrorAction SilentlyContinue
$pyProbe = if ($pyCmd) { Invoke-Probe "python -V" } else { $null }
$pythonUsable = ($pyProbe -ne $null -and $pyProbe.Ok)

if ($pythonUsable) {
    Say-Ok "Python（$($pyProbe.Out)）"
} else {
    if ($pyCmd -and (Test-StoreAlias "python")) {
        Say-Missing "Python（PATH 里只有微软商店的“应用执行别名”存根，不是真 Python）"
    } else {
        Say-Missing "Python"
    }
    if (-not $DryRun) {
        Write-Host "        安装: winget install Python.Python.3.14"
        Install-Winget "Python.Python.3.14"
        Write-Host "        重开终端后会自动生效——本会话的 PATH 是安装前的旧快照，不用管。"
        if (Test-AliasShadowsPython) {
            Write-Host "        [!!] 但持久化 PATH 里 WindowsApps 仍排在真 Python 之前，存根会一直抢在前头：" -ForegroundColor Red
            Write-Host "             设置 → 应用 → 高级应用设置 → 应用执行别名 → 关掉 python.exe / python3.exe" -ForegroundColor Red
        }
    }
}

# 3. PyYAML —— 只有 Python 真能用时才有意义，否则报"缺 pyyaml"会误导
if (-not $pythonUsable) {
    Write-Host "  [跳过] PyYAML（Python 不可用，无法判定 —— 先解决上面的 Python）" -ForegroundColor DarkYellow
} elseif ((Invoke-Probe "python -m pip show pyyaml").Ok) {
    Say-Ok "PyYAML"
} else {
    Say-Missing "PyYAML"
    if (-not $DryRun) { python -m pip install --quiet pyyaml }
}

# 4. .NET runtime（Luban 用）
if ((Invoke-Probe "dotnet --list-runtimes").Ok) {
    Say-Ok ".NET runtime"
} else {
    Say-Missing ".NET runtime"
    if (-not $DryRun) { Install-Winget "Microsoft.DotNet.Runtime.8" }
}

# 5. protoc-gen-go（装到 %USERPROFILE%\go\bin）
$pgGo = Get-Command protoc-gen-go -ErrorAction SilentlyContinue
$pgGoInBin = Test-Path (Join-Path $env:USERPROFILE "go\bin\protoc-gen-go.exe")
if ($pgGo -or $pgGoInBin) {
    Say-Ok "protoc-gen-go"
} else {
    Say-Missing "protoc-gen-go"
    if (-not $DryRun) { go install google.golang.org/protobuf/cmd/protoc-gen-go@latest }
}

Write-Host ""
Write-Host "=== 检测完成 ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "后续注意事项：" -ForegroundColor Cyan
Write-Host "  1. 新安装的工具可能要重开终端才进 PATH；若上面有 [缺失]，重开终端后重跑确认。"
Write-Host "  2. 若本次命中了微软商店别名存根：重开终端后 python 通常就会指向真 Python。"
Write-Host "     若重开后仍是存根，再去 设置 → 应用 → 高级应用设置 → 应用执行别名"
Write-Host "     关掉 python.exe / python3.exe。"
Write-Host "  3. 需设置 PROTOC_INCLUDE 指向 protoc 的 include 目录，例如："
Write-Host "       $env:LOCALAPPDATA\Microsoft\WinGet\Packages\Google.Protobuf_*\include"
Write-Host "  4. 环境就绪后，构建/启动命令："
Write-Host "       python scripts/config.py --env dev --world-id 2 --out run"
Write-Host "       python scripts/build.py --out run --build build"
Write-Host "       cd run 再 ./startall.sh"
