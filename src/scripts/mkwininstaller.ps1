param (
    [Parameter(Mandatory=$true)][string]$version
)

$target="mrc-qt-wallet-v$version"

Remove-Item -Path release/wininstaller -Recurse | Out-Null
New-Item release/wininstaller -itemtype directory | Out-Null

Copy-Item release/$target/mrc-qt-wallet.exe release/wininstaller/
Copy-Item release/$target/LICENSE release/wininstaller/
Copy-Item release/$target/README.md release/wininstaller/
Copy-Item release/$target/moonroomcashd.exe release/wininstaller/
Copy-Item release/$target/moonroomcash-cli.exe release/wininstaller/

Get-Content src/scripts/mrc-qt-wallet.wxs | ForEach-Object { $_ -replace "RELEASE_VERSION", "$version" } | Out-File -Encoding utf8 release/wininstaller/mrc-qt-wallet.wxs

candle.exe release/wininstaller/mrc-qt-wallet.wxs -o release/wininstaller/mrc-qt-wallet.wixobj 
if (!$?) {
    exit 1;
}

light.exe -ext WixUIExtension -cultures:en-us release/wininstaller/mrc-qt-wallet.wixobj -out release/wininstaller/mrc-qt-wallet.msi 
if (!$?) {
    exit 1;
}

New-Item artifacts -itemtype directory -Force | Out-Null
Copy-Item release/wininstaller/mrc-qt-wallet.msi ./artifacts/$target.msi