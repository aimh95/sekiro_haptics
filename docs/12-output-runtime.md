# 12 — 출력 런타임: Adaptive Trigger · PCM Mixer · 프리셋 · 통합

패링/방어 **감지**는 [10](10-deflect-signal-first.md)과
[DEFLECT_STATUS.md](astra/results/DEFLECT_STATUS.md)가, **소리와 촉감의 설계**는
[11](11-guard-feedback.md)이 담당한다. 이 문서는 그 사이 — 감지된 이벤트가
어떤 경로로 컨트롤러의 네 가지 출력(PCM 햅틱, 내장 스피커, L2/R2 적응형 트리거,
레거시 럼블)에 도달하는지 — 만 다룬다.

```
GameSignal → IGameEventDetector → GameEvent
                                     │
                              MappingRepository        (event → presetId)
                                     │
                           OutputPresetRepository      (presetId → 레이어들)
                                     │
                                OutputRuntime
                                 │           │
                        IPcmOutputSink   AdaptiveTriggerRuntime
                      (DualSenseAudio-       (L2/R2 효과 수명)
                       PcmSink)                  │
                                          DualSenseOutputState
                                         (HID 출력 상태의 단일 소유자)
                                                  │
                                          IDualSenseTransport
```

Detector는 HID도 WASAPI도 직접 호출하지 않는다. 이 구조는 [01](01-architecture.md)이
예고해 둔 확장이고, `HapticPreset`/`PresetRepository`는 **그대로 남아 있다**.

---

## 1. 적응형 트리거 프로토콜 — 출처와 검증 범위

추측으로 채운 바이트는 없다. 아래 네 출처를 2026-09-20에 직접 내려받아 대조했다.

| # | 출처 | 이 저장소가 가져온 것 |
|---|---|---|
| [1] | Nielk1, *DualSense Trigger Effect Generator*, **Revision 6** ([gist](https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db)) | 공식 모드 바이트, 존 비트팩킹, 파라미터 유효 범위, "효과 없음은 Off로 보낸다" 규칙 |
| [2] | Linux `drivers/hid/hid-playstation.c` (`git.kernel.org` plain) | `struct dualsense_output_report_common` 필드 순서(`static_assert` 크기 47), `DS_OUTPUT_VALID_FLAG*` 비트, `audio_control` 출력 경로 비트 4..5, `audio_control2` 프리앰프 비트 0..2 |
| [3] | Ohjurot/DualSense-Windows `DS5_Output.cpp` | 오른쪽 트리거 블록이 리포트 바이트 11, 왼쪽이 22. 모드 `0x01`은 `[start, force]` 원시 바이트 |
| [4] | flok/pydualsense `enums.py` | 모드 바이트 교차 확인 |

[2]의 구조체가 `reserved2[27]`를 **바이트 11..37**에 놓는데, [1]/[3]이 트리거 블록을
쓰는 위치가 정확히 그 안이다. 이 일치가 매핑이 맞다는 교차 검증이다. 같은 구조체가
`audio_control2`를 38, `valid_flag2`를 39에 놓는 것도 이 저장소가 이미 쓰던 값과 같다.

### 1.1 구현한 모드와 그 모드만의 파라미터

**모드마다 파라미터 어휘가 다르다.** Feedback에는 frequency가 없고 Vibration에는
end position이 없다. 그래서 모드별로 별도 구조체를 두고, 인코더는 `mode`가 지목한
구조체만 읽는다.

| 모드 | 바이트 | 파라미터 | 범위 | 단위 |
|---|---|---|---|---|
| `off` | `0x05` | 없음 | — | — |
| `feedback` | `0x21` | `position` | 0..9 | **존 인덱스** (0=안 누름, 9=끝까지) |
| | | `strength` | 0..8 | 3비트로 패킹되는 **등급**, 0=효과 없음 |
| `weapon` | `0x25` | `startPosition` | **2..7** | 존 인덱스 |
| | | `endPosition` | start+1..8 | 존 인덱스 |
| | | `strength` | 0..8 | 등급 |
| `vibration` | `0x26` | `position` | 0..9 | 존 인덱스 |
| | | `amplitude` | 0..8 | 등급 |
| | | `frequencyHz` | 0..255 | **헤르츠**, 1바이트 |
| `simple-feedback` | `0x01` | `startPosition`, `force` | 0..255 | **원시 바이트**, 존 인덱스 아님 |

