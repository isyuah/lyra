$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http

function Start-Example([string] $Program, [string] $Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($Program, $Arguments)
    $start.WorkingDirectory = $PSScriptRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($name in 'PATH', 'LIB', 'INCLUDE') { $start.EnvironmentVariables.Remove($name) }
    $inputEncoding = [Console]::InputEncoding
    try {
        [Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
        [Diagnostics.Process]::Start($start)
    } finally {
        [Console]::InputEncoding = $inputEncoding
    }
}

function Close-Example([Diagnostics.Process] $Process) {
    try {
        if (!$Process.HasExited) { $Process.Kill() }
        if (!$Process.WaitForExit(5000)) { throw 'Example did not exit' }
    } finally {
        $Process.Dispose()
    }
}

$database = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'users.db')).Path
$handler = [Net.Http.HttpClientHandler]::new()
$handler.UseProxy = $false
$client = [Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(5)
try {
    foreach ($profile in 'debug', 'release') {
        $stats = Start-Example (Join-Path $PSScriptRoot "$profile/stats.exe") ''
        try {
            $stdout = $stats.StandardOutput.ReadToEndAsync()
            $stderr = $stats.StandardError.ReadToEndAsync()
            $stats.StandardInput.WriteLine('{"station":"west","delta":7}')
            $stats.StandardInput.WriteLine('invalid')
            $stats.StandardInput.Close()
            if (!$stats.WaitForExit(10000)) { throw "$profile JSONL app timed out" }
            if ($stats.ExitCode -ne 0) { throw "$profile JSONL exited $($stats.ExitCode): $($stderr.Result)" }
            $summary = $stdout.Result | ConvertFrom-Json
            if ($summary.rows -ne 1 -or $summary.total -ne 7 -or $summary.rejected -ne 1 -or $stderr.Result -notmatch 'line 2:') {
                throw "$profile JSONL output differed: $($stdout.Result) $($stderr.Result)"
            }
        } finally {
            Close-Example $stats
        }

        $service = Start-Example (Join-Path $PSScriptRoot "$profile/users.exe") ('"' + $database + '"')
        try {
            $stderr = $service.StandardError.ReadToEndAsync()
            $line = $service.StandardOutput.ReadLineAsync()
            if (!$line.Wait(10000)) { throw "$profile HTTP service did not announce its port" }
            if ($service.HasExited) { throw "$profile HTTP service exited $($service.ExitCode): $($stderr.Result)" }
            $port = 0
            if (![int]::TryParse($line.Result, [ref] $port) -or $port -lt 1 -or $port -gt 65535) {
                throw "$profile HTTP service printed an invalid port: $($line.Result)"
            }
            $found = $client.GetAsync("http://127.0.0.1:$port/users/7").GetAwaiter().GetResult()
            $missing = $client.GetAsync("http://127.0.0.1:$port/users/8").GetAwaiter().GetResult()
            try {
                if ([int] $found.StatusCode -ne 200 -or [int] $missing.StatusCode -ne 404) {
                    throw "$profile HTTP status differed: $($found.StatusCode), $($missing.StatusCode)"
                }
                $user = $found.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
                if ($user.id -ne 7 -or $user.name -cne 'Ada') { throw "$profile HTTP returned the wrong user" }
            } finally {
                $found.Dispose()
                $missing.Dispose()
            }
        } finally {
            Close-Example $service
        }
        Write-Output "PASS $profile JSONL and HTTP/SQLite standalone"
    }
} finally {
    $client.Dispose()
}
