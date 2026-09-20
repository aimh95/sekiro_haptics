#!/usr/bin/env python3
"""READ-ONLY probe: does the enemy side carry the same guard-resolution signal?

WHAT IS ALREADY ESTABLISHED (docs/astra/results/DEFLECT_STATUS.md)
-----------------------------------------------------------------
For the PLAYER, one guard resolution is:

    SprjChrActionFlagModule + 0x3C    one-frame pulse, once per resolution
    SprjChrActionFlagModule + 0xE10   0 = normal block, 1 = deflect

and the module is found by walking down from PlayerIns and accepting only a
container that ALSO holds a SprjPlayerDamageModule -- the player-specific
concrete type. That ownership test is what keeps the player's signal apart
from everyone else's.

WHAT THIS SCRIPT TESTS (and does NOT assume)
--------------------------------------------
SprjChrActionFlagModule is ONE class, shared by every character -- the static
map has a single vftable for it (RVA 0x2A72158), not a player and an enemy
variant. So the same two fields PROBABLY mean the same thing on an enemy.
"Probably" is the whole point of this script: nothing here is wired into the
feedback app until it has been watched on a real fight.

The mirror of the player's ownership test is used: a container reachable from
an EnemyIns that also holds a SprjEnemyDamageModule (RVA 0x2A7D628).

    NS_SPRJ::EnemyIns                 0x2A27F28   (static vftable map)
    NS_SPRJ::SprjChrActionFlagModule  0x2A72158
    SprjEnemyDamageModule             0x2A7D628

WHAT IT CANNOT ANSWER
---------------------
+0xE10 says "this character blocked" or "this character deflected". It does
NOT say WHOSE attack was guarded. An enemy blocking another enemy, or blocking
a shuriken instead of the sword, looks identical here. Deciding that needs a
separate signal and is not solved by this probe -- see the summary it prints.

Nothing is written to the game. Only OpenProcess(VM_READ|QUERY) is used.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

IMAGE_BASE = 0x140000000
WORLD_CHR_MAN_SLOT_RVA = 0x3D7A1E0
WORLD_CHR_MAN_VFTABLE_RVA = 0x2A30058
PLAYER_INS_VFTABLE_RVA = 0x2A2B338
PLAYER_INS_OFFSET = 0x88

ENEMY_INS_VFTABLE_RVA = 0x2A27F28
ACTION_FLAG_VFTABLE_RVA = 0x2A72158
ENEMY_DAMAGE_VFTABLE_RVA = 0x2A7D628
PLAYER_DAMAGE_VFTABLE_RVA = 0x2A7DA48

PULSE_OFFSET = 0x3C
OUTCOME_OFFSET = 0xE10

SEARCH_WIDTH = 0x2000
SEARCH_DEPTH = 2

# This console is cp949; without this every Korean line comes out as mojibake.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wt.HANDLE]


class Reader:
    def __init__(self, pid: int):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise SystemExit(f"OpenProcess({pid}) 실패: {ctypes.get_last_error()} "
                             "(게임이 실행 중인지, pid가 맞는지 확인)")
        self._got = ctypes.c_size_t(0)
        self._q = ctypes.create_string_buffer(8)

    def read(self, addr: int, size: int):
        buf = ctypes.create_string_buffer(size)
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size,
                                   ctypes.byref(self._got))
        return buf.raw if ok and self._got.value == size else None

    def qword(self, addr: int):
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), self._q, 8,
                                   ctypes.byref(self._got))
        return struct.unpack("<Q", self._q.raw)[0] if ok and self._got.value == 8 else None

    def close(self):
        if self.h:
            k32.CloseHandle(self.h)
            self.h = None


def plausible(p) -> bool:
    return p is not None and 0x10000 < p < 0x7FFFFFFFFFFF


class EnemyGuardLocator:
    """Finds every reachable EnemyIns and, for each, its ActionFlagModule.

    Deliberately the same shape as the player locator, including the two
    corrections that one needed:
      * a vptr check does not prove an object is live, so the whole set is
        rebuilt whenever WorldChrMan or PlayerIns changes;
      * "the first module with the right vptr" is arbitrary, so a module is
        only accepted from a container that also holds SprjEnemyDamageModule.
    """

    def __init__(self, reader: Reader, base: int):
        self.r = reader
        self.base = base
        self.last_world = None
        self.last_player = None
        self.enemies: dict[int, int] = {}      # EnemyIns -> ActionFlagModule
        self.searches = 0
        # (offset of the module container inside ChrIns, offset of the module
        # inside that container). Learned from the first enemy that resolves
        # the slow way; every later enemy uses it and still gets vptr-checked.
        self.path = None
        self.last_outcome: dict[int, int] = {}

    def _roots(self):
        world = self.r.qword(self.base + WORLD_CHR_MAN_SLOT_RVA)
        if not plausible(world):
            return None, None, "WorldChrMan null"
        if self.r.qword(world) != self.base + WORLD_CHR_MAN_VFTABLE_RVA:
            return None, None, "WorldChrMan vptr mismatch"
        player = self.r.qword(world + PLAYER_INS_OFFSET)
        if not plausible(player) or self.r.qword(player) != self.base + PLAYER_INS_VFTABLE_RVA:
            return world, None, "PlayerIns unavailable (title screen / loading?)"
        return world, player, None

    def _find_enemy_ins(self, world, budget=40000):
        """Breadth-first from WorldChrMan for objects whose vptr is EnemyIns.

        No hardcoded 'enemy list' offset is used, because none has been
        verified on this build. A bounded walk is slower but it cannot be
        quietly wrong about a field's meaning.
        """
        want = self.base + ENEMY_INS_VFTABLE_RVA
        found, seen, frontier = [], {world}, [world]
        for depth in range(SEARCH_DEPTH + 1):
            nxt = []
            for node in frontier:
                blk = self.r.read(node, SEARCH_WIDTH)
                if blk is None:
                    continue
                for o in range(0, len(blk) - 8, 8):
                    p, = struct.unpack_from("<Q", blk, o)
                    if not plausible(p) or p in seen:
                        continue
                    seen.add(p)
                    if self.r.qword(p) == want:
                        found.append(p)
                    elif depth < SEARCH_DEPTH:
                        nxt.append(p)
                    if len(seen) > budget:
                        return found
            frontier = nxt
        return found

    def _module_via_learned_path(self, chr_ins):
        """Once the layout is known from one enemy, every other enemy of the
        same class lays out identically, so the module can be reached with two
        reads instead of a graph walk. The vptr is still checked, so a wrong
        guess is rejected rather than reported."""
        if self.path is None:
            return None
        container_off, module_off = self.path
        container = self.r.qword(chr_ins + container_off)
        if not plausible(container):
            return None
        module = self.r.qword(container + module_off)
        if not plausible(module):
            return None
        if self.r.qword(module) != self.base + ACTION_FLAG_VFTABLE_RVA:
            return None
        return module

    def _learn_path(self, chr_ins, module):
        """Records WHERE the module was found relative to the character, so the
        remaining enemies skip the search entirely."""
        blk = self.r.read(chr_ins, SEARCH_WIDTH)
        if blk is None:
            return
        for o in range(0, len(blk) - 8, 8):
            container, = struct.unpack_from("<Q", blk, o)
            if not plausible(container):
                continue
            inner = self.r.read(container, SEARCH_WIDTH)
            if inner is None:
                continue
            for j in range(0, len(inner) - 8, 8):
                q, = struct.unpack_from("<Q", inner, j)
                if q == module:
                    self.path = (o, j)
                    return

    def _module_for(self, chr_ins):
        """The mirror of the player's ownership test: majority vote among
        containers that hold BOTH an ActionFlagModule and an
        SprjEnemyDamageModule."""
        af_want = self.base + ACTION_FLAG_VFTABLE_RVA
        dmg_want = self.base + ENEMY_DAMAGE_VFTABLE_RVA
        votes: dict[int, int] = {}
        seen, frontier = {chr_ins}, [chr_ins]
        for depth in range(SEARCH_DEPTH):
            nxt = []
            for node in frontier:
                blk = self.r.read(node, SEARCH_WIDTH)
                if blk is None:
                    continue
                action_flag, has_damage = 0, False
                for o in range(0, len(blk) - 8, 8):
                    p, = struct.unpack_from("<Q", blk, o)
                    if not plausible(p):
                        continue
                    v = self.r.qword(p)
                    if v == af_want and action_flag == 0:
                        action_flag = p
                    elif v == dmg_want:
                        has_damage = True
                    if p not in seen:
                        seen.add(p)
                        if depth + 1 < SEARCH_DEPTH:
                            nxt.append(p)
                if action_flag and has_damage:
                    votes[action_flag] = votes.get(action_flag, 0) + 1
            frontier = nxt
        if not votes:
            return None, 0, 0
        best = max(votes.items(), key=lambda kv: kv[1])
        return best[0], best[1], len(votes)

    def calibrate(self, chars, sample=40):
        """Finds the (ChrIns offset, container offset) pair that reaches an
        ActionFlagModule in the MOST characters.

        Walking the graph to find "an ActionFlagModule reachable in two hops"
        was wrong: a second hop leaves the character and lands on a NEIGHBOUR,
        so 104 characters resolved to only 75 modules and 24 modules were
        shared. A shared module means someone else's fight is reported as
        yours and your own is read off the wrong object -- exactly the
        "nearby enemies register, my own attacks do not" symptom.

        The real structural path is one offset pair, the same for every
        character of a class. It is measured here rather than hardcoded, so a
        different build or a different character class cannot silently give a
        wrong answer.
        """
        want = self.base + ACTION_FLAG_VFTABLE_RVA
        pairs = {}
        for c in chars[:sample]:
            blk = self.r.read(c, SEARCH_WIDTH)
            if blk is None:
                continue
            for o in range(0, len(blk) - 8, 8):
                cont, = struct.unpack_from("<Q", blk, o)
                if not plausible(cont):
                    continue
                inner = self.r.read(cont, 0x800)
                if inner is None:
                    continue
                for j in range(0, len(inner) - 8, 8):
                    q, = struct.unpack_from("<Q", inner, j)
                    if plausible(q) and self.r.qword(q) == want:
                        pairs[(o, j)] = pairs.get((o, j), 0) + 1
        if not pairs:
            return None
        best = max(pairs.items(), key=lambda kv: kv[1])
        self.path = best[0]
        self.path_support = best[1]
        return best

    def refresh(self):
        world, player, why = self._roots()
        if world is None:
            self.enemies = {}
            return why
        if (world, player) != (self.last_world, self.last_player):
            self.last_world, self.last_player = world, player
            self.enemies = {}
        if self.enemies:
            return None
        self.searches += 1
        chars = self._find_enemy_ins(world)
        if self.calibrate(chars) is None:
            return "no ActionFlagModule reachable from any EnemyIns"
        claimed = {}
        for chr_ins in chars:
            module = self._module_via_learned_path(chr_ins)
            if module is None:
                continue
            # One module, one character. If two characters resolve to the same
            # module the path is wrong for at least one of them and there is no
            # way to tell which, so BOTH are dropped rather than reporting a
            # guess.
            if module in claimed:
                claimed[module] = None
                continue
            claimed[module] = chr_ins
        self.enemies = {c: m for m, c in claimed.items() if c is not None}
        self.last_slow = 0
        return None if self.enemies else "no EnemyIns with an owned ActionFlagModule reachable"

    def sample(self):
        """Two small reads per enemy. Reading the whole 0xE11-byte head of
        every module instead cost ~500 KB per poll with 144 enemies tracked,
        which is what made the 5 ms loop impossible to keep.

        The vptr is NOT re-read here for the same reason. It is re-validated
        on the slower refresh path; a recycled object would have to also carry
        a 0 -> non-zero edge on exactly this byte to produce a false event."""
        out = {}
        for chr_ins, module in list(self.enemies.items()):
            pulse = self.r.read(module + PULSE_OFFSET, 1)
            if pulse is None:
                continue
            # The outcome byte is only read when the pulse is actually set.
            # Reading both for all 144 tracked enemies every tick was 288
            # ReadProcessMemory calls per poll and pushed the worst gap to
            # 20 ms -- longer than the one-frame pulse it is trying to catch.
            if pulse[0] == 0:
                out[chr_ins] = (0, self.last_outcome.get(chr_ins, 0))
                continue
            outcome = self.r.read(module + OUTCOME_OFFSET, 1)
            if outcome is None:
                continue
            self.last_outcome[chr_ins] = outcome[0]
            out[chr_ins] = (pulse[0], outcome[0])
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--base", type=lambda s: int(s, 0), default=IMAGE_BASE)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--interval-ms", type=float, default=5.0)
    args = ap.parse_args()

    r = Reader(args.pid)
    loc = EnemyGuardLocator(r, args.base)
    print(f"attached pid={args.pid} base=0x{args.base:x}")

    # Resolve BEFORE the capture window opens, so the ~13 s walk is setup time
    # rather than lost recording time.
    print("적 객체를 찾는 중입니다 (10~20초). 아직 공격하지 마세요...")
    t_search = time.perf_counter()
    why = loc.refresh()
    print(f"  탐색 완료: {time.perf_counter() - t_search:.1f}초, {len(loc.enemies)}개 추적"
          + (f"  (느린 탐색 {loc.last_slow}회, 나머지는 학습된 경로)"
             if getattr(loc, "last_slow", 0) else ""))
    if why:
        print(f"  경고: {why}")
    if not loc.enemies:
        r.close()
        print("적을 하나도 찾지 못했습니다. 적이 있는 곳에서 다시 실행하세요.")
    if getattr(loc, "path", None):
        o, j = loc.path
        print(f"  구조 경로: ChrIns + 0x{o:X} -> container + 0x{j:X}"
              f"  (표본 중 {getattr(loc,'path_support',0)}개가 지지)")
    from collections import Counter
    counts = Counter(loc.enemies.values())
    shared = {m: n for m, n in counts.items() if n > 1}
    print(f"  캐릭터 {len(loc.enemies)}개 -> 서로 다른 모듈 {len(counts)}개"
          + ("   !! 중복 %d" % len(shared) if shared else "   중복 없음"))

    print(f"\n>>> 지금부터 {args.seconds:.0f}초간 기록합니다. 적을 칼로 공격하세요. <<<\n")

    prev: dict[int, tuple[int, int]] = {}
    events = 0
    polls = 0
    worst_gap = 0.0
    last_tick = None
    t0 = time.perf_counter()
    last_status = None
    last_count = -1
    while time.perf_counter() - t0 < args.seconds:
        # No refresh() in here. The walk takes ~13 s; running it per tick ate
        # the entire capture window and left exactly one poll.
        for chr_ins, (pulse, outcome) in loc.sample().items():
            was = prev.get(chr_ins)
            # A pulse is a 0 -> non-zero edge. A held value is NOT an event;
            # that rule is what kept the player detector from inventing hits.
            if was is not None and was[0] == 0 and pulse != 0:
                events += 1
                kind = {0: "BLOCK  ", 1: "DEFLECT"}.get(outcome, f"?({outcome})")
                print(f"  [{(time.perf_counter()-t0)*1000:7.0f}ms] enemy 0x{chr_ins:x} "
                      f"{kind}   pulse={pulse} outcome={outcome}")
            prev[chr_ins] = (pulse, outcome)
        now = time.perf_counter()
        if last_tick is not None:
            worst_gap = max(worst_gap, now - last_tick)
        last_tick = now
        polls += 1
        time.sleep(args.interval_ms / 1000.0)

    r.close()
    # The pulse lasts about one frame (~16 ms at 60 fps). A worst gap anywhere
    # near that means events were missed, so it is reported rather than assumed.
    print(f"\n--- {events} candidate enemy guard event(s), {loc.searches} search(es) ---")
    print(f"    polls={polls}  worst poll gap={worst_gap*1000:.1f} ms"
          f"{'   <-- longer than one frame; events may have been MISSED' if worst_gap > 0.016 else ''}")
    print("이 숫자는 후보일 뿐입니다. 확인이 필요한 것:")
    print("  1. 내가 때렸을 때만 뜨는가 (적끼리 싸울 때 안 떠야 함)")
    print("  2. 튕김/막기 구분이 실제 화면과 일치하는가")
    print("  3. 수리검·폭죽처럼 칼이 아닌 공격에도 뜨는가")
    print("  4. 적이 여러 명일 때 맞은 적에서만 뜨는가")


if __name__ == "__main__":
    sys.exit(main())
