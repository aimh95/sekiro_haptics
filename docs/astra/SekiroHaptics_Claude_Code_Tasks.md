# SekiroHaptics — Claude Code 구현 작업서

기준: 2026-09-09 독립 감사 · 판정 B · 이 문서는 구현 지시서이며, 이번 감사에서는 저장소 코드를 수정하지 않았다.

## 공통 실행 계약

이 작업서는 처음부터 다시 설계하라는 요청이 아니다. 아래 작업을 순서대로 수행하고, 이미 검증된 Replay·Mapping·Preset·프로세스 읽기 seam·Legacy rumble을 보존한다. 한 작업을 마칠 때 변경 파일, 실제 실행한 명령·결과, 사용 fixture, 남은 제약을 보고한다. 다른 작업까지 묶은 대규모 리팩터링은 하지 않는다.

Claude Code에는 이 공통 계약과 해당 Txx 작업을 함께 전달한다. 앞선 작업의 산출물을 입력으로 사용하되, 완료 보고만 믿지 말고 그 작업에 필요한 테스트와 artifact를 확인한다. 저장소 전체 감사를 다시 시작하지 않는다. 사용자의 미커밋 변경을 보존하고, 현재 트리가 감사 시점과 다르면 해당 작업의 영향 범위만 확인한다.

감사 HEAD는 e80de4065a48fb961d186192355a72515877e24b다. 그러나 실제 감사 대상은 미커밋 변경 14개 파일과 신규 5개 파일을 포함한 ZIP 작업 트리다. HEAD만 checkout해 감사 결과를 재현하려 해서는 안 된다.

기본 제약은 C++20, 읽기 전용 외부 관찰, 지원 build에서만 해석, ambiguous/invalid 상태의 fail-closed다. WriteProcessMemory, VM_WRITE, VM_OPERATION, VirtualAllocEx, CreateRemoteThread, 프로세스 상태·세이브·Steam 변경, DLL 주입은 범위 밖이다. 새로운 실게임 주소·offset·signature·animation ID를 추측하지 않는다. 5 ms 설정을 실제 성공 간격이나 지연 보장으로 간주하지 않는다.

**이 감사에서 실제 실행한 수치는 공통 494/494, Linux 2/5, 전체 496/499다.** 실패 3개는 이 환경에서 process_vm_readv(self)=EPERM, 직접 실행한 자식의 maps 읽기=EACCES라는 제약과 함께 기록됐다. Windows/HIDAPI 전용 테스트·실게임 검출·물리 장치는 이번 실행의 증거가 아니다.

### 증거 분류를 유지할 것

| 분류 | 이미 확보한 근거 | 구현자가 해서는 안 되는 해석 |
|---|---|---|
| Reproduced bug / loss | R1 stale pointer, R2 세대 간 delta, R3 덮어쓰기, R4 관측 누락, R5–R8 analyzer, R9 codec, R10 Reset 순서 | 단순 추정으로 격하하거나 실게임에서 발생한 것처럼 서술 |
| Code-level risk | probe의 detached worker가 stack controller/queue를 참조하고 종료 시 join하지 않음 | 실제 Windows crash를 재현했다고 주장 |
| Environment-related test failure | Linux 3개 실패와 EPERM/EACCES 진단 | 실패를 PASS로 바꾸거나 Windows 성공으로 확대 |
| Unverified Sekiro hypothesis | AOB root 의미, +0x8 관계, HP/MaxHP/Posture/MaxPosture 가설 | 범위 검사 통과만으로 게임 필드 검증 완료 처리 |
| Live-game evidence | 동봉 세션의 EXE 지문·후보 watch·미완료 scan metadata | marker 없는 후보 변화를 실제 combat detector 정확도로 사용 |

특히 R7의 정확한 범위는 **겹치는 두 trial에 같은 delta 하나가 각각 변화로 집계되는 것**이다. 원본 캡처에 delta가 두 번 쓰인다는 뜻이 아니다. R2는 **generation 변경 후 첫 baseline 읽기가 실패한 경우**다. 모든 generation 변경이 항상 잘못 비교되는 것은 아니다.

### 무엇을 다음 캡처 전에 끝내야 하는가

| 순서 | 작업 | 실게임 필요 | 다음 raw live capture 전 | 비고 |
|---|---|---|---|---|
| T01 | 캡처 소유권·파일 보존·종료 | 아니오. Windows 종료 smoke 필요 | **필수** | R3와 worker lifetime |
| T02 | pointer/epoch와 baseline 무효화 | 아니오 | **필수** | R1/R2, 이후 실게임 수명 검증 별도 |
| T03 | 재구성 가능한 observation 기록·replay | 아니오 | **필수** | R4/R9, 시간·gap·저장 실패 |
| T04 | 읽기 전용 Windows preflight·가설 출처 | 아니오. Windows helper 필요 | **필수** | 실제 read/query 권한과 build gate |
| T05 | 원본 입력·annotation·정답 분리 | 아니오 | **입력/전투 레이블을 수집하는 캡처에는 필수** | 단순 HP 관측은 독립 영상/수동 기록으로 대체 가능 |
| T06 | 실제 HP·수명 전환 대조 실험 | **예** | 다음 캡처 자체 | 첫 실게임 증거 생산 |
| T07 | analyzer 경계·중복·분모 수정 | 아니오 | 아니오 | 통계·검출 성능 판정 전 필수 |
| T08 | Block/Deflect/Damage 실제 corpus | **예** | 아니오 | T05/T07 이후 |
| T09 | raw 관측 기반 실제 detector | 구현·회귀는 오프라인 | 아니오 | T08 근거 필요 |
| T10 | 출력 Reset·dispatch·paced replay | 코드/Fake는 오프라인 | 아니오 | 실제 haptic 연결 전 필수 |
| T11 | 실제 end-to-end 검증 | **예, DualSense도 필요** | 아니오 | accuracy·latency·오류 동작 |
| T12 | 선택적 target 분리·중복 제거 | 아니오 | **미뤄도 됨** | 구조 변경 자체를 PoC gate로 두지 않음 |

**캡처 재개 gate는 T01–T04다.** guard_input이나 outcome annotation을 수집할 때는 T05도 완료한다. T07·T09·T10·T12를 끝내느라 신뢰할 수 있는 첫 HP 캡처를 지연할 필요는 없다. 다만 analyzer 수치를 합격 근거로 쓸 때는 T07, 실제 진동을 켤 때는 T10을 먼저 완료해야 한다.

