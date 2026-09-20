# 11 — 패링 / 일반 방어 충돌 피드백

패링(Just Guard)과 일반 방어를 **소리와 촉감으로 구별**하는 출력 경로. 감지 쪽은
[docs/astra/results/DEFLECT_STATUS.md](astra/results/DEFLECT_STATUS.md) 7절이 담당하고,
이 문서는 그 이벤트를 실제 DualSense 출력으로 바꾸는 부분만 다룬다.

## 1. 이 PC에서 실제로 확인된 장치 사실

추측으로 고정한 값은 없다. 아래는 전부 도구로 조회하거나 손으로 확인한 결과다.

```
HID      DualSense Wireless Controller  VID 0x054C / PID 0x0CE6, USB
오디오    "스피커(2- DualSense Wireless Controller)"  (--list-devices 기준 index 6)
형식      GetMixFormat       48000 Hz, 4 ch, 32-bit IEEE_FLOAT, 채널마스크 0x33
         IsFormatSupported  shared = S_OK,  exclusive = AUDCLNT_E_UNSUPPORTED_FORMAT
```

`IsFormatSupported(SHARED, mixFormat)`가 `S_OK`가 아니면 열기를 **거부**한다.
지원되지 않는 채널 배치를 조용히 변환해 성공한 것처럼 처리하지 않는다.

### 채널 역할 — 들어보고 만져서 확정했다

채널마스크는 `FL/FR/BL/BR`이라고 말하지만 **실제 역할은 달랐다.**

| 채널 | 880 Hz | 150 Hz | 60 Hz 위치 | 결론 |
|---|---|---|---|---|
| 0, 1 | 무음 | 무음 | — | 헤드셋 출력으로 추정 (헤드셋 미연결 시 무음) |
| 2 | 소리 남 | **진동** | 왼손 | **왼쪽 보이스코일 액추에이터** |
| 3 | 소리 남 | **진동** | 오른손 | **오른쪽 보이스코일 액추에이터** |

같은 채널에서 고역은 소리로 새고 저역은 진동으로 전달되는 것이 보이스코일의 전형적인
거동이다. 좌/우로 갈린다는 점이 결정적이었다 — **DualSense의 내장 스피커는 모노 1개**라
좌우 분리가 나올 수 없다.

확인 방법:

```powershell
$E = ".\build-hw\apps\guard_feedback\sekiro_guard_feedback.exe"
& $E --channel-test --audio 6 --endpoint-volume 0.8               # 880/150 Hz 두 번씩
& $E --channel-test --audio 6 --endpoint-volume 0.8 --probe-hz 60 # 느낌/소리 분리용
```

### 소리가 안 들릴 때 실제로 걸렸던 것들

| 증상 | 원인 | 조치 |
|---|---|---|
| 귀를 대야 들림 | **Windows 엔드포인트가 음소거 + 볼륨 0%** | `--probe-format`이 표시, `--endpoint-volume 0.8`로 해제 |
| 컨트롤러 스피커가 조용함 | 라우팅 리포트의 볼륨 바이트가 `0x50`(31%)에 고정 | `--speaker-volume`(기본 `0xFF`) |
| `--channel-test`는 진동하는데 `--play --mode haptic`은 무음 | 라우팅 리포트를 **스피커를 원할 때만** 보내고 있었음. 출력 경로 바이트는 엔드포인트 전체를 설정한다 | 모드와 무관하게 항상 전송 |
| 짧은 큐가 안 느껴짐 | 햅틱 피크 0.42로 지각 역치 아래 | `gain` 0.55 → 0.90 (피크 0.69/0.77) |

출력 경로 바이트는 원래 `3`으로 하드코딩돼 있었고 검증된 근거가 없었다. 이제
`--audio-path 0..3`으로 지정하고 `--path-sweep`으로 훑을 수 있다.

## 2. 두 가지 피드백

### 소리 — 공급받은 녹음에서 **단일 타격**을 잘라 사용

`audio/`의 두 MP3는 **연속 타격 녹음**이었다.

```
sekiro_deflect.mp3  충돌 5회 @ 42,110,215,312,376 ms   centroid 5339 Hz
sekiro_parry.mp3    충돌 6회 @ 20,120,211,330,418,495 ms  centroid 3584 Hz
```

한 이벤트에 여러 번 부딪히는 음원을 쓰면 안 되므로 단일 타격을 추출했다. 추출기는
다음 충돌을 침범하지 않도록 길이를 자동으로 줄이고 그 사실을 보고한다.

```powershell
& $E --extract-hit audio/sekiro_deflect.mp3 --out audio/generated/deflect_hit.wav --onset 0 --hit-length-ms 170
& $E --extract-hit audio/sekiro_parry.mp3   --out audio/generated/block_hit.wav   --onset 1 --hit-length-ms 78
```

| 자산 | 길이 | 충돌 | centroid | 대역 |
|---|---|---|---|---|
| `deflect_hit.wav` | 72 ms | 1 | 5482 Hz | 3–8k 41%, 8–24k 24% → 맑고 선명 |
| `block_hit.wav` | 78 ms | 1 | 3512 Hz | 1–3k 40%, 8–24k 8% → 낮고 억제, 금속성 유지 |

**매핑 근거는 파일 이름이 아니라 음색이다.** 영문에서 deflect와 parry는 같은 뜻이라
이름으로는 패링/방어를 구분할 수 없다. 밝은 쪽을 패링, 어두운 쪽을 일반 방어로 배정했다.
바꾸려면 `config/guard_cues.json`의 `clipPath` 두 줄만 교체하면 된다.
`clipPath`를 `""`로 비우면 내장 합성 파형으로 되돌아간다.

### 촉감 — 항상 별도로 합성

**녹음을 액추에이터로 복사하지 않는다.** kHz 금속음은 보이스코일에서 충격이 아니라
얇은 버즈가 된다. 햅틱은 저역에서 따로 설계한다.

| | 패링 | 일반 방어 |
|---|---|---|
| 시작(attack) | 2 ms | 6 ms |
| 충격(impact) | 20 ms @ 185 Hz | 28 ms @ 110 Hz |
| 잔향(tail) | 35 ms @ 250 Hz, 빠른 감쇠 75/s | 45 ms @ 135 Hz, 느린 감쇠 48/s |
| 결과 피크 | 0.69 | 0.77 |

세기가 아니라 **시작 모양·주파수·감쇠**로 구분된다. 테스트
`GuardCue_TheTwoProfilesAreNotDistinguishedByLoudnessAlone`이 두 프리셋의 RMS 비를
0.5~2.0 안으로 강제해서, "방어를 더 크게 만들어 구분"하는 방향으로 흘러가지 못하게 막는다.

## 3. 조절 — `config/guard_cues.json`

| 하고 싶은 것 | 바꿀 값 |
|---|---|
| **패링이 너무 날카롭다** | `deflect.speaker.clipPeak` ↓ / 합성일 땐 `baseHz` ↓, 높은 `partials`의 `amplitude` ↓ |
| **방어가 너무 둔하다** | `block.speaker.baseHz` ↑, `partials[2..]`의 `amplitude` ↑, `strikeNoiseHighpass` ↑ |
| **잔진동이 길다** | `haptic.tailMs` ↓, `haptic.tailDecayPerSecond` ↑ |
| **첫 충격이 약하다** | `haptic.gain` ↑, `haptic.attackMs` ↓ |
| **둘이 너무 비슷하다** | `haptic.impactHz` 격차를 벌린다 (185 vs 110) |
| 연속 타격이 뭉갠다 | `retriggerDuck` ↓ (잔향을 더 줄임) |
| 소리/진동 전체 세기 | `speakerVolume`, `hapticStrength` (서로 독립) |
| 좌우 균형 | `balance` (−1 왼쪽 … +1 오른쪽) |

