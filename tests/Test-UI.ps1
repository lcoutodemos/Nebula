param(
    [int]$AppPid = 0,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('ARM64', 'x64')][string]$Architecture = $(if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'ARM64' } else { 'x64' }),
    [string]$WindowHandle,
    [Parameter(Mandatory = $true)][string]$LibraryJson,
    [string]$SearchTerm = 'Islamic Art',
    [int]$ExpectedMatches = 4,
    [string]$Artifacts = (Join-Path $env:LOCALAPPDATA ('Nebula\TestResults\ui-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
$exe = (Get-Command winapp -ErrorAction Stop).Source
$results = [Collections.Generic.List[object]]::new()
$screenshots = [Collections.Generic.List[string]]::new()
$oldWorkflow = $env:WINAPP_UI_WORKFLOW_ID
$env:WINAPP_UI_WORKFLOW_ID = 'nebula-acceptance-' + [guid]::NewGuid().ToString('N')
New-Item -ItemType Directory -Path $Artifacts -Force | Out-Null
$library = Get-Content -LiteralPath $LibraryJson -Raw -Encoding utf8 | ConvertFrom-Json
$count = $library.Photos.Count
$first = $library.Photos | Where-Object { ($_ | ConvertTo-Json -Depth 3) -match [regex]::Escape($SearchTerm) } | Select-Object -First 1
if (-not $first) { throw "No library photo matches '$SearchTerm'." }
$allStatus = "$($count.ToString('N0')) photos · "
$movie = $null

function Ui {
    param([string[]]$Arguments)
    $output = & $exe ui @Arguments --on local -a $AppPid
    if ($LASTEXITCODE -ne 0) { throw "UI command failed: $($Arguments -join ' '): $($output -join "`n")" }
    return $output
}
function Test-Case {
    param([string]$Name, [scriptblock]$Action)
    try {
        & $Action | Out-Null
        $results.Add(@{ name = $Name; status = 'PASS' })
    } catch {
        $results.Add(@{ name = $Name; status = 'FAIL'; detail = $_.Exception.Message })
        Write-Warning "${Name}: $($_.Exception.Message)"
    }
}
function Screenshot {
    param([string]$Name)
    $path = Join-Path $Artifacts "$Name.png"
    Ui @('hover', 'SearchBox') | Out-Null
    Start-Sleep -Milliseconds 180
    & $exe ui screenshot -w $WindowHandle --on local --output $path --json | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $path)) { throw "Screenshot failed: $Name" }
    $screenshots.Add($path)
}
function Clear-Search {
    Ui @('send-keys', 'ctrl+a', '--target', 'SearchBox', '--via', 'send-input') | Out-Null
    Ui @('send-keys', 'backspace', '--via', 'send-input') | Out-Null
    Ui @('wait-for', 'LibraryStatus', '--value', $allStatus, '--contains', '-t', '10000') | Out-Null
}
function Set-Arrangement {
    param([string]$Mode)
    $segment = (& $exe ui search "Mode$Mode" --on local -a $AppPid --json | ConvertFrom-Json).matches |
        Where-Object automationId -eq "Mode$Mode" | Select-Object -First 1
    if ($segment) { Ui @('invoke', "Mode$Mode", '--action', 'select') | Out-Null }
    else { Ui @('invoke', 'CompactArrangements') | Out-Null; Ui @('invoke', "Menu$Mode") | Out-Null }
}
function Type-Search {
    param([string]$Text)
    Ui @('send-keys', 'ctrl+a', '--target', 'SearchBox', '--via', 'send-input') | Out-Null
    Ui @('send-keys', $Text, '--target', 'SearchBox', '--via', 'send-input', '--verbatim') | Out-Null
}
try {
    if ($AppPid -eq 0) {
        $project = Join-Path $PSScriptRoot '..\src\Nebula.Windows\Nebula.Windows.csproj'
        $launch = (& $exe run $project --arch $Architecture.ToLowerInvariant() -c $Configuration --no-build --no-restore --detach --json --on local) | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or -not $launch.ProcessId) { throw 'Packaged app activation did not return a PID.' }
        $AppPid = [int]$launch.ProcessId
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    while (-not $WindowHandle -and [DateTime]::UtcNow -lt $deadline) {
        $windows = @(& $exe ui list-windows --on local -a $AppPid --json | ConvertFrom-Json)
        $main = $windows | Where-Object { $_.title -eq 'Nebula' -and $_.hwnd } | Select-Object -First 1
        if ($main) { $WindowHandle = [string]$main.hwnd; break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $WindowHandle) { throw "No visible Nebula window after activation (PID $AppPid); inspect application.log." }
    Test-Case 'Real library restored and textures ready' {
        # Preferences persist across runs; always start from the visible-sidebar state.
        $float = (& $exe ui search 'ShowSidebar' --on local -a $AppPid --json | ConvertFrom-Json).matches |
            Where-Object { $_.automationId -eq 'ShowSidebar' -and -not $_.isOffscreen } | Select-Object -First 1
        if ($float) { Ui @('invoke', 'ShowSidebar') | Out-Null; Start-Sleep -Milliseconds 600 }
        Ui @('wait-for', 'LibraryStatus', '--value', $allStatus, '--contains', '-t', '15000')
        Screenshot 'overview'
    }
    $video = Join-Path $Artifacts 'interaction.mp4'
    $recordArgs = @('ui', 'record', '-w', $WindowHandle, '--on', 'local', '--duration-sec', '85', '--fps', '30', '--output', "`"$video`"")
    $movie = Start-Process -FilePath $exe -ArgumentList $recordArgs -PassThru -NoNewWindow
    Start-Sleep -Seconds 2
    Test-Case 'Metadata search gathers the exact matching set' {
        Type-Search $SearchTerm
        # Semantic ranking adds look-alikes; metadata matches are always kept, so the count is a floor.
        Ui @('wait-for', 'LibraryStatus', '--value', "$([char]0x201C)$SearchTerm$([char]0x201D) $([char]0x2014) ", '--contains', '-t', '30000')
        $status = (& $exe ui get-value LibraryStatus --on local -a $AppPid --json | ConvertFrom-Json).text
        if ($status -notmatch '— ([\d,]+) of') { throw "Unexpected search status: $status" }
        $found = [int]($Matches[1] -replace ',', '')
        if ($found -lt $ExpectedMatches) { throw "Search found $found, expected at least $ExpectedMatches ($status)" }
        Start-Sleep -Milliseconds 900
        Screenshot 'search'
    }
    Test-Case 'Photo selection from results and local preview' {
        # Semantic ranking decides the order: select whatever ranks first.
        $top = ((& $exe ui search 'Photo_' --on local -a $AppPid --json --max 5) | ConvertFrom-Json).matches |
            Where-Object { $_.automationId -like 'Photo_*' -and -not $_.isOffscreen } | Select-Object -First 1
        if (-not $top) { throw 'No visible result rows after search.' }
        $id = $top.automationId.Substring(6)
        $photo = $library.Photos | Where-Object Id -eq $id | Select-Object -First 1
        Ui @('invoke', $top.automationId, '--action', 'select')
        Ui @('wait-for', 'PhotoTitle', '--value', $photo.Title, '-t', '5000')
        Start-Sleep -Milliseconds 900
        Screenshot 'selection'
    }
    Test-Case 'Explicit focus and Back' {
        Ui @('invoke', 'FocusPhoto')
        Start-Sleep -Milliseconds 800
        Screenshot 'focused'
        Ui @('invoke', 'BackView')
        Ui @('invoke', 'CloseInspector')
    }
    Test-Case 'Search reversal and clear restore all identities' {
        Type-Search 'zzzz-not-present-in-this-collection'
        Ui @('wait-for', 'LibraryStatus', '--value', '“zzzz-not-present-in-this-collection”', '--contains', '-t', '30000')
        Clear-Search
    }
    Test-Case 'Grid and colour arrangements' {
        Set-Arrangement 'Grid'
        Ui @('wait-for', 'LibraryStatus', '--value', 'Grid', '--contains', '-t', '15000')
        Start-Sleep -Milliseconds 700
        Screenshot 'grid'
        Set-Arrangement 'Colour'
        Ui @('wait-for', 'LibraryStatus', '--value', 'Colour', '--contains', '-t', '15000')
        Start-Sleep -Milliseconds 700
        Screenshot 'colour'
    }
    Test-Case 'Pixel art and photo mode' {
        Ui @('invoke', 'Pixels')
        Ui @('invoke', 'Pixels4')
        Start-Sleep -Milliseconds 400
        Screenshot 'four-pixels'
        Ui @('invoke', 'Pixels')
        Ui @('invoke', 'PixelsPhoto')
    }
    Test-Case 'Synchronized panel collapse and reveal' {
        Ui @('invoke', 'ToggleSidebar')
        Ui @('wait-for', 'ShowSidebar', '-t', '3000')
        Start-Sleep -Milliseconds 500
        Screenshot 'sidebar-collapsed'
        Ui @('invoke', 'ShowSidebar')
        Start-Sleep -Milliseconds 600
        Ui @('wait-for', 'AllPhotos', '-t', '5000')
        Screenshot 'panels-restored'
    }
    Test-Case 'Mouse pan exposes actual canvas content behind glass' {
        $inspection = (Ui @('inspect', '--json')) | ConvertFrom-Json
        $main = $inspection.windows | Where-Object title -eq 'Nebula' | Select-Object -First 1
        $bounds = $main.elements[0]
        $fromX = [int]($bounds.x + $bounds.width * .70)
        $toX = [int]($bounds.x + $bounds.width * .30)
        $dragY = [int]($bounds.y + $bounds.height * .55)
        Ui @('drag', "$fromX,$dragY", "$toX,$dragY", '--hold-ms', '120', '--dwell-ms', '100')
        Start-Sleep -Milliseconds 500
        Screenshot 'glass-with-content'
        Ui @('invoke', 'FitCollection')
    }
    Test-Case 'Similarity arrangement from local image embeddings' {
        Ui @('wait-for', 'ModeSimilarity', '-p', 'IsEnabled', '--value', 'True', '-t', '120000')
        Set-Arrangement 'Similarity'
        Ui @('wait-for', 'LibraryStatus', '--value', 'Similarity', '--contains', '-t', '180000')
        Ui @('invoke', 'FitCollection')
        Start-Sleep -Milliseconds 1200
        Screenshot 'similarity'
    }
    Test-Case 'Visual groups: pin a category, then clear' {
        Ui @('wait-for', 'Category_0', '-t', '120000')
        Ui @('invoke', 'Category_0')
        Ui @('wait-for', 'LibraryStatus', '--value', (' photos ' + [char]0x00B7 + ' Similarity'), '--contains', '-t', '5000')
        Start-Sleep -Milliseconds 1500
        Screenshot 'visual-group-pinned'
        Ui @('invoke', 'GroupClear')
    }
    Test-Case 'Cloud restored' {
        Set-Arrangement 'Cloud'
        Ui @('wait-for', 'LibraryStatus', '--value', 'Cloud', '--contains', '-t', '15000')
        Ui @('invoke', 'FitCollection')
        Start-Sleep -Milliseconds 700
        Screenshot 'final-cloud'
    }
    if ($movie) {
        $movie.WaitForExit()
        if ($movie.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $video)) { throw 'Interaction recording failed.' }
    }
    Test-Case 'Light theme and system-theme restoration' {
        Ui @('invoke', 'MoreOptions')
        Ui @('invoke', 'AppearanceMenu', '--action', 'expand')
        Ui @('invoke', 'AppearanceLight')
        Start-Sleep -Milliseconds 500
        Screenshot 'light'
        Ui @('invoke', 'MoreOptions')
        Ui @('invoke', 'AppearanceMenu', '--action', 'expand')
        Ui @('invoke', 'AppearanceSystem')
    }
} catch {
    $results.Add(@{ name = 'Test setup / recording'; status = 'FAIL'; detail = $_.Exception.Message })
} finally {
    if ($movie -and -not $movie.HasExited) { $movie.WaitForExit() }
    & $exe ui yield --on local | Out-Null
    $env:WINAPP_UI_WORKFLOW_ID = $oldWorkflow
    $report = @{
        app_pid = $AppPid; window = $WindowHandle; library_count = $count
        results = @($results.ToArray()); screenshots = @($screenshots.ToArray())
        video = (Join-Path $Artifacts 'interaction.mp4')
        visual_review = 'required; UIA success alone is not visual acceptance'
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Artifacts 'results.json') -Encoding utf8
}
$failed = @($results | Where-Object status -eq 'FAIL').Count
Write-Host "$($results.Count - $failed) passed, $failed failed. $Artifacts"
if ($failed) { exit 1 }


