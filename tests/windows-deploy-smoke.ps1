$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
$references = @{}
if ($PSVersionTable.PSEdition -eq 'Desktop') { $references.ReferencedAssemblies = 'System.Net.Http' }
Add-Type @references -TypeDefinition @'
using System;
using System.Net.Http;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

public static class LyraDeploymentTls {
    public static void SetAuthority(HttpClientHandler handler, X509Certificate2 authority) {
        var pinned = Convert.ToBase64String(authority.RawData);
        handler.ServerCertificateCustomValidationCallback = (request, certificate, peer, errors) => {
            if (certificate == null || (errors & ~SslPolicyErrors.RemoteCertificateChainErrors) != SslPolicyErrors.None)
                return false;
            using (var chain = new X509Chain()) {
                chain.ChainPolicy.ExtraStore.Add(authority);
                chain.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.1"));
                chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
                chain.ChainPolicy.VerificationFlags = X509VerificationFlags.AllowUnknownCertificateAuthority;
                if (!chain.Build(certificate)) return false;
                foreach (var status in chain.ChainStatus) {
                    if ((status.Status & ~X509ChainStatusFlags.UntrustedRoot) != X509ChainStatusFlags.NoError)
                        return false;
                }
                var root = chain.ChainElements[chain.ChainElements.Count - 1].Certificate;
                return Convert.ToBase64String(root.RawData) == pinned;
            }
        };
    }
}
'@

$utf8 = [Text.UTF8Encoding]::new($false)

