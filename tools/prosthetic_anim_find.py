#!/usr/bin/env python3
"""READ-ONLY: 의수를 쓸 때 재생되는 **애니메이션**을 알려 주는 필드를 찾는다.

왜 애니메이션인가
-----------------
처음에는 영자(소모 구슬)를 보려 했는데 그건 틀린 접근이다. 영자는 "무언가를
썼다" 는 양일 뿐이라 **무엇을** 썼는지 구분하지 못하고, 강화형·무료 사용·다른
소모처와도 섞인다.

애니메이션 id 는 그 자체가 동작이다. 수리검을 던지는 동작과 창을 내미는 동작은
서로 다른 id 이고, 칼을 휘두르는 것과도 다르다. 그래서 "쐈다" 와 "무엇을 쐈다"
가 한 값에서 같이 나온다.

무엇을 보는가
-------------
플레이어의 모듈 컨테이너에서 행동 관련 모듈들을 훑는다. 컨테이너는
SprjPlayerDamageModule 이 같이 들어 있는 것만 받아들인다 -- 이 저장소가
DEFLECT_STATUS.md 6.5 에서 쓴 것과 같은 소유권 증명이고, "vptr 이 맞는 첫
객체" 를 잡는 실수를 막는다.

어떻게 거르는가
---------------
애니메이션 id 는 **그 동작 중에만 특정 값을 가진다.** 그래서 단계마다 각 자리가
가진 값들의 집합을 모아 두고:

    수리검 단계들에서 공통으로 나온 값
      - 가만히 있는 단계에서 나온 값
      - 칼질 단계에서 나온 값        <- 음성 대조

을 남긴다. 음성 대조가 핵심이다. 그게 없으면 "움직이면 바뀌는 아무 값" 과
구분되지 않는다.

찾아도 그 자리에 이름을 붙이지 않는다. 관측한 것은 "이 동작 중에 이 값이었다"
뿐이고, 그것이 곧 애니메이션 id 라는 보장은 여기서 나오지 않는다.

쓰는 법
-------
    python tools/prosthetic_anim_find.py --pid <sekiro pid>

게임을 읽기만 한다. 아무것도 쓰지 않는다.
"""
import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from module_watch import IMAGE_BASE, PlayerModules, Reader, VFTABLE_MAP  # noqa: E402

# 동작이 실릴 만한 모듈들. 어디에 있는지 모르므로 몇 개를 같이 본다.
MODULES = [
    'SprjChrBehaviorModule',
    'SprjChrActionFlagModule',
    'SprjChrActionRequestModule',
]
WINDOW = 0x400        # 모듈마다 이만큼 훑는다


def sample(reader, addrs, span):
    """모듈마다 span 바이트를 통으로 읽어 (모듈, 오프셋) -> u32 로."""
    out = {}
    for name, addr in addrs.items():
        raw = reader.read(addr, span)
        if raw is None:
            return None
        for off in range(0, span - 3, 4):
            out[(name, off)] = int.from_bytes(raw[off:off + 4], 'little')
    return out


def collect(reader, resolver, span, seconds, label):
    """한 단계 동안 각 자리가 가진 값들의 집합."""
    seen = {}
    deadline = time.perf_counter() + seconds
    reads = 0
    while time.perf_counter() < deadline:
        got, why = resolver.resolve()
        if got is None:
            time.sleep(0.01)
            continue
        addrs = {name: got[name][0] for name in got}
        values = sample(reader, addrs, span)
        if values is None:
            time.sleep(0.005)
            continue
        reads += 1
        for key, value in values.items():
            seen.setdefault(key, set()).add(value)
        time.sleep(0.005)
    print('   %s: %d 회 읽음, %d 자리' % (label, reads, len(seen)))
    return seen


