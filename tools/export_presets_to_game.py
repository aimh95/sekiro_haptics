"""워크벤치 프리셋을 게임(`--live`)이 재생하는 햅틱 파일로 내보낸다.

무엇이 연결되고 무엇이 안 되는가
--------------------------------
게임 경로가 **확인된 신호로 실제 발동시키는 이벤트는 패링과 방어 둘뿐**이다
(docs/astra/results/DEFLECT_EVIDENCE.json). 그래서 여기서 내보내는 것도 그 둘이다.

수리검·창·도끼·우산·불꽃놀이는 프리셋은 있어도 **게임에서 "썼다" 를 알 방법이 아직
없다.** 지금 있는 것은 선택된 의수가 무엇인지(R2 준비 저항)뿐이고, 그건 사용도
명중도 아니다. 그 신호를 찾기 전에는 연결할 것이 없다 -- 타이머로 만들어 내는 것은
이 저장소가 하지 않기로 한 것이다.

레벨을 두 번 건드리지 않기
--------------------------
게임의 blade PCM 경로는 파일을 읽은 뒤 잔향을 붙이고(`--blade-tail-ms`), 게인을
곱하고(`--blade-gain`, 기본 80), 천장으로 접는다(`--blade-ceiling`). 그 기본값은
peak 0.68 짜리 조용한 원본에 맞춰 둔 것이라, 이미 완성된 프리셋을 그대로 넣으면
80배가 곱해져 모양이 사라진다.

그래서 이 스크립트가 출력하는 실행 명령은 그 셋을 **끄고**(게인 1, 잔향 0, 천장
0.99) 쓴다. 프리셋이 이미 워크벤치에서 맞춰 놓은 그대로 나가야 워크벤치에서 들은
것과 게임에서 들리는 것이 같아진다.
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build-hw', 'apps', 'haptic_lab', 'sekiro_haptic_lab.exe')
PRESETS = os.path.join(ROOT, 'presets', 'haptic_lab')
OUT = os.path.join(ROOT, 'assets', 'game_haptics')

# 게임이 찾는 파일 이름 <- 프리셋 파일
WIRED = [
    ('blade_deflect', '01_deflect.json', '패링'),
    ('blade_block', '02_block.json', '방어'),
    # 의수는 "바꿨다" 에만 붙는다. 아래 두 개는 확인된 장비 ID(70000/78000)가
    # 있는 것뿐이고, 발사/명중 신호는 아직 없다.
    # 바꿈 알림: 어느 의수로 가든 이 하나.
    ('tool_switch', '09_tool_switch.json', '의수 변경'),
    # 공격 큐: 동작 코드의 공격 단계(…001xx)에서 울린다.
    ('shuriken', '03_shuriken.json', '수리검(발사)'),
    ('spear', '04_spear.json', '장창(발사)'),
    ('axe', '05_axe.json', '도끼(발사)'),
]

TOKENS = {"startMs": "start", "durationMs": "dur", "frequencyHz": "freq",
          "frequencyEndHz": "freq2", "modulationHz": "modhz", "modulationDepth": "moddepth",
          "decayPerSecond": "decayrate", "amplitude": "amp", "gainDb": "gaindb",
          "startPhaseDeg": "phase", "leftGain": "left", "rightGain": "right",
          "attackMs": "atk", "holdMs": "hold", "decayMs": "dec", "sustain": "sus",
          "releaseMs": "rel", "t0Seconds": "t0"}


def main():
    p = subprocess.Popen([EXE, '--force'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1,
                         encoding='utf-8', cwd=ROOT)

    def read(expect=None, timeout=25.0):
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
            if expect is None or msg.get('event') in (expect, 'rejected'):
                return msg
        raise SystemExit('출력 프로세스가 응답하지 않습니다 (%s)' % expect)

    def send(line):
        p.stdin.write(line + '\n')
        p.stdin.flush()

    read('ready')
    os.makedirs(OUT, exist_ok=True)
    rows = []

    for stem, preset_file, label in WIRED:
        path = os.path.join(PRESETS, preset_file)
        if not os.path.exists(path):
            print('프리셋 없음: %s' % path)
            return 1
        doc = json.load(open(path, encoding='utf-8'))

        send('preset.clear')
        read('preset')
        send('preset.name name=%s master=%s ceiling=%s'
             % (str(doc['name']).replace(' ', '_'), doc['masterGain'],
                doc.get('saturationCeiling', 0.0) or 0.0))
        read('preset')
        for layer in doc['layers']:
            parts = ['layer.add', 'type=%s' % layer['waveform'], 'name=n',
                     'env=%d' % (1 if layer['envelopeEnabled'] else 0),
                     'mute=%d' % (1 if layer['muted'] else 0),
                     'solo=%d' % (1 if layer['solo'] else 0)]
            for key, token in TOKENS.items():
                parts.append('%s=%s' % (token, layer[key]))
            send(' '.join(parts))
            reply = read('preset')
            if reply.get('event') == 'rejected':
                print('%s 거절됨: %s' % (label, reply['message']))
                return 1
        for index, layer in enumerate(doc['layers']):
            if layer['waveform'] == 'formula' and layer['formula']:
                send('layer.formula %d %s' % (index, layer['formula']))
                read('preset')

        send('preset.analyze points=100')
        a = read('analysis')
        send('preset.export path=%s' % os.path.join(OUT, stem).replace('\\', '/'))
        e = read('exported')
        if not e.get('ok'):
            print('%s 내보내기 실패' % label)
            return 1
        rows.append((label, doc['name'], a['lengthMs'], a['statsLeft']['peak'],
                     a['statsLeft']['rms'], e['frames'], e['wav']))

    send('quit')
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()

    print('%-6s %-22s %8s %8s %8s %8s' % ('이벤트', '프리셋', '길이ms', 'peak', 'rms', '샘플'))
    print('-' * 68)
    for label, name, ms, peak, rms, frames, wav in rows:
        print('%-6s %-22s %8.0f %8.3f %8.3f %8d' % (label, name, ms, peak, rms, frames))
    print()
    print('내보낸 곳: %s' % OUT)
    print()
    print('게임에서 쓰는 명령 (게인/잔향/천장을 꺼서 프리셋 그대로 나가게 한다):')
    print()
    print('  .\\build-hw\\apps\\guard_feedback\\sekiro_guard_feedback.exe --live '
          '--pid (Get-Process sekiro).Id --enemy `')
    print('       --blade-pcm-dir assets/game_haptics --blade-gain 1 '
          '--blade-tail-ms 0 --blade-ceiling 0.99')
    print()
    print('의수 선택 큐까지 쓰려면 뒤에 붙인다 (의수를 바꾸는 순간에만 울린다):')
    print()
    print('       --prosthetic-cue-dir assets/game_haptics')
    print()
    print('되돌리려면 붙인 옵션을 빼면 된다 (기본 blade 파형으로 돌아간다).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
