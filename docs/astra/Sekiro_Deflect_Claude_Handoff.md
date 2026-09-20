# Sekiro — 실제 튕겨내기 신호를 찾는 Claude Code 작업서

2026-09-10 · 기존 저장소 무수정 · 신호 발견과 검출기 구현을 분리한다.

## 먼저 결론

목표는 **플레이어가 적 공격을 성공적으로 튕겨낸 Deflect**다. 자동으로 방어 버튼을 누르는 기능이 아니며, 별도의 게임 내부 `PerfectDeflect` 등급/필드가 있다는 주장도 아니다.

기존 haptics 프로젝트를 처음부터 다시 만들 이유는 없다. 지금 필요한 것은 새 플랫폼이 아니라 **실제 게임 결과와 연결된 신호 하나**다. 우선 일반 가드와 튕겨내기가 갈라지는 결과 처리 경로를 좁힌다. 거기서 외부 읽기로 관측 가능한 결과 기록이 발견되면 우선 사용한다. 그렇지 않으면 플레이어의 성공 반응 애니메이션/상태를 후보로 검증한다.

중요한 한계: 업로드된 프로젝트 코드 분석과 실제 `sekiro.exe` 역분석은 다르다. 이번 답변에서는 게임 실행 파일을 역분석하지 않았고, 확정된 패링 주소·함수·animation ID를 확보하지 못했다. 공개 도구/코드는 조사 출발점이지 현재 빌드의 패링 검출 증거가 아니다. 아래 C++ 예제도 게임에서 신호를 읽는 완성 프로그램이 아니다.

## 무엇을 읽어야 하는가

| 조사 대상 | 우선순위 | 채택 조건 / 오해하면 안 되는 점 |
|---|---|---|
| 일반 가드와 성공 튕겨내기를 구분하는 전투 결과 기록 | 가장 먼저 조사 | 결과 의미, 방어자=플레이어, 발생별 식별, 관측 가능 수명을 확인해야 한다. 이런 지속 필드/카운터가 존재한다고 가정하지 않는다. |
| 플레이어의 성공 반응 애니메이션/상태 | 현실적인 대안 후보 | 일반 가드·적이 플레이어 공격을 튕김·허공 방어와 구별하고, 같은 ID의 연속 재생까지 식별해야 한다. |
| SpEffect·사운드·VFX 발생 | 경로를 찾는 보조 단서 | 성공 이후의 결과인지, 성공 가능한 시간창인지, 다른 캐릭터도 사용하는지 확인한다. |
| 가드 입력, HP 무변화, 체간 변화 | 보조 관측만 | 단독으로 플레이어 튕겨내기를 확정하지 않는다. |
| 화면/오디오 분류 | 필요시 별도 근사 검출 | 게임 내부 판정과 같다고 보장할 수 없고 주체 구분·지연·중첩 문제가 있다. 이번 우선 구현은 아니다. |

## 실제로 찾아가는 순서

### 1. 게임 바이너리와 데이터에서 범위를 줄인다 — 오프라인 가능

사용자 PC의 실제 게임 설치 경로와 `sekiro.exe` SHA-256, 버전, 사용 중인 모드를 기록한다. 분석 산출물은 별도 작업 폴더에 쓴다. 원본 EXE, 게임 데이터, 세이브는 변경하지 않는다. 업로드 ZIP 안의 EXE 해시 문자열을 실제 파일을 다시 해시한 결과라고 보고하지 않는다.

임의의 DLL을 전부 분석하는 것으로 시작하지 않는다. 우선 EXE의 전투 처리와 플레이어 행동/애니메이션 데이터를 조사하고, 실제 호출 경로가 다른 모듈을 가리키면 그 모듈을 추가한다. Ghidra 같은 디스어셈블러에서 전체 코드를 이해하려 하지 말고 아래 질문에 답할 최소 경로를 찾는다.

