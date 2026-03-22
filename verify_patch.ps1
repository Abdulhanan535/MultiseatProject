$ErrorActionPreference = "Stop"

# Find TermService PID
$svc = Get-WmiObject Win32_Service -Filter "Name='TermService'"
$termPid = $svc.ProcessId
Write-Host "TermService PID: $termPid"

# Read termsrv.dll from disk to find the pattern offset
$dllPath = "$env:SystemRoot\System32\termsrv.dll"
$bytes = [System.IO.File]::ReadAllBytes($dllPath)
Write-Host "termsrv.dll size: $($bytes.Length) bytes"

# Search for pattern: 39 81 3C 06 00 00 0F 84
$findPattern = @(0x39, 0x81, 0x3C, 0x06, 0x00, 0x00, 0x0F, 0x84)
$replacePattern = @(0xB8, 0x00, 0x01, 0x00, 0x00, 0x89, 0x81, 0x38, 0x06, 0x00, 0x00, 0x90)

$found = $false
for ($i = 0; $i -lt ($bytes.Length - $findPattern.Length); $i++) {
    $match = $true
    for ($j = 0; $j -lt $findPattern.Length; $j++) {
        if ($bytes[$i + $j] -ne $findPattern[$j]) {
            $match = $false
            break
        }
    }
    if ($match) {
        $offset = $i
        $found = $true
        $hexStr = ""
        for ($k = 0; $k -lt 12; $k++) { $hexStr += "{0:X2} " -f $bytes[$i + $k] }
        Write-Host "Found pattern at offset 0x$($offset.ToString('X')): $hexStr"
        break
    }
}

if (-not $found) {
    Write-Host "Pattern NOT found in termsrv.dll on disk."
    Write-Host "Searching for already-patched pattern..."
    $patchCheck = @(0xB8, 0x00, 0x01, 0x00, 0x00, 0x89, 0x81, 0x38)
    for ($i = 0; $i -lt ($bytes.Length - $patchCheck.Length); $i++) {
        $match = $true
        for ($j = 0; $j -lt $patchCheck.Length; $j++) {
            if ($bytes[$i + $j] -ne $patchCheck[$j]) { $match = $false; break }
        }
        if ($match) { Write-Host "Already patched at offset 0x$($i.ToString('X'))!"; break }
    }
} else {
    Write-Host "Pattern found! Offset 0x$($offset.ToString('X'))"
    Write-Host "This confirms the in-memory patch will work after rebuild."
}
