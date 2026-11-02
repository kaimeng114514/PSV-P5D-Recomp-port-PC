# 一键跑四条回归线（互不干扰）：
#   ① 探针/RE 工具红线（模拟器侧基线，保持不变）
#   ② PC 原生渲染回归（游戏原生 960×544，含图像级判据）
#   ③ ver3 GAP 块图遍历（1039/1039 基线）
#   ④ 原生对局功能线（真实谱面 mns108_3 -> DANCE_LOOP_SUMMARY 精确常数，18.3358）
#   ⑤ 原生判定链 oracle（mns218_2，20 断言，18.3363）
#   ⑥ 原生 rank 计算线（mns218_2，RANK_SUMMARY 常数，18.3365）
# 用法: powershell -ExecutionPolicy Bypass -File run_all_regressions.ps1
$ErrorActionPreference = 'Continue'
$fail = 0

Write-Output '===== ① 探针/RE 工具红线（模拟器侧）====='
Push-Location 'C:\dev\p5d_build\_android'
$out1 = cmd /c "powershell -ExecutionPolicy Bypass -File offline_regression.ps1" 2>&1
Pop-Location
$line1 = ($out1 | Select-String -Pattern 'OFFLINE_REGRESSION_ALL_PASS|OFFLINE_REGRESSION' | Select-Object -Last 1)
$out1 | Select-String -Pattern 'APK_BUILD_OK|PASS=|FAIL' | ForEach-Object { Write-Output ('  ' + $_.Line.Trim()) }
if ($line1 -and $line1.Line -match 'ALL_PASS') { Write-Output '  => 探针线 PASS' } else { Write-Output '  => 探针线 FAIL'; $fail = 1 }

Write-Output ''
Write-Output '===== ② PC 原生渲染回归（960x544 游戏原生）====='
Push-Location 'C:\dev\p5dsrc\tools'
$out2 = py -3 render_regression.py 2>&1
Pop-Location
$out2 | Where-Object { $_ -match '960x544|RENDER_REGRESSION' } | ForEach-Object { Write-Output ('  ' + $_.ToString().Trim()) }
if (($out2 | Select-String -Pattern 'RENDER_REGRESSION_PASS')) { Write-Output '  => 渲染线 PASS' } elseif (($out2 | Select-String -Pattern 'RENDER_REGRESSION_SKIP')) { Write-Output '  => 渲染线 SKIP (captured inputs missing, regenerable)' } else { Write-Output '  => 渲染线 FAIL'; $fail = 1 }

Write-Output ''
Write-Output '===== ③ ver3 GAP 块图遍历（1000/1039 基线，见 18.3224/18.3227）====='
Push-Location 'C:\dev\p5dsrc\tools'
$out3 = py -3 gap_walk_ver3.py 2>&1
Pop-Location
$out3 | ForEach-Object { Write-Output ('  ' + $_.ToString().Trim()) }
if ($out3 -match 'GAP_WALK_VER3 1039/1039') { Write-Output '  => 块图线 PASS' } else { Write-Output '  => 块图线 FAIL'; $fail = 1 }