- 일반 가드와 튕겨내기 성공의 결과가 어디서 갈라지는가?
- 성공 반응 애니메이션·사운드·VFX를 선택하는 호출자는 어디인가?
- 그 호출에 등장하는 캐릭터는 공격자, 방어자 중 누구인가?
- 결과가 메모리 객체에 남는가, 아니면 함수 호출/레지스터 안에서만 잠깐 존재하는가?
- 같은 애니메이션으로 여러 번 튕길 때 무엇이 매번 달라지는가?

공개 도구 DSAnimStudio는 Sekiro의 애니메이션/TAE를 지원하고, 압축 게임 아카이브를 열어 애니메이션 ID와 이벤트를 조사하는 절차를 제공한다. 별도 프로젝트 폴더에 추출해 관찰하고 게임에 수정본을 설치하지 않는다. 문서에 나오는 parry window는 성공 결과 자체가 아니다. [DSAnimStudio 공식 README](https://github.com/Meowmaritus/DSAnimStudio)

가용한 스크립트/심볼에는 `deflect`, `parry`, `just guard`, `guard` 등을 검색할 수 있다. 그러나 문자열이 없다고 해당 처리가 없는 것은 아니고, 이름이 있다고 성공 판정인 것도 아니다. 정적 후보마다 실제 비교문·호출자·참조 관계를 기록한다.

구조체 조사 출발점으로 fromsoftware-rs의 Sekiro crate를 참고할 수 있다. 해당 프로젝트는 런타임 구조체 바인딩을 제공하지만, 이것만으로 현재 빌드의 필드 오프셋이나 Deflect 의미가 검증되는 것은 아니다. 참조한 commit과 실제 사용한 정의를 남긴다. [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs)

주의: SoulMemory를 단순 읽기 라이브러리라고 생각하고 곧바로 `TryRefresh()`하는 방식은 이 작업에 맞지 않는다. 공식 README는 Sekiro refresh 과정에서 모드와 타이머 수정을 설치한다고 설명한다. 읽기 전용 제약에서는 초기화까지 확인 없이 도입하지 않는다. [SoulMemory README](https://github.com/FrankvdStam/SoulSplitter/blob/main/src/SoulMemory/README.md)

이 단계의 산출물은 '완성 검출기'가 아니라 출처와 반증 방법이 있는 후보 목록이다. 자산 ID와 런타임 ID의 표현이 같다는 것도 대조해야 한다.

### 2. 작은 읽기 전용 probe로 후보를 관측한다 — Windows/실게임 필요

기존 `IProcessReader`, AOB/RIP resolver를 재사용한다. root AOB가 하나 발견됐다는 사실과 플레이어의 올바른 자식 객체까지 도달했다는 사실을 구분한다. 영구 heap 주소를 하드코딩하지 않는다.

매 시도마다 다음을 기록한다. 실패도 기록한다.

- session ID, build hash, profile revision, sample sequence, monotonic read start/end.
- 읽기 성공/부분 읽기/실패, 요구 바이트 수·읽은 바이트 수, 오류 코드.
- 플레이어 객체의 확인 근거와 lifecycle epoch, 실제로 따라간 포인터 경로.
- 선택된 반응 lane의 raw animation/state/result ID, 가능한 경우 재생 시간과 실제 발생 식별자.
- 변경 없는 baseline/관측, 관측 gap, 독립적인 trial/영상 동기화 marker.

`ReadProcessMemory`는 대상 프로세스의 데이터를 복사하는 API이지 일관된 게임 프레임을 보장하는 API가 아니다. 다중 읽기의 중간 교체와 torn observation 가능성을 다룬다. source가 제공하는 version/sequence가 있으면 활용하되, 전후 포인터가 같다는 검사만으로 ABA나 동시 변경이 불가능하다고 주장하지 않는다. [Microsoft ReadProcessMemory](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory)

프로세스 권한은 사용하는 API가 요구하는 읽기/조회 권한만 요청한다. 임의로 PROCESS_ALL_ACCESS, 쓰기 권한, 권한 상승을 추가하지 않는다. 접근 거절은 원인과 함께 보고한다. [Microsoft 프로세스 접근 권한](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights)

처음에는 설정상 5 ms 등으로 샘플링할 수 있지만 실제 성공 읽기 간격 분포를 측정한다. 설정값을 지연 보장으로 보고하지 않는다. 상태가 샘플 사이에 사라지면 놓칠 수 있다. 카운터가 여러 번 증가해도 마지막 결과만 남는다면 중간 공격 결과를 복원했다고 해서는 안 된다.

### 3. 성공과 실패를 직접 대조한다 — 실게임 필요

소규모 pilot에서는 조건별 약 20회로 후보를 빠르게 버릴 수 있다. 이 숫자는 권장 실험 설계일 뿐 실행 결과나 통계적 보장이 아니다. 학습/후보 선택에 쓴 장면과 최종 확인 장면은 분리한다.

| 실험 조건 | 판정 기대 |
|---|---|
| 일반 가드로 막기 | 플레이어 Deflect 없음 |
| 플레이어가 단발 공격을 성공적으로 튕기기 | 성공 1회와 검출 1회의 시간 대응 |
| 적이 내 공격을 튕기기 | 플레이어 Deflect 없음 |
| 허공에서 방어 버튼 누르기/연타/유지 | 공격을 튕긴 결과가 없으면 검출 없음 |
| 피격, 회피, 공격 빗나감 | 해당 구간에 Deflect 검출 없음 |
| 빠른 연속 튕겨내기, 동일 동작 반복 | 각 성공을 따로 구분; 고정 cooldown으로 뭉개지 않음 |
| 다른 적·공격 방향·현재 지원하려는 다른 방어 동작 | 후보의 적용 범위를 확인하고 미지원은 명시 |
| 사망·부활·로드·워프·재접속·읽기 gap | 이전 객체/세션의 상태로 가짜 이벤트 없음 |

독립 영상/오디오 판독과 수동 annotation으로 실제 결과를 표시한다. 검출기 결과를 그대로 정답 레이블로 사용하지 않는다. 모호한 장면은 별도 표기한다. 한 정답과 여러 검출을 모두 성공으로 세지 말고 시간창 내 일대일 매칭하여 TP/FP/FN, 중복, 지연을 분리한다. 입력 marker의 시간은 실제 충돌 시각과 다르다.

held-out 세션에서는 예컨대 성공 50회 이상, 음성 조건별 20회 이상을 시작 목표로 둘 수 있다. 지원 범위·실제 N·오류 수를 보고하며 '몇 번 맞았으니 완벽'이라고 결론 내리지 않는다. 허용 오검출/미검출 기준은 진동 연결 전에 명시한다.

### 4. 검증한 신호만 기존 haptics에 연결한다

원본 관측 → 후보 이벤트 → 검증된 게임 이벤트 → 기존 mapping/preset/scheduler/backend 순서로 연결한다. 이 명칭은 개념적 단계이며 기존 코드에 그런 이름의 함수가 이미 있다는 뜻이 아니다. signature 일치, synthetic test 통과, annotation 일치 중 하나만으로 모두 충족됐다고 처리하지 않는다.

반응 애니메이션이 일반 가드와 공유되거나 동일 ID의 반복 발생을 구분할 수 없다면 이 후보를 탈락시키거나 적용 범위를 제한한다. animation ID가 바뀔 때만 진동하는 코드를 완성 Deflect detector로 승인하지 않는다.

외부 읽기로 잡을 수 없는 순간적인 결과만 존재한다면 그 한계를 보고한다. 다음 선택지는 호출 지점의 관측 hook이지만, 관측용이어도 주입/코드 패치 등 프로세스 변경이 수반될 수 있다. 현재 읽기 전용 범위에서 몰래 전환하지 말고 별도 승인을 받는다. 한 번의 정적 분석으로 정확한 판정 신호를 보장할 수는 없다.

## C++ 예제가 하는 일과 하지 않는 일

함께 제공한 `deflect_candidate_reference.cpp`는 게임을 실행하지 않는 독립 C++20 예제다. 확정 ID 목록은 없고 기본 목록은 비어 있다. `main()`의 큰 숫자 두 개는 전부 synthetic fixture다.

핵심 조건은 다음과 같다.

```cpp
// 실제 Update()는 이 조건 앞에서 build, 읽기, 주체, epoch, gap을 검사한다.
if (*s.activation == *p.activation ||
    !candidates_.contains(s.animation_id)) {
    return std::nullopt;
}
return CandidateDeflect{s.session, s.epoch, *s.activation,
                        s.observed_us, s.animation_id};
```

`activation`은 '같은 동작이라도 새로 발생했는가'를 알 수 있어야 한다는 **우리 측 인터페이스 계약**이다. Sekiro에 같은 이름의 카운터가 있다는 뜻이 아니다. 실제 재생 인스턴스/결과 sequence가 확인되면 그 의미를 문서화해 연결하고, 확인되지 않으면 null로 둔다. sampling sequence·현재 시각·플레이어 포인터를 대신 넣으면 안 된다. 반복 재생 시 timer 감소만 보고도 확정하지 않는다.

따라서 이 예제는 가장 어려운 '게임 신호 발견'을 해결했다고 주장하지 않는다. 이미 찾아낸 신호에서 중복·세대 혼합을 줄이는 후속 로직이다. 출력 이름도 의도적으로 `CandidateDeflect`다. 애니메이션과 성공의 대응 관계 및 관측 수명이 검증되어야 실제 이벤트로 승격할 수 있다. gap 직후 이벤트를 억제하는 것은 보수적인 선택이며 실제 성공을 놓칠 수 있으므로 관측 품질 지표에 남긴다.

독립 실행:

```bash
g++ -std=c++20 -Wall -Wextra -Werror -pedantic deflect_candidate_reference.cpp -o deflect_candidate_reference
./deflect_candidate_reference
```

Windows Developer Command Prompt에서는 C++20을 지원하는 MSVC로 별도 컴파일할 수 있다. 이 문서 작성 환경에서는 Windows 컴파일/게임 실행을 하지 않았다.

실제 실행 결과: 위 GCC 명령으로 경고를 오류로 처리하여 컴파일했고, synthetic 계약 검사 **18/18이 통과**했다. 이는 제공한 예제의 baseline·세대·중복·gap 처리 검사다. 실제 Sekiro 신호 추출, animation ID 의미, Windows 프로세스 접근, 게임 내 인식률, 물리 진동은 이 테스트로 검증하지 않았다. 기존 저장소의 테스트를 이번에 다시 돌린 수치도 아니다.

## Claude Code에 맡길 단일 작업: DEFLECT-SIGNAL-001

아래 항목 전체를 작업 지시로 사용한다. 기존 감사 전체를 다시 하거나 플랫폼을 재설계하지 않는다.

### Goal

현재 사용자 빌드에서 일반 가드와 **플레이어 성공 튕겨내기**를 구분할 실제 신호를 찾아, 근거 있는 읽기 전용 probe와 이벤트 검출에 필요한 최소 변경을 만든다. 처음 산출물은 진동이 아니라 원본 관측과 판정 근거다.

### Why

현재 프로젝트에는 재사용 가능한 프로세스 읽기·replay·haptic 구조가 있지만 실제 Sekiro Deflect source는 확정되어 있지 않다. HP·체간 후보를 더 많이 수집하는 것보다 성공 결과 경로를 좁히는 것이 이번 작업의 목적이다.

### Scope

기존 reader/resolver와 capture 수명/기록의 필요한 부분, 새 진단 probe 또는 작은 adapter, build별 후보 profile, 독립 annotation과 replay test만 다룬다. 정적 조사는 즉시 시작할 수 있다. 정확한 EXE/자산 또는 Windows 실행 환경이 없으면 필요한 로컬 경로/산출물을 요청하고, 오프라인 구현만 진행했음을 보고한다.

### Constraints

- 저장소 전체 재작성, 새로운 범용 플랫폼, GUI는 금지한다. 사용자의 미커밋 변경을 보존한다.
- 게임 메모리 쓰기, EXE/세이브/게임 데이터 수정, 주입, 자동 입력, hook 설치는 이번 작업에 포함하지 않는다.
- 주소·offset·함수명·ID를 추측하지 않는다. 공개 소스의 commit/빌드 적용 범위/라이선스를 확인하고 그대로 복제하지 않는다.
- 모르는 값은 null/unsupported로 남긴다. 가짜 값으로 실행 가능하게 꾸미지 않는다.
- 가드 입력, parry window, HP 무변화, 체간 변화는 단독 성공 판정이 아니다.
- 코드를 컴파일한 사실과 실제 세키로에서 신호를 검증한 사실을 별도로 보고한다.

### Required Implementation

1. 현재 EXE/모드 지문과 자료 접근 가능 여부를 확인한다. 제공되지 않은 바이너리를 분석했다고 주장하지 않는다.
2. 결과 처리 분기 및 플레이어 성공 반응 후보를 찾는다. 후보마다 출처, 함수 RVA/관련 비교문 또는 자산 ID, player 소유 근거, 일반 가드와의 차이, 읽기 가능한 수명, 반복 발생 구분 근거, 반증 실험을 기록한다.
3. 검증된 경로로만 raw 값을 읽는 probe를 만든다. 현재 게임 profile은 미확정 상태에서 시작하고, profile 변경 시 새 세션/epoch로 baseline을 리셋한다. build 불일치 또는 불명확한 결과에는 이벤트를 내지 않는다.
4. 파일 충돌·재시작 보존, worker 종료, root/child 재확인, generation 변경 후 실패한 첫 읽기, baseline/unchanged/gap/error 기록을 먼저 테스트한다. 입력/annotation과 검출 결과는 별도 record로 저장한다.
5. 이 문서의 일반 가드/성공/적 성공/허공 입력/피격/연속/수명 전환 실험을 사용자가 실행할 정확한 절차로 출력한다. 실제 원본 로그가 없으면 live 단계 완료를 선언하지 않는다.
6. 독립 annotation과 일대일 시간 매칭으로 후보를 평가한다. 현재 analyzer의 겹치는 trial 중복 귀속 문제를 수정하거나, 같은 의미의 오류가 없는 작은 별도 평가기를 사용한다. raw delta 하나를 여러 성공으로 집계하지 않는다.
7. 검증된 후보에 한해 이벤트 adapter를 붙이고 기존 mapping/scheduler로 전달한다. 실제 진동 연결 전 기존 Reset/dispatch 수명 문제를 해결한다. 신호가 부적합하면 실패 근거와 다음 후보를 보고한다.

최소 인터페이스 예시는 다음과 같다. 이름은 기존 프로젝트 관례에 맞춰 조정한다.

```cpp
// 설계 스케치: 기존 게임 API라는 뜻이 아니다.
struct ProbeRead {
    ReadStatus status;             // exact/partial/failed/unsupported/inconsistent
    ObservationIdentity identity; // session + object epoch
    RawReactionFields raw;        // 실제 확인한 필드만; 없는 필드는 optional
};

class IReactionProbe {
public:
    virtual ~IReactionProbe() = default;
    virtual ProbeRead ReadOnce() = 0;
};
```

### Migration Strategy

기존 replay/mapping/preset/backend는 유지한다. 검증되지 않은 후보 관측은 별도 diagnostic 경로에서 기록한다. 기존 수집기를 재사용하면 감사 T01–T04의 capture 재개 요건을 충족시키고, 입력/전투 annotation 수집에는 T05의 분리 원칙을 적용한다. 새로운 작은 probe를 쓰더라도 동일한 데이터 보존·수명·오류 기록 요건을 생략하지 않는다.

static 분석을 하기 위해 HP 가설부터 모두 검증하거나 T01–T12 전체를 끝낼 필요는 없다. 반대로 기존 capture가 깨져 있는 상태에서 수집한 데이터를 확정 근거로 쓰지도 않는다. profile은 근거와 함께 candidate → live-validated로 별도 승격하고, code review만으로 승격하지 않는다.

### Tests

오프라인: unsupported build, 빈 후보 목록, short read, 객체 교체 직후 첫 읽기 실패, attach 중간 상태, 지속 상태 중복 억제, 같은 ID의 새 발생, 다른 actor, seq/time gap, counter rollback, 파일 충돌/중복 시작, 종료 중 읽기, epoch/reconnect, 일대일 평가 매칭을 검사한다.

라이브: 성공·음성 조건·빠른 연속·다른 적·수명 전환을 독립 정답과 대조한다. 정확도 외에 실제 읽기 간격, dropped/invalid samples, 매칭 불가 구간, 검출 지연 및 근거 시계의 한계를 기록한다.

### Fixtures

- synthetic fixtures: 게임과 무관한 ID/주소/sequence로 lifecycle·중복·오류 계약만 테스트한다.
- build fixtures: 실제 허용된 바이너리에서 확인한 최소 signature window와 metadata; 게임 전체 파일을 산출물에 재배포하지 않는다.
- live fixtures: session/build/profile이 붙은 원본 observations, 별도 annotations, 영상 동기화 근거. 이것이 없으면 '실게임 검증 안 됨'이다.
- 진짜 live fixture와 synthetic fixture의 디렉터리/metadata를 구분한다.

### Acceptance Criteria

- 어떤 값을 어떤 경로로 읽으며 왜 플레이어 성공 Deflect인지 설명할 근거가 있다. 단순히 함수 이름이나 ID 이름만 근거로 사용하지 않는다.
- 실제 같은 빌드의 독립 실험에서 일반 가드와 적의 튕겨내기를 구분하고, 동일 ID 연속 성공을 개별 검출하는지 확인했다.
- 실제 N, TP/FP/FN, 중복, 미지원 조건, gap을 공개한다. 정확도 목표 미합의 상태에서는 detector를 candidate로 유지한다.
- 파일 덮어쓰기 및 cross-generation false delta 회귀 테스트를 통과한다. 관측 실패를 숨기지 않는다.
- live 자료가 없으면 오프라인/후보 probe 단계까지만 완료로 보고한다.
- 실패나 판정 불가도 합법적인 조사 결과다. 결과를 맞추려고 예제를 실제 게임 증거로 바꾸지 않는다.

### Out of Scope

처음부터 재작성, 모든 Sekiro 동작 지원 보장, 새 HID/PCM 엔진, 여러 게임 지원, 자동 패링 입력, mod/주입/hook 구현, DRM/보호 우회, 기존 게임 데이터 변경, 소규모 실험만으로 완벽 검출 선언.

## 다음 실험 전 필수와 미뤄도 되는 것

**정적 신호 조사는 지금 시작 가능하다.** 단, 다음 라이브 캡처에서는 다음을 먼저 충족한다.

1. 캡처 재시작/파일 충돌로 기존 기록을 지우지 않고 worker를 안전하게 종료한다.
2. 플레이어/root/child 수명 전환을 감지하고, 객체 교체 뒤 첫 읽기가 실패하면 이전 baseline을 폐기한다.
3. baseline·변경 없음·실패·gap·정확한 raw 값·시간을 남겨 관측을 재구성할 수 있게 한다.
4. Windows 읽기/조회 권한과 지원 build 경로를 작은 helper로 확인한다. 입력/정답 annotation을 실제 게임 결과와 분리한다.

analyzer 중복 집계 수정은 **정확도 수치를 해석하기 전 필수**다. 원시 후보 probe를 켜기 전 모든 통계 코드 개편을 마칠 필요는 없다. Reset/dispatch 수정은 실제 haptic 출력 전 필수다. target/library 재분리, 대형 profile 개편, GUI, 범용 detector framework 등은 미뤄도 된다.

이번에 Claude에게 가장 먼저 시킬 말은 이것이다: **“전체 구조를 다시 짜지 말고, 현재 Sekiro 빌드에서 일반 가드와 플레이어 Deflect 성공이 갈라지는 경로를 찾아라. 실제 근거가 있는 후보만 raw probe로 관측하고, 모르는 주소나 ID를 만들어 넣지 마라.”**