def watch(reader, resolver, specs, seconds):
    """정해진 자리만 보고 값이 **바뀔 때마다** 한 줄 찍는다.

    넓게 훑는 단계에서 후보가 나온 뒤, 그것이 정말 "무엇을 썼는가" 인지 보는
    단계다. 무기를 바꿔 가며 눌러 보면 값이 따라 바뀌는지 바로 보인다.
    """
    wanted = []
    for spec in specs:
        if '+' not in spec:
            print('형식이 다릅니다: %s (예: SprjChrBehaviorModule+0x6c)' % spec)
            return 1
        name, off = spec.split('+', 1)
        wanted.append((name.strip(), int(off, 0)))

    print()
    print('%g 초 동안 봅니다. 값이 바뀔 때만 찍습니다.' % seconds)
    print('수리검 몇 번 -> 창으로 바꿔 몇 번 -> R2 만 눌렀다 떼기 -> 칼질 순으로 해보세요.')
    print()
    last = {}
    started = time.perf_counter()
    while time.perf_counter() - started < seconds:
        try:
            got, why = resolver.resolve()
        except Exception as problem:          # 로딩/객체 교체 중에는 실패가 정상이다
            print('  (일시적 해석 실패: %s)' % problem)
            time.sleep(0.05)
            continue
        if got is None:
            time.sleep(0.01)
            continue
        for name, off in wanted:
            if name not in got:
                continue
            raw = reader.read(got[name][0] + off, 4)
            if raw is None:
                continue
            value = int.from_bytes(raw, 'little')
            key = (name, off)
            if last.get(key) != value:
                # 처음 본 값은 기준선이라 화살표 없이 적는다.
                if key in last:
                    print('  %7.2fs  %-28s +0x%04x   %d -> %d'
                          % (time.perf_counter() - started, name, off, last[key], value))
                else:
                    print('  %7.2fs  %-28s +0x%04x   %d (시작값)'
                          % (time.perf_counter() - started, name, off, value))
                last[key] = value
        time.sleep(0.005)
    print()
    print('수리검일 때와 창일 때 값이 **다르면** 그 자리가 "무엇을 썼는가" 입니다.')
    print('같으면 "의수를 썼다" 까지만 아는 것이고, R2 만 눌렀을 때도 바뀌면 입력이지 동작이 아닙니다.')
    return 0


