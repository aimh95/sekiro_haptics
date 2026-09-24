"""햅틱 워크벤치 — 게임 없이 DualSense PCM 진동을 설계·재생·비교한다.

이 파일은 화면과 조작만 담당한다. 샘플 하나하나를 만들고 내보내는 일은 전부
C++ 쪽(sekiro_haptic_lab.exe)에 있고, 여기서는 설정을 보내고 결과를 받아 그린다.
그래서 화면에 보이는 숫자는 전부 실제로 재생된 신호에서 나온 값이다.

화면에 나오는 모든 수치는 PCM 샘플 값이다. 힘(N)이나 실제 이동거리, 손에 전달된
에너지가 아니다. 이 프로그램에는 그것을 잴 수단이 없고, 추정해서 보여주지도 않는다.

실행:
    python tools/haptic_lab_gui.py
저장소 루트에서 실행한다 (exe 가 config/guard_cues.json 을 거기서 읽는다).

디자인 자산: 이 화면의 도형·배색·문구는 전부 이 파일 안에서 직접 그린 것이다.
외부 이미지, 아이콘 폰트, 로고, 제품 문양을 쓰지 않는다. 컨트롤러 그림도
사각형·원·선으로만 그린 자체 도식이며 실제 제품 외곽선이 아니다.
"""

import json
import math
import os
import queue
import random
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

DEFAULT_EXE = os.path.join("build-hw", "apps", "haptic_lab", "sekiro_haptic_lab.exe")
SCHEMA_VERSION = 1

# 라이트 모드 팔레트. 강조색은 두 가지만 쓰고, 상태는 색과 문구를 함께 보여준다
# (색만으로 구분하면 색각 이상이 있는 사람에게는 정보가 사라진다).
BG = "#eceef1"          # 전체 배경, 밝은 회색
SURFACE = "#ffffff"     # 작업 영역, 흰색
LINE = "#c9ced6"
INK = "#1d2125"         # 본문 글자, 짙은 색
MUTED = "#69727e"
TEAL = "#0f7d7d"        # 강조 1 — 왼쪽 / 기준
ORANGE = "#bf5f0b"      # 강조 2 — 오른쪽 / 변경분
SLATE = "#5b6b7a"       # 소리 파형 — 진동 레이어(청록/주황)와 구분되는 중성색
OK_BG = "#e4f1ec"
WARN_BG = "#fbeee0"
BAD_BG = "#f7e3e3"

# 전문용어 도움말. 비전공자가 읽어서 바로 쓸 수 있는 문장으로만 적는다.
GLOSSARY = {
    "진폭": "신호의 크기. 0~1 이고 1 이 이 형식에서 낼 수 있는 최대치다. 소리의 '크기'에 해당한다.",
    "Hz": "1초에 진동이 몇 번 반복되는지. 60 Hz 는 1초에 60번 떨린다는 뜻이다.",
    "위상": "진동 한 주기 중 어디에서 시작할지. 0도는 가운데에서 올라가며 시작, 90도는 꼭대기에서 시작한다.",
    "포락선": "시간이 지나면서 세기가 어떻게 변하는지의 윤곽. 커졌다가(상승) 유지되고 줄어드는(감쇠) 모양.",
    "RMS": "신호의 평균적인 크기를 나타내는 계산값. 순간 최대값(peak)과 달리 전체적으로 얼마나 센지를 본다.",
    "peak": "구간 안에서 가장 큰 순간값. 한 번만 튀어도 올라간다.",
    "DC": "신호가 가운데(0)에서 얼마나 치우쳐 있는지. 0이 아니면 진동자가 한쪽으로 밀린 채 있게 되어 "
          "움직임이 아니라 열이 된다.",
    "리미터": "출력이 한계를 넘지 않도록 자동으로 크기를 낮추는 장치. 얼마나 낮췄는지를 dB 로 표시한다.",
    "dB": "크기의 비율을 나타내는 단위. -6 dB 는 대략 절반 크기다.",
    "FFT": "신호 안에 어떤 주파수가 얼마나 들어 있는지 분해해서 보는 계산.",
    "창 함수": "FFT 로 자를 때 구간의 양 끝을 부드럽게 깎는 방법. 안 깎으면(rect) 없는 주파수가 생겨 보인다.",
    "마스터 게인": "모든 레이어를 합친 뒤에 한 번 곱하는 값. A/B 비교 중에는 고정해야 조건이 같아진다.",
    "출력 포화": "합친 파형의 봉우리를 이 높이로 접어 넣는다. 크게 만들려고 게인만 올리면 "
                "장치의 리미터가 대부분 도로 가져가는데(측정: +24 dB 넣고 +0.9~+13.9 dB 나옴), "
                "여기서 미리 접어 두면 리미터가 아예 동작하지 않아 그 세기가 남는다. "
                "대신 파형 모양이 바뀐다 — 세게 접을수록 거칠어진다.",
    "chirp": "재생되는 동안 주파수가 서서히 변하는 파형.",
    "AM": "진폭 변조. 빠른 진동의 크기를 느린 진동이 주기적으로 키웠다 줄였다 한다.",
    "impulse": "짧게 '탁' 치고 사라지는 파형. 세게 시작해서 빠르게 잦아든다.",
    "수식": "직접 쓴 계산식으로 파형을 만든다. sin/cos 의 괄호 안은 라디안이다 — "
            "sin(460*t) 는 460 Hz 가 아니라 460÷(2π) ≈ 73 Hz 다.",
}

WAVEFORMS = [("sine", "사인파 (한 주파수)"),
             ("impulse", "충격 (짧게 치고 사라짐)"),
             ("chirp", "스윕 (주파수가 변함)"),
             ("am", "진폭 변조"),
             ("formula", "수식")]
WAVEFORM_LABEL = dict(WAVEFORMS)

# 오른쪽 편집기 항목: (키, 라벨, 단위, 도움말 키, 이 파형들에서만 쓰임)
FIELDS = [
    ("startMs", "시작 시각", "ms", None, None),
    ("durationMs", "지속시간", "ms", None, None),
    ("frequencyHz", "주파수", "Hz", "Hz", {"sine", "impulse", "chirp", "am"}),
    ("frequencyEndHz", "끝 주파수", "Hz", "chirp", {"chirp"}),
    ("modulationHz", "변조 주파수", "Hz", "AM", {"am"}),
    ("modulationDepth", "변조 깊이", "0~1", "AM", {"am"}),
    ("decayPerSecond", "감쇠율", "/s", "impulse", {"impulse"}),
    ("amplitude", "진폭", "0~1", "진폭", None),
    ("gainDb", "gain", "dB", "dB", None),
    ("startPhaseDeg", "시작 위상", "도", "위상", None),
    ("leftGain", "왼쪽 출력", "0~1", None, None),
    ("rightGain", "오른쪽 출력", "0~1", None, None),
    ("attackMs", "포락선 상승", "ms", "포락선", None),
    ("holdMs", "포락선 유지", "ms", "포락선", None),
    ("decayMs", "포락선 감쇠", "ms", "포락선", None),
    ("sustain", "유지 레벨", "0~1", "포락선", None),
    ("releaseMs", "포락선 종료", "ms", "포락선", None),
    ("t0Seconds", "t0", "초", "수식", {"formula"}),
]

# 막대의 범위와 눈금: (최소, 최대, 최소 눈금, 곡선)
#
# **막대 끝은 그 값이 실제로 가질 수 있는 최대값이다.** 처음에는 "자주 쓰는
# 구간만" 훑게 좁혀 놨는데, 그러면 끝까지 끌어도 주파수가 500 Hz 에서 멈추면서
# 실제로는 2000 Hz 까지 넣을 수 있다 -- 막대가 거짓말을 하는 셈이라 되돌렸다.
#
# 대신 곡선을 준다. 1~2000 Hz 를 곧게 펴면 40~300 Hz 가 막대의 2% 안에 눌려
# 손으로 맞출 수 없다. 로그로 펴면 60 Hz 가 한가운데쯤(0.54) 오고 2000 Hz 도
# 여전히 끝에 있다.
#
#   lin   곧게. 이미 비율로 된 값(진폭, dB, 0~1 비율)에 쓴다.
#   log   비율로 펴기. 주파수처럼 "두 배" 가 의미 있는 값.
#   pow3  0 근처를 넓게. 시작 시각이나 포락선 구간처럼 0~수십 ms 가 중요한 값.
#
# 곡선을 쓰면 눈금이 자리마다 달라지므로, 숫자는 자릿수에 맞춰 반올림하고
# 뒤의 0 은 지워서 보여준다(1000, 437.3, 0.05 처럼).
BAR_RANGE = {
    "startMs": (0.0, 10000.0, 1.0, "pow3"),
    "durationMs": (1.0, 10000.0, 0.1, "log"),
    "frequencyHz": (1.0, 2000.0, 0.1, "log"),
    "frequencyEndHz": (1.0, 2000.0, 0.1, "log"),
    "modulationHz": (0.5, 500.0, 0.1, "log"),
    "modulationDepth": (0.0, 1.0, 0.01, "lin"),
    "decayPerSecond": (0.0, 2000.0, 1.0, "pow3"),
    "amplitude": (0.0, 1.0, 0.01, "lin"),
    "gainDb": (-60.0, 24.0, 0.5, "lin"),
    # 위상은 360도가 한 바퀴다. 그 위는 같은 각도의 반복이라 막대에 더 담을
    # 것이 없다 -- 검증기가 3600까지 받는 것은 여유지 다른 값이 아니다.
    "startPhaseDeg": (0.0, 360.0, 1.0, "lin"),
    "leftGain": (0.0, 1.0, 0.01, "lin"),
    "rightGain": (0.0, 1.0, 0.01, "lin"),
    "attackMs": (0.0, 2000.0, 1.0, "pow3"),
    "holdMs": (0.0, 2000.0, 1.0, "pow3"),
    "decayMs": (0.0, 2000.0, 1.0, "pow3"),
    "sustain": (0.0, 1.0, 0.01, "lin"),
    "releaseMs": (0.0, 2000.0, 1.0, "pow3"),
    "t0Seconds": (0.001, 2.0, 0.001, "log"),
}
MASTER_RANGE = (0.0, 4.0, 0.01, "lin")
CEILING_RANGE = (0.5, 0.99, 0.01, "lin")
SPEAKER_GAIN_RANGE = (0.0, 2.0, 0.01, "lin")
SPEAKER_OFFSET_RANGE = (0.0, 500.0, 1.0, "pow3")

# 같이 딸려 오는 프리셋이 있는 곳. 없으면 목록이 비어 있을 뿐 오류가 아니다.
PRESET_DIR = os.path.join("presets", "haptic_lab")

# 단축키. 화면(F1)에 그대로 뿌리므로 여기가 유일한 목록이다.
SHORTCUTS = [
    ("재생", "Space 또는 F5"),
    ("정지", "Esc"),
    ("전체 출력 해제", "Ctrl+Shift+X"),
    ("레이어 추가", "Insert"),
    ("레이어 복제", "Ctrl+D"),
    ("레이어 삭제", "Delete"),
    ("위/아래 레이어 선택", "↑ / ↓"),
    ("음소거 전환", "M"),
    ("단독 듣기 전환", "S"),
    ("새 프리셋", "Ctrl+N"),
    ("프리셋 열기", "Ctrl+O"),
    ("프리셋 저장", "Ctrl+S"),
    ("WAV 내보내기", "Ctrl+E"),
    ("A 재생 / B 재생", "Ctrl+1 / Ctrl+2"),
    ("파형 보기 전환", "Tab"),
    ("단축키 도움말", "F1"),
]


# 레이어 키 → C++ 명령의 key=value 토큰 이름.
FIELD_TOKEN = {
    "startMs": "start", "durationMs": "dur", "frequencyHz": "freq",
    "frequencyEndHz": "freq2", "modulationHz": "modhz", "modulationDepth": "moddepth",
    "decayPerSecond": "decayrate", "amplitude": "amp", "gainDb": "gaindb",
    "startPhaseDeg": "phase", "leftGain": "left", "rightGain": "right",
    "attackMs": "atk", "holdMs": "hold", "decayMs": "dec", "sustain": "sus",
    "releaseMs": "rel", "t0Seconds": "t0",
}

class Tooltip:
    """마우스를 올리면 뜨는 짧은 설명. 전문용어 옆에 붙인다."""

    def __init__(self, widget, text):
        self.widget = widget
        self.text = text
        self.window = None
        widget.bind("<Enter>", self.show, add="+")
        widget.bind("<Leave>", self.hide, add="+")

    def show(self, _event=None):
        if self.window or not self.text:
            return
        x = self.widget.winfo_rootx() + 12
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        self.window = tk.Toplevel(self.widget)
        self.window.wm_overrideredirect(True)
        self.window.wm_geometry("+%d+%d" % (x, y))
        tk.Label(self.window, text=self.text, justify="left", background="#fdfdf5",
                 foreground=INK, relief="solid", borderwidth=1, wraplength=340,
                 font=("Malgun Gothic", 9), padx=8, pady=5).pack()

    def hide(self, _event=None):
        if self.window:
            self.window.destroy()
            self.window = None