현재 없거나 부정확한 문서 설명은 T03/T04 변경과 함께 수정한다. 별도 library 분리, JSON parser 교체, HapticEffectType 제거, 범용 profile 재설계, GUI·PCM·추가 게임은 캡처 재개의 조건이 아니다.

감사 보고서의 세 milestone과 대응시키면 M1은 T01–T06, M2는 T07–T09, M3는 T10–T11이다. T12는 제품 증거 뒤에 판단할 선택적 정리다. 이 작업서의 task 수를 별도의 추가 milestone 수로 해석하지 않는다.

## T01 — 캡처 소유권과 파일 보존을 먼저 보장한다

**Goal**

중복 시작·실패한 시작·종료 때문에 기존 캡처를 잃거나, 파괴된 객체에 worker가 접근하지 않게 한다.

**Why**

R3에서 같은 controller와 파일에 두 번 Start한 결과 둘 다 Started였고 첫 marker가 사라졌다. 하위 session의 AlreadyRunning 보호는 controller가 객체를 먼저 교체하면서 우회된다. probe의 detached worker 수명은 별도의 코드상 위험이다.

**Scope**

SekiroCombatSessionController::StartCapture/StopCapture, CaptureSession의 파일 열기·마감, CommandProcessor의 오류 결과, probe main의 stdin/hotkey/guard/sampler 소유·종료 경로.

**Constraints**

scanner·detector·haptic 구조를 바꾸지 않는다. 사용자 파일을 기본 truncate하지 않는다. 종료 해결을 위해 게임 프로세스를 종료하거나 강제 조작하지 않는다. jthread 도입만으로 blocking 입력 취소가 해결됐다고 보고하지 않는다.

**Required Implementation**

활성 세션이면 기존 객체·파일을 보존한 채 AlreadyRunning을 반환한다. 새 세션은 고유 capture ID와 충돌하지 않는 경로를 사용하며, 파일 충돌은 명시적으로 실패시킨다. 시작 실패가 기존 session을 파괴하지 않게 한다.

worker를 명시적으로 소유하고 StopRequested → 입력/메시지 대기 해제 → worker join → capture flush/close 결과 확인 → reader detach → 객체 파괴 순서를 만든다. sampler가 IProcessReader를 쓰는 동안 외부 detach가 실행되지 않게 한다. 반복 Stop은 안전해야 한다. shutdown 중 신규 명령·marker의 수용 정책을 정하고 거절 결과를 숨기지 않는다.

**작은 reference sketch — 이름은 기존 코드에 맞춰 조정 가능**

```cpp
class ProbeSessionOwner {
public:
    StartResult StartCapture(const CaptureRequest&);
    void RequestStop();       // wakes owned input/sampling loops
    StopResult JoinAndClose();// returns only after workers and sink stop
};
```

**Migration Strategy**

현재 controller/session을 유지하면서 active 검사와 파일 정책을 먼저 수정한다. main의 worker 집합만 작은 owner로 옮긴다. 기존 명령 이름을 유지하고 결과 메시지에 capture ID·경로를 명확히 한다.

**Tests**

R3를 올바른 기대값의 회귀로 만든다. double-start 시 첫 내용이 그대로이고 두 번째가 AlreadyRunning인지, stop/start가 새 파일을 만드는지, 경로 충돌·open 실패·flush 실패가 보고되는지 검사한다. controlled latch를 사용해 read 진행 중 종료의 순서를 확인한다. stdin 종료·Ctrl+C·quit·target 종료에 대한 Windows smoke 결과를 남긴다. 실제로 수행하지 못한 smoke는 NOT RUN으로 적는다.

**Fixtures**

기존 capture marker fixture, audit restart.jsonl의 재현 구성, 쓰기 실패를 유도하는 작은 sink seam, read를 명시적으로 지연시키는 Fake. 실제 게임 주소는 사용하지 않는다.

**Acceptance Criteria**

활성 파일의 bytes가 중복 시작 전후 동일하고, failed start가 기존 세션을 보존한다. stop 반환 시 모든 worker가 종료됐고 이후 read/write/callback이 없다. 반복 종료가 안전하며 저장 실패가 정상 종료로 표시되지 않는다. 변경 경로의 회귀 테스트가 통과한다.

**Out of Scope**

lossless frame 설계(T03), root 가설 검증, 파일 관리 GUI, logging framework, Scheduler thread 변경.

## T02 — 현재 객체 확인과 baseline 경계를 수정한다

**Goal**

이전 player 객체의 값이나 읽기 공백 전 값을 현재 객체의 변화로 잘못 해석하지 않게 한다.

**Why**

R1에서는 실제 참조 pointer가 바뀌어도 이전 readable 블록을 Valid로 읽었다. R2에서는 generation을 먼저 변경한 뒤 첫 baseline 읽기가 실패해, 다음 성공 read가 이전 객체와 비교됐다.

**Scope**

SekiroKnownRootResolver, SekiroRawCombatReader, controller의 마지막 resolve 상태, CaptureSession baseline-valid 상태와 generation 처리.

**Constraints**

새 주소·offset을 추측하지 않는다. 매 5 ms tick에 전체 AOB scan을 반복하지 않는다. pointer equality만으로 객체 identity를 증명하지 않는다. failure를 정상 0값으로 변환하지 않는다.

**Required Implementation**

검증된 module slot 해석과 현재 root/child pointer dereference를 분리한다. bounded field read 전후의 pointer 일관성을 확인하고, 바뀌었거나 확인할 수 없으면 관측을 invalid로 표시한다. 재연결·해석 실패·관측 공백에서 epoch와 검출 상태의 무효화 정책을 명시한다. 같은 주소가 재등장해도 공백 전 상태와 연속이라고 자동 간주하지 않는다.

baseline은 epoch 변경·unresolved·read 실패 시 무효화한다. 다음 성공 read는 baseline이며 delta가 아니다. bytes와 baseline epoch를 성공 시 함께 commit한다. generation 업데이트만 먼저 하고 이전 bytes를 남기는 경로를 제거한다.

**작은 reference sketch**

