"""기본 프리셋을 만들어 presets/haptic_lab/ 에 넣는다.

손으로 JSON 을 쓰지 않고 실제 엔진에 세운 뒤 dump 를 받아 저장한다. 그래야
파일이 앱이 만들어 내는 것과 정확히 같은 형식이고, 만드는 도중에 검증기가
거절하면 바로 드러난다.

이 값들은 **설계 후보**다. 파형이 어떤 모양이어야 할지를 생각해서 만든 것이지
실기에서 재보고 맞춘 것이 아니다. 손으로 판단한 뒤 고치라고 넣어 두는 것이다.
"""
import json
import os
import subprocess
import sys

ROOT = r'C:\workspace\sekiro_haptics'
EXE = os.path.join(ROOT, 'build-hw', 'apps', 'haptic_lab', 'sekiro_haptic_lab.exe')
OUT = os.path.join(ROOT, 'presets', 'haptic_lab')

# (파일이름, 표시이름, 같이 낼 소리, 소리 늦추기 ms, 레이어들)
#
# 레이어: (이름, 명령 토큰들, 수식 또는 None)
PRESETS = [
    ("01_deflect", "패링 (칼 튕김)", "audio/generated/deflect_cue.wav", 0, [
        # 날카롭게 시작해서 금속처럼 울리고 빠르게 죽는다. 고역이 먼저 사라지고
        # 중역 울림이 남도록 감쇠율을 다르게 줬다.
        ("첫접촉", "type=impulse freq=420 start=0 dur=70 amp=0.60 decayrate=110", None),
        ("금속울림", "type=impulse freq=260 start=0 dur=260 amp=0.32 decayrate=28", None),
        ("몸통", "type=impulse freq=95 start=0 dur=120 amp=0.39 decayrate=45", None),
        ("잔향", "type=chirp freq=300 freq2=170 start=40 dur=200 amp=0.13", None),
    ]),
    ("02_block", "방어 (일반 막기)", "audio/generated/block_cue.wav", 0, [
        # 패링과 반대로: 울림이 없고 낮고 둔하게 밀린다. 고역 레이어는 아주 짧게
        # 넣어 "맞긴 맞았다" 만 남기고 바로 죽인다.
        ("충격", "type=impulse freq=70 start=0 dur=220 amp=0.80 decayrate=30", None),
        ("둔탁", "type=impulse freq=140 start=0 dur=90 amp=0.40 decayrate=70", None),
        ("눌림", "type=sine freq=45 start=0 dur=180 amp=0.35 env=1 atk=12 hold=0 dec=120 sus=0.25 rel=60", None),
    ]),
    ("03_shuriken", "수리검 (기본 수리검)", None, 0, [
        # 가볍고 짧다. 의수는 왼팔이므로 왼쪽으로 치우치게 뒀다 -- 이건 게임의
        # 설정을 따른 것이지, 그렇게 느껴진다고 측정한 것이 아니다.
        ("발사", "type=impulse freq=300 start=0 dur=40 amp=0.60 decayrate=150 left=1.0 right=0.35", None),
        ("장전걸림", "type=impulse freq=520 start=0 dur=20 amp=0.35 decayrate=260 left=1.0 right=0.20", None),
        ("회전", "type=am freq=180 modhz=60 moddepth=0.9 start=35 dur=90 amp=0.22 left=1.0 right=0.30", None),
    ]),
    ("04_spear", "장창 (기계 장창)", None, 0, [
        # 찌르고 -> 끌리고 -> 회수. 세 동작이 시간으로 떨어져 있어서 한 프리셋
        # 안에서 순서가 그대로 보인다.
        ("찌름", "type=chirp freq=70 freq2=160 start=0 dur=130 amp=0.70 left=1.0 right=0.60", None),
        ("끌림", "type=am freq=55 modhz=11 moddepth=0.8 start=130 dur=320 amp=0.45 env=1 atk=30 hold=120 dec=90 sus=0.5 rel=80 left=1.0 right=0.55", None),
        ("회수", "type=impulse freq=120 start=430 dur=110 amp=0.40 decayrate=45", None),
    ]),
    ("05_axe", "도끼 (기계 도끼)", None, 0, [
        # 무겁다 = 낮고 길다. 휘두르는 동안 주파수가 올라가고, 충돌은 그 뒤에
        # 온다. 저역 꼬리를 길게 끌어서 "덩치" 를 만든다.
        ("휘두름", "type=chirp freq=30 freq2=90 start=0 dur=160 amp=0.20", None),
        ("충격", "type=impulse freq=40 start=160 dur=480 amp=0.76 decayrate=11", None),
        ("날박힘", "type=impulse freq=170 start=160 dur=140 amp=0.44 decayrate=60", None),
    ]),
    ("06_flame", "화염통 (기름 분사)", None, 0, [
        # 때리는 것이 아니라 계속 나오는 것. 시작이 뾰족하면 안 되므로 포락선의
        # 상승을 길게 줬다.
        ("불꽃", "type=am freq=48 modhz=17 moddepth=0.7 start=0 dur=900 amp=0.50 env=1 atk=90 hold=420 dec=140 sus=0.8 rel=250", None),
        ("쉭소리", "type=am freq=130 modhz=31 moddepth=1.0 start=0 dur=900 amp=0.18 env=1 atk=120 hold=400 dec=140 sus=0.7 rel=240", None),
    ]),
    ("07_umbrella", "우산 (철 우산)", None, 0, [
        # 펼치는 순간의 덜컹 + 버티는 동안의 낮은 유지.
        ("펼침", "type=impulse freq=90 start=0 dur=160 amp=0.70 decayrate=40", None),
        ("버팀", "type=sine freq=38 start=120 dur=600 amp=0.30 env=1 atk=60 hold=300 dec=120 sus=0.5 rel=200", None),
    ]),
    ("08_firecracker", "불꽃놀이 (폭죽)", None, 0, [
        # 연달아 터지는 것을 타이머가 아니라 레이어의 시작 시각으로 만든다.
        ("탁1", "type=impulse freq=260 start=0 dur=70 amp=0.55 decayrate=120", None),
        ("탁2", "type=impulse freq=310 start=55 dur=70 amp=0.60 decayrate=120", None),
        ("탁3", "type=impulse freq=230 start=115 dur=70 amp=0.50 decayrate=120", None),
        ("탁4", "type=impulse freq=350 start=160 dur=70 amp=0.62 decayrate=120", None),
        ("탁5", "type=impulse freq=280 start=215 dur=90 amp=0.45 decayrate=90", None),
        ("깔림", "type=sine freq=52 start=0 dur=320 amp=0.22 env=1 atk=10 hold=120 dec=110 sus=0.4 rel=80", None),
    ]),
    ("09_tool_switch", "의수 변경 알림", None, 0, [
        # 무기의 성격을 흉내내지 않는다. 이건 "바뀌었다" 만 알리는 신호라,
        # 짧고 중립적이고 무엇으로 바꿨든 같아야 한다. 두 번 톡 치는 모양이
        # 한 번짜리보다 다른 이벤트와 헷갈리지 않는다.
        ("톡1", "type=impulse freq=190 start=0 dur=45 amp=0.38 decayrate=160", None),
        ("톡2", "type=impulse freq=260 start=55 dur=45 amp=0.30 decayrate=160", None),
    ]),
    ("90_formula_a", "예제 A — cos(2t)·sin(460t)", None, 0, [
        ("A", "type=formula start=0 dur=400 amp=0.8 t0=0.05", "cos(2*t)*sin(460*t)"),
    ]),
    ("91_formula_b", "예제 B — cos(60(t−t0))/t", None, 0, [
        ("B", "type=formula start=0 dur=600 amp=0.045 t0=0.05",
         "if(t < t0, 0, cos(60*(t - t0))/t)"),
    ]),
]