- **존 인덱스도 등급도 백분율이 아니고 뉴턴도 아니다.** 실제로 얼마나 밀어내는지는
  측정하지 않았다.
- **범위를 벗어난 값은 clamp하지 않고 거부한다.** clamp된 저항은 요청한 효과가 아니고,
  두 설정을 비교하는 실험 자체를 무의미하게 만든다.
- `strength`/`amplitude`/`frequencyHz`가 0이면 [1]을 따라 **Off로 인코딩**한다.
  이 사실은 `TriggerEncodeResult::reducedToOff`로 드러난다 (조용히 넘어가지 않는다).
- `simple-feedback`(0x01)은 이 저장소가 이미 쓰던 모드다. [1]은 대신 `feedback`을
  권하지만, 기존 동작을 바꾸지 않기 위해 남겼다.

### 1.2 구현하지 **않은** 것과 그 이유

- **모드 `0x02`** — [3]은 "section resistance `[start, force]`", [1]은
  "Simple_Weapon `[start, end, strength]`"라고 한다. **둘이 모순되고 이 컨트롤러에서
  확인한 바 없다.** 그래서 구현하지 않았다. 두 출처가 일치하는 `weapon`(0x25)을 쓴다.
- **Bow(0x22) / Galloping(0x23) / Machine(0x27)** — [1]이 "펌웨어에 남아 있지만
  비공식, 향후 제거될 수 있음"으로 분류한 것들. 이번 범위 밖이다.
- **디버그 모드 0xFC..0xFE** — [1]에 따르면 **물리 리셋 버튼을 누르기 전까지 트리거
  상태를 손상시킨다.** 도달할 수 있는 코드 경로를 만들지 않았다.

---

## 2. HID 출력 상태의 단일 소유자 (`DualSenseOutputState`)

### 2.1 무엇이 문제였나

기존에는 빌더 세 개가 각각 **0으로 채운 새 리포트**를 만들었다.

| 빌더 | 세운 valid flag |
|---|---|
| `BuildRumbleReport` | `0x01\|0x02` (모터) |
| `BuildFeedbackReport` | `0x01\|0x02\|0x04\|0x08` (모터 + **양쪽 트리거**) |
| `BuildSpeakerOutputReport` | `0xF0` + flag1 `0x80` (오디오) |

flag가 꺼진 필드는 펌웨어가 무시하므로 한 번에 하나만 쓰는 동안은 문제가 없다.
그러나 `BuildFeedbackReport`는 **매 전송마다 트리거 enable 비트를 세운다.** 럼블만
바꾸려던 호출이 그 호출자의 `FeedbackState`에 들어 있던 값으로 **양쪽 트리거를 덮어쓴다.**
적응형 트리거와 스피커 라우팅이 동시에 살아 있는 순간 이건 실제 버그가 된다.

### 2.2 해결

상태를 한 곳에 두고, **전송할 때마다 이 프로세스가 점유한 모든 섹션을 함께 싣는다.**

- 섹션별 **점유(claim)** 플래그를 둔다. 점유한 섹션의 flag만 세우므로 라이트바,
  플레이어 인디케이터, 마이크 뮤트 LED, 절전 제어는 **건드리지 않는다.**
- 트리거를 바꿔도 오디오 라우팅이 빠지지 않고, 오디오를 설정해도 트리거가 풀리지 않는다.
  둘 다 매번 같은 상태에서 다시 인코딩되기 때문이다.
- `Submit()`은 **바뀐 게 있을 때만** 쓴다. 실패하면 dirty를 유지해 다음에 재시도한다
  (장치가 받아들였다고 가정하지 않는다).

