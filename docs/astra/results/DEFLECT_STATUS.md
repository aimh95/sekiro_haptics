# DEFLECT_STATUS — 플레이어 튕겨내기 / 일반 방어 감지

작성 2026-09-12. 실행 환경: Windows 10 19045, `C:\workspace\sekiro_haptics`, 브랜치 `feature/sekiro-probe-disk-backed`.

## 0. 최종 상태 요약 (4단계 구분)

| 단계 | 상태 | 근거 |
|---|---|---|
| 코드 변경 + 합성 테스트 통과 | **완료** | 아래 2절. C++ 625/625, ctest 3/3 |
| 정적 분석 후보 확보 | **완료** | 아래 3절. 6절에서 라이브로 확증됨 |
| 실게임 관측 | **완료 (2026-09-18)** | 아래 6절. 신호 확정 + 플레이어 소유 증명 |
| 별도 세션에서 패링·방어 구분 검증 | **통과 (2026-09-18)** | 7.7절. 독립 영상 정답은 여전히 없음 |
| 연속 패링/연속 방어 구분 | **held-out 세션까지 통과** | 7.6·7.7절. `+0x3C` 1프레임 펄스 |
| 객체 교체 후 지속 동작 | **통과 (2026-09-19)** | 7.9절. 지역 이동 관통 확인 |

**합성 fixture 통과는 게임 인식률이 아니다.** 이 문서의 어떤 수치도 실제 감지 성능이 아니다.

---

## 1. 기존 작업 보존과 ZIP 변경 반영

### 적용 방식
`SekiroHaptics_Feedback_Implementation.zip`을 프로젝트 밖 `C:\tmp\sekiro_feedback_zip`에 풀고,
`update/apply_update.py`를 **검사 모드로 먼저** 실행했다.

```
python update/apply_update.py --repo "C:/workspace/sekiro_haptics"
  → Preflight passed: 48 files to update, 0 already current.
python update/apply_update.py --repo "C:/workspace/sekiro_haptics" --apply
  → Updated 48 files. Original files backed up at:
    C:\workspace\sekiro_haptics\feedback_update_backups\20260912T010132Z-ag4a3716
```

적용 전에 48개 항목을 전수 분류했고 **기존 26개 파일 전부가 manifest의 baseline 해시와 정확히 일치**(conflict 0, 이미 최신 0)했다. 해시 목록 변조·검사 무력화·`git reset`/`clean`·소스 전체 덮어쓰기는 하지 않았다. 커밋하지 않은 로컬 작업(`MonotonicDebounce.hpp`, `RisingEdgeDetector.hpp`, `test_monotonic_debounce.cpp`, `test_rising_edge_detector.cpp`, `test_sekiro_combat_capture_integration.cpp`, `tests/CMakeLists.txt`의 해당 항목)은 그대로 보존됐다.

### 지시문이 지목한 5개 수정 — 모두 아직 필요한 수정이었음을 diff로 확인

| 항목 | 적용 전 로컬 코드 | 적용 후 |
|---|---|---|
| capture 재시작 시 기존 기록 보존 | `stream_.open(path, ios::trunc)` — 기존 캡처 파괴 | `O_CREAT\|O_EXCL`(Win `_wsopen_s`+`_SH_DENYWR`) 단일 원자 연산, 존재 시 `OutputExists` |
| 객체/root/child 변경·첫 읽기 실패 후 기준값 폐기 | discontinuity 시 새 객체를 즉시 재baseline, **읽기 실패해도 이전 바이트 유지** | `previousBytes_.clear()`를 새 소유자 첫 읽기 **전에** 수행, `baselineValid_=false`, 주소 변경도 discontinuity로 취급 |
| baseline / 무변화 성공 샘플 / 실패 / gap 기록 | 없음(delta만) | schema v3: `capture_start`, `baseline`, `gap`, `dropped`, `discontinuity` 레코드 추가 |
| 분석 시 겹치는 시도 중복 집계 제외 | 없음 | `overlappingTrialsExcluded` / `discontinuousTrialsExcluded`로 분자·분모 **양쪽에서** 제외 |
| 입력 worker 종료 대기 후 핸들 해제 | `readerThread.detach()` 등 4개 스레드 전부 detach | `StdinWorker`(jthread + `CancelSynchronousIo` 재시도 + join), 나머지 3개도 `request_stop()`+`join()` |

### 내가 추가로 수정한 2건 (이 PC 툴체인 대응)

이 PC에는 **MSVC가 없고** 기존 빌드는 MinGW-w64 UCRT(g++ 16.1.0) + Ninja다. 문서의 `-A x64` MSVC 명령은 그대로 쓸 수 없어 환경에 맞췄고, 그 과정에서 나온 두 컴파일/링크 오류를 고쳤다.

1. [apps/sekiro_signal_probe/main.cpp](../../../apps/sekiro_signal_probe/main.cpp) — `CancelSynchronousIo(worker_.native_handle())`.
   libstdc++/MinGW의 `std::jthread::native_handle_type`은 winpthreads `pthread_t`(정수 id)라서 `HANDLE`로 변환되지 않는다(MSVC STL에서는 이미 `HANDLE`). `Win32ThreadHandle()` 헬퍼를 추가해 MSVC는 그대로, 그 외에는 `pthread_gethandle()`로 변환한다. **동작 의도 변경 없음.**
2. [CMakeLists.txt](../../../CMakeLists.txt) — `sekiro_haptics_windows_audio`에 `ksuser` 링크 추가.
   `KSDATAFORMAT_SUBTYPE_PCM/IEEE_FLOAT` GUID가 MSVC의 `uuid.lib`에는 있으나 MinGW에서는 `libksuser.a`에 분리돼 있어 링크가 실패했다. 이번 감지 작업과 무관한 오디오 출력 경로지만, 업데이트 적용으로 기본 빌드가 깨진 상태를 남기지 않기 위해 고쳤다.

HID 트랜스포트는 `-DSEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT=OFF`로 비활성화했고, 이번 조사의 의존성으로 삼지 않았다.

---

## 2. Windows 빌드와 테스트 — 실제 실행 명령과 결과

### 빌드 (실제로 실행한 명령)

```powershell
$mg='C:\Users\aimh9\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin'
cmake -S . -B build-deflect -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="$mg/g++.exe" -DSEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT=OFF
cmake --build build-deflect
```

결과: **성공** (115/115 타깃). `sekiro_haptics_signal_probe`, `sekiro_haptics_tests`, `capture_v3_fixture` 모두 생성.

### 테스트 (실제 결과)

```powershell
ctest --test-dir build-deflect --output-on-failure --timeout 300
```

```
1/3 sekiro_haptics_unit_tests ....  Passed   13.74 sec   (625/625 tests passed)
2/3 capture_v3_decoder ..........   Passed    0.16 sec   (Python 11/11)
3/3 deflect_signal_analysis .....   Passed    0.61 sec   (Python 22/22)
100% tests passed out of 3
```

`capture_v3` decoder와 `deflect_signal` 분석 테스트는 CMake가 실제로 정의한 CTest 이름(`capture_v3_decoder`, `deflect_signal_analysis`)으로 실행했다. 테스트를 삭제하거나 기대값을 약화하지 않았고, 과거 문서의 528/587 같은 숫자에 맞추지 않았다. 현재 실제 수는 **C++ 625, Python 11 + 22**다.

### 실패를 낸 뒤 원인을 분리한 2건 (둘 다 소스 버그 아님 — 환경/빌드 설정)

1. **테스트 실행 파일이 시작조차 못 하고 ctest가 멈춤** — 종료코드 `0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND`.
   원인: PATH에서 `libwinpthread-1.dll`이 **Anaconda의 오래된 사본**(`E:\Users\aimh9\anaconda3\Library\mingw-w64\bin`)으로 먼저 해석됨. g++ 16.1.0의 libstdc++가 요구하는 심볼이 없어 로더가 실패하고 모달 오류가 ctest를 블록했다.
   조치: 산출물 디렉터리에 정확한 런타임 DLL 3개(`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`)를 동봉. 이후 PATH 조작 없이 통과.
2. **`SignalProbe_Integration_RealHelperProcess_ScanFilterAndWatchCapture` 실패** (`decreased.size() == 1`).
   이 테스트는 헬퍼 **프로세스의 main module**을 실제로 스캔해 "감소한 u32가 정확히 1개"임을 요구한다. 내가 일시적으로 넣었던 `-static` 링크가 헬퍼 exe의 main module 안에 정적 CRT의 가변 데이터를 끌어들여 감소 후보가 하나 더 생겼다.
   검증: 동일 소스를 동적 링크로 빌드하면 3회 연속 625/625 통과, `-static`이면 4회 연속 실패 → **무작위 flake가 아니라 링크 설정이 원인**. `-static`을 제거해 해결. 소스·테스트는 건드리지 않았다.

---

## 3. 실제 게임 판정 신호 조사 — 이번 핵심

### 3.1 조사 대상 고정