BASE_NOTE = ("설계 후보입니다. 파형이 어떤 모양이어야 할지 생각해서 만든 값이고, "
             "실기에서 재보거나 손으로 확인해 맞춘 것이 아닙니다. 듣고 느껴 본 뒤 고치세요.")

NOTES = {
    "01_deflect": "고역이 먼저 죽고 중역 울림이 남도록 레이어마다 감쇠율을 다르게 줬습니다. "
                  "합이 1.0 을 넘지 않게 맞춰 뒀으니, 약하면 마스터 게인부터 올리세요.",
    "02_block": "패링과 반대로 만든 것입니다. 울림이 없고, 고역 레이어는 '맞긴 맞았다' 만 "
                "남기고 바로 죽습니다. 패링과 번갈아 재생해 차이가 나는지 보세요.",
    "03_shuriken": "가볍고 짧습니다. 의수가 왼팔이라는 게임 설정을 따라 왼쪽으로 치우쳐 "
                   "있습니다 — 그렇게 느껴진다고 측정한 것이 아닙니다.",
    "04_spear": "찌름 → 끌림 → 회수가 시간으로 떨어져 있습니다. 레이어 시간 배치에서 "
                "세 동작이 그대로 보입니다.",
    "05_axe": "무겁다 = 낮고 길다. 휘두르는 동안 주파수가 올라가고 충돌은 그 뒤에 옵니다. "
              "저역 꼬리가 길어서 덩치가 생깁니다.",
    "06_flame": "때리는 것이 아니라 계속 나오는 것이라, 포락선 상승을 길게 줘서 시작이 "
                "뾰족하지 않게 했습니다.",
    "07_umbrella": "펼치는 순간의 덜컹 + 버티는 동안의 낮은 유지.",
    "08_firecracker": "연달아 터지는 것을 타이머가 아니라 레이어의 시작 시각으로 만들었습니다. "
                      "간격을 바꾸려면 각 레이어의 시작 시각을 끌면 됩니다.",
    "09_tool_switch": "의수를 바꾸는 순간에만 울리는 알림입니다. 무기의 성격을 흉내내지 "
                      "않습니다 — 그건 쐈을 때 오는 것이고, 이건 UI 를 보지 않고 무엇을 "
                      "골랐는지 알기 위한 것뿐입니다. 어느 의수로 바꾸든 같은 진동입니다.",
    "90_formula_a": "수식 편집기 예제. 괄호 안은 라디안이라 sin(460*t) 는 460 Hz 가 아니라 "
                    "약 73 Hz 입니다.",
    "91_formula_b": "수식 편집기 예제. t 가 t0 에 가까울 때 1/t 가 커져서 원래대로면 peak 가 "
                    "10 까지 갑니다. 그대로 두면 리미터가 -19 dB 를 먹어 무엇을 듣는지 알 수 "
                    "없으므로 진폭을 0.045 로 낮춰 두었습니다. 올려 보면 리미터가 어떻게 "
                    "동작하는지 분석 패널에서 볼 수 있습니다.",
}