**이 값들은 비교 시작점이지 측정된 최적값이나 장치 사양이 아니다.** 실제 감각 판단은
직접 듣고 쥐어봐야 한다 — 자동 테스트는 파형의 성질만 검사한다.

## 4. 실행

```powershell
cd C:\workspace\sekiro_haptics
$E = ".\build-hw\apps\guard_feedback\sekiro_guard_feedback.exe"

# 장치 확인
& $E --list-devices
& $E --probe-format --audio 6
& $E --channel-test --audio 6 --endpoint-volume 0.8

# 촉감만 / 소리만 / 둘 다
& $E --play alternate --count 6 --audio 6 --mode haptic  --haptic-left 2 --haptic-right 3
& $E --play alternate --count 6 --audio 6 --mode speaker --speaker-channel 2
& $E --play alternate --count 6 --audio 6 --mode both --speaker-channel 2 --haptic-left 2 --haptic-right 3

# 겹침 확인
& $E --play mixed --audio 6 --mode haptic --haptic-left 2 --haptic-right 3 --interval-ms 500
& $E --play mixed --audio 6 --mode haptic --haptic-left 2 --haptic-right 3 --interval-ms 200
& $E --play mixed --audio 6 --mode haptic --haptic-left 2 --haptic-right 3 --interval-ms 100

# 실제 게임 연동 (읽기 전용)
& $E --live --pid <sekiro pid> --audio 6 --mode both --speaker-channel 2 --haptic-left 2 --haptic-right 3
```

패턴: `deflect` / `block` / `alternate` / `repeat`(같은 종류 반복 후 반대) / `mixed`(패링2+방어2+패링).

**수동 재생은 게임 인식 성능과 무관하다.** 그 수치를 감지 성능으로 보고하지 않는다.

## 5. 실시간 경로

`--live`는 `SekiroPlayerGuardReader`(5 ms 폴링) → `GuardOutcomeEventDetector` →
오디오 큐 순서로 흐른다.

- 파형은 **루프 시작 전에 전부 렌더링**한다. 폴링 루프는 합성·파일 읽기·장치 초기화를
  하지 않는다.
- 폴링 간격은 5 ms **격자**로 유지하고, 서비스하지 못한 슬롯은 늘리는 대신 `missedSlots`로
  센다. 실제 평균/최악 간격을 세션 끝에 보고한다. 200 ms로 되돌아가지 않는다.
- `maxOutputLatencyUs`(기본 120 ms)보다 늦게 도착한 이벤트는 **버린다.** 오래된 이벤트를
  나중에 몰아서 재생하지 않는다.
- 연속 이벤트를 고정 cooldown으로 합치지 않는다. 대신 새 충격 직전에 `DuckActive()`로
  남은 잔향만 줄여 첫 순간을 살린다.
- 겹침은 `voiceLimit`(기본 12)로 상한을 두고, 초과 시 가장 오래된 잔향부터 버린다.
  믹스는 채널별로 ±1.0에 clamp한다.
- 객체 교체·읽기 실패·종료 시 `DropPending()`으로 대기 중인 출력을 버리고 라우팅을 반납한다.

`SekiroPlayerGuardReader`는 Python 레코더가 겪은 두 버그의 수정을 그대로 담고 있다
(DEFLECT_STATUS.md 7.8): **PlayerIns를 매 틱 다시 읽어 바뀌면 캐시를 통째로 버리고**,
모듈은 **`SprjPlayerDamageModule`을 함께 가진 컨테이너에서만** 채택한다. vptr 검사만으로는
해제된 객체를 걸러내지 못한다.

## 6. 아직 확인되지 않은 것

- **내장 스피커 출력 경로 미확정.** 채널 0·1은 무음이었고, 어느 `--audio-path` 값이
  내장 스피커에 도달하는지 아직 확인하지 못했다. 현재 소리는 액추에이터(채널 2·3)로도
  들리지만 그것은 **"내장 스피커 출력"이 아니라 액추에이터에서 새어 나오는 소리**다.
- 실제 게임 연동(`--live`) 실측 미실시.
- 다른 게임 진동/Steam Input과의 간섭 미확인.
- 적응형 트리거는 이번 범위에서 제외.

---

## 7. 소리 끊김 / 노이즈의 원인과 수정 (2026-09-19)

### 7.1 무엇이 원인이 **아니었는지** (코드에서 확인)

계측 전에 코드에서 배제한 후보들:

- **이벤트마다 장치를 열고 닫지 않는다.** `--live`, `--play`, `--audio-diag` 모두
  `DualSenseAudioDevice::Open()`과 `IAudioClient::Start()`를 루프 진입 **전에 한 번씩만**
  호출한다. 이벤트는 기존 렌더링 스트림 위에 voice를 얹을 뿐이고, `Stop`/`Reset`/재초기화는
  종료 시점에만 일어난다.
- **버퍼 경계에서 읽기 위치가 초기화되거나 건너뛰지 않는다.** `AudioVoice::position`은
  `Pump()` 안에서 프레임당 한 번만 증가하고, WASAPI 버퍼 경계와 아무 관계가 없다.
- **4채널 공용 정규화·리미터는 존재하지 않는다.** `Pump()`의 클램프는 채널별이고
  (`src/DualSenseAudioDevice.cpp`), 게인은 voice별이다. 진동이 세져도 스피커 샘플값은
  수치적으로 변하지 않는다. 클리핑 카운터도 speaker / haptic을 따로 센다.
- **햅틱 게인이 중복 적용되고 있지 않다.** 체인은
  `normalizePeak(0.30) × profileGain(1.0) × hapticStrength(1.0) × voiceGain(1.0)` = 0.30이고,
  `--audio-diag`가 이 곱을 그대로 출력한다. 렌더된 WAV의 실측 peak도 정확히 0.300이다.

### 7.2 실제 원인 — 버퍼 기아 (실측)

`--audio-diag`의 첫 실행 결과:

```
buffer   : 1056 frames (22 ms)
feed gap : worst 31.002 ms   <-- LONGER THAN THE BUFFER
padding  : zero-while-sounding = 0~2   (phase 2, phase 4)
```

31 ms는 Windows 기본 타이머 간격 15.6 ms의 정확히 2배다. 공급 스레드가
`sleep_for(1ms)`로 돌고 있었고, 1 ms 요청은 실제로 다음 15.6 ms 틱에서 깨어난다.
22 ms 버퍼를 31 ms마다 채우면 매번 말라붙는다. 이것이 끊김의 직접 원인이다.

원래 코드에서는 여기에 더해 `Pump()`가 **탐지 루프와 같은 스레드**에 있었다.
그 루프는 `ReadProcessMemory` 그래프 탐색(최대 2만 객체)과 콘솔 출력을 수행하며,
같은 세션에서 `worst poll interval = 24.5 ms`가 관측됐다. 즉 게임 연동 중에는
공급 간격이 타이머 문제와 무관하게도 버퍼를 넘길 수 있었다.

