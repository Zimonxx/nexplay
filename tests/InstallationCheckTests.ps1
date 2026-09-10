param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($Executable).ProductName -cne 'NexPlay') {
    throw 'Expected a packaged NexPlay executable'
}
# Run under the SAME Windows PowerShell host as the production updater.
# Its Start-Process appends whitespace, unlike PowerShell 7 used for packaging.
foreach ($arguments in @('--verify-installation', '--verify-installation  ', '"--verify-installation"')) {
    $probe = Start-Process $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    try {
        if (!$probe.WaitForExit(10000)) { $probe.Kill(); throw 'Headless package check did not exit promptly' }
        if ($probe.ExitCode -ne 0) { throw "Package check failed: $($probe.ExitCode)" }
    } finally { $probe.Dispose() }
}
Write-Output 'PASS: packaged headless verification under Windows PowerShell, including trailing spaces and quoted arguments.'
