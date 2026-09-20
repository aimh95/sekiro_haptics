# 먼저 만들어야 하는 것: 플레이어 튕겨내기와 일반 방어 감지

2026-09-11 · 현재 상태: 신호 탐색/평가 도구 구현, 실제 Sekiro 판정 신호 미확정.

**개발 순서는 실제 게임 신호 확인 → 패링/방어 구분 → 연속 성공 및 오검출 검증 → 진동/스피커 연결이다.** 출력 명령을 수동으로 실행하는 기능은 이미 구현했지만, 그것으로 패링 감지 개발이 완료된 것은 아니다. 이 문서의 순서가 출력 안내 문서의 실행 순서보다 우선한다.

## 정확히 무엇을 찾아야 하나

우선 일반 방어와 튕겨내기가 갈라지는 **게임 결과 처리**를 조사한다. 외부 읽기로 관측 가능한 결과 기록이 있으면, 방어자가 플레이어인지와 각 충돌을 어떻게 구별하는지 확인한다. 지속되는 결과 기록이 없다면 플레이어 반응 애니메이션/상태를 후보로 조사한다. 같은 애니메이션의 연속 실행을 구별할 수 있어야 한다.

가드 버튼 입력, 튕겨내기 가능 시간창, HP 무변화, 체간 변화는 성공 판정 자체가 아니다. ‘적이 내 공격을 튕김’도 플레이어 성공 이벤트가 아니다. `PerfectDeflect`라는 별도의 게임 필드가 존재한다고 가정하지 않는다.

지금 저장소에는 **확정된 결과 필드, 플레이어 반응 객체까지의 포인터 경로, 검증된 애니메이션 ID가 없다.** `GameDataMan → PlayerGameData`와 HP/체간 오프셋은 기존 가설이다. 그 영역에 성공 판정이 있을 것이라고 보장할 근거도 없다. `target-resolve` 역시 WorldChrMan의 루트만 얻는 코드다. 이를 성공 판정 주소로 해석하면 안 된다.