### 7.3 수정

1. **렌더 스레드 분리** — `StartRenderThread()`. 탐지 스레드와 mutex 하나만 공유하고,
   `ReadProcessMemory`도 콘솔 출력도 하지 않는다. `THREAD_PRIORITY_TIME_CRITICAL`.
2. **이벤트 구동 WASAPI** — `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetEventHandle()`.
   오디오 엔진이 직접 깨우므로 시스템 타이머에 의존하지 않는다. 실패 시에는 타이머
   폴백으로 내려가되, 그 사실을 `feeder :` 줄에 **명시**한다(조용히 넘어가지 않는다).
3. **리트리거 덕킹을 페이드로** — 이전에는 울리는 꼬리의 진폭에 0.45를 한 샘플 만에
   곱했다. 파형의 계단 = 클릭이다. 이제 `duck`이 `duckTarget`까지 샘플당
   `duckStep`씩 걸어간다(기본 5 ms, `--duck-fade-ms`). **새 충돌은 이 페이드를 기다리지
   않는다** — 같은 틱에 별도 voice로 큐잉된다.
4. **버퍼 크기를 인자로** — `--audio-buffer-ms`(기본 20 ms). 늘리면 기아 여유가 커지고
   지연도 같이 커지므로, 두 값을 항상 함께 출력한다
   (`buffer : ... ms   worst-case added latency ... ms`).

수정 후 같은 측정:

```
feeder   : event-driven (the audio engine wakes the feeder)
feed gap : worst 10.85 ms   (under the buffer)
padding  : zero-while-sounding = 0   (네 단계 전부, 빠른 연타 포함)
clipped  : speaker=0  haptic=0
```

> `padding == 0`을 곧바로 underrun으로 세지 않는다. 무음 대기 중에는 항상 0이다.
> `zero-while-sounding`과 `zero-while-silent`를 따로 센다.

### 7.4 왜곡(음색 깨짐)에 대해 지금까지 말할 수 있는 것

- **최종 제출 PCM은 클리핑하지 않는다.** 네 단계 전부 `clipped samples: speaker=0`.
  스피커 클립의 digital peak는 0.92(-0.72 dBFS)다.
- 따라서 남은 깨짐이 있다면 그것은 **디지털 클리핑이 아니라 그 이후 단계**
  (컨트롤러 preamp `speakerPreGain=7`, 스피커 볼륨 바이트 0x64, 소형 드라이버 자체)다.
  `--speaker-trim-db -6` / `-12`가 이 둘을 가르는 테스트다. 낮췄을 때 깨짐이 줄면
  증폭/드라이버 쪽, 그대로면 신호 자체 문제다.
- `--speaker-highpass` / `--speaker-lowpass`는 **스피커 클립에만** 적용된다. 햅틱
  파형에는 절대 적용되지 않는다(같은 대역에 살고 있으므로 적용하면 실험 대상 자체가 바뀐다).
  필터는 음량도 바꾸므로 전후 peak/RMS와 dB 차이를 함께 출력한다.
- 이 중 어느 것도 "컨트롤러가 재생할 수 있는 주파수 한계"에 대한 주장이 아니다.
  비교해 볼 후보일 뿐이다.

### 7.5 촉감 — 몸통 보강 (A / B / C)

기존 프리셋의 문제는 코드에서 확인된다: `LayerAt()`의 감쇠가
`exp(-decayPerSecond * local)`로 **레이어 시작 즉시** 시작했다. "몸통"이라고 이름 붙은
레이어도 t=0부터 죽기 시작한다. 실측(5 ms 버킷 최대 진폭):

```
deflect A: 0-5ms 0.300 | 5-20ms 0.142 | 20-40ms 0.034 | 40-70ms 0.009
```

첫 순간 외에는 남는 게 없다. 그래서 `HapticLayer`에 `holdMs`를 추가했다 —
상승 후 그 세기를 유지하다가 감쇠가 시작된다. `holdMs = 0`이면 기존 동작과 완전히 같고,
그래서 변형 A는 예전에 만졌던 파형 그대로다.

| | 길이 | peak | RMS | 0-5ms | 5-20ms | 20-40ms | 40-70ms |
|---|---|---|---|---|---|---|---|
| deflect A | 71 ms | 0.300 | 0.0499 | 0.300 | 0.142 | 0.034 | 0.009 |
| deflect B | 85 ms | 0.300 | 0.1115 | 0.273 | 0.300 | 0.264 | 0.029 |
| deflect C | 85 ms | 0.450 | 0.1673 | 0.410 | 0.450 | 0.396 | 0.043 |
| block A | 92 ms | 0.300 | 0.0897 | 0.300 | 0.267 | 0.153 | 0.068 |
| block B | 105 ms | 0.300 | 0.0951 | 0.193 | 0.300 | 0.235 | 0.122 |
| block C | 105 ms | 0.450 | 0.1426 | 0.290 | 0.450 | 0.352 | 0.183 |

같은 peak 0.30에서 deflect의 RMS가 **+6.4 dB**. 이것이 "허전함"에 대한 수치적 변화다.

**0.30과 0.45는 디지털 진폭이다.** 물리적 힘의 백분율이 아니고, 145 Hz에서 액추에이터
출력이 이 값에 선형으로 비례한다는 보장도 없다.

두 번 때리는 느낌인지 확인: 8 ms 이동 RMS 포락선에서 첫 피크 이후 55% 아래로 내려갔다가
80% 위로 다시 올라오는 지점을 찾았다. **A/B/C 여섯 파형 전부 해당 없음** — 한 충돌은
한 번의 타격으로 남는다.

### 7.6 새 명령

```powershell
$E = ".\build-hw\apps\guard_feedback\sekiro_guard_feedback.exe"

# 출력 경로 진단: 스피커만 / 햅틱만 / 동시 / 연타, 단계마다 계측
& $E --audio-diag --haptic-variant b

# 스피커 레벨만 비교 (햅틱 무관)
& $E --play deflect --count 1 --mode speaker --speaker-trim-db -6

# 촉감만 A / B / C
& $E --play alternate --count 4 --mode haptic --haptic-variant b

# 버퍼를 늘려 보기 (지연도 같이 출력된다)
& $E --audio-diag --audio-buffer-ms 40
```

---

## 8. 겹침 정책 — 새 이벤트는 이전 소리를 건드리지 않는다 (2026-09-19)

### 8.1 바뀐 규칙

이전에는 새 이벤트마다 `DuckActive(retriggerDuck=0.45)`를 호출해 울리고 있던 꼬리를
줄였다. 이제 **정상 이벤트는 아무것도 줄이지 않는다.** voice를 하나 더 추가할 뿐이고,
이전 소리는 자기 길이대로 끝난다.

- 각 voice는 **자기 재생 위치(`position`)와 자기 수명**을 가진다. 공용 샘플 커서는
  존재하지 않으며, 새 voice가 다른 voice의 위치를 되돌리거나 자르는 경로도 없다.
- `IAudioClient::Stop()`/`Reset()`은 `Close()`와 소멸자에만 있다. 이벤트 경로에는 없다.
- `DropPending()`은 종료·장치 상실·플레이어 객체 교체에서만 호출된다.
- 스피커와 햅틱은 **서로 다른 voice**다. 짧은 햅틱이 끝난다고 음원이 끝나지 않는다.

