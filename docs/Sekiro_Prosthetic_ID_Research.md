# 세키로 의수 ID 대조 자료

공개 자료 조사일: 2026-09-20. 사용자 PC의 게임 메모리나 바이너리를 이번 조사에서 직접 읽지는 않았다.

공개 Paramdex의 EquipParamWeapon 행과 Sekiro-Practice-CT의 ItemWeapon 이름표를 대조했다. 두 자료에 10계열 40종의 일반 의수 장비 행 ID가 동일하게 존재한다. 아래 값은 공개 카탈로그이며 사용자 빌드의 관측 주소까지 검증됐다는 뜻은 아니다. 강화형을 포함하지만, 가상 무기·미해금용 내부 행이나 모드가 추가한 장비는 포함하지 않는다. 원문 이름을 사용해 한국어 공식 번역과 혼동하지 않도록 했다.

| 계열 | EquipParamWeapon ID | 원문 이름 |
|---|---:|---|
| 수리검·표창 | 70000 | Loaded Shuriken |
| 수리검·표창 | 70100 | Spinning Shuriken |
| 수리검·표창 | 70200 | Gouging Top |
| 수리검·표창 | 70300 | Phantom Kunai |
| 수리검·표창 | 70400 | Sen Throw |
| 수리검·표창 | 70500 | Lazulite Shuriken |
| 폭죽 | 71000 | Shinobi Firecracker |
| 폭죽 | 71100 | Spring-load Firecracker |
| 폭죽 | 71200 | Long Spark |
| 폭죽 | 71300 | Purple Fume Spark |
| 화통 | 72000 | Flame Vent |
| 화통 | 72100 | Spring-load Flame Vent |
| 화통 | 72200 | Okinaga's Flame Vent |
| 화통 | 72300 | Lazulite Sacred Flame |
| 장치 도끼 | 73000 | Loaded Axe |
| 장치 도끼 | 73100 | Spring-load Axe |
| 장치 도끼 | 73200 | Sparking Axe |
| 장치 도끼 | 73300 | Lazulite Axe |
| 안개 까마귀 | 74000 | Mist Raven |
| 안개 까마귀 | 74100 | Aged Feather Mist Raven |
| 안개 까마귀 | 74200 | Great Feather Mist Raven |
| 사비마루 | 75000 | Sabimaru |
| 사비마루 | 75100 | Improved Sabimaru |
| 사비마루 | 75200 | Piercing Sabimaru |
| 사비마루 | 75300 | Lazulite Sabimaru |
| 장치 우산 | 76000 | Loaded Umbrella |
| 장치 우산 | 76100 | Loaded Umbrella - Magnet |
| 장치 우산 | 76200 | Suzaku's Lotus Umbrella |
| 장치 우산 | 76300 | Phoenix's Lilac Umbrella |
| 행방불명 | 77000 | Divine Abduction |
| 행방불명 | 77100 | Double Divine Abduction |
| 행방불명 | 77200 | Golden Vortex |
| 장치 창 | 78000 | Loaded Spear |
| 장치 창 | 78100 | Loaded Spear Thrust type |
| 장치 창 | 78200 | Loaded Spear Cleave type |
| 장치 창 | 78300 | Spiral Spear |
| 장치 창 | 78400 | Leaping Flame |
| 손가락 피리 | 79000 | Finger Whistle |
| 손가락 피리 | 79100 | Mountain Echo |
| 손가락 피리 | 79200 | Malcontent |

## 현재 관측값에 대한 판정