def count_pulses(reader, resolver, seconds, off, name):
    """정해진 횟수만큼 하고, 펄스가 몇 번 잡히는지 센다.

    한 번의 동작이 요청을 몇 번 내는지 모르면 "한 번 던졌다" 를 셀 수 없다.
    이 저장소는 가까운 것을 시간으로 뭉치지 않기로 했으므로(그건 진짜 연타를
    지워 버린다), 대신 **몇 번 하면 몇 개가 잡히는지** 를 재서 규칙을 고른다.
    """
    print()
    print('%g 초 동안 봅니다. 시키는 횟수를 정확히 지켜 주세요.' % seconds)
    print('평상시 값(790010/790040/0)은 무시하고, 의수/칼 계열 번호만 셉니다.')
    print()
    runs = []          # (값, 시작초, 펄스수)
    last = None
    # 잡는 즉시 한 줄씩 찍는다. 끝에서만 보여 주면, 던지는 30초 동안 화면이
    # 가만히 있어서 "아무것도 안 잡힌다" 로 보이고, 중간에 Ctrl+C 로 끊으면
    # 요약도 사라진다 -- 실제로 그렇게 읽혔다.
    FAMILY = {70: '수리검', 73: '도끼', 78: '장창', 50: '칼'}
    started = time.perf_counter()
    next_beat = 5.0
    try:
        while time.perf_counter() - started < seconds:
            at = time.perf_counter() - started
            if at >= next_beat:
                print('  ... %2.0f초 지남, 지금까지 %d 구간' % (at, len(runs)))
                next_beat += 5.0
            try:
                got, why = resolver.resolve()
            except Exception:
                time.sleep(0.05)
                continue
            if got is None or name not in got:
                time.sleep(0.01)
                continue
            raw = reader.read(got[name][0] + off, 4)
            if raw is None:
                continue
            value = int.from_bytes(raw, 'little')
            # 동작 계열만: 1천만 이상이면 <장비><동작> 꼴이다. 평상시 값
            # (790010/790040/0)은 훨씬 작다.
            family = value // 1000000 if value >= 10000000 else None
            if family is None:
                last = None
                time.sleep(0.001)
                continue
            if runs and last == value:
                runs[-1][2] += 1
            else:
                runs.append([value, at, 1])
                print('  %6.2fs  %-6s %d' % (at, FAMILY.get(family, '계열%d' % family), value))
            last = value
            time.sleep(0.001)
    except KeyboardInterrupt:
        print('\n  (중간에 멈춤 -- 여기까지 잡은 것으로 요약합니다)')

    print()
    print('%-12s %-10s %-8s %s' % ('시각', '값', '계열', '연속 폴'))
    for value, at, hits in runs:
        print('%10.2fs %-10d %-8d %d' % (at, value, value // 1000000, hits))
    print()
    print('서로 다른 번호: %d 개, 구간: %d 개'
          % (len({v for v, _a, _h in runs}), len(runs)))
    print('시킨 횟수와 구간 수가 맞으면 "번호가 바뀔 때 한 번" 이 규칙입니다.')
    print('구간이 더 많으면 같은 번호가 여러 번 요청되는 것이라, 같은 번호가')
    print('이어지는 동안은 한 번으로 봐야 합니다.')
    return 0


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, required=True)
    ap.add_argument('--base', type=lambda x: int(x, 0), default=IMAGE_BASE)
    ap.add_argument('--span', type=lambda x: int(x, 0), default=WINDOW)
    ap.add_argument('--seconds', type=float, default=3.0,
                    help='각 단계를 몇 초 동안 볼지 (기본 3)')
    ap.add_argument('--watch', nargs='*', metavar='Module+0xOFF',
                    help='넓게 훑는 대신 정해진 자리만 보고 값이 바뀔 때마다 찍는다. '
                         '예: --watch SprjChrActionRequestModule+0xc8 '
                         'SprjChrBehaviorModule+0x6c')
    ap.add_argument('--watch-seconds', type=float, default=60.0)
    ap.add_argument('--count', metavar='Module+0xOFF',
                    help='한 자리만 보면서 동작 계열 번호의 구간 수를 센다. '
                         '정확히 N 번 하고 구간이 N 개인지 보는 용도.')
    args = ap.parse_args()

    by_rva = {int(k, 16): v for k, v in
              json.loads(VFTABLE_MAP.read_text(encoding='utf-8')).items()}
    reader = Reader(args.pid)
    resolver = PlayerModules(reader, args.base, by_rva, MODULES)
    got, why = resolver.resolve()
    if got is None:
        print('플레이어 모듈을 찾지 못했습니다: %s' % why)
        return 1
    for name in got:
        print('%-28s 0x%x' % (name, got[name][0]))
    print('모듈마다 0x%x 바이트씩 봅니다.' % args.span)
    print()
    print('각 단계는 %g 초입니다. Enter 를 누른 **직후부터** 재니까,'
          % args.seconds)
    print('누르고 바로 시키는 동작을 반복하세요.')
    print()

    if args.count:
        name, off = args.count.split('+', 1)
        return count_pulses(reader, resolver, args.watch_seconds, int(off, 0), name.strip())
    if args.watch:
        return watch(reader, resolver, args.watch, args.watch_seconds)

    phases = []

    def run(prompt, label):
        input('  %s -> Enter: ' % prompt)
        phases.append((label, collect(reader, resolver, args.span, args.seconds, label)))

    run('가만히 서 있기', 'idle1')
    run('수리검 던지기를 반복', 'shuriken1')
    run('가만히 서 있기', 'idle2')
    run('수리검 던지기를 반복', 'shuriken2')
    run('칼로만 공격 (의수 금지)', 'sword')
    run('달리기/점프만', 'move')

    shuriken = [dict(v) for label, v in phases if label.startswith('shuriken')]
    negatives = [v for label, v in phases if not label.startswith('shuriken')]

    keys = set(shuriken[0]) if shuriken else set()
    for other in shuriken[1:]:
        keys &= set(other)

    hits = []
    for key in keys:
        # 두 수리검 단계에 **공통으로** 나온 값들
        common = set(shuriken[0][key])
        for other in shuriken[1:]:
            common &= set(other[key])
        if not common:
            continue
        # 음성 단계 어디에도 나오지 않은 값만
        for neg in negatives:
            common -= set(neg.get(key, ()))
        if common:
            hits.append((key, sorted(common)))

    print()
    if not hits:
        print('수리검 때만 나온 값이 없습니다.')
        print('  - --span 을 키워 보세요 (예: 0x1000)')
        print('  - --seconds 를 늘려 던지기를 더 많이 담아 보세요')
        print('  - 동작이 다른 모듈에 실릴 수도 있습니다')
        return 1

    print('수리검 단계에만 나온 값 %d 자리:' % len(hits))
    for (name, off), values in sorted(hits)[:40]:
        shown = ', '.join(str(v) for v in values[:6])
        print('   %-28s + 0x%04x   %s%s'
              % (name, off, shown, ' ...' if len(values) > 6 else ''))
    print()
    print('다음으로 확인할 것 (이것까지 맞아야 씁니다):')
    print('  - 창으로 바꿔 같은 단계를 돌렸을 때 **다른** 값이 나오는가')
    print('    (같은 값이면 그건 "의수를 썼다" 지 "무엇을 썼다" 가 아니다)')
    print('  - 던지지 않고 R2 만 눌렀다 뗐을 때는 안 나오는가')
    print('  - 같은 자리가 다음 세션에서도 같은 값을 주는가')
    return 0


if __name__ == '__main__':
    sys.exit(main())