로컬 설치를 Steam 라이브러리에서 찾았다(업로드·외부 전송 없음, 읽기 전용).

- 경로: `E:\Program Files\Steam\steamapps\common\Sekiro\sekiro.exe`
- SHA-256: `637aca527538c0ec6e1f136c8ed66046e95dfbdbb1f51926e134d9916398b856`
- 크기: `68005144` 바이트, FileVersion `1.6.0.0`, Steam buildid `5794815`
- 모드: **없음**(`dinput8.dll`/ModEngine 등 주입 DLL 부재, 동봉 DLL 7개 전부 원본 구성)

**저장소의 identity gate와 정확히 일치한다.** [apps/sekiro_signal_probe/main.cpp:166-169](../../../apps/sekiro_signal_probe/main.cpp#L166-L169)의 `MakeKnownGoodSekiroIdentity()`가 `fileSizeBytes = 68005144`, sha256 `637aca52...`로 하드코딩돼 있어, 이 PC의 실제 EXE에서 hash gate가 열린다. (gate 해제·우회는 하지 않았다.)

### 3.2 **결정적 제약: 디스크상의 `.text`는 Steam DRM으로 암호화되어 있다**

정적 분석 도중 확인한 사실이다. 이것이 이번 조사 범위를 규정한다.

- PE 엔트리포인트 RVA `0x429f310` → **`.bind` 섹션**(SteamStub 래퍼 섹션)
- `.text` 섹션 엔트로피 **8.000/8.0 (최대치)**, 43MB 전체가 난수와 구분되지 않음
- `.text` 전역에서 `48 8D xx` (`lea reg,[rip+disp32]`) 패턴이 **단 20개** — 43MB 실제 x86-64 코드에서 불가능한 수치
- 반면 `.rdata`(4.309) / `.data`(3.707)는 정상 엔트로피 — **문자열·RTTI·테이블은 평문으로 남아 있다**

**따라서 디스크 EXE에서 함수 디스어셈블·분기 추적·offset 도출은 불가능하다.** 코드는 실행 중 프로세스 메모리에서만 복호화된다. 이 사실이 "정적 분석으로 offset을 확정한다"는 원래 계획을 대체했고, 아래 후보는 **평문으로 남은 자료(문자열·RTTI·ID 테이블)에서만** 도출했다.

### 3.3 가장 강한 후보 — `IsSucceededJustGuard` (플레이어 캐릭터 상태 플래그)

게임 내부 용어로 **튕겨내기 = ジャストガード(Just Guard) / 약어 ジャスガ**이며, 일반 가드와 **엔진 전 계층에서 분리된 별개 결과**임을 로컬 바이너리에서 직접 확인했다.

**(a) 상태 플래그 — 이번 목표에 가장 직접적**

```
VA 0x142a725b0 [.rdata], UTF-16
"IsSucceededJustGuard|(前回のガード時に)ジャスガ成功したか[%s]"
   = "직전 가드 시점에 저스트가드가 성공했는가"
```
바로 앞 항목:
```
VA 0x142a72588  "IsGuard|ガード中か[%s]"        = "가드 중인가"
```
두 항목 모두 `[%s]`(bool) 포맷이고 연속 배치돼 있다. 앞뒤 `IsDivingState|水中か[%d]`(0x142a724f8), `IsEnableAlwaysCancelState|...[%d]`(0x142a72530)와 함께 **캐릭터 상태 플래그 묶음**을 이룬다.

이것이 찾던 것에 정확히 해당한다: **일반 방어(0)와 튕겨내기 성공(1)을 구별하는, 캐릭터에 귀속된 지속 상태**.

**(b) 반응 동작이 분리되어 있다 — EzState 커맨드 ID 테이블 (평문 `.data`)**

`{이름 포인터, ID}` 쌍 배열이 `.data`에 평문으로 남아 있다:

| VA(이름) | 문자열 | ID | 의미 |
|---|---|---|---|
| 0x142a74f28 | ガード弾き挙動取得 | **0xBF1** | 일반 가드로 **튕겨낸** 동작 취득 |
| 0x142a74f40 | ジャストガード弾き挙動取得 | **0xBF2** | **저스트가드로** 튕겨낸 동작 취득 |
| 0x142a76468 | ガード弾かれ挙動取得 | **0xBCB** | 가드가 **튕겨진** 동작 취득 |
| 0x142a76480 | ジャストガード弾かれ挙動取得 | **0xBCC** | **저스트가드가** 튕겨진 동작 취득 |
| 0x142a75d70 | ガードレベル取得 | 0x11C | 가드 레벨 취득 |

일반/저스트가 **다른 ID의 다른 커맨드**라는 것은, 플레이어 반응 애니메이션이 두 결과에서 갈린다는 뜻이다. 지시문이 말한 대체 후보("플레이어 반응 애니메이션/상태")가 실재함을 뒷받침한다.

**(c) 히트 이펙트 파라미터도 분리되어 있다**

```
RTTI: .?AVHitEffectSeJustGuardParam@NS_SPRJ@@          (VA 0x143ceee38)
RTTI: .?AVHitEffectSfxConceptJustGuardParam@NS_SPRJ@@  (VA 0x143ceee70)
파라미터명(UTF-16): "HitEffectSeJustGuardParam"          (VA 0x142b99f38)
                    "HitEffectSfxConceptJustGuardParam"  (VA 0x142b99ea0)
```
일반판 `HitEffectSeParam` / `HitEffectSfxConceptParam`과 **별도 타입**이다. 히트 해결 시점에 JustGuard 분기가 존재한다는 증거다.

**(d) 디버그 강제 토글의 존재**
```
VA 0x142a30db0  "Force Just Guard Mode｜強制ジャストガードする【%s】"
```

### 3.4 "방어자가 플레이어인가"를 판별할 **실행 가능한 수단** (RTTI로 정적 확보)

RTTI 타입 디스크립터가 평문이라 vftable 주소를 정적으로 계산할 수 있었다. **플레이어와 적은 서로 다른 구체 타입**이다:

| 클래스 | vftable RVA | 가상 메서드 수 | slot0(소멸자) RVA |
|---|---|---|---|
| `SprjPlayerDamageModule` | **0x2A7DA48** | 23 | 0xB72E30 |
| `SprjEnemyDamageModule` | **0x2A7D628** | 23 | 0xB70D20 |
| `SprjChrDamageModule` (기반) | 0x2A7CD70 | 23 | 0xB6FBF0 |
| `SprjChrActionFlagModule` | 0x2A72158 | 2 | 0xB2A650 |
| `CSChrToughnessModule` (체간) | 0x2A8F2D8 | 2 | 0xBE55E0 |
| `SprjChrBehaviorModule` | 0x2A790A8 | 2 | 0xB4C750 |
| `SprjPlayerHitStopModule` | 0x2A83598 | 2 | 0xB9E1F0 |

전체 메서드 RVA는 [docs/astra/results/static/vftables.json](static/vftables.json), 추출 스크립트는 [static/rtti.py](static/rtti.py) · [static/xref.py](static/xref.py) · [static/dump_vft.py](static/dump_vft.py)에 있다(모두 읽기 전용 분석).

**활용:** 런타임에 후보 객체의 첫 qword(vptr)를 읽어 `모듈베이스 + 0x2A7DA48`와 비교하면 **그 damage module이 플레이어의 것임을 타입 수준에서 증명**할 수 있다. "읽기가 성공했다"보다 훨씬 강한 소유권 근거다. 지시문의 "방어자가 플레이어임을 어떻게 확인하는가"에 대한 답이 여기에 있다.

### 3.5 부수적으로 확정된 사실

- **체간(Posture)의 내부 이름은 Toughness / 体幹(Taikan)**이며 `CSChrToughnessModule`이 소유한다(디버그 문자열 `体幹|Taikan: %d` @ 0x1429cce48, `補正あり体幹|Taikan : %d` @ 0x1429cd050). **`PlayerGameData`가 아니다.**
- 따라서 기존 `combat-resolve`의 `GameDataMan → PlayerGameData` 경로는 **이 후보들이 있는 곳이 아니다.** 그 영역은 세이브/스탯 계열이고, JustGuard 플래그와 체간 모듈은 `WorldChrMan → (Player)ChrIns → modules`에 매달린다. 기존 경로에서 좋은 후보가 안 나온 것은 예상된 결과이며, 그 사실로 결과 경로가 없다고 결론 내리지 않았다.

### 3.6 반증하거나 배제한 후보

| 후보 | 판정 | 근거 |
|---|---|---|
| ASCII 문자열 `Deflect` (6건) | **배제** | 전부 Havok 물리(`hknpDeflectedLinearCast...`). 전투 무관 |
| `SprjGoalParry`, `SprjGoalGuardBreakAttackBase` | **배제(플레이어 이벤트 아님)** | `SprjGoal*`은 **적 AI 행동 목표** 계열. "적이 내 공격을 튕김"이지 플레이어 성공이 아니다 |
| `Repel` / `PerfectDeflect` 라는 영문 필드 | **부존재** | 바이너리 전체에 `Repel` 0건, `PerfectDeflect` 0건. 별도 필드 가정은 근거 없음 |
| `GameDataMan → PlayerGameData` HP/체간 | **의미 검증 실패** | 3.5 참조. 읽힌다는 사실은 패링 의미 검증이 아니다 |
| 디버그 문자열의 코드 xref로 offset 도출 | **불가** | `IsSucceededJustGuard` 등에 대한 rip-relative/절대 참조 **0건** — `.text` 암호화 때문이며, 릴리스에서 디버그 메뉴 등록 코드가 제거됐을 가능성도 있음 |

### 3.7 **아직 unknown인 것 (임의로 채우지 않음)**

- `IsSucceededJustGuard`의 **소유 객체와 필드 offset** — unknown
- `WorldChrMan → PlayerIns → (해당 모듈)` **포인터 경로** — unknown
- 값의 **수명**(다음 가드까지 유지된다는 것은 문자열 의미상의 추정이며 미관측)
- **연속 성공 구별 수단** — unknown. bool이라면 연속 패링 시 1이 유지되어 2번째 이후를 놓칠 가능성이 크다(문서가 경고한 FN 패턴). 발생 카운터나 발생 식별자는 아직 찾지 못했다
- `IsGuard`/`IsSucceededJustGuard`가 `SprjChrActionFlagModule` 소속이라는 것은 **문자열 리터럴의 TU 배치 근거에 기반한 추정**이며 확정이 아니다(해당 클래스명 문자열 0x142a71f00과 다음 클래스명 0x142a726a8 사이에 위치)

---

## 4. 남은 문제

1. **오프셋을 얻으려면 살아 있는 프로세스가 필요하다.** 디스크 `.text`가 암호화되어 있으므로, 분기 추적형 정적 분석은 이 PC에서 추가 도구를 넣어도 불가능하다(언패킹은 게임 파일 변조 영역이라 수행하지 않았다). 남은 합법적 경로는 **실행 중 프로세스의 읽기 전용 관측**이다.
2. probe의 스캔은 `u8|u16|u32|i32|f32`만 지원한다. vftable 비교(u64)를 스캐너로 직접 돌릴 수 없으므로, 1차로는 **bool 차분 스캔**으로 좁히고, 후보가 나온 뒤 그 주소 주변을 `combat-capture custom-address`로 읽어 vptr을 확인하는 2단 절차가 필요하다.
3. `custom-address` 캡처는 고정 주소라 객체 교체를 추적하지 못한다. 사망·로드·워프 전에 종료해야 한다.

---

## 5. 라이브 사전 점검 결과 (2026-09-18 실행) 와 확정된 절차

게임이 실행 중인 상태(pid 17508)에서 probe를 붙여 **실제로 확인한 사실**이다. 아직 스캔은 시작하지 않았고 캡처도 만들지 않았다.

### 5.1 확인된 것

| 항목 | 결과 |
|---|---|
| probe attach | **성공 — 관리자 권한 불필요** (`attached to pid 17508`) |
| identity gate | **열림.** `fileSizeBytes=68005144 sha256=637aca52...b856` — 저장소의 known-good과 일치 |
| main module | `sekiro.exe base=0x140000000 size=0x42d2000` |
| 쓰기 | 없음. `this tool never writes to the target process -- read-only for the whole session.` |

`base=0x140000000`이 image base와 같으므로, 3.4절의 vftable RVA는 현재 세션에서 **VA = RVA + 0x140000000**으로 그대로 대조할 수 있다. (재실행 시 ASLR로 바뀔 수 있으므로 세션마다 base를 다시 읽어야 한다.)

### 5.2 `plan`이 잡아낸 제약 — **출력 디렉터리를 E: 드라이브에 두어야 한다**

```
plan u8 private-readable
  scopeBytes=7565840384 (7.05 GiB)   regions=4088
  alignedValueCount=7565840384
  estimatedInMemoryBytes=169.11 GiB  → inMemoryFeasible=no
  estimatedPeakDiskBytes=70.47 GiB
```

출력을 기본 위치(C:, 여유 38.1 GiB)에 두면 **`plan failed: InsufficientDiskSpace`**로 거부된다. 드라이브 여유는 C: 38.1 / D: 70.1 / **E: 505.5 GiB**이므로 E:를 쓰면 통과한다.

| 타입 | 후보 수 | 예상 peak disk | E:에서 |
|---|---|---|---|
| `u8` | 7,565,840,384 | **70.47 GiB** | `diskBackedFeasible=yes` |
| `u32` | 1,891,460,096 | **28.19 GiB** | `diskBackedFeasible=yes` |

`u8`이 bool에 정확하고 전 바이트를 덮지만 느리다. `u32`는 2.5배 빠르고, bool이 dword 안에서 뒤집히면 그 dword도 증감하므로 여전히 잡힌다 — 다만 같은 dword의 다른 바이트가 반대로 움직이면 상쇄될 수 있다. **1차는 `u8` 권장, 패스가 너무 느리면 `u32`로 재시도.**

in-memory `begin`은 두 타입 모두 불가(512 MiB 예산 초과)이므로 **`begin-disk`를 쓴다.**

### 5.3 실행 절차

**준비**
1. 세키로 그래픽 설정을 **테두리 없는 창(borderless)** 으로 둔다. probe 콘솔이 화면 우상단에 항상 위로 고정되는데, 전체화면(exclusive fullscreen)이면 보이지 않는다.
2. **황폐한 사원의 불사신 한베**에게 대련을 신청한다. 죽지 않아 객체 교체가 없고 패링/방어를 무한 반복할 수 있다.
3. 일반 PowerShell(관리자 불필요)에서:

```powershell
cd C:\workspace\sekiro_haptics
.uild-deflectpps\sekiro_signal_probe\sekiro_haptics_signal_probe.exe --process-name sekiro.exe --output E:\sekiro_scan\deflect-live-01
```

**핵심 원리:** `IsSucceededJustGuard`는 *직전 가드의 결과*라 다음 가드까지 값이 유지된다. 그래서 "행동 1회 → 가드 금지 상태로 스캔 완료까지 대기"를 반복하면 된다. 대기 중에는 **한베의 사정거리 밖으로 물러나 가만히 있는다** (맞아도 가드가 아니므로 값은 유지되지만, 조용할수록 노이즈 후보가 줄어든다).

**스캔**

먼저 게임에서 **일반 방어를 1회** 하고 물러난 뒤, probe 콘솔에 타이핑:
```
begin-disk u8 private-readable
```
7.05 GiB baseline을 쓰므로 몇 분 걸린다. 완료 출력이 나올 때까지 가드 입력을 하지 않는다.

이후는 **알트탭 없이 전역 단축키로** 진행한다 (게임에 포커스가 있어도 동작):

| 순서 | 게임에서 할 행동 | 누를 키 | 의미 |
|---|---|---|---|
| 1 | **패링 성공** 1회 → 물러나 대기 | `Insert` | filter increased (0→1) |
| 2 | **일반 방어** 1회 → 물러나 대기 | `PageUp` | filter decreased (1→0) |
| 3 | **패링 성공** 1회 → 물러나 대기 | `Insert` | filter increased |
| 4 | **일반 방어** 1회 → 물러나 대기 | `PageUp` | filter decreased |
| 5 | **패링 성공** 1회 → 물러나 대기 | `Insert` | filter increased |

각 단계 사이에 `End`(status)로 남은 후보 수를 본다. 다음 키를 누르기 전에 **직전 filter가 끝났다는 출력**을 반드시 확인한다.

**후보 보고**
```
Home   (= list 10)
End    (= status)
```
출력을 그대로 알려주면, 3.4절 vftable(`SprjPlayerDamageModule` RVA `0x2A7DA48` 등)과 대조해 플레이어 소유 객체인지 판정하고, 이어서 `combat-capture custom-address` + `combat-mark`로 라벨 있는 raw 캡처를 뜨는 절차를 준다.

**아직 하지 않을 것:** 진동/트리거/스피커 출력 연결. 소유권·의미·수명·연속 발생 구별이 확인되기 전에는 연결하지 않는다.


---

## 6. 라이브 관측 결과 (2026-09-18) — **신호 확정**

세키로 pid 10132, 모듈 base `0x140000000`, EXE는 3.1절과 동일(`637aca52...b856`). 전 과정 읽기 전용.

### 6.1 확정된 신호

```
클래스      SprjChrActionFlagModule   (vftable RVA 0x2A72158 — 3.4절에서 정적으로 추출)
필드 오프셋  +0xE10 (3600)
타입        u8 (bool)
의미        일반 방어 = 0,  튕겨내기(Just Guard) 성공 = 1
수명        다음 가드까지 유지 (문자열 의미 "(前回のガード時に)"와 일치)
```

이번 세션의 실제 주소는 객체 `0x7FF4A5979470` + `0xE10` = `0x7FF4A597A280`이다.
**이 절대주소는 ASLR/세션 한정이며 재실행 시 무효다.**

### 6.2 발견 경로 — 디스크 기반 차분 스캔

`begin-disk u8 private-readable` 이후 실제 플레이로 필터를 걸었다.

| 패스 | 게임에서 한 행동 | 필터 | 남은 후보 |
|---|---|---|---|
| — | 일반 방어 후 baseline | `begin-disk u8 private-readable` | 7,565,840,384 |
| 1 | 패링 성공 | `increased` | 18,197,537 |
| 2 | 일반 방어 | `decreased` | 11,623,684 |
| 3 | 대기(무입력) | `unchanged` | 6,571,326 |
| 4 | 패링 성공 | `exact 1` | 90,413 |
| 5–8 | 방어/패링 교대 | `exact 0` / `exact 1` | 1,748 |
| 9–12 | 방어/패링 교대 | `exact 0` / `exact 1` | **176** |

방향 필터(`increased`/`decreased`/`unchanged`)는 패스당 절반밖에 못 깎았다(방향은 잡음에서도 반반이므로). **`filter exact`로 절대값을 고정한 뒤에야 수렴했다**(657만 → 9만, 한 패스에 98.6%).

### 6.3 vptr 대조로 176개 → 1개

3.4절에서 정적으로 뽑아 둔 vftable RVA를 기준으로, 각 후보에서 앞쪽으로 거슬러 올라가 소유 객체의 vptr을 찾아 대조했다(읽기 전용, 32KB·128KB 두 창 모두 동일 결과).

- 176개 중 **173개**: 앞 32KB 안에 vftable 포인터가 전혀 없음 → C++ 객체가 아닌 순수 데이터 버퍼 = 잡음
- 3개만 객체 내부에 위치, 그중 **알려진 클래스와 일치하는 것은 정확히 1개**

정적 분석 단계에서 "`IsSucceededJustGuard` 라벨이 `SprjChrActionFlagModule` 번역단위에 있다"고 **추정**했던 것과, 라이브 차분 스캔이 **독립적으로 같은 클래스에 도달**했다.

### 6.4 실시간 검증 — 지정한 동작 순서와 정확히 일치

176개 후보 전부를 30Hz로 240초간 폴링하며 전환을 기록했다. 사용자는 사전에 지정한 순서로 플레이했다.

| 지정 동작 | 관측된 `+0xE10` 전환 |
|---|---|
| 패링 성공 #1 | `[8.471s] 0 → 1` ✅ |
| 패링 성공 #2 | 전환 없음 (1 유지) — **예상된 동작** |
| 일반 방어 #1 | `[15.448s] 1 → 0` ✅ |
| 일반 방어 #2 | 전환 없음 (0 유지) — **예상된 동작** |
| 패링 성공 #3 | `[24.382s] 0 → 1` ✅ |
| 일반 방어 #3 | `[36.416s] 1 → 0` ✅ |

**총 전환 4회, 순서·방향 모두 일치. 값은 0과 1만 취했다.** 240초 동안 객체의 vptr은 `SprjChrActionFlagModule`로 유지되어 객체 교체는 없었다.

### 6.5 **플레이어 소유 증명 (타입 수준)**

이 모듈 객체를 가리키는 포인터를 읽기 가능한 전 영역(8.96 GiB)에서 찾아 소유자를 역추적했다. 포인터는 2곳에서만 발견됐고, 그중 `0x7FF4A5979260`이 모듈 테이블이다.

| 슬롯 | 가리키는 객체 | 클래스 |
|---|---|---|
| `+0x00` | `0x7FF4A5979470` | **SprjChrActionFlagModule** ← 우리 플래그가 여기 `+0xE10` |
| `+0x28` | `0x7FF4A597A5D0` | SprjChrBehaviorModule |
| `+0x48` | `0x7FF4A597BC40` | **CSChrToughnessModule** (체간/Taikan) |
| `+0x80` | `0x7FF4A597CC30` | SprjChrActionRequestModule |
| `+0x90` | `0x7FF4A5986F60` | **SprjPlayerHitStopModule** |
| `+0x98` | `0x7FF4A5986FA0` | **SprjPlayerDamageModule** |

같은 모듈 테이블이 `SprjEnemyDamageModule`이 아니라 **`SprjPlayerDamageModule` / `SprjPlayerHitStopModule`** 을 들고 있다. 3.4절에서 "플레이어와 적은 서로 다른 구체 타입이므로 vptr 비교로 소유권을 증명할 수 있다"고 한 것이 **실제로 성립했다.** 이 캐릭터는 플레이어다.

부수 소득: **체간(`CSChrToughnessModule`)이 같은 테이블 `+0x48`에 있다.** 3.5절의 "체간은 PlayerGameData가 아니다"가 확인됐다.

### 6.6 반증된 경쟁 후보

| 주소 | 거동 | 판정 |
|---|---|---|
| `0x7FF4351E5C68` | 패링마다 1회 펄스(3/3), 방어엔 무반응 — **연속 패링 구분 가능성** | 앞 128KB에 vftable 없음 → 소유 객체 불명. **소유권 근거 없어 채택 불가** |
| `0x7FF491A4DB04` | 패링 #1에 1, 그러나 패링 #2 도중 0으로 복귀 | 방어(15.4s)를 구분 못 함 |
| `0x7FF49231F034` | 위와 동일 | 소유 클래스 미지(vftable RVA `0x3A50000`) |

### 6.7 남은 문제

1. **안정 루트로부터의 포인터 경로가 unknown.** `0x7FF4A5979260`은 이번 프로세스 한정이다. `WorldChrMan → PlayerIns → 모듈 테이블` 경로를 확정해야 재실행마다 재현된다. **detector 연결 전 필수.**
2. **연속 패링을 구분하지 못한다(FN).** 6.4에서 패링 #2가 전환을 만들지 못한 것이 실측으로 확인됐다. 문서가 경고한 실패 모드 그대로다. 발생 카운터나 이벤트 식별자는 아직 없다.
3. **독립 영상 정답 없음.** 6.4의 라벨은 사용자가 지정 순서대로 수행했다는 자기보고다. 순서·방향이 완전히 일치해 신뢰도는 높지만, `Sekiro_Deflect_Development.md`가 요구하는 독립 영상 + 시계 동기화 기준은 아니다.
4. **별도 세션 검증 미실시.** 6절은 전부 **발견 세션**이다. 새 플레이 세션에서 `evaluate`로 확인해야 한다.
5. 사망·로드·워프 시 객체 교체 거동 미검증.

**따라서 "실게임 관측으로 신호 확정"은 완료이나, "패링·방어 구분 검증(별도 세션)"은 미완이다.** 진동 출력 연결은 1·2번이 해결되기 전까지 하지 않는다.

### 6.8 포인터 경로 확정 및 객체 교체 검증 (2026-09-18)

6.7의 남은 문제 1번(안정 루트로부터의 포인터 경로)이 해결됐다.

#### 확정된 경로

```
[sekiro.exe + 0x3D7A1E0]   ->  WorldChrMan        (vftable RVA 0x2A30058)
              + 0x88       ->  PlayerIns          (vftable RVA 0x2A2B338)
              + 0x10B8     ->  모듈 컨테이너       (vptr 없는 배열/구조체)
              + 0x1D0      ->  SprjChrActionFlagModule  (vftable RVA 0x2A72158)
              + 0xE10      ->  패링 플래그 u8
```

WorldChrMan은 저장소가 이미 가지고 있던 AOB(`48 8B 35 ?? ?? ?? ?? 44 0F 28 18`,
displacement offset 3, instruction length 7 — [main.cpp의 MakeWorldChrManSpec()](../../../apps/sekiro_signal_probe/main.cpp))를
**라이브 프로세스의 .text**에 적용해 찾았다. 디스크 `.text`는 3.2절대로 암호화돼 있지만
메모리에서는 복호화돼 있어 스캔이 성립했다. 이미지 전체에서 **일치 site가 정확히 1개**(RVA `0x87F2F0`)라
모호함이 없다. 해당 RIP-relative 목적지가 static slot RVA `0x3D7A1E0`이다.

동일한 ActionFlagModule에 도달하는 경로가 5개 발견됐다
(`+0x88` 뒤 `+0x10B8/+0x1100/+0x1118/+0x1150/+0x1168`, 각각 `+0x1D0/+0x190/+0x150/+0x130/+0xF0`).
모두 같은 객체로 수렴하므로 위 하나를 대표로 채택했다.

#### 객체 교체 검증 — **통과**

절대주소를 캐싱하지 않고 **매 틱 체인 전체를 재해석하고 vptr을 재검증**하는 감시기로,
사용자가 불상(조각상)을 통해 다른 지역에 갔다가 되돌아오는 동안 관측했다.

```
[  0.000] OBJECT = 0x7FF4A5979470   (최초 — 황폐한 사원)
[121.248] RESOLVE FAIL: hop 0 (+0x88) null      <- 지역 이동 로딩 시작
[121.989] RESOLVE FAIL: WorldChrMan null
[122.668] RESOLVE FAIL: hop 0 (+0x88) null
[125.696] OBJECT = 0x7FF4A900B470   (교체됨)     <- 새 지역, 새 객체 자동 추적
[158.833] RESOLVE FAIL: hop 0 (+0x88) null      <- 되돌아오기 로딩
[158.925] RESOLVE FAIL: WorldChrMan null
[159.049] RESOLVE FAIL: hop 0 (+0x88) null
[160.192] OBJECT = 0x7FF4A7243CD0   (교체됨)     <- 또 새 객체 자동 추적
[182.558] FLAG 0 -> 1   패링 성공                <- 새 객체에서 정상 동작
[201.597] FLAG 1 -> 0   일반 방어                <- 정상
```

확인된 것:

1. **객체 교체를 2회 자동 추적했다.** `0x7FF4A5979470` → `0x7FF4A900B470` → `0x7FF4A7243CD0`.
   절대주소 방식이라면 첫 로딩에서 죽었을 것이다.
2. **로딩 중에는 fail-closed로 동작했다.** 옛 주소의 낡은 값을 읽어 이벤트를 만들지 않고,
   `WorldChrMan null` / `hop null`로 정직하게 실패했다. 문서가 요구한
   "객체 변경·부분 읽기·실패·gap 이후에는 이전 기준값으로 이벤트를 만들지 않는다"에 부합한다.
3. **교체 이후 완전히 다른 주소의 객체에서도 패링(0→1)과 일반 방어(1→0)를 그대로 구분했다.**

#### 이로써 해결된 것 / 남은 것

| 6.7의 항목 | 상태 |
|---|---|
| 1. 안정 루트로부터의 포인터 경로 | **해결** — 위 경로, static RVA 기준이라 재실행에도 재현 |
| 2. 연속 패링 구분(FN) | **미해결** — 여전히 bool 상태값이라 연속 성공 2번째부터 놓침 |
| 3. 독립 영상 정답 | **미해결** — 사용자 자기보고 |
| 4. 별도 세션 검증 | **미해결** — 전부 발견 세션 |
| 5. 사망/로드 시 객체 교체 | **해결** — 지역 이동 2회로 검증 (사망은 미검증) |

**진동 출력 연결은 여전히 하지 않는다.** 2·3·4번이 남아 있다.
특히 2번(연속 패링)은 이 신호의 구조적 한계이며, 해결하려면 발생 카운터나
이벤트 식별자를 추가로 찾아야 한다.

### 6.9 **정정: 6.8의 고정 포인터 체인은 구조적 불변이 아니다** (2026-09-19 확인)

6.8은 `WorldChrMan +0x88 -> +0x10B8 -> +0x1D0 -> ActionFlagModule` 을 "확정된 경로"로 기록했다.
같은 게임 프로세스에서 다시 확인한 결과 **그 경로는 더 이상 해석되지 않는다.**

```
PlayerIns + 0x10B8  -> 0x7ff4aa69bd80      (객체는 존재)
            + 0x1D0 -> 0x30474154306e0000  (포인터가 아님 -- ASCII 쓰레기값)
            vptr 검사 => 실패
```

그 시점에 ActionFlagModule은 대신 **`PlayerIns +0x50 -> +0xB50`** (및 `+0x1630->+0x1C0`,
`+0x1630->+0x810`, `+0x1B58->+0xD0`, `+0x1B60->+0x10`) 로 도달 가능했다. 즉 6.8의 깊은 오프셋들은
그 세션의 객체 그래프에서 **우연히 성립했던 경로**이며, 하드코딩하면 안 된다.

6.8의 객체 교체 추적 성공(지역 이동 2회)은 여전히 유효한 관측이지만, 그것이 경로의 구조적 안정성을
보장하지는 않았다. 더 오래 지나거나 상태가 더 바뀌면 깨진다.

#### 무엇이 실제로 안정적인가

| 앵커 | 검증 방법 | 판정 |
|---|---|---|
| `[sekiro.exe + 0x3D7A1E0]` -> WorldChrMan | 저장소 AOB, 이미지 전체에서 일치 1건 + vptr `NS_SPRJ::WorldChrManImp` | **안정** |
| `WorldChrMan + 0x88` -> PlayerIns | vptr `NS_SPRJ::PlayerIns` | **안정** |
| 그 아래 모든 깊은 오프셋 | — | **불안정, 사용 금지** |

#### 채택한 방식

깊은 오프셋을 버리고, **PlayerIns를 앵커로 삼아 vptr로 모듈을 탐색**한다.
찾은 포인터는 캐시하되 **매 틱 vptr을 재검증**하므로, 객체가 교체되거나 메모리가 재사용되면
낡은 값을 읽는 대신 탐색을 다시 수행한다. 이 방식은 레이아웃 변화에 영향을 받지 않는다.

이를 위해 `docs/astra/results/static/vftable_map.py` 로 sekiro.exe의 평문 RTTI에서
**vftable RVA -> 클래스명 전체 맵(9,581개)** 을 추출했다
(`vftable_map.json`). 이제 살아있는 임의의 객체를 타입으로 식별할 수 있다.

---

## 7. 연속 발생 인식 작업 (2026-09-19)

### 7.1 문제

`+0xE10`은 **마지막 방어 결과를 유지**한다. 따라서 패링→패링은 1이 유지되고 방어→방어는 0이
유지되어, 이 값의 변화만으로는 **같은 결과가 반복되는 타격을 구분할 수 없다.**
6.4에서 패링 #2와 방어 #2가 전환을 만들지 못한 것이 실측 증거다.

해결하려면 "**새 방어 판정이 한 번 발생했다**"를 말해주는 **독립적인 두 번째 신호**가 필요하다.
발생 카운터, 판정 식별자, 매 발생마다 갱신되는 값, 짧은 펄스 등이 후보다.

**현재 그런 필드는 아직 게임에서 확인되지 않았다.** 이번 작업은 그것을 찾기 위한 도구와,
찾았을 때 안전하게 이벤트로 바꾸는 규칙을 구현한 것이다.

### 7.2 구현한 것

| 파일 | 역할 |
|---|---|
| `include/sekiro_haptics/process/GuardOutcomeEventDetector.hpp` | 발생 신호 + 결과값 -> 이벤트. 아래 7.3의 규칙 구현 |
| `tests/test_guard_outcome_event_detector.cpp` | C++ 경계 테스트 18개 |
| `tools/deflect_occurrence_record.py` | 라이브 고속 레코더. PlayerIns 앵커 + vptr 탐색, 5ms, capture-v3 출력 |
| `tools/deflect_occurrence.py` | `find`(발생 신호 후보 순위) / `replay`(캡처 -> 이벤트 로그) |
| `tests/test_deflect_occurrence.py` | Python 규칙 테스트 18개 — C++와 **동일한 시나리오 표** |
| `docs/astra/results/static/vftable_map.py` / `.json` | 전체 RTTI vftable -> 클래스명 맵 (9,581개) |

Python `Detector`와 C++ `GuardOutcomeEventDetector`는 같은 시나리오 표로 각각 테스트되므로
재생 도구와 런타임 판별기가 서로 어긋날 수 없다.

### 7.3 오류를 막기 위해 강제한 규칙

지시문 3절의 각 항목이 테스트 한 개 이상으로 고정돼 있다.

| 규칙 | 근거 테스트 |
|---|---|
| 값이 1로 유지되는 동안 이벤트를 반복 생성하지 않음 | `HeldOutcomeWithoutOccurrenceChange_EmitsNothing` |
| 값이 0이라는 이유만으로 방어 이벤트를 만들지 않음 | `IdleAtZero_NeverEmitsABlock` |
| 결과는 **발생을 관측한 그 샘플**에서 읽음 | `LaggingOutcome_ClassifiesFromSettledValue` |
| 결과 갱신이 늦으면 확정을 보류, 추측하지 않음 | `NextOccurrenceBeforeSettle_ReportsUnresolvedNotAGuess` |
| 카운터가 2 이상 뛰면 **1개의 Unresolved**, N개로 복원하지 않음 | `CounterJump_ReportsOneUnresolvedNotTwoEvents` |
| 카운터 순환은 1 스텝으로 계산 | `CounterWrap_CountsAsOneStepNotAHugeJump` |
| 최초 연결 시 baseline만, 이벤트 없음 | `FirstObservation_IsBaselineOnly` |
| 객체 교체 전후 값을 비교하지 않음 | `ObjectReplacement_DoesNotDiffAcrossInstances` |
| 읽기 실패 후 재개 시 가짜 이벤트 없음 | `ReadFailure_ThenResume_EmitsNoFabricatedEvent` |
| 캡처 누락(gap) 후 재개 시 가짜 이벤트 없음 | `CaptureGap_BreaksContinuityAndResumesCleanly` |
| **시간 디바운스로 병합하지 않음** (20ms 간격 2타 = 2이벤트) | `RapidResolutions_AreNotMergedByProximity` |
| 결과값이 0/1 밖이면 패링/방어로 출력하지 않음 | `OutcomeOutsideZeroOne_IsUnresolvedNotADeflect` |

확정할 수 없는 구간은 `Unresolved` + 사유(`CounterJump` / `OutcomeNeverSettled` /
`ContinuityBreak` / `OutcomeOutOfRange`)로 남기며, **패링이나 방어로 출력하지 않는다.**

### 7.4 실제로 실행한 명령과 결과

```
cmake --build build-deflect                         -> 성공
ctest --test-dir build-deflect --output-on-failure  -> 4/4 통과
  sekiro_haptics_unit_tests   643/643   (기존 625 + 신규 18)
  capture_v3_decoder           11/11
  deflect_signal_analysis      22/22
  deflect_occurrence_rules     18/18    (신규)
```

레코더는 **실행 중인 게임(pid 10132)에 실제로 붙여** 검증했다.

```
python tools/deflect_occurrence_record.py --pid 10132 --out ... --seconds 12 --interval-ms 5
  -> samples=2398 dropped=0 gaps=0 objectSwaps=0 moduleSearches=1
python tools/deflect_capture.py <위 파일>
  -> 엄격 파서 통과 (capture_start/baseline/sample/capture_end)
```

즉 **출력이 기존 capture-v3 체계에서 그대로 재검토 가능**함을 실물로 확인했다.
유휴 12초 동안 ActionFlagModule 0x1800 창에서 **변한 4바이트 셀이 0개**였다 —
이 객체는 조용하므로 실제 타격 시 변화가 잘 드러난다.

**이 수치들은 전부 합성 또는 유휴 관측이다. 게임 인식률이 아니다.**

### 7.5 아직 확인하지 못한 것

1. **발생 신호 필드가 아직 없다.** 후보조차 확정되지 않았다. 7.2의 판별기는
   발생 신호를 **호출자가 넣어주는 입력**으로 받으며, 그 입력이 무엇인지는 모른다.
   따라서 **"연속 인식 완료"가 아니다.**
2. 6.6의 펄스 후보(`0x7FF4351E5C68`)는 과거 세션의 절대주소라 **재사용할 수 없다.**
   재탐색·소유 대상·수명을 다시 확인해야 한다. 주변에 vptr이 없다는 이유만으로 잡음으로
   단정하지는 않았다 — 힙 버퍼일 수도 있으므로 새 세션에서 다시 평가한다.
3. 패링에만 반응하는 신호를 찾더라도 **일반 방어의 반복 발생**은 별도로 풀어야 한다.
4. 독립 영상 정답, 별도 세션 검증은 여전히 미실시.

### 7.6 **발생 신호 확정 — `SprjChrActionFlagModule +0x3C` (1프레임 펄스)** (2026-09-18 관측)

7.1의 문제가 해결됐다. 라이브 캡처(300초, 59,798샘플, 5ms, pid 8312)에서 **방어 판정이
일어날 때마다 한 프레임 동안만 켜졌다 꺼지는 값**을 찾았다.

```
SprjChrActionFlagModule + 0x3C   u8/u32   0 -> 1 -> 0,  지속 약 15ms (1프레임)
SprjChrActionFlagModule + 0xE10  u8       결과: 0 = 일반 방어, 1 = 패링   (6절에서 확정)
SprjChrActionFlagModule + 0x64   u32      결과 보강: 0x00010001 패링 / 0xFFFF0001 방어
```

세 값 모두 **같은 모듈 안**에 있고, 6.5에서 플레이어 소유가 이미 타입 수준으로 증명된 객체다.

#### 구간별 펄스 수 — 음성 대조군 포함

| 구간 | 실제 행동 | 펄스 |
|---|---|---|
| p0 | 뛰어다니기 (패링 섞임) | 5 |
| p1 | **패링 연속** | **5** |
| p2 | 뛰어다니기 | **0** |
| p3 | **일반 방어 5회** | **5** |
| p4 | 뛰어다니기 | **0** |
| p5 | **패링2 + 방어3** | **5** |
| p6 | 뛰어다니기 | **0** |
| p7 | **허공에 가드만 유지** | **0** |
| p8 | 뛰어다니기 | **0** |
| p9 | **가드 없이 피격 2회** | **0** |
| p10 | 뛰어다니기 | **0** |

음성 대조군 두 개가 모두 통과했다. **가드 버튼을 눌러도 공격이 안 오면 0**,
**공격을 맞아도 가드가 아니면 0**이다. 즉 이 펄스는 가드 입력이나 피격이 아니라
**방어 판정 그 자체**에 반응한다.

#### 결정적 증거 — p5

사용자가 실제로 한 동작은 **패링, 패링, 방어, 방어, 방어**였다.

```
189.760s  펄스   +0xE10 = 1   +0x64 = 0x00010001   -> 패링
191.130s  펄스   +0xE10 = 1   +0x64 = 0x00010001   -> 패링   << 결과값은 1 그대로. 펄스가 구분
192.475s  펄스   +0xE10 = 0   +0x64 = 0xFFFF0001   -> 방어
194.460s  펄스   +0xE10 = 0   +0x64 = 0xFFFF0001   -> 방어   << 결과값은 0 그대로. 펄스가 구분
195.530s  펄스   +0xE10 = 0   +0x64 = 0xFFFF0001   -> 방어   << 같음
```

**결과값이 유지되는 동안에도 펄스는 매번 뛴다.** 6.4에서 패링 #2와 방어 #2를 놓쳤던
바로 그 실패 모드가 사라졌다.

#### 재생 결과 (`tools/deflect_occurrence.py replay --occurrence 0x3c --mode pulse`)

```
events: deflect=12 block=8 unresolved=0

 [145.925s] deflect   [148.210s] deflect   [150.495s] deflect   [152.780s] deflect   [155.059s] deflect
 [170.355s] block     [171.424s] block     [172.490s] block     [173.555s] block     [175.174s] block
 [189.760s] deflect   [191.130s] deflect   [192.475s] block     [194.460s] block     [195.530s] block
```

지시문 4절의 검증 시나리오 대조:

| 시나리오 | 결과 |
|---|---|
| 패링 연속 → 패링 이벤트 같은 수 | **통과** (5회 → 5개) |
| 일반 방어 연속 → 방어 이벤트 같은 수 | **통과** (5회 → 5개) |
| 혼합 → 종류와 순서 일치 | **통과** (패링2+방어3 → D,D,B,B,B) |
| 대기 / 가드 자세만 유지 / 헛방어 → 0개 | **통과** (p2,p4,p6,p7,p8,p10 전부 0) |
| 일반 피격을 패링으로 오인하지 않음 | **통과** (p9 무방어 피격 2회 → 0개) |
| 적이 내 공격을 튕겨냄 | **미검증** — 이번 세션에 해당 구간이 없었다 |
| 로딩·객체 교체 전후 | **이번 캡처에서는 미발생** (generation 1 유지) |

`unresolved=0` 이며, 결과 갱신 지연은 관측되지 않았다 — 펄스가 뜬 바로 그 샘플에서
`+0xE10`이 이미 새 값이었다(예: 192.475s에 전환과 펄스가 같은 샘플).

#### 구현상 반드시 지켜야 할 제약

**펄스 지속이 약 15ms(1프레임)이므로 폴링이 그보다 촘촘해야 한다.**
기존 C++ 캡처 세션의 기본 간격 200ms로는 **거의 모두 놓친다.** 최소 간격 5ms를 써야 한다.
이 제약은 detector를 연결할 때 반드시 반영해야 한다.

#### 아직 확인하지 못한 것

1. 정답이 **사용자 자기보고**다. 독립 영상 + 시계 동기화가 아니다.
2. **발견 세션이다.** 별도 세션 held-out 검증이 남아 있다.
3. p0(뛰어다니기 구간)에서 펄스가 5회 나왔다. 그 구간에 실제 방어가 있었던 것으로 보이지만
   기록된 정답이 없어 확정할 수 없다. **오검출이 아니라고 단정하지 않는다.**
4. 객체 교체·로딩 중 거동은 이번 캡처에 포함되지 않았다.
5. 적이 내 공격을 튕겨내는 경우가 미검증이다.

### 7.7 **별도 세션(held-out) 검증 — 통과** (2026-09-18, 2차 캡처)

7.6의 후보는 **그것을 찾아낸 바로 그 캡처**로 확인한 것이라 자기 자신을 검증한 셈이었다.
완전히 별개의 플레이 세션에서 새로 기록해 다시 평가했다.

```
capture  occ-02.jsonl  299.5s, 58,996 samples, 5ms, --siblings
records  delta 288,018 / baseline 39 / gap 36 / dropped 631 / discontinuity 3
```

후보 오프셋(`+0x3C` 펄스, `+0xE10` 결과)은 **1차 세션에서 고정한 그대로** 사용했고,
2차 자료를 보고 규칙을 고치지 않았다.

#### 결과 — 양성 대조군

| 지시한 동작 | 검출된 이벤트 |
|---|---|
| 패링 5회 연속 | `16.5 17.9 19.3 21.6 23.9s` → **패링 5개** |
| 일반 방어 5회 연속 | `35.4 36.5 37.6 39.1 40.1s` → **방어 5개** |
| 패링,패링,방어,방어,패링 | `48.6 D  49.9 D  51.3 B  53.3 B  54.4 D` → **종류·순서 완전 일치** |

`deflect=8  block=7  unresolved=0`. 총 15개, 지시한 15회와 정확히 같다.

#### 결과 — 음성 대조군 (전부 0개)

| 상황 | 이벤트 |
|---|---|
| 뛰어다니기 (여러 구간) | 0 |
| 허공에 가드만 유지 | 0 |
| 가드 없이 피격 2회 | 0 |
| **내가 공격 → 한베가 5회 다 맞음** | **0** |
| **한베가 내 공격을 막음 (적의 가드)** | **0** |
| **지역 이동 로딩 + 객체 교체 3회** | **0 (가짜 이벤트 없음)** |

**적의 가드가 0개라는 것이 특히 중요하다.** 이 신호가 "누군가의 방어"가 아니라
**플레이어 자신의 방어 판정**에만 반응한다는 직접 증거이며, 6.5의 타입 수준 소유 증명과 일치한다.

#### 객체 교체 중 가짜 이벤트가 나오지 않았다

```
142.91s  discontinuity  gen 1 -> 2      (지역 이동 로딩)
161.74s  discontinuity  gen 2 -> 3
163.07s  discontinuity  gen 3 -> 4
dropped 631건 (PlayerIns null 592, WorldChrMan null 39)
```

`+0xE10`이 142.91s에 1→0으로 바뀌었지만 **이벤트는 생성되지 않았다.**
새 객체의 초기값이었을 뿐 방어 판정이 아니었고, 판별기의 연속성 규칙
(`ObjectReplacement_DoesNotDiffAcrossInstances`)이 정확히 이것을 막았다.
로딩 중에는 `PlayerIns null`로 fail-closed 했고 낡은 값을 읽지 않았다.

#### 미검증으로 남은 것

**지역 이동 복귀 이후에 방어 동작을 수행하지 않았다.** 163초 이후 구간은 전부
플레이어가 *공격하는* 상황이었고, 그 동안 `+0x3C` 펄스도 `+0xE10` 전환도 0회였다.
읽기 자체는 gen 4로 정상 재개되었으나, **"객체 교체 후에도 패링/방어가 다시 잡히는가"는
직접 확인하지 못했다.** 이것만 남았다.

또한 정답은 여전히 **사용자 자기보고**이며 독립 영상·시계 동기화 자료가 아니다.
F5 마커도 일부 누락되어(15개) 후반 구간 경계는 자기보고에 의존했다.

### 7.8 **관측 코드 버그 2건 발견과 수정** (2026-09-19)

7.7의 "지역 이동 복귀 후 미검증" 항목을 확인하려고 3차 캡처(`occ-03`, 180초)를 떴더니
**이벤트가 0개**였다. 사용자는 패링 3회·방어 3회를 이동 전후로 수행했다.

#### 진단

```
occ-03:  35,295 샘플 전부  +0xE10 = 0,  +0x3C = 0
         그런데 형제 모듈 SprjPlayerDamageModule+0x70 은 3,452회 변함 (플레이어는 활동 중)
         discontinuity 2회 (47.67s, 73.05s), dropped 555건
```

같은 프로세스에서 **캐시 없이 매 틱 다시 찾는** 진단 코드를 돌리자 값이 정상적으로 움직였다.

```
[ 0.00s] player ActionFlagModule = 0x7ff4a7a3a330
[17.21s] +0xE10 0 -> 1
[22.25s] +0xE10 1 -> 0
```

즉 신호가 아니라 **관측 코드가 틀렸다.**

#### 버그 1 — vptr 검사만으로는 살아있는 객체를 보증하지 못한다

지역 이동으로 객체가 교체돼도 **옛 할당 영역에 vftable 포인터가 그대로 남아 있을 수 있다.**
캐시한 모듈 주소의 vptr만 검사하면 이 죽은 사본이 계속 타입 검사를 통과한다.
occ-03은 180초 내내 그 사본을 읽었다. 겉으로는 정상 동작처럼 보이면서
**조용히 아무것도 감지하지 않는** 상태였다.

**수정:** `PlayerIns`를 매 틱 다시 읽고, 그 주소가 바뀌면 모듈 캐시를 **통째로 폐기**한다.

#### 버그 2 — "그 vptr을 가진 첫 객체"는 근거가 없다

6.9에서 고정 오프셋을 버리고 vptr 탐색으로 바꿀 때, BFS가 처음 만난 객체를 채택했다.
탐색 순서에 의존하므로 어떤 객체가 선택될지 보장이 없다.

**수정:** `SprjPlayerDamageModule`(플레이어 전용 구체 타입)을 **함께 보유한 컨테이너**에서만
모듈 집합을 가져온다. 6.5의 타입 수준 소유 증명과 같은 근거다.

#### 수정 후 검증 (`occ-04`, 75초)

지시: 패링 3회 → 방어 3회.

```
events: deflect=3  block=3  unresolved=0
  [32.544s] deflect   [33.928s] deflect   [35.314s] deflect
  [37.579s] block     [38.643s] block     [39.714s] block
```

**종류·개수·순서 모두 일치.** `moduleSearches=1`, `dropped=0`, `objectSwaps=0`.

#### 이 버그가 중요한 이유

진동 출력을 먼저 연결했다면 **지역 이동 후 조용히 먹통이 되면서도 정상으로 보였을** 종류의
결함이다. 7.7에서 "복귀 후 방어 동작 미검증"을 미확인으로 남겨두고 확인하러 간 덕분에 잡혔다.

#### 아직 남은 것

`occ-04`에는 **객체 교체가 없었다**(`objectSwaps=0`). 수정본이 실제 지역 이동을 건너서도
계속 잡는지는 아직 직접 확인하지 못했다. 이것이 마지막 한 칸이다.

### 7.9 **객체 교체 관통 검증 — 통과** (2026-09-19, `occ-05`)

7.8에서 수정한 관측 코드로, 버그가 실제로 터졌던 바로 그 시나리오를 다시 돌렸다.

지시: **패링 2회 → 방어 2회 → 불상으로 지역 이동 후 복귀 → 패링 2회 → 방어 2회**

```
capture  occ-05.jsonl  91.8s, 17,725 samples, 5ms, --siblings
sha256   13efcf6ea9e7369f1c69def6cd588a3c2776ccab2e7f5f0b206ec306b5994c57
records  delta 33,899 / baseline 8 / gap 5 / dropped 583 / discontinuity 2
```

```
events: deflect=4  block=4  unresolved=0

  [ 5.052s] deflect     [ 6.417s] deflect     [ 7.782s] block     [ 9.782s] block
                          -- 지역 이동 --
  [78.082s] deflect     [79.467s] deflect     [80.852s] block     [82.952s] block
```

| 확인 항목 | 결과 |
|---|---|
| 이동 **전** 패링2+방어2 | **D, D, B, B — 일치** |
| 객체 교체 발생 | `34.27s gen 1→2`, `58.00s gen 2→3` — 2회 |
| 로딩 중 거동 | `dropped 583` (PlayerIns null 544, WorldChrMan null 39) — fail-closed |
| 로딩 중 **가짜 이벤트** | **0개** |
| 이동 **후** 패링2+방어2 | **D, D, B, B — 일치** ← occ-03에서 실패했던 지점 |
| `unresolved` | 0 |

총 8개, 지시한 8회와 정확히 같다. **수정본은 객체 교체를 건너서도 계속 감지한다.**

---

## 7.10 이번 목표 최종 상태

**연속 패링·연속 일반 방어를 각각 독립된 사건으로 인식한다 — 달성.**

| 검증 시나리오 | 결과 | 근거 |
|---|---|---|
| 패링 N회 연속 → 패링 이벤트 N개 | **통과** | occ-01(5), occ-02(5), occ-04(3), occ-05(2+2) |
| 일반 방어 N회 연속 → 방어 이벤트 N개 | **통과** | occ-01(5), occ-02(5), occ-04(3), occ-05(2+2) |
| 혼합 → 종류·순서 일치 | **통과** | occ-02 `D,D,B,B,D` |
| 대기 / 가드 자세만 유지 / 헛방어 → 0개 | **통과** | occ-01, occ-02 |
| 일반 피격을 패링으로 오인하지 않음 | **통과** | occ-02 (무방어 피격 2회 → 0) |
| **적이 내 공격을 막음 → 오인하지 않음** | **통과** | occ-02 (0개) |
| 로딩·객체 교체 전후 가짜 이벤트 없음, 재개 후 재인식 | **통과** | occ-05 |
| 자동 테스트 (반복·중복·교체·누락·갱신 시차) | **통과** | C++ 18개 + Python 18개, ctest 4/4 |

### 확정된 신호

```
루트     [sekiro.exe + 0x3D7A1E0]  -> WorldChrMan   (AOB, vptr NS_SPRJ::WorldChrManImp)
         WorldChrMan + 0x88        -> PlayerIns     (vptr NS_SPRJ::PlayerIns)
모듈     PlayerIns 로부터 vptr 탐색.  SprjPlayerDamageModule 을 함께 가진 컨테이너에서만 채택
발생     SprjChrActionFlagModule + 0x3C    1프레임(~15ms) 펄스 0->1->0, 방어 판정마다 1회
결과     SprjChrActionFlagModule + 0xE10   0 = 일반 방어, 1 = 패링
보강     SprjChrActionFlagModule + 0x64    0x00010001 = 패링, 0xFFFF0001 = 방어
```

**폴링은 5ms급이어야 한다.** 펄스가 1프레임이라 기존 C++ 캡처의 기본 200ms로는 거의 다 놓친다.

### 여전히 확인되지 않은 것

1. **정답이 전부 사용자 자기보고다.** 독립 영상 + 시계 동기화 자료가 아니다.
   순서·개수가 매번 정확히 일치해 신뢰도는 높지만, `Sekiro_Deflect_Development.md`가
   요구하는 기준은 아니다. 이는 방법론 등급의 문제이지 기능 미검증이 아니다.
2. 사망으로 인한 객체 교체는 미검증(지역 이동만 검증).
3. 보스전·다수 적 등 다른 전투 상황에서의 오검출률은 측정하지 않았다.
4. **진동 출력은 연결하지 않았다.** 이번 범위 밖이다.

---

## 8. 이번에 하지 않은 것 (명시)

- 라이브 캡처 0건. `tools/deflect_signal.py discover`/`evaluate`를 **실제 게임 자료로 실행한 적 없음.** 합성 fixture 22/22는 도구 검증이지 인식률이 아니다.
- 독립 영상·시계 동기화 자료 없음. 따라서 TP/FP/FN·지연 수치를 만들지 않았다.
- 메모리 쓰기, DLL 주입, 코드 패치, 입력 자동화, 게임/세이브 변경, EXE 언패킹 — 전부 수행하지 않음.
- 게임 EXE·전체 덤프를 이 대화나 외부로 전송하지 않음. 분석은 전부 로컬에서 수행했고, 결과물에는 작은 발췌와 주소만 남겼다.

---

## 8. 적 측 가드 신호 — 라이브 확인 (2026-09-19)

### 8.1 가설

`SprjChrActionFlagModule`은 정적 vftable 맵 9,581개 중 **단 하나**(RVA 0x2A72158)다.
플레이어용/적용이 따로 없으므로 `+0x3C`(펄스)·`+0xE10`(0=막기, 1=튕김)이 적 캐릭터에도
같은 의미일 것이라고 예상했다.

적 측 주소 해석에 쓴 타입 (전부 정적 맵에서 확인):

| 클래스 | RVA |
|---|---|
| `NS_SPRJ::EnemyIns` | 0x2A27F28 |
| `SprjEnemyDamageModule` | 0x2A7D628 |
| `NS_SPRJ::SprjChrActionFlagModule` | 0x2A72158 |

소유권 판정은 플레이어 쪽과 **대칭**이다: EnemyIns에서 도달 가능하고
ActionFlagModule과 SprjEnemyDamageModule을 **함께** 들고 있는 컨테이너에서만 모듈을 받는다.

### 8.2 실측 (pid 15208, 60초, 폴링 5 ms)

`tools/enemy_guard_probe.py` (읽기 전용). 사용자가 적을 칼로 공격.

```
탐색 완료: 13.8초, 144개 추적
  [33795ms] enemy 0x7ff4a1f3b140 BLOCK     pulse=4 outcome=0
  [34431ms] enemy 0x7ff4a1f3b140 DEFLECT   pulse=4 outcome=1
  [36175ms] enemy 0x7ff4a1f3b140 BLOCK     pulse=4 outcome=0
  [38696ms] enemy 0x7ff4a1f3b140 BLOCK     pulse=4 outcome=0
  [39308ms] enemy 0x7ff4a1f3b140 DEFLECT   pulse=4 outcome=1
  ...
--- 19 candidate enemy guard event(s) ---
    polls=9998  worst poll gap=12.5 ms   (한 프레임 16.7 ms 미만)
```

확인된 것:
- **펄스와 결과 바이트가 적 객체에서도 동작한다.** `pulse=4`로 뜨고 `outcome`이
  0/1로 갈린다 — 플레이어와 같은 값 체계.
- **BLOCK → DEFLECT가 610~650 ms 간격으로 반복**된다. 사용자가 보고한
  "적이 맞고 방어 패링 방어 패링"과 순서가 일치한다.
- **144개를 추적했는데 이벤트가 난 객체는 하나뿐**(0x7ff4a1f3b140)이다.
  나머지 143개는 60초간 오탐 0건 — 유휴/미로드 객체가 노이즈를 만들지 않는다.

### 8.3 아직 확인되지 않은 것

- `+0xE10`은 **"누구의 공격을 막았는지" 말하지 않는다.** 적끼리 싸울 때,
  수리검·폭죽 같은 비칼 공격, 적이 여러 명일 때의 구분은 미검증이다.
- 내가 공격하지 않는 동안의 오탐률(네거티브 컨트롤) 미측정.
- 보스·특수 적이 `EnemyIns`가 아닌 타입일 가능성 미확인.

### 8.4 성능 메모

- 초기 탐색 13.8초. `_learn_path`가 동작하지 않아 144개 전부 느린 경로로 갔다
  (`느린 탐색 152회`). 개선 여지가 있지만 1회성 비용이라 막지는 않는다.
- 폴링 중 추적 대상 전체의 결과 바이트까지 읽으면 폴당 288회 ReadProcessMemory가
  되어 최악 간격이 20.4 ms(한 프레임 초과)까지 올라갔다. **펄스가 0이 아닐 때만**
  결과 바이트를 읽도록 바꿔 12.5 ms로 내렸다.

### 8.5 적 모듈 소유권 오류 — 원인과 수정 (2026-09-19)

증상: "주변 적이 싸우는 건 인식되는데 내 공격을 방어하는 건 인식 못 한다."

원인은 모듈 해석이었다. 플레이어 리더와 같은 방식(캐릭터에서 2홉 이내에 도달 가능한
ActionFlagModule)을 적에 그대로 쓴 것이 잘못이었다. 플레이어는 하나뿐이라 안전하지만,
**적은 2홉이면 이웃 캐릭터로 넘어간다.** 실측:

```
캐릭터 104개 -> 서로 다른 모듈 75개
!! 모듈 공유: 24개 모듈이 여러 캐릭터에 물려 있음
   0x7ff4a418d3c0 <- 캐릭터 4개
```

공유된 모듈은 없는 것보다 나쁘다. 내가 때리는 적이 **다른 캐릭터의 객체를 읽게 되어**
내 공격은 놓치고, 그 다른 캐릭터의 전투가 두 개의 정체로 보고된다. 증상과 정확히 일치한다.

시도하고 기각한 것:
- **모듈 → 캐릭터 역포인터**: 모듈 앞 0x400 바이트에 일관된 오프셋 없음(40개 중 각 1개씩).
- **깊이 1로 제한**: 공유가 24 → 6으로 줄 뿐 사라지지 않음.
- **플레이어 경로 그대로 적용**(`ChrIns+0x10B8 -> container+0x1D0`): 적 151개 중 **0개** 해석.
  PlayerIns는 파생 클래스라 레이아웃이 다르다.

**해결 — 오프셋 쌍을 런타임에 측정한다.** 표본 캐릭터들에서 실제
ActionFlagModule에 도달하는 `(ChrIns 오프셋, container 오프셋)` 쌍을 모두 세고,
가장 많은 캐릭터가 지지하는 쌍을 쓴다. 우연한 포인터는 한 캐릭터에서만 맞고,
진짜 필드 레이아웃은 대부분에서 맞는다.

```
most common (ChrIns offset, container offset):
  ChrIns+0x1B58 -> container+0xD0    23/50 characters
  ChrIns+0x1168 -> container+0xF0    12/50
  ChrIns+0x10B8 -> container+0x1D0    6/50   <- 플레이어 모양
path ChrIns+0x1B58/+0xD0: 23 resolved, 23 distinct, 0 shared
```

여기에 **유일성 제약**을 더했다: 두 캐릭터가 같은 모듈을 주장하면 어느 쪽이 맞는지
알 수 없으므로 **둘 다 버린다**.

수정 후:
```
탐색 완료: 4.0초, 81개 추적
구조 경로: ChrIns + 0x1B58 -> container + 0xD0  (표본 중 17개가 지지)
캐릭터 81개 -> 서로 다른 모듈 81개   중복 없음
```

탐색 시간도 17.3초 → 4.0초로 줄었다(캐릭터마다 그래프를 걷지 않고 두 번만 읽으므로).

**아직 미검증**: 이 경로가 "내가 때리는 적"의 모듈을 실제로 맞게 잡는지는 구조적
유일성만 확인됐을 뿐, 행동으로는 확인되지 않았다. 공격해 보고 화면과 대조해야 한다.