- 2449 = 0x0991, 2701 = 0x0A8D. 두 값은 이 공개 장비 ID 목록과 직접 일치하지 않는다. 어느 쪽이 어떤 의수인지는 현재 자료로 확정하지 않았다.
- 기본 표창의 공개 장비 ID는 70000, 기본 사비마루는 75000이다. u16의 최댓값은 65535이므로 위 40개 ID를 u16으로 온전히 표현할 수 없다.
- 70000의 하위 16비트는 4464, 75000의 하위 16비트는 9464다. 2449/2701을 이 두 기본 장비 ID의 단순 하위 16비트라고 설명할 수도 없다.
- +0xf2가 독립적인 u16 필드일 수 있지만, 더 큰 정수나 포인터 일부일 가능성도 아직 배제되지 않았다. 현재로서는 의수 전환과 상관된 후보 값이다.
- +0xf2와 +0x232가 같은 값으로 변한다는 사실만으로 슬롯 배열인지, 현재 선택 상태의 복사본인지, 캐시나 포인터인지 결정할 수는 없다.
- fromsoftware-rs는 ItemId를 u32로 표현하고 상위 비트를 category, 나머지를 param ID로 분리한다. 이는 또 다른 표현 체계가 존재한다는 자료이며, 관측한 +0xf2의 타입을 입증하는 것은 아니다. 실제 읽은 필드의 의미를 확인하기 전에 마스크나 형변환을 적용하지 않는다.

## 클로드 코드에 전달할 지시

공개 의수 카탈로그를 먼저 활용해 모든 의수 이름을 하나씩 사용자에게 물어보는 작업을 줄여줘. 첨부된 Sekiro_Prosthetic_ID_Catalog.json은 참고 데이터이며 앱 설정 파일로 바로 교체하지 마라.

현재 EquipMagicData+0xf2의 u16 값 2449/2701은 정식 EquipParamWeapon ID로 확정하지 마라. 기존 읽기 전용 probe로 이 필드 주변의 raw byte를 보존하고, +0xf0/+0xf4 주변의 32/64비트 해석도 비교해 독립 필드인지 정수·포인터의 일부인지 확인해라. 해당 주소가 실제 EquipMagicData인지도 기존 포인터 경로와 함께 재확인해라. 포인터 후보는 유효한 관측 영역인지 확인한 뒤 조사해라.

관측값을 현재 선택된 의수의 전체 장비 ID/검증된 매개 식별자와 연결해라. 슬롯 위치를 바꿔 같은 의수를 선택하고, 다른 의수를 같은 슬롯에 넣고, 로드·프로세스 재시작 뒤에도 대응이 유지되는지 확인해라. 두 값 중 현재 이름을 한 번 확인하는 것은 보조 근거가 될 수 있지만, 그 대응만으로 전 빌드·세이브에 고정하지 마라.

전체 EquipParamWeapon ID를 읽는 경로가 검증되면 40개 ID를 정확한 조회표로 매핑해라. 계열 판정도 명시적 목록을 사용하고, 미등록 ID는 unknown으로 남겨라. 공개 목록과 일치하는 것과 실기에서 확인한 것을 따로 기록해라. 선택 의수를 알아내는 것은 실제 사용·발사·피격 판정을 완성한 것이 아니다.

이 조사는 기존 정상 실행 명령과 적 반응 오검출 수정 요구를 유지하면서 진행한다.

    .\build-hw\apps\guard_feedback\sekiro_guard_feedback.exe --live --pid (Get-Process sekiro).Id --enemy

적끼리의 전투 및 표창 때문에 발생한 적 방어·패링 피드백은 제외해야 한다. 의수 선택값을 얻었다고 이 문제가 자동 해결됐다고 보지 마라. 표창을 선택한 채 칼로 공격한 정상 반응은 유지해야 한다.

## 출처

- [paramdex](https://github.com/soulsmods/Paramdex/blob/ff7245e524329bc3eab00036723d2bd53384cedf/SDT/Names/EquipParamWeapon.txt) — revision ff7245e524329bc3eab00036723d2bd53384cedf
- [practice_ct](https://github.com/ElaDiDu/Sekiro-Practice-CT/blob/328065da6dc3c5c9138cd31030791804e8e14e4e/Ela_Sekiro_Table.CT) — revision 328065da6dc3c5c9138cd31030791804e8e14e4e
- [item_id_layout](https://github.com/vswarte/fromsoftware-rs/blob/59fbd3b3b7daaf14aca47c9f73530493dba6bc79/crates/sekiro/src/sprj/item_id.rs) — revision 59fbd3b3b7daaf14aca47c9f73530493dba6bc79
