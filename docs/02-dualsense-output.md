# DualSense 진동 출력 — 동작 원리

*문서 2/5 — 이전: [01-architecture.md](01-architecture.md), 다음: [03-trace-format.md](03-trace-format.md).*

이 문서는 SekiroHaptics의 DualSense 진동 출력이 **왜 그렇게 동작하는지**를 먼저 설명하고,
그 설명의 근거로만 파일명·함수명·라인 번호를 붙인다. 코드 나열이 아니라 "지금 이 시스템이
실제로 무엇을 하고 있는가"를 이해하는 것이 목적이다.

---

## 1. 시스템 목적

SekiroHaptics는 게임에서 벌어진 일(예: Perfect Deflect)을 컨트롤러 진동으로 옮기는 프로젝트다.
이를 위해서는 개념적으로 두 가지가 모두 필요하다.

1. **"무엇을, 언제" 진동시킬지 표현하는 계층** — 게임 이벤트를 `HapticEffect`라는 데이터로
   추상화하고, 그 재생 시점을 관리한다. 백엔드가 무엇이든(Mock이든 진짜 컨트롤러든) 이 계층은
   달라지지 않아야 한다.
2. **"실제로 어떻게" DualSense를 진동시킬지 아는 계층** — USB HID로 정확히 몇 바이트짜리
   리포트를, 어떤 값으로 채워서 보내야 컨트롤러 모터가 도는지에 대한 지식이다.

지금 이 저장소는 이 두 계층을 **각각 따로** 갖고 있지만, 아직 서로 연결되어 있지 않다. 1번
계층은 `MockHapticBackend`(로그만 남김)까지만 이어져 있고, 2번 계층은 사람이 직접 실행하는
CLI 테스트 앱까지만 이어져 있다. 이 문서 전체는 이 "두 계층이 왜 나뉘어 있고, 어디서 끊겨
있는가"를 축으로 설명한다.

---

## 2. 목표 아키텍처

원래 의도된 흐름은 다음과 같다.

```
Game Event → HapticEffect → HapticScheduler → DualSenseBackend → (HID Write) → DualSense Controller
```

- **Game Event**: Sekiro에서 일어난 일(파훼, 피격 등). 이 프로젝트 범위 밖(후킹 미구현).
- **HapticEffect**: "무엇이 일어났는지"를 재생 가능한 데이터로 바꾼 것 (`HapticEffectType`,
  좌/우 모터 세기, 지속 시간).
- **HapticScheduler**: 그 이펙트를 "언제" 재생할지(지연, 취소, 리셋) 관리하는 타이밍 계층.
  스케줄러는 백엔드가 Mock인지 실제 하드웨어인지 몰라야 한다 — `IHapticBackend&`만 알 뿐이다.
- **DualSenseBackend**: `IHapticBackend`를 구현해, 스케줄러가 넘긴 `HapticEffect`를 실제
  DualSense USB 출력 리포트로 바꾸고 장치에 써야 하는 컴포넌트. **아직 존재하지 않는다.**
- 이렇게 계층을 나누는 이유는 단순하다: 게임 이벤트/타이밍 로직을 실제 컨트롤러 없이도
  `MockHapticBackend`로 테스트할 수 있게 하고, 반대로 "USB로 진동이 실제로 나오는가"라는
  하드웨어 문제를 게임 로직 없이 독립적으로 검증할 수 있게 하기 위해서다. 지금 존재하는
  `MockHapticBackend`(§5)와 `dualsense_rumble_test` 앱(§3)이 각각 그 두 가지 독립 검증을
  수행하고 있는 것이지, 아직 이 둘을 잇는 `DualSenseBackend`가 있어서가 아니다.

이 목표 아키텍처는 §8에서 다시 다루며, 지금 없는 부분(`DualSenseBackend`)을 명시한다.

---

## 3. 현재 실제 실행 흐름

지금 이 저장소에서 **실제로 물리적인 진동까지 도달하는** 유일한 경로는 사람이 직접 실행하는
`dualsense_rumble_test` 콘솔 앱 하나뿐이다. 이 경로는 §2의 목표 아키텍처를 전혀 거치지 않는다.

```
CLI motorStrength → BuildRumbleReport → HID Write → (DualSense Controller)
```

동작 원리를 순서대로 설명하면:

1. 사람이 프로그램을 실행하면서 진동 세기를 숫자(0-255)로 넘긴다. 값을 안 주면 기본값 180을
   쓴다. 즉 **이 숫자는 게임에서 온 것이 아니라 사람이 커맨드라인에서 직접 준 값**이다
   (`apps/dualsense_rumble_test/main.cpp:25-29`).
2. HID 장치를 검색해서 첫 번째로 찾은 DualSense를 연다. 검색은 "USB로 연결된 장치 중
   Sony/DualSense의 제조사·제품 ID와 일치하는 것"만 걸러내는 방식으로 이루어진다
   (`main.cpp:36-42`, `src/HidApiDualSenseTransport.cpp:69-89`).