```cpp
if (boundaryChanged || readUnavailable) {
    baseline.reset();
    RecordBoundaryOrGap();
}
if (readSucceeded) {
    if (baseline && baseline->epoch == frame.epoch) Diff(*baseline, frame);
    baseline = MakeBaseline(frame); // success commits bytes and epoch together
}
```

이 코드는 흐름 설명이며 실제 read 실패 때 Diff가 호출되지 않는 분기와 pointer 일관성 검사는 구현에서 명확히 한다.

**Migration Strategy**

기존 AobScanner/RipRelative/Fake를 재사용한다. resolver의 public API는 가능하면 유지하고 current pointer/epoch 검증을 작은 내부 경계로 추가한다. 새 상태 필드를 구형 CombatSnapshot으로 노출하는 adapter를 두고 호출부를 단계적으로 옮긴다.

**Tests**

R1/R2를 회귀로 전환한다. root 교체, child 교체, 이전 객체가 계속 readable, 첫 새 baseline 실패 후 성공, null→same-address 복귀, detach/reattach, read 도중 pointer 교체, 정상 unchanged를 각각 검사한다. 모든 boundary에서 cross-object delta와 이벤트가 0이어야 한다.

**Fixtures**

기존 FakeProcessReader/FakeProcessInspector, audit generation_read_failure 구성, 두 개의 synthetic object block과 pointer chain. 실제 게임 layout의 의미를 증명하는 fixture로 부르지 않는다.

**Acceptance Criteria**

R1의 이전 객체 값이 유효한 현재 관측으로 반환되지 않는다. R2의 다음 성공 read는 baseline으로 처리돼 cross-generation delta가 없다. gap 뒤 같은 주소 복귀도 새 연속 구간으로 처리된다. AOB 재해석과 정상 sampling의 호출 횟수가 구분되고 전체 scan을 매 tick 돌리지 않는다.

**Out of Scope**

실게임에서 HP/Posture 의미 검증(T06), arbitrary pointer-chain interpreter, 모든 객체 lifetime을 외부 읽기로 완벽하게 판별한다는 보장, 새 signature 조사.

## T03 — 관측을 그대로 저장하고 같은 입력으로 재생한다

**Goal**

캡처를 읽으면 detector가 live에서 받은 관측의 값·순서·상태·시간·epoch를 동일하게 받을 수 있게 한다.

**Why**

R4는 성공한 unchanged sample 두 번이 파일에 아무것도 남기지 않음을 보였다. R9는 기존 v1 codec이 소수 정밀도와 nested snapshot을 잃음을 보였다. 현재 combat delta 파일은 legacy replay 입력이 아니다.

**Scope**

CombatSnapshot을 기반으로 한 작은 typed observation frame, capture sink/codec, 신규 replay reader. 구형 v1 codec과 기존 ReplaySignalSource는 호환 경로로 유지.

**Constraints**

전체 GameSignal API를 한 번에 바꾸지 않는다. std::variant·template·schema registry를 도입하기 위한 설계를 하지 않는다. 기존 v1 fixture에서 잃어버린 원본 정보를 추측해 채우지 않는다. 기존 JSON number의 double 경로를 임의 정수의 무손실 저장으로 간주하지 않는다.

**Required Implementation**

새 format kind와 버전을 명시한다. header에 capture ID, exact build identity, producer revision, hypothesis/layout ID, clock 기준을 기록한다. 각 관측에 sequence, source/subject/epoch, scheduled time, read start/end, status/reason, typed 값과 필요한 raw bytes를 남긴다. 초기는 작은 snapshot을 매 관측 쓰는 방식으로 충분하다.

시작 baseline, unchanged 성공 관측, 읽기 실패, missed slot 범위, epoch 변경, 종료·write 실패를 기록한다. successful read 수와 attempted read 수를 분리한다. latency 통계는 실제 read 시각을 기준으로 계산하고 파일 쓰기·mutex 대기 이전의 시각을 완료 시각처럼 쓰지 않는다.

저장 오류를 감지하고 해당 capture를 incomplete로 표시한다. strict reader는 unsupported schema·손상된 필수 record를 실패시킨다. 허용한 optional field 확장은 보존/무시 정책을 명시한다. 부분 파일 salvage는 명시적 개발 모드이며 합격 평가와 분리한다.

**작은 reference sketch**

```cpp
struct SekiroObservationFrame {
    uint64_t sequence, sourceId, subjectEpoch;
    int64_t scheduledUs, readStartUs, readEndUs;
    ObservationStatus status;
    CombatFields fields; // fixed typed values; validity is explicit
};
```

정확한 필드명·ID 타입은 기존 코드에 맞춰 최소화한다. 이 struct만으로 전체 header·gap·annotation schema가 정의됐다고 간주하지 않는다.

**Migration Strategy**

새 frame codec/source를 옆에 추가하고 기존 v1 tests는 그대로 통과시킨다. capture 조립부부터 새 frame을 쓰고, 이후 detector를 T09에서 연결한다. delta-only 파일은 과거 발견 자료로 남기며 완전한 observation corpus라고 부르지 않는다.

**Tests**

baseline-only, unchanged, same-timestamp sequence, multi-field snapshot, 실패·누락·epoch 변경, 정확한 정수, 저장 오류, malformed/unsupported 버전의 round-trip을 검사한다. direct live feed를 Fake로 받은 frame 목록과 decode한 frame 목록을 내용까지 비교한다. 문자열 변환 결과를 다시 예상값으로 만드는 자기복제 테스트는 피한다.

**Fixtures**

R4/R9 입력, 기존 v1 fixtures 전체, 신규 minimal frame golden fixture, corrupted/truncated fixture, 연속 성공·gap·새 epoch의 짧은 trace.

**Acceptance Criteria**

정의된 frame 정보의 round-trip 동등성이 100%이며 unchanged 성공 관측도 복원된다. raw annotation은 detector feed에 섞이지 않는다. attempted/success/failed/missed가 구분된다. unsupported/손상 파일이 정상 완료로 처리되지 않는다. 기존 v1 테스트가 유지된다.

**Out of Scope**

전체 JSON parser 교체, 대용량 압축 최적화, 범용 multi-game schema, 실제 event detector, 대규모 async logging 인프라.

## T04 — 읽기 전용 Windows preflight와 가설 상태를 명확히 한다

**Goal**

실험에 쓰는 Windows read/query가 필요한 권한으로 동작하고, build identity와 미검증 field 의미가 혼동되지 않게 한다.