### 2.3 럼블 ↔ PCM 햅틱 모드 충돌은 명시적이다

`valid_flag0` 비트 1은 `HAPTICS_SELECT`이고, Linux 드라이버는 이를
*"Select classic rumble style haptics and enable it"* 이라고 주석했다 [2]. 즉
**모터를 점유하면 액추에이터가 PCM 경로에서 떨어져 나간다.**

- 모터 섹션은 호출자가 **레거시 럼블을 명시적으로 요청할 때만** 점유한다.
- `MarkPcmHapticsActive()`는 바이트를 하나도 쓰지 않는 장부 기록이다. 특히
  `HAPTICS_SELECT`를 세우지 않는다.
- `HapticPathInUse()`가 `pcm` / `legacy-rumble` / `unclaimed` 중 실제 상태를 보고한다.
  둘 다 설정돼 있으면 `legacy-rumble`을 보고한다 — 펌웨어가 실제로 따르는 건 그 flag이고,
  그 상태에서 "pcm"이라고 말하는 건 의도를 보고하는 것이지 장치 상태가 아니다.

`--play` / `--live`는 이제 시작할 때 이 경로를 한 줄로 찍는다.

### 2.4 기존에 검증된 바이트와 동일함을 테스트로 고정했다

단일 소유자를 도입하면서 **검증된 경로로 나가던 바이트는 하나도 바뀌지 않았다.**
`tests/test_adaptive_trigger.cpp`의 네 테스트가 이를 고정한다.

| 테스트 | 비교 대상 |
|---|---|
| `OutputState_FullAudioClaimMatchesTheVerifiedSpeakerOutputReport` | `BuildSpeakerOutputReport(enable)` |
| `OutputState_AudioReleaseMatchesTheVerifiedReleaseReport` | `BuildSpeakerOutputReport(!enable)` |
| `OutputState_RoutingOnlyClaimMatchesTheNarrowRoutingReport` | `BuildAudioRoutingReport` |
| `OutputState_LegacyRumbleClaimMatchesTheVerifiedRumbleReportFlags` | `BuildRumbleReport` |

`AudioOutputSettings::routingOnly`는 좁은 라우팅 리포트(볼륨 + 경로 두 바이트만)를
재현하기 위한 것이고, `guard_feedback`의 라이브 경로는 **전체 오디오 섹션**을 쓴다 —
`EnableSpeakerRouting`이 원래 보내던 것이 그쪽이기 때문이다.

---

## 3. 효과 수명 (`AdaptiveTriggerRuntime`)

### 3.1 effect id가 필요한 이유

수명이 있는 효과는 만료 시 꺼야 한다. "이 쪽에 뭔가 예약돼 있다"만 기억하면,
이전 효과가 만료될 때 **그 사이 새로 걸린 효과까지 꺼버린다.**

그래서 `Apply()`마다 단조 증가하는 id를 발급하고, 각 side는 지금 들고 있는 id를 기억하며,
**만료된 id가 아직 그 side가 들고 있는 id일 때만** 트리거를 푼다. `Cancel()`도 같은
검사를 하므로 오래된 id로 취소해도 새 효과가 죽지 않는다.

테스트: `TriggerRuntime_AnOldEffectsExpiryDoesNotTurnOffTheOneThatReplacedIt`,
`TriggerRuntime_CancellingAStaleIdIsANoOp`.

### 3.2 규칙 표