3. 커맨드라인 세기 값으로 64바이트 진동 리포트를 하나 만든다(§6에서 그 구조를 설명).
4. 그 리포트를 장치가 잊어버리지 않도록 50ms마다 같은 내용을 반복해서 보낸다. 이렇게 하는
   이유는 §4에서 설명한다.
5. 정해진 시간(1.5초)이 지나면 세기 0짜리 리포트를 한 번 보내 진동을 멈추고 장치를 닫는다.

이 흐름 어디에도 `HapticEffect`, `HapticEffectType`, `HapticScheduler`가 등장하지 않는다 —
숫자 하나가 곧바로 바이트로 직렬화되어 나갈 뿐이다. 이것이 "목표 아키텍처"와 "현재 흐름"의
가장 근본적인 차이다.

---

## 4. 진동 시작·변경·정지 상태 흐름

DualSense 진동을 상태 기계로 보면 다음과 같다.

```
[장치 닫힘] --Open 성공--> [장치 열림, 무진동] --리포트 전송(세기>0)--> [진동 중]
[진동 중] --같은 리포트 재전송--> [진동 중]  (세기를 유지하려면 계속 다시 보내야 함)
[진동 중] --세기 0 리포트 전송--> [장치 열림, 무진동] --Close--> [장치 닫힘]
```

핵심 원리는 "진동을 바꾸거나 유지한다"는 별도의 명령이 없고, **매번 완전한 리포트를 통째로
다시 보내는 것**만이 유일한 조작 방법이라는 점이다. 세기를 바꾸고 싶으면 다른 값으로 리포트를
새로 만들어 다시 쓰고, 세기를 유지하고 싶어도 같은 리포트를 계속 다시 써야 한다.

- **시작**: 0이 아닌 좌/우 모터 값을 담은 리포트를 처음 한 번 쓰는 순간 (`main.cpp:49`, 최초
  `:57` 호출).
- **유지(=사실상 "변경"과 동일한 동작)**: 앱이 50ms마다 같은 리포트를 계속 다시 쓴다
  (`main.cpp:54-59`). 이 재전송이 필요하다고 판단한 이유는 코드 주석에 "DualSense는 리포트가
  갱신되지 않으면 모터를 타임아웃시킨다"고 적혀 있기 때문인데, 이 프로젝트 안에서 그 타임아웃
  동작 자체를 실측해 검증하는 코드는 없다 — 전제로만 존재한다 (`main.cpp:52-53` 주석).
- **정지**: 세기 0인 리포트를 한 번 쓰는 것으로 이루어진다(`main.cpp:50, 61`). "정지 명령"이
  따로 있는 게 아니라 "세기 0으로 리포트를 다시 쓰는 것"이 곧 정지다.
- **비정상 종료 시**: 장치 핸들은 소멸자가 항상 닫아 주지만(`src/HidApiDualSenseTransport.cpp:64-67`),
  세기 0 리포트를 보내는 것은 `main()`이 마지막 줄까지 정상적으로 도달했을 때만 실행된다. 즉
  프로그램이 재전송 루프 도중 강제 종료되면, "핸들은 닫히지만 정지 리포트는 못 보낸 채" 끝날 수
  있고, 이후 모터가 멈추는 것은 위에서 언급한 (검증되지 않은) 컨트롤러 자체의 타임아웃에 맡겨진다.

---

## 5. 각 컴포넌트의 책임

계층마다 알아도 되는 것과 몰라야 하는 것이 명확히 나뉘어 있다.

- **`IHapticBackend`** — "이 `HapticEffect`를 재생하라"는 요청만 받는다. 그 이펙트가 왜
  발생했는지, 언제 재생돼야 하는지는 모른다. `SendEffect`/`Reset`가 성공/실패를
  `HapticBackendResult`로 알려준다는 점에서, 하드웨어가 응답하지 않을 수 있다는 것까지는
  이 인터페이스 설계에 반영돼 있다 — 다만 그 결과를 실제로 만들어내는 하드웨어 구현체는 아직 없다.
- **`MockHapticBackend`** — 위 인터페이스의 유일한 현재 구현체. 진짜로 아무것도 재생하지
  않고, 받은 이펙트를 기록·로그만 한다. 스케줄러/이펙트 계층을 하드웨어 없이 검증하기 위한 것이다.
- **`HapticScheduler`** — "언제" 재생할지만 담당한다. 백엔드를 소유하지 않고 참조만 가지며
  (`IHapticBackend&`), 그 백엔드가 무엇을 하는지는 알지도 관여하지도 않는다. 지연 후 재생,
  취소, 전체 리셋을 하나의 백그라운드 스레드로 처리한다.
