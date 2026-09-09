param([string]$TestJob, [string]$TestTarget, [int]$TestParentId, [string]$TestInstance)
# Test-only entry point: invokes the exact production restart/apply flow with an
# isolated mutex. Never competes with or closes the user's real NexPlay instance.
. "$PSScriptRoot/../src/update/Updater.ps1" -Mode Library
$Job = $TestJob
try { Apply-Update $TestTarget '0.2.2' $TestParentId $TestInstance }
catch { Write-State 'error' 0 '' $_.Exception.Message; exit 1 }