`retriggerDuck`은 설정 파일에 남아 있지만 **새 이벤트에는 적용되지 않는다**(기본값 1.0).
명시적 정지에만 쓰인다.

### 8.2 voice 한도

한 이벤트 = voice 3개(스피커 1 + 햅틱 좌/우 2). 가장 긴 클립이 ~105 ms이므로
50 ms 연타에서도 동시 6개, 30 ms 연타에서도 11개다. soft cap을 24로 두어
**정상적인 연속 패링은 한도 근처에도 가지 않는다.** 한도를 넘으면:

1. soft cap(24)에서 **가장 남은 게 적은** 꼬리 하나를 `retireFadeMs`(25 ms)로 **페이드**.
   "가장 오래된"이 아니라 "가장 조용하고 거의 끝난" 것을 고른다 — 늦게 시작한 짧은
   햅틱보다 먼저 시작한 긴 음원이 더 남아 있을 수 있기 때문이다.
2. hard cap(soft+8 = 32)에서만 실제로 제거한다. 여기까지 오는 건 버스트/버그이고,
   `retiredAtCap`과 `hardDropped`를 따로 세서 둘을 혼동하지 않는다.

### 8.3 합산 클리핑 — 리미터, voice 개수 나누기 아님

채널별로 고정 헤드룸(스피커 0.85) → 스무딩된 게인 감쇠(threshold 0.95, attack 1 ms,
release 120 ms) → 0 지연 소프트 니(tanh) 순서다.

**활성 voice 개수로 나누지 않는다.** 게인은 합이 실제로 threshold를 넘을 때만 움직이므로,
두 번째 타격이 시작됐다는 이유로 첫 번째가 조용해지지 않는다. 스피커와 햅틱은 각자
독립된 리미터 상태를 갖는다.

### 8.4 실측 (`--overlap-test`, 제출 직전 믹스 기준)

| 단계 | 동시 voice | 끝까지 재생 | 페이드/삭제 | 중간 공백 | 합산 peak(리미터 전) | 리미터 | 클리핑 |
|---|---|---|---|---|---|---|---|
| 1 단발 | 3 | 3/3 | 0 / 0 | 0 | 0.782 | 없음 | 0 |
| 2 400 ms x4 | 3 | 12/12 | 0 / 0 | 0 (타격 사이 공백만) | 0.782 | 없음 | 0 |
| 3 200 ms x4 | 3 | 12/12 | 0 / 0 | 0 | 0.782 | 없음 | 0 |
| 4 100 ms x6 | 3 | 18/18 | 0 / 0 | 0 | 0.782 | 없음 | 0 |
| 5 혼합 150 ms x6 | 3 | 18/18 | 0 / 0 | 0 | 0.782 | 없음 | 0 |
| 6 **50 ms x8** | **6** | 24/24 | 0 / 0 | **0** | 1.0007 | -0.02 dB | 0 |
| 7 **혼합 30 ms x10** | **11** | 30/30 | 0 / 0 | **0** | 1.700 | -1.85 dB | 0 |

주의: 1~5단계는 간격이 전부 스피커 클립(71 ms)보다 길어서 **실제로는 겹치지 않는다**
(`maxConcurrent=3`). 겹침 정책이 실제로 시험되는 건 6·7단계뿐이고, 거기서도
**잘린 voice 0개, 중간 공백 0개**다.

**짧은 햅틱이 음원을 끊지 않는다는 직접 증거**: 스피커 320 ms / 햅틱 85 ms 조합으로
단발 재생 → 제출 믹스의 스피커 채널이 **317.2 ms 동안 공백 없이** 울렸다.

### 8.5 확인된 제약 — 겹칠 잔향이 애초에 없다

`audio/sekiro_deflect.mp3`에는 충돌이 **5개** 있고(42 / 110 / 215 / 312 / 376 ms),
첫 충돌과 다음 충돌 간격이 68 ms다. 그래서 `--extract-hit`은 길이를 얼마로 요청하든
**71 ms로 줄인다** — 더 길게 자르면 두 번째 타격이 들어온다. 즉 현재
`deflect_hit.wav`에는 남길 잔향 자체가 없고, 실제 연속 패링 간격(≥250 ms)에서는
겹칠 일이 없다.

잔향이 있는 대안: **마지막 충돌**(뒤에 아무것도 없다)을 쓰면 된다. 실측 감쇠는
0 → -6.4 → -8.5 → -11.0 → -14.8 dB / 320 ms로, 한 번의 타격이고 꼬리가 살아 있다.

```powershell
# 이미 audio/generated/deflect_hit_long.wav 로 만들어 두었다
& $E --extract-hit audio/sekiro_deflect.mp3 --onset 4 --hit-length-ms 320 `
     --out audio/generated/deflect_hit_long.wav
# config의 deflect.speaker.clipPath 를 이 파일로 바꿔서 A/B
```

이건 소리의 성격을 바꾸는 변경이라 기본 설정은 **건드리지 않았다.**

### 8.6 명령

```powershell
$E = ".\build-hw\apps\guard_feedback\sekiro_guard_feedback.exe"

# 단발 -> 400/200/100 ms 반복 -> 혼합 -> 50/30 ms 실제 겹침
& $E --overlap-test