class ParamBar(tk.Canvas):
    """숫자 하나를 보여주고 끌어서 바꾸는 가느다란 막대.

    옆의 입력칸과 **같은 값**을 가리킨다. 막대는 대충 맞추고 입력칸은 정확히
    맞추는 용도라, 둘 중 하나가 주인이 아니라 둘 다 같은 값을 본다.

    막대 범위 밖의 값은 끌어당기지 않는다. 범위 밖이면 끝까지 채운 뒤 주황색
    표시를 남겨서 "이 막대로는 여기까지"라고 말한다 -- 조용히 범위 안으로
    당기면 입력한 적 없는 값이 적용된다.
    """

    def __init__(self, parent, lo, hi, step, curve, on_preview, on_commit,
                 width=96, height=13):
        tk.Canvas.__init__(self, parent, width=width, height=height, bg=SURFACE,
                           highlightthickness=1, highlightbackground=LINE, cursor="hand2")
        self.lo, self.hi, self.step, self.curve = lo, hi, step, curve
        self.on_preview = on_preview
        self.on_commit = on_commit
        self.value = lo
        self.enabled = True
        self.bind("<Button-1>", self._grab)
        self.bind("<B1-Motion>", self._grab)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<Configure>", lambda _e: self.redraw())

    def set_enabled(self, enabled):
        self.enabled = enabled
        self.configure(cursor="hand2" if enabled else "")
        self.redraw()

    def set_value(self, value):
        try:
            self.value = float(value)
        except (TypeError, ValueError):
            self.value = self.lo
        self.redraw()

    def _drawable(self):
        """테두리를 뺀 폭. winfo_width() 는 highlight 두께까지 세므로, 그대로
        나누면 오른쪽 끝까지 끌어도 최대값에 닿지 못한다."""
        return max(1, self.winfo_width() - 2 * int(self.cget("highlightthickness")))

    def _ratio_of(self, value):
        """값 -> 막대 위 자리. _value_at 의 역함수이므로, 그려진 자리를 클릭하면
        같은 값이 나온다. 둘이 어긋나면 막대가 가리키는 곳과 잡히는 곳이 달라진다."""
        if self.hi <= self.lo:
            return 0.0
        if self.curve == "log":
            if value <= 0 or self.lo <= 0:
                return -1.0 if value < self.lo else 1.0
            return math.log(value / self.lo) / math.log(self.hi / self.lo)
        normalised = (value - self.lo) / (self.hi - self.lo)
        if self.curve == "pow3":
            return normalised ** (1.0 / 3.0) if normalised >= 0 else normalised
        return normalised

    def redraw(self):
        self.delete("all")
        width = max(20, self._drawable())
        height = max(8, self.winfo_height())
        ratio = self._ratio_of(self.value)
        outside = ratio < -1e-9 or ratio > 1.0 + 1e-9
        ratio = max(0.0, min(1.0, ratio))
        self.configure(bg="#f4f5f7" if not self.enabled else SURFACE)
        fill = "#d7dde3" if not self.enabled else (ORANGE if outside else TEAL)
        if ratio > 0:
            self.create_rectangle(0, 0, width * ratio, height, fill=fill, outline="")
        if outside:
            # 막대가 훑는 범위를 벗어났다는 표시. 값은 그대로 둔다.
            edge = width - 3 if self.value > self.hi else 3
            self.create_line(edge, 1, edge, height - 1, fill=ORANGE, width=3)

    def _value_at(self, x):
        ratio = max(0.0, min(1.0, x / self._drawable()))
        if self.curve == "log" and self.lo > 0:
            raw = self.lo * (self.hi / self.lo) ** ratio
        elif self.curve == "pow3":
            raw = self.lo + (self.hi - self.lo) * ratio ** 3
        else:
            raw = self.lo + (self.hi - self.lo) * ratio
        raw = self._quantize(raw)
        return max(self.lo, min(self.hi, raw))

    def _quantize(self, value):
        """자릿수에 맞춰 반올림하되 최소 눈금보다 잘게 쪼개지는 않는다.

        곡선 막대는 왼쪽에서 0.01 씩, 오른쪽에서 수십씩 움직인다. 한쪽에 맞춘
        고정 눈금은 반대쪽에서 못 쓰게 된다 -- 1 Hz 눈금이면 저역에서 20 Hz 와
        21 Hz 사이가 없고, 0.1 눈금이면 고역에서 1999.9 Hz 같은 값이 나온다.
        """
        magnitude = abs(value)
        if magnitude >= 100:
            grid = 1.0
        elif magnitude >= 10:
            grid = 0.1
        elif magnitude >= 1:
            grid = 0.01
        else:
            grid = 0.001
        grid = max(grid, self.step)
        return round(value / grid) * grid

    def _grab(self, event):
        if not self.enabled:
            return
        self.value = self._value_at(event.x)
        self.redraw()
        self.on_preview(self.value)

    def _release(self, _event=None):
        if self.enabled:
            self.on_commit(self.value)


class LabProcess:
    """명령 통로. 한 줄 보내고 JSON 한 줄 받는다."""

    def __init__(self, exe, extra_args):
        self.replies = queue.Queue()
        self.proc = subprocess.Popen(
            [exe] + list(extra_args),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            # HID 전송 로그가 stderr 로 나온다. 읽지 않는 파이프는 가득 차면
            # 자식 프로세스를 멈추게 하므로 버린다.
            stderr=subprocess.DEVNULL,
            text=True, bufsize=1, encoding="utf-8", errors="replace",
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.alive = True
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self.replies.put(json.loads(line))
            except json.JSONDecodeError:
                self.replies.put({"event": "raw", "message": line})
        self.alive = False
        self.replies.put({"event": "closed"})

    def send(self, line):
        if not self.alive or self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError):
            self.alive = False

    def close(self):
        # 먼저 출력을 끊고 나간다. 프로세스가 늦게 죽더라도 컨트롤러에 남는 게 없다.
        self.send("stop")
        self.send("trigger.reset")
        self.send("quit")
        try:
            self.proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.alive = False


def fmt_param(value):
    """파라미터 값을 읽기 좋게. 곡선 막대는 자리마다 눈금이 달라서, 고정
    소수점으로 찍으면 1000.000 이나 0.050 처럼 쓸데없는 자리가 붙는다."""
    try:
        value = float(value)
    except (TypeError, ValueError):
        return "-"
    text = ("%.4f" % value).rstrip("0").rstrip(".")
    return text if text not in ("", "-") else "0"


def fmt(value, digits=3):
    try:
        return ("%." + str(digits) + "f") % float(value)
    except (TypeError, ValueError):
        return "-"


