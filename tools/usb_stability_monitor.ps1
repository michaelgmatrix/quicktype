param(
  [string]$Port = "COM11",
  [double]$DurationMinutes = 60,
  [double]$IntervalSeconds = 2,
  [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
if (-not $OutputPath) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $OutputPath = Join-Path $PSScriptRoot "..\diagnostics\usb-stability-$stamp.csv"
}

$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($OutputPath)) | Out-Null
$writer = [System.IO.StreamWriter]::new($OutputPath, $false, [System.Text.UTF8Encoding]::new($false))
$writer.WriteLine("timestamp,uptimeMs,comConnected,nativeMounted,hostInterfaces,keyboardInterfaces,consumerInterfaces,hostMounts,hostUnmounts,requestFails,zeroLength,decodeFails,hostQuiesces,hostRecovers,receiveAborts,receiveAbortFails,maxLoopGapMs,freeHeap")
$writer.Flush()

$serial = $null
$requestId = 3000
$deadline = [DateTime]::UtcNow.AddMinutes($DurationMinutes)
$nextSummary = [DateTime]::UtcNow
$serialFailures = 0
$samples = 0
$anomalies = 0
$lastData = $null

function Close-MonitorPort {
  if ($script:serial) {
    try { if ($script:serial.IsOpen) { $script:serial.Close() } } catch {}
    try { $script:serial.Dispose() } catch {}
    $script:serial = $null
  }
}

try {
  Write-Output "monitor-start port=$Port duration=${DurationMinutes}m log=$OutputPath"
  while ([DateTime]::UtcNow -lt $deadline) {
    try {
      if (-not $serial -or -not $serial.IsOpen) {
        Close-MonitorPort
        $serial = [System.IO.Ports.SerialPort]::new($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $serial.DtrEnable = $true
        $serial.ReadTimeout = 250
        $serial.WriteTimeout = 1000
        $serial.Open()
        Start-Sleep -Milliseconds 350
        $serial.DiscardInBuffer()
      }

      $requestId++
      $serial.WriteLine("{`"qt`":1,`"id`":$requestId,`"command`":`"get-telemetry`"}")
      $responseDeadline = [DateTime]::UtcNow.AddMilliseconds(1400)
      $message = $null
      while ([DateTime]::UtcNow -lt $responseDeadline -and -not $message) {
        try {
          $line = $serial.ReadLine().Trim()
          if (-not $line.StartsWith("{")) { continue }
          $candidate = $line | ConvertFrom-Json
          if ($candidate.id -eq $requestId -and $candidate.type -eq "telemetry") { $message = $candidate }
        } catch [System.TimeoutException] {
        } catch [System.ArgumentException] {
        }
      }
      if (-not $message) { throw "Telemetry timeout" }

      $data = $message.data
      $counters = $data.counters
      $last = $data.last
      $samples++
      $lastData = $data
      $isAnomaly =
        [int]$data.hostInterfaces -ne 2 -or
        [int]$data.keyboardInterfaces -ne 1 -or
        [int]$data.consumerInterfaces -ne 1 -or
        [int]$counters.hostUnmounts -gt 0 -or
        [int]$counters.hostMounts -gt 2 -or
        [int]$counters.hostReportRequestFails -gt 0 -or
        [int]$counters.zeroLengthReports -gt 0 -or
        [int]$counters.keyboardDecodeFails -gt 0 -or
        [int]$counters.hostQuiesces -gt 0 -or
        [int]$counters.hostRecovers -gt 0 -or
        [int]$counters.hostReceiveAbortFails -gt 0
      if ($isAnomaly) {
        $anomalies++
        Write-Output "ANOMALY time=$(Get-Date -Format o) host=$($data.hostInterfaces) mounts=$($counters.hostMounts) unmounts=$($counters.hostUnmounts) reqFail=$($counters.hostReportRequestFails) zlen=$($counters.zeroLengthReports) decodeFail=$($counters.keyboardDecodeFails)"
      }

      $writer.WriteLine((@(
        (Get-Date -Format o), $data.uptimeMs, 1, $data.tinyUsbMounted,
        $data.hostInterfaces, $data.keyboardInterfaces, $data.consumerInterfaces,
        $counters.hostMounts, $counters.hostUnmounts, $counters.hostReportRequestFails,
        $counters.zeroLengthReports, $counters.keyboardDecodeFails, $counters.hostQuiesces,
        $counters.hostRecovers, $counters.hostReceiveAborts, $counters.hostReceiveAbortFails,
        $last.maxLoopGapMs, $data.freeHeap
      ) -join ","))
      $writer.Flush()

      if ([DateTime]::UtcNow -ge $nextSummary) {
        Write-Output "summary time=$(Get-Date -Format o) samples=$samples serialFailures=$serialFailures anomalies=$anomalies host=$($data.hostInterfaces) mounts=$($counters.hostMounts) unmounts=$($counters.hostUnmounts) reqFail=$($counters.hostReportRequestFails) heap=$($data.freeHeap)"
        $nextSummary = [DateTime]::UtcNow.AddSeconds(30)
      }
    } catch {
      $serialFailures++
      $anomalies++
      Write-Output "SERIAL-FAILURE time=$(Get-Date -Format o) count=$serialFailures error=$($_.Exception.Message)"
      $writer.WriteLine("$(Get-Date -Format o),,0,,,,,,,,,,,,,,,,")
      $writer.Flush()
      Close-MonitorPort
      Start-Sleep -Milliseconds 750
    }
    Start-Sleep -Milliseconds ([Math]::Max(100, [int]($IntervalSeconds * 1000)))
  }
} finally {
  Close-MonitorPort
  $writer.Dispose()
}

if ($lastData) {
  Write-Output "monitor-complete samples=$samples serialFailures=$serialFailures anomalies=$anomalies host=$($lastData.hostInterfaces) mounts=$($lastData.counters.hostMounts) unmounts=$($lastData.counters.hostUnmounts) log=$OutputPath"
} else {
  Write-Output "monitor-complete samples=0 serialFailures=$serialFailures anomalies=$anomalies log=$OutputPath"
}