**Why**

현재 handle은 QUERY_LIMITED_INFORMATION | VM_READ지만 VirtualQueryEx의 문서 계약은 QUERY_INFORMATION을 요구한다. 파일 크기/hash에는 동봉 세션 근거가 있지만 AOB root·필드 의미에는 실게임 확인 근거가 없다.

**Scope**

Win32ProcessReader/Win32Api의 읽기·영역 조회 권한, OS helper 검증, 실험 가설 catalog/상태 표현, 관련 README·문서 설명.

**Constraints**

PROCESS_ALL_ACCESS·관리자 실행 강요·게임 메모리 쓰기·주입을 해결책으로 쓰지 않는다. 권한 추가는 필요한 조회 기능에 한정한다. 이미 명시적인 실험 명령을 쓰는 흐름에 매번 승인 UI를 추가하지 않는다. UNKNOWN 가설을 자동 production profile로 승격하지 않는다.

**Required Implementation**

VirtualQueryEx가 필요한 handle에는 QUERY_INFORMATION | VM_READ처럼 문서 계약에 맞는 최소 읽기 전용 권한을 적용하거나, 해당 기능을 별도 제한된 handle로 분리한다. 단순히 호출을 재시도하거나 모든 권한을 늘리지 않는다. forbidden bits가 없는지와 실제 helper read/query가 되는지를 함께 검증한다.

빌드 크기/hash는 기존 측정 자료와 연결한다. AOB·+0x8·HP/체간 offsets는 출처 UNKNOWN·unvalidated인 상태를 명시한다. read 성공, 정수 invariant 통과, semantic validation을 구분한다. 지원하지 않는 build는 scan/read를 시작하기 전에 실패시킨다. 원래 없다고 잘못 적힌 backend/hash/signature 설명과 누락 문서 참조를 현재 상태로 고친다.

**Migration Strategy**

IProcessReader와 Fake seam을 유지한다. 잘못 고정된 mask assertion만 의미 있는 권한 계약 검사로 바꾼다. SignatureProfile의 일반 기능을 재작성하지 말고, 이번에는 실험 가설의 상태·출처를 명시하는 정도로 제한한다.

**Tests**

Windows helper의 알려진 값 read, readable region query, inaccessible/exit 처리, unsupported build fail-closed, forbidden 권한 부재를 검사한다. Linux EACCES/EPERM을 PASS로 우회하지 않는다. 테스트 수는 실제 해당 플랫폼에서 실행한 수만 보고한다.

**Fixtures**

기존 Win32 helper/Fake와 synthetic identity. 실제 hash는 동봉 session metadata 출처를 적고, game field fixture는 합성임을 유지한다.

**Acceptance Criteria**

현재 Windows에서 helper read/query가 실제 통과하고 종료·실패가 명확히 처리된다. 쓰기·주입 권한이나 API가 추가되지 않는다. unsupported build가 해석 전에 거절된다. 문서와 결과에 “범위 유효”와 “게임 의미 검증”이 분리된다. Windows 실행 환경이 없으면 해당 gate는 BLOCKED/NOT RUN으로 남긴다.

**Out of Scope**

실제 Sekiro field 검증, signature를 새로 수집하는 작업, 모든 가설의 production profile 이전, Windows와 Linux 프로세스 구현의 통합 리팩터링.

근거: [VirtualQueryEx](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex), [프로세스 접근 권한](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights).

## T05 — 원본 입력과 사람의 정답을 별도 기록한다

**Goal**

실제 빠른 입력을 debounce로 잃지 않고, guard 입력 시각을 공격 충돌 시각으로 오해하거나 사람 label이 detector 입력으로 새지 않게 한다.

**Why**

현재 guard_input의 debounce와 analyzer pairing은 빠른 연속 입력·held guard·guard 없는 damage를 왜곡할 수 있다. 입력 시각과 processing 시각을 구분한 기존 개선은 유지할 가치가 있다.

**Scope**

controller guard watch, MonotonicDebounce/RisingEdgeDetector 적용 지점, marker record, trial annotation schema와 capture-to-detector adapter.

**Constraints**

XInputGetState를 game rumble 관측이라고 부르지 않는다. guard onset=contact라는 가정을 넣지 않는다. UI marker 중복 억제와 raw input edge 보존을 구별한다. 영상 판독을 수행하지 않았다면 영상 기반 정답을 만들지 않는다.

**Required Implementation**

원본 button rising/falling edge를 보존하고 hardcoded 120 ms 억제를 raw stream에서 제거한다. 필요한 UI 중복 억제는 별도 경로에 둔다. 각 source의 관찰 시각, 처리 시각, gap을 보존한다.

trial ID, contact 시각/구간, label과 label uncertainty를 annotation으로 표현한다. held guard와 다중 충돌, guard 없는 damage/idle 대조도 나타낼 수 있게 한다. capture 컨테이너에 함께 저장하더라도 record kind와 feed 경계로 detector가 annotation을 받지 않도록 한다.

**Migration Strategy**

기존 marker 명령을 유지하고 새 필드를 additive하게 도입한다. 구형 guard_input marker는 입력 관찰로만 변환하며 정확한 contact label로 승격하지 않는다. 수동 HP 대조를 영상으로만 기록한다면 그 경로의 annotation 방법을 문서화한다.

**Tests**

120 ms 이내 press-release-press, long hold, input/processed 시각 차이, 여러 contact에 대응하는 한 guard, guard 없는 damage, same timestamp ordering, annotation 제거 후 raw detector feed 동등성을 검사한다.

**Fixtures**

합성 button edge sequence, delayed command, held guard와 두 contact, unguarded damage, 별도 annotation 파일. 실제 trial 데이터는 T06/T08에서 생성한다.

**Acceptance Criteria**

원본 edge가 UI debounce 때문에 사라지지 않는다. guard 없이도 outcome과 음성 대조를 기록할 수 있다. 사람 label·manual event 이름이 raw detector 입력에 전달되지 않는다. 시각 불확실성이 저장되고 contact와 guard 시각이 구별된다.

**Out of Scope**

자동 영상 인식, game rumble interception, 주입·hook, 통계 분석(T07), 실제 label 수집(T06/T08).

## T06 — 실제 HP와 객체 수명 전환을 대조한다