# 제출 직전 믹스를 4채널 WAV로 덤프 (음원 중단 vs 버퍼 기아 구분용)
& $E --overlap-test --dump-mix audio/mix
```

---

## 9. 기본값 조정 — 소리 -18 dB, 촉감을 길고 세게 (2026-09-19)

요청: 소리는 훨씬 작게, 진동은 오디오 길이에 맞춰 길게 + 세게.

### 9.1 소리

`speaker.trimDb = -18`을 두 프로필 기본값에 넣었다(처음 -4로 내렸다가, 여전히 크다는
피드백을 받아 -18로). **스피커 클립에만** 적용되는 트림이라 햅틱 채널, 컨트롤러 볼륨
바이트(0x64), Windows 엔드포인트 레벨은 전부 그대로다 — 진동 세기는 영향받지 않는다.
실측: `peak 0.92 -> 0.1158, rms -18.0 dB`(이전 진폭의 약 1/8).
더 조절하려면 `--speaker-trim-db`(예: `-12`면 더 크게, `-24`면 더 작게).

### 9.2 촉감

| | 길이 | peak | RMS | 0-5ms | 5-20 | 20-50 | 50-90 | 90-140 | 140-170 |
|---|---|---|---|---|---|---|---|---|---|
| deflect 이전 | 71 ms | 0.300 | 0.0499 | 0.300 | 0.142 | 0.034 | 0.006 | 0 | 0 |
| **deflect 현재** | **140 ms** | **0.550** | **0.2007** | 0.460 | 0.550 | 0.499 | 0.361 | 0.060 | 0 |
| block 이전 | 92 ms | 0.300 | 0.0897 | 0.300 | 0.267 | 0.153 | 0.051 | 0.001 | 0 |
| **block 현재** | **170 ms** | **0.550** | **0.1873** | 0.348 | 0.550 | 0.439 | 0.366 | 0.230 | 0.038 |

deflect RMS **+12.1 dB**, block **+6.4 dB**. 이중 타격 검사(8 ms 이동 RMS 포락선에서
첫 피크 이후 55% 아래 → 80% 위 재상승)는 둘 다 해당 없음.

**촉감이 소리보다 오래 간다**(140 ms vs 71.7 ms, 170 ms vs 78 ms). 의도한 것이다 —
두 출력은 각자 voice이고 서로의 수명을 끊지 않는다. 0.55는 디지털 진폭이지 힘의
백분율이 아니다.

`--haptic-variant a`는 이전의 짧고 약한 72/92 ms · peak 0.30 프리셋을 그대로 재현하므로
비교용으로 계속 쓸 수 있다. 아무 것도 주지 않으면 **설정 파일 값을 그대로** 쓴다.

### 9.3 겹침 재확인 (`--overlap-test`, 새 값)

7단계 전부 `playedToTheEnd` 100%, `retiredAtCap=0`, `hardDropped=0`, `clipped=0`.
리미터는 현실적인 간격에서 전혀 개입하지 않고, 30 ms 극단 연타(동시 voice 15개)에서만
햅틱 -0.92 dB / 스피커 -0.05 dB 움직였다. soft cap 24까지 여유가 남는다.

---

## 10. 출력 길이를 원본 녹음에 맞춤 (2026-09-19)

### 10.1 왜 71 ms였는가

`--extract-hit`은 다음 충돌이 섞이지 않도록 길이를 자동으로 줄인다.
`sekiro_deflect.mp3`의 첫 충돌 뒤 68 ms에 두 번째가 오므로 71 ms로 잘렸다.
**두 녹음 모두 마지막 충돌 뒤에는 아무것도 없다** — 거기가 온전한 잔향이 있는 곳이다.

| 녹음 | 충돌 위치 | 마지막 충돌의 꼬리 |
|---|---|---|
| `sekiro_deflect.mp3` | 42 / 110 / 215 / 312 / **376** ms | **378 ms**, 0 → -14.9 dB 단조 감쇠 |
| `sekiro_parry.mp3` | 20 / 120 / 211 / 330 / 418 / **495** ms | 1096 ms, 단 760 ms 이후는 -20 dB 이하 |

### 10.2 현재 값

- `deflect_hit.wav` ← `sekiro_deflect.mp3 --onset 4`, **378 ms**(자연 길이 전체), fade 40 ms
- `block_hit.wav` ← `sekiro_parry.mp3 --onset 5`, **500 ms**, fade 60 ms
  - 1096 ms 전체를 쓰지 않은 이유: 500 ms 이후 구간이 -11~-20 dB로 내려가며
    단조롭지 않게 출렁인다(150-250 ms 구간에서 한 번 되올라온다). 방어 한 번에
    1초를 쓰면 전투 중 계속 겹친다. 전체를 원하면
    `--hit-length-ms 1096`으로 다시 자르면 된다.
- 햅틱 `totalMs`를 **각자의 스피커 클립과 같게** 맞췄다(378 / 500 ms).
  늘어난 길이는 **전부 ring 레이어**이고 contact·body는 그대로다 — 세기가 있는
  앞부분은 건드리지 않고 꼬리만 길어진다.

실측 (20 ms 구간별 최대 진폭):

```
deflect SPEAKER  378ms peak 0.920 | 0.831 0.920 0.439 0.267 0.181 0.162
deflect HAPTIC   378ms peak 0.550 | 0.550 0.499 0.253 0.055 0.027 0.011
block   SPEAKER  500ms peak 0.920 | 0.920 0.612 0.598 0.631 0.410 0.430 0.214
block   HAPTIC   500ms peak 0.550 | 0.550 0.439 0.366 0.063 0.036 0.018 0.009
                                    0-20  20-60 60-120 120-200 200-300 300-400 400-500 ms
