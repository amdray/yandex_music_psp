# Собирает publish-папку для совместной работы и GitHub-релизов:
#   ..\YMPSP-publish\<ver>\  <- свежий исходник (zip) + релиз (zip) + описание.
# Ничего лишнего (токены, логи, объектные файлы) туда не попадает:
# исходник берётся через git archive (только отслеживаемые файлы),
# релиз — из release/ (там шаблон без токена, проверяется).
param(
    [string]$Version = "v0.2"
)

$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $MyInvocation.MyCommand.Path
$Out = Join-Path (Split-Path -Parent $Repo) "YMPSP-publish\$Version"

Write-Host "== build =="
& C:\tools\msys64\usr\bin\bash.exe -lc 'export PSPDEV=/c/pspdev; export PATH=$PSPDEV/bin:/usr/local/bin:/usr/bin:/bin; cd /c/Users/Tsifrokayf/Desktop/psp/amdray/yandex_music_psp; make'
if ($LASTEXITCODE -ne 0) { throw "make failed" }

Write-Host "== checks =="
$tok = Join-Path $Repo "release\config\token.txt"
$tokText = (Get-Content $tok -Raw).Trim()
if ($tokText -ne 'YANDEX_TOKEN = ""') { throw "token.txt is NOT blank, abort: $tokText" }
$eboot = Join-Path $Repo "release\EBOOT.PBP"
if (-not (Test-Path $eboot)) { throw "no EBOOT.PBP" }
$dirty = C:\tools\msys64\usr\bin\bash.exe -lc 'cd /c/Users/Tsifrokayf/Desktop/psp/amdray/yandex_music_psp && git status --porcelain | head -n 5'
if ($dirty) { Write-Host "WARNING: uncommitted changes:"; Write-Host $dirty }

New-Item -ItemType Directory -Force -Path $Out | Out-Null

Write-Host "== source zip (git archive) =="
$srcZip = Join-Path $Out "YMPSP-src-$Version.zip"
& C:\tools\msys64\usr\bin\bash.exe -lc "cd /c/Users/Tsifrokayf/Desktop/psp/amdray/yandex_music_psp && git archive --format=zip --output=`"$srcZip`" HEAD"
if ($LASTEXITCODE -ne 0) { throw "git archive failed (uncommitted files?)" }

Write-Host "== release zip =="
$relZip = Join-Path $Out "YMPSP-$Version.zip"
$stage = Join-Path $env:TEMP "ympsp-rel-stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path (Join-Path $stage "YMPSP") | Out-Null
Copy-Item (Join-Path $Repo "release\EBOOT.PBP") (Join-Path $stage "YMPSP\") -Force
Copy-Item (Join-Path $Repo "release\fonts") (Join-Path $stage "YMPSP\") -Recurse -Force
Copy-Item (Join-Path $Repo "release\assets") (Join-Path $stage "YMPSP\") -Recurse -Force
Copy-Item (Join-Path $Repo "release\config") (Join-Path $stage "YMPSP\") -Recurse -Force
Copy-Item (Join-Path $Repo "RELEASE_NOTES.md") (Join-Path $stage "YMPSP\CHANGES-$Version.txt") -Force
Compress-Archive -Path (Join-Path $stage "YMPSP") -DestinationPath $relZip -Force
Remove-Item -Recurse -Force $stage

Write-Host "== notes =="
Copy-Item (Join-Path $Repo "RELEASE_NOTES.md") (Join-Path $Out "CHANGES-$Version.txt") -Force

Write-Host ""
Write-Host "OK: $Out"
Get-ChildItem $Out | Select-Object Name, Length
