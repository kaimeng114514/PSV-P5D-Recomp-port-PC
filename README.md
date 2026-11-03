# P5D-PC 鈥?P5D锛圥CSH10109锛塒C 鍘熺敓閲嶇紪璇戠Щ妞?
鎶?PS Vita 鐗堛€奝ersona 5: Dancing in Starlight銆嬶紙P5D锛孭CSH10109锛夌殑**鍘熺増娓告垙浜岃繘鍒跺師鏍峰姞杞?*锛?鍦?PC锛堝悗缁彲鎵╁睍鍒版墜鏈猴級涓婄敱鎴戜滑瀹炵幇鐨勬湰鍦板吋瀹瑰眰锛圚LE锛夐┍鍔ㄨ繍琛屻€?CPU 閲囩敤 **AOT 闈欐€侀噸缂栬瘧**锛堝師鐗?ARM/Thumb 浠ｇ爜涓€娆℃€х炕璇戜负 x86-64 鍘熺敓鎵ц锛? 瑙ｉ噴鍣?fallback锛?绯荤粺 API锛堟枃浠?瀛樻。銆佸唴瀛樸€佺嚎绋嬨€丟XM鈫扸ulkan銆侀煶棰戙€佽緭鍏ャ€佽棰戣В鐮侊級鍏ㄩ儴鐢?HLE 鎺ョ锛?鏈€缁堢敱**鍘熺増娓告垙浠ｇ爜鑷繁缁樺埗**鐢婚潰锛岀洰鏍囦覆鑱?`S0_LOGO 鈫?S1_TITLE 鈫?S2_MENU 鈫?S3_SELECT 鈫?S4_GAME 鈫?RESULT` 鍏ㄦ祦绋嬨€?
> 鏈粨搴?*涓嶅寘鍚换浣曟父鎴忔枃浠躲€佹父鎴忓瘑閽ャ€佸浐浠舵垨鐢辨父鎴忔暟鎹炕璇戝嚭鐨勪簩杩涘埗浜х墿**銆?> 瑕佽繍琛屾湰椤圭洰锛岀帺瀹跺繀椤讳娇鐢?*鑷繁鍚堟硶鎸佹湁鐨勬鐗堟父鎴?*浠庤嚜宸辩殑娓告垙鏈烘彁鍙栨暟鎹紙璇﹁ [docs/RELEASE_COMPLIANCE.md](docs/RELEASE_COMPLIANCE.md)锛夈€?
---