- **`IDualSenseTransport`** — `HapticEffect`라는 개념 자체를 모른다. 아는 것은 "USB HID
  장치를 찾고, 열고, 닫고, 바이트를 쓴다"는 것뿐이다. 어떤 바이트를 왜 쓰는지는 이 계층의 책임이
  아니다.
- **`HidApiDualSenseTransport`** — 위 인터페이스를 HIDAPI로 구현한 것. 이 저장소에서
  실제 하드웨어를 만지는 유일한 코드다.
- **`dualsense_protocol::BuildRumbleReport`** — 순수 데이터 함수다. 장치를 열거나 쓰지
  않고, 그저 "이런 세기면 바이트가 이렇게 생겨야 한다"는 변환만 안다. `HapticEffect`도
  `IDualSenseTransport`도 모른다.

이렇게 나뉜 이유: "무엇을 재생할지"(정책) · "언제 재생할지"(타이밍) · "바이트가 무엇을
의미하는지"(포맷) · "바이트를 장치에 어떻게 쓰는지"(I/O)를 서로 몰라도 되게 만들면, 하드웨어가
없어도 정책/타이밍을 테스트할 수 있고(`MockHapticBackend`), 게임 이벤트가 없어도 포맷/I/O를
테스트할 수 있다(`dualsense_rumble_test`, `test_dualsense_usb_report.cpp`). 지금 두 축이
분리돼 있는 것은 설계 실수가 아니라 이 분리 자체가 목적이다 — 다만 그 둘을 다시 이어주는
`DualSenseBackend`가 아직 없을 뿐이다.

---

## 6. Output Report가 만들어지고 전송되는 원리

DualSense는 USB HID 장치이므로, 호스트가 컨트롤러에 무언가를 지시하려면 정해진 크기의
바이트 뭉치("출력 리포트")를 정해진 형식으로 만들어 보내야 한다. 이 프로젝트가 다루는 것은
그중 진동(rumble)에 관련된 부분뿐이다.

원리는 세 부분으로 나뉜다.

1. **리포트를 식별하는 부분** — 리포트 맨 앞 1바이트는 "이것이 어떤 종류의 출력 리포트인가"를
   나타내는 ID다. USB로 연결된 DualSense는 이 값을 고정된 하나의 번호로 기대한다.
2. **어떤 기능을 켤지 알리는 부분** — 리포트 안에는 "이번 리포트에서 어떤 기능을 실제로
   적용할 것인가"를 나타내는 플래그 바이트가 있다. DualSense는 다양한 기능(모터, LED,
   어댑티브 트리거 등)을 하나의 리포트로 한 번에 제어할 수 있게 설계되어 있어서, 관여하지 않는
   기능까지 매번 덮어쓰지 않도록 "이번에는 이 기능을 건드린다"고 명시적으로 선언해야 한다. 이
   프로젝트는 "메인 모터를 제어한다"는 것만 선언한다.
3. **실제 강도 값을 담는 부분** — 좌/우 모터 각각의 세기를 나타내는 1바이트씩(0~255, 정규화되지
   않은 raw 값)이 리포트 안 정해진 위치에 들어간다.

나머지 바이트(LED 색, 어댑티브 트리거 저항 곡선, 오디오/헤드폰 관련 필드 등으로 알려진 부분)는
이 프로젝트가 아예 건드리지 않고 전부 0으로 남긴다 — "그 기능을 켠다"고 선언하지 않았으므로,
값이 0이든 아니든 컨트롤러가 그 필드를 적용하지 않을 것이라는 전제다. 다만 이 필드들 각각이
정확히 몇 번째 바이트에 대응하는지는 이 프로젝트 코드에 없다(§7 표에서 Unknown으로 표기).

**전송**은 이렇게 만든 고정 크기 바이트 배열을 OS의 HID 드라이버에게 그대로 넘기는 것으로
이루어진다. 이 프로젝트는 HIDAPI라는 라이브러리를 통해 그 작업을 하는데, HIDAPI가 하는 일은
"장치를 찾고, 열고, 바이트를 쓰고, 닫는" 것뿐이며 USB의 세부 전송 방식(인터럽트 전송 등)은
HIDAPI와 OS 드라이버 내부에 감춰져 있어 이 프로젝트 코드는 그 계층을 몰라도 된다. 쓰기가
실패할 수 있는 이유(장치가 갑자기 뽑히는 등)를 이 프로젝트는 예외가 아니라 "성공/실패를 나타내는
값"으로 다루도록 설계했다 — 컨트롤러 연결은 언제든 끊길 수 있는 것이라는 전제가 인터페이스
설계에 반영된 것이다.

---

## 7. 주요 코드와의 매핑

앞서 설명한 원리들이 실제로 어디에 있는지에 대한 근거다.

