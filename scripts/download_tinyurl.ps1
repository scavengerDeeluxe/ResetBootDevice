<#
.SYNOPSIS
  Download the target of a tinyurl (or any short URL) and save to disk.

.DESCRIPTION
  Resolves a short URL (follows redirects) and downloads the final resource to a file.
  Uses .NET HttpWebRequest so it works in Windows PowerShell and WinPE/WinRE where
  Invoke-WebRequest may be limited.

.PARAMETER TinyUrl
  The short URL to resolve and download (e.g. https://tinyurl.com/abc123).

.PARAMETER OutFile
  Destination file path. If omitted, the script will try to use the filename from
  the resolved URL or fall back to "downloaded.bin" in the current folder.

.PARAMETER Silent
  If set, suppresses progress output.

Examples:
  .\download_tinyurl.ps1 -TinyUrl "https://tinyurl.com/abc123"
  .\download_tinyurl.ps1 -TinyUrl $env:SHORTLINK -OutFile C:\Temp\cctk.exe

param(
	[Parameter(Mandatory=$true)]
	[string]$TinyUrl,

	[string]$OutFile = "",

	[switch]$Silent
)

function Get-FilenameFromUrl {
	param([string]$url)
	try {
		$uri = [System.Uri]::new($url)
		$name = [System.IO.Path]::GetFileName($uri.LocalPath)
		if ([string]::IsNullOrWhiteSpace($name)) { return $null }
		return $name
	} catch { return $null }
}

Write-Host "Resolving: $TinyUrl"

try {
	$req = [System.Net.HttpWebRequest]::Create($TinyUrl)
	$req.Method = 'GET'
	$req.AllowAutoRedirect = $true
	$req.Timeout = 300000

	$resp = $req.GetResponse()
	$finalUri = $resp.ResponseUri.AbsoluteUri
	Write-Host "Resolved to: $finalUri"

	if ([string]::IsNullOrWhiteSpace($OutFile)) {
		$fname = Get-FilenameFromUrl $finalUri
		if ($null -ne $fname) { $OutFile = Join-Path -Path (Get-Location) -ChildPath $fname } else { $OutFile = Join-Path -Path (Get-Location) -ChildPath 'downloaded.bin' }
	}

	Write-Host "Downloading to: $OutFile"

	$contentLength = 0
	try { $contentLength = [int64]$resp.Headers['Content-Length'] } catch {}

	$inStream = $resp.GetResponseStream()
	$outDir = Split-Path -Path $OutFile -Parent
	if (!(Test-Path -Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

	$outFs = [System.IO.File]::Open($OutFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
	$buffer = New-Object byte[] 81920
	$total = 0
	while (($read = $inStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
		$outFs.Write($buffer, 0, $read)
		$total += $read
		if (-not $Silent) {
			if ($contentLength -gt 0) {
				$percent = [int](($total * 100) / $contentLength)
				Write-Progress -Activity "Downloading" -Status ("{0} / {1} bytes" -f $total, $contentLength) -PercentComplete $percent
			} else {
				Write-Progress -Activity "Downloading" -Status ("{0} bytes" -f $total) -PercentComplete 0
			}
		}
	}
	$outFs.Close(); $inStream.Close(); $resp.Close()

	if (-not $Silent) { Write-Host "Downloaded $total bytes to $OutFile" }
	exit 0

} catch {
	Write-Error "Download failed: $($_.Exception.Message)"
	exit 1
}
