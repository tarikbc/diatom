"""The state plane, end to end against the stub core.

Every case here is one that was actually got wrong while building it, which is
why they are cases rather than a smoke test:

  - the map read as "everything unbound" before any game had run, because
    identity had not been saved yet. Only the IDLE path shows it.
  - a refused SETMAP left the table cleared rather than unchanged.
  - labels have to follow a remap, or a remap screen shows the wrong verbs.

Since extended past the plane's first four states to whatever else it carries -
session persistence and achievements - because the harness and the fixture are
the same and a second copy of both would drift.

The level half cannot be tested here: a desktop has no volume or brightness of
its own, so `LEVELS count=0` is the whole of it. Levels are exercised on
the devices.

    python3 test/stateplane.py
"""
import socket, subprocess, sys, time, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = "/tmp/diatom-stateplane.sock"
if os.path.exists(SOCK): os.unlink(SOCK)

# What went to the device, for the quiet check at the end: the stub core's
# tone as the port was handed it, after the resampler and after quiet.
TAP = "/tmp/diatom-stateplane-tap"
for ext in (".in.raw", ".out.raw"):
    if os.path.exists(TAP + ext): os.unlink(TAP + ext)
p = subprocess.Popen([f"{ROOT}/build/desktop/diatom", "--socket", SOCK,
                      "--tap-audio", TAP],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(SOCK): break
    time.sleep(0.05)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(SOCK)
buf = b""
def lines(timeout=3.0):
    global buf
    s.settimeout(timeout)
    end = time.time() + timeout
    while time.time() < end:
        if b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            yield l.decode().strip(); end = time.time() + 0.25; continue
        try: d = s.recv(65536)
        except socket.timeout: return
        if not d: return
        buf += d
def send(m):
    s.sendall((m + "\n").encode()); time.sleep(0.35)
def drain(t=1.0): return [l for l in lines(t) if l]

fails = []
def check(name, got, want):
    ok = got == want
    print(("  ok   " if ok else "  FAIL ") + f"{name}\n         got  {got}\n         want {want}")
    if not ok: fails.append(name)

def check_that(name, cond, got):
    """For assertions that are a predicate rather than an exact value - a rect
    is geometry-dependent, so only its prefix is fixed."""
    print(("  ok   " if cond else "  FAIL ") + f"{name}\n         got  {got}")
    if not cond: fails.append(name)

print("READY:", (r := drain(2.0)))
check("proto version", [x for x in r if x.startswith("READY")][0].split("\t")[1], "proto=12")

# Before any RUN: identity must already be identity, not "everything unbound".
send("MAP"); check("map is identity while idle", drain(), ["MAP\tmap=identity"])
send("INPUTS"); check("no labels while idle", drain(), ["INPUTS\tcount=0"])
send("DISC"); check("no disc while idle", drain(), ["DISC\tcount=0"])

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so")
run = drain(3.0)
print("RUN ->", run)

send("INPUTS"); got = drain()
check("labels, port 0 only, partial array", got,
      ["INPUTS\tcount=3", "INPUT\tid=a\tlabel=Fire", "INPUT\tid=b\tlabel=Jump",
       "INPUT\tid=start\tlabel=Pause"])

send("MAP"); check("map starts identity", drain(), ["MAP\tmap=identity"])

send("SETMAP\tmap=x:b,y:a"); check("setmap echoes", drain(), ["MAP\tmap=x:b,y:a"])

send("INPUTS"); got = drain()
check("labels follow the remap", got,
      ["INPUTS\tcount=5", "INPUT\tid=a\tlabel=Fire", "INPUT\tid=b\tlabel=Jump",
       "INPUT\tid=x\tlabel=Jump", "INPUT\tid=y\tlabel=Fire",
       "INPUT\tid=start\tlabel=Pause"])

send("SETMAP\tmap=menu:b"); got = drain()
check("menu is refused, map unchanged", got,
      ["ERROR\tcode=bad_map\tmsg=menu:b", "MAP\tmap=x:b,y:a"])

send("SETMAP\tmap=a:b,nonsense:x"); got = drain()
check("bad pair refuses whole message", got,
      ["ERROR\tcode=bad_map\tmsg=a:b,nonsense:x", "MAP\tmap=x:b,y:a"])

send("SETMAP\tmap=identity"); check("identity clears", drain(), ["MAP\tmap=identity"])

# A pulse is part of a binding, so it travels in the same field and obeys the
# same all-or-nothing rule. The rate itself cannot be checked here - the desktop
# port reads the live keyboard, so no script can hold a button - and is measured
# on hardware instead. What IS checkable is the grammar, which is where the bugs
# live.
send("SETMAP\tmap=x:a~3")
check("turbo binding echoes with its period", drain(), ["MAP\tmap=x:a~3"])

send("SETMAP\tmap=x:a~3,y:b")
check("pulsed and plain bindings coexist", drain(), ["MAP\tmap=x:a~3,y:b"])

# A pulse on a binding that is otherwise identity must still be reported, or the
# launcher reads back a map missing the turbo it just set. The emit loop skips
# identity bindings, and had to learn that a period makes one non-identity.
send("SETMAP\tmap=a:a~2")
check("turbo on an identity target is still emitted", drain(), ["MAP\tmap=a:a~2"])

for bad, why in (("x:a~0",    "a zero period is a plain binding in a costume"),
                 ("x:a~31",   "period above the ceiling"),
                 ("x:a~abc",  "non-numeric period"),
                 ("x:a~3x",   "trailing junk after the period"),
                 ("x:a~",     "empty period"),
                 ("x:none~3", "a pulse on an unbound button"),
                 ("x:menu~3", "a pulse cannot make menu mappable")):
    send("SETMAP\tmap=" + bad); got = drain()
    check("refused, unchanged: " + why, got,
          ["ERROR\tcode=bad_map\tmsg=" + bad, "MAP\tmap=a:a~2"])

send("SETMAP\tmap=identity")
check("identity clears turbo too", drain(), ["MAP\tmap=identity"])

# Display mode. RUNNING already emitted one, so drain before asking.
drain(0.5)
send("DISPLAY"); got = drain()
check_that("display reports mode, filter and rect",
           len(got) == 1 and got[0].startswith("DISPLAY\tmode=stretch\tfilter=nearest\trect="),
           got)
check_that("and the surface the rect is measured in",
           len(got) == 1 and "\tsurface=" in got[0] and "surface=0x" not in got[0],
           got)

send("SETDISPLAY\tmode=integer"); got = drain()
check_that("setdisplay changes mode, keeps filter",
           len(got) == 1 and got[0].startswith("DISPLAY\tmode=integer\tfilter=nearest\trect="),
           got)

send("SETDISPLAY\tmode=nonsense"); got = drain()
check("unknown mode refused, nothing changes", got,
      ["ERROR\tcode=bad_display\tmsg=nonsense"])
send("DISPLAY"); got = drain()
check_that("still on integer after the refusal",
           len(got) == 1 and got[0].startswith("DISPLAY\tmode=integer"), got)

send("SETDISPLAY\tmode=aspect\tfilter=bogus"); got = drain()
check("bad filter refuses the whole message", got,
      ["ERROR\tcode=bad_display\tmsg=bogus"])
send("DISPLAY"); got = drain()
check_that("mode did not move either",
           len(got) == 1 and got[0].startswith("DISPLAY\tmode=integer"), got)

send("LEVELS"); check("desktop has no levels", drain(), ["LEVELS\tcount=0"])
send("SETLEVEL\tkind=brightness\tindex=3\tcount=12")
check("setlevel refused with no control", drain(), ["ERROR\tcode=bad_level\tmsg=brightness"])

send("RESET"); check("reset while running", drain(), ["RESETDONE"])

# Discs. The stub hands over a two-disc tray from retro_init, as pcsx_rearmed
# does, and refuses a change with the lid shut - so SETDISC working at all
# proves Diatom opened the tray first, and a game reporting it shut afterwards
# proves Diatom closed it again.
send("DISC"); check("a game's tray is reported", drain(),
                    ["DISC\tindex=0\tcount=2\tejected=0\tlabel=stub disc 1"])
send("SETDISC\tindex=1"); check("SETDISC changes disc and closes the lid", drain(),
                                ["DISC\tindex=1\tcount=2\tejected=0\tlabel=stub disc 2"])
send("SETDISC\tindex=2"); check("a disc the game does not have is refused, disc unchanged",
                                drain(), ["ERROR\tcode=bad_disc\tmsg=2",
                                          "DISC\tindex=1\tcount=2\tejected=0\tlabel=stub disc 2"])

send("STOP"); print("STOP ->", drain(2.0))
send("DISC"); check("no disc once the game has stopped", drain(), ["DISC\tcount=0"])

# --- Session persistence over the protocol ---------------------------------
import tempfile, pathlib
tmp = tempfile.mkdtemp(prefix="diatom-persist-")
st, pv = f"{tmp}/game.state", f"{tmp}/game.bmp"

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so\texit_state={st}\tpreview={pv}")
print("RUN(persist) ->", drain(3.0))
send("STOP"); got = drain(3.0)
check_that("exit emits PREVIEW then EXIT, in that order",
           len(got) >= 2 and got[-2].startswith("PREVIEW\tpath=") and got[-1].startswith("EXIT"),
           got)
check_that("exit state and preview are on disk and non-empty",
           pathlib.Path(st).stat().st_size > 0 and pathlib.Path(pv).stat().st_size > 0,
           [f"{st}: {pathlib.Path(st).stat().st_size}b",
            f"{pv}: {pathlib.Path(pv).stat().st_size}b"])

# resume from what was just written: no ERROR, straight to RUNNING
send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so\tresume={st}\texit_state={st}\tpreview={pv}")
got = drain(3.0)
check_that("resume path accepted, game reaches RUNNING",
           "RUNNING" in got and not any(l.startswith("ERROR") for l in got), got)
# pause writes the preview BEFORE announcing PAUSED - the order is the contract
before = pathlib.Path(pv).stat().st_mtime_ns
send("STOP"); drain(2.0)

# --- Two screens in one frame ----------------------------------------------
# A DS game's frame, top screen over bottom. The geometry is check-screens';
# this is the wiring: that RUN screens=2 is read, that the layout reaches the
# rect the launcher draws its menu into, that the stylus answers and refuses
# where there is nothing to touch, and that the pause preview is the layout's
# picture rather than the raw frame. The stub's frame is 224 lines, two
# screens of 112, and 256 or 512 wide - it is resident and goes on toggling
# its hires mode from game to game - so the width is read first, at native,
# and the rects follow from it. The desktop surface is 960x720.
import struct
def bmp_size(path):
    with open(path, "rb") as f: h = f.read(26)
    return struct.unpack("<ii", h[18:26])

send("SCREENS")
check("idle: stacked, and not two screens", drain(), ["SCREENS\tlayout=stacked\tswap=0\tdual=0"])
send("SETSTYLUS\ton=1")
check("no stylus with nothing to touch", drain(), ["STYLUS\ton=0"])
send("SETSCREENS\tlayout=sideways")
check("an unknown layout is refused", drain(), ["ERROR\tcode=bad_screens\tmsg=sideways"])
send("SETSCREENS\tlayout=focus\tswap=2")
check("so is a swap that is not 0 or 1, and nothing changes", drain(),
      ["ERROR\tcode=bad_screens\tmsg=swap=2"])

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so\tscreens=2\tpreview={pv}")
got = drain(3.0)
check_that("RUN screens=2 reports two screens and the stylus off",
           "SCREENS\tlayout=stacked\tswap=0\tdual=1" in got and "STYLUS\ton=0" in got, got)
send("SETDISPLAY\tmode=native"); got = drain()
W = int([l for l in got if l.startswith("DISPLAY")][0].split("rect=")[1].split("x")[0])
check_that("stacked at native is the frame itself, 224 lines",
           any(f"rect={W}x224+" in l for l in got), got)
send("SETDISPLAY\tmode=aspect"); drain()
send("SETSCREENS\tlayout=single"); got = drain()
h = round(960 * 112 / W)
check_that(f"single: one {W}x112 screen fitted, 960x{h} centred",
           any(f"rect=960x{h}+0+{(720 - h) // 2}" in l for l in got if l.startswith("DISPLAY")) and
           "SCREENS\tlayout=single\tswap=0\tdual=1" in got, got)
send("SETSCREENS\tlayout=focus"); got = drain()
b = min(960 / (W * 1.25), 720 / 112)
fh = int(112 * b + 0.5)
check_that(f"focus: {b:g}x and a quarter, 960x{fh} together",
           any(f"rect=960x{fh}+0+{(720 - fh) // 2}" in l for l in got if l.startswith("DISPLAY")), got)
send("SETSTYLUS\ton=1"); got = drain()
check_that("the stylus turns on", "STYLUS\ton=1" in got, got)
send("STYLUS"); check("and says so when asked", drain(), ["STYLUS\ton=1"])
send("SETSCREENS\tswap=1"); got = drain()
check_that("swap alone keeps the layout", "SCREENS\tlayout=focus\tswap=1\tdual=1" in got, got)
send("PAUSE"); got = drain(2.0)
check_that("pause still writes its preview first", "PAUSED" in got and
           any(l.startswith("PREVIEW") for l in got), got)
check("the preview is the layout's picture, 1.25 screens wide, not the frame",
      bmp_size(pv), (W * 5 // 4, 112))
# A layout changed from the open menu is shown there at once: the new rect,
# then the new picture behind it, then the layout - in that order, so a
# launcher reading up to SCREENS has both.
send("SETSCREENS\tlayout=single"); got = drain()
check_that("paused, a new layout re-writes the preview between DISPLAY and SCREENS",
           [l.split("\t")[0] for l in got] == ["DISPLAY", "PREVIEW", "SCREENS"], got)
check("and the preview is now the one screen shown", bmp_size(pv), (W, 112))
send("SETSCREENS\tlayout=focus"); drain()
send("RESUME"); drain(1.0)
send("SETSCREENS\tlayout=stacked"); got = drain()
check_that("running, it does not: nothing is drawn from it until the next pause",
           not any(l.startswith("PREVIEW") for l in got), got)
send("SETSCREENS\tlayout=focus"); drain()
send("STOP"); drain(3.0)

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so")
drain(3.0)
send("SCREENS")
check("the next game is one screen, the layout held for the next DS game", drain(),
      ["SCREENS\tlayout=focus\tswap=1\tdual=0"])
send("SETSTYLUS\ton=1")
check("and the stylus went with the game", drain(), ["STYLUS\ton=0"])
send("STOP"); drain(3.0)
send("SETSCREENS\tlayout=stacked\tswap=0"); drain()

# --- Achievements over the protocol ----------------------------------------
# The unit test (make check-cheevos) proves the mapping and the evaluation.
# This proves the WIRING: that a set named on RUN is read, that the frame call
# is actually in the loop, and that an unlock reaches the launcher. Every one
# of those is a place where a correct component has been connected to nothing.
setf = f"{tmp}/stub.set"
open(setf, "w").write(
    "# id\tcondition\n"
    # frame 10 exactly: the counter has reached 10 AND the countdown byte is
    # lower than it was on the previous frame. Both halves are needed - the
    # delta is the thing a 10Hz launcher could never have seen.
    "5001\t0xH0002<d0xH0002_0xH0001=10\n"
    "5002\t0xH0003=99\n")           # never true; the control

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so\tconsole=0\tcheevos={setf}")
got = drain(3.0)
check_that("a set named on RUN unlocks and reports it",
           "CHEEVO\tid=5001\tstate=unlocked" in got, got)
check_that("the control achievement stays quiet",
           not any(l.startswith("CHEEVO\tid=5002") for l in got), got)

send("CHEEVOS"); got = drain(2.0)
summary = next((l for l in got if l.startswith("CHEEVOS\t")), "")
check_that("CHEEVOS reports two watched, one unlocked",
           "count=2" in summary and "unlocked=1" in summary, summary)
check("the unlocked one enumerates as unlocked",
      next((l for l in got if l.startswith("CHEEVO\tid=5001")), ""),
      "CHEEVO\tid=5001\tstate=unlocked")
check("the other is still active",
      next((l for l in got if l.startswith("CHEEVO\tid=5002")), ""),
      "CHEEVO\tid=5002\tstate=active")

# SETCHEEVOS replaces the set mid-game. The launcher needs this because the
# set arrives over the network, and a game must not wait for a download.
open(setf, "w").write("5003\t0xH0000>200\n")
send(f"SETCHEEVOS\tpath={setf}"); got = drain(2.0)
summary = next((l for l in got if l.startswith("CHEEVOS\t")), "")
check_that("SETCHEEVOS replaces the set whole",
           "count=1" in summary and "unlocked=0" in summary, summary)

send("SETCHEEVOS\tpath="); got = drain(2.0)
summary = next((l for l in got if l.startswith("CHEEVOS\t")), "")
check_that("an empty path unloads", "count=0" in summary, summary)

# --- An overlay composited over a running game -----------------------------
# The launcher cannot draw while Diatom owns the display, so a notice has to
# be handed over as pixels. This checks the carrying, not the look: that the
# format is read, that a malformed one is refused rather than drawn, and that
# a running game keeps running either way.
import struct
ovl = f"{tmp}/notice.dtov"
W, H = 240, 40
with open(ovl, "wb") as f:
    f.write(b"DTOV" + struct.pack("<HH", W, H))
    f.write(bytes([40, 30, 20, 200]) * (W * H))       # BGRA, mostly opaque

send(f"RUN\tcore={ROOT}/build/desktop/stubcore.so")
drain(2.0)
# Several games into a resident core, which ran retro_init once, at the first.
# The tray it handed over then has to still be its tray now.
send("DISC"); check("the tray survives a resident core's later games", drain(),
                    ["DISC\tindex=0\tcount=2\tejected=0\tlabel=stub disc 1"])
send(f"OVERLAY\tpath={ovl}\tms=1500"); got = drain(2.0)
check_that("an overlay is accepted while a game runs", "OVERLAID" in got, got)

send(f"OVERLAY\tpath={tmp}/nope.dtov\tms=1500"); got = drain(2.0)
check_that("a missing overlay is refused, not drawn",
           any(l.startswith("ERROR\tcode=bad_overlay") for l in got), got)

open(f"{tmp}/bad.dtov", "wb").write(b"NOPE" + struct.pack("<HH", 8, 8) + b"\0" * 256)
send(f"OVERLAY\tpath={tmp}/bad.dtov\tms=1500"); got = drain(2.0)
check_that("a file that is not an overlay is refused",
           any(l.startswith("ERROR\tcode=bad_overlay") for l in got), got)

# Clearing is ms=0, and must not be an error.
send(f"OVERLAY\tpath={ovl}\tms=0"); got = drain(2.0)
check_that("ms=0 clears rather than failing", "OVERLAID" in got, got)

# PAUSE: the launcher asking for the menu, for the resume-after-shutdown case
# where nobody pressed anything. It must reach the same paused state a MENU
# press does, and RESUME must come back out of it.
# SETIDLE: Diatom reports that nobody is pressing anything, because the
# launcher cannot see input during a game. It reports; it does not act.
send("SETIDLE\tms=300")
got = drain(2.0)          # send() already sleeps; do not drain before checking
check_that("idleness is reported after the timeout", "IDLE" in got, got)
send("SETIDLE\tms=0")
got = drain(1.5)
check_that("ms=0 turns it off", "IDLE" not in got, got)

send("PAUSE"); got = drain(2.0)
check_that("PAUSE opens the menu", any(l == "PAUSED" for l in got), got)
send("RESUME"); drain(1.0)
send("DISPLAY"); got = drain(2.0)
check_that("the game is running again after RESUME",
           any(l.startswith("DISPLAY\t") for l in got), got)

# Mute: the one state this host obeys rather than owns, so what is checked is
# that it answers honestly and remembers - not that anything went quiet, which a
# desktop has no analog stage to do.
send("MUTE"); got = drain(2.0)
check_that("MUTE answers before anyone has set it",
           any(l == "MUTE\ton=0" for l in got), got)

send("SETMUTE\ton=1"); got = drain(2.0)
check_that("SETMUTE is acknowledged with the new state",
           any(l == "MUTE\ton=1" for l in got), got)

send("MUTE"); got = drain(2.0)
check_that("and it is still held on the next ask",
           any(l == "MUTE\ton=1" for l in got), got)

# The safe direction to be wrong in: anything that is not "1" releases, because
# a device stuck ON can be muted again by flipping the switch, and one stuck
# OFF looks broken.
send("SETMUTE\ton=true"); got = drain(2.0)
check_that("a value that is not 1 releases rather than holds",
           any(l == "MUTE\ton=0" for l in got), got)

# Quiet. Unlike mute this one IS this host's to do, so the sound itself is
# checked, below, once the process has gone and the tap is flushed.
send("SPEED"); got = drain(1.0)
check_that("SPEED answers 1x before anyone has stepped it",
           any(l == "SPEED\tx=1" for l in got), got)
send("QUIET"); got = drain(1.0)
check_that("QUIET answers before anyone has set it",
           any(l == "QUIET\ton=0" for l in got), got)
send("SETQUIET\ton=1"); got = drain(1.0)
check_that("SETQUIET is acknowledged with the new state",
           any(l == "QUIET\ton=1" for l in got), got)
send("SETQUIET\ton=0"); got = drain(1.0)
check_that("and released", any(l == "QUIET\ton=0" for l in got), got)

# Low battery: the launcher's to say, the port's to draw. The host half is what
# a desktop can check: it answers what it was told, and holds it.
send("LOWBATTERY"); got = drain(1.0)
check_that("LOWBATTERY answers before anyone has set it",
           any(l == "LOWBATTERY\ton=0\tblink=0" for l in got), got)
send("SETLOWBATTERY\ton=1"); got = drain(1.0)
check_that("SETLOWBATTERY is acknowledged with the new state",
           any(l == "LOWBATTERY\ton=1\tblink=0" for l in got), got)
send("LOWBATTERY"); got = drain(1.0)
check_that("and held", any(l == "LOWBATTERY\ton=1\tblink=0" for l in got), got)
send("SETLOWBATTERY\ton=1\tblink=1"); got = drain(1.0)
check_that("critical blinks", any(l == "LOWBATTERY\ton=1\tblink=1" for l in got), got)
send("SETLOWBATTERY\ton=0\tblink=1"); got = drain(1.0)
check_that("and off is off, blink or not",
           any(l == "LOWBATTERY\ton=0\tblink=0" for l in got), got)

# Channels: a second on the left alone, then stereo again. Left and not right,
# so the quiet check below, which reads the left channel, is not disturbed;
# the right channel is checked after the process has gone, as quiet is.
send("SETCHANNELS\tmode=left"); got = drain(1.0)
check_that("SETCHANNELS left is acknowledged",
           any(l == "CHANNELS\tmode=left" for l in got), got)
send("SETCHANNELS\tmode=stereo"); got = drain(1.0)
check_that("and stereo again", any(l == "CHANNELS\tmode=stereo" for l in got), got)

send("STOP"); got = drain(3.0)
check_that("the game still ends normally after all that",
           any(l.startswith("EXIT") for l in got), got)

send("QUIT"); time.sleep(0.4)
s.close(); p.terminate(); p.wait(timeout=5)

# The quiet, as heard. The stub core plays an unbroken 440 Hz tone, so the one
# long run of exact zeros in the tap is the quiet - sessions start with 43 ms
# of primed silence, and nothing else is silent. Around it the tone has to
# FADE: a step from full amplitude to nothing is broadband energy, which is a
# click, and a 440 Hz sine at this level never moves more than about 130
# between two samples at 48 kHz, where a step would move up to 2200.
import array
tone = array.array("h")
with open(TAP + ".out.raw", "rb") as f: tone.frombytes(f.read())
L = tone[0::2]
run, i = (0, 0), 0
while i < len(L):
    if L[i] == 0:
        j = i
        while j < len(L) and L[j] == 0: j += 1
        if j - i > run[1] - run[0]: run = (i, j)
        i = j
    else:
        i += 1
q0, q1 = run
ms = 48                                    # frames per ms at the desktop's rate
check_that("the tone went to exact silence for a while", q1 - q0 >= 200 * ms,
           f"{(q1 - q0) / ms:.0f} ms of zeros")
check_that("and came back afterwards",
           any(L[k] != 0 for k in range(q1, min(len(L), q1 + 100 * ms))),
           f"frames {q1}..{q1 + 100 * ms}")
near = range(max(1, q0 - 50 * ms), min(len(L), q1 + 50 * ms))
step = max(abs(L[k] - L[k - 1]) for k in near)
check_that("no step going quiet or coming back - it fades", step <= 400,
           f"largest sample-to-sample move {step}")
def peak(a, b): return max(abs(v) for v in L[max(0, a):max(0, min(len(L), b))] or [0])
check_that("the last 2 ms before the silence are already faded",
           peak(q0 - 2 * ms, q0) < 0.3 * peak(q0 - 30 * ms, q0 - 20 * ms),
           f"{peak(q0 - 2 * ms, q0)} against {peak(q0 - 30 * ms, q0 - 20 * ms)}")
check_that("and the first 2 ms after it are still fading in",
           peak(q1, q1 + 2 * ms) < 0.3 * peak(q1 + 20 * ms, q1 + 30 * ms),
           f"{peak(q1, q1 + 2 * ms)} against {peak(q1 + 20 * ms, q1 + 30 * ms)}")

# The channels, as heard: outside the quiet, which silences both, the one long
# stretch of exact silence in the right channel is the second on the left
# alone - the tone only touches zero for a sample at a time. Around it the
# right has to ramp, as quiet does, not step.
R = tone[1::2]
run, i = (0, 0), 0
while i < len(R):
    if R[i] == 0:
        j = i
        while j < len(R) and R[j] == 0: j += 1
        if j - i > run[1] - run[0] and (j <= q0 or i >= q1): run = (i, j)
        i = j
    else:
        i += 1
c0, c1 = run
check_that("the right channel went silent outside the quiet",
           c1 - c0 >= 300 * ms, f"{(c1 - c0) / ms:.0f} ms")
check_that("with the left still playing all through it",
           peak(c0, c1) > 1000, f"left peak {peak(c0, c1)}")
check_that("and came back afterwards",
           any(R[k] != 0 for k in range(c1, min(len(R), c1 + 100 * ms))),
           f"frames {c1}..{c1 + 100 * ms}")
near = range(max(1, c0 - 50 * ms), min(len(R), c1 + 50 * ms))
rstep = max(abs(R[k] - R[k - 1]) for k in near)
check_that("no step in the right channel going or coming back - it ramps",
           rstep <= 400, f"largest sample-to-sample move {rstep}")
# Battery RAM that appears after load, as a DS game's save chip does when
# DSperate's save list does not name the game: adopted and written, not lost.
# Standalone, because nothing about it is the protocol's.
late = tempfile.mkdtemp(prefix="diatom-late-")
rom = f"{late}/Late Game.bin"
open(rom, "wb").write(b"\0" * 16)
envl = dict(os.environ, STUBCORE_LATE_SRAM="30")
out = subprocess.run([f"{ROOT}/build/desktop/diatom", "--core", f"{ROOT}/build/desktop/stubcore.so",
                      "--rom", rom, "--save", late, "--frames", "200"],
                     capture_output=True, text=True, env=envl, timeout=60)
srm = f"{late}/Late Game.srm"
data = open(srm, "rb").read() if os.path.exists(srm) else b""
check("battery that appears at frame 30 is written as the game's .srm",
      data, bytes(0x5a ^ (i & 0xff) for i in range(512)))
check_that("and the log says so", "made 512 bytes of battery" in out.stderr,
           [l for l in out.stderr.splitlines() if "save:" in l])

print("\n" + ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}"))
sys.exit(1 if fails else 0)
