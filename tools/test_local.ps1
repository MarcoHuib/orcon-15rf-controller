param(
    [string]$OutputDirectory = '',
    [string]$Compiler = 'g++',
    [ValidateSet('None','Undefined','AddressUndefined','Thread')][string]$Sanitizer = 'None',
    [switch]$Firmware,
    [string]$Platformio = 'pio',
    [int]$TestTimeoutSeconds = 60
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root ('build/local-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Sanitizer) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
if (-not $IsWindows) { [IO.File]::SetUnixFileMode($OutputDirectory, [IO.UnixFileMode]448) }
$rows = [Collections.Generic.List[object]]::new()
$manifest = Join-Path $OutputDirectory 'RESULTS.json'
function Save-Results {
    $data = [ordered]@{ commit = $script:commit; dirty = $script:dirty; sanitizer = $Sanitizer; recorded_utc = [DateTime]::UtcNow.ToString('o'); checks = @($rows.ToArray()) }
    [IO.File]::WriteAllText($manifest, ($data | ConvertTo-Json -Depth 8) + "`n")
}
function Invoke-Check([string]$Name,[string]$Program,[string[]]$Argv,[int]$Timeout=60,[string]$Stdin='') {
    Write-Host "RUN $Name"
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Program; $info.WorkingDirectory = $root
    $info.UseShellExecute = $false; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true; $info.RedirectStandardInput = $true
    foreach ($arg in $Argv) { $info.ArgumentList.Add($arg) }
    $info.Environment['PYTHONDONTWRITEBYTECODE'] = '1'
    if ($Name.StartsWith('firmware-')) { $info.Environment['PLATFORMIO_BUILD_FLAGS'] = '-DFW_VERSION=\"' + $script:commit + '\"' }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    $code = 127; $stdout = ''; $stderr = ''
    try {
        if (-not $process.Start()) { throw "Cannot start $Program" }
        $outTask = $process.StandardOutput.ReadToEndAsync(); $errTask = $process.StandardError.ReadToEndAsync()
        if ($Stdin) { $process.StandardInput.Write($Stdin) }; $process.StandardInput.Close()
        if (-not $process.WaitForExit($Timeout * 1000)) {
            $process.Kill($true); $process.WaitForExit(); $code = 124
        } else { $code = $process.ExitCode }
        $stdout = $outTask.GetAwaiter().GetResult(); $stderr = $errTask.GetAwaiter().GetResult()
    } catch { $stderr += $_.Exception.Message } finally { $process.Dispose() }
    $log = Join-Path $OutputDirectory ($Name + '.log')
    [IO.File]::WriteAllText($log, $stdout + $stderr + "`nexit=$code`n")
    $rows.Add([ordered]@{ name=$Name; exit=$code; result=$(if ($code -eq 0) {'PASS'} else {'FAIL'}); program=$Program; args=$Argv; log=[IO.Path]::GetFileName($log) })
    Save-Results
    if ($code -ne 0) { Write-Host $stderr; Write-Host "FAIL $Name exit=$code" } else { Write-Host "PASS $Name exit=0" }
    return $code
}
# Git reads are checked too. No fetch, Actions, artifact download or hardware tooling.
$commit = & git rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'git rev-parse failed' }
$dirty = ((& git status --porcelain) -join "`n")
if ($LASTEXITCODE -ne 0) { throw 'git status failed' }
$files = @(& git ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed' }
$hashes = foreach ($file in $files) { if (Test-Path -LiteralPath $file -PathType Leaf) { [ordered]@{path=$file; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()} } }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'SOURCE_HASHES.json'), ($hashes | ConvertTo-Json -Depth 3))
Save-Results
$null = Invoke-Check 'compiler-version' $Compiler @('--version')
$flags = @('-std=c++17','-O1','-g')
switch ($Sanitizer) {
    Undefined { $flags += @('-fsanitize=undefined','-fno-sanitize-recover=all') }
    AddressUndefined { $flags += @('-fsanitize=address,undefined','-fno-sanitize-recover=all') }
    Thread { $flags += '-fsanitize=thread' }
}
$orcon = @('src/orcon.cpp','src/orcon_frames.cpp','src/orcon_log.cpp','src/ramses_codec.cpp')
$suites = [ordered]@{
    codec=@('test/test_codec.cpp','src/ramses_codec.cpp')
    frames=@('test/test_frames.cpp','src/ramses_codec.cpp','src/orcon_frames.cpp')
    rx=@('test/test_rx.cpp','src/rx_stream.cpp','src/ramses_codec.cpp')
    log=@('test/test_log.cpp','src/ramses_codec.cpp','src/orcon_frames.cpp','src/orcon_log.cpp')
    orcon_rx=(@('-DRX_ONLY=1','test/test_orcon.cpp') + $orcon)
    orcon_v3=(@('-DRX_ONLY=0','test/test_orcon.cpp') + $orcon)
    rxpipe=@('-pthread','test/test_rxpipe.cpp','src/rx_stream.cpp','src/ramses_codec.cpp')
    netcfg=@('test/test_netcfg.cpp')
    oled=@('test/test_oled_power.cpp')
    tx_poc=@('-pthread','-Wall','-Wextra','-Werror','test/test_tx_poc.cpp','src/tx_poc.cpp','src/ramses_codec.cpp','src/orcon_frames.cpp')
}
$sanitizerUnavailable = $false
if ($Sanitizer -ne 'None') {
    $probe = Join-Path $OutputDirectory 'sanitizer-probe'
    $probeCompile = Invoke-Check 'compile-sanitizer-probe' $Compiler ($flags + @('-x','c++','-o',$probe,'-')) 60 "int main() { return 0; }`n"
    if ($probeCompile -ne 0) { $sanitizerUnavailable = $true }
    else { $sanitizerUnavailable = (Invoke-Check 'sanitizer-probe' $probe @() $TestTimeoutSeconds) -ne 0 }
}
foreach ($suite in $suites.GetEnumerator()) {
    if ($sanitizerUnavailable) { $rows.Add([ordered]@{name=$suite.Key; result='NOT TESTED'; exit=$null; reason='prior sanitizer process failed; inspect log'}); continue }
    $binary = Join-Path $OutputDirectory ($suite.Key + $(if ($IsWindows) {'.exe'} else {''}))
    $compile = Invoke-Check ('compile-' + $suite.Key) $Compiler ($flags + @('-I','src','-I','test/shim') + $suite.Value + @('-o',$binary))
    if ($compile -eq 0) {
        $run = Invoke-Check $suite.Key $binary @() $TestTimeoutSeconds
        if ($Sanitizer -ne 'None' -and $run -ne 0) { $sanitizerUnavailable = $true }
    }
}
if ($Sanitizer -eq 'None') {
    $null = Invoke-Check 'safety-and-negative-compiles' 'python3' @('-B','tools/test_tx_poc_safety.py','--compiler',$Compiler)
    $null = Invoke-Check 'privacy' 'python3' (@('-B','tools/check_privacy.py') + $files)
    $null = Invoke-Check 'node-version' 'node' @('--version')
    foreach ($page in @('legacy','poc')) {
        $binary = Join-Path $OutputDirectory ('dump-' + $page)
        $source = if ($page -eq 'legacy') {'test/dump_pages.cpp'} else {'test/dump_tx_poc.cpp'}
        $code = Invoke-Check ('compile-pages-' + $page) $Compiler @('-std=c++17','-DPROGMEM=','-I','src','-I','test/shim',$source,'-o',$binary)
        if ($code -ne 0) { continue }
        $indices = if ($page -eq 'legacy') {0..6} else {0}
        foreach ($i in $indices) {
            $name = 'page-' + $page + '-' + $i
            $code = Invoke-Check $name $binary @([string]$i)
            if ($code -ne 0) { continue }
            $html = [IO.File]::ReadAllText((Join-Path $OutputDirectory ($name + '.log')))
            $scripts = [regex]::Matches($html,'(?s)<script>(.*?)</script>') | ForEach-Object {$_.Groups[1].Value}
            $js = Join-Path $OutputDirectory ($name + '.js')
            [IO.File]::WriteAllText($js,($scripts -join "`n;`n"))
            $null = Invoke-Check ('syntax-' + $name) 'node' @('--check',$js)
            if ($page -eq 'poc') { $null = Invoke-Check 'poc-web' 'node' @('test/test_tx_poc_web.js') 30 $html }
            if ($page -eq 'legacy' -and $i -eq 0) { $null = Invoke-Check 'oled-main-web' 'node' @('test/test_oled_main.js') 30 $html }
            if ($page -eq 'legacy' -and $i -eq 5) { $null = Invoke-Check 'oled-setup-web' 'node' @('test/test_oled_setup.js') 30 $html }
        }
    }
}
if ($Firmware) {
    $null = Invoke-Check 'platformio-version' $Platformio @('--version')
    foreach ($envName in @('heltec_v2_sx1276','heltec_v2_sx1276_tx_poc_dryrun','heltec_v3_cc1101_ota','heltec_v3_1262_ota','esp32-480480s040')) {
        $code = Invoke-Check ('firmware-' + $envName) $Platformio @('run','-e',$envName) 300
        if ($code -ne 0) { continue }
        $dest = Join-Path $OutputDirectory ('artifacts/' + $envName)
        [IO.Directory]::CreateDirectory($dest) | Out-Null
        foreach ($image in @('firmware.bin','partitions.bin','bootloader.bin')) {
            Copy-Item -LiteralPath (Join-Path $root ('.pio/build/' + $envName + '/' + $image)) -Destination $dest
        }
        $firmwareText = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes((Join-Path $dest 'firmware.bin')))
        $imageOk = $firmwareText.Contains($commit)
        if ($envName -eq 'heltec_v2_sx1276' -or $envName -eq 'heltec_v2_sx1276_tx_poc_dryrun') {
            foreach ($forbidden in @('ArduinoOTA','auth_upload','Updater.cpp')) { if ($firmwareText.Contains($forbidden)) { $imageOk = $false } }
            $imageOk = $imageOk -and $firmwareText.Contains('SX1276 direct RX (TX disabled)')
            if ($envName -eq 'heltec_v2_sx1276') { $imageOk = $imageOk -and $firmwareText.Contains('RX-ONLY TEST FIRMWARE') -and -not $firmwareText.Contains('tx-poc-dryrun') }
            else { $imageOk = $imageOk -and $firmwareText.Contains('tx-poc-dryrun') }
            $null = Invoke-Check ('partitions-' + $envName) 'python3' @('-B','tools/check_parts.py',(Join-Path $dest 'partitions.bin'))
        }
        $rows.Add([ordered]@{name=('image-' + $envName); result=$(if ($imageOk) {'PASS'} else {'FAIL'}); exit=$(if ($imageOk) {0} else {1})})
        Save-Results
        $null = Invoke-Check ('packages-' + $envName) $Platformio @('pkg','list','-e',$envName)
        $info = [ordered]@{commit=$commit; dirty=$dirty; environment=$envName; rf_release=$false; files=@(Get-ChildItem $dest -File | ForEach-Object {[ordered]@{name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}})}
        [IO.File]::WriteAllText((Join-Path $dest 'BUILD_INFO.json'),($info | ConvertTo-Json -Depth 5))
    }
    # Flash tooling (precheck, local_ok, markers) on simulated boards; reference = the local V2 table.
    $parts = Join-Path $OutputDirectory 'artifacts/heltec_v2_sx1276/partitions.bin'
    if (Test-Path -LiteralPath $parts) { $null = Invoke-Check 'flash-tooling' 'bash' @('tools/test_precheck.sh',$parts) 120 }
    else { $rows.Add([ordered]@{name='flash-tooling'; result='FAIL'; exit=1; reason='no local V2 partitions.bin'}); Save-Results }
}
Save-Results
Write-Host "Local evidence: $manifest"
if (@($rows | Where-Object {$_.result -eq 'FAIL'}).Count) { exit 1 }
exit 0