실제 EXE/자산에서 호출 경로를 조사할 때 [DSAnimStudio](https://github.com/Meowmaritus/DSAnimStudio)는 Sekiro 애니메이션/TAE 조사에 활용할 수 있다. [fromsoftware-rs의 Sekiro 구조체 목록](https://github.com/vswarte/fromsoftware-rs/tree/main/crates/sekiro/src/sprj)은 구조체 조사 출발점이며, 검증된 Deflect detector를 제공한다는 뜻은 아니다. 공개 도구를 읽은 것과 사용자의 `sekiro.exe`를 역분석한 것은 구분한다. 이번 작업에서 후자는 수행하지 못했다.

## 이번에 실제로 추가한 코드

`tools/deflect_signal.py`는 기존 v3 raw capture를 읽고 **작은 필드 후보를 찾고 반증하는 오프라인 도구**다. `discover`는 후보를 고르는 자료만 읽고, `evaluate`는 고정한 후보 하나를 별도 세션에 적용한다. 새 프레임워크나 게임 주입 모듈은 추가하지 않았다.

| 기능 | 실제 동작 |
|---|---|
| 패링과 방어 후보를 각각 조사 | `--target player_deflect` 또는 `player_block` |
| 상태/enum 후보 | 지정한 raw 값으로 **진입할 때**만 후보 발생. 시작 시 이미 그 값이면 발생하지 않음 |
| 카운터 후보 | 관측된 unsigned 증가량이 정확히 1일 때만 발생. wrap 포함. 2 이상 증가/감소는 별도 진단이며 중간 성공을 복원하지 않음 |
| 원시 필드 | little-endian 1/2/4바이트. 기본은 앞 4096바이트 안의 정렬된 4바이트 필드. 특정 offset과 폭으로 좁힐 수 있음 |
| 일반 방어·적 패링·허공 방어·피격 | 서로 다른 음성 조건으로 오검출 집계. 방어 버튼 marker로 시도를 만들지 않음 |
| 연속 패링 | 고정 cooldown 없이 발생 후보를 개별 집계. 한 정답과 여러 예측의 중복은 FP로 계산 |
| 데이터 손실 | gap, 실패 후 rebaseline, 객체 세대 변경, 과도하게 긴 성공 읽기 간격을 가로지르는 시도 전체 제외 |
| 겹치는 시도 | 양쪽 모두 분자/분모에서 제외. 제외 사유와 정답 수를 보고 |
| 별도 검증 | 발견 세션 ID 또는 같은 capture SHA를 재사용하면 거부. 빌드/관측 영역/자료 종류 일치 요구 |
| 결과 | 항상 `candidate_only`, `hapticOutputEnabled: false`. 통계가 좋아도 자동 승격하지 않음 |

상태값이 계속 유지되는 동안 실제 패링이 세 번 있었다면, 값 진입 방식은 한 번만 예측하고 나머지 두 번은 FN이다. 샘플 사이에서 같은 상태로 재진입했다가 되돌아온 경우도 복원할 수 없다. 그런 후보로 반복 패링 지원을 완료했다고 주장하면 안 된다. 여러 결과가 섞인 카운터라면 타입/주체 정보도 추가로 필요하다.

## 게임 없이 먼저 도구 실행

Python 3.10 이상, 추가 패키지 없음. 프로젝트 루트에서 실행한다. 출력 디렉터리와 출력 JSON은 새 이름이어야 한다.

```powershell
python tests/deflect_signal_fixture.py synthetic-signal-demo
python tools/deflect_signal.py discover synthetic-signal-demo/synthetic-discovery.manifest.json --target player_deflect --offset 0 --tolerance-us 5000 --output synthetic-signal-demo/deflect-candidates.json
python tools/deflect_signal.py evaluate synthetic-signal-demo/synthetic-heldout.manifest.json --candidates synthetic-signal-demo/deflect-candidates.json --index 0 --output synthetic-signal-demo/deflect-evaluation.json
python tools/deflect_signal.py discover synthetic-signal-demo/synthetic-discovery.manifest.json --target player_block --offset 0 --tolerance-us 5000 --output synthetic-signal-demo/block-candidates.json
python tests/test_deflect_signal.py -v
```

**여기의 900000001 등 모든 값, 카운터, 주소와 시각은 합성 fixture다. 실제 Sekiro 값으로 복사하지 않는다.** 테스트는 도구가 자료를 제대로 평가하는지 검사하며 게임 인식률을 측정하지 않는다.

## 실제 자료를 준비하는 순서

### 1. 빌드와 조사 대상을 고정한다

사용자 PC의 실제 `sekiro.exe` SHA-256, 파일 크기, 설치된 모드 목록을 기록한다. 기존 코드에 적힌 해시를 복사하지 않는다. DLL 전체가 아니라 EXE의 전투 결과 분기와 플레이어 반응 데이터부터 좁힌다. 정적 분석 자료는 별도 폴더에 두며 게임 파일/메모리/세이브를 수정하지 않는다.

```powershell
Get-FileHash -Algorithm SHA256 "<실제 게임 설치 폴더>\sekiro.exe"
```

필요한 산출물은 결과 분기의 RVA/관련 코드 또는 반응 ID 후보, 플레이어 소유 근거, 관측 포인터 경로, 값의 수명, 연속 발생 구분 근거다. 이 자료나 실제 바이너리가 없으면 아직 신호 발견 단계다. 모르는 offset을 임의로 채워 `지원됨`으로 만들지 않는다.

### 2. 읽기 전용 probe를 빌드하고 후보 주변을 짧게 기록한다

Windows C++ 빌드 도구, Windows SDK, CMake가 필요하다. HID 출력은 이 단계의 의존성이 아니다.

```powershell
cmake -S . -B build-probe -A x64 -DSEKIRO_HAPTICS_BUILD_DUALSENSE_TRANSPORT=OFF
cmake --build build-probe --config Release --target sekiro_haptics_signal_probe
.\build-probe\apps\sekiro_signal_probe\Release\sekiro_haptics_signal_probe.exe --process-name sekiro.exe --output captures
```

콘솔에서 `identity`로 실제 빌드를 확인한다. 근거 있는 후보 주소가 확보된 경우에만 다음을 실행한다. 아래 주소 칸은 실제 관측 주소로 바꿔야 하며 제공된 확정 주소가 아니다.

```text
combat-capture custom-address <확인한-현재-주소> 256 5
combat-mark sync_start
...
combat-stop
combat-export
quit
```

`custom-address`는 **고정 주소**라서 객체 교체를 자동 추적하지 못한다. 짧은 한 장면으로 제한하고 사망/로드/워프 전에 종료한다. 실제 detector 연결 전에는 확인한 플레이어 포인터 경로를 매번 재검증하는 reader가 필요하다. 읽기 성공만으로 플레이어 소유권이나 동일 객체를 보장하지 않는다. 지원되지 않은 빌드의 hash gate를 해제하는 방법으로 진행하지 않는다.

기존 `combat-resolve` / `combat-capture player-game-data 256 5`도 가설 영역 관측용이다. 여기에 좋은 후보가 없다는 사실만으로 실제 결과 경로가 없다고 결론 내리지 않는다.

### 3. 독립 영상으로 정답을 만든다

각 세션에서 플레이어 패링, 일반 방어, 적이 내 공격을 튕김, 허공 방어, 피격을 포함한다. 빠른 연속 패링은 각 충돌 시각을 따로 표시한다. 결과를 구별하기 어려운 장면은 `ambiguous`로 남긴다. 초기 pilot의 예로 조건별 20회 정도를 수집할 수 있지만, 이는 아직 실행한 수치도 승인 기준도 아니다.

동영상과 capture의 단조 시계를 동기화할 수 있는 장면을 기록한다. 예를 들어 영상에 probe 콘솔의 `sync_start` 처리와 timestamp를 함께 보여 주고 두 시계의 대응 및 오차를 적는다. **단순히 F5를 누른 시각을 패링 충돌 시각으로 사용하지 않는다.** 시간 오차가 큰 자료로 낮은 지연을 주장하지 않는다.

캡처마다 별도 JSON manifest를 만든다. 아래 형식은 값의 설명을 위한 예시이며 유효한 실제 자료가 아니다. `startUs`와 `endUs`는 capture 단조 시계의 반열린 구간 `[startUs, endUs)`다. 모든 `eventTimesUs`도 같은 시계로 변환한다. 전후 baseline 관측을 포함하도록 캡처하고, 서로 겹치지 않는 시도 구간을 잡는다.

```json
{
  "schemaVersion": 1,
  "evidenceKind": "live_review",
  "exeSha256": "실제_EXE의_소문자_SHA256_64자리",
  "regionKey": "확인한_포인터_경로와_객체_레이아웃_리비전",
  "sessions": [{
    "sessionId": "새로운_플레이_세션_ID",
    "capture": "session-folder/combat_capture.jsonl",
    "captureSha256": "이_capture_파일의_소문자_SHA256_64자리",
    "annotationSource": "independent_video_review",
    "annotationEvidence": "영상 파일, capture↔영상 시계 대응, 판독자/판독 방법",
    "syncUncertaintyUs": 16667,
    "trials": [
      {"id":"p1","label":"player_deflect","startUs":1000000,"endUs":1400000,"eventTimesUs":[1200000]},
      {"id":"b1","label":"player_block","startUs":2000000,"endUs":2400000,"eventTimesUs":[2200000]},
      {"id":"e1","label":"enemy_deflect","startUs":3000000,"endUs":3400000,"eventTimesUs":[]},
      {"id":"g1","label":"empty_guard","startUs":4000000,"endUs":4400000,"eventTimesUs":[]},
      {"id":"d1","label":"damage","startUs":5000000,"endUs":5400000,"eventTimesUs":[]}
    ]
  }]
}
```

파일 해시와 선언된 세션 ID로 명백한 자료 재사용을 막지만, 이 도구가 영상 내용을 보거나 session/build/region 선언의 진위를 검증하지는 않는다. 원본 영상·probe의 `session.json`·분석 manifest를 함께 보관한다. regionKey는 ASLR로 바뀌는 절대주소 대신 같은 객체/경로/필드 배치를 뜻해야 한다.

### 4. 발견과 검증을 분리한다

발견 manifest에 `discover --target player_deflect`와 `discover --target player_block`을 각각 실행한다. 관심 offset을 알면 `--offset 0x...`로 좁힌다. 초기 탐색 결과는 다수 비교 중 잘 맞은 가설일 수 있다. 최상위 후보라는 이유로 게임 의미를 확정하지 않는다.

선택한 후보, 분석 읽기 간격 한계와 매칭 허용치를 고정한 다음 **새로운 플레이 세션**을 수집해 `evaluate`한다. 같은 세션의 다른 부분을 validation이라고 부르지 않는다. held-out 결과를 보고 규칙을 수정했다면 그 자료는 개발 자료가 되었으므로 최종 검증 세션이 다시 필요하다.

TP/FP/FN, 중복, 조건별 정답/예측 대응, 제외 구간, `unscoredDetections`, `counterJumpsNotExpanded`를 함께 본다. 정답이 없는 구간의 이벤트는 오검출이 없다고 간주하지 않고 미평가로 남긴다. `observedDelayUs`는 관측 시각과 영상 annotation 시각의 차이며 전체 게임→패드 지연 측정값이 아니다. 낮은 표본 수의 높은 수치는 보장으로 바꾸지 않는다.

## 다음 live capture 전 필수 / 미룰 것

**필수:** 이번 파일 보존·객체 전환 baseline·실패/gap 기록 수정본 사용, Windows 읽기/종료 경로 확인, 현재 EXE 지문, 근거 있는 관측 영역과 그 수명, 독립 영상 및 시계 동기화. 후보가 없으면 실제 EXE/반응 데이터 조사부터 진행한다.

**자동 피드백 연결 전 필수:** 플레이어/적 구분, 일반 가드 음성 검증, 동일 상태의 반복 발생 구분, 로드/재접속 시 baseline 재설정, 실제 별도 세션의 오검출·누락 보고. 부족한 후보는 출력에 연결하지 않는다.

**미뤄도 됨:** 진동 강도 튜닝, 의수/와이어 신호 확장, GUI, 기존 스캐너/설정 형식 전면 정리, Bluetooth, 고해상도 햅틱. 현재 출력 코드를 다시 작성할 필요는 없다.

현재 이 환경에는 Windows 세키로 프로세스, 실제 게임 바이너리, 독립 영상이 있는 라이브 capture가 없다. 따라서 이번 완료 범위는 **관측 신뢰성 수정 및 후보 발견·평가 구현**이고, 실제 게임 자동 감지는 완료되지 않았다.
