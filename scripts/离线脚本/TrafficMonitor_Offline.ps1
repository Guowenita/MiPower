# TrafficMonitor 离线模式核心逻辑
# 由同名 .bat 提权后调用，参数: -Mode block|unblock|status

param(
    [ValidateSet('block','unblock','status')][string]$Mode = 'status'
)

$ErrorActionPreference = 'Continue'
$script:Dir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Log([string]$Text) {
    Write-Host $Text
}

function Get-TargetExe {
    # 优先: 脚本所在目录的上一级 -> TrafficMonitor\TrafficMonitor.exe
    $c1 = Join-Path (Split-Path -Parent $script:Dir) 'TrafficMonitor\TrafficMonitor.exe'
    if (Test-Path $c1) { return $c1 }
    # 兜底: 桌面下递归查找
    $hit = Get-ChildItem -Path "$env:USERPROFILE\Desktop" -Filter 'TrafficMonitor.exe' `
                        -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return ''
}

function Get-MatchingRules {
    $result = New-Object System.Collections.ArrayList
    $all = Get-NetFirewallRule -ErrorAction SilentlyContinue
    foreach ($r in $all) {
        $af = $r | Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue
        $name = $r.DisplayName
        $prog = ''
        if ($af) { $prog = $af.Program }
        if (($name -like 'TM-Offline*') -or ($name -like 'Block TrafficMonitor*') -or
            ($prog -like '*TrafficMonitor*')) {
            $rf = $r | Get-NetFirewallAddressFilter -ErrorAction SilentlyContinue
            $remote = ''
            if ($rf) { $remote = ($rf.RemoteAddress -join ',') }
            [void]$result.Add([pscustomobject]@{
                Name = $name; Dir = $r.Direction; Action = $r.Action
                Enabled = $r.Enabled; Remote = $remote; Program = $prog
            })
        }
    }
    return $result
}

function Remove-MatchingRules {
    $n = 0
    $all = Get-NetFirewallRule -ErrorAction SilentlyContinue
    foreach ($r in $all) {
        if (($r.DisplayName -like 'TM-Offline*') -or ($r.DisplayName -like 'Block TrafficMonitor*')) {
            Remove-NetFirewallRule -Name $r.Name -ErrorAction SilentlyContinue
            $n++
        }
    }
    return $n
}

switch ($Mode) {

    'block' {
        Write-Host '[1/4] 清理历史规则...'
        $removed = Remove-MatchingRules
        Write-Host ("      已清理 " + $removed + " 条")

        $exe = Get-TargetExe
        if (-not $exe) {
            Write-Host '[错误] 未找到 TrafficMonitor.exe'
            Write-Host '       请确认脚本位于 TrafficMonitor 安装根目录 目录下'
            exit 1
        }
        Write-Host ('[信息] 目标程序: ' + $exe)

        Write-Host '[2/4] 创建互联网阻断规则...'
        #
        # 重要: 不能用 -RemoteAddress Internet!
        #       实测它会把局域网设备(如 192.168.1.10 米家插座)也一并拦截,
        #       导致插件取不到数据。必须显式枚举公网网段来阻断。
        #
        $publicRanges = @(
            '0.0.0.0/1',      '128.0.0.0/2'    # 覆盖 0.0.0.0-191.255.255.255
            '192.0.0.0/3'                        # 192.0.0.0-223.255.255.255
            '224.0.0.0/3'                        # 224.0.0.0-255.255.255.255
        )
        # 但 192.0.0.0/3 含 192.168.x.x, 所以再排除私有段:
        # 用 4 条更精确的规则覆盖公网, 私有段完全不动
        $publicRanges = @(
            '1.0.0.0/8',    '2.0.0.0/7',   '4.0.0.0/6',  '8.0.0.0/5',
            '16.0.0.0/4',   '32.0.0.0/3',  '64.0.0.0/2', '128.0.0.0/2'
        )
        # 上面的分段覆盖 1.0.0.0 - 191.255.255.255, 不含 192.168.x.x
        # 再补 224.0.0.0/3 (组播) 与 240.0.0.0/4 (保留)
        $publicRanges += @('224.0.0.0/3', '240.0.0.0/4')

        $created = 0
        foreach ($r in $publicRanges) {
            try {
                New-NetFirewallRule -DisplayName 'TM-Offline Block Internet' `
                    -Direction Outbound -Program $exe -Action Block `
                    -Profile Any -RemoteAddress $r -Enabled True -ErrorAction Stop | Out-Null
                $created++
            } catch { }
        }
        if ($created -gt 0) {
            Write-Host ("      [OK] 已创建 " + $created + " 条公网阻断规则")
        } else {
            Write-Host '      [警告] 公网阻断规则创建失败'
        }

        Write-Host '[3/4] 添加局域网放行规则...'
        foreach ($n in @('192.168.0.0/16','10.0.0.0/8','172.16.0.0/12','127.0.0.0/8')) {
            try {
                New-NetFirewallRule -DisplayName 'TM-Offline Allow LAN' `
                    -Direction Outbound -Program $exe -Action Allow `
                    -Profile Any -RemoteAddress $n -Enabled True -ErrorAction SilentlyContinue | Out-Null
            } catch { }
        }

        Write-Host '[4/4] 验证...'
    }

    'unblock' {
        Write-Host '[1/2] 删除所有离线规则...'
        $removed = Remove-MatchingRules
        Write-Host ("      已删除 " + $removed + " 条")

        Write-Host '[2/2] 校验...'
    }

    'status' {
        $exe = Get-TargetExe
        Write-Host ('目标程序: ' + $exe)
        Write-Host ''
    }
}

# ---------- 状态输出 ----------
$rules = Get-MatchingRules
Write-Host ''
Write-Host '============================================================'
if (@($rules).Count -eq 0) {
    Write-Host ' 当前状态: 在线模式（无任何拦截规则）'
    Write-Host ' TrafficMonitor 可正常访问互联网'
} else {
    Write-Host ' 当前生效规则:'
    Write-Host ''
    foreach ($r in $rules) {
        Write-Host ('   ' + $r.Name)
        Write-Host ('     方向    : ' + $r.Dir)
        Write-Host ('     动作    : ' + $r.Action)
        Write-Host ('     启用    : ' + $r.Enabled)
        Write-Host ('     目标地址: ' + $r.Remote)
        Write-Host ''
    }
    $hasBlock = @($rules | Where-Object { $_.Action -eq 'Block' -and $_.Name -like 'TM-Offline Block*' }).Count
    if ($hasBlock -gt 0) {
        Write-Host ' => 离线模式生效中: 互联网已断, 局域网(米家插座等)仍可用'
    } else {
        Write-Host ' => 存在其他 TrafficMonitor 相关规则'
    }
}
Write-Host '============================================================'

# 记录到状态文件
$out = New-Object System.Collections.ArrayList
[void]$out.Add('=== TrafficMonitor 离线状态 ===')
[void]$out.Add('查询时间: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
[void]$out.Add('模式    : ' + $Mode)
[void]$out.Add('')
foreach ($r in $rules) {
    [void]$out.Add(('{0} | {1} | {2} | {3} | {4}' -f $r.Name, $r.Dir, $r.Action, $r.Enabled, $r.Remote))
}
if (@($rules).Count -eq 0) { [void]$out.Add('(无规则 - 在线模式)') }
$out | Out-File -FilePath (Join-Path $script:Dir 'offline_status.txt') -Encoding utf8
