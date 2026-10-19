param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$ExpectedSha256
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Repository -cnotmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or
    $Tag -cnotmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
    $Commit -notmatch '^[0-9a-f]{40}$' -or $ExpectedSha256 -notmatch '^[0-9a-f]{64}$') {
    throw 'Invalid repository, version tag, commit, or SHA-256.'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf) -or
    [IO.Path]::GetFileName($Executable) -cne 'XTray.exe') {
    throw 'XTray.exe is required.'
}
$taskExecutable = (Resolve-Path -LiteralPath $Executable).Path
if ((Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash -ne $ExpectedSha256) {
    throw 'The downloaded build artifact does not match the build-job SHA-256.'
}

function Invoke-GitHub {
    param([Parameter(Mandatory, Position = 0)][string[]]$Arguments)
    $taskOutput = & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "GitHub CLI failed: gh $($Arguments[0]) $($Arguments[1])."
    }
    return $taskOutput
}

function Assert-TagCommit {
    $taskResponse = (Invoke-GitHub @('api', "repos/$Repository/git/ref/tags/$Tag")) -join "`n"
    $taskObject = ($taskResponse | ConvertFrom-Json -AsHashtable).object
    # Resolve both lightweight and annotated tags; reject malformed tag chains.
    for ($taskDepth = 0; $taskDepth -lt 8; ++$taskDepth) {
        if ($taskObject.sha -notmatch '^[0-9a-f]{40}$') {
            throw 'GitHub returned an invalid tag object.'
        }
        if ($taskObject.type -eq 'commit') {
            if ($taskObject.sha -ne $Commit) {
                throw 'The remote tag no longer points to the commit that was built.'
            }
            return
        }
        if ($taskObject.type -ne 'tag') {
            throw 'The remote tag does not resolve to a commit.'
        }
        $taskResponse = (Invoke-GitHub @('api', "repos/$Repository/git/tags/$($taskObject.sha)")) -join "`n"
        $taskObject = ($taskResponse | ConvertFrom-Json -AsHashtable).object
    }
    throw 'The annotated tag chain is too deep.'
}

function Get-TaggedRelease {
    # Drafts use a pending tag; the REST list may expose an untagged placeholder.
    # GraphQL resolves that pending tag, as the GitHub CLI does for draft uploads.
    $taskRepositoryParts = $Repository.Split('/')
    $taskQuery = 'query($owner: String!, $name: String!, $tag: String!) { repository(owner: $owner, name: $name) { release(tagName: $tag) { isDraft } } }'
    $taskResponse = (Invoke-GitHub @(
        'api', 'graphql', '-f', "query=$taskQuery", '-f', "owner=$($taskRepositoryParts[0])",
        '-f', "name=$($taskRepositoryParts[1])", '-f', "tag=$Tag"
    )) -join "`n"
    $taskResult = $taskResponse | ConvertFrom-Json -AsHashtable
    if ($taskResult.Contains('errors') -or $null -eq $taskResult.data.repository) {
        throw 'The release query failed; no release may be created or changed.'
    }
    return $taskResult.data.repository.release
}

function Assert-Draft {
    param($Release)
    if ($null -eq $Release) {
        throw "The release for '$Tag' could not be found; the draft remains unpublished."
    }
    if ($Release.isDraft -isnot [bool] -or -not $Release.isDraft) {
        throw 'Only an unpublished draft may be changed; published versions are never overwritten.'
    }
}

Assert-TagCommit
$taskRelease = Get-TaggedRelease
if ($null -eq $taskRelease) {
    Invoke-GitHub @(
        'release', 'create', $Tag, '--repo', $Repository, '--verify-tag',
        '--target', $Commit, '--draft', '--title', "XTray $Tag", '--generate-notes'
    ) | Write-Host
}
Assert-Draft (Get-TaggedRelease)

# Retrying may replace an incomplete asset, but only while the release is a draft.
Invoke-GitHub @('release', 'upload', $Tag, $taskExecutable, '--repo', $Repository, '--clobber') | Write-Host
$taskVerificationDirectory = Join-Path ([IO.Path]::GetDirectoryName($taskExecutable)) ('.verify-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($taskVerificationDirectory) | Out-Null
Invoke-GitHub @(
    'release', 'download', $Tag, '--repo', $Repository, '--pattern', 'XTray.exe', '--dir', $taskVerificationDirectory
) | Write-Host
$taskUploadedExecutable = Join-Path $taskVerificationDirectory 'XTray.exe'
if (-not (Test-Path -LiteralPath $taskUploadedExecutable -PathType Leaf) -or
    (Get-FileHash -LiteralPath $taskUploadedExecutable -Algorithm SHA256).Hash -ne $ExpectedSha256) {
    throw 'The uploaded release asset failed SHA-256 verification; the draft remains unpublished.'
}

Assert-TagCommit
Assert-Draft (Get-TaggedRelease)
Invoke-GitHub @('release', 'edit', $Tag, '--repo', $Repository, '--verify-tag', '--draft=false') | Write-Host
Write-Host "Published XTray $Tag from $Commit (SHA-256: $ExpectedSha256)."
