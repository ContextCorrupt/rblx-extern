param(
    [Parameter(Mandatory = $true)][string]$DefaultExecutable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$WebhookUrl
)

$ErrorActionPreference = 'Continue'

function Resolve-ExecutablePath {
    param(
        [string]$DefaultExecutable,
        [string]$OutputDirectory
    )

    $latestLog = Join-Path $OutputDirectory "latest_build_name.txt"
    if (Test-Path $latestLog) {
        try {
            $name = Get-Content $latestLog -ErrorAction Stop | Select-Object -First 1
            if ($name) {
                $candidate = Join-Path $OutputDirectory $name.Trim()
                if (Test-Path $candidate) {
                    Write-Host "upload_build.ps1: resolved randomized executable: $candidate"
                    return $candidate
                }
            }
        }
        catch {
            Write-Host "upload_build.ps1: failed reading latest_build_name.txt - $_"
        }
    }

    if (Test-Path $DefaultExecutable) {
        Write-Host "upload_build.ps1: using default executable: $DefaultExecutable"
        return $DefaultExecutable
    }

    return $null
}

if ($env:CRADLE_SKIP_WEBHOOK -eq "1") {
    Write-Host "upload_build.ps1: CRADLE_SKIP_WEBHOOK set, skipping webhook upload."
    exit 0
}

if (-not $WebhookUrl) {
    Write-Host "upload_build.ps1: Webhook URL missing; aborting upload."
    exit 0
}

$exePath = Resolve-ExecutablePath -DefaultExecutable $DefaultExecutable -OutputDirectory $OutputDirectory
if (-not $exePath) {
    Write-Host "upload_build.ps1: executable not found, skipping upload."
    exit 0
}

# Try curl.exe first (Windows 10+ native)
$curl = Get-Command curl.exe -ErrorAction SilentlyContinue
if (-not $curl) {
    Write-Host "upload_build.ps1: curl.exe not found in PATH; skipping upload."
    exit 0
}

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"
$fileName = [System.IO.Path]::GetFileName($exePath)
$fileSize = (Get-Item $exePath).Length
$fileSizeKB = [math]::Round($fileSize / 1KB, 2)

$payload = @{ 
    content = "**New cradle build uploaded**`n:clock1: $timestamp`n:file_folder: ``$fileName`` ($fileSizeKB KB)" 
}
$payloadJson = $payload | ConvertTo-Json -Compress

Write-Host "upload_build.ps1: uploading $fileName ($fileSizeKB KB) to Discord webhook..."

try {
    $curlArgs = @(
        '-s',
        '-X', 'POST',
        '-F', "payload_json=$payloadJson",
        '-F', "file=@$exePath;filename=$fileName",
        $WebhookUrl
    )
    
    & $curl.Source $curlArgs 2>&1 | Out-Null
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "upload_build.ps1: curl upload failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
    else {
        Write-Host "upload_build.ps1: build uploaded successfully."
    }
}
catch {
    Write-Host "upload_build.ps1: unexpected error invoking curl - $_"
    exit 1
}

exit 0
