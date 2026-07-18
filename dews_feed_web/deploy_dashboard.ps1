# Dews Feed auto-deploy: pushes dashboard files to the office Pi whenever
# they've changed AND the Pi is reachable. Runs every 10 min via Task
# Scheduler. Safe to run forever: a hash stamp prevents redundant deploys,
# and unreachable-Pi runs exit silently.
$ErrorActionPreference = "SilentlyContinue"

$src   = "C:\Users\dewit\OneDrive\Documents\Arduino\dews_feed_web"
$tasks = "C:\Users\dewit\OneDrive\Desktop\Claude-Workspace\projects\Patriot Bank\WORK_TASKS.md"
$key   = "C:\Users\dewit\.ssh\dewsfeed_pi"
$pi    = "dew@100.65.128.36"
$stamp = "$src\.last_deploy_hash"
$ssh   = "C:\Windows\System32\OpenSSH\ssh.exe"
$scp   = "C:\Windows\System32\OpenSSH\scp.exe"

# Hash the deployable set (NB: -Path parameter, not pipeline — PS 5.1's
# Get-FileHash does not bind plain strings from the pipeline)
$files = @("$src\index.html", "$src\proxy.py", "$src\secrets.js", $tasks) | Where-Object { Test-Path $_ }
$hash  = ((Get-FileHash -Algorithm SHA256 -Path $files).Hash) -join "|"

# Always sync the task file (cheap); only reboot on dashboard changes
& $scp -i $key -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new $tasks "${pi}:/home/dew/dews_feed_web/work_tasks.md" 2>$null

$old = if (Test-Path $stamp) { Get-Content $stamp -Raw } else { "" }
if ($hash.Trim() -eq $old.Trim()) { exit 0 }   # nothing new to deploy

# Reachable?
& $ssh -i $key -o ConnectTimeout=8 $pi "true" 2>$null
if ($LASTEXITCODE -ne 0) { exit 0 }            # Pi unreachable — retry next run

# Deploy + apply
& $scp -i $key -o ConnectTimeout=8 "$src\index.html" "$src\proxy.py" "$src\secrets.js" "${pi}:/home/dew/dews_feed_web/" 2>$null
if ($LASTEXITCODE -eq 0) {
    Set-Content -Path $stamp -Value $hash -Encoding ascii
    & $ssh -i $key -o ConnectTimeout=8 $pi "sudo reboot" 2>$null
}