```

길이는 같고, 촉감은 120 ms 안에 대부분 끝나고 나머지는 희미한 여운이다.
소리는 끝까지 들린다.

### 10.3 voice 한도 상향

클립이 71 ms에서 378/500 ms로 길어지면서 겹치는 수가 크게 늘었다.
30 ms 극단 연타(10회)에서 동시 voice가 27개까지 올라가 기존 soft cap 24에서
6개가 페이드로 은퇴했다. **정상 연속 패링이 한도에 걸리면 안 된다**는 원칙에 따라
`voiceLimit`을 36(hard 44)으로 올렸다. 재측정: 동시 30개, `retiredAtCap=0`.

### 10.4 `--overlap-test` 재측정

7단계 전부 `playedToTheEnd` 100%, `retiredAtCap=0`, `hardDropped=0`,
`clipped=0`, 중간 공백 0. 리미터는 30 ms 극단에서만 햅틱 -1.06 dB 움직였고
스피커는 전 구간 0 dB(트림 -18 dB 덕분에 여유가 많다).


---

## 11. 진동 세기 상향 — peak 0.85 (2026-09-19)

`normalizePeak` 0.55 → **0.85**. 원래 0.30 기준으로 **약 +9 dB**.

| | 길이 | peak | RMS |
|---|---|---|---|
| deflect (원래) | 71 ms | 0.300 | 0.0499 |
| deflect (현재) | 378 ms | **0.850** | **0.1908** |
| block (원래) | 92 ms | 0.300 | 0.0897 |
| block (현재) | 500 ms | **0.850** | **0.1707** |

0.85는 **디지털 진폭**이지 힘의 백분율이 아니다. 액추에이터 출력은 이 값에 선형으로
비례하지 않고, 어느 지점부터는 더 세게 미는 대신 덜그럭거린다.

### 리미터 개입 (`--overlap-test` 실측)

| 반복 간격 | 햅틱 합산 peak | 리미터 |
|---|---|---|
| 단발 | 0.850 | 없음 |
| 400 ms | 0.850 | 없음 |
| 200 ms | 0.868 | 없음 |
| 100 ms | 0.889 | 없음 |
| 혼합 150 ms | 0.906 | 없음 |
| 50 ms | 1.030 | -0.36 dB |
| 30 ms | 1.894 | -3.76 dB |

**100 ms 반복까지 리미터가 전혀 개입하지 않는다** — 현실적인 연속 패링 간격에서는
0.85가 그대로 나간다. 50/30 ms는 일부러 만든 극단 테스트다.
전 구간 클리핑 0, `retiredAtCap=0`, `hardDropped=0`.

### 더 세게 하고 싶다면

게인을 더 올리는 건 권하지 않는다(1.0에서 클리핑, 그 전에 액추에이터가 덜그럭거린다).
남은 실질적인 방법은 **파형**이다 — 보이스 코일은 낮은 주파수에서 더 묵직하게 느껴지므로
`body.startHz`를 deflect 190 → ~150, block 145 → ~110으로 내리는 쪽이
같은 진폭에서 더 세게 느껴진다. 이건 소리의 성격과도 맞물리므로 기본값은 바꾸지 않았다.

### 테스트 수정

`GuardCue_ContactLayerGlidesWithoutADiscontinuity`가 실패했다. 이 검사는 샘플 간 최대
변화량을 **절대값 0.10**으로 제한했는데, 그 값은 `normalizePeak`이 0.30이던 시절에
맞춘 것이다. 세기를 올리면 같은 파형의 변화량도 함께 커져서 아무 문제가 없는데도 걸린다.

고친 방식:
- 한계를 **peak 대비 비율**로 바꿨다. 48 kHz에서 260 Hz 사인의 샘플당 최대 변화는
  peak의 0.034배이므로 0.06배로 잡았다.
- 글라이드를 측정할 때 **노이즈 그레인을 끈다.** 화이트 노이즈는 샘플 간에 자기 진폭의
  두 배까지 튀므로, 켜 둔 채 재면 이 테스트가 이름에 내건 스윕이 아니라 그레인을 잰다.
- 그레인을 켠 전체 큐에 대해서도 peak 대비 0.25배 한계를 따로 둬서, 실제로 잘린 파형이
  생기면 여전히 잡히게 했다.

---

## 12. 적 측 가드 감지 (2026-09-19)

### 12.1 근거

`SprjChrActionFlagModule`은 정적 vftable 맵에 **단 하나**(RVA 0x2A72158)뿐이라
`+0x3C`/`+0xE10`은 캐릭터 종류와 무관한 필드다. 라이브 확인 결과
(`DEFLECT_STATUS.md` 8절): 적을 공격한 60초 동안 19건, BLOCK/DEFLECT가
화면과 같은 순서로 교대, 나머지 143개 객체는 오탐 0건.

네거티브 컨트롤(공격하지 않고 이동, 주변 적들끼리 교전): **60초간 0건**.
단, 그 구간의 최악 폴링 간격이 33.2 ms로 한 프레임을 넘어 **놓친 구간이 있을 수
있다.** 그리고 0건은 "적끼리는 신호가 안 난다"는 증거이기도 하지만 "그 적들이
애초에 가드를 하지 않았다"는 뜻일 수도 있다. 둘은 아직 구분되지 않았다.

### 12.2 구현

`SekiroEnemyGuardReader` — 플레이어 리더의 대칭. 소유권 판정만
`SprjPlayerDamageModule` → `SprjEnemyDamageModule`로 바뀌고, 다수결과
vptr 재검증은 동일하다. 적 목록 오프셋은 이 빌드에서 확인된 바 없으므로
**타입으로 찾는다**(WorldChrMan에서 `EnemyIns` vptr 탐색). 느리지만 오프셋을
잘못 가정하지 않는다.

- 탐색(`Discover`)은 7.3초. 폴링 루프 밖에서 돌고, `--enemy-rediscover`(기본 6초)
  간격으로 `NeedsDiscovery()`가 참일 때만 다시 돈다.
- 폴링은 캐릭터당 **펄스 1바이트만** 읽고, 펄스가 0이 아닐 때만 결과 바이트를
  읽는다. 전부 읽으면 148개 × 2회 = 폴당 296회 ReadProcessMemory가 되어
  한 프레임을 넘긴다.
- 적마다 **독립된 `GuardOutcomeEventDetector`**. 공용으로 쓰면 두 전투가
  하나의 시퀀스로 섞인다.

```powershell
& $E --live --pid (Get-Process sekiro).Id --enemy
```

기본은 꺼져 있다. 신호가 말하는 건 "이 캐릭터가 가드했다"이지
"내 공격을 가드했다"가 아니기 때문이다.

### 12.3 이 과정에서 발견해 고친 실제 버그 2개

**(1) 렌더 스레드 스핀 — 20초에 펌프 1억 2766만 회.**
`--live` 루프에 `device.Pump()` 호출이 남아 있었다. 렌더 스레드를 도입하기 전의
잔재로, 이제는 두 스레드가 같은 버퍼를 두드리고 있었다. `TIME_CRITICAL` 스레드가
초당 630만 회 도는 상태로 게임 옆에서 돌아가고 있었다 — 오디오는 정상으로 보였지만
(`framesSubmitted`는 정확) CPU를 태우고 있었다. 메인 루프의 `Pump()`를 제거.

**(2) MinGW `std::this_thread::sleep_for`는 `timeBeginPeriod`를 따르지 않는다.**
(1)을 고치자 폴링 간격이 5 ms → **15.6 ms**로 뛰었다(`missedSlots=1700`).
winpthreads의 sleep_for는 500 µs 요청에 **즉시 반환**하고(이게 스핀의 원인이었다)
1 ms 요청에는 **15.6 ms 틱 전체**를 잔다. `timeBeginPeriod(1)`은 이 구현에 영향을
주지 않는다. Win32 `::Sleep`으로 교체하니 mean 5008 µs로 복귀.

수정 후: `pumps=1533`(15초), `poll interval mean=5008us`,
`feed gap worst 12.2 ms`, `zero-while-sounding=0`.

### 12.4 "적 블록이 인식될 때도 있고 안 될 때도 있다"의 원인

`NeedsDiscovery()`는 **WorldChrMan이나 PlayerIns가 바뀔 때만** 참이다 —
즉 지역 이동이나 사망뿐이다. 재탐색을 여기에 걸어 놨기 때문에, 한 지역에 머무는 동안
**시작 시점에 찾은 적 목록이 그대로 얼어붙었다.** 나중에 로드된 적, 또는 탐색 예산
(40,000 노드) 안에 들어오지 못한 적은 끝까지 추적되지 않는다. 어떤 적은 되고 어떤 적은
안 되는 증상이 정확히 이것이다.

단순히 매번 재탐색하게 바꾸면 더 나빠진다. 실측:

```
poll interval: mean=4195780us worst=5615787us missedSlots=3353
```

탐색 한 번이 5.7초인데 그걸 폴링 스레드에서 돌리니 **폴링 간격이 5 ms → 4.2초**가 됐다.
이 상태에서는 플레이어 감지까지 죽는다.

**해결**: 탐색을 전용 스레드로 옮기고(`StartBackgroundDiscovery`), 완성된 목록만
락 아래에서 교체한다. 탐색 중에도 기존 목록으로 계속 폴링하므로 눈먼 구간이 없다.
재탐색에서 같은 캐릭터·같은 모듈이 다시 나오면 `lastOutcome`과 generation을
이어받는다 — 그러지 않으면 재탐색이 가짜 엣지를 만든다.

수정 후 (25초, 재탐색 4초 간격):
```
poll interval: mean=5005us worst=34409us missedSlots=5
enemy: tracked=149 discoveries=4 dropped=0
  resolves: fast=3 slow=593   lastDiscovery=5621 ms