| 상황 | 동작 |
|---|---|
| `durationUs = kHoldUntilReplaced(0)` | 교체·취소·리셋 전까지 유지. 암묵적 타임아웃 없음 |
| 같은 side에 새 `Apply` | 교체. `replaced` 카운터 증가, 이전 id의 만료는 무효화 |
| 수명 만료 (`Tick`) | 해당 side만 Off. 다른 side와 오디오/럼블은 그대로 |
| `Cancel(side, id)` | id가 현재 것일 때만 해제. 아니면 `staleCancelsIgnored`++ |
| `ResetToNeutral` | **트리거 두 개만** Off. 오디오 라우팅과 럼블은 건드리지 않는다 |
| 파라미터 거부 | 상태 무변경, 전송 없음. 들고 있던 효과가 그대로 남는다 |
| HID 쓰기 실패 | 처리 가능한 오류로 보고 **해당 트리거를 해제**하고 슬롯을 비운다. 도달하지 않은 효과를 나중에 만료시키지 않는다 |
| `OnDeviceLost()` | 점유를 버린다. **아무것도 쓰지 않고**, 하드웨어가 중립이 됐다고 주장하지 않는다 |
| `OnReconnect()` | 슬롯을 비우고 중립 상태를 **강제 전송**. 끊기기 전 효과를 재생하지 않는다 |

### 3.3 보고하지 않는 것

컨트롤러를 뽑거나 프로세스가 강제 종료되면 쓰기가 일어나지 않는다. 그때 펌웨어에
마지막으로 전달된 상태가 남는다. 이 런타임은 **물리적 Reset을 보장한다고 보고하지 않는다.**

---

## 4. PCM Mixer — 이미 있던 것과 이번에 더한 것

[11절 7~13](11-guard-feedback.md)에서 만들어진 믹서는 이미 요구사항을 만족하고 있었다.
다시 만들지 않았다.

**이미 있던 것** (코드에서 확인):
- 세션 동안 스트림 유지. `Open()`/`Start()`는 루프 **진입 전 한 번**, `Stop`/`Reset`은
  `Close()`와 소멸자에만.
- voice마다 **자기 `position`과 자기 수명**. 공용 커서 없음.
- 이벤트 구동 WASAPI + 전용 `TIME_CRITICAL` 렌더 스레드.
- soft cap(36)/hard cap(44). 초과 시 **가장 조용하고 거의 끝난** 꼬리부터 페이드.
- 스피커/햅틱 **독립 리미터**. voice 개수로 나누지 않는다.

**이번에 더한 것** (믹싱 연산은 한 줄도 바꾸지 않았다):
- `Queue(..., correlationId)` / `QueuePair(..., correlationId)` — 이벤트 추적용 태그.
- `AudioVoice::queuedUs` / `firstRenderedUs` — 큐에 들어간 시각과 **첫 샘플이 실제로
  제출 버퍼에 들어간** 시각.
- `VoiceLifecycleRecord` + `VoiceEndReason` — voice별 종료 이유
  (`completed` / `retired-at-cap` / `hard-dropped` / `dropped`)를 **경계 있는 링**(기본 256)에 기록.
  합계만으로는 "어느 이벤트가 잘렸나"를 답할 수 없기 때문이다.

`--play`와 `--live`가 세션 끝에 이 분포를 찍는다.

---

## 5. 프리셋 확장 (`OutputPreset`)

### 5.1 스키마 버전

| 버전 | 내용 |
|---|---|
| 1 | `presets[].legacyRumble`만. **기존 파일은 그대로 로드된다.** |
| 2 | `speaker[]`, `pcmHaptic[]`, `trigger[]` 추가. `legacyRumble`도 함께 가질 수 있다 |

`schemaVersion` 키가 **없으면 1로 취급한다** — 이 변경 이전에 쓰인 모든 파일이 그렇다.
이 빌드가 아는 것보다 **높은 버전은 거부한다** (모르는 필드를 추측해 읽지 않는다).

`config/presets.json`(v1)이 계속 로드되는지는
`OutputPreset_TheShippedConfigFileStillLoads`가 실제 파일로 확인한다.

### 5.2 필드 · 단위 · 기본값

