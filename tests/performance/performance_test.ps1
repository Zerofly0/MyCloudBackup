param(
    [string]$BaseUrl = "http://8.137.100.109:8080",
    [int]$FileSizeMB = 100,
    [int]$Concurrency = 10,
    [int]$RequestsPerWorker = 5,
    [switch]$KeepFiles
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Write-Section([string]$Title) {
    Write-Host ""
    Write-Host ("=" * 64)
    Write-Host $Title
    Write-Host ("=" * 64)
}

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "找不到命令：$Name"
    }
}

function Invoke-CurlMetrics {
    param(
        [string[]]$Arguments
    )
    $output = & curl.exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "curl 执行失败，退出码：$LASTEXITCODE"
    }
    return ($output | Out-String).Trim()
}

function Parse-CurlMetrics {
    param(
        [string]$Text,
        [string]$Stage
    )
    $parts = $Text.Split(",")
    if ($parts.Count -lt 4) {
        throw "$Stage 指标解析失败：$Text"
    }

    [pscustomobject]@{
        Stage        = $Stage
        HttpCode     = [int]$parts[0]
        TimeSeconds  = [double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture)
        SpeedBytesPS = [double]::Parse($parts[2], [Globalization.CultureInfo]::InvariantCulture)
        SizeBytes    = [double]::Parse($parts[3], [Globalization.CultureInfo]::InvariantCulture)
    }
}

function Run-ConcurrentGetTest {
    param(
        [string]$Name,
        [string]$Url,
        [int]$Workers,
        [int]$Loops,
        [string]$CsvPath
    )

    Write-Host "并发测试：$Name"
    Write-Host "URL：$Url"
    Write-Host "并发数：$Workers；每个并发请求数：$Loops；总请求数：$($Workers * $Loops)"

    $wall = [Diagnostics.Stopwatch]::StartNew()

    $jobs = 1..$Workers | ForEach-Object {
        $workerId = $_
        Start-Job -ArgumentList $Url, $Loops, $workerId, $Name -ScriptBlock {
            param($TargetUrl, $LoopCount, $WorkerId, $TestName)

            $rows = @()
            for ($i = 1; $i -le $LoopCount; $i++) {
                $sw = [Diagnostics.Stopwatch]::StartNew()
                $status = 0
                $success = $false
                $errorMessage = ""

                try {
                    $response = Invoke-WebRequest -UseBasicParsing -Uri $TargetUrl -Method Get -TimeoutSec 60
                    $status = [int]$response.StatusCode
                    $success = ($status -ge 200 -and $status -lt 300)
                }
                catch {
                    if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
                        $status = [int]$_.Exception.Response.StatusCode
                    }
                    $errorMessage = $_.Exception.Message
                }
                finally {
                    $sw.Stop()
                }

                $rows += [pscustomobject]@{
                    Test       = $TestName
                    Worker     = $WorkerId
                    Iteration  = $i
                    Success    = $success
                    StatusCode = $status
                    ElapsedMs  = [math]::Round($sw.Elapsed.TotalMilliseconds, 3)
                    Error      = $errorMessage
                }
            }
            return $rows
        }
    }

    $rows = $jobs | Receive-Job -Wait -AutoRemoveJob
    $wall.Stop()

    $rows | Export-Csv -Path $CsvPath -NoTypeInformation -Encoding UTF8

    $total = @($rows).Count
    $ok = @($rows | Where-Object { $_.Success -eq $true }).Count
    $failed = $total - $ok
    $avg = ($rows | Measure-Object -Property ElapsedMs -Average).Average
    $min = ($rows | Measure-Object -Property ElapsedMs -Minimum).Minimum
    $max = ($rows | Measure-Object -Property ElapsedMs -Maximum).Maximum
    $errorRate = if ($total -gt 0) { 100.0 * $failed / $total } else { 0 }
    $throughput = if ($wall.Elapsed.TotalSeconds -gt 0) { $total / $wall.Elapsed.TotalSeconds } else { 0 }

    [pscustomobject]@{
        Test              = $Name
        TotalRequests     = $total
        Successful        = $ok
        Failed            = $failed
        ErrorRatePercent  = [math]::Round($errorRate, 2)
        AverageMs         = [math]::Round($avg, 2)
        MinimumMs         = [math]::Round($min, 2)
        MaximumMs         = [math]::Round($max, 2)
        WallTimeSeconds   = [math]::Round($wall.Elapsed.TotalSeconds, 2)
        ThroughputReqPerS = [math]::Round($throughput, 2)
    }
}

