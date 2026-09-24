#!/usr/bin/env python3
"""Which SprjChrActionFlagModule instances actually pulse, and who owns them?

WHY THIS EXISTS
---------------
Two different ways of resolving an enemy's module have now been wrong in two
different directions:

  * graph walk, 2 hops, with the SprjEnemyDamageModule ownership test
        -> produced events, but 104 characters resolved to only 75 modules;
           24 modules were shared, so some characters were reading a
           neighbour's object.
  * a single measured offset pair (ChrIns + 0x1B58 -> container + 0xD0)
        -> 81 characters, 81 distinct modules, no sharing at all...
           and ZERO events during a 60 s melee. Unique but wrong.

Guessing a third path would be the same mistake again. So this script stops
resolving anything and measures the ground truth instead:

  1. collect EVERY object whose vptr is SprjChrActionFlagModule, wherever it
     sits in the graph -- no ownership assumption at all;
  2. watch all of them for a 0 -> non-zero edge on +0x3C;
  3. for each module that ACTUALLY pulsed, report every EnemyIns that reaches
     it and at which (ChrIns offset, container offset).

The offsets that show up under real pulses are the real path. Everything else
in this file is deliberately assumption-free.

READ-ONLY. OpenProcess(VM_READ|QUERY) and ReadProcessMemory only.
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")
import enemy_guard_probe as P

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def collect(reader, base, world, budget=120_000):
    """Every ActionFlagModule and every EnemyIns reachable from WorldChrMan.

    Deliberately one hop deeper and with a larger budget than the character
    walk: a module the characters cannot reach is exactly the kind of thing
    that would have been missed by every resolver so far.
    """
    af_want = base + P.ACTION_FLAG_VFTABLE_RVA
    enemy_want = base + P.ENEMY_INS_VFTABLE_RVA
    modules, enemies = [], []
    seen = {world}
    frontier = [world]
    for depth in range(4):
        nxt = []
        for node in frontier:
            blk = reader.read(node, P.SEARCH_WIDTH)
            if blk is None:
                continue
            for o in range(0, len(blk) - 8, 8):
                p, = struct.unpack_from("<Q", blk, o)
                if not P.plausible(p) or p in seen:
                    continue
                seen.add(p)
                v = reader.qword(p)
                if v == af_want:
                    modules.append(p)
                elif v == enemy_want:
                    enemies.append(p)
                    nxt.append(p)
                elif depth < 3:
                    nxt.append(p)
                if len(seen) > budget:
                    return modules, enemies
        frontier = nxt
    return modules, enemies


def owners_of(reader, module, enemies, outer_width, inner_width):
    """Every (EnemyIns, ChrIns offset, container offset) that reaches `module`."""
    found = []
    for chr_ins in enemies:
        blk = reader.read(chr_ins, outer_width)
        if blk is None:
            continue
        for o in range(0, len(blk) - 8, 8):
            cont, = struct.unpack_from("<Q", blk, o)
            if not P.plausible(cont):
                continue
            inner = reader.read(cont, inner_width)
            if inner is None:
                continue
            for j in range(0, len(inner) - 8, 8):
                q, = struct.unpack_from("<Q", inner, j)
                if q == module:
                    found.append((chr_ins, o, j))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=P.IMAGE_BASE)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--interval-ms", type=float, default=5.0)
    args = ap.parse_args()

    r = P.Reader(args.pid)
    loc = P.EnemyGuardLocator(r, args.base)
    world, player, why = loc._roots()
    if world is None:
        raise SystemExit(f"WorldChrMan 사용 불가: {why}")

    print("모든 ActionFlagModule을 수집 중입니다 (소유권 가정 없음)...")
    t0 = time.perf_counter()
    modules, enemies = collect(r, args.base, world)
    enemies = sorted(set(enemies))
    # Second pass: the general walk found 50 modules for 190 characters, so it
    # was not reaching the per-character ones. Scan every character's direct
    # children explicitly. Still no ownership claim -- this only widens the set
    # of modules being WATCHED.
    af_want = args.base + P.ACTION_FLAG_VFTABLE_RVA
    extra = set(modules)
    for chr_ins in enemies:
        blk = r.read(chr_ins, P.SEARCH_WIDTH)
        if blk is None:
            continue
        for o in range(0, len(blk) - 8, 8):
            cont, = struct.unpack_from("<Q", blk, o)
            if not P.plausible(cont):
                continue
            inner = r.read(cont, 0x800)
            if inner is None:
                continue
            for j in range(0, len(inner) - 8, 8):
                q, = struct.unpack_from("<Q", inner, j)
                if P.plausible(q) and q not in extra and r.qword(q) == af_want:
                    extra.add(q)
    modules = sorted(extra)
    print(f"  {time.perf_counter()-t0:.1f}초: ActionFlagModule {len(modules)}개, "
          f"EnemyIns {len(enemies)}개")
    if not modules:
        raise SystemExit("ActionFlagModule을 하나도 찾지 못했습니다.")

    print(f"\n>>> {args.seconds:.0f}초간 기록합니다. 평소대로 싸우세요. <<<\n")
    prev = {}
    pulses = Counter()
    # Only a value of exactly 1 has the shape of the real one-frame pulse.
    # The first run recorded 153 "pulses" of which 142 were values like 255,
    # 244 and 238, fired in simultaneous bursts across dozens of modules --
    # that is a freed allocation whose vftable still matches, not guarding.
    shaped = Counter()
    outcomes = defaultdict(Counter)
    order = []
    start = time.perf_counter()
    polls = 0
    worst = 0.0
    last = None
    while time.perf_counter() - start < args.seconds:
        for m in modules:
            b = r.read(m + P.PULSE_OFFSET, 1)
            if b is None:
                continue
            pulse = b[0]
            was = prev.get(m)
            if was == 0 and pulse != 0:
                ob = r.read(m + P.OUTCOME_OFFSET, 1)
                outcome = ob[0] if ob else -1
                pulses[m] += 1
                if pulse == 1:
                    shaped[m] += 1
                outcomes[m][outcome] += 1
                if m not in order:
                    order.append(m)
                print(f"  [{(time.perf_counter()-start)*1000:7.0f}ms] module 0x{m:x} "
                      f"pulse={pulse} outcome={outcome}")
            prev[m] = pulse
        now = time.perf_counter()
        if last is not None:
            worst = max(worst, now - last)
        last = now
        polls += 1
        time.sleep(args.interval_ms / 1000.0)

    print(f"\n--- {sum(pulses.values())}회(원시), 서로 다른 모듈 {len(pulses)}개 ---")
    print(f"    값이 정확히 1: {sum(shaped.values())}회, 모듈 {len(shaped)}개"
          f"   <- 진짜 펄스의 모양")
    print(f"    polls={polls}  worst poll gap={worst*1000:.1f} ms")
    if shaped:
        # Back-trace ONLY the correctly shaped ones. Tracing the rest would
        # bury the answer under memory churn.
        order = [m for m in order if m in shaped]
    if not pulses:
        print("    펄스가 전혀 없었습니다. 수집 범위 밖이거나 +0x3C가 아닐 수 있습니다.")
        r.close()
        return 0

    # The player's own module, so it is never mistaken for an enemy's.
    # enemy_guard_probe has no ActionFlagLocator; module_watch resolves it the
    # proven way (a container that also holds SprjPlayerDamageModule).
    player_module = 0
    try:
        import json as _json
        import module_watch as MW
        _map = {int(k, 16): v for k, v in _json.loads(
            MW.VFTABLE_MAP.read_text(encoding="utf-8")).items()}
        _pm = MW.PlayerModules(r, args.base, _map, ["SprjChrActionFlagModule"])
        _got, _why = _pm.resolve()
        if _got:
            player_module = _got["SprjChrActionFlagModule"][0]
    except Exception as exc:                       # noqa: BLE001
        print(f"    (플레이어 모듈 확인 실패: {exc})")
    if player_module:
        print(f"    (참고: 플레이어 모듈 = 0x{player_module:x})")

    print("\n실제로 펄스가 난 모듈의 소유 경로:")
    for m in order:
        tag = "  <- PLAYER" if m == player_module else ""
        print(f"\n  module 0x{m:x}  pulses={pulses[m]}  "
              f"outcomes={dict(outcomes[m])}{tag}")
        found = owners_of(r, m, enemies, P.SEARCH_WIDTH, 0x800)
        if not found:
            print("    이 모듈을 가리키는 EnemyIns 없음 (플레이어이거나 수집 범위 밖)")
        for chr_ins, o, j in found[:6]:
            print(f"    EnemyIns 0x{chr_ins:x}  ChrIns+0x{o:X} -> container+0x{j:X}")

    paths = Counter()
    for m in order:
        if m == player_module:
            continue
        for _, o, j in owners_of(r, m, enemies, P.SEARCH_WIDTH, 0x800):
            paths[(o, j)] += 1
    if paths:
        print("\n펄스가 난 적 모듈들이 실제로 걸려 있던 경로:")
        for (o, j), n in paths.most_common(8):
            print(f"    ChrIns+0x{o:X} -> container+0x{j:X}   {n}회")
    r.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