**Goal**

현재 후보가 정말 player HP인지, 정상적인 게임 수명 전환 뒤에도 현재 객체를 읽는지 첫 실게임 증거를 만든다. **실제 Sekiro가 필요한 작업이다.**

**Why**

동봉 EXE 지문과 후보 watch만으로 field 의미를 확정할 수 없다. 제대로 된 player 관측이 없는 상태에서 Block/Deflect classifier를 만드는 것은 순서가 틀리다.

**Scope**

T01–T04의 capture를 사용한 짧은 통제 실험, 독립적인 화면/행동 annotation, replay 확인, 가설별 결과표. 입력 marker를 사용하면 T05도 선행한다.

**Constraints**

T01–T04 gate를 통과하기 전에는 새 유효 corpus를 수집했다고 하지 않는다. 프로세스 메모리·세이브를 수정하지 않고 일반 게임 플레이로 피해·회복·전환을 수행한다. 숫자 UI가 없으면 정확한 HP 수치까지 확인했다고 주장하지 않는다.

**Required Implementation**

실험 실행 순서와 capture manifest 템플릿을 준비한다. build identity, 도구 revision, hypothesis ID, scope, sampling 설정, 시간 동기화 방법, 정상/비정상 종료 상태를 남긴다. 동일 조건에서 분리된 피해 20회·회복 10회와 idle/menu 대조를 기록한다. 사망/부활·이동·저장 재로딩을 포함한 수명 전환을 최소 3회 관찰한다.

각 field를 독립 평가한다. HP가 맞아도 Posture까지 맞다고 하지 않는다. 실제 환경을 사용할 수 없으면 실행 준비만 완료하고 Live Validation=NOT RUN, milestone=BLOCKED로 보고한다. Fake 값을 실제 관측 칸에 넣지 않는다.

**Migration Strategy**

새 데이터는 T03 형식으로만 수집한다. 기존 marker 없는 watch는 참고 자료로 남기고 학습/평가 정답으로 합치지 않는다. 이번에 확인한 field만 검증 상태를 올리고 근거 capture ID와 연결한다.

**Tests**

기존 오프라인 회귀를 확인하고 실제 파일을 strict decode/replay한다. capture 중 실제 읽기 간격·read duration·실패·누락·epoch 전환을 검사한다. UI/영상과 field 변화 방향·가능한 척도를 대조한다. 게임 조작을 자동화하는 테스트 코드는 추가하지 않는다.

**Fixtures**

실제 raw capture, 독립 annotation/화면 기록, identity manifest, 사건별 결과표, lifecycle gap/epoch 구간. 개인정보나 불필요한 전체 프로세스 dump는 수집하지 않는다.

**Acceptance Criteria**

독립적으로 판정 가능한 모든 대조 trial에서 HP 후보가 일치하고 모순이 없다. 전체·유효·모호한 trial 수를 모두 적는다. 정상 메모리 읽기와 게임 의미 검증을 분리한다. 공백/객체 교체에서 가짜 변화가 없고 기록과 replay가 동등하다. 결과는 해당 build·조건에 한정한다. 불일치하면 field는 UNKNOWN이고 T08/T09의 검증된 입력으로 쓰지 않는다.

**Out of Scope**

Block/Deflect 분류 정확도 주장, 전 적·공격 일반화, Deathblow/Posture Break 검출, 실험 실패를 숨기는 새 offset 추측.

## T07 — analyzer의 경계·중복·분모를 고친다

**Goal**

offset 변화 집계가 객체 교체·관측 공백·겹친 trial 때문에 부풀려지지 않고, 계산할 수 없는 오탐률을 0으로 표시하지 않게 한다. 오프라인으로 수행한다.

**Why**

R5는 경계 이후 변화를 앞 trial에 집계하고 음성 분모 없이 FPR=0을 내는 경로, R6는 guard 없는 damage 제외, R7은 delta 하나를 겹친 두 trial에 각각 집계, R8은 malformed hex 수용을 재현했다.

**Scope**

SekiroCombatCaptureAnalyzer의 parser, trial 구성, 시간 창, change/pulse 계산, score/report. 필요하면 T03 frame reader를 공통 사용한다.

**Constraints**

기존 support/FPR 이름 아래 의미만 조용히 바꾸지 않는다. raw delta 중복 기록 결함으로 오진하지 않는다. ambiguous trial을 음성으로 취급하지 않는다. 매 offset의 변화 여부만으로 detector 정확도를 주장하지 않는다.

**Required Implementation**

format kind/schema, 정수 범위, hex 길이·문자, cell 크기를 검증한다. generation·source·capture를 보존하고 gap/epoch를 넘는 비교를 금지한다. contact trial을 guard 입력에 종속시키지 않는다. overlap이 있는 trial은 분할·독립성 판정 또는 ambiguous 제외 규칙을 명시하고, 같은 변화의 귀속·공유 여부를 드러낸다. 물리적으로 공유된 관측을 독립 표본처럼 세지 않는다.

분모 0은 N/A로 반환한다. block→deflect FP는 실제 유효 Block trial을 분모로 사용한다. 전체·유효·제외·unknown·coverage를 표시하고 train/holdout을 분리한다. A→B→A pulse의 첫 변화·복귀·유지 구간을 보존하며 gap 너머 지속 시간을 아는 것처럼 적지 않는다.

**작은 reference sketch**

```cpp
struct Rate {
    uint64_t numerator = 0, denominator = 0;
    std::optional<double> Value() const; // nullopt when denominator == 0
};
```

**Migration Strategy**

기존 analyzer를 수정하되 output schema/의미가 달라지면 version을 올린다. 구형 파일은 legacy exploratory 모드로 읽거나 명확히 거절한다. 기존 수치와 신규 성능 수치를 같은 표의 동등한 결과로 합치지 않는다.

**Tests**

R5–R8을 올바른 기대값으로 전환한다. gap/epoch 변경, guard 없는 damage·idle, overlap, 음성 0개, 잘못된 hex, truncated file, pulse 복귀, 다중 source, held guard를 검증한다. 기대 numerator/denominator를 사람이 확인한 작은 사례로 계산한다.

**Fixtures**

audit analyzer_gap, damage_without_guard, overlap, malformed_hex 입력, 새 contact/annotation fixture, T06에서 얻은 실제 gap·lifecycle 자료. 실제 combat label이 없으면 performance fixture로 쓰지 않는다.