try {
    Require-Command "curl.exe"

    if ($FileSizeMB -le 0) { throw "FileSizeMB 必须大于 0。" }
    if ($Concurrency -le 0) { throw "Concurrency 必须大于 0。" }
    if ($RequestsPerWorker -le 0) { throw "RequestsPerWorker 必须大于 0。" }

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    $resultDir = Join-Path $scriptRoot "performance_results_$timestamp"
    New-Item -ItemType Directory -Path $resultDir -Force | Out-Null

    $testFile = Join-Path $resultDir "perf_${FileSizeMB}MB.bin"
    $downloadFile = Join-Path $resultDir "downloaded_perf_${FileSizeMB}MB.bin"
    $remoteName = "perf_${FileSizeMB}MB_$timestamp.bin"
    $transferCsv = Join-Path $resultDir "transfer_metrics.csv"
    $summaryCsv = Join-Path $resultDir "summary.csv"

    Write-Section "个人云备份服务器自动化性能测试"
    Write-Host "服务器：$BaseUrl"
    Write-Host "测试文件：$FileSizeMB MB"
    Write-Host "并发数：$Concurrency"
    Write-Host "每个并发请求数：$RequestsPerWorker"
    Write-Host "结果目录：$resultDir"

    Write-Section "[1] 健康检查"
    $health = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/health" -TimeoutSec 15
    Write-Host "HTTP 状态码：$($health.StatusCode)"
    Write-Host "响应内容：$($health.Content)"
    if ($health.StatusCode -ne 200) {
        throw "服务器健康检查失败。"
    }

    Write-Section "[2] 创建测试文件"
    $bytes = [int64]$FileSizeMB * 1MB
    $stream = [IO.File]::Open($testFile, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.SetLength($bytes)
    }
    finally {
        $stream.Dispose()
    }
    Write-Host "已创建：$testFile"
    Write-Host "实际大小：$((Get-Item $testFile).Length) 字节"

    Write-Section "[3] 上传性能测试"
    $uploadResponse = Join-Path $resultDir "upload_response.json"
    $uploadText = Invoke-CurlMetrics -Arguments @(
        "-sS",
        "-o", $uploadResponse,
        "-w", "%{http_code},%{time_total},%{speed_upload},%{size_upload}",
        "-X", "POST",
        "--data-binary", "@$testFile",
        "$BaseUrl/upload?filename=$remoteName"
    )
    $uploadMetric = Parse-CurlMetrics -Text $uploadText -Stage "Upload"
    $uploadMetric | Format-List
    if ($uploadMetric.HttpCode -lt 200 -or $uploadMetric.HttpCode -ge 300) {
        throw "上传失败，HTTP 状态码：$($uploadMetric.HttpCode)"
    }

    Write-Section "[4] 下载性能测试"
    $downloadText = Invoke-CurlMetrics -Arguments @(
        "-sS",
        "-o", $downloadFile,
        "-w", "%{http_code},%{time_total},%{speed_download},%{size_download}",
        "$BaseUrl/download/$remoteName"
    )
    $downloadMetric = Parse-CurlMetrics -Text $downloadText -Stage "Download"
    $downloadMetric | Format-List
    if ($downloadMetric.HttpCode -lt 200 -or $downloadMetric.HttpCode -ge 300) {
        throw "下载失败，HTTP 状态码：$($downloadMetric.HttpCode)"
    }

    Write-Section "[5] 文件完整性校验"
    $sourceHash = (Get-FileHash -Path $testFile -Algorithm SHA256).Hash
    $downloadHash = (Get-FileHash -Path $downloadFile -Algorithm SHA256).Hash
    Write-Host "上传前 SHA-256：$sourceHash"
    Write-Host "下载后 SHA-256：$downloadHash"
    if ($sourceHash -ne $downloadHash) {
        throw "SHA-256 不一致，文件完整性校验失败。"
    }
    Write-Host "SHA-256 一致，文件完整性校验通过。"

    $uploadMetric, $downloadMetric | Export-Csv -Path $transferCsv -NoTypeInformation -Encoding UTF8

    Write-Section "[6] 并发健康检查"
    $healthSummary = Run-ConcurrentGetTest `
        -Name "Health" `
        -Url "$BaseUrl/health" `
        -Workers $Concurrency `
        -Loops $RequestsPerWorker `
        -CsvPath (Join-Path $resultDir "health_requests.csv")
    $healthSummary | Format-List

    Write-Section "[7] 并发列表查询"
    $listSummary = Run-ConcurrentGetTest `
        -Name "List" `
        -Url "$BaseUrl/list" `
        -Workers $Concurrency `
        -Loops $RequestsPerWorker `
        -CsvPath (Join-Path $resultDir "list_requests.csv")
    $listSummary | Format-List

    Write-Section "[8] 删除云端测试文件"
    $delete = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/delete/$remoteName" -Method Delete -TimeoutSec 60
    Write-Host "HTTP 状态码：$($delete.StatusCode)"
    Write-Host "响应内容：$($delete.Content)"

    $uploadMBps = $uploadMetric.SpeedBytesPS / 1MB
    $downloadMBps = $downloadMetric.SpeedBytesPS / 1MB

    $summary = @(
        [pscustomobject]@{
            Item = "Upload"
            Value = [math]::Round($uploadMetric.TimeSeconds, 3)
            Unit = "seconds"
            Extra = "$([math]::Round($uploadMBps, 2)) MB/s"
        },
        [pscustomobject]@{
            Item = "Download"
            Value = [math]::Round($downloadMetric.TimeSeconds, 3)
            Unit = "seconds"
            Extra = "$([math]::Round($downloadMBps, 2)) MB/s"
        },
        [pscustomobject]@{
            Item = "HealthConcurrent"
            Value = $healthSummary.AverageMs
            Unit = "average ms"
            Extra = "$($healthSummary.ThroughputReqPerS) req/s; error $($healthSummary.ErrorRatePercent)%"
        },
        [pscustomobject]@{
            Item = "ListConcurrent"
            Value = $listSummary.AverageMs
            Unit = "average ms"
            Extra = "$($listSummary.ThroughputReqPerS) req/s; error $($listSummary.ErrorRatePercent)%"
        }
    )
    $summary | Export-Csv -Path $summaryCsv -NoTypeInformation -Encoding UTF8

    Write-Section "测试完成"
    Write-Host "上传耗时：$([math]::Round($uploadMetric.TimeSeconds, 3)) 秒"
    Write-Host "上传速度：$([math]::Round($uploadMBps, 2)) MB/s"
    Write-Host "下载耗时：$([math]::Round($downloadMetric.TimeSeconds, 3)) 秒"
    Write-Host "下载速度：$([math]::Round($downloadMBps, 2)) MB/s"
    Write-Host "并发健康检查错误率：$($healthSummary.ErrorRatePercent)%"
    Write-Host "并发列表查询错误率：$($listSummary.ErrorRatePercent)%"
    Write-Host "结果已保存到：$resultDir"

    if (-not $KeepFiles) {
        Remove-Item $testFile, $downloadFile -Force -ErrorAction SilentlyContinue
        Write-Host "本地大文件已清理；CSV 和响应结果已保留。"
    }
}
catch {
    Write-Host ""
    Write-Host "测试失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
