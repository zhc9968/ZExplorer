$ErrorActionPreference = 'Stop'

function Send-Frame($pipe, [uint32]$type, [string[]]$argv) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($a in $argv) { [void]$sb.Append($a); [void]$sb.Append([char]0) }
    $payload = [System.Text.Encoding]::Unicode.GetBytes($sb.ToString())
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write($type); $bw.Write([uint32]$payload.Length); $bw.Write($payload)
    $b = $ms.ToArray()
    $pipe.Write($b, 0, $b.Length); $pipe.Flush()
}

function Recv-Frame($pipe) {
    $hdr = New-Object byte[] 8
    $read = 0
    while ($read -lt 8) { $n = $pipe.Read($hdr, $read, 8 - $read); if ($n -le 0) { break }; $read += $n }
    if ($read -lt 8) { return @() }
    $bytes = [BitConverter]::ToUInt32($hdr, 4)
    $buf = New-Object byte[] $bytes
    $got = 0
    while ($got -lt $bytes) { $n = $pipe.Read($buf, $got, $bytes - $got); if ($n -le 0) { break }; $got += $n }
    return @(([System.Text.Encoding]::Unicode.GetString($buf)).Split([char]0) | Where-Object { $_ -ne '' })
}

function Call([uint32]$type, [string[]]$argv) {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'ZExplorer.Host', [System.IO.Pipes.PipeDirection]::InOut)
    $p.Connect(2000)
    Send-Frame -pipe $p -type $type -argv $argv
    $r = Recv-Frame -pipe $p
    $p.Dispose()
    return $r
}

$root = 'C:\project\ZExplorer\tmp_fast'
if (Test-Path $root) { Remove-Item $root -Recurse -Force }

Write-Host ("ping before : " + ((Call -type 1 -argv @()) -join ' | '))
Write-Host ("submit      : " + ((Call -type 2 -argv @('newfolder', $root)) -join ' | '))
Start-Sleep -Milliseconds 1200
Write-Host ("created     : " + (Test-Path $root))
Write-Host ("ping after  : " + ((Call -type 1 -argv @()) -join ' | ') + "   (第3项=任务窗口数，快速任务应为 0)")