```jsonc
{
  "schemaVersion": 2,
  "presets": [{
    "presetId": "guard.deflect",
    "displayName": "Deflect",
    "speaker":   [{ "cueId": "deflect.speaker", "gain": 1.0, "startOffsetMs": 0 }],
    "pcmHaptic": [{ "cueId": "deflect.haptic", "gain": 1.0, "balance": 0.0,
                    "startOffsetMs": 0 }],
    "trigger":   [{ "side": "r2", "mode": "weapon", "startPosition": 3,
                    "endPosition": 7, "strength": 8,
                    "startOffsetMs": 15, "durationMs": 120 }],
    "legacyRumble": { "left": 0.4, "right": 0.8, "durationMs": 28, "startOffsetMs": 0 }
  }]
}
```

| 필드 | 단위 / 범위 | 기본값 | 비고 |
|---|---|---|---|
| `startOffsetMs` | ms, ≥ 0 | 0 | 이벤트 시각 기준. **음수는 거부** (출력이 원인보다 앞설 수 없다) |
| `durationMs` | ms, ≥ 0 | 0 | **trigger 레이어에만 있다.** 0 = 교체/리셋까지 유지 |
| `gain` | 선형 배수 0..4 | 1.0 | dB 아님, 물리량의 백분율 아님 |
| `balance` | −1..+1 | 0.0 | pcmHaptic 전용 |
| `cueId` | 문자열 | — | 런타임 클립 라이브러리의 이름. **바인드 시점에 해석** |
| `side` | `l2` / `r2` | — | trigger 전용 |

**speaker/pcmHaptic 레이어에는 duration 필드가 아예 없다.** 길이는 클립의 길이이고,
클립을 도중에 자르는 것은 겹침 정책이 금지하는 바로 그 동작이다.

### 5.3 거부 규칙 — 출력 전에 막는다

`PresetRepository`는 범위를 벗어난 럼블 세기를 clamp하고 로드했다. `OutputPresetRepository`는
**거부한다.** 트리거 파라미터는 "조금 줄여도 되는 음량"이 아니라 펌웨어가 거부하거나
다른 뜻으로 읽는 바이트이기 때문이다.

- 모드가 갖지 않은 파라미터 키(예: `feedback`에 `frequencyHz`)는 **오류**다.
  조용히 무시하면 설정 실수가 숨는다.
- 레이어가 하나도 없는 프리셋은 거부한다 (아무것도 출력하지 않을 것이므로).
- 잘못된 엔트리 하나가 파일 전체를 버리지는 않는다. 그 엔트리만 `errors`에 남고
  리포지토리에 **들어가지 않으므로 출력 경로에서 도달할 수 없다.**

---

## 6. 통합 (`OutputRuntime`)

### 6.1 바인드 — 시작할 때 막는다

`Bind()`는 모든 프리셋의 `cueId`가 실제로 해석되는지, 트리거 레이어가 있는데 트리거
런타임이 없지는 않은지를 **이벤트가 오기 전에** 검사한다. 없는 클립은 전투 중의 조용한
무동작이 아니라 시작 시점의 오류여야 한다.

### 6.2 correlation ID와 단계별 시각

이벤트마다 correlation id를 발급하고, **실제로 관측 가능한 단계만** 기록한다.

| 필드 | 의미 |
|---|---|
| `eventUs` | detector가 찍은 이벤트 시각 |
| `receivedUs` | 런타임이 받은 시각 |
| `mappedUs` | 매핑이 해석된 시각 |
| `lastDispatchUs` | 이 이벤트의 마지막 레이어가 나간 시각 |
| `firstPcmQueuedUs` | 첫 PCM voice가 **믹서에 들어간** 시각 |
| `firstHidSubmitUs` | 첫 HID 리포트 쓰기가 **반환된** 시각 |

같은 id가 믹서의 `VoiceLifecycleRecord`에도 실려, "이 패링이 만든 voice 3개가 모두
끝까지 재생됐는가"를 확인할 수 있다.

> **이 시각들은 소리가 들린 순간도 트리거가 움직인 순간도 아니다.** PCM 큐 시각은
> voice가 믹서에 들어간 때이고, `firstRenderedUs`조차 오디오 엔진 버퍼 지연 **이전**이다.
> HID 제출 시각은 `hid_write`가 반환한 때다. 어느 쪽도 물리적 시작 시각이 아니며,
> 이 코드는 그렇게 보고하지 않는다.