| 설명한 원리 | 코드 위치 |
|---|---|
| §3 CLI 세기 값 파싱 | `apps/dualsense_rumble_test/main.cpp:25-29` |
| §3 장치 검색(USB 벤더/제품 ID 필터) | `src/HidApiDualSenseTransport.cpp:16-17`(`kSonyVendorId=0x054C`, `kDualSenseUsbProductId=0x0CE6`), `:69-89`(`EnumerateCandidates`) |
| §3 첫 번째 후보 선택 | `main.cpp:36-42` |
| §3/§4 장치 열기(기존 핸들 먼저 닫기) | `src/HidApiDualSenseTransport.cpp:91-106`(`Open`) |
| §4 반복 재전송(50ms, 1.5초) | `main.cpp:54-59` |
| §4 정지 리포트 + Close | `main.cpp:50, 61-62` |
| §4 소멸자에서 무조건 Close | `src/HidApiDualSenseTransport.cpp:64-67` |
| §6 리포트 길이(64바이트)·ID(0x02) | `include/sekiro_haptics/DualSenseUsbReport.hpp:10, 13` |
| §6 "메인 모터 켠다" 플래그 (byte[1] = 0x01\|0x02) | `src/DualSenseUsbReport.cpp:7-16` |
| §6 좌/우 모터 바이트 위치(byte[4]/byte[3]) | `src/DualSenseUsbReport.cpp:14-16, 20-27` |
| §6 나머지 바이트가 0으로 남는다는 것 | `test_dualsense_usb_report.cpp:31-43`(`BuildRumbleReport_LeavesUnrelatedBytesZeroed`, 자동 테스트로 고정) |
| §6 HID 쓰기 및 실패 처리 | `src/HidApiDualSenseTransport.cpp:123-136`(`WriteOutputReport`) |
| §6 hid_init/hid_exit 참조 카운트 | `src/HidApiDualSenseTransport.cpp:19-44` |
| §5 인터페이스 경계 | `include/sekiro_haptics/IHapticBackend.hpp`, `include/sekiro_haptics/IDualSenseTransport.hpp` |
| §5 스케줄러가 백엔드를 소유하지 않음 | `include/sekiro_haptics/HapticScheduler.hpp`(`IHapticBackend&`) |

**Unknown (코드에 정의가 없음, 추측하지 않음):**
- `report[2]`의 의미
- `report[5]`~`report[63]` 개별 바이트가 정확히 어떤 필드(LED/트리거/오디오 등)에 대응하는지의
  오프셋 매핑
- 여러 DualSense가 동시에 연결됐을 때 `EnumerateCandidates()`가 반환하는 순서 규칙
- USB가 아닌 Bluetooth 연결 시의 리포트 형식 — 이 코드베이스에는 Bluetooth 처리 자체가 없다
  (`IDualSenseTransport.hpp:45` "USB only; Bluetooth is out of scope" 등 여러 주석에서 명시적으로
  범위 밖으로 선언).

---

## 8. 현재 미구현 연결부와 다음 단계

지금 이 저장소에는 서로 끊어진 두 갈래 경로가 있다.

```
[경로 A · 미연결] PerfectDeflect → HapticScheduler → MockHapticBackend
    (게임 이벤트를 이펙트로 표현하고 스케줄링하는 것까지는 되지만, 로그만 남기고 끝난다)

[경로 B · 하드웨어까지 도달하지만 게임과 무관] CLI motorStrength → BuildRumbleReport → HID Write
    (실제로 컨트롤러를 진동시키지만, 사람이 CLI로 세기를 직접 넣어야 하고 HapticEffect를 모른다)
```

이 둘을 잇는, 즉 §2의 목표 아키텍처에서 마지막으로 빠진 조각은 `IHapticBackend`를 구현하는
**`DualSenseBackend`** 하나다. 이것이 있다면:

- `HapticScheduler`가 넘기는 `HapticEffect`(`MotorIntensity`의 `left`/`right`, `[0.0, 1.0]`)를
  받아,
- 그 값을 `BuildRumbleReport`가 요구하는 raw `0-255` 범위로 변환하고(`DualSenseUsbReport.hpp:23-26`
  주석에 "이 변환은 상위 계층의 몫"이라고 이미 명시되어 있다),
- `IDualSenseTransport::WriteOutputReport`로 실제 전송하며,
- §4에서 사람이 수동으로 하던 "50ms 재전송"과 "정지 리포트 전송"을 `HapticEffect.duration`에
  맞춰 자동으로 수행해야 한다 — 지금은 이 재전송/정지 로직이 `main.cpp`에 하드코딩된 일회성
  스크립트로만 존재하고, 재사용 가능한 컴포넌트로 분리되어 있지 않다.

`docs/01-architecture.md`의 "Likely next steps"에도 같은 항목이 계획으로만 적혀 있으며, 실제
구현 코드는 아직 없다.