class Workbench:
    def __init__(self, root, lab):
        self.root = root
        self.lab = lab
        self.preset = {"name": "새 프리셋", "masterGain": 1.0, "layers": []}
        self.selected = 0
        self.analysis = None
        self.fft = None
        self.slots = {"A": None, "B": None}
        self.ab_last_played = None
        self.field_vars = {}
        self.log_rows = []
        self.endpoint_id = None
        self.channels = None
        self.sample_rate = 48000
        self.pending_formula = None
        self.pending_speaker = None
        self.speaker = {}
        self.speaker_wave = []
        self.library = []
        self.zoom_start = tk.DoubleVar(value=0.0)
        self.zoom_span = tk.DoubleVar(value=100.0)
        # 타임라인 드래그 상태. 잡은 순간의 값을 들고 있다가 놓을 때 한 번 보낸다.
        # (오른쪽 수치 패널의 값 막대는 self.bars 이고 이것과 다른 것이다.)
        self.timeline_bars = []
        self.drag = None
        # 드래그하는 동안 시간 눈금이 같이 늘어나면 막대가 커서에서 달아난다.
        # 그래서 잡고 있는 동안에는 눈금을 얼려 둔다.
        self.frozen_span = None
        self.view_mode = tk.StringVar(value="합성")

        root.title("햅틱 워크벤치 — DualSense PCM")
        root.configure(bg=BG)
        root.minsize(1180, 780)
        root.protocol("WM_DELETE_WINDOW", self.quit)
        self._bind_shortcuts(root)

        self._style()
        self._build_top(root)

        book = ttk.Notebook(root)
        book.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        editor = ttk.Frame(book, style="Card.TFrame")
        device = ttk.Frame(book, style="Card.TFrame")
        book.add(editor, text="  파형 편집  ")
        book.add(device, text="  장치 · 트리거  ")
        self._build_editor(editor)
        self._build_device(device)

        self.root.after(50, self.drain)
        self.new_preset(initial=True)

    # ---------------------------------------------------------------- 단축키

    def _typing(self):
        """글자를 입력하는 중인가?

        Delete, M, S, Space, 화살표는 입력칸 안에서도 뜻이 있는 키다. 그 위에
        단축키를 덮으면 이름을 고치다가 레이어가 지워진다. 그래서 포커스가
        입력 위젯에 있으면 단축키는 아무 일도 하지 않는다.
        """
        widget = self.root.focus_get()
        if widget is None:
            return False
        return widget.winfo_class() in ("Entry", "TEntry", "Text", "TCombobox",
                                        "Spinbox", "TSpinbox")

    def _bind_shortcuts(self, root):
        def guarded(action):
            def run(_event=None):
                if self._typing():
                    return None
                action()
                return "break"       # 기본 동작(버튼 눌림 등)까지 가지 않게
            return run

        # Esc 와 Ctrl 조합은 입력 중에도 뜻이 같으므로 막지 않는다.
        root.bind("<Escape>", lambda _e: self.stop())
        root.bind("<F5>", lambda _e: self.play())
        root.bind("<F1>", lambda _e: self.show_shortcuts())
        root.bind("<Control-n>", lambda _e: self.new_preset())
        root.bind("<Control-o>", lambda _e: self.load_preset())
        root.bind("<Control-s>", lambda _e: self.save_preset())
        root.bind("<Control-e>", lambda _e: self.export_wav())
        root.bind("<Control-d>", lambda _e: self.dup_layer())
        root.bind("<Control-Key-1>", lambda _e: self.play_slot("A"))
        root.bind("<Control-Key-2>", lambda _e: self.play_slot("B"))
        root.bind("<Control-Shift-X>", lambda _e: self.panic())

        # 글자 키와 Delete 는 입력 중이 아닐 때만.
        root.bind("<Delete>", guarded(self.del_layer))
        root.bind("<Insert>", guarded(self.add_layer))
        root.bind("<space>", guarded(self.play))
        root.bind("<m>", guarded(lambda: self.toggle("muted")))
        root.bind("<M>", guarded(lambda: self.toggle("muted")))
        root.bind("<s>", guarded(lambda: self.toggle("solo")))
        root.bind("<S>", guarded(lambda: self.toggle("solo")))
        root.bind("<Up>", guarded(lambda: self.step_layer(-1)))
        root.bind("<Down>", guarded(lambda: self.step_layer(1)))
        root.bind("<Tab>", guarded(self.cycle_view))

    def step_layer(self, delta):
        layers = self.preset.get("layers", [])
        if not layers:
            return
        self.selected = max(0, min(len(layers) - 1, self.selected + delta))
        self.layer_list.selection_clear(0, "end")
        self.layer_list.selection_set(self.selected)
        self.layer_list.see(self.selected)
        self.refresh_inspector()
        self.draw_timeline()
        self.redraw_waveforms()

    def cycle_view(self):
        modes = ["합성", "레이어별", "선택 레이어"]
        self.view_mode.set(modes[(modes.index(self.view_mode.get()) + 1) % len(modes)])
        self.redraw_waveforms()

    def show_shortcuts(self):
        window = tk.Toplevel(self.root)
        window.title("단축키")
        window.configure(bg=SURFACE)
        window.transient(self.root)
        window.bind("<Escape>", lambda _e: window.destroy())
        tk.Label(window, text="단축키", bg=SURFACE, fg=INK,
                 font=("Malgun Gothic", 11, "bold")).grid(
            row=0, column=0, columnspan=2, sticky="w", padx=14, pady=(12, 6))
        for i, (what, keys) in enumerate(SHORTCUTS):
            tk.Label(window, text=what, bg=SURFACE, fg=INK,
                     font=("Malgun Gothic", 9)).grid(row=1 + i, column=0, sticky="w",
                                                     padx=(14, 18), pady=1)
            tk.Label(window, text=keys, bg=SURFACE, fg=TEAL,
                     font=("Consolas", 9)).grid(row=1 + i, column=1, sticky="w",
                                                padx=(0, 14), pady=1)
        tk.Label(window, text="입력칸에 글자를 쓰는 중에는 Delete·Space·M·S·화살표가\n"
                              "단축키로 동작하지 않는다 (그 칸의 원래 동작이 된다).",
                 bg=SURFACE, fg=MUTED, justify="left",
                 font=("Malgun Gothic", 9)).grid(row=1 + len(SHORTCUTS), column=0,
                                                 columnspan=2, sticky="w", padx=14,
                                                 pady=(10, 12))
        window.resizable(False, False)

    # ---------------------------------------------------------------- 스타일

    def _style(self):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        base = ("Malgun Gothic", 9)
        style.configure(".", background=BG, foreground=INK, font=base)
        style.configure("Card.TFrame", background=SURFACE)
        style.configure("Bar.TFrame", background=BG)
        style.configure("TLabel", background=SURFACE, foreground=INK, font=base)
        style.configure("Bar.TLabel", background=BG, foreground=INK, font=base)
        style.configure("Muted.TLabel", background=SURFACE, foreground=MUTED, font=base)
        style.configure("Head.TLabel", background=SURFACE, foreground=INK,
                        font=("Malgun Gothic", 10, "bold"))
        style.configure("TButton", font=base, padding=(8, 4))
        style.configure("Accent.TButton", font=("Malgun Gothic", 9, "bold"))
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", font=base, padding=(14, 6))
        style.configure("TCheckbutton", background=SURFACE, font=base)
        style.configure("TRadiobutton", background=SURFACE, font=base)
        style.configure("TSeparator", background=LINE)

    # ---------------------------------------------------------------- 상단

    def _build_top(self, root):
        bar = ttk.Frame(root, style="Bar.TFrame")
        bar.pack(fill="x", padx=10, pady=(10, 8))

        ttk.Label(bar, text="프로젝트", style="Bar.TLabel").pack(side="left")
        self.project = tk.StringVar(value="새 프리셋")
        entry = ttk.Entry(bar, textvariable=self.project, width=20)
        entry.pack(side="left", padx=(6, 16))
        entry.bind("<FocusOut>", lambda _e: self.push_name())
        entry.bind("<Return>", lambda _e: self.push_name())

        ttk.Label(bar, text="장치", style="Bar.TLabel").pack(side="left")
        self.device_box = ttk.Combobox(bar, width=32, state="readonly")
        self.device_box.pack(side="left", padx=(6, 16))

        # 상태는 색과 문구를 함께 쓴다. 색만으로는 정보가 되지 않는다.
        self.status = tk.Label(bar, text="● 연결 확인 중", bg=WARN_BG, fg=INK,
                               font=("Malgun Gothic", 9, "bold"), padx=10, pady=4)
        self.status.pack(side="left")

        for text, action, style_name in (("전체 출력 해제 (^⇧X)", self.panic, "TButton"),
                                         ("■ 정지 (Esc)", self.stop, "TButton"),
                                         ("▶ 재생 (Space)", self.play, "Accent.TButton")):
            ttk.Button(bar, text=text, command=action, style=style_name).pack(side="right", padx=3)

    # ---------------------------------------------------------------- 편집 탭

    def _build_editor(self, parent):
        parent.columnconfigure(1, weight=1)
        parent.rowconfigure(0, weight=3)
        parent.rowconfigure(1, weight=2)

        left = ttk.Frame(parent, style="Card.TFrame")
        left.grid(row=0, column=0, rowspan=2, sticky="nsew", padx=(8, 4), pady=8)
        self._build_left(left)

        center = ttk.Frame(parent, style="Card.TFrame")
        center.grid(row=0, column=1, sticky="nsew", padx=4, pady=8)
        self._build_center(center)

        right = ttk.Frame(parent, style="Card.TFrame")
        right.grid(row=0, column=2, rowspan=2, sticky="nsew", padx=(4, 8), pady=8)
        self._build_inspector(right)

        bottom = ttk.Notebook(parent)
        bottom.grid(row=1, column=1, sticky="nsew", padx=4, pady=(0, 8))
        fft_tab = ttk.Frame(bottom, style="Card.TFrame")
        ab_tab = ttk.Frame(bottom, style="Card.TFrame")
        log_tab = ttk.Frame(bottom, style="Card.TFrame")
        bottom.add(fft_tab, text=" 주파수 분석 ")
        bottom.add(ab_tab, text=" A/B 비교 ")
        bottom.add(log_tab, text=" 실행 기록 ")
        self._build_fft(fft_tab)
        self._build_ab(ab_tab)
        self._build_log(log_tab)

    def _build_left(self, left):
        ttk.Label(left, text="프리셋", style="Head.TLabel").pack(anchor="w")
        row = ttk.Frame(left, style="Card.TFrame")
        row.pack(fill="x", pady=(2, 6))
        for text, action, keys in (("새로", self.new_preset, "Ctrl+N"),
                                   ("열기", self.load_preset, "Ctrl+O"),
                                   ("저장", self.save_preset, "Ctrl+S"),
                                   ("WAV", self.export_wav, "Ctrl+E")):
            button = ttk.Button(row, text=text, width=5, command=action)
            button.pack(side="left", padx=1)
            Tooltip(button, keys)

        ttk.Label(left, text="기본 프리셋 (실험용)", style="Muted.TLabel").pack(anchor="w")
        self.library_list = tk.Listbox(left, height=8, width=34, bg=SURFACE, fg=INK,
                                       selectbackground=TEAL, selectforeground="#ffffff",
                                       highlightthickness=1, highlightbackground=LINE,
                                       activestyle="none", font=("Malgun Gothic", 9),
                                       exportselection=False)
        self.library_list.pack(fill="x", pady=(2, 2))
        self.library_list.bind("<Double-Button-1>", lambda _e: self.load_library())
        ttk.Button(left, text="불러오기 (자동 재생 안 함)",
                   command=self.load_library).pack(fill="x", pady=(0, 8))

        # --- 소리(스피커) 같이 재생
        ttk.Label(left, text="같이 낼 소리 (컨트롤러 스피커)",
                  style="Head.TLabel").pack(anchor="w")
        self.speaker_label = ttk.Label(left, text="없음", style="Muted.TLabel",
                                       wraplength=230)
        self.speaker_label.pack(anchor="w", pady=(2, 2))
        row = ttk.Frame(left, style="Card.TFrame")
        row.pack(fill="x")
        ttk.Button(row, text="파일 선택", command=self.pick_speaker).pack(
            side="left", padx=1, fill="x", expand=True)
        ttk.Button(row, text="해제", command=self.clear_speaker).pack(
            side="left", padx=1, fill="x", expand=True)
        self.speaker_on = tk.BooleanVar(value=True)
        ttk.Checkbutton(left, text="재생할 때 같이 내기", variable=self.speaker_on,
                        command=self.push_speaker).pack(anchor="w", pady=(2, 0))
        grid = ttk.Frame(left, style="Card.TFrame")
        grid.pack(fill="x", pady=(2, 8))
        ttk.Label(grid, text="세기").grid(row=0, column=0, sticky="w")
        lo, hi, step, curve = SPEAKER_GAIN_RANGE
        self.speaker_gain = ParamBar(grid, lo, hi, step, curve,
                                     on_preview=lambda v: self.speaker_readout(),
                                     on_commit=lambda v: self.push_speaker())
        self.speaker_gain.set_value(1.0)
        self.speaker_gain.grid(row=0, column=1, sticky="we", padx=4)
        label = ttk.Label(grid, text="늦추기")
        label.grid(row=1, column=0, sticky="w")
        Tooltip(label, "진동보다 소리를 이만큼 늦게 낸다. 0 이면 같은 샘플에서 함께 시작한다.")
        lo, hi, step, curve = SPEAKER_OFFSET_RANGE
        self.speaker_offset = ParamBar(grid, lo, hi, step, curve,
                                       on_preview=lambda v: self.speaker_readout(),
                                       on_commit=lambda v: self.push_speaker())
        self.speaker_offset.grid(row=1, column=1, sticky="we", padx=4)
        self.speaker_numbers = ttk.Label(grid, text="", style="Muted.TLabel")
        self.speaker_numbers.grid(row=2, column=0, columnspan=2, sticky="w")
        grid.columnconfigure(1, weight=1)

        ttk.Label(left, text="파형 레이어", style="Head.TLabel").pack(anchor="w")
        self.layer_list = tk.Listbox(left, height=8, width=34, bg=SURFACE, fg=INK,
                                     selectbackground=TEAL, selectforeground="#ffffff",
                                     highlightthickness=1, highlightbackground=LINE,
                                     activestyle="none", font=("Malgun Gothic", 9))
        self.layer_list.pack(fill="both", expand=True, pady=(2, 4))
        self.layer_list.bind("<<ListboxSelect>>", self.on_layer_select)
        # Tk 는 위젯 -> 클래스 -> 최상위 순으로 바인딩을 부른다. Listbox 의 클래스
        # 바인딩이 먼저 선택을 옮기고 최상위 단축키가 또 옮기면 한 번 눌렀는데
        # 두 칸이 간다. 위젯에서 잡고 "break" 로 끊는다.
        self.layer_list.bind("<Up>", lambda _e: (self.step_layer(-1), "break")[1])
        self.layer_list.bind("<Down>", lambda _e: (self.step_layer(1), "break")[1])

        row = ttk.Frame(left, style="Card.TFrame")
        row.pack(fill="x")
        for text, action in (("추가 (Ins)", self.add_layer), ("복제 (^D)", self.dup_layer),
                             ("삭제 (Del)", self.del_layer)):
            ttk.Button(row, text=text, width=9, command=action).pack(side="left", padx=1)
        row2 = ttk.Frame(left, style="Card.TFrame")
        row2.pack(fill="x", pady=(3, 0))
        ttk.Button(row2, text="음소거 (M)", command=lambda: self.toggle("muted")).pack(
            side="left", padx=1, fill="x", expand=True)
        ttk.Button(row2, text="단독 듣기 (S)", command=lambda: self.toggle("solo")).pack(
            side="left", padx=1, fill="x", expand=True)
        ttk.Button(left, text="단축키 보기 (F1)", command=self.show_shortcuts).pack(
            fill="x", pady=(6, 0))

    def _build_center(self, center):
        head = ttk.Frame(center, style="Card.TFrame")
        head.pack(fill="x", pady=(4, 0))
        ttk.Label(head, text="레이어 시간 배치", style="Head.TLabel").pack(side="left")
        self.timeline_hint = ttk.Label(
            head, text="막대를 끌면 시작 시각, 오른쪽 끝을 끌면 지속시간이 바뀐다.",
            style="Muted.TLabel")
        self.timeline_hint.pack(side="left", padx=10)
        self.timeline = tk.Canvas(center, height=140, bg=SURFACE, highlightthickness=1,
                                  highlightbackground=LINE)
        self.timeline.pack(fill="x", pady=(2, 6))
        self.timeline.bind("<Configure>", lambda _e: self.draw_timeline())
        self.timeline.bind("<Button-1>", self.on_timeline_press)
        self.timeline.bind("<B1-Motion>", self.on_timeline_drag)
        self.timeline.bind("<ButtonRelease-1>", self.on_timeline_release)
        self.timeline.bind("<Motion>", self.on_timeline_hover)

        head = ttk.Frame(center, style="Card.TFrame")
        head.pack(fill="x")
        ttk.Label(head, text="파형", style="Head.TLabel").pack(side="left")
        for text in ("합성", "레이어별", "선택 레이어"):
            ttk.Radiobutton(head, text=text, value=text, variable=self.view_mode,
                            command=self.redraw_waveforms).pack(side="left", padx=(8, 0))
        ttk.Label(head, text="청록=왼쪽 · 주황=오른쪽 · 연회색=리미터 적용 후",
                  style="Muted.TLabel").pack(side="left", padx=12)
        self.composite = tk.Canvas(center, height=170, bg=SURFACE, highlightthickness=1,
                                   highlightbackground=LINE)
        self.composite.pack(fill="both", expand=True, pady=(2, 6))
        self.composite.bind("<Configure>", lambda _e: self.draw_composite())
        self.composite.bind("<Button-1>", self.on_composite_click)

        head = ttk.Frame(center, style="Card.TFrame")
        head.pack(fill="x")
        ttk.Label(head, text="확대 보기", style="Head.TLabel").pack(side="left")
        ttk.Label(head, text="시작(ms)", style="Muted.TLabel").pack(side="left", padx=(12, 2))
        tk.Scale(head, variable=self.zoom_start, from_=0, to=1000, resolution=1,
                 orient="horizontal", length=150, bg=SURFACE, highlightthickness=0,
                 troughcolor=BG, command=lambda _v: self.draw_zoom()).pack(side="left")
        ttk.Label(head, text="폭(ms)", style="Muted.TLabel").pack(side="left", padx=(10, 2))
        tk.Scale(head, variable=self.zoom_span, from_=5, to=1000, resolution=1,
                 orient="horizontal", length=150, bg=SURFACE, highlightthickness=0,
                 troughcolor=BG, command=lambda _v: self.draw_zoom()).pack(side="left")
        self.zoom_readout = ttk.Label(head, text="", style="Muted.TLabel")
        self.zoom_readout.pack(side="left", padx=10)

        self.zoom = tk.Canvas(center, height=130, bg=SURFACE, highlightthickness=1,
                              highlightbackground=LINE)
        self.zoom.pack(fill="both", expand=True, pady=(2, 6))
        self.zoom.bind("<Motion>", self.on_zoom_cursor)
        self.zoom.bind("<Configure>", lambda _e: self.draw_zoom())

    def _build_inspector(self, parent):
        """오른쪽 수치 패널.

        세로로 길어서 스크롤 되는 영역 안에 넣는다. 창을 줄였을 때 아래쪽
        입력칸이 잘려 나가면 그 값은 편집할 수 없는 값이 된다.
        """
        parent.rowconfigure(0, weight=1)
        parent.columnconfigure(0, weight=1)
        holder = tk.Canvas(parent, bg=SURFACE, highlightthickness=0, width=300)
        bar = ttk.Scrollbar(parent, orient="vertical", command=holder.yview)
        holder.configure(yscrollcommand=bar.set)
        holder.grid(row=0, column=0, sticky="nsew")
        bar.grid(row=0, column=1, sticky="ns")

        inner = ttk.Frame(holder, style="Card.TFrame")
        window = holder.create_window((0, 0), window=inner, anchor="nw")
        inner.bind("<Configure>",
                   lambda _e: holder.configure(scrollregion=holder.bbox("all")))
        holder.bind("<Configure>", lambda e: holder.itemconfigure(window, width=e.width))
        # 포인터가 이 패널 위에 있을 때만 휠을 가져간다. bind_all 을 계속 걸어
        # 두면 레이어 목록이나 기록 창의 휠까지 이 패널이 먹는다.
        def wheel(event):
            holder.yview_scroll(-1 if event.delta > 0 else 1, "units")
        holder.bind("<Enter>", lambda _e: holder.bind_all("<MouseWheel>", wheel))
        holder.bind("<Leave>", lambda _e: holder.unbind_all("<MouseWheel>"))

        ttk.Label(inner, text="선택한 레이어", style="Head.TLabel").grid(
            row=0, column=0, columnspan=4, sticky="w")

        ttk.Label(inner, text="이름").grid(row=1, column=0, sticky="w", pady=2)
        self.layer_name = tk.StringVar()
        entry = ttk.Entry(inner, textvariable=self.layer_name, width=14)
        entry.grid(row=1, column=1, columnspan=3, sticky="we", pady=2)
        entry.bind("<Return>", lambda _e: self.push_layer())
        entry.bind("<FocusOut>", lambda _e: self.push_layer())

        label = ttk.Label(inner, text="파형")
        label.grid(row=2, column=0, sticky="w", pady=2)
        Tooltip(label, "레이어가 어떤 모양의 진동을 만들지 고른다.")
        self.waveform_box = ttk.Combobox(inner, width=16, state="readonly",
                                         values=[text for _k, text in WAVEFORMS])
        self.waveform_box.grid(row=2, column=1, columnspan=3, sticky="we", pady=2)
        self.waveform_box.bind("<<ComboboxSelected>>", lambda _e: self.push_layer())

        self.env_on = tk.BooleanVar(value=False)
        check = ttk.Checkbutton(inner, text="포락선 사용", variable=self.env_on,
                                command=self.push_layer)
        check.grid(row=3, column=0, columnspan=4, sticky="w", pady=(4, 2))
        Tooltip(check, GLOSSARY["포락선"])

        self.rows = {}
        self.bars = {}
        for i, (key, text, unit, help_key, only) in enumerate(FIELDS):
            r = 4 + i
            name = ttk.Label(inner, text=text)
            name.grid(row=r, column=0, sticky="w", pady=1)
            lo, hi, step, curve = BAR_RANGE[key]
            shape = {"log": " · 비율 눈금", "pow3": " · 작은 값 쪽이 넓음"}.get(curve, "")
            Tooltip(name, "%s\n막대: %g ~ %g %s%s"
                    % (GLOSSARY.get(help_key, ""), lo, hi, unit, shape))
            var = tk.StringVar()
            self.field_vars[key] = var
            box = ttk.Entry(inner, textvariable=var, width=8, justify="right")
            box.grid(row=r, column=1, sticky="we", pady=1)
            box.bind("<Return>", lambda _e: self.push_layer())
            box.bind("<FocusOut>", lambda _e: self.push_layer())
            unit_label = ttk.Label(inner, text=unit, style="Muted.TLabel")
            unit_label.grid(row=r, column=2, sticky="w", padx=(3, 3))
            slider = ParamBar(inner, lo, hi, step, curve,
                              on_preview=lambda v, k=key: self.preview_field(k, v),
                              on_commit=lambda v, k=key: self.commit_field(k, v))
            slider.grid(row=r, column=3, sticky="we", pady=1)
            self.bars[key] = slider
            self.rows[key] = (name, box, unit_label, only)

        r = 4 + len(FIELDS)
        formula_label = ttk.Label(inner, text="수식  f(t)")
        formula_label.grid(row=r, column=0, columnspan=4, sticky="w", pady=(8, 1))
        Tooltip(formula_label, GLOSSARY["수식"])
        self.formula = tk.Text(inner, height=3, width=24, font=("Consolas", 9),
                               bg=SURFACE, fg=INK, relief="solid", borderwidth=1, wrap="word")
        self.formula.grid(row=r + 1, column=0, columnspan=4, sticky="we")
        ttk.Button(inner, text="수식 적용", command=self.push_formula).grid(
            row=r + 2, column=0, columnspan=4, sticky="we", pady=(3, 0))
        self.formula_hint = ttk.Label(inner, text="", style="Muted.TLabel", wraplength=270)
        self.formula_hint.grid(row=r + 3, column=0, columnspan=4, sticky="w", pady=(3, 0))

        ttk.Separator(inner, orient="horizontal").grid(
            row=r + 4, column=0, columnspan=4, sticky="we", pady=8)
        master_label = ttk.Label(inner, text="마스터 게인")
        master_label.grid(row=r + 5, column=0, sticky="w")
        Tooltip(master_label, GLOSSARY["마스터 게인"])
        self.master = tk.StringVar(value="1.000")
        box = ttk.Entry(inner, textvariable=self.master, width=8, justify="right")
        box.grid(row=r + 5, column=1, sticky="we")
        box.bind("<Return>", lambda _e: self.push_name())
        box.bind("<FocusOut>", lambda _e: self.push_name())
        lo, hi, step, curve = MASTER_RANGE
        self.master_bar = ParamBar(inner, lo, hi, step, curve,
                                   on_preview=lambda v: self.master.set(fmt_param(v)),
                                   on_commit=lambda v: (self.master.set(fmt_param(v)),
                                                        self.push_name()))
        self.master_bar.grid(row=r + 5, column=3, sticky="we")

        self.ceiling_on = tk.BooleanVar(value=False)
        check = ttk.Checkbutton(inner, text="출력 포화", variable=self.ceiling_on,
                                command=self.push_name)
        check.grid(row=r + 6, column=0, sticky="w")
        Tooltip(check, GLOSSARY["출력 포화"])
        self.ceiling = tk.StringVar(value="0.94")
        box = ttk.Entry(inner, textvariable=self.ceiling, width=8, justify="right")
        box.grid(row=r + 6, column=1, sticky="we")
        box.bind("<Return>", lambda _e: self.push_name())
        box.bind("<FocusOut>", lambda _e: self.push_name())
        lo, hi, step, curve = CEILING_RANGE
        self.ceiling_bar = ParamBar(inner, lo, hi, step, curve,
                                    on_preview=lambda v: self.ceiling.set(fmt_param(v)),
                                    on_commit=lambda v: (self.ceiling.set(fmt_param(v)),
                                                         self.push_name()))
        self.ceiling_bar.set_value(0.94)
        self.ceiling_bar.grid(row=r + 6, column=3, sticky="we")

        self.measure_text = tk.Text(inner, height=14, width=32, bg=BG, fg=INK,
                                    relief="flat", font=("Consolas", 9), wrap="word")
        self.measure_text.grid(row=r + 7, column=0, columnspan=4, sticky="nsew", pady=(8, 0))
        inner.rowconfigure(r + 7, weight=1)
        inner.columnconfigure(1, weight=1)
        inner.columnconfigure(3, weight=2)

    def preview_field(self, key, value):
        """끄는 동안: 입력칸만 갱신하고 아무것도 보내지 않는다."""
        self.field_vars[key].set(fmt_param(value))

    def commit_field(self, key, value):
        self.preview_field(key, value)
        self.push_layer()

    def _build_fft(self, parent):
        bar = ttk.Frame(parent, style="Card.TFrame")
        bar.pack(fill="x", pady=4, padx=6)
        label = ttk.Label(bar, text="FFT 길이")
        label.pack(side="left")
        Tooltip(label, GLOSSARY["FFT"])
        self.fft_size = ttk.Combobox(bar, width=7, state="readonly",
                                     values=["256", "512", "1024", "2048", "4096", "8192"])
        self.fft_size.set("2048")
        self.fft_size.pack(side="left", padx=(4, 12))
        label = ttk.Label(bar, text="창 함수")
        label.pack(side="left")
        Tooltip(label, GLOSSARY["창 함수"])
        self.fft_window = ttk.Combobox(bar, width=9, state="readonly",
                                       values=["hann", "hamming", "rect"])
        self.fft_window.set("hann")
        self.fft_window.pack(side="left", padx=(4, 12))
        ttk.Label(bar, text="분석 시작(샘플)").pack(side="left")
        self.fft_start = tk.StringVar(value="0")
        ttk.Entry(bar, textvariable=self.fft_start, width=8, justify="right").pack(
            side="left", padx=(4, 12))
        ttk.Label(bar, text="채널").pack(side="left")
        self.fft_channel = ttk.Combobox(bar, width=7, state="readonly", values=["왼쪽", "오른쪽"])
        self.fft_channel.set("왼쪽")
        self.fft_channel.pack(side="left", padx=(4, 12))
        ttk.Button(bar, text="분석", command=self.request_fft).pack(side="left")

        self.fft_caption = ttk.Label(parent, text="분석을 누르면 설정과 분해능이 여기에 표시된다.",
                                     style="Muted.TLabel")
        self.fft_caption.pack(anchor="w", padx=6)
        self.fft_canvas = tk.Canvas(parent, height=170, bg=SURFACE, highlightthickness=1,
                                    highlightbackground=LINE)
        self.fft_canvas.pack(fill="both", expand=True, pady=(2, 4), padx=6)
        self.fft_canvas.bind("<Motion>", self.on_fft_cursor)

    def _build_ab(self, parent):
        left = ttk.Frame(parent, style="Card.TFrame")
        left.pack(side="left", fill="y", padx=8, pady=6)
        ttk.Label(left, text="설정 고정", style="Head.TLabel").pack(anchor="w")
        for slot in ("A", "B"):
            row = ttk.Frame(left, style="Card.TFrame")
            row.pack(fill="x", pady=2)
            ttk.Button(row, text="%s 저장" % slot, width=8,
                       command=lambda s=slot: self.store_slot(s)).pack(side="left")
            ttk.Button(row, text="%s 불러오기" % slot, width=10,
                       command=lambda s=slot: self.recall_slot(s)).pack(side="left", padx=3)
            ttk.Button(row, text="%s 재생" % slot, width=7,
                       command=lambda s=slot: self.play_slot(s)).pack(side="left")
        ttk.Button(left, text="A ↔ B 번갈아 재생", command=self.play_alternating).pack(
            fill="x", pady=(6, 2))
        button = ttk.Button(left, text="무작위로 한 번 재생 (어느 쪽인지 숨김)",
                            command=self.play_blind)
        button.pack(fill="x", pady=(0, 6))
        Tooltip(button, "순서를 섞어서 재생하고, 결과를 기록할 때까지 어느 쪽이었는지 보여주지 않는다.")
        self.ab_state = ttk.Label(left, text="A: 없음\nB: 없음", style="Muted.TLabel",
                                  wraplength=210)
        self.ab_state.pack(anchor="w")
        ttk.Label(left, text="A/B 마다 자동 정규화를 하지 않는다.\n비교 조건이 달라지기 때문이다.",
                  style="Muted.TLabel").pack(anchor="w", pady=(8, 0))

        right = ttk.Frame(parent, style="Card.TFrame")
        right.pack(side="left", fill="both", expand=True, padx=8, pady=6)
        ttk.Label(right, text="체감 기록", style="Head.TLabel").grid(
            row=0, column=0, columnspan=4, sticky="w")
        ttk.Label(right,
                  text="‘찡’ 하는 발생음과 손에 오는 진동은 따로 평가한다. 귀를 막고 한 번 더 해보면 구분된다.",
                  style="Muted.TLabel", wraplength=520).grid(
            row=1, column=0, columnspan=4, sticky="w", pady=(0, 6))
        self.rating = {}
        for i, (key, text) in enumerate((("strength", "강도"), ("sharp", "날카로움"),
                                         ("dull", "둔탁함"), ("ring", "잔향"))):
            ttk.Label(right, text=text).grid(row=2 + i, column=0, sticky="w", pady=1)
            var = tk.IntVar(value=3)
            self.rating[key] = var
            tk.Scale(right, variable=var, from_=1, to=5, orient="horizontal", length=170,
                     bg=SURFACE, highlightthickness=0, troughcolor=BG).grid(
                row=2 + i, column=1, sticky="w")
        ttk.Label(right, text="느껴진 위치").grid(row=6, column=0, sticky="w")
        self.felt_where = ttk.Combobox(right, width=16, state="readonly",
                                       values=["왼손", "오른손", "양손 같게", "양손 다르게", "모르겠음"])
        self.felt_where.set("양손 같게")
        self.felt_where.grid(row=6, column=1, sticky="w", pady=2)
        ttk.Label(right, text="메모").grid(row=7, column=0, sticky="nw", pady=(4, 0))
        self.note = tk.Text(right, height=3, width=38, font=("Malgun Gothic", 9),
                            relief="solid", borderwidth=1, bg=SURFACE, fg=INK)
        self.note.grid(row=7, column=1, columnspan=3, sticky="we", pady=4)
        ttk.Button(right, text="이 결과 기록", command=self.record_trial).grid(
            row=8, column=1, sticky="w")
        right.columnconfigure(3, weight=1)

    def _build_log(self, parent):
        bar = ttk.Frame(parent, style="Card.TFrame")
        bar.pack(fill="x", pady=4, padx=6)
        ttk.Button(bar, text="기록 내보내기 (JSON)", command=self.export_log).pack(side="left")
        ttk.Label(bar, text="  여기 남는 값은 전부 PCM 샘플 값이다. 물리적 진동 측정이 아니다.",
                  style="Muted.TLabel").pack(side="left")
        self.log = tk.Text(parent, height=9, bg=SURFACE, fg=INK, relief="flat",
                           font=("Consolas", 9), wrap="none")
        self.log.pack(fill="both", expand=True, pady=(0, 4), padx=6)

    # ---------------------------------------------------------------- 장치 탭

    def _build_device(self, parent):
        left = ttk.Frame(parent, style="Card.TFrame")
        left.pack(side="left", fill="both", expand=True, padx=10, pady=10)

        ttk.Label(left, text="출력 경로", style="Head.TLabel").pack(anchor="w")
        self.device_text = tk.Text(left, height=8, width=56, bg=BG, fg=INK, relief="flat",
                                   font=("Consolas", 9), wrap="word")
        self.device_text.pack(fill="x", pady=(2, 8))
        ttk.Button(left, text="장치 목록 새로 고침",
                   command=lambda: self.lab.send("device.list")).pack(anchor="w")
        ttk.Label(left,
                  text="Windows 기본 출력 장치는 바꾸지 않는다. 엔드포인트 볼륨은 이 앱의 gain 과 "
                       "별개이며 여기서 자동으로 건드리지 않는다. 설정은 목록 번호가 아니라 "
                       "안정 식별자로 저장한다.",
                  style="Muted.TLabel", wraplength=430).pack(anchor="w", pady=(6, 0))

        ttk.Separator(left, orient="horizontal").pack(fill="x", pady=10)
        ttk.Label(left, text="적응형 트리거", style="Head.TLabel").pack(anchor="w")
        ttk.Label(left,
                  text="구현되어 있는 네 모드만 제공하고, 각 모드가 실제로 쓰는 값만 입력칸이 열린다. "
                       "트리거 조작은 PCM 출력이나 스피커 라우팅을 덮어쓰지 않는다.",
                  style="Muted.TLabel", wraplength=430).pack(anchor="w", pady=(0, 6))

        grid = ttk.Frame(left, style="Card.TFrame")
        grid.pack(anchor="w")
        ttk.Label(grid, text="쪽").grid(row=0, column=0, sticky="w")
        self.trigger_side = ttk.Combobox(grid, width=10, state="readonly", values=["L2", "R2"])
        self.trigger_side.set("L2")
        self.trigger_side.grid(row=0, column=1, sticky="w", padx=4, pady=2)
        ttk.Label(grid, text="모드").grid(row=1, column=0, sticky="w")
        self.trigger_mode = ttk.Combobox(grid, width=14, state="readonly",
                                         values=["off", "feedback", "weapon", "vibration"])
        self.trigger_mode.set("weapon")
        self.trigger_mode.grid(row=1, column=1, sticky="w", padx=4, pady=2)
        self.trigger_mode.bind("<<ComboboxSelected>>", lambda _e: self.sync_trigger_fields())

        self.trigger_vars = {}
        self.trigger_rows = {}
        for i, (key, text, unit) in enumerate((("start", "시작 구간", "0~9"),
                                               ("end", "끝 구간", "0~9"),
                                               ("strength", "세기", "1~8"),
                                               ("freq", "진동 주파수", "Hz"),
                                               ("ms", "유지 시간 (0=계속)", "ms"))):
            label = ttk.Label(grid, text=text)
            label.grid(row=2 + i, column=0, sticky="w")
            var = tk.StringVar(value={"start": "2", "end": "5", "strength": "4",
                                      "freq": "40", "ms": "0"}[key])
            self.trigger_vars[key] = var
            box = ttk.Entry(grid, textvariable=var, width=8, justify="right")
            box.grid(row=2 + i, column=1, sticky="w", padx=4, pady=1)
            ttk.Label(grid, text=unit, style="Muted.TLabel").grid(row=2 + i, column=2, sticky="w")
            self.trigger_rows[key] = (label, box)

        row = ttk.Frame(left, style="Card.TFrame")
        row.pack(anchor="w", pady=8)
        ttk.Button(row, text="적용", command=self.trigger_apply).pack(side="left", padx=2)
        ttk.Button(row, text="해제", command=self.trigger_release).pack(side="left", padx=2)
        ttk.Button(row, text="양쪽 Reset", command=self.trigger_reset).pack(side="left", padx=2)
        self.trigger_state = ttk.Label(left, text="트리거 미적용", style="Muted.TLabel")
        self.trigger_state.pack(anchor="w")

        right = ttk.Frame(parent, style="Card.TFrame")
        right.pack(side="left", fill="both", expand=True, padx=10, pady=10)
        ttk.Label(right, text="출력 도식", style="Head.TLabel").pack(anchor="w")
        ttk.Label(right,
                  text="사각형과 원으로만 직접 그린 개념도다. 실제 제품 외형이 아니고, 여기 보이는 것은 "
                       "이 도구가 실제로 제어하는 경로뿐이다. 측정하지 않은 값은 표시하지 않는다.",
                  style="Muted.TLabel", wraplength=380).pack(anchor="w", pady=(0, 6))
        self.schematic = tk.Canvas(right, height=300, bg=SURFACE, highlightthickness=1,
                                   highlightbackground=LINE)
        self.schematic.pack(fill="both", expand=True)
        self.schematic.bind("<Configure>", lambda _e: self.draw_schematic())
        self.sync_trigger_fields()

    # -------------------------------------------------------- 컨트롤러 개념도

    def draw_schematic(self):
        """사각형·원·선으로만 그린 자체 도식. 외부 자산을 쓰지 않는다."""
        c = self.schematic
        c.delete("all")
        w = max(340, c.winfo_width())
        h = max(240, c.winfo_height())
        cx, cy = w / 2, h / 2
        body_w, body_h = min(280, w - 80), 88

        c.create_rectangle(cx - body_w / 2, cy - body_h / 2, cx + body_w / 2, cy + body_h / 2,
                           outline=LINE, width=2, fill="#f7f8fa")
        for sign in (-1, 1):
            x = cx + sign * (body_w / 2 - 24)
            c.create_oval(x - 24, cy + body_h / 2 - 20, x + 24, cy + body_h / 2 + 64,
                          outline=LINE, width=2, fill="#f2f4f7")

        held = self.trigger_state.cget("text")
        for sign, name in ((-1, "L2"), (1, "R2")):
            x = cx + sign * (body_w / 2 - 38)
            active = name in held and "적용됨" in held
            c.create_rectangle(x - 22, cy - body_h / 2 - 28, x + 22, cy - body_h / 2 - 8,
                               outline=ORANGE if active else LINE, width=2,
                               fill=WARN_BG if active else SURFACE)
            c.create_text(x, cy - body_h / 2 - 18, text=name, fill=INK,
                          font=("Malgun Gothic", 9, "bold"))

        left_ch = self.channels[0] if self.channels else "?"
        right_ch = self.channels[1] if self.channels else "?"
        for sign, text, channel in ((-1, "왼쪽 진동", left_ch), (1, "오른쪽 진동", right_ch)):
            x = cx + sign * (body_w / 2 - 24)
            y = cy + body_h / 2 + 22
            c.create_oval(x - 17, y - 17, x + 17, y + 17, outline=TEAL, width=2, fill=OK_BG)
            c.create_text(x, y, text="ch%s" % channel, fill=TEAL, font=("Consolas", 9, "bold"))
            c.create_text(x, y + 30, text=text, fill=INK, font=("Malgun Gothic", 9))

        c.create_rectangle(cx - 34, cy - 12, cx + 34, cy + 12, outline=MUTED, width=2,
                           fill="#eef0f3")
        c.create_text(cx, cy, text="스피커", fill=MUTED, font=("Malgun Gothic", 8))
        c.create_text(cx, h - 16,
                      text="레거시 럼블 모터는 쓰지 않는다 (액추에이터는 PCM 경로 유지)",
                      fill=MUTED, font=("Malgun Gothic", 8))

    # ---------------------------------------------------------------- 명령

    def push_name(self):
        # 꺼져 있으면 0 을 보낸다 -- 0 이 "포화 없음" 이다.
        ceiling = self.ceiling.get() if self.ceiling_on.get() else "0"
        self.lab.send("preset.name name=%s master=%s ceiling=%s"
                      % (self.project.get().replace(" ", "_") or "preset",
                         self.master.get() or "1.0", ceiling or "0"))

    def new_preset(self, initial=False):
        self.lab.send("preset.clear")
        self.lab.send("layer.add type=sine freq=60 start=0 dur=200 amp=0.5")
        self.selected = 0
        if not initial:
            self.write_log("새 프리셋")

    def add_layer(self):
        self.lab.send("layer.add type=sine freq=120 start=0 dur=200 amp=0.4")

    def dup_layer(self):
        self.lab.send("layer.dup index=%d" % self.selected)

    def del_layer(self):
        if len(self.preset.get("layers", [])) <= 1:
            messagebox.showinfo("레이어", "마지막 레이어는 지울 수 없다.")
            return
        self.lab.send("layer.remove index=%d" % self.selected)
        self.selected = max(0, self.selected - 1)

    def toggle(self, key):
        layer = self.current_layer()
        if not layer:
            return
        token = "mute" if key == "muted" else "solo"
        self.lab.send("layer.set index=%d %s=%d"
                      % (self.selected, token, 0 if layer.get(key) else 1))

    # ---------------------------------------------------------------- 소리

    def speaker_readout(self):
        self.speaker_numbers.config(
            text="세기 %s · 늦추기 %s ms"
                 % (fmt_param(self.speaker_gain.value), fmt_param(self.speaker_offset.value)))
        # 막대를 끄는 동안 타임라인의 소리 줄이 같이 움직이는 것이 이 그림의 쓸모다.
        if self.speaker.get("loaded"):
            self.speaker["offsetMs"] = self.speaker_offset.value
            self.draw_timeline()

    def push_speaker(self):
        self.speaker_readout()
        self.lab.send("speaker.set gain=%s offset=%s on=%d"
                      % (self.speaker_gain.value, self.speaker_offset.value,
                         1 if self.speaker_on.get() else 0))

    def pick_speaker(self):
        path = filedialog.askopenfilename(
            title="같이 낼 소리 파일",
            initialdir="audio" if os.path.isdir("audio") else ".",
            filetypes=[("소리 파일", "*.wav *.mp3 *.flac *.m4a"), ("모든 파일", "*.*")])
        if path:
            self.lab.send("speaker.load %s" % path)

    def clear_speaker(self):
        self.lab.send("speaker.clear")

    # ---------------------------------------------------------------- 기본 프리셋

    def refresh_library(self):
        self.library = []
        self.library_list.delete(0, "end")
        if not os.path.isdir(PRESET_DIR):
            self.library_list.insert("end", "(%s 없음)" % PRESET_DIR)
            return
        for name in sorted(os.listdir(PRESET_DIR)):
            if not name.lower().endswith(".json"):
                continue
            path = os.path.join(PRESET_DIR, name)
            try:
                with open(path, encoding="utf-8") as src:
                    document = json.load(src)
            except (OSError, json.JSONDecodeError):
                continue
            self.library.append((path, document))
            self.library_list.insert("end", "%s  (%d개 레이어)"
                                     % (document.get("name", name),
                                        len(document.get("layers", []))))

    def load_library(self):
        picked = self.library_list.curselection()
        if not picked or picked[0] >= len(self.library):
            messagebox.showinfo("기본 프리셋", "목록에서 하나를 고른다.")
            return
        path, document = self.library[picked[0]]
        self.apply_preset(document)
        self.write_log("기본 프리셋 불러옴: %s (자동 재생 안 함)" % os.path.basename(path))

    def current_layer(self):
        layers = self.preset.get("layers", [])
        if 0 <= self.selected < len(layers):
            return layers[self.selected]
        return None

    def push_layer(self):
        layer = self.current_layer()
        if not layer:
            return
        parts = ["layer.set index=%d" % self.selected]
        name = self.layer_name.get().strip().replace(" ", "_")
        if name:
            parts.append("name=%s" % name)
        for key, text in WAVEFORMS:
            if text == self.waveform_box.get():
                parts.append("type=%s" % key)
                break
        parts.append("env=%d" % (1 if self.env_on.get() else 0))
        for key, token in FIELD_TOKEN.items():
            raw = self.field_vars[key].get().strip()
            if not raw:
                continue
            try:
                parts.append("%s=%s" % (token, float(raw)))
            except ValueError:
                self.flash("%s 값이 숫자가 아니다: %s" % (key, raw))
                return
        self.lab.send(" ".join(parts))

    def push_formula(self):
        text = self.formula.get("1.0", "end").strip()
        if text:
            self.lab.send("layer.formula %d %s" % (self.selected, text))

    def request_analysis(self):
        self.lab.send("preset.analyze points=760")

    def request_fft(self):
        self.lab.send("preset.fft size=%s window=%s start=%s channel=%d"
                      % (self.fft_size.get(), self.fft_window.get(),
                         self.fft_start.get() or "0",
                         1 if self.fft_channel.get() == "오른쪽" else 0))

    def play(self):
        self.lab.send("preset.play")
        self.write_log("재생: %s" % self.project.get())

    def stop(self):
        self.lab.send("stop")
        self.set_status("정지됨", OK_BG)

    def panic(self):
        """전체 출력 해제 — 진동과 트리거를 한 번에 내린다."""
        self.lab.send("stop")
        self.lab.send("trigger.reset")
        self.trigger_state.config(text="트리거 미적용")
        self.draw_schematic()
        self.set_status("전체 출력 해제됨", OK_BG)
        self.write_log("전체 출력 해제")

    def export_wav(self):
        path = filedialog.asksaveasfilename(title="WAV 내보내기 (확장자 없이)",
                                            defaultextension="", initialfile="preset")
        if path:
            self.lab.send("preset.export path=%s" % path.replace(" ", "_"))

    # ---------------------------------------------------------------- A/B

    def store_slot(self, slot):
        self.slots[slot] = json.loads(json.dumps(self.preset))
        self.update_ab_state()
        self.write_log("설정 %s 저장 (레이어 %d개, master %s)"
                       % (slot, len(self.preset.get("layers", [])),
                          fmt(self.preset.get("masterGain", 1.0))))

    def recall_slot(self, slot):
        if not self.slots[slot]:
            messagebox.showinfo("A/B", "%s 에 저장된 설정이 없다." % slot)
            return
        self.apply_preset(self.slots[slot])

    def play_slot(self, slot):
        if not self.slots[slot]:
            messagebox.showinfo("A/B", "%s 에 저장된 설정이 없다." % slot)
            return
        # 명령은 보낸 순서대로 처리되므로, 설정을 다 보낸 뒤 재생을 이어 보내면 된다.
        self.apply_preset(self.slots[slot])
        self.ab_last_played = slot
        self.lab.send("preset.play")
        self.write_log("A/B 재생: %s" % slot)

    def play_alternating(self):
        if not (self.slots["A"] and self.slots["B"]):
            messagebox.showinfo("A/B", "A 와 B 를 모두 저장해야 한다.")
            return
        self.play_slot("B" if self.ab_last_played == "A" else "A")

    def play_blind(self):
        if not (self.slots["A"] and self.slots["B"]):
            messagebox.showinfo("A/B", "A 와 B 를 모두 저장해야 한다.")
            return
        slot = random.choice(["A", "B"])
        self.apply_preset(self.slots[slot])
        self.ab_last_played = slot
        self.lab.send("preset.play")
        self.ab_state.config(text="무작위로 재생했다.\n기록하면 어느 쪽이었는지 알려준다.")
        self.write_log("A/B 무작위 재생 (숨김)")

    def update_ab_state(self):
        bits = []
        for slot in ("A", "B"):
            saved = self.slots[slot]
            bits.append("%s: %s" % (slot, ("레이어 %d개, master %s"
                                           % (len(saved.get("layers", [])),
                                              fmt(saved.get("masterGain", 1.0))))
                                    if saved else "없음"))
        self.ab_state.config(text="\n".join(bits))

    def record_trial(self):
        which = self.ab_last_played or "-"
        entry = {
            "time": time.strftime("%H:%M:%S"),
            "slot": which,
            "preset": self.preset.get("name"),
            "masterGain": self.preset.get("masterGain"),
            "ratings": {k: v.get() for k, v in self.rating.items()},
            "felt": self.felt_where.get(),
            "note": self.note.get("1.0", "end").strip(),
        }
        if self.analysis:
            entry["measured"] = {"peakLeft": self.analysis["statsLeft"]["peak"],
                                 "rmsLeft": self.analysis["statsLeft"]["rms"],
                                 "unit": "PCM 샘플 값"}
        self.log_rows.append(entry)
        self.write_log("기록 [%s] 강도%d 날카%d 둔탁%d 잔향%d %s %s"
                       % (which, entry["ratings"]["strength"], entry["ratings"]["sharp"],
                          entry["ratings"]["dull"], entry["ratings"]["ring"],
                          entry["felt"], entry["note"][:40]))
        self.ab_state.config(text="방금 재생한 것은 %s 였다." % which)
        self.note.delete("1.0", "end")

    def export_log(self):
        path = filedialog.asksaveasfilename(title="실행 기록 저장", defaultextension=".json")
        if not path:
            return
        with open(path, "w", encoding="utf-8") as out:
            json.dump({"schemaVersion": SCHEMA_VERSION,
                       "note": "PCM 샘플 값 기준. 물리적 진동 측정이 아님.",
                       "trials": self.log_rows}, out, ensure_ascii=False, indent=2)
        self.write_log("기록 저장: %s" % path)

    # ---------------------------------------------------------------- 저장/열기

    def save_preset(self):
        path = filedialog.asksaveasfilename(title="프리셋 저장", defaultextension=".json")
        if not path:
            return
        document = json.loads(json.dumps(self.preset))
        document["schemaVersion"] = SCHEMA_VERSION
        document["output"] = {"endpointId": self.endpoint_id,
                              "hapticLeft": self.channels[0] if self.channels else None,
                              "hapticRight": self.channels[1] if self.channels else None,
                              "sampleRateHz": self.sample_rate}
        document["analysis"] = {"fftSize": int(self.fft_size.get()),
                                "window": self.fft_window.get(),
                                "startFrame": int(self.fft_start.get() or 0),
                                "unit": "PCM 진폭"}
        with open(path, "w", encoding="utf-8") as out:
            json.dump(document, out, ensure_ascii=False, indent=2)
        self.write_log("프리셋 저장: %s" % path)

    def load_preset(self):
        path = filedialog.askopenfilename(title="프리셋 열기",
                                          filetypes=[("프리셋", "*.json"), ("모든 파일", "*.*")])
        if not path:
            return
        try:
            with open(path, encoding="utf-8") as src:
                document = json.load(src)
        except (OSError, json.JSONDecodeError) as problem:
            messagebox.showerror("프리셋", "열 수 없다: %s" % problem)
            return
        version = document.get("schemaVersion")
        if version != SCHEMA_VERSION:
            # 읽히긴 하는데 뜻이 다른 파일이 조용히 열리는 것보다 거절하는 쪽이 낫다.
            messagebox.showerror("프리셋", "지원하지 않는 schemaVersion: %s (이 프로그램은 %d)"
                                 % (version, SCHEMA_VERSION))
            return
        analysis = document.get("analysis") or {}
        if analysis.get("fftSize"):
            self.fft_size.set(str(analysis["fftSize"]))
        if analysis.get("window"):
            self.fft_window.set(analysis["window"])
        self.apply_preset(document)
        # 불러왔다고 재생하지 않는다.
        self.write_log("프리셋 열기: %s (자동 재생 안 함)" % path)

    def apply_preset(self, document):
        """프리셋을 출력 프로세스에 그대로 다시 세운다."""
        self.lab.send("preset.clear")
        self.lab.send("preset.name name=%s master=%s ceiling=%s"
                      % (str(document.get("name", "preset")).replace(" ", "_"),
                         document.get("masterGain", 1.0),
                         document.get("saturationCeiling", 0.0) or 0.0))
        layers = document.get("layers", [])
        for layer in layers:
            parts = ["layer.add", "type=%s" % layer.get("waveform", "sine"),
                     "name=%s" % str(layer.get("name", "레이어")).replace(" ", "_"),
                     "env=%d" % (1 if layer.get("envelopeEnabled") else 0),
                     "mute=%d" % (1 if layer.get("muted") else 0),
                     "solo=%d" % (1 if layer.get("solo") else 0),
                     "localt=%d" % (1 if layer.get("formulaTimeFromLayerStart") else 0)]
            for key, token in FIELD_TOKEN.items():
                if key in layer:
                    parts.append("%s=%s" % (token, layer[key]))
            self.lab.send(" ".join(parts))
        for index, layer in enumerate(layers):
            if layer.get("waveform") == "formula" and layer.get("formula"):
                self.lab.send("layer.formula %d %s" % (index, layer["formula"]))

        # 소리도 같이 복원한다. 파일이 없으면 그렇다고 말하고 넘어간다 --
        # 프리셋 전체를 거절할 일은 아니다.
        speaker = document.get("speaker") or {}
        path = speaker.get("path") or ""
        if path and os.path.exists(path):
            self.lab.send("speaker.load %s" % path)
        else:
            if path:
                self.write_log("소리 파일을 찾지 못함: %s (진동만 불러옴)" % path)
            self.lab.send("speaker.clear")
        self.speaker_on.set(bool(speaker.get("enabled", True)))
        self.speaker_gain.set_value(speaker.get("gain", 1.0))
        self.speaker_offset.set_value(speaker.get("offsetMs", 0.0))
        self.push_speaker()
        self.selected = 0

    # ---------------------------------------------------------------- 트리거

    def sync_trigger_fields(self):
        """모드가 실제로 쓰는 값만 열어 둔다. 안 쓰는 칸이 채워져 있으면
        그 값이 적용된 것처럼 보인다."""
        mode = self.trigger_mode.get()
        used = {"off": set(),
                "feedback": {"start", "strength"},
                "weapon": {"start", "end", "strength"},
                "vibration": {"start", "strength", "freq"}}[mode]
        if mode != "off":
            used = used | {"ms"}
        for key, (label, box) in self.trigger_rows.items():
            enabled = key in used
            box.configure(state="normal" if enabled else "disabled")
            label.configure(foreground=INK if enabled else MUTED)

    def trigger_apply(self):
        side = "l" if self.trigger_side.get() == "L2" else "r"
        parts = ["trigger.apply", "side=%s" % side, "mode=%s" % self.trigger_mode.get()]
        for key in ("start", "end", "strength", "freq", "ms"):
            parts.append("%s=%s" % (key, self.trigger_vars[key].get() or "0"))
        self.lab.send(" ".join(parts))

    def trigger_release(self):
        side = "l" if self.trigger_side.get() == "L2" else "r"
        self.lab.send("trigger.release side=%s" % side)

    def trigger_reset(self):
        self.lab.send("trigger.reset")

    # ---------------------------------------------------------------- 그리기

    def any_solo(self):
        return any(layer.get("solo") for layer in self.preset.get("layers", []))

    def preset_length(self):
        """프리셋 자체가 끝나는 시각. 소리는 여기에 넣지 않는다 -- 합성 파형과
        분석은 프리셋 길이를 시간축으로 쓰고, 거기에 소리를 섞으면 그 그림들의
        시간축이 소리 길이에 따라 흔들린다."""
        layers = self.preset.get("layers", [])
        if not layers:
            return 1.0
        return max(1.0, max(float(l.get("startMs", 0)) + float(l.get("durationMs", 0))
                            for l in layers))

    def speaker_span_ms(self):
        """소리가 차지하는 구간. 실리지 않았거나 꺼져 있으면 없다."""
        if not self.speaker_wave or not self.speaker.get("loaded"):
            return None
        start = float(self.speaker.get("offsetMs", 0.0))
        return start, start + float(self.speaker.get("durationMs", 0.0))

    def timeline_end_ms(self):
        """타임라인이 덮어야 할 끝. 소리가 프리셋보다 길면 소리까지 보여야 한다 --
        패링 큐는 687 ms 인데 프리셋은 260 ms 라, 프리셋 길이로만 잡으면 소리의
        3분의 1만 보인다."""
        end = self.preset_length()
        span = self.speaker_span_ms()
        return max(end, span[1]) if span else end

    TIMELINE_LEFT = 96
    EDGE_GRAB_PX = 7          # 이 안쪽을 잡으면 길이 조절, 그 밖은 이동

    def timeline_span(self):
        """눈금의 오른쪽 끝(ms). 여유를 둬서 막대를 늘릴 자리가 항상 보이게 한다."""
        if self.frozen_span is not None:
            return self.frozen_span
        return max(100.0, self.timeline_end_ms() * 1.15)

    def timeline_geometry(self):
        width = max(240, self.timeline.winfo_width())
        return self.TIMELINE_LEFT, width - 10, self.timeline_span()

    def ms_to_x(self, ms):
        x_left, x_right, total = self.timeline_geometry()
        return x_left + (x_right - x_left) * ms / total

    def x_to_ms(self, x):
        x_left, x_right, total = self.timeline_geometry()
        return (x - x_left) / max(1.0, x_right - x_left) * total

    def draw_timeline(self):
        c = self.timeline
        c.delete("all")
        self.timeline_bars = []
        width = max(240, c.winfo_width())
        height = max(90, c.winfo_height())
        layers = self.preset.get("layers", [])
        x_left, x_right, total = self.timeline_geometry()
        span = self.speaker_span_ms()
        rows = max(1, len(layers) + (1 if span else 0))
        row_h = min(26, (height - 26) / rows)

        for i in range(5):
            x = x_left + (x_right - x_left) * i / 4
            c.create_line(x, 16, x, height - 4, fill="#e6e9ee")
            c.create_text(x, 8, text="%.0f" % (total * i / 4), fill=MUTED, font=("Consolas", 8))
        c.create_text(4, 8, anchor="nw", text="ms", fill=MUTED, font=("Consolas", 8))
        # 프리셋이 실제로 끝나는 자리. 눈금 끝과 다르므로 선으로 표시한다.
        end_x = self.ms_to_x(self.preset_length())
        c.create_line(end_x, 16, end_x, height - 4, fill="#d9c49a", dash=(3, 3))

        for index, layer in enumerate(layers):
            y = 20 + index * row_h
            start = float(layer.get("startMs", 0))
            dur = max(1.0, float(layer.get("durationMs", 1)))
            x0 = self.ms_to_x(start)
            x1 = max(self.ms_to_x(start + dur), x0 + 4)
            silent = layer.get("muted") or (self.any_solo() and not layer.get("solo"))
            chosen = index == self.selected
            top, bottom = y + 2, y + row_h - 4
            c.create_rectangle(x0, top, x1, bottom,
                               fill="#eef1f4" if silent else (OK_BG if chosen else "#e9f2f2"),
                               outline=TEAL if chosen else LINE, width=2 if chosen else 1)
            # 길이 조절용 손잡이. 잡을 수 있다는 것이 보여야 한다.
            c.create_line(x1 - 3, top + 3, x1 - 3, bottom - 3,
                          fill=TEAL if chosen else MUTED, width=2)
            self.timeline_bars.append((index, x0, x1, top, bottom))

            text = layer.get("name", "레이어")
            if layer.get("solo"):
                text += " [단독]"
            elif layer.get("muted"):
                text += " [음소거]"
            c.create_text(4, y + row_h / 2, anchor="w", text=text[:14],
                          fill=MUTED if silent else INK, font=("Malgun Gothic", 8))
            if x1 - x0 > 70:
                c.create_text((x0 + x1) / 2, y + row_h / 2,
                              text="%.0f→%.0f ms" % (start, start + dur),
                              fill=MUTED, font=("Consolas", 8))

        # 소리는 맨 아래 줄에. 레이어가 아니므로 timeline_bars 에 넣지 않는다 --
        # 끌어서 옮길 수 있는 것처럼 보이면 안 되고, 늦추기는 왼쪽 막대로 바꾼다.
        if span:
            y = 20 + len(layers) * row_h
            mid = y + row_h / 2
            half = row_h / 2 - 4
            x0 = self.ms_to_x(span[0])
            x1 = max(self.ms_to_x(span[1]), x0 + 3)
            c.create_rectangle(x0, y + 2, x1, y + row_h - 3, fill="#f4f6f8",
                               outline=LINE, dash=(3, 3))
            c.create_line(x0, mid, x1, mid, fill="#dfe3e9")
            wave = self.speaker_wave
            if len(wave) > 1:
                # 소리는 -1~1 을 다 쓰지 않는다(게임 레벨이라 peak 0.116). 제 최대값에
                # 맞춰 늘려 그린다 -- 여기서 보고 싶은 것은 크기가 아니라 시간에 따른
                # 모양이고, 실제 크기는 옆의 세기 막대와 측정 패널이 말한다.
                top = max(max(abs(lo), abs(hi)) for lo, hi in wave) or 1.0
                for i, (lo, hi) in enumerate(wave):
                    x = x0 + (x1 - x0) * i / (len(wave) - 1)
                    y0 = mid - (hi / top) * half
                    y1 = mid - (lo / top) * half
                    if abs(y1 - y0) < 1.0:
                        y1 = y0 + 1.0
                    c.create_line(x, y0, x, y1, fill=SLATE)
            label = "소리" if self.speaker.get("enabled", True) else "소리 (끔)"
            c.create_text(4, mid, anchor="w", text=label,
                          fill=INK if self.speaker.get("enabled", True) else MUTED,
                          font=("Malgun Gothic", 8))
            if x1 - x0 > 90:
                c.create_text((x0 + x1) / 2, y + row_h - 8,
                              text="%.0f→%.0f ms · 제 최대값에 맞춰 그림"
                                   % (span[0], span[1]),
                              fill=MUTED, font=("Consolas", 8))

    def bar_at(self, x, y):
        """커서 아래의 막대와, 오른쪽 끝을 잡았는지."""
        for index, x0, x1, top, bottom in self.timeline_bars:
            if top <= y <= bottom and x0 - 2 <= x <= x1 + 2:
                return index, abs(x - x1) <= self.EDGE_GRAB_PX
        return None, False

    def on_timeline_hover(self, event):
        if self.drag:
            return
        index, on_edge = self.bar_at(event.x, event.y)
        self.timeline.configure(
            cursor="sb_h_double_arrow" if (index is not None and on_edge)
            else ("fleur" if index is not None else ""))

    def on_timeline_press(self, event):
        index, on_edge = self.bar_at(event.x, event.y)
        if index is None:
            return
        self.selected = index
        self.layer_list.selection_clear(0, "end")
        self.layer_list.selection_set(index)
        self.refresh_inspector()
        layer = self.preset["layers"][index]
        # 눈금을 얼리고, 잡은 순간의 값을 기억한다. 드래그 중에는 이 값에
        # 이동량을 더해 계산하므로 오차가 누적되지 않는다.
        self.frozen_span = self.timeline_span()
        self.drag = {"index": index, "edge": on_edge, "x": event.x,
                     "start": float(layer.get("startMs", 0)),
                     "dur": float(layer.get("durationMs", 1))}
        self.draw_timeline()

    def on_timeline_drag(self, event):
        if not self.drag:
            return
        layer = self.preset["layers"][self.drag["index"]]
        moved = self.x_to_ms(event.x) - self.x_to_ms(self.drag["x"])
        if self.drag["edge"]:
            # 길이 조절. 1 ms 아래로는 내려가지 않고, 검증기의 10초 상한도 지킨다.
            dur = max(1.0, min(10000.0 - self.drag["start"], self.drag["dur"] + moved))
            layer["durationMs"] = round(dur)
        else:
            start = max(0.0, min(10000.0 - self.drag["dur"], self.drag["start"] + moved))
            layer["startMs"] = round(start)
        self.timeline_hint.config(
            text="%s   시작 %.0f ms · 길이 %.0f ms"
                 % (layer.get("name", "레이어"), layer["startMs"], layer["durationMs"]))
        self.draw_timeline()
        self.refresh_inspector()

    def on_timeline_release(self, _event=None):
        """놓을 때 한 번만 보낸다.

        끄는 동안 매번 보내면 명령 통로가 초당 수십 번 렌더를 시키게 되고,
        거절이라도 하나 섞이면 화면과 실제 설정이 어긋난다. 화면은 끄는 동안
        그 자리에서 미리 그려 두었으므로 반응이 늦어 보이지도 않는다.
        """
        if not self.drag:
            return
        index = self.drag["index"]
        layer = self.preset["layers"][index]
        self.drag = None
        self.frozen_span = None
        self.timeline.configure(cursor="")
        self.timeline_hint.config(
            text="막대를 끌면 시작 시각, 오른쪽 끝을 끌면 지속시간이 바뀐다.")
        self.lab.send("layer.set index=%d start=%s dur=%s"
                      % (index, layer["startMs"], layer["durationMs"]))

    def draw_band(self, canvas, series, colour, width, mid, span, dashed=False):
        if not series:
            return
        count = max(1, len(series) - 1)
        for i, pair in enumerate(series):
            lo = max(-1.2, min(1.2, pair[0]))
            hi = max(-1.2, min(1.2, pair[1]))
            x = width * i / count
            y0, y1 = mid - hi * span, mid - lo * span
            if abs(y1 - y0) < 1.0:
                y1 = y0 + 1.0
            canvas.create_line(x, y0, x, y1, fill=colour, dash=(2, 3) if dashed else None)

    def redraw_waveforms(self):
        self.draw_composite()
        self.draw_zoom()

    def layer_trace(self, index):
        """분석 결과 안의 레이어 파형. 순서는 레이어 목록과 같다."""
        traces = (self.analysis or {}).get("layers") or []
        if 0 <= index < len(traces):
            return traces[index]
        return None

    def on_composite_click(self, event):
        """레이어별 보기에서 줄을 클릭하면 그 레이어가 선택된다."""
        if self.view_mode.get() != "레이어별":
            return
        traces = (self.analysis or {}).get("layers") or []
        if not traces:
            return
        height = max(110, self.composite.winfo_height())
        lane = height / len(traces)
        index = min(len(traces) - 1, max(0, int(event.y / lane)))
        self.selected = index
        self.layer_list.selection_clear(0, "end")
        self.layer_list.selection_set(index)
        self.refresh_inspector()
        self.redraw_waveforms()
        self.draw_timeline()

    def draw_composite(self):
        c = self.composite
        c.delete("all")
        if not self.analysis:
            return
        width = max(240, c.winfo_width())
        height = max(110, c.winfo_height())
        mode = self.view_mode.get()

        if mode == "레이어별":
            self.draw_layer_lanes(c, width, height)
        elif mode == "선택 레이어":
            self.draw_single_layer(c, width, height)
        else:
            self.draw_sum(c, width, height)

        bucket = self.analysis.get("samplesPerBucket", 1)
        if bucket > 1:
            c.create_text(width - 4, height - 6, anchor="se",
                          text="픽셀당 최소/최대 (%d 샘플)" % bucket,
                          fill=MUTED, font=("Malgun Gothic", 8))

    def draw_sum(self, c, width, height):
        mid = height / 2
        span = mid - 14
        c.create_line(0, mid, width, mid, fill="#dfe3e9")
        for level in (1.0, -1.0):
            y = mid - level * span
            c.create_line(0, y, width, y, fill="#e0c79c", dash=(3, 4))
        c.create_text(width - 4, mid - span - 2, anchor="ne", text="±1.0 = 형식 한계",
                      fill=MUTED, font=("Consolas", 8))
        # 리미터 적용 후를 먼저 연하게, 원래 합성값을 그 위에.
        self.draw_band(c, self.analysis.get("limitedLeft"), "#cbd8d8", width, mid, span)
        self.draw_band(c, self.analysis.get("sumLeft"), TEAL, width, mid, span)
        self.draw_band(c, self.analysis.get("sumRight"), ORANGE, width, mid, span)
        c.create_text(4, 8, anchor="nw", text="합성 (모든 레이어의 합)", fill=MUTED,
                      font=("Malgun Gothic", 8))

    def draw_layer_lanes(self, c, width, height):
        """레이어마다 자기 줄에 그린다.

        모든 줄이 같은 세로 배율을 쓴다. 줄마다 최대값에 맞춰 늘리면 조용한
        레이어가 큰 레이어와 똑같이 커 보여서, 무엇이 실제로 센지를 눈으로
        비교할 수 없게 된다 -- 그 비교가 이 화면의 용도다.
        """
        traces = self.analysis.get("layers") or []
        layers = self.preset.get("layers", [])
        if not traces:
            return
        lane = height / len(traces)
        for index, trace in enumerate(traces):
            top = index * lane
            mid = top + lane / 2
            span = lane / 2 - 7
            silent = False
            if index < len(layers):
                silent = layers[index].get("muted") or \
                    (self.any_solo() and not layers[index].get("solo"))
            if index == self.selected:
                c.create_rectangle(0, top, width, top + lane, fill="#f4f9f9", outline="")
            c.create_line(0, mid, width, mid, fill="#e6e9ee")
            self.draw_band(c, trace.get("left"), "#cfd6dd" if silent else TEAL,
                           width, mid, span)
            self.draw_band(c, trace.get("right"), "#dcd6cf" if silent else ORANGE,
                           width, mid, span)
            label = trace.get("name", "레이어 %d" % (index + 1))
            if silent:
                label += " (안 들림)"
            c.create_text(4, top + 8, anchor="nw", text=label,
                          fill=MUTED if silent else INK, font=("Malgun Gothic", 8))
            if index:
                c.create_line(0, top, width, top, fill="#eceef1")
        c.create_text(width - 4, 8, anchor="ne", text="줄마다 같은 세로 배율",
                      fill=MUTED, font=("Malgun Gothic", 8))

    def draw_single_layer(self, c, width, height):
        trace = self.layer_trace(self.selected)
        if not trace:
            return
        mid = height / 2
        span = mid - 14
        c.create_line(0, mid, width, mid, fill="#dfe3e9")
        for level in (1.0, -1.0):
            y = mid - level * span
            c.create_line(0, y, width, y, fill="#e0c79c", dash=(3, 4))
        # 합성값을 연하게 뒤에 깔아 둔다. 이 레이어가 전체에서 어느 정도인지
        # 같이 보이지 않으면 혼자만 봤을 때 항상 커 보인다.
        self.draw_band(c, self.analysis.get("sumLeft"), "#e3e7ec", width, mid, span)
        self.draw_band(c, trace.get("left"), TEAL, width, mid, span)
        self.draw_band(c, trace.get("right"), ORANGE, width, mid, span)
        c.create_text(4, 8, anchor="nw",
                      text="%s   (연회색은 합성 전체)" % trace.get("name", ""),
                      fill=INK, font=("Malgun Gothic", 8))

    def zoom_window(self):
        total = self.analysis.get("lengthMs", 1.0) or 1.0
        start = min(self.zoom_start.get(), max(0.0, total - 1))
        span = max(1.0, min(self.zoom_span.get(), total - start))
        return total, start, span

    def draw_zoom(self):
        c = self.zoom
        c.delete("all")
        if not self.analysis:
            return
        width = max(240, c.winfo_width())
        height = max(100, c.winfo_height())
        mid = height / 2
        span_px = mid - 12
        # 확대 보기도 위의 보기 방식을 따라간다. 레이어를 따로 보는 중이라면
        # 확대해서 볼 대상도 그 레이어다.
        trace = self.layer_trace(self.selected) if self.view_mode.get() != "합성" else None
        if trace:
            series = trace.get("left") or []
            right = trace.get("right") or []
            caption = trace.get("name", "")
        else:
            series = self.analysis.get("sumLeft") or []
            right = self.analysis.get("sumRight") or []
            caption = "합성"
        total, start_ms, span_ms = self.zoom_window()
        i0 = int(len(series) * start_ms / total)
        i1 = max(i0 + 2, int(len(series) * (start_ms + span_ms) / total))

        c.create_line(0, mid, width, mid, fill="#dfe3e9")
        self.draw_band(c, series[i0:i1], TEAL, width, mid, span_px)
        self.draw_band(c, right[i0:i1], ORANGE, width, mid, span_px)
        c.create_text(4, 8, anchor="nw", text=caption, fill=MUTED, font=("Malgun Gothic", 8))
        self.zoom_readout.config(text="%.0f ~ %.0f ms" % (start_ms, start_ms + span_ms))

    def on_zoom_cursor(self, event):
        if not self.analysis:
            return
        width = max(1, self.zoom.winfo_width())
        _total, start_ms, span_ms = self.zoom_window()
        at = start_ms + span_ms * event.x / width
        mid = max(100, self.zoom.winfo_height()) / 2
        value = (mid - event.y) / max(1.0, mid - 12)
        self.zoom_readout.config(text="%.0f ~ %.0f ms   커서 %.2f ms / %.3f"
                                      % (start_ms, start_ms + span_ms, at, value))

    def draw_fft(self):
        c = self.fft_canvas
        c.delete("all")
        if not self.fft:
            return
        width = max(240, c.winfo_width())
        height = max(110, c.winfo_height())
        freq = self.fft["frequencyHz"]
        mag = self.fft["magnitude"]
        top_hz = 600.0        # 손이 쓸 범위. 그 위는 이 용도에 정보가 없다.
        usable = [i for i, f in enumerate(freq) if f <= top_hz]
        if len(usable) < 2:
            return
        peak = max(1e-9, max(mag[i] for i in usable))
        base = height - 20
        for i in range(7):
            x = 44 + (width - 54) * i / 6
            c.create_line(x, 6, x, base, fill="#eef0f4")
            c.create_text(x, base + 9, text="%.0f" % (top_hz * i / 6), fill=MUTED,
                          font=("Consolas", 8))
        c.create_text(4, 6, anchor="nw", text="PCM 진폭\n(물리 단위 아님)", fill=MUTED,
                      font=("Malgun Gothic", 8))
        points = []
        for i in usable:
            x = 44 + (width - 54) * freq[i] / top_hz
            points += [x, base - (base - 14) * (mag[i] / peak)]
        if len(points) >= 4:
            c.create_line(*points, fill=TEAL, width=1)
        c.create_text(width - 4, 6, anchor="ne", text="최대 %.4f @ %.0f Hz"
                      % (peak, freq[max(usable, key=lambda i: mag[i])]),
                      fill=INK, font=("Consolas", 9))

    def on_fft_cursor(self, event):
        if not self.fft:
            return
        width = max(1, self.fft_canvas.winfo_width())
        hz = max(0.0, (event.x - 44) / max(1, width - 54) * 600.0)
        freq = self.fft["frequencyHz"]
        mag = self.fft["magnitude"]
        best = min(range(len(freq)), key=lambda i: abs(freq[i] - hz))
        self.fft_caption.config(
            text="커서 %.1f Hz → %.5f    |    길이 %d · 창 %s · 분해능 %.2f Hz · 시작 %d 샘플 · %s · 단위 %s"
                 % (freq[best], mag[best], self.fft["size"], self.fft["window"],
                    self.fft["binHz"], self.fft["startFrame"], self.fft["channel"],
                    self.fft["unit"]))

    # ---------------------------------------------------------------- 갱신

    def refresh_layers(self):
        self.layer_list.delete(0, "end")
        for index, layer in enumerate(self.preset.get("layers", [])):
            marks = []
            if layer.get("solo"):
                marks.append("단독")
            if layer.get("muted"):
                marks.append("음소거")
            kind = WAVEFORM_LABEL.get(layer.get("waveform", "sine"), "").split(" ")[0]
            detail = ("수식" if layer.get("waveform") == "formula"
                      else "%.0f Hz" % float(layer.get("frequencyHz", 0)))
            self.layer_list.insert(
                "end", "%d. %s · %s · %s · %.0f→%.0f ms%s"
                % (index + 1, layer.get("name", "레이어"), kind, detail,
                   float(layer.get("startMs", 0)),
                   float(layer.get("startMs", 0)) + float(layer.get("durationMs", 0)),
                   ("  [" + "/".join(marks) + "]") if marks else ""))
        if self.preset.get("layers"):
            self.selected = min(self.selected, len(self.preset["layers"]) - 1)
            self.layer_list.selection_clear(0, "end")
            self.layer_list.selection_set(self.selected)
        self.refresh_inspector()
        self.draw_timeline()

    def on_layer_select(self, _event=None):
        picked = self.layer_list.curselection()
        if picked:
            self.selected = picked[0]
            self.refresh_inspector()
            self.draw_timeline()
            self.redraw_waveforms()

    def refresh_inspector(self):
        layer = self.current_layer()
        if not layer:
            return
        self.layer_name.set(layer.get("name", ""))
        self.waveform_box.set(WAVEFORM_LABEL.get(layer.get("waveform", "sine"), ""))
        self.env_on.set(bool(layer.get("envelopeEnabled")))
        self.master.set(fmt_param(self.preset.get("masterGain", 1.0)))
        self.master_bar.set_value(self.preset.get("masterGain", 1.0))
        ceiling = float(self.preset.get("saturationCeiling", 0.0) or 0.0)
        self.ceiling_on.set(ceiling > 0.0)
        if ceiling > 0.0:
            self.ceiling.set(fmt_param(ceiling))
            self.ceiling_bar.set_value(ceiling)
        kind = layer.get("waveform", "sine")
        for key, (name, box, unit, only) in self.rows.items():
            used = only is None or kind in only
            box.configure(state="normal" if used else "disabled")
            name.configure(foreground=INK if used else MUTED)
            if key in layer:
                self.field_vars[key].set(fmt_param(layer[key]))
                self.bars[key].set_value(layer[key])
            self.bars[key].set_enabled(used)
        self.master_bar.set_value(self.preset.get("masterGain", 1.0))
        self.formula.delete("1.0", "end")
        self.formula.insert("1.0", layer.get("formula", ""))
        self.formula_hint.config(
            text=("괄호 안은 라디안이다. sin(460*t) 는 460 Hz 가 아니라 "
                  "460÷(2π) ≈ %.1f Hz 다. 이 수식에는 포락선·정규화·필터가 자동으로 붙지 않는다."
                  % (460 / 6.283185307179586)) if kind == "formula" else "")

    def refresh_measurements(self):
        a = self.analysis
        if not a:
            return
        left, right = a["statsLeft"], a["statsRight"]
        limited = a["statsLimitedLeft"]
        lines = [
            "합성 결과 (리미터 적용 전)",
            "단위: PCM 샘플 값",
            "",
            "             왼쪽       오른쪽",
            "peak    %10s %10s" % (fmt(left["peak"], 4), fmt(right["peak"], 4)),
            "RMS     %10s %10s" % (fmt(left["rms"], 4), fmt(right["rms"], 4)),
            "평균 DC %10s %10s" % (fmt(left["mean"], 5), fmt(right["mean"], 5)),
            "범위초과%10d %10d" % (left["overRange"], right["overRange"]),
            "",
            ("출력 포화 %s" % (("켬 %s" % fmt(a.get("saturationCeiling", 0), 2))
                              if a.get("saturationCeiling", 0) > 0 else "끔")),
            "",
            "리미터 적용 후 (문턱 %s)" % fmt(a["limiterThreshold"], 2),
            "  peak %s   RMS %s" % (fmt(limited["peak"], 4), fmt(limited["rms"], 4)),
            "  감쇄 최대 -%s dB, %d 프레임" % (fmt(a["worstReductionDbLeft"], 2),
                                            a["limitedFramesLeft"]),
            "  최종 한계 초과 %d 샘플" % a["clampedLeft"],
            "",
            "길이 %s ms · %d 샘플 @ %d Hz" % (fmt(a["lengthMs"], 1), a["frames"], a["sampleRate"]),
        ]
        if a["guardedDivisions"] or a["nonFiniteExpression"] or a["nonFiniteSamples"]:
            lines += ["", "※ 수식 경고",
                      "  0 으로 나눔 %d / 계산 불가 %d / 비정상 %d"
                      % (a["guardedDivisions"], a["nonFiniteExpression"], a["nonFiniteSamples"])]
        if a.get("envelopeScaled"):
            lines += ["", "※ 포락선이 지속시간에 맞게 축소됨:",
                      "  " + ", ".join(a["envelopeScaled"])]
        self.measure_text.delete("1.0", "end")
        self.measure_text.insert("1.0", "\n".join(lines))

    # ---------------------------------------------------------------- 상태

    def set_status(self, text, colour):
        self.status.config(text="● " + text, bg=colour)

    def flash(self, text):
        self.set_status(text[:60], WARN_BG)
        self.write_log("! " + text)

    def write_log(self, text):
        self.log.insert("end", "[%s] %s\n" % (time.strftime("%H:%M:%S"), text))
        self.log.see("end")

    # ---------------------------------------------------------------- 수신

    def drain(self):
        while True:
            try:
                msg = self.lab.replies.get_nowait()
            except queue.Empty:
                break
            self.handle(msg)
        self.root.after(50, self.drain)

    def handle(self, msg):
        event = msg.get("event")
        if event == "ready":
            self.channels = (msg.get("hapticLeft"), msg.get("hapticRight"))
            self.sample_rate = msg.get("sampleRate", 48000)
            self.endpoint_id = msg.get("endpointId")
            self.device_box.configure(values=[msg.get("endpoint", "")])
            self.device_box.set(msg.get("endpoint", ""))
            routed = msg.get("routingClaimed")
            self.set_status("연결됨 · 출력 준비" if routed else "연결됨 · 라우팅 미확보",
                            OK_BG if routed else WARN_BG)
            self.device_text.delete("1.0", "end")
            self.device_text.insert(
                "1.0",
                "엔드포인트  : %s\n안정 식별자 : %s\n형식        : %s Hz / %s 채널\n"
                "햅틱 채널   : 왼쪽 ch%s · 오른쪽 ch%s\n라우팅      : %s\n설정 파일   : %s\n"
                % (msg.get("endpoint"), msg.get("endpointId"), msg.get("sampleRate"),
                   msg.get("channels"), msg.get("hapticLeft"), msg.get("hapticRight"),
                   "확보됨" if routed else "확보 실패", msg.get("config")))
            self.lab.send("device.list")
            self.refresh_library()
            self.speaker_readout()
            self.draw_schematic()
        elif event == "preset":
            self.preset = msg["preset"]
            self.project.set(self.preset.get("name", ""))
            if self.pending_formula:
                index = len(self.preset.get("layers", [])) - 1
                formula, self.pending_formula = self.pending_formula, None
                if index >= 0:
                    self.lab.send("layer.formula %d %s" % (index, formula))
                    return
            self.selected = min(self.selected, max(0, len(self.preset.get("layers", [])) - 1))
            self.refresh_layers()
            self.request_analysis()
        elif event == "analysis":
            self.analysis = msg
            self.redraw_waveforms()
            self.refresh_measurements()
        elif event == "fft":
            self.fft = msg
            self.draw_fft()
            self.fft_caption.config(
                text="길이 %d · 창 %s · 분해능 %.2f Hz · 시작 %d 샘플 · %s · 단위 %s"
                     % (msg["size"], msg["window"], msg["binHz"], msg["startFrame"],
                        msg["channel"], msg["unit"]))
        elif event == "speaker":
            self.speaker = msg.get("speaker", {})
            self.speaker_wave = msg.get("waveform", [])
            path = self.speaker.get("path") or ""
            self.speaker_label.config(
                text=("%s\n%s" % (os.path.basename(path), msg.get("report", ""))) if path
                else "없음 — 진동만 나온다")
            self.speaker_readout()
            self.draw_timeline()
        elif event == "devices":
            names = ["[%d] %s%s" % (e["index"], e["name"],
                                    "  (컨트롤러로 보임)" if e["looksLikeDualSense"] else "")
                     for e in msg["endpoints"]]
            self.device_box.configure(values=names)
            for e in msg["endpoints"]:
                if e["id"] == self.endpoint_id:
                    self.device_box.set("[%d] %s" % (e["index"], e["name"]))
        elif event == "played":
            self.set_status("재생 (%s ms)" % fmt(msg.get("lengthMs", 0), 0), OK_BG)
        elif event == "stopped":
            self.set_status("정지됨", OK_BG)
        elif event == "trigger":
            action = msg.get("action")
            if action == "apply":
                side = "L2" if msg.get("side") == "left" else "R2"
                ok = msg.get("accepted")
                self.trigger_state.config(
                    text="%s %s: %s%s" % (side, msg.get("mode"), "적용됨" if ok else "거절됨",
                                          "" if ok else " — " + (msg.get("error") or "")))
            elif action == "release":
                self.trigger_state.config(text="해제됨")
            elif action == "reset":
                self.trigger_state.config(text="트리거 미적용")
            self.draw_schematic()
        elif event == "exported":
            self.write_log("WAV 내보내기: %s (%d 샘플 @ %d Hz)"
                           % (msg.get("wav") or "실패", msg.get("frames", 0),
                              msg.get("sampleRate", 0)))
        elif event == "rejected":
            self.flash(msg.get("message", ""))
        elif event == "error":
            self.set_status(msg.get("message", "오류")[:70], BAD_BG)
            self.write_log("오류: %s" % msg.get("message"))
        elif event == "closed":
            self.set_status("출력 프로세스 종료됨", BAD_BG)

    def quit(self):
        self.lab.close()
        self.root.destroy()


def main():
    exe = DEFAULT_EXE
    args = sys.argv[1:]
    if args and args[0].endswith(".exe"):
        exe, args = args[0], args[1:]
    if not os.path.exists(exe):
        print("찾을 수 없음: %s\n먼저 빌드하거나 첫 인자로 exe 경로를 넘긴다." % exe)
        return 2
    lab = LabProcess(exe, args)
    root = tk.Tk()
    Workbench(root, lab)
    try:
        root.mainloop()
    finally:
        lab.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