## 褰撳墠鐘舵€侊紙2026-11-02锛岄噷绋嬬 **M1123**锛涚郴缁熷眰绾?89%锛岀鍒扮鐩爣绾?60鈥?0%锛?
**涓€鍙ヨ瘽**锛氬師鐗?eboot 宸插湪 PC 涓婂師鐢燂紙AOT锛夎窇璧锋潵锛孲0_LOGO 瀹屾暣閫氳繃锛?**CRI FS 鐨?CPK 鎸傝浇/鏂囦欢绱㈠紩宸插畬鍏ㄦ墦閫?*鈥斺€擿data.cpk`锛?0,555 鏂囦欢锛? `vita.cpk`锛?31 鏂囦欢锛?鍙屽弻鎸傝浇鎴愬姛锛屾爣棰樼礌鏉愬悕鍏ㄩ儴鏋氫妇骞堕€変腑 `title/title_p5.bin`锛涘綋鍓嶅崱鐐规帹杩涘埌**鏍囬鏂囦欢寮傛鍔犺浇鐨勮疆璇㈢瓑寰?*銆?
| 妯″潡 | 瀹屾垚搴?| 褰撳墠鐘舵€侊紙M1115 瀹炴祴锛?|
|---|---|---|
| ARM/Thumb 鈫?x86-64 **AOT 闈欐€侀噸缂栬瘧** | ~95% | **230,252 涓熀鏈潡 / 12,921 涓嚱鏁?/ 36 涓涓绘壒娆?*锛沗[NOFN]=0`銆乣[INDIRECT-MISS]=0`銆乣[BADMEM]=0`锛涙湰闃舵淇涓や釜**閫氱敤缈昏瘧鍣?bug**锛氣憼 鍏ㄥ眬 `LDRD rX,rY,[rX,#imm]`锛?4 澶勶級鈶?姝ゅ墠鐨?`[pc,#imm]` 瀛楅潰閲忓姞杞斤紙485 澶勶級 |
| 鍘熺増 eboot/SELF **鍘熸牱鍔犺浇** | ~95% | ELF 娈垫槧灏勩€佸鍏ヨ〃/NID 瑙ｆ瀽銆佹ā鍧楀惎鍔ㄥ叏閮ㄥ彲鐢紱`module_start=0x81198464`銆乣IMG_VA_BASE=0x80600000`锛涘畬鏁?boot 閾撅細S0_LOGO 鈫?鍚姩 USM 褰辩墖锛坓uest 杞欢瑙ｇ爜锛夆啋 搴旂敤涓栫晫鎭㈠ |
| **HLE 绯荤粺 API 鎺ョ** | ~80% | 鍐呭瓨/绾跨▼/fiber/淇″彿閲?浜嬩欢鏍囧織/娑堟伅/鏂囦欢/GXM/闊抽(NGS)/CRI 鏃ュ織锛涙湰闃舵鏂板锛?*SceNgs HLE**锛坄P5D_NGS=1`锛屼慨澶?39MB 鍋囧昂瀵革級銆?*HLE I/O 娉?*锛坄P5D_IOPUMP=1`锛屾墦閫?criFs 璇诲畬鎴愶級銆?*HDRFIX**锛?4 浣?CPK 澶存爣鍑嗗寲锛夈€佸姩鎬?reader 娉点€丆RILOG 璋冪敤鐐瑰寮?|
| **CRI FS / CPK 铏氭嫙鏂囦欢绯荤粺** | ~95% | **CPK 鎸傝浇瀹屽叏鎵撻€?*锛歚[FIOS] mounted data.cpk files=10555` / `mounted vita.cpk files=531`锛宍[CRILOG]=0`锛涙爣棰樼礌鏉愬悕鍏ㄩ儴鏋氫妇锛坄title/title_p5.bin`銆乣title.spr4`銆乣title.p3/logo.tga` 鈥︼級骞堕€変腑 `title/title_p5.bin`锛涗笁椤?HLE 鍙樻崲锛歍OCSZPATCH锛?4 浣嶅彉浣?TocSize +0x18锛? TOCPAD锛圗OF 闆跺～鍏咃級+ TOCFIX锛堝悎鎴?TOC 娈靛ご銆乁TF 鍚庣Щ 0x10锛?|
| **GXM 鈫?Vulkan** | ~60% | 鐪熷疄 SPIR-V 绠＄嚎锛圙XP鈫扴PIR-V manifest 4331 鏉★級鍒涘缓鎴愬姛锛孊eginScene鈫扗raw鈫扲T鈫抯wapchain鈫抈vkQueuePresentKHR result=0` 鍏ㄩ摼鎵ц锛?*鐢婚潰鍐呭浠嶄负榛?*锛堢汗鐞?dummy / uniform 闆跺€?/ 椤剁偣鏁版嵁鏈牎楠岋紝M940鈥?45 閬楃暀闂锛?|
| 闊抽锛圫ceNgs / CRI Atom锛?| ~15% | SceNgs HLE 宸茶兘缁欏嚭姝ｇ‘ work size锛堜慨澶?E2009011903锛夛紱ACF/ACB 鍔犺浇涓庣湡瀹炶В鐮?杈撳嚭浠嶇己 |
| 杈撳叆锛圫ceCtrl / SceTouch锛?| ~10% | Pad/Touch HLE 楠ㄦ灦鍙敤銆佸彲娉ㄥ叆鎸夐敭锛涙父鎴忎晶杈撳叆娴佹湭鎺ラ€?|
| 瑙嗛瑙ｇ爜 | ~15% | 鍚姩 USM 褰辩墖鐢?guest 杞欢瑙ｇ爜璺戦€氾紱鐪熷疄 AVC/CRI Mana 瑙ｇ爜鏈畬鎴?|
| 鍦烘櫙閾?S0鈫扴4鈫扲ESULT | S0 鉁?/ S1 杩涜涓?| S0_LOGO 瀹屾暣閫氳繃锛汼1_TITLE 璧勬簮鍔犺浇锛圕PK 鎸傝浇鈫掓枃浠剁储寮曗啋title 绱犳潗锛夎繘琛屼腑锛汼2鈥揝4 鏈埌杈?|
| 鍗?exe 鎵撳寘 / 绉诲姩绔?| ~40% / 0% | 鐩墠鏄崟涓€ Windows 鍙墽琛?`recomp_harness.exe`锛圓OT+HLE 寮€鍙戞€侊級锛涙寮忔墦鍖?绉诲姩绔湭寮€濮?|

## 鏈€鏂版爣鍑嗚繍琛岀幆澧冿紙寮€鍙戣皟璇曠敤锛屽叏閮ㄩ粯璁ゅ叧闂級

```powershell
$env:P5D_NESTED_UNTIL=1500000; $env:P5D_GXMABI=1; $env:P5D_NOSCHEDRUN=1; $env:P5D_GETSELFFIX=1
$env:P5D_FASTSLEEP=1; $env:P5D_THRYIELD=1; $env:P5D_PERIODPUMP=1; $env:P5D_VTIME=1
$env:P5D_P1TICK=1; $env:P5D_RINIT=1; $env:P5D_OTFORCE=1; $env:P5D_STEPFORCE=1
$env:P5D_NOMOVIE=1; $env:P5D_FORCETITLE=1; $env:P5D_FIOSFILL=1; $env:P5D_WAITFORCE=1
$env:P5D_SCNFORCE=1; $env:P5D_THRPUMP=1; $env:P5D_PUMPMAIN=1; $env:P5D_POSTMAIN=1
$env:P5D_NGS=1; $env:P5D_IOPUMP=1
.\recomp_harness.exe 0x81198464 300000000
```

鍏朵腑鏈樁娈垫柊澧炵殑**蹇呴渶椤?*锛?
| 鍙橀噺 | 浣滅敤 |
|---|---|
| `P5D_NGS=1` | **蹇呴渶**锛歋ceNgs HLE锛堢己澶变細瀵艰嚧 39MB 鍋?work size 鈫?`E2009011903`锛?|
| `P5D_IOPUMP=1` | **蹇呴渶**锛欻LE 涓诲惊鐜车 criFs Load 鐘舵€佹満锛堟墦閫氳瀹屾垚/澶?reader 鑷姩鍙戠幇锛?|
| `P5D_POSTMAIN=1` / `P5D_PUMPMAIN=1` | 涓荤嚎绋嬭繑鍥炲悗缁х画 pump guest 绾跨▼锛圕PK 鎸傝浇绛夊緟闇€瑕侊級 |

## 寮€鍙戠姸鎬佸紑鍏筹紙褰撳墠闃舵锛屼粎璋冪爺鐢級

| 鍙橀噺 | 浣滅敤 |
|---|---|
| `P5D_GXMABI=1` | GXM 璋冪敤璧?ABI 妗ユ帴灞傦紙褰撳墠涓荤嚎蹇呴渶锛?|
| `P5D_NOSCHEDRUN=1` | 鍏抽棴鏃ц皟搴?hack锛屼娇鐢?guest 鑷繁鐨勫弻 fiber 鍚姩閾?|
| `P5D_GETSELFFIX=1` | 绾跨▼绾?`sceFiberGetSelf` 杩斿洖閲嶆柊姝﹁鐨?dummy锛堣皟搴﹀櫒鎭㈠鍒囨崲锛?|
| `P5D_THRYIELD=1` / `P5D_PERIODPUMP=1` | guest 绾跨▼鍚堜綔璁╁嚭 + 鍛ㄦ湡鎬ф姠鍗?pump |
| `P5D_VTIME=1` | 杩涚▼鏃堕棿/RTC 杩斿洖铏氭嫙鏃堕棿锛堥厤鍚堣櫄鎷熺潯鐪狅級 |
| `P5D_FASTSLEEP=1` | 铏氭嫙鐫＄湢鍔犻€?|
| `P5D_OTFORCE=1` / `P5D_STEPFORCE=1` / `P5D_WAITFORCE=1` | boot/page 鐘舵€佹満閫夋嫨鎬у己鍒讹紙鐮旂┒鐢級 |
| `P5D_NOMOVIE=1` / `P5D_FORCETITLE=1` | 璺宠繃 USM 褰辩墖/鐩村啿鏍囬锛堝紑鍙戝姞閫燂級 |
| `P5D_FIOSFILL=1` / `P5D_SCNFORCE=1` / `P5D_THRPUMP=1` | FIOS/鍦烘櫙/绾跨▼娉靛姞閫熷紑鍏?|
| `P5D_P1TICK=1` | 姣?1M n 鎵撳嵃杩涘害锛坈tr/seq/pc/GXM 璁℃暟锛?|
| `P5D_CRIFSHLE=1` | 鍘嗗彶寮€鍏筹細鐢?HLE 璋?guest `criFs_Initialize`锛堢幇鍦ㄦ父鎴忚嚜韬?init 宸茶窇閫氾紝涓嶅啀蹇呴渶锛?|

## 浠撳簱缁撴瀯

```
docs/
  P5D_MASTER.md              鏉冨▉鐮旂┒/寮€鍙戣褰曪紙鍏ㄩ儴閲岀▼纰戙€佽瘉鎹€佺粨璁轰笌鍘嗗彶绾犳锛岀害 36MB锛?  P5D_DIGEST.md              绮剧畝鎽樿锛堣剼鏈嚜鍔ㄧ敓鎴愶紝鍕挎墜鏀癸級
  P5D_OPERATION_CHECKLIST.md 鎿嶄綔/楠岃瘉娓呭崟
  REUSABLE_KNOWLEDGE.md      鍙鐢ㄧ煡璇嗘竻鍗曪紙鍘嗗彶鎴愭灉/宸ュ叿/瑙勬牸/璐熺粨鏋滐紝鎸?ROI 鎺掑簭锛?  COMPARISON_VITA3K.md       涓?Vita3K 妯℃嫙鍣ㄧ殑鍖哄埆
  GXM_ABI_TEST_REPORT.md     GXM ABI 妗ユ帴娴嬭瘯鎶ュ憡
  HLE_DIFF_REPORT.md         HLE 涓庣湡瀹炵郴缁熺殑宸紓鎶ュ憡
  SCHED_SPIN_ANALYSIS.md     璋冨害/鑷棆寰幆鍒嗘瀽
  P5R_ANDROID_REFERENCE.md   鍚屾棌寮曟搸绉绘椤圭洰锛圥5R Android锛夊弬鑰冪爺绌?  RELEASE_COMPLIANCE.md      鍙戝竷鍚堣绛栫暐锛堢孩绾?浜や粯褰㈡€?鏈湴缂撳瓨锛?  _MD_INDEX.md               鏂囨。绱㈠紩
