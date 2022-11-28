SET SZ="C:\Program Files\7-Zip\7z.exe"
SET V=0.4

pushd Win32\ReleaseStatic && (%SZ% a ..\..\sapicli-%V%-x86-static.zip sapicli.exe & popd)
pushd Win32\ReleaseDynamic && (%SZ% a ..\..\sapicli-%V%-x86.zip sapicli.exe ogg.dll libvorbis.dll opus.dll & popd)
pushd x64\ReleaseStatic && (%SZ% a ..\..\sapicli-%V%-x64-static.zip sapicli.exe & popd)
pushd x64\ReleaseDynamic && (%SZ% a ..\..\sapicli-%V%-x64.zip sapicli.exe libvorbis.dll opus.dll & popd)
