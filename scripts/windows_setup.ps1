# windows_setup.ps1 - one-time setup so two machines can play through WSL.
#
# Run it in PowerShell as Administrator, from the repo folder:
#   powershell -ExecutionPolicy Bypass -File scripts\windows_setup.ps1
#
# It does this:
#   1. Turns on WSL mirrored networking (WSL gets the same address as Windows)
#   2. Allows ping, the game ports (UDP 7400-7600) and the join code port
#      (TCP 7390) in the Windows firewall
#   3. Opens the same ports for WSL itself (Hyper-V firewall)
#   4. Shows if your network is set to Public
# Then run:  wsl --shutdown   and open WSL again.

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Please run this in an Administrator PowerShell."
    exit 1
}

# 1. Mirrored networking
$cfg = Join-Path $env:USERPROFILE ".wslconfig"
if (-not (Test-Path $cfg)) {
    Set-Content -Path $cfg -Value "[wsl2]`r`nnetworkingMode=mirrored" -Encoding ASCII
    Write-Host "Made $cfg with mirrored networking."
} elseif (Select-String -Path $cfg -Pattern "networkingMode\s*=\s*mirrored" -Quiet) {
    Write-Host "$cfg already has mirrored networking."
} else {
    Write-Host "$cfg already exists. Add these two lines to it yourself:"
    Write-Host "  [wsl2]"
    Write-Host "  networkingMode=mirrored"
}

# 2. Windows firewall (skips a rule that is already there)
function Add-GameRule([string]$Name, [hashtable]$Params) {
    if (Get-NetFirewallRule -DisplayName $Name -ErrorAction SilentlyContinue) {
        Write-Host "Rule already there: $Name"
    } else {
        New-NetFirewallRule -DisplayName $Name -Action Allow -Profile Any @Params | Out-Null
        Write-Host "Added rule: $Name"
    }
}
Add-GameRule "Blast Arena ping"    @{ Direction = "Inbound";  Protocol = "ICMPv4"; IcmpType = 8 }
Add-GameRule "Blast Arena UDP in"  @{ Direction = "Inbound";  Protocol = "UDP"; LocalPort = "7400-7600" }
Add-GameRule "Blast Arena UDP out" @{ Direction = "Outbound"; Protocol = "UDP"; LocalPort = "7400-7600" }
Add-GameRule "Blast Arena join code" @{ Direction = "Inbound"; Protocol = "TCP"; LocalPort = "7390" }

# 3. WSL firewall (Hyper-V). Older Windows may not have this.
$vm = '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}'
try {
    Set-NetFirewallHyperVVMSetting -Name $vm -DefaultInboundAction Allow -ErrorAction Stop
    if (-not (Get-NetFirewallHyperVRule -Name "BlastArenaUdpIn" -ErrorAction SilentlyContinue)) {
        New-NetFirewallHyperVRule -Name "BlastArenaUdpIn" -DisplayName "Blast Arena UDP in" -Direction Inbound -VMCreatorId $vm -Protocol UDP -LocalPorts 7400-7600 -ErrorAction Stop | Out-Null
    }
    if (-not (Get-NetFirewallHyperVRule -Name "BlastArenaJoinCode" -ErrorAction SilentlyContinue)) {
        New-NetFirewallHyperVRule -Name "BlastArenaJoinCode" -DisplayName "Blast Arena join code" -Direction Inbound -VMCreatorId $vm -Protocol TCP -LocalPorts 7390 -ErrorAction Stop | Out-Null
    }
    Write-Host "WSL firewall is set."
} catch {
    Write-Host "Could not set the WSL firewall. Skip this if the game connects anyway."
}

# 4. Network type
Write-Host ""
Get-NetConnectionProfile | ForEach-Object {
    Write-Host ("Network '{0}' is {1}" -f $_.Name, $_.NetworkCategory)
    if ($_.NetworkCategory -eq "Public") {
        Write-Host "  To make it Private:  Set-NetConnectionProfile -Name '$($_.Name)' -NetworkCategory Private"
    }
}

Write-Host ""
Write-Host "Done. Now run:  wsl --shutdown   and open WSL again."
