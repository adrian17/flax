@echo off
msbuild /nologo /verbosity:m flax.vcxproj && build\sysroot\windows\Debug\flax.exe -sysroot build\sysroot -run build\%1.flx