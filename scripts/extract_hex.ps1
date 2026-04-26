$lines = Get-Content -Path "D:\MeetingRecord\tshark_tcs_hex.txt"
$payload = ""

foreach ($line in $lines) {
    if ($line -match "^[0-9a-fA-F]{4}\s+([0-9a-fA-F\s]+)\s+.*") {
        $hexPart = $matches[1] -replace '\s+', ''
        $payload += $hexPart
    }
}

$idx = $payload.IndexOf("28c50101")
if ($idx -ge 0) {
    Write-Host "Found AAC-LD OID at index $idx"
    $start = [math]::Max(0, $idx - 20)
    $len = [math]::Min(200, $payload.Length - $start)
    Write-Host $payload.Substring($start, $len)
} else {
    Write-Host "AAC-LD OID not found in hex dump"
}

$idx2 = $payload.IndexOf("28f1000001")
if ($idx2 -ge 0) {
    Write-Host "Found H.264 OID at index $idx2"
    $start = [math]::Max(0, $idx2 - 20)
    $len = [math]::Min(200, $payload.Length - $start)
    Write-Host $payload.Substring($start, $len)
}