feed gap: worst 13.2 ms
```

남은 한계:
- 탐색이 여전히 5.6초다. `fast=3 / slow=593`이라 학습한 오프셋 경로가 거의 안 먹는다 —
  캐릭터 클래스마다 모듈 컨테이너 위치가 다른 것으로 보인다. 지금은 백그라운드라
  감지를 막지는 않지만, 약 10초에 5.6초는 한 코어를 쓴다.
- 따라서 **새로 로드된 적은 최대 ~10초 뒤에 추적**된다. 전투 도중 합류한 적이
  잠깐 인식되지 않을 수 있다.
- 진짜 해법은 WorldChrMan의 적 리스트 오프셋을 찾아 그래프 탐색을 없애는 것이다.
  아직 이 빌드에서 확인되지 않았다.

### 12.5 적 모듈 경로 — 두 번의 오답과 현재 상태

| 방식 | 결과 |
|---|---|
| 그래프 2홉 + EnemyDamage 소유권 | 이벤트는 나지만 캐릭터 104개 → 모듈 75개, **24개 공유** |
| 오프셋 쌍 하나(Python 보정, `ChrIns+0x1B58 -> +0xD0`) | 공유 0, **난전 60초 이벤트 0** |
| 오프셋 쌍 하나(C++ 런타임 보정) | `ChrIns+0x1FF8 -> container+0x0`, 154개 추적, 공유 0, 탐색 1.3초 |

C++과 Python이 **서로 다른 쌍**으로 수렴했다. 보정은 표본 40개에서 가장 많은 캐릭터가
지지하는 쌍을 고르는데, 표본에 어떤 캐릭터가 들어오느냐에 따라 답이 갈린다는 뜻이다.
C++ 쪽은 `support 40/40`(표본 전부 지지)이라 구조적으로는 더 강해 보이지만,
**펄스와 대조한 행동 검증은 아직 없다.**

행동 검증용 도구: `tools/enemy_module_truth.py` — 소유권을 전혀 가정하지 않고
모든 ActionFlagModule(149개)을 감시해서 **실제로 펄스가 난** 모듈만 역추적한다.

### 12.6 내가 만든 회귀 — 방어음에 충돌이 4개 (2026-09-19)

10절에서 "원본 오디오 길이에 맞추라"는 요청에 따라 마지막 onset을 길게 잘랐는데,
**그 뒤에 더 약한 타격이 더 있었다.** onset 목록은 임계값 기반이라 약한 타격을
놓친다. 실측:

```
block_hit.wav  500ms -> 4 impacts at 3 / 74 / 186 / 276 ms
deflect_hit.wav 378ms -> 2 impacts at 2 / 70 ms
```

한 번의 방어에 클랭이 4번 났다. `--live` 실행 시 도구가 스스로 경고를 찍고 있었다
(`WARNING: ... contains 4 impacts`).

두 녹음 모두 다음 타격이 **약 66~70 ms 뒤**에 온다. 길이별 실측:

```
deflect  80ms -> 2 / 71ms -> 2 / 66ms -> 1
block    80ms -> 2 / 71ms -> 2 / 66ms -> 1
```

그래서 둘 다 **66 ms**로 되돌렸다. 이 녹음들에는 잔향이 긴 단일 타격이 **없다** —
이건 소스의 한계이지 튜닝으로 넘길 수 있는 문제가 아니다.

결과적으로 소리 66 ms / 촉감 378·500 ms로 **다시 길이가 벌어졌다.** 10절의 요청과
어긋나지만, 대안은 한 번의 방어에 클랭을 4번 내는 것뿐이다. 촉감을 소리에 맞춰
짧게 하려면 `config/guard_cues.json`의 `haptic.totalMs`를 줄이면 된다.

### 12.7 경로를 플레이어에서 유도하도록 변경 (최종)

표본 기반 보정은 **운에 좌우됐다.** 같은 게임에서 두 번 돌렸는데 서로 다른 쌍이 나왔다:

```
C++ 보정 (표본 40)      -> ChrIns+0x1FF8 -> container+0x0   support 40/40
Python 보정 (표본 40)   -> ChrIns+0x1B58 -> container+0xD0  support 23/50
```

그리고 그 쌍은 **빌드 상수가 아니다** — 실행 사이에 바뀌었다. 하드코딩도 답이 아니다.

**해결: 정답을 아는 유일한 캐릭터에서 유도한다.**

플레이어의 모듈은 이미 검증된 방법으로 구할 수 있다(ActionFlagModule과
`SprjPlayerDamageModule`을 함께 들고 있는 컨테이너). 플레이어가 하나뿐이라 2홉이
이웃으로 새지 않으므로 적에게는 쓸 수 없는 이 방법이 여기서는 안전하다.

1. 플레이어 모듈을 그 방법으로 확정한다.
2. PlayerIns에서 **그 모듈에 도달하는 모든** `(o, j)` 쌍을 모은다.
3. 그중 적을 **가장 많이, 전부 서로 다른 모듈로** 해석하는 쌍을 고른다.
   하나라도 공유가 생기는 쌍은 점수 0 — 공유는 누군가에게는 틀렸다는 뜻이다.

교차 검증 (게임 플레이 불필요):
```
player module (proven walk) = 0x7ff4b037fd30
  ChrIns+0x1FF8/+0x0 : 0x7ff4b037fd30  -> MATCH
