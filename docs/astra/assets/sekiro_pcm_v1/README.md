# Sekiro PCM v1 — 사용 순서

1. `sekiro_pcm_v1` 폴더를 `C:\workspace\sekiro_haptics\assets\` 아래에 둔다.
2. Claude에게 `INTEGRATE.md`와 `preset_map.json`을 읽고 기존 출력에 연결하게 한다. 파형은 이미 생성돼 있다.
3. 설명이 궁금하면 `DESIGN.md`, 정확한 수치는 `waveform_layers.csv` 또는 `recipes.json`을 본다.

22개 48kHz/stereo/float32 WAV, 의수 10계열+2변형, 패링/방어, 조건부 추가 동작/루프를 포함한다. 작성용 JSON은 앱 설정 파일과 다른 형식이다. 실제 지원 여부는 현재 저장소의 검증된 신호에 따라 달라진다. 미확인 동작은 수동 비교용으로 둔다.

검증은 `validation.json`, `validation_summary.md`에 있다. 실제 DualSense 체감·Windows 실행·게임 감지는 이 묶음에서 검증하지 않았다. `waveforms.png`는 두 기본 파형의 비교 그림이다.
