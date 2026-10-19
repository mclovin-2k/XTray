param(
    [Parameter(Mandatory)][string]$VersionFile,
    [string]$Tag = '',
    [string]$NotesDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$taskVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($taskVersion -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw 'The project version must be MAJOR.MINOR.PATCH without leading zeros.'
}
foreach ($taskPart in $taskVersion.Split('.')) {
    $taskNumber = 0
    if (-not [int]::TryParse($taskPart, [ref]$taskNumber) -or $taskNumber -gt 65535) {
        throw 'Each version component must fit in a Windows version-resource WORD (0-65535).'
    }
}
if ($Tag) {
    if ($Tag -cne "v$taskVersion") {
        throw "Release tag '$Tag' does not match project version 'v$taskVersion'."
    }
    if (-not $NotesDirectory -or -not (Test-Path -LiteralPath (Join-Path $NotesDirectory "$Tag.md") -PathType Leaf)) {
        throw "Release notes for '$Tag' are missing."
    }
}
Write-Output $taskVersion
