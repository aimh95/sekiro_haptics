# 세키로 동작별 듀얼센스 피드백 구현

이 변경은 **동작 명령 → 진동·트리거 저항·컨트롤러 스피커**를 실행하는 코드다.
`sekiro_action_feedback`는 수동 명령과 작성된 시나리오를 실행한다.
**세키로에서 패링 성공·선택 의수·와이어 단계를 자동으로 읽는 탐지기는 아직 연결되지 않았다.**
메모리 주소나 애니메이션 ID를 추측해서 성공 이벤트로 출력하는 코드는 넣지 않았다.

**현재 개발 우선순위는 실제 패링/방어 신호 확인이다.**
후보 발견·독립 검증·라이브 준비 절차는 [10-deflect-signal-first.md](10-deflect-signal-first.md)를 따른다.
아래 출력 실행 절차는 감지 구현 완료를 뜻하지 않는다.

## 지원 기준

- Windows 10/11 x64, 일반 DualSense, USB 연결. 이번 HID 경로는 VID 054C / PID 0CE6의 USB gamepad 인터페이스만 연다.
- 사용자가 말한 “듀얼쇼크”가 DualShock 4라면 장력을 조절하는 하드웨어가 없다. 이 구현의 대상은 PS5 DualSense다.
- 스피커는 Windows가 보여 주는 **해당 컨트롤러의 재생 장치**를 명시적으로 선택해야 한다. PC 기본 스피커를 자동으로 선택하지 않는다.
- DualSense Edge, Bluetooth, DualShock 4 출력, 컨트롤러 에뮬레이션은 이번 구현 범위 밖이다.
- 진동은 기존 HID rumble 방식이다. 오디오의 별도 액추에이터 채널을 사용하는 고해상도 햅틱은 후속 작업이다.