**Acceptance Criteria**

R7의 단일 변화가 설명 없이 독립적인 두 지지 표본으로 부풀려지지 않는다. generation/gap을 가로지르는 trial 비교가 차단된다. guard 없는 음성/피해 trial이 보존된다. 분모 0은 N/A이고 손상 입력은 명시적 실패다. 집계 결과마다 실제 표본 수와 제외 이유를 추적할 수 있다.

**Out of Scope**

ML 모델, 자동 label 생성, 성능 목표 달성 주장, GUI·대규모 통계 플랫폼, 새 게임 신호 조사.

## T08 — 실제 세 이벤트의 분리 가능한 corpus를 만든다

**Goal**

Normal Block, Perfect Deflect, Take Damage를 현재 관측으로 구분할 수 있는지 판단할 실제 자료를 만든다. **실제 Sekiro가 필요한 작업이다.**

**Why**

정확한 HP 관측은 유용하지만 Block/Deflect 분리의 증거는 아니다. classifier를 먼저 만들면 marker 시점이나 우연한 offset 변화에 과적합하기 쉽다.

**Scope**

T03/T05의 기록과 T07 분석을 활용한 controlled combat pilot, feature 검토, 세션별 개발/holdout corpus 및 독립 label.

**Constraints**

T06에서 확인되지 않은 field를 검증된 입력으로 취급하지 않는다. 정답 label을 runtime feature에 넣지 않는다. holdout 결과를 보고 같은 자료에 맞춰 규칙을 반복 조정하지 않는다. 새 animation/state ID를 추측하지 않는다.

**Required Implementation**

먼저 짧은 pilot으로 동일 적의 통제된 공격을 기록한다. block/deflect/damage 외에 held guard, idle, jump, menu, attack miss, 빠른 연속 충돌을 포함한다. 실제 contact 기준 annotation과 그 불확실성을 적는다.

후보 feature가 보이면 개발 세션과 별도의 평가 세션을 수집한다. 평가 계획의 예는 Deflect 200회·Block 200회·Damage 100회이며, 이는 이미 확보된 데이터 수가 아니다. 같은 공격의 상관된 반복을 독립 표본으로 과장하지 말고 session/조건을 보존한다.

HP/Posture가 충분하지 않으면 분리 불가능한 실제 사례를 남긴다. 그다음에만 bounded read-only animation/state 후보 조사를 제안한다. 허용되는 관측 경로를 검증하기 전에 hook·전체 메모리 무제한 탐색으로 넘어가지 않는다.

**Migration Strategy**

기존 수동 fixture는 배선 테스트로 유지한다. 실제 corpus는 별도 provenance와 버전으로 추가하고 source revision·layout 가설을 고정한다. 수집된 raw 자료는 다시 label할 수 있게 원본을 보존한다.

**Tests**

모든 파일 strict decode, capture/annotation ID 대응, 중복 trial ID, 시간 구간·epoch·coverage 검증을 실행한다. analyzer가 label을 검출 입력으로 전달하지 않는지 검사한다. pilot 분석과 holdout 평가를 구별한다.

**Fixtures**

실제 positive/negative 원본과 annotation, train/development/holdout manifest, ambiguous·읽기 실패 사례. 실패 사례를 별도 폴더로 숨기지 말고 전체 집계에 포함한다.

**Acceptance Criteria**

label 출처와 원본 관측을 추적할 수 있고 개발 자료와 holdout이 분리된다. 어떤 관측 특징이 각 사건을 구분하는지, 어떤 반례가 있는지를 제시한다. 충분한 특징이 없으면 “현 관측으로 분리 미입증”이 정직한 결과다. 이때 T09의 Block/Deflect 구현은 BLOCKED로 두고 실제 증거 없이 성공한 detector를 만들지 않는다.

**Out of Scope**

모든 적·패턴 보장, PCM·adaptive trigger, 자동 플레이, 새로운 game memory layout 창작, 라이브러리 재설계.

## T09 — 같은 관측으로 동작하는 최소 실제 detector를 구현한다

**Goal**

사람 label 없이 raw observation만으로 사건을 만들고, live와 replay에서 같은 입력에 같은 논리 결과를 내게 한다. 구현과 회귀는 오프라인이지만 T06/T08의 실제 근거가 필요하다.

**Why**

ManualLabelEventDetector는 배선을 검증할 뿐이다. OnSignal의 stateful push 방식은 쓸 수 있으나 frame coherence·gap·epoch·시간 전진·종료 의미가 빠져 있다.

**Scope**

작은 Sekiro detector, typed observation 입력, GameEvent occurrence/시각 metadata, old interface adapter, offline evaluator.

**Constraints**

rule engine·plugin graph·범용 ML platform을 만들지 않는다. 실제 feature가 없으면 Block/Deflect 규칙을 invent하지 않는다. 게임 의미에 맞는 입력 없이 enum 이름만 있는 detector를 완성품으로 보고하지 않는다.

**Required Implementation**

먼저 검증된 player HP의 감소에 기반한 Take Damage를 작은 규칙으로 구현한다. 같은 epoch의 유효한 연속 관측만 비교하고 max/state/lifecycle 변화의 반례를 검토한다. Block/Deflect는 T08의 분리 근거가 있을 때만 추가한다.

frame 기반 push와 작은 temporal state를 사용한다. unchanged 관측으로 시간이 전진하고 gap/epoch가 pending 판단을 무효화하게 한다. 필요한 경우에만 명시적인 AdvanceTo를 추가한다. EOF/비정상 종료는 미확정 사건을 자동 확정하지 않는다.

occurrence ID 또는 동등한 상태 기준으로 중복을 막는다. 추정 발생 구간과 detector emit 시각을 분리한다. 실시간 haptic에 전달하는 것은 확정 이벤트이며, retraction 프레임워크는 현재 추가하지 않는다.

**작은 reference sketch**

```cpp
class SekiroCombatDetector {
public:
    void OnObservation(const SekiroObservationFrame&, std::vector<GameEvent>&);
    void OnBoundary(const ObservationBoundary&);
    void EndOfStream(EndReason, std::vector<GameEvent>&);
};
```

**Migration Strategy**