### 6.3 HID와 오디오의 전송 방식 차이

공통 기준 시각(단조 시계) 하나를 넘기지만 두 경로를 **따로** 기록한다. HID는 USB 쓰기가
반환하면 끝이고, PCM voice는 믹서의 다음 버퍼를 기다린다. 두 값을 하나로 합치면 그 차이가
사라진다.

### 6.4 트리거 충돌 처리는 PCM 합산과 별개다

트리거는 side별 슬롯 교체 규칙으로, PCM은 voice 합산 + 리미터로 각각 처리된다.
`OutputRuntime_TriggerChangesDoNotDisturbTheQueuedPcmOutput`이 트리거의 적용·교체·만료가
PCM 호출을 하나도 건드리지 않음을 고정한다.

### 6.5 늦은 이벤트

`maxOutputLatencyUs`(기본 120 ms, `config/guard_cues.json`에서 옴)보다 늦게 도착한
이벤트는 **버린다.** 기존 라이브 경로의 정책과 값이 같고, 이제 런타임이 소유한다.

---

## 6.6 실행 형태 — 채널 인자를 주지 마라

**최종 실행 형태는 이것이다.**

```powershell
& $E --live --pid (Get-Process sekiro).Id --enemy
```

`--audio` / `--mode` / `--speaker-channel` / `--haptic-left` / `--haptic-right`를
주지 않으면 `ApplyDeviceDefaults`가 `config/guard_cues.json`의 `device` 블록에서
채운다 — 엔드포인트 6, **스피커 채널 1**, 액추에이터 2/3. 이 값들은 이 기계에서
듣고 만져서 확정한 것이다(`GuardDeviceSettings` 주석).

특히 **`--speaker-channel 2`는 틀렸다.** 2는 왼쪽 보이스코일이고 내장 스피커는 1이다.
2로 보내면 클랭이 스피커가 아니라 액추에이터로 가서 peak 0.85짜리 진동 밑에 묻힌다.
11절 4항의 예시 명령이 이 값을 확정하기 전에 쓰인 탓에 한동안 `2`를 달고 있었다.
인자는 실험으로 덮어쓸 때만 쓴다.

## 7. 이번에 바뀐 실행 경로

- `--play`와 `--live` **둘 다** 동일한 `GameEvent → Mapping → Preset → Output` 경로를 탄다.
  `--play`는 이벤트 소스가 스크립트일 뿐, 그 외에는 `--live`의 예행 연습이다.
- 큐잉 파라미터(클립, 채널, 게인, balance)와 겹침 정책은 **바뀌지 않았다.**
  `--overlap-test` 7단계 결과가 통합 전후 동일하다.
- 감지 루프는 5 ms 그리드와 `SekiroPlayerGuardReader`/`GuardOutcomeEventDetector`를
  그대로 쓴다. 추가된 것은 이벤트당 해시 조회와 폴당 `outputRuntime.Tick()` 한 번인데,
  Tick은 **실제로 만료·예약이 있을 때만** 쓴다.
- `--trigger-demo`가 새로 생겼다. **기본은 꺼져 있다.**

### 7.1 `--trigger-demo`의 값은 데모다

| 이벤트 | 트리거 |
|---|---|
| deflect | R2 `weapon` start 3, end 7, strength 8, 120 ms |
| block | L2 `feedback` position 2, strength 5, 180 ms |

이 값들이 맞다고 말하는 측정은 **없다.** Sekiro의 L1 가드가 L2/R2 저항과 대응한다는
근거도 없다. 게임 버튼을 재매핑하지 않는다. 트리거 출력을 실제 PCM 큐와 **나란히**
느껴 보기 위한 장치이고, 그래서 명시적으로 켜야 한다.

---

## 8. 검증 상태

### 8.1 자동으로 실행해 통과한 것