Sony의 [PC 연결 및 기능 지원 안내](https://www.playstation.com/en-us/support/hardware/pair-dualsense-controller-bluetooth/)에서 PC의 유선 햅틱과 내장 스피커 지원 조건을 확인할 수 있다.

## 실행

Visual Studio C++ 빌드 도구와 Windows SDK, CMake 3.20 이상이 필요하다. HIDAPI 0.15.0은 기존 CMake FetchContent 설정으로 가져온다. Python 3.10 이상이 있으면 캡처 분석기 테스트도 등록된다.

프로젝트 루트에서:

```powershell
.\tools\build_feedback_windows.ps1
$app = ".\build-feedback\apps\action_feedback\Release\sekiro_action_feedback.exe"
& $app --scenario .\configs\action_feedback_demo.jsonl --dry-run --fast
& $app --list-devices
```

장치 목록을 보고 번호를 선택한다. 아래 `0`, `3`은 예시 번호다. 실제 목록에서 **USB DualSense와 그 컨트롤러의 오디오 재생 장치**를 골라 바꾼다.

```powershell
& $app --scenario .\configs\action_feedback_demo.jsonl --hardware --device 0
& $app --scenario .\configs\action_feedback_demo.jsonl --hardware --device 0 --audio-device 3
& $app --manual --hardware --device 0 --audio-device 3 --rumble-gain 0.65 --trigger-gain 0.55 --speaker-gain 0.20
```

`--manual`은 프로그램 콘솔에 포커스를 두고 키를 누르는 출력 시험이다. 게임 동작을 인식하거나 버튼을 대신 누르지 않는다.

| 키 | 출력 명령 |
|---|---|
| `p` / `b` / `d` | 패링 / 일반 방어 / 피격 |
| `n` / `u` | 다음 의수 선택 / 선택된 의수 사용 |
| `j` → `k` → `l` → `e` | 와이어 발사 → 걸림 → 당김 → 종료 |
| Space / `r` | 일시정지 및 모든 효과 해제 / 재개 |
| `q` 또는 Ctrl+C | 종료 및 해제 |

스피커 음소거는 이 프로그램이 강제로 해제하지 않는다. 소리가 없으면 선택한 장치, Windows 재생 장치의 음소거, 컨트롤러 상태를 확인한다. 다른 프로그램도 같은 패드에 출력하면 효과가 덮어써질 수 있으므로 하드웨어 시험 때는 이 실행기 하나가 진동·트리거 출력을 담당하게 한다.

## 구현한 감각

| 명령 | 진동·장력·소리 |
|---|---|
| `deflect` | 100ms 이내의 날카로운 충격, 합성 금속 충돌음. 90ms 간격의 연속 성공도 각각 출력 |
| `block` | 패링보다 약하고 둔한 150ms 충격 |
| `damage`, `posture_break`, `deathblow` | 각각 다른 강도·길이의 충격 |
| `select` | 선택된 의수에 따라 R2 기본 저항 변경 |
| `use` | 의수별 반동과 소리 후 기본 장력으로 복귀 |
| `grapple_launch` | 낮은 저항의 시작 지점이 이동하여 줄이 풀리는 느낌 |
| `grapple_latch` | 짧은 충격과 강해지는 L2 저항 |
| `grapple_pull` | 8Hz로 변하는 L2 저항과 양쪽 진동 |
| `grapple_end` | L2 저항 해제 |

| 의수 이름 | `tool` 값 | 설정한 특성 |
|---|---|---|
| 수리검 | `shuriken` | 가벼운 저항, 짧은 반동 |
| 도끼 | `axe` | 무거운 저항, 큰 충격 |
| 창 | `spear` | 중간 저항, 찌르는 반동 |
| 화통 | `flame_vent` | 약한 기본 저항, 긴 분사 진동 |
| 폭죽 | `firecracker` | 가벼운 기본 저항, 짧고 강한 반동 |
| 우산 | `umbrella` | 일찍 걸리는 저항, 펼칠 때 강해지는 장력 |
| 사비마루 | `sabimaru` | 가볍고 빠른 반동 |
| 안개 까마귀 | `mist_raven` | 부드러운 저항과 짧은 효과 |
| 손가락 피리 | `finger_whistle` | 매우 약한 저항과 짧은 효과 |
| 행방불명 | `divine_abduction` | 중간 저항과 퍼지는 효과 |

강도·시간은 이번에 작성한 감각 설정이다. 실제 패드에서 조정해야 하며, 게임 물리량이나 검증된 감각 측정값이 아니다. 현재 음원은 직접 합성한 금속음·충격음·노이즈 효과다. 세키로 원본 음원을 포함하지 않는다.

## 구성과 연동 지점

`ActionCommand`를 `SekiroFeedbackEngine::Submit()`에 전달하고, 같은 출력 스레드에서 10ms마다 `Tick()`을 호출한다. 반환된 `FeedbackFrame`을 `FeedbackOutput::Send()`에 전달한다. `PumpAudio()`는 5ms마다 호출한다.

```cpp
// 아래 명령은 앞단에서 실제 성공을 검증한 뒤에만 제출한다.
engine.Submit({SekiroAction::Deflect, Prosthetic::None, nextSequence++}, nowUs);
auto frame = engine.Tick(nowUs);
if (!output.Send(frame) || !output.PumpAudio()) {
    output.Close();
    // 연결이 복구되어도 이전 명령을 재생하지 말고 새 세션으로 시작한다.
}
```

관측 상태가 정상인 동안에만 `Heartbeat`를 공급한다. 데모 실행기의 합성 heartbeat를 향후 실제 게임 어댑터에 복사하면 안 된다. 읽기 실패·플레이어 변경·로딩 때는 `Discontinuity`, 재검증이 끝나면 `Resume`을 보내고 의수를 다시 선택한다. 기본 500ms 관측 시간 초과는 모든 효과를 지운다. 와이어 단계는 별도로 최대 시간을 갖는다. 패링 중복 제거는 증가하는 발생 번호로 처리하며 고정 120ms 시간 제한으로 연속 패링을 버리지 않는다.

새 출력 경로는 기존 `HapticScheduler`와 함께 같은 패드를 제어하지 않는다. 한 스레드가 전체 출력 상태를 계산하므로 이전 효과의 예약된 `Reset`이 새 효과를 지우지 않는다. 기존 replay/mock 경로는 보존했다. 기존 스케줄러 자체의 경합 문제까지 이번에 전부 고친 것은 아니다.

스피커 활성화는 USB 오디오의 오른쪽 전방 채널을 내장 스피커로 보내는 HID 경로와 WASAPI PCM 재생을 함께 사용한다. 종료할 때 진동·트리거를 끄고 스피커 볼륨 0 및 스테레오 헤드셋 경로로 돌린다. 실행 이전의 임의 오디오 설정을 읽어 복원하는 기능은 없다. 프로세스 강제 종료나 USB 단절 때는 호스트에서 마지막 해제 패킷이 전달됐다고 보장할 수 없다.

## 캡처 수정

- 기존 캡처 파일은 OS의 exclusive-create로 보호한다. 재시작은 번호가 붙은 새 파일을 만든다.
- root와 child 포인터를 샘플 전후에 다시 읽고 주소·세대를 확인한다. 객체 변경이나 읽기 실패 뒤에는 새 기준값을 잡는다.
- v3는 전체 초기값, 변화 셀, **값이 같아도 모든 성공 샘플의 commit**, 실패·시간 공백·끝 레코드를 기록한다.
- v3 분석기는 원시 바이트를 복원하고, 잘못된 hex·이전 값 불일치·중복 셀·누락된 commit·잘린 파일을 거부한다.
- 이전 v1/v2 상관 분석기는 겹치는 시도 구간과 불연속 구간을 집계에서 제외한다. v3는 해당 분석기에 넣지 않는다.
- Windows 프로세스 핸들 권한을 `VirtualQueryEx`에 필요한 query-information + read로 수정했다.
- 캡처 스레드와 입력 스레드의 종료를 기다린 뒤 파일을 닫고 프로세스에서 분리하도록 바꿨다.

```powershell
python .\tools\deflect_capture.py <capture.jsonl>
python .\tools\deflect_capture.py <capture.jsonl> --offset 0x18 --type u32
```

이 명령은 원시 값 확인이다. `0x18`이 실제 HP라는 보증도, 출력된 값이 패링이라는 보증도 아니다. 수동 `perfect_deflect` 마커는 사용자의 주석으로 보존한다. 성공 탐지로 승격하지 않는다. 원시 바이트를 유지하므로 부동소수점 표시나 정수 변환 때문에 상위 비트가 사라지지 않는다. 재구성 메모리는 128MiB로 제한한다.

포인터 전후 비교는 프로세스를 정지시키는 원자적 스냅샷이 아니다. 읽는 동안 주소가 바뀌었다가 동일 주소로 돌아오는 경우나 프로세스 내부 필드가 찢어지는 상황을 완전히 배제할 수 없다. 라이브 실험에서 버전별 객체 식별자·프레임 카운터 등으로 추가 검증해야 한다.

## 다음 실제 세키로 연결 작업

1. **먼저 실제 패링/일반 방어 판정 경로를 조사한다.** EXE 지문, 플레이어 반응 후보, 반복 발생 구분 근거를 확보한다. 구체적인 절차는 `10-deflect-signal-first.md`를 따른다.
2. **Windows에서 probe를 확인하고 후보를 라이브 기록한다.** 파일 보존·관측 수명 수정본과 독립 영상으로 패링/방어/적 패링/허공 방어/피격을 대조한다. 새 분석기는 후보를 고를 때 쓰지 않은 세션에서 다시 평가한다.
3. **검증된 신호를 `ActionCommand`에 연결하고 패드 출력을 확인한다.** 의수와 와이어 감지는 패링/방어 이후에 확장한다. 이번 환경에서 Windows 바이너리·실제 패드·실게임 탐지는 검증하지 못했다.

다음 라이브 캡처 전에 필수인 것은 **파일 보존, 객체 변경 시 재기준화, 읽기 실패/공백 기록, 중복 집계 방지, Windows 종료·권한 경로 확인**이다. 새 프레임워크, 스캐너 전면 교체, 설정 형식 통일, 기존 스케줄러 전체 재작성, Bluetooth, 고해상도 오디오 햅틱은 기다려도 된다.

## 프로토콜 및 API 근거

- USB 모터·트리거 위치: [pydualsense 출력 보고서](https://github.com/flok/pydualsense/blob/master/pydualsense/pydualsense.py).
- 연속 저항 모드: [DualSense-Windows 출력 코드](https://github.com/Ohjurot/DualSense-Windows/blob/main/VS19_Solution/DualSenseWindows/src/DualSenseWindows/DS5_Output.cpp).
- 스피커 볼륨·경로 제어 비트: [duaLib 자료 구조](https://github.com/WujekFoliarz/duaLib/blob/master/src/include/dataStructures.h), [오디오 경로 상수](https://github.com/WujekFoliarz/duaLib/blob/master/src/include/duaLib.h).
- Windows 오디오 렌더링: [Microsoft WASAPI 렌더링 안내](https://learn.microsoft.com/en-us/windows/win32/coreaudio/rendering-a-stream).
- 메모리 영역 조회 권한: [Microsoft VirtualQueryEx](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex).

공개 구현에서 보고서 형식의 근거를 확인해 기존 프로젝트에 맞는 코드를 작성했다. 외부 라이브러리의 전체 구현이나 게임 자산을 복사하지 않았다. 이 출처들은 세키로 동작 탐지에 대한 라이브 증거가 아니다.