기존 ManualLabelEventDetector·IGameEventDetector·v1 pipeline은 테스트 경로로 유지한다. 새 detector를 typed source에 붙이고, event 출력은 기존 GameEvent로 연결한다. 모든 소비자를 한 번에 바꾸지 않는다.

**Tests**

실제 raw fixture별 expected event와 음성 사례를 검사한다. epoch/gap, 동일 timestamp, rapid successive contact, held guard, no-change, 미확정 EOF, max/state 변경, replay 속도 차이, 중복 0을 검증한다. label을 제거해도 detector 입력/결과가 유지되는지 확인한다.

**Fixtures**

T06/T08의 versioned corpus, 독립 expected event와 시간 허용 구간, 기존 수동 fixture. 실제 성능 평가는 미사용 holdout에서 수행한다.

**Acceptance Criteria**

사람 label 없이 세 이벤트가 검출되고 같은 frame에 deterministic 결과를 낸다. 목표는 사건별 recall ≥95%, block→deflect FP <5%, duplicate=0이다. raw count·confusion matrix·95% 신뢰구간·coverage·제외 수를 함께 보고한다. 자료가 부족하거나 목표 미달이면 결과와 반례를 남기고 완료를 주장하지 않는다.

**Out of Scope**

실제 HID 연결(T10/T11), Posture Break/Deathblow, general event DSL, event retraction, 실험 가설 없는 추가 주소 도입.

## T10 — 기존 출력 경로의 Reset 순서와 재생 시간을 바로잡는다

**Goal**

검증된 GameEvent를 기존 mapping/preset/output에 연결할 준비를 하고, Reset 뒤 이전 출력이 재등장하지 않게 한다. 코드·Fake 검증은 실게임 없이 수행한다.

**Why**

R10은 진행 중 SendEffect가 Reset 반환 뒤 완료될 수 있음을 보였다. hardware replay는 현재 기록 timestamp에 맞춰 기다리지 않는 빠른 loop를 사용한다. Dispatched는 실제 HID 성공이 아니라 큐 등록이다.

**Scope**

HapticScheduler의 worker/backend 명령 순서, reset/cancel 완료 계약, ReplayPipeline의 작은 event dispatch 공용화, replay_hardware pacing, transport 오류와 시간 계측.

**Constraints**

IHapticBackend·IDualSenseTransport·MappingRepository·PresetRepository를 재작성하지 않는다. 측정 없이 lock-free 큐를 도입하지 않는다. logical replay clock과 실제 장치 재생 clock을 혼동하지 않는다.

**Required Implementation**

backend send/reset을 한 소유 경로에서 직렬화한다. 이미 진행 중인 호출의 완료와 그 뒤의 reset을 포함해, Reset 반환 후 이전 세대의 출력 명령이 다시 완료되지 않는 계약을 정한다. 취소할 수 없는 backend 호출은 끝날 때까지 기다리는 등 실제 가능한 의미를 사용하고 실패/timeout을 성공으로 숨기지 않는다.

GameEvent → mapping → preset → schedule 부분만 작은 dispatcher로 분리해 live/replay가 공유한다. queue 등록, backend dispatch, HID 결과를 구분한다. 기존 Dispatched enum을 유지한다면 그 의미를 명시하고 실제 전송 결과는 별도로 기록한다.

빠른 평가 replay는 유지하고 hardware mode에 timestamp 기반 pacing과 종료 시 마지막 effect 처리 정책을 둔다. stale cue·장치 분리·전송 실패를 늦은 burst로 재생하지 않는다. transport 반환 길이와 실패 전파를 해당 HIDAPI 계약에 맞춰 검사한다.

**Migration Strategy**

기존 scheduler duration/preemption 계약을 유지하면서 reset 순서를 먼저 고친다. 이후 작은 dispatcher와 clock adapter를 추가한다. HapticEffectType 제거·preset schema 변경은 묶지 않는다.

**Tests**

R10의 gated backend를 회귀로 전환한다. send 진행 중 reset, reset 실패, stop 반복, queue burst, disconnected backend, short/failed transport write, pacing 간격, 마지막 effect 종료를 검증한다. thread timing은 고정 sleep에 의존하지 말고 latch/clock seam을 쓴다.

수정한 Reset이 진행 중 send 완료를 기다리는 계약이면, reset 호출을 별도 제어 thread에서 시작한 뒤 test thread가 send gate를 해제하고 완료 순서를 검사한다. 기존 감사 harness의 “Reset 반환 뒤 gate 해제” 순서를 그대로 복사하면 올바르게 blocking하는 구현에서 테스트 자체가 교착될 수 있다.

**Fixtures**

audit gated backend 구성, 기존 effect/preset/mapping fixture, 서로 다른 timestamp의 짧은 replay, error transport. 실제 기기를 사용하지 않았다면 hardware validation은 NOT RUN이다.

**Acceptance Criteria**

Reset 성공 반환 뒤 이전 세대 send가 실행/완료되지 않고, 실패는 caller에게 전달된다. 기존 mapping/preset 회귀가 유지된다. fast replay의 논리 결과가 바뀌지 않으며 paced hardware 경로는 기록 간격을 존중한다. 큐 성공과 HID 성공이 구별된다.

**Out of Scope**

PCM·speaker·adaptive trigger, 스케줄러 전면 교체, 실제 Sekiro 성능 주장, 플랫폼용 output graph.

## T11 — 실제 게임에서 정확도와 DualSense 지연을 검증한다

**Goal**

실제 사건 → raw 관측 → detector → mapping/preset → Legacy rumble 경로의 정확도·추가 지연·오류 처리를 확인한다. **실제 Sekiro와 DualSense가 모두 필요하다.**

**Why**

공통 테스트와 Fake transport 통과는 실게임 검출 또는 물리적 진동의 증거가 아니다. 기존 투자 가치를 제품 결과로 연결하는 단계다.

**Scope**

T09 detector와 T10 output의 최소 live 조립부, Windows 실제 빌드, 장치 smoke, 통제 전투 평가와 timing log.

**Constraints**

T10의 reset gate 전에 live haptic을 켜지 않는다. 20 ms를 전체 체감 지연으로 표현하지 않는다. 범위가 확인된 build/적/공격을 명시하고 조건 밖의 성공을 추정하지 않는다.

**Required Implementation**