```
cmake --build build-hw
ctest --test-dir build-hw --output-on-failure --timeout 240   → 4/4 통과
sekiro_haptics_unit_tests                                     → 722/722
```

기준선(작업 시작 시점)은 667이었다. 신규 55개의 내역:

| 파일 | 개수 | 다루는 것 |
|---|---|---|
| `test_adaptive_trigger.cpp` | 19 | 모드별 인코딩, 범위 거부, 리포트 합성, 기존 리포트와의 바이트 동일성 |
| `test_adaptive_trigger_runtime.cpp` | 12 | L/R 독립, 만료, 교체, 취소, Reset, 재연결, 쓰기 실패 |
| `test_output_preset.cpp` | 10 | v1/v2 로드, 범위 거부, 모드 외 파라미터 거부, 실제 config 파일 |
| `test_output_runtime.cpp` | 14 | 매핑/프리셋 해석, 연속 이벤트, 시작 오프셋, 단계별 시각, 바인드 |

### 8.2 실제 하드웨어에서 실행한 것 (기능 수준)

컨트롤러(USB, VID 0x054C / PID 0x0CE6, HID 출력 리포트 선언 길이 48바이트)가 연결된
상태에서 실행했다. **확인한 것은 "쓰기가 성공했고 수명 규칙이 돌았다"이지, 저항이
어떻게 느껴지는지가 아니다.**

| 명령 | 결과 |
|---|---|
| `trigger_lab --effect feedback --side r2 --position 3 --strength 6 --duration-ms 600` | `applied=1 expired=1 writeFailures=0`, 트리거 중립 복귀 |
| `guard_feedback --play mixed --count 5 ... --interval-ms 200` | 5 이벤트 → 10 레이어 → **15 voice, 15/15 끝까지 재생**, 클리핑 0 |
| `guard_feedback --play ... --trigger-demo` | 5 이벤트 → 15 레이어, 트리거 `applied=5 expired=5 writeFailures=0`, **PCM은 그대로 15/15** |
| `guard_feedback --overlap-test` | 7단계 전부 `playedToTheEnd` 100%, `retiredAtCap=0`, `hardDropped=0`, 중간 공백 0, 클리핑 0 |

### 8.3 실행하지 **않은** 것 / 확인되지 않은 것

- **`--live`(Sekiro 연결)는 이 세션에서 실행하지 못했다.** 게임이 실행 중이 아니었다.
  라이브 경로의 코드 변경(런타임 경유, Tick, 종료 시 중립 복귀)은 **컴파일과 단위
  테스트만 통과했고 게임과 함께 실행된 적이 없다.** ([11절 14.2](11-guard-feedback.md)와
  같은 함정을 피하려고 명시한다.)
- **저항·진동의 체감은 전혀 검증되지 않았다.** 존 인덱스 3..7이 손가락의 어디인지,
  strength 8이 얼마나 센지 측정하지 않았다.
- **Vibration 모드(0x26)는 실기에서 눌러 본 적이 없다.** 바이트는 [1]과 일치하지만
  이 펌웨어에서 어떤 느낌인지는 미확인이다.
- **실제 케이블 뽑기 시나리오는 테스트하지 않았다.** `trigger_lab --demo reconnect`는
  핸들을 소프트웨어로 닫았다 여는 것이고, 뽑힌 케이블에 대해서는 아무것도 증명하지 않는다.
- 레거시 럼블 레이어는 단위 테스트로만 확인했다. 실기에서 럼블과 PCM을 오가며
  `HAPTICS_SELECT` 전환이 어떻게 들리는지는 미확인이다.

---

## 9. 다음 단계 (이번 범위 밖)

기록만 해 둔다: EffectLab → Sekiro UX 및 **날카로운 최초 접촉 · 금속성 질감 ·
주파수별 체감 보정 · 실제 충돌 측정과 FFT/STFT** → 배포 준비 → 다른 게임 → Haptic Studio.
GUI, 실제 충돌 측정, 다른 게임 지원, 자동 배포는 이번 변경에 포함하지 않았다.