function Start-Example([string] $Program, [string] $Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($Program, $Arguments)
    $start.WorkingDirectory = $PSScriptRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = $utf8
    $start.StandardErrorEncoding = $utf8
    foreach ($name in 'PATH', 'LIB', 'INCLUDE', 'LYRA_STATS_INPUT', 'LYRA_STATS_MAX_LINE_BYTES') {
        $start.EnvironmentVariables.Remove($name)
    }
    $inputEncoding = [Console]::InputEncoding
    try {
        [Console]::InputEncoding = $utf8
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

function Confirm-Stats([string] $Program, [string] $Label, [string] $Arguments, [string[]] $Lines = @()) {
    $process = Start-Example $Program $Arguments
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        foreach ($line in $Lines) { $process.StandardInput.WriteLine($line) }
        $process.StandardInput.Close()
        if (!$process.WaitForExit(10000)) { throw "$Label timed out" }
        if ($process.ExitCode -ne 0) { throw "$Label exited $($process.ExitCode): $($stderr.Result)" }
        $summary = $stdout.Result | ConvertFrom-Json
        if ($summary.rows -ne 1 -or $summary.total -ne 7 -or $summary.rejected -ne 1 -or $stderr.Result -notmatch 'line 2:') {
            throw "$Label output differed: $($stdout.Result) $($stderr.Result)"
        }
    } finally {
        Close-Example $process
    }
}

function Start-ExampleServer([string] $Program, [string] $Label, [string] $Arguments) {
    $process = Start-Example $Program $Arguments
    $stderr = $process.StandardError.ReadToEndAsync()
    try {
        $line = $process.StandardOutput.ReadLineAsync()
        if (!$line.Wait(10000)) { throw 'Did not announce its port' }
        if ($process.HasExited) { throw "Exited $($process.ExitCode)" }
        $port = 0
        if (![int]::TryParse($line.Result, [ref] $port) -or $port -lt 1 -or $port -gt 65535) {
            throw "Printed an invalid port: $($line.Result)"
        }
        [pscustomobject] @{ Process = $process; Port = $port; ErrorOutput = $stderr }
    } catch {
        Close-Example $process
        throw "${Label}: $_`n$($stderr.Result)"
    }
}

function Invoke-Https([int] $Port, [string] $Path, [int] $Status, [string] $Json = '') {
    $uri = "https://localhost:$Port$Path"
    $method = if ($Json) { [Net.Http.HttpMethod]::Post } else { [Net.Http.HttpMethod]::Get }
    $request = [Net.Http.HttpRequestMessage]::new($method, $uri)
    try {
        $request.Headers.ConnectionClose = $true
        $request.Headers.ExpectContinue = $false
        if ($Json) { $request.Content = [Net.Http.StringContent]::new($Json, $utf8, 'application/json') }
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try {
            $body = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            if ([int] $response.StatusCode -ne $Status) {
                throw "${method} ${uri}: expected $Status, got $([int] $response.StatusCode): $body"
            }
            $body
        } finally {
            $response.Dispose()
        }
    } finally {
        $request.Dispose()
    }
}

function Confirm-User([string] $Body, [int] $Id, [string] $Name) {
    $user = $Body | ConvertFrom-Json
    if ($user.id -ne $Id -or $user.name -cne $Name) { throw "Expected user $Id ($Name), got $Body" }
}

$database = Join-Path $PSScriptRoot 'users.db'
$certificate = Join-Path $PSScriptRoot 'server.pem'
$key = Join-Path $PSScriptRoot 'server.key'
$ca = Join-Path $PSScriptRoot 'ca.pem'
$authority = [Security.Cryptography.X509Certificates.X509Certificate2]::new($ca)
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('lyra deploy ' + [Guid]::NewGuid().ToString('N'))
[void] [IO.Directory]::CreateDirectory($scratch)
$handler = [Net.Http.HttpClientHandler]::new()
$client = [Net.Http.HttpClient]::new($handler)
try {
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    [LyraDeploymentTls]::SetAuthority($handler, $authority)
    $client.Timeout = [TimeSpan]::FromSeconds(5)
    $data = Join-Path $scratch "$([char] 0x8BB0) input.jsonl"
    $config = Join-Path $scratch "$([char] 0x7EDF) config.toml"
    $station = "$([char] 0x4E1C)$([char] 0x533A)"
    $lines = @(('{"station":"' + $station + '","delta":7}'), 'invalid')
    [IO.File]::WriteAllLines($data, $lines, $utf8)
    $settings = 'input = "{0}"' + "`nmaxLineBytes = 128`n"
    $tls = '--cert "{0}" --key "{1}"' -f $certificate, $key
    foreach ($profile in 'debug', 'release') {
        $stats = Join-Path $PSScriptRoot "$profile/stats.exe"
        Confirm-Stats $stats "$profile JSONL stdin" '' $lines
        [IO.File]::WriteAllText($config, ($settings -f $data.Replace('\', '/')), $utf8)
        Confirm-Stats $stats "$profile JSONL config" ('--config "{0}"' -f $config)
        [IO.File]::WriteAllText($config, ($settings -f 'not-selected.jsonl'), $utf8)
        Confirm-Stats $stats "$profile JSONL override" ('--config "{0}" --input "{1}"' -f $config, $data)

        $users = Join-Path $PSScriptRoot "$profile/users.exe"
        $upstreamDb = Join-Path $scratch "$profile upstream.db"
        $serviceDb = Join-Path $scratch "$profile service.db"
        [IO.File]::Copy($database, $upstreamDb)
        [IO.File]::Copy($database, $serviceDb)
        $upstream = Start-ExampleServer $users "$profile HTTPS upstream" ('"{0}" {1}' -f $upstreamDb, $tls)
        try {
            Confirm-User (Invoke-Https $upstream.Port '/users' 201 '{"name":"Origin"}') 8 'Origin'
            $target = "https://localhost:$($upstream.Port)/users/8"
            $arguments = '"{0}" {1} --upstream "{2}" --ca "{3}"' -f $serviceDb, $tls, $target, $ca
            $service = Start-ExampleServer $users "$profile HTTPS service" $arguments
            try {
                Confirm-User (Invoke-Https $service.Port '/users/7' 200) 7 'Ada'
                $null = Invoke-Https $service.Port '/users/8' 404
                Confirm-User (Invoke-Https $service.Port '/users' 201 '{"name":"Grace"}') 8 'Grace'
                Confirm-User (Invoke-Https $service.Port '/users/8' 200) 8 'Grace'
                Confirm-User (Invoke-Https $service.Port '/upstream' 200) 8 'Origin'
            } finally {
                Close-Example $service.Process
                if ($service.ErrorOutput.Result) { [Console]::Error.WriteLine($service.ErrorOutput.Result) }
            }
        } finally {
            Close-Example $upstream.Process
            if ($upstream.ErrorOutput.Result) { [Console]::Error.WriteLine($upstream.ErrorOutput.Result) }
        }
        Write-Output "PASS $profile JSONL and HTTPS/SQLite standalone"
    }
} finally {
    $client.Dispose()
    $authority.Dispose()
    foreach ($file in [IO.Directory]::EnumerateFiles($scratch)) { [IO.File]::Delete($file) }
    [IO.Directory]::Delete($scratch)
}