Write-Output ''
Write-Output '===== ④ 原生对局功能线（真实谱面 mns108_3，18.3358）====='
$chart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns108_3.bin'
$exe = 'C:\dev\p5dsrc\bin\p5d_dance_loop.exe'
if ((Test-Path $chart) -and (Test-Path $exe)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = '"' + $chart + '" 3'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out4 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line4 = ($out4 -split "`n" | Where-Object { $_ -match 'DANCE_LOOP_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line4).Trim())
    if ($line4 -match 'DANCE_LOOP_SUMMARY frames=1198 hits=609 misses=0 gauge=1\.0000 score=30450000') {
        Write-Output '  => 对局功能线 PASS'
    } else { Write-Output '  => 对局功能线 FAIL'; $fail = 1 }
} else { Write-Output '  => 对局功能线 SKIP (chart/exe missing)' }

Write-Output ''
Write-Output '===== ⑤ 原生判定链 oracle（mns218_2，20 断言，18.3363）====='
$jchart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns218_2.bin'
$jexe = 'C:\dev\p5d_build\_mns\mns_judge_test.exe'
if ((Test-Path $jchart) -and (Test-Path $jexe)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $jexe
    $psi.Arguments = '"' + $jchart + '"'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out5 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line5 = ($out5 -split "`n" | Where-Object { $_ -match 'JUDGE_TEST_ALL_PASS|PASS=\d+ FAIL=\d+' } | Select-Object -Last 2)
    $line5 | ForEach-Object { Write-Output ('  ' + ([string]$_).Trim()) }
    if ($out5 -match 'JUDGE_TEST_ALL_PASS') { Write-Output '  => 判定链 oracle PASS' } else { Write-Output '  => 判定链 oracle FAIL'; $fail = 1 }
} else { Write-Output '  => 判定链 oracle SKIP (chart/exe missing)' }

Write-Output ''
Write-Output '===== ⑥ 原生 rank 计算线（mns218_2，18.3365）====='
$kchart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns218_2.bin'
$kexe = 'C:\dev\p5d_build\p5d_rank_test.exe'
if ((Test-Path $kchart) -and (Test-Path $kexe)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $kexe
    $psi.Arguments = '"' + $kchart + '" 218'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out6 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line6 = ($out6 -split "`n" | Where-Object { $_ -match 'RANK_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line6).Trim())
    if ($line6 -match 'RANK_SUMMARY bpm=190\.0000 dur=78\.7026 x=62 rank=21') {
        Write-Output '  => rank 线 PASS'
    } else { Write-Output '  => rank 线 FAIL'; $fail = 1 }
} else { Write-Output '  => rank 线 SKIP (chart/exe missing)' }

Write-Output ''
Write-Output '===== ⑦ 进度/gauge/section/baseScore 单元线（DANCE_PROGRESS_SUMMARY fails=0，18.3378）====='
$pexg = 'C:\dev\p5d_build\p5d_dance_progress_test.exe'
if (Test-Path $pexg) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pexg
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out7 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line7 = ($out7 -split "`n" | Where-Object { $_ -match 'DANCE_PROGRESS_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line7).Trim())
    if ($line7 -match 'DANCE_PROGRESS_SUMMARY fails=0') { Write-Output '  => 进度单元线 PASS' } else { Write-Output '  => 进度单元线 FAIL'; $fail = 1 }
} else { Write-Output '  => 进度单元线 SKIP (exe missing)' }

Write-Output ''
Write-Output '===== ⑧ 结算画面端到端线（RESULT_DEMO_SUMMARY，18.3380）====='
$rchart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns218_2.bin'
$rex = 'C:\dev\p5d_build\p5d_result_demo.exe'
if ((Test-Path $rchart) -and (Test-Path $rex)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rex
    $psi.Arguments = '"' + $rchart + '" 2 0'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out8 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line8 = ($out8 -split "`n" | Where-Object { $_ -match 'RESULT_DEMO_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line8).Trim())
    if ($line8 -match 'RESULT_DEMO_SUMMARY frames=2361 hits=802 miss=16 gauge=0\.8000 bonus=30000 final=30000 fails=0') {
        Write-Output '  => 结算画面线 PASS'
    } else { Write-Output '  => 结算画面线 FAIL'; $fail = 1 }
} else { Write-Output '  => 结算画面线 SKIP (chart/exe missing)' }

Write-Output ''
Write-Output '===== ⑨ 结算画面渲染线（RESULT_RENDER_SUMMARY + BMP，18.3383）====='
$rrchart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns218_2.bin'
$rrbmp = 'C:\dev\tmp\_reg_result_render.bmp'
$rrex = 'C:\dev\p5d_build\p5d_result_render.exe'
if ((Test-Path $rrchart) -and (Test-Path $rrex)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rrex
    $psi.Arguments = '"' + $rrchart + '" "' + $rrbmp + '" 2 0'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out9 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line9 = ($out9 -split "`n" | Where-Object { $_ -match 'RESULT_RENDER_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line9).Trim())
    if (($line9 -match 'RESULT_RENDER_SUMMARY final=30000 stars=5 gauge=0\.8000') -and (Test-Path $rrbmp)) {
        Write-Output '  => 结算渲染线 PASS'
    } else { Write-Output '  => 结算渲染线 FAIL'; $fail = 1 }
} else { Write-Output '  => 结算渲染线 SKIP (chart/exe missing)' }

Write-Output ''
Write-Output '===== ⑩ 时钟 apply 单元线（DANCE_CLOCK_SUMMARY fails=0，18.3399）====='
$cexe = 'C:\dev\p5d_build\p5d_dance_clock_test.exe'
if (Test-Path $cexe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $cexe
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out10 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line10 = ($out10 -split "`n" | Where-Object { $_ -match 'DANCE_CLOCK_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line10).Trim())
    if ($line10 -match 'DANCE_CLOCK_SUMMARY fails=0') { Write-Output '  => 时钟单元线 PASS' } else { Write-Output '  => 时钟单元线 FAIL'; $fail = 1 }
} else { Write-Output '  => 时钟单元线 SKIP (exe missing)' }

Write-Output ''
Write-Output '===== ⑪ eboot 表单元线（EBOOT_TABLES_SUMMARY checks=5/5，18.3399）====='
$bexe = 'C:\dev\p5d_build\p5d_eboot_tables_test.exe'
if (Test-Path $bexe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $bexe
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out11 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line11 = ($out11 -split "`n" | Where-Object { $_ -match 'EBOOT_TABLES_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line11).Trim())
    if ($line11 -match 'EBOOT_TABLES_SUMMARY checks=5/5') { Write-Output '  => eboot 表单元线 PASS' } else { Write-Output '  => eboot 表单元线 FAIL'; $fail = 1 }
} else { Write-Output '  => eboot 表单元线 SKIP (exe missing)' }

Write-Output ''
Write-Output '===== ⑫ ARC 格式全量线（ARC_ALL_PASS 135/135，18.3400）====='
$aexe = 'C:\dev\p5d_build\p5d_arc_test.exe'
$aroot = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted'
if ((Test-Path $aexe) -and (Test-Path $aroot)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $aexe
    $psi.Arguments = '"' + $aroot + '"'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out12 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(180000) | Out-Null
    $line12 = ($out12 -split "`n" | Where-Object { $_ -match 'ARC_ALL_PASS' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line12).Trim())
    if ($line12 -match 'ARC_ALL_PASS 135/135') { Write-Output '  => ARC 全量线 PASS' } else { Write-Output '  => ARC 全量线 FAIL'; $fail = 1 }
} else { Write-Output '  => ARC 全量线 SKIP (exe/root missing)' }

Write-Output ''
Write-Output '===== ⑬ GMD 全库自洽线（571 文件 / 顶点 3568310 / 三角 5244958，18.3401）====='
$gexe = 'C:\dev\p5d_build\p5d_gmd_test.exe'
if (Test-Path $gexe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $gexe
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out13 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(300000) | Out-Null
    if (($out13 -match '571') -and ($out13 -match '3568310') -and ($out13 -match '5244958')) {
        Write-Output '  => GMD 全库线 PASS (571 files, verts 3568310, tris 5244958)'
    } else { Write-Output '  => GMD 全库线 FAIL'; $fail = 1 }
} else { Write-Output '  => GMD 全库线 SKIP (exe missing)' }

Write-Output ''
Write-Output '===== ⑭ 分数动画单元线（SCORE_ANIM_SUMMARY fails=0，18.3402）====='
$saexe = 'C:\dev\p5d_build\p5d_score_anim_test.exe'
if (Test-Path $saexe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $saexe
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out14 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line14 = ($out14 -split "`n" | Where-Object { $_ -match 'SCORE_ANIM_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line14).Trim())
    if ($line14 -match 'SCORE_ANIM_SUMMARY fails=0') { Write-Output '  => 分数动画线 PASS' } else { Write-Output '  => 分数动画线 FAIL'; $fail = 1 }
} else { Write-Output '  => 分数动画线 SKIP (exe missing)' }

Write-Output ''
Write-Output '===== ⑮ 伞头链单元线（UMBRELLA_SUMMARY ok=7，18.3402）====='
$uhexe = 'C:\dev\p5d_build\p5d_umbrella_test.exe'
if (Test-Path $uhexe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $uhexe
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out15 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line15 = ($out15 -split "`n" | Where-Object { $_ -match 'UMBRELLA_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line15).Trim())
    if ($line15 -match 'UMBRELLA_SUMMARY ok=7') { Write-Output '  => 伞头链线 PASS' } else { Write-Output '  => 伞头链线 FAIL'; $fail = 1 }
} else { Write-Output '  => 伞头链线 SKIP (exe missing)' }

Write-Output ''
Write-Output '===== ⑯ 可判定音符计数线（PRED_COUNT_SUMMARY A=819 B=802 diff=17，18.3403）====='
$pcexe = 'C:\dev\p5d_build\p5d_pred_count.exe'
$pcchart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns218_2.bin'
if ((Test-Path $pcexe) -and (Test-Path $pcchart)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pcexe
    $psi.Arguments = '"' + $pcchart + '"'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out16 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line16 = ($out16 -split "`n" | Where-Object { $_ -match 'PRED_COUNT_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line16).Trim())
    if ($line16 -match 'PRED_COUNT_SUMMARY A=819 B=802 diff=17') { Write-Output '  => 可判定计数线 PASS' } else { Write-Output '  => 可判定计数线 FAIL'; $fail = 1 }
} else { Write-Output '  => 可判定计数线 SKIP (exe/chart missing)' }

Write-Output ''
Write-Output '===== ⑰ 第二谱面对局功能线（mns201_3，DANCE_LOOP_SUMMARY，18.3404）====='
$c2 = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns201_3.bin'
$dl = 'C:\dev\p5dsrc\bin\p5d_dance_loop.exe'
if ((Test-Path $c2) -and (Test-Path $dl)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $dl
    $psi.Arguments = '"' + $c2 + '" 3'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out17 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(180000) | Out-Null
    $line17 = ($out17 -split "`n" | Where-Object { $_ -match 'DANCE_LOOP_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line17).Trim())
    if ($line17 -match 'DANCE_LOOP_SUMMARY frames=1552 hits=566 misses=0 gauge=1\.0000 score=28300000') {
        Write-Output '  => 第二谱面线 PASS'
    } else { Write-Output '  => 第二谱面线 FAIL'; $fail = 1 }
} else { Write-Output '  => 第二谱面线 SKIP (chart/exe missing)' }

Write-Output ''
Write-Output '===== ⑱ 第二谱面可判定计数线（mns108_3，PRED_COUNT_SUMMARY A=611 B=609 diff=2，18.3405）====='
$pcexe2 = 'C:\dev\p5d_build\p5d_pred_count.exe'
$pcchart2 = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns108_3.bin'
if ((Test-Path $pcexe2) -and (Test-Path $pcchart2)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pcexe2
    $psi.Arguments = '"' + $pcchart2 + '"'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $out18 = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(120000) | Out-Null
    $line18 = ($out18 -split "`n" | Where-Object { $_ -match 'PRED_COUNT_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line18).Trim())
    if ($line18 -match 'PRED_COUNT_SUMMARY A=611 B=609 diff=2') { Write-Output '  => 第二谱面计数线 PASS' } else { Write-Output '  => 第二谱面计数线 FAIL'; $fail = 1 }
} else { Write-Output '  => 第二谱面计数线 SKIP (exe/chart missing)' }

Write-Output ''
Write-Output '===== ⑲ GAP 骨骼计数一致性线（GAP_BONE_COUNT_PASS 84/84，18.3418）====='
$bctool = 'C:\dev\p5dsrc\tools\gap_bone_count.py'
if (Test-Path $bctool) {
    $out19 = py -3 $bctool 2>&1
    $line19 = ($out19 | Select-String -Pattern 'GAP_BONE_COUNT_PASS|GAP_BONE_COUNT_FAIL' | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$line19).Trim())
    if ($line19 -match 'GAP_BONE_COUNT_PASS 84/84') { Write-Output '  => 骨骼计数线 PASS' } else { Write-Output '  => 骨骼计数线 FAIL'; $fail = 1 }
} else { Write-Output '  => 骨骼计数线 SKIP (tool missing)' }

Write-Output ''
Write-Output '===== ⑦ 拍速/时间基线（measure x 240/BPM，证据 18.3546）====='
$mexe = 'C:\dev\p5dsrc\bin\p5d_dance_loop_measure.exe'
$mchart = 'C:\dev\p5dsrc\game\PCSH10109\data_extracted\dance\mns\mns218_2.bin'
if ((Test-Path $mexe) -and (Test-Path $mchart)) {
    $env:P5D_MEASURE_TIMING = '1'
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $mexe
    $psi.Arguments = '"' + $mchart + '" 3'
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outM = $proc.StandardOutput.ReadToEnd()
    $proc.WaitForExit(180000) | Out-Null
    Remove-Item Env:P5D_MEASURE_TIMING -ErrorAction SilentlyContinue
    $durM = ($outM -split "`n" | Where-Object { $_ -match 'judgeable duration=' } | Select-Object -Last 1)
    $lineM = ($outM -split "`n" | Where-Object { $_ -match 'DANCE_LOOP_SUMMARY' } | Select-Object -Last 1)
    Write-Output ('  ' + ([string]$durM).Trim())
    Write-Output ('  ' + ([string]$lineM).Trim())
    if (($durM -match 'judgeable duration=314\.810') -and ($lineM -match 'DANCE_LOOP_SUMMARY frames=9437 hits=802 misses=0 gauge=1\.0000 score=40100000')) {
        Write-Output '  => 拍速/时间基线 PASS'
    } else { Write-Output '  => 拍速/时间基线 FAIL'; $fail = 1 }
} else { Write-Output '  => 拍速/时间基线 SKIP (exe/chart missing)' }

Write-Output ''
Write-Output '===== (8) AOT Thumb->x86-64 translator self-test (18.3560) ====='
$aotscript = 'C:\dev\p5dsrc\tools\run_aot_selftest.ps1'
if (Test-Path $aotscript) {
    $outA = & powershell -ExecutionPolicy Bypass -File $aotscript 2>&1
    $lineA = ($outA | Select-String -Pattern 'AOT_TRANSLATOR_SELFTEST_PASS|AOT_EXECUTOR_SELFTEST_PASS' | Select-Object -Last 1)
    Write-Output ('  ' + (([string]$lineA) -replace '\s+$',''))
    if ($lineA -match 'AOT_TRANSLATOR_SELFTEST_PASS') { Write-Output '  => AOT translator PASS' } else { Write-Output '  => AOT translator FAIL'; $fail = 1 }
} else { Write-Output '  => AOT translator SKIP (script missing)' }

Write-Output ''
if ($fail -eq 0) { Write-Output 'ALL_REGRESSIONS_PASS' } else { Write-Output 'ALL_REGRESSIONS_FAIL' }
exit $fail
