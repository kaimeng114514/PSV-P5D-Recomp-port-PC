# Vulkan backend object rebuild (M1319, verified)
Set-Location C:\dev\p5dsrc
C:\dev\mingw64\bin\g++.exe -std=c++17 -O2 -g -DP5D_HAS_VULKAN -include cmath -include cstdlib -include cstring `
  -I C:\dev\p5dsrc\src -I C:\dev\p5dsrc\src\gxm -I C:\dev\p5dsrc\third_party\vulkan\include `
  -c C:\dev\p5dsrc\src\gxm\vulkan_backend.cpp -o C:\dev\tmp\recomp_split\gxm_vk.o
Write-Host ("backend compile exit=" + $LASTEXITCODE)
cmd /c "py -3 C:\dev\tmp\_p_m1304_link.py"
