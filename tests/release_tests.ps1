Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$taskProject = Split-Path $PSScriptRoot
$taskPublisher = Join-Path $taskProject 'tools/ci/publish-release.ps1'
$taskValidator = Join-Path $taskProject 'tools/ci/validate-version.ps1'
$taskFixtures = Join-Path $taskProject ('build/release-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($taskFixtures) | Out-Null
$taskExecutable = Join-Path $taskFixtures 'XTray.exe'
[IO.File]::WriteAllText($taskExecutable, 'A controlled build artifact; not an executable.')
$taskHash = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash
$taskCommit = '1' * 40

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

# This function shadows gh in the child script. No test contacts GitHub.
function gh {
    $taskArguments = @($args)
    $taskState = $global:XTrayReleaseTestState
    $global:LASTEXITCODE = 0
    if ($taskArguments[0] -eq 'api') {
        $taskEndpoint = $taskArguments[-1]
        if ($taskEndpoint -like '*/git/ref/tags/*' -or $taskEndpoint -like '*/git/tags/*') {
            $taskRemoteCommit = $taskState.Commit
            if ($taskState.MovedTag -or ($taskState.MoveAfterUpload -and $taskState.Downloaded)) {
                $taskRemoteCommit = '3' * 40
            }
            if ($taskState.Annotated -and $taskEndpoint -like '*/git/ref/tags/*') {
                return (@{ object = @{ type = 'tag'; sha = ('2' * 40) } } | ConvertTo-Json -Compress)
            }
            return (@{ object = @{ type = 'commit'; sha = $taskRemoteCommit } } | ConvertTo-Json -Compress)
        }
        if ($taskArguments[1] -eq 'graphql') {
            if ($taskState.LookupFailure) {
                $global:LASTEXITCODE = 1
                return
            }
            Assert-Test ($taskArguments -contains 'owner=example' -and $taskArguments -contains 'name=XTray' -and $taskArguments -contains 'tag=v0.1.0') 'Release lookup omitted the repository or pending tag.'
            $taskQuery = @($taskArguments | Where-Object { $_ -like 'query=*' })
            Assert-Test ($taskQuery.Count -eq 1 -and $taskQuery[0].Contains('release(tagName: $tag)')) 'Release lookup does not resolve the pending draft tag.'
            if ($taskState.GraphqlErrors) {
                return '{"data":{"repository":null},"errors":[{"message":"Release lookup failed."}]}'
            }
            if ($taskState.MissingRepository) {
                return '{"data":{"repository":null}}'
            }
            if ($taskState.Release -eq 'none' -or
                ($taskState.MissingAfterCreate -and $taskState.Calls.Contains('create')) -or
                ($taskState.MissingBeforePublish -and $taskState.Downloaded)) {
                return '{"data":{"repository":{"release":null}}}'
            }
            $taskDraft = if ($taskState.Release -eq 'draft') { 'true' } else { 'false' }
            if ($taskState.MalformedDraft) { $taskDraft = '"false"' }
            # A draft's REST tag_name can be untagged-...; resolve its pending tag instead.
            return ('{"data":{"repository":{"release":{"isDraft":' + $taskDraft + '}}}}')
        }
        throw "Unexpected test API endpoint: $taskEndpoint"
    }

    Assert-Test ($taskArguments[0] -eq 'release') 'Unexpected GitHub CLI command.'
    $taskCommand = $taskArguments[1]
    $taskState.Calls.Add($taskCommand)
    switch ($taskCommand) {
        'create' {
            Assert-Test ($taskArguments -contains '--verify-tag' -and $taskArguments -contains '--draft') 'A release must start as a verified-tag draft.'
            Assert-Test ($taskArguments -contains '--generate-notes' -and $taskArguments -notcontains '--notes-file') 'A release must use GitHub-generated notes without a local notes file.'
            if ($taskState.CreateFailure) {
                $global:LASTEXITCODE = 1
                return
            }
            $taskState.Release = 'draft'
        }
        'upload' {
            Assert-Test ($taskState.Release -eq 'draft') 'An upload targeted a published release.'
            if ($taskState.UploadFailure) {
                $global:LASTEXITCODE = 1
            }
        }
        'download' {
            $taskDestinationIndex = [Array]::IndexOf($taskArguments, '--dir') + 1
            Assert-Test ($taskDestinationIndex -gt 0) 'Download destination is missing.'
            $taskDestination = Join-Path $taskArguments[$taskDestinationIndex] 'XTray.exe'
            if ($taskState.CorruptDownload) {
                [IO.File]::WriteAllText($taskDestination, 'An incomplete or different uploaded asset.')
            } else {
                Copy-Item -LiteralPath $taskState.Executable -Destination $taskDestination
            }
            $taskState.Downloaded = $true
        }
        'edit' {
            Assert-Test ($taskState.Release -eq 'draft' -and $taskState.Downloaded) 'Publication occurred before verified draft upload.'
            Assert-Test ($taskArguments -contains '--draft=false') 'Publication flag is missing.'
            $taskState.Release = 'published'
        }
        default { throw "Unexpected release command in test: $taskCommand" }
    }
}

$taskCases = @(
    @{ Name = 'new draft with a pending tag'; Changes = @{}; Error = ''; Calls = 'create,upload,download,edit' }
    @{ Name = 'release note generation failure prevents upload'; Changes = @{ CreateFailure = $true }; Error = 'GitHub CLI failed'; Calls = 'create' }
    @{ Name = 'annotated tag'; Changes = @{ Annotated = $true }; Error = ''; Calls = 'create,upload,download,edit' }
    @{ Name = 'resume existing draft by its pending tag'; Changes = @{ Release = 'draft' }; Error = ''; Calls = 'upload,download,edit' }
    @{ Name = 'published version cannot be overwritten'; Changes = @{ Release = 'published' }; Error = 'published versions are never overwritten'; Calls = '' }
    @{ Name = 'moved tag rejected before creation'; Changes = @{ MovedTag = $true }; Error = 'no longer points'; Calls = '' }
    @{ Name = 'tag moved during upload'; Changes = @{ MoveAfterUpload = $true }; Error = 'no longer points'; Calls = 'create,upload,download' }
    @{ Name = 'upload failure leaves a draft'; Changes = @{ UploadFailure = $true }; Error = 'GitHub CLI failed'; Calls = 'create,upload' }
    @{ Name = 'uploaded hash mismatch leaves a draft'; Changes = @{ CorruptDownload = $true }; Error = 'failed SHA-256 verification'; Calls = 'create,upload,download' }
    @{ Name = 'API lookup failure cannot create a release'; Changes = @{ LookupFailure = $true }; Error = 'GitHub CLI failed'; Calls = '' }
    @{ Name = 'GraphQL errors cannot create a release'; Changes = @{ GraphqlErrors = $true }; Error = 'release query failed'; Calls = '' }
    @{ Name = 'inaccessible repository cannot create a release'; Changes = @{ MissingRepository = $true }; Error = 'release query failed'; Calls = '' }
    @{ Name = 'missing newly created draft prevents upload'; Changes = @{ MissingAfterCreate = $true }; Error = 'could not be found'; Calls = 'create' }
    @{ Name = 'missing verified draft prevents publication'; Changes = @{ MissingBeforePublish = $true }; Error = 'could not be found'; Calls = 'create,upload,download' }
    @{ Name = 'non-boolean draft status prevents upload'; Changes = @{ Release = 'draft'; MalformedDraft = $true }; Error = 'published versions are never overwritten'; Calls = '' }
    @{ Name = 'build artifact hash mismatch'; Changes = @{ BadHash = $true }; Error = 'does not match the build-job'; Calls = '' }
    @{ Name = 'invalid version tag'; Changes = @{ BadTag = $true }; Error = 'Invalid repository'; Calls = '' }
)

try {
    foreach ($taskCase in $taskCases) {
        $global:XTrayReleaseTestState = @{
            Commit = $taskCommit; Executable = $taskExecutable; Release = 'none'
            Annotated = $false; MovedTag = $false; MoveAfterUpload = $false
            Downloaded = $false; UploadFailure = $false; CorruptDownload = $false; CreateFailure = $false
            LookupFailure = $false; GraphqlErrors = $false; MissingRepository = $false
            MissingAfterCreate = $false; MissingBeforePublish = $false; MalformedDraft = $false
            BadHash = $false; BadTag = $false
            Calls = [Collections.Generic.List[string]]::new()
        }
        foreach ($taskKey in $taskCase.Changes.Keys) {
            $global:XTrayReleaseTestState[$taskKey] = $taskCase.Changes[$taskKey]
        }
        $taskParameters = @{
            Repository = 'example/XTray'; Tag = 'v0.1.0'; Commit = $taskCommit
            Executable = $taskExecutable; ExpectedSha256 = $taskHash
        }
        if ($global:XTrayReleaseTestState.BadHash) { $taskParameters.ExpectedSha256 = '0' * 64 }
        if ($global:XTrayReleaseTestState.BadTag) { $taskParameters.Tag = 'v0.1.0-preview' }
        $taskError = ''
        try {
            & $taskPublisher @taskParameters
        } catch {
            $taskError = $_.Exception.Message
        }
        if ($taskCase.Error) {
            Assert-Test ($taskError.Contains($taskCase.Error)) "Unexpected failure for '$($taskCase.Name)': $taskError"
        } else {
            Assert-Test (-not $taskError) "Unexpected failure for '$($taskCase.Name)': $taskError"
        }
        Assert-Test (($global:XTrayReleaseTestState.Calls -join ',') -ceq $taskCase.Calls) "Unexpected mutation sequence for '$($taskCase.Name)'."
        Write-Host "Release test passed: $($taskCase.Name)."
    }

    $taskVersionFile = Join-Path $taskFixtures 'version.txt'
    $taskVersionCases = @(
        @{ Version = '0.1.0'; Tag = 'v0.1.0'; Error = '' }
        @{ Version = '0.1.0'; Tag = ''; Error = '' }
        @{ Version = '0.1.0'; Tag = 'v0.1.1'; Error = 'does not match' }
        @{ Version = '01.1.0'; Tag = ''; Error = 'without leading zeros' }
        @{ Version = '1.2'; Tag = ''; Error = 'without leading zeros' }
        @{ Version = '65536.0.0'; Tag = ''; Error = '0-65535' }
        @{ Version = '999999999999999999999.0.0'; Tag = ''; Error = '0-65535' }
    )
    foreach ($taskCase in $taskVersionCases) {
        [IO.File]::WriteAllText($taskVersionFile, $taskCase.Version)
        $taskError = ''
        try {
            $taskResult = & $taskValidator -VersionFile $taskVersionFile -Tag $taskCase.Tag
        } catch {
            $taskError = $_.Exception.Message
        }
        if ($taskCase.Error) {
            Assert-Test ($taskError.Contains($taskCase.Error)) "Unexpected version validation failure: $taskError"
        } else {
            Assert-Test (-not $taskError -and $taskResult -ceq $taskCase.Version) 'A valid project version was rejected.'
        }
        Write-Host "Version test passed: $($taskCase.Version) / $($taskCase.Tag)."
    }
    Write-Host 'Release and version validation: 24 cases passed; no GitHub access or releases were used.'
    $global:LASTEXITCODE = 0
} finally {
    Remove-Variable -Name XTrayReleaseTestState -Scope Global -ErrorAction SilentlyContinue
}