detector가 소비하는 frame을 동시에 기록하고 event occurrence·read end·emit·queue·backend·HID 시각을 연결한다. 기존 Legacy rumble을 사용한다. 명령 scanner나 analyzer를 live 제품 loop에 포함하지 않는다.

실제 Windows CMake/HIDAPI 구성을 빌드하고 장치 output 강도·좌우·duration을 확인한다. 통제 전투에서 record-only와 haptic-on 조건을 평가한다. 읽기 실패, 프로세스 종료, USB 분리/재연결, 반복 stop/reset, 빠른 연속 사건을 시험한다.

**Migration Strategy**

기존 apps의 검증된 조립 코드를 재사용하고 새 runtime mode를 최소화한다. 기존 replay와 fixture는 같은 detector/dispatcher를 사용한다. 사용자 설정을 대규모로 바꾸지 않는다.

**Tests**

실제 사건별 recall·block→deflect FP·duplicate를 계산한다. emit→queue와 queue→HID를 각각 표본 수·p50/p95/p99/max로 측정한다. 발생 추정 구간→검출도 별도로 보고한다. 오류 후 stale cue 재생이 없는지 확인한다.

**Fixtures**

실제 capture와 annotation, expected/observed event, event-to-output timing log, 사용 preset/config, build·장치 정보. 물리적 motor onset 측정 장비를 쓰지 않았다면 해당 값은 비워 둔다.

**Acceptance Criteria**

시험 범위에서 recall ≥95%, block→deflect FP <5%, 중복 0을 목표로 검증한다. detector emit→queue는 측정한 각 사건에서 <20 ms를 목표로 하며 초과 사례를 보고한다. queue/HID 지연은 별도 공개한다. Reset 뒤 이전 effect가 재등장하지 않고 장치·관측 실패가 정상 cue 성공으로 처리되지 않는다. 실제 환경이 없으면 구현 준비와 live gate 미완료를 구분한다.

**Out of Scope**

물리 onset 미측정 상태의 end-to-end latency 보장, 전투 전체 일반화, 두 번째 게임, profile 공유·GUI·PCM.

## T12 — 증거를 얻은 뒤 필요한 구조 정리만 한다

**Goal**

실험·분석 코드가 제품 런타임에 우연히 의존성을 만들지 않도록 필요한 target 경계만 정리한다. **연기 가능한 선택적 작업**이다.

**Why**

현재 공통 정적 라이브러리에 discovery와 analyzer가 함께 있다. 그러나 정적 library라는 이유만으로 모든 앱이 모든 scanner object를 포함하는 것은 아니다. 실제 의존성·가설 유입을 막는 이득이 있을 때만 분리한다.

**Scope**

최소 CMake target 분리, 필요 없는 중복 조립/해석 경로 정리, 검증된 global slot의 기존 SignatureProfile 재사용.

**Constraints**

디렉터리·namespace·공개 API를 미적으로 재배치하지 않는다. 사용자 미커밋 작업을 되돌리지 않는다. capture·detector·preset schema 변경을 묶지 않는다. T01–T04 완료 조건에 이 작업을 끼워 넣지 않는다.

**Required Implementation**

runtime에서 필요한 source와 개발 전용 source의 실제 참조를 확인해 선택적 discovery target을 만든다. 기존 Windows/HID target과 test support 분리를 유지한다. 모듈 slot은 기존 AddressResolver/Profile을 재사용하되 heap pointer/field 해석을 module-relative range에 억지로 넣지 않는다.

사용처가 사라지고 호환 adapter가 검증된 중복 코드만 제거한다. HapticEffectType은 실제 사용처·호환성 이득이 확인될 때만 제거하고, custom JSON parser 교체는 별도 문제가 요구할 때까지 연기한다.

**Migration Strategy**

기존 target 또는 alias를 유지한 채 새 target으로 source를 옮긴다. 소비자별 링크를 단계적으로 바꾸고 회귀를 확인한다. proven component를 복제해서 새 이름으로 다시 구현하지 않는다.

**Tests**

runtime app이 discovery target 없이 빌드되는지, discovery app과 기존 테스트가 계속 빌드되는지 검증한다. 실제 링크 의존성·실험 catalog 유입을 검사한다. 테스트·fixture 경로 변경으로 검증이 조용히 빠지지 않게 한다.

**Fixtures**

기존 v1 fixture, 새 observation corpus, mapping/preset/expected events 모두 그대로 사용한다. 구조 정리 때문에 실제 corpus를 새로 수집하지 않는다.

**Acceptance Criteria**

runtime 의존성에서 개발 전용 탐색·분석 경로가 분리되고 기존 public 동작과 replay 결과가 유지된다. 코드 삭제의 이유와 사용처 부재를 제시한다. 이득이 없으면 변경을 생략하고 이유를 기록해도 된다.

**Out of Scope**

새 플랫폼 아키텍처, 모든 폴더 이동, 테스트 프레임워크 교체, JSON 교체의 동시 수행, GUI·PCM·추가 게임.

## Claude Code 완료 보고 형식

각 작업은 구현 완료와 실게임 검증 완료를 구별해 보고한다. 최소한 해당 task ID, 변경 이유와 영향 파일, migration 결과, 실행한 테스트의 정확한 counts/실패, 생성·재사용한 fixture, Acceptance Criteria별 PASS/FAIL/NOT RUN, 미해결 사항을 포함한다. 테스트 수를 늘리는 대신 그 작업의 구체적인 위험을 막는 회귀를 우선한다.

T06/T08/T11에서 실제 환경이 없으면 실행 가능한 절차·입력 템플릿·평가 도구까지 준비하되, 합성 실행 결과로 live gate를 통과시키지 않는다. 결과가 기준 미달이면 원본과 반례를 보존하고 다음 작업의 해당 기능을 BLOCKED로 남긴다.

감사 원문: [SekiroHaptics_Architecture_Audit.md](sandbox:/workspace/scratch/d7e4abee2739/sekiro_haptics_audit/SekiroHaptics_Architecture_Audit.md)

재현 코드·fixture·원본 테스트 로그: [SekiroHaptics_Audit_Evidence.zip](sandbox:/workspace/scratch/d7e4abee2739/sekiro_haptics_audit/SekiroHaptics_Audit_Evidence.zip)

If I owned this repository, the very next thing I would do is: turn the reproduced capture overwrite into a regression test, then fix capture ownership and file preservation before recording another Sekiro session.