```

적용 결과:
```
enemy reader: tracking 151 character(s) (discovery took 203 ms)
path: ChrIns+0x1ff8 -> container+0x0 (support 151)  ambiguousDropped=0
lastDiscovery=134 ms
```

151개 전부 서로 다른 모듈, 공유 0. 탐색은 5.7초 → 1.3초 → **134 ms**.
6초마다 다시 유도하므로 레이아웃이 바뀌어도 따라간다.

**아직 검증되지 않은 것**: 이 경로가 "내가 때리는 그 적"의 모듈을 잡는다는 것은
구조적으로만 확인됐다(플레이어 정답 일치 + 151/151 고유). 실제 전투에서 내 공격과
적끼리의 싸움이 구분되는지는 행동 검증이 남아 있다. 사용자는 "정확해 보인다"고
보고했으나 그건 인상이지 대조 측정이 아니다. 대조가 필요하면
`tools/enemy_module_truth.py`가 그 용도다.

---

## 13. 잔향 합성 — 66 ms가 잘려 들리던 문제 (2026-09-19)

### 13.1 원인은 드롭아웃이 아니었다

`--live` 실행 중 오디오 경로를 계측한 결과:

```
feeder   : event-driven
feed gap : worst 10.9 ms   (버퍼 22 ms 미만)
padding  : zero-while-sounding=0
clipped  : speaker=0  haptic=0
```

기아도 클리핑도 없다. "잘려 들린다"는 것은 **클립이 실제로 66 ms로 잘려 있었기**
때문이다(12.6절). 소스에 긴 단일 타격이 없어 어쩔 수 없이 자른 것이고, 잘린 게
들리는 건 당연하다.

### 13.2 해결: 클립 자신의 재료로 잔향을 만든다

`ExtendDecayTail()` — 클립의 **후반 1/3**(어택이 아닌 링 구간)에서 그레인을 떼어
크로스페이드하고 감쇠 포락선을 씌워 뒤에 붙인다. 어택 구간을 쓰지 않으므로 두 번째
타격이 생기지 않는다. 기본값: deflect 66 + 240 ms, block 66 + 320 ms.

### 13.3 네 번 틀렸고 매번 계측이 잡았다

| 시도 | 실패한 이유 (실측) |
|---|---|
| 그레인에 `exp(-decay·t)` 곱하기 | 랜덤 오프셋 그레인은 무상관이라 Hann 크로스페이드가 상수로 합쳐지지 않는다. 버킷 피크가 `0.00297 -> 0.00547` 상승 |
| 러닝 **max** 포락선으로 나누기 | 러닝 max는 지연된다. 피크가 창을 벗어날 때 오히려 꺼졌다 |
| 블록 **mean** 포락선 강제 | 피크 단조성이 깨져 유닛 테스트 실패, 접합부 단차 2.67배 |
| 클립 끝 **피크**에 앵커 | 링의 peak/mean 비가 2~3배라 잔향의 mean이 녹음보다 커졌다 (`0.0051 -> 0.0137`) |

그리고 가장 큰 것 — **첫 그레인이 페이드 인하고 있었다.** Hann의 상승 반쪽이 0에서
시작하므로 잔향이 그레인 절반(~9 ms) 동안 올라오며 **접합부에 구멍**이 생겼다.
실측에서 `0.0156 -> 0.0075 -> 0.0200`으로 푹 꺼졌다 올라왔다. 첫 그레인의 상승
반쪽만 창을 씌우지 않도록 고쳤다.

또 `ApplyFadeOut`이 66 ms 클립을 0까지 페이드한 뒤 잔향이 원래 레벨에서 시작해서
접합부에 실제 재어택이 있었다. 잔향이 있을 때는 클립 페이드를 걸지 않는다(잔향이
자기 끝 페이드를 가진다). 추출 시 `--fade-ms`도 12 → 1로 줄였다.

최종 (블록별 피크를 지수 곡선에 올리고, 앵커는 클립 끝 mean에서 유도):
```
접합부 5 ms 포락선: 0.0190 0.0146 0.0135 0.0156 0.0119 0.0139 0.0117 0.0086 ...
최대 상승폭: x1.44   (이전 x3.37)
WARNING: 없음
```

### 13.4 온셋 경고의 오검출도 같이 고쳤다

`FindOnsets`는 포락선이 임계값 위에 있는 동안 `separation`마다 국소 최대를 하나씩
보고한다. 녹음에서 타격을 찾는 데는 맞지만, **감쇠하는 잔향**도 임계값 위에 한동안
머물면 "impact"로 세어진다. 그래서 단조 감쇠가 보장된 잔향에도
`contains 2 impacts`가 찍혔다.

`KeepAttacks()`를 추가해 **포락선이 실제로 상승해 들어온 온셋만** 남긴다
(15 ms 전 대비 1.6배). 진짜 두 번째 타격은 상승하고, 감쇠하는 잔향은 상승할 수 없다.
검증: **알려진 4-impact 클립은 여전히 4개로 잡히고**, 현재 클립은 1개다.

### 13.5 측정 임계값도 고쳤다

`MeasureChannel`이 절대값 0.002를 쓰고 있었다. 트림 -18 dB에 감쇠 잔향이 붙으니
마지막 수십 ms가 그 밑으로 떨어져 **완전히 재생된 큐를 "SHORT: a cue ended before
its clip did"로 보고**했다. 채널 자신의 피크 대비 1%로 바꿨다. 이후 7단계 전부
`OK: nothing was cut short`, 단발 공백 0.

### 13.6 현재 값

```
deflect speaker 306 ms (66 녹음 + 240 잔향)   haptic 378 ms
block   speaker 386 ms (66 녹음 + 320 잔향)   haptic 500 ms
```

조절: `config/guard_cues.json`의 `tailMs`, `tailDecayPerSecond`(13/10), `tailGrainMs`.
`tailMs: 0`이면 잔향 없이 66 ms 녹음만 나간다.

---

## 14. 검증 상태 (2026-09-20 기준)

이 절은 **어느 빌드에서 무엇이 확인됐는지**를 구분한다. 소스에 들어 있다는 것과
실행됐다는 것은 다르고, 이 저장소에서는 실제로 어긋나 있다.

### 14.1 실기에서 확인된 것 — 2026-09-19 14:31 빌드

이 빌드로 관찰된 사용자 보고:

| 관찰 | 의미 |
|---|---|
| "오디오는 잘 적용됐어" | 잔향 합성(deflect 386 ms, block 486 ms, 감쇠 4.0/3.2)이 들을 만하다 |
| "적이 막은거 인지는 잘 해" / "정확도도 꽤나 정확해 보여" | 적 가드 감지가 동작한다 |
| "초반에 안되다가 갑자기 되네" | 6초 주기 재탐색이 경로를 뒤늦게 고쳐 준다 |
| "표창을 던져 막아도 소리 들려" | `+0xE10`은 "이 캐릭터가 가드했다"만 말한다 (12절의 미검증 항목이 확인된 것) |

자동 검증: 유닛/통합 테스트 4/4 통과, `--overlap-test` 7단계 전부
`playedToTheEnd` 100% / 중간 공백 0 / 클리핑 0.

### 14.2 소스에는 있으나 **한 번도 실행되지 않은 것**

14:31 이후의 변경은 전부 링크가 막힌 채 소스만 커밋됐다. 실행 중이던
`sekiro_guard_feedback.exe`(pid 5392)가 exe를 잡고 있어
`cannot open output file ... Permission denied`로 세 번 연속 실패했고,
그 사실을 알아채지 못한 채 "다음 실행에서 확인하자"로 넘어갔다.

따라서 **아래 셋은 컴파일만 통과했고 동작은 미확인이다**:

1. **적 재탐색 주기 6초 → 2초** (`Options::enemyRediscoverSeconds`)
   "초반에 안되다가 갑자기 되는" 증상을 겨냥한 변경. 탐색이 ~134 ms이므로
   6초를 기다릴 이유가 없다는 근거는 측정됐지만, 증상이 실제로 사라지는지는 미확인.
2. **`rawPulseEdges` / `nonZeroPulseSamples` / `pathChanges` 카운터**
   "리더가 펄스를 못 보는가" 와 "보는데 detector가 이벤트를 안 내는가"를 가르기 위한
   계측. 이 숫자를 한 번도 읽어 본 적이 없다.
3. **`clipRaw` 원본 재생 모드** (`SpeakerCueProfile::clipRaw`)
   녹음을 자르지도 정규화하지도 않고 내보낸다. 기본값은 꺼져 있다.
   켜면 원본 파일의 충돌 5~6개가 한 이벤트에 전부 재생된다 — 의도된 동작이지만
   들어 본 적은 없다.

### 14.3 다음에 할 일

```powershell
# 실행 중인 인스턴스를 먼저 종료해야 링크가 된다
cmake --build build-hw
.\build-hw\apps\guard_feedback\sekiro_guard_feedback.exe --live --pid (Get-Process sekiro).Id --enemy --live-seconds 60
```

세션 요약의 이 줄이 위 1·2번을 동시에 판정한다:

```
enemy: deflect=? block=?  tracked=? rawPulseEdges=? nonZeroSamples=?
    pathChanges=? ambiguousDropped=? lastDiscovery=? ms
```

- `rawPulseEdges = 0` → 주소 해석이 틀렸다. 19건을 잡았던 그래프 탐색 방식으로
  되돌리되, 모듈 중복은 하나만 남기는 식으로 처리한다.
- `rawPulseEdges > 0` 인데 이벤트 0 → detector 쪽 문제다.
- `pathChanges = 0` → 경로가 처음부터 맞았다. 1 이상이면 재탐색이 고쳐 준 것이고,
  그렇다면 2초 주기가 "초반에 안 되는" 구간을 얼마나 줄이는지가 다음 질문이다.

### 14.4 아직 신호 자체가 답하지 못하는 것

표창·폭죽처럼 칼이 아닌 공격, 적끼리의 교전, 여러 적 중 누구를 때렸는지 —
`+0xE10` 하나로는 구분할 수 없다. 구분하려면 새 신호가 필요하다:
적이 가드한 공격의 출처, 또는 플레이어의 공격 상태 플래그와의 시간 상관.
어느 쪽도 아직 조사하지 않았다.