p = subprocess.Popen([EXE, '--force'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                     encoding='utf-8', cwd=ROOT)


def read(expect=None, timeout=25.0):
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = p.stdout.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        if msg.get('event') == 'rejected':
            raise SystemExit('거절됨: %s' % msg['message'])
        if expect is None or msg.get('event') == expect:
            return msg
    raise SystemExit('응답 없음 (%s)' % expect)


def send(line):
    p.stdin.write(line + '\n')
    p.stdin.flush()


read('ready')
os.makedirs(OUT, exist_ok=True)
made = []

skipped = []

for stem, title, wav, offset, layers in PRESETS:
    # 손으로 맞춘 파일은 덮지 않는다.
    #
    # 한 번 덮어서 사용자가 워크벤치에서 맞춘 값을 전부 날린 적이 있다. 이
    # 스크립트는 처음 한 벌을 만들어 두기 위한 것이고, 그 뒤로는 워크벤치에서
    # 고친 것이 정본이다. designStatus 가 그렇게 말하는 파일은 건너뛴다.
    target = os.path.join(OUT, stem + '.json')
    if os.path.exists(target):
        try:
            with open(target, encoding='utf-8') as existing:
                if json.load(existing).get('designStatus') == 'hand_tuned_by_user':
                    skipped.append(title)
                    continue
        except (OSError, json.JSONDecodeError):
            pass

    send('preset.clear')
    read('preset')
    send('preset.name name=%s master=1.0' % title.replace(' ', '_'))
    read('preset')
    for index, (name, tokens, formula) in enumerate(layers):
        send('layer.add name=%s %s' % (name, tokens))
        read('preset')
        if formula:
            send('layer.formula %d %s' % (index, formula))
            read('preset')
    if wav:
        send('speaker.load %s' % wav)
        read('speaker')
        send('speaker.set gain=1.0 offset=%d on=1' % offset)
        read('speaker')
    else:
        send('speaker.clear')
        read('speaker')

    # 정말 렌더되는지, 어떤 값이 나오는지 확인한 뒤에 저장한다.
    send('preset.analyze points=120')
    a = read('analysis')

    send('preset.dump')
    document = read('preset')['preset']
    document['name'] = title
    document['note'] = NOTES.get(stem, BASE_NOTE)
    document['designStatus'] = 'design_candidate_not_hand_verified'
    document['output'] = {'endpointId': None, 'hapticLeft': None, 'hapticRight': None,
                          'sampleRateHz': 48000}
    document['analysis'] = {'fftSize': 2048, 'window': 'hann', 'startFrame': 0,
                            'unit': 'PCM 진폭'}
    path = os.path.join(OUT, stem + '.json')
    with open(path, 'w', encoding='utf-8') as out:
        json.dump(document, out, ensure_ascii=False, indent=2)
    made.append((title, a['lengthMs'], a['statsLeft']['peak'], a['statsLeft']['rms'],
                 a['statsLeft']['overRange'], a['worstReductionDbLeft'],
                 wav or '-'))

send('quit')
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()

print('%-26s %8s %8s %8s %8s %8s  %s'
      % ('프리셋', '길이ms', 'peak', 'rms', '범위초과', '리미터dB', '같이 낼 소리'))
print('-' * 100)
for row in made:
    print('%-26s %8.0f %8.3f %8.3f %8d %8.2f  %s'
          % (row[0], row[1], row[2], row[3], row[4], row[5], os.path.basename(row[6])))
print('\n%d개 저장: %s' % (len(made), OUT))
if skipped:
    print('%d개는 손으로 맞춘 파일이라 건너뜀: %s' % (len(skipped), ', '.join(skipped)))