tools/
  release_audit.py           鍙戝竷鏍戝悎瑙勬壂鎻忥紙绂佹娓告垙鏂囦欢/娓告垙琛嶇敓鏁版嵁锛?  kb_audit.py                鐭ヨ瘑搴撻敋鐐瑰璁★紙姣忎釜閲岀▼纰戝繀椤昏拷鍔犻敋鐐癸級
  verify_doc_integrity.ps1   鏂囨。瀹屾暣鎬ф牎楠?  run_kb_audit.ps1 / run_all_regressions.ps1   涓€閿牎楠?  aot_difftest.py            AOT 宸垎娴嬭瘯
  diag.py / hle_probe.py     璇婃柇涓?HLE 鎺㈡祴宸ュ叿
engine/
  recompiler/_gen_split.py   ARM/Thumb 鈫?C++ AOT 閲嶇紪璇戠敓鎴愬櫒
  recompiler/_gen_batch2.py  鎸囦护鍙戝皠鍚庣锛堢Щ浣?鍐欏洖/瀛楅潰閲?LDRD 绛夎涔変慨姝ｏ級
  runtime/recomp_harness.cpp 闈欐€侀噸缂栬瘧杩愯鏃?+ HLE锛堝唴瀛樻槧灏勩€佺嚎绋?fiber銆丟XM/鏂囦欢/Pad銆両/O 娉点€丠DRFIX锛?  runtime/gxm_abi.cpp        GXM ABI 妗ユ帴灞傦紙瀵煎叆 stub + 鎸囬拡缈昏瘧锛?  runtime/cpk_reader.h       CPK 铏氭嫙鏂囦欢绯荤粺 + CRILAYLA 瑙ｅ帇锛坔eader-only锛?  gxm/                       GXM 鈫?Vulkan 鍚庣锛坰ce_gxm銆乿ulkan_backend銆乼exture_format锛?  mem/                       铏氭嫙鍦板潃绌洪棿娉ㄥ唽/缈昏瘧锛堝鏈?VA 鈫?瀹夸富鎸囬拡锛?  common/                    鍏叡绫诲瀷涓庢棩蹇?锛堝紑鍙戝伐浣滃尯鍙︽湁 AOT 鐢熸垚鐨?batch_000..batch_035.cpp銆乺ecomp_harness.exe 绛夛紝涓嶈繘浠撳簱锛?```

## 璁捐璺嚎

1. **鍘熺増浜岃繘鍒朵竴瀛椾笉鏀?*锛氱洿鎺ヨ鍙栫帺瀹舵彁渚涚殑 eboot/SELF锛堜笉淇敼銆佷笉閲嶆墦鍖咃級銆?2. **闈欐€侀噸缂栬瘧**锛氬惎鍔ㄥ墠涓€娆℃€ф妸 ARM/Thumb 鍏ㄩ噺缈昏瘧涓?x86-64锛堟湰鏈虹敓鎴愬苟缂撳瓨锛夛紝鎵ц璺緞涓嶅仛杩愯鏃朵唬鐮佺敓鎴愶紱瑙ｉ噴鍣ㄤ粎浣?fallback銆?3. **HLE 鍙浛鎹㈢郴缁熻竟鐣?*锛氭父鎴忎唬鐮佸師鏍锋墽琛岋紝鏂囦欢/鍐呭瓨/绾跨▼/鍥惧舰/闊抽/杈撳叆绛夌郴缁?API 鐢卞吋瀹瑰眰瀹炵幇銆?4. **GXM鈫扸ulkan 鍦?API 灞?*锛氫笉妯℃嫙 GPU 鍛戒护娴侊紝鎶?SceGxm 璋冪敤鏄犲皠涓?Vulkan 璋冪敤锛涚潃鑹插櫒鐢辩帺瀹舵暟鎹腑鐨?GSC/GXP **鍦ㄧ帺瀹舵満鍣ㄤ笂涓€娆℃€х炕璇?*涓?SPIR-V 骞剁紦瀛樸€?5. **鍙戝竷褰㈡€?*锛歚P5D_Engine.exe`锛堝紩鎿庯紝鏃犳父鎴忔暟鎹級 + 鐜╁鑷娓告垙鏁版嵁 + 鏈湴缂撳瓨锛坄cache/recomp`銆乣cache/shaders`銆乣cache/pipelines`锛夈€?
## 楠岃瘉涓庡璁?
```powershell
py tools/release_audit.py .        # 鍙戝竷鏍戝悎瑙勶紙PASS 鎵嶅彲鎻愪氦锛?powershell -File tools/run_kb_audit.ps1
powershell -File tools/verify_doc_integrity.ps1
powershell -File tools/run_all_regressions.ps1
```

姣忎釜閲岀▼纰戣姹傦細鏉冨▉缁撹鍐欏叆 `docs/P5D_MASTER.md` 鐨勩€孉NTI-FORGETTING ENTRY銆? `tools/kb_audit.py` 杩藉姞閿氱偣锛?浠ヤ笂鍥涢」妫€鏌ュ叏缁垮悗鎵嶄細鎻愪氦锛涘巻鍙茶褰曞彧杩藉姞銆佷笉鍒犳敼銆?
## 鏋勫缓涓庤繍琛岋紙寮€鍙戜腑锛?
褰撳墠浠撳簱浠嶆槸**寮€鍙戜腑闂寸姸鎬?*锛屽皻鏈彁渚涘紑绠卞嵆鐢ㄧ殑鏋勫缓鑴氭湰锛涘悗缁細琛ュ厖锛?
- 缂栬瘧 GXM鈫扸ulkan 鍚庣涓庤繍琛屾椂锛圡inGW g++ / MSVC + Vulkan SDK锛?- AOT 鐢熸垚鍣ㄧ敤娉曪紙杈撳叆锛氱帺瀹惰嚜澶?eboot锛涜緭鍑猴細鏈湴浠ｇ爜缂撳瓨锛?- 棣栨杩愯 "Prepare" 娴佺▼锛堟湰鍦?AOT + 鐫€鑹插櫒缈昏瘧锛?
## 鍚堣涓庡厤璐ｅ０鏄?
- 鏈粨搴撳彧鍖呭惈**鎴戜滑鑷繁鐨勫紩鎿?宸ュ叿婧愪唬鐮?*涓?*鐮旂┒璁板綍**锛涗笉鍖呭惈涔熶笉鎺ュ彈浠讳綍娓告垙鏂囦欢銆佸瘑閽ャ€佸浐浠躲€佺敱娓告垙鏁版嵁缈昏瘧寰楀埌鐨勪簩杩涘埗锛堝 SPIR-V 缂撳瓨锛夋垨娓告垙鐢婚潰绱犳潗銆?- 鏈」鐩笌 Atlus / SEGA / Sony 鏃犲叧锛涗粎鐢ㄤ簬**浜掓搷浣滄€х爺绌朵笌涓汉澶囦唤娓哥帺**銆傝浣跨敤鑷繁鍚堟硶鎸佹湁鐨勬鐗堟父鎴忋€?- 璁稿彲璇侊細**MIT**锛堝鏉捐鍙紝瑙?[LICENSE](LICENSE)锛夈€傛湰浠撳簱涓殑婧愪唬鐮併€丄OT 鐢熸垚鍣ㄣ€佸伐鍏蜂笌鐮旂┒鏂囨。鍧囦负 MIT锛涚涓夋柟缁勪欢璁稿彲瑙?[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)銆?- MIT 浠呰鐩栨湰浠撳簱涓垜浠師鍒涚殑鍐呭锛涙父鎴忔湰浣撲笌娓告垙绱犳潗鐗堟潈褰?ATLUS / SEGA锛?*涓嶅湪 MIT 鎺堟潈鑼冨洿鍐?*锛屼笖鏈粨搴撲笉鍖呭惈浠讳綍娓告垙鏁版嵁銆?
## 鐩稿叧璁板綍

- 鏉冨▉寮€鍙戣褰曪紙鍚墍鏈夐噷绋嬬涓庣粨璁猴級锛歔docs/P5D_MASTER.md](docs/P5D_MASTER.md)
- 椤圭洰鎽樿锛歔docs/P5D_DIGEST.md](docs/P5D_DIGEST.md)
- 涓?Vita3K 妯℃嫙鍣ㄧ殑鍖哄埆锛歔docs/COMPARISON_VITA3K.md](docs/COMPARISON_VITA3K.md)
- 鍙戝竷鍚堣锛歔docs/RELEASE_COMPLIANCE.md](docs/RELEASE_COMPLIANCE.md)
- HLE 宸紓鎶ュ憡锛歔docs/HLE_DIFF_REPORT.md](docs/HLE_DIFF_REPORT.md)

## License

GNU General Public License, version 2 (see LICENSE). Third-party references and attribution: see NOTICE.md.

