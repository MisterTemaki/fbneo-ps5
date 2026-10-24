#!/bin/bash
# FBNeo PS5 host tests: the real port code (main-boot, shims, frontend, FBNeo's core; the installer and the helper)
# on Linux, with the PS5 calls faked by host/sce_host.cpp. A scripted pad drives the app; flips are de-tiled to PPM
# and checked. No real ROMs: tests/make_fake_set.py writes stand-in sets with the names and CRCs FBNeo expects;
# Pac-Man (vertical) and Ponpoko (horizontal, one button) get a tiny Z80 program (tests/data): a red screen, green
# while the joystick or the button is pressed.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/build/host/fbneo-ps5-host
INSTALLER=$PWD/build/host/fbneo-ps5-installer
HELPER=$PWD/build/host/fbneo-ps5-helper
HEADLESS=$PWD/build/host/fbneo_headless
# ports nobody listens on, so the app's ordinary runs find no helper and no ELF loader
export FBNEO_HELPER_PORT=$((20000 + RANDOM % 10000)) FBNEO_ELFLDR_PORT=$((30000 + RANDOM % 10000)) FBNEO_JB_NO_OTHERS=1
# the debug logs are off by default (1.9.1): the tests read boot.log, so here a settings file without the key means on
export FBNEO_LOGS_DEFAULT=1
CHECK="python3 $PWD/tests/check_ppm.py"
RECT="python3 $PWD/tests/check_rect.py"
FAKE="python3 $PWD/tests/make_fake_set.py $HEADLESS"
WORK=${WORK:-$(mktemp -d)}
ONLY=${ONLY:-}
VER=$(sed -n 's/^VERSION ?= //p' Makefile)
PASS=0
FAIL=0

# pad bits
CROSS=4000; CIRCLE=2000; TRIANGLE=1000; SQUARE=8000; UP=10; DOWN=40; RIGHT=20; LEFT=80; L2=100; R2=200; L3R3=6
L1=400; R1=800; L3=2; R3=4
OPTIONS=8; TOUCH=100000
L2UP=$(printf %x $((0x$L2 | 0x$UP))); L2DOWN=$(printf %x $((0x$L2 | 0x$DOWN))); L2R2=$(printf %x $((0x$L2 | 0x$R2)))
# pause menu: L3+R3, then Up from "Resume" wraps to "Quit FBNeo PS5"
QUITAT() { echo "$1:$L3R3;$(($1 + 2)):0;$(($1 + 10)):$UP;$(($1 + 12)):0;$(($1 + 20)):$CROSS;$(($1 + 22)):0"; }
SHELFQUIT_AT() { echo "$1:$OPTIONS;$(($1 + 2)):0;$(($1 + 10)):$CROSS;$(($1 + 12)):0"; }

ok() { echo "  ok: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }
waitfor() { for i in $(seq 1 50); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop_helpers() { pkill -f 'build/host/fbneo-ps5-(installer|helper)' 2>/dev/null; pkill -f 'received.elf' 2>/dev/null; sleep 0.3; }

# the stand-in ROM sets, made once per work dir: sets/<name>/<zips>
SETS=$WORK/sets
mkset() { # name [make_fake_set options] -> $SETS/<name>/
	local name=$1
	shift
	if [ ! -d "$SETS/$name" ]; then
		mkdir -p "$SETS/$name"
		ASAN_OPTIONS=detect_leaks=0 $FAKE "$name" "$SETS/$name" "$@" >/dev/null 2>&1 || echo "  (could not make the set $name)"
	fi
}
PAC_PUT="--put pacman.6e=tests/data/pacman-test.6e --put 82s123.7f=tests/data/pacman-test.7f --put 82s126.4a=tests/data/pacman-test.4a --put pacman.5e= --put pacman.5f="
PON_PUT="--put ppokoj1.bin=tests/data/pacman-test.6e --put 82s123.7f=tests/data/pacman-test.7f --put 82s126.4a=tests/data/pacman-test.4a --put ppoko9.bin= --put ppoko10.bin="
mkset pacman $PAC_PUT
mkset ponpoko $PON_PUT
addset() { cp "$SETS/$1"/*.zip "$2/"; } # name roms-dir

newroot() {
	local t=$WORK/$1
	rm -rf "$t" && mkdir -p "$t/root/roms" "$t/dump"
	echo "shader=0" >"$t/root/fbneo-ps5.ini" # the plain picture: the colour checks expect it (group 12 tests the shaders)
	echo "$t"
}

run() { # dir pad dumps [args...]
	local t=$1 pad=$2 dumps=$3
	shift 3
	FBNEO_HOST_OFFLINE=${OFFLINE-1} FBNEO_COVER_URL=${COVER_URL-} \
	FBNEO_PS5_ROOT=$t/root FBNEO_PS5_HOMEBREW=$t/homebrew FBNEO_HOST_DUMP_DIR=$t/dump FBNEO_HOST_DUMP=$dumps \
		FBNEO_HOST_MAX_FLIPS=${MAXFLIPS-5000} FBNEO_HOST_PAD=$pad ASAN_OPTIONS=detect_leaks=0 timeout 180 "$BIN" "$@" >"$t/out.txt" 2>&1
	echo $?
}
nosan() { expect "! grep -q 'runtime error\|AddressSanitizer' $1/out.txt" "no sanitizer reports"; }

if want 1; then
echo "== 1. a vertical game from the command line (Pac-Man): picture turned upright, input, save state from the pause menu, quit"
T=$(newroot t1)
addset pacman "$T/root/roms"
rc=$(run "$T" "0:0;100:$UP;140:0;200:$L3R3;202:0;210:$DOWN;212:0;220:$CROSS;222:0;230:$CIRCLE;232:0;$(QUITAT 240)" "90,130" "$T/root/roms/pacman.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'running pacman \"Pac-Man (Midway)\" (Pac-man), 60.000 fps (at 60 Hz, vsync), aspect 0.7500' $T/root/logs/boot.log" "FBNeo runs the set (60 Hz, a 3:4 monitor)"
expect "grep -q 'picture 224x288' $T/root/logs/boot.log" "the 288x224 screen is turned upright: 224x288"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "red picture where the log says, black around it"
expect "grep -q -- '-> [0-9]*,0 810x1080' $T/root/logs/boot.log" "a tall picture: 810x1080 in the middle of the screen"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "Up -> the game sees the joystick (green)"
expect "[ -f $T/root/states/pacman.state1 ]" "the pause menu's Save state wrote states/pacman.state1"
expect "grep -q 'save state 1: ok' $T/root/logs/boot.log" "logged the save"
expect "grep -q '\[burn\] FBNeo 1.0.0.3, [0-9]* drivers' $T/root/logs/boot.log" "the core's own log reaches boot.log"
nosan "$T"
fi

if want 2; then
echo "== 2. a horizontal game with a button (Ponpoko): Cross is button 1; save and load a state from the pause menu"
T=$(newroot t2)
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;40:$L3R3;42:0;48:$DOWN;50:0;56:$CROSS;58:0;64:$CIRCLE;66:0;100:$CROSS;140:0;160:$L3R3;162:0;168:$DOWN;170:0;176:$DOWN;178:0;184:$CROSS;186:0;$(QUITAT 210)" "90,130" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'running ponpoko \"Ponpoko\" (Pac-man), 60.000 fps (at 60 Hz, vsync), aspect 1.3333, 1 button(s)' $T/root/logs/boot.log" "a 4:3 game with one button"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "red picture where the log says"
expect "grep -q -- '-> 240,0 1440x1080' $T/root/logs/boot.log" "a 4:3 picture: 1440x1080"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "Cross -> button 1 (green)"
expect "grep -q 'load state 1: ok' $T/root/logs/boot.log" "the pause menu's Load state loaded it"
nosan "$T"
fi

if want 3; then
echo "== 3. the library: sets by name, parents and BIOS sets found, incomplete sets hidden, the check cached"
T=$(newroot t3)
addset pacman "$T/root/roms"
mkdir -p "$T/root/roms/more"
addset ponpoko "$T/root/roms/more"
mkset mslug
cp "$SETS/mslug/mslug.zip" "$T/root/roms/" # Metal Slug without neogeo.zip: incomplete
echo "not a rom set" >"$T/root/roms/notfbneo.txt"
python3 -c "import zipfile; zipfile.ZipFile('$T/root/roms/unknownset.zip','w').writestr('x.bin', b'1')"
# the shelf: Pac-Man, Ponpoko, Puck Man; Cross -> Pac-Man; back to the list; quit
rc=$(run "$T" "0:0;40:$CROSS;42:0;150:$L3R3;152:0;160:$UP;162:0;164:$UP;166:0;170:$CROSS;172:0;$(SHELFQUIT_AT 200)" "20,120")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[games\] 5 archive(s): 4 arcade set(s) (3 complete, 4 checked now), 0 BIOS set(s), 1 not FBNeo.s' $T/root/logs/boot.log" "5 zips: 4 sets, 3 complete, 1 unknown"
expect "grep -q 'mslug: [0-9]* ROM(s) missing' $T/root/logs/boot.log" "Metal Slug without the Neo Geo BIOS: incomplete"
expect "grep -q 'loading .*/roms/pacman.zip' $T/root/logs/boot.log" "Cross started the first game (Pac-Man)"
expect "$CHECK $T/dump/flip00120.ppm 960 540 red >/dev/null" "the game picked on the shelf runs (its parent puckman.zip found)"
expect "grep -q 'last_rom=.*pacman.zip' $T/root/fbneo-ps5.ini" "the shelf remembers the last game"
expect "grep -q '^mslug	' $T/root/config/romcheck.txt && grep -q '^pacman	' $T/root/config/romcheck.txt" "the check is kept in config/romcheck.txt"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 30)" "")
expect "grep -q '(3 complete, 0 checked now)' $T/root/logs/boot.log" "second start: nothing checked again"
mkset neogeo
cp "$SETS/neogeo/neogeo.zip" "$T/root/roms/more/"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 30)" "")
expect "grep -q '6 archive(s): 4 arcade set(s) (4 complete, 1 checked now), 1 BIOS set(s)' $T/root/logs/boot.log" "neogeo.zip added (another folder): Metal Slug checked again, complete; the BIOS isn't a game"
nosan "$T"
fi

if want 4; then
echo "== 4. a set with a ROM missing: the log and the screen say which; Show incomplete sets lists it"
T=$(newroot t4)
if [ ! -d "$SETS/v-ponpoko-drop" ]; then
	mkdir -p "$SETS/v-ponpoko-drop"
	ASAN_OPTIONS=detect_leaks=0 $FAKE ponpoko "$SETS/v-ponpoko-drop" --drop 2 >/dev/null 2>&1
fi
cp "$SETS/v-ponpoko-drop/ponpoko.zip" "$T/root/roms/"
rc=$(MAXFLIPS=300 run "$T" "0:0;60:$CROSS;62:0" "40" "$T/root/roms/ponpoko.zip")
expect "grep -q 'ponpoko: 1 ROM(s) missing (ppokoj3.bin), looked in ponpoko.zip' $T/root/logs/boot.log" "the missing ROM is named in the log"
expect "grep -q 'could not start ponpoko.zip' $T/out.txt && [ -f $T/dump/flip00040.ppm ]" "a notification and a message on the screen"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 30)" "")
expect "grep -q '1 arcade set(s) (0 complete' $T/root/logs/boot.log && grep -q 'ponpoko: 1 ROM(s) missing, first ppokoj3.bin' $T/root/logs/boot.log" "the shelf's check finds it incomplete"
expect "! grep -q 'loading ' $T/root/logs/boot.log" "and doesn't offer it"
echo "show_incomplete=1" >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;30:$CROSS;32:0;80:$CROSS;82:0;$(SHELFQUIT_AT 100)" "60")
expect "grep -q 'loading .*ponpoko.zip' $T/root/logs/boot.log && grep -q 'ROM(s) missing (ppokoj3.bin)' $T/root/logs/boot.log" "listed with the setting on; starting it names the ROM"
nosan "$T"
fi

if want 5; then
echo "== 5. DIP switches from the pause menu: kept per game, read back at the next start"
T=$(newroot t5)
addset pacman "$T/root/roms"
# pause menu: Resume, Save, Load, Slot, DIP switches (Down x4), Cross; first DIP (Coinage) Right; Circle; Circle (resume)
rc=$(run "$T" "0:0;60:$L3R3;62:0;70:$DOWN;72:0;76:$DOWN;78:0;82:$DOWN;84:0;88:$DOWN;90:0;96:$CROSS;98:0;110:$RIGHT;112:0;120:$CIRCLE;122:0;130:$CIRCLE;132:0;$(QUITAT 160)" "115" "$T/root/roms/pacman.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^Coinage=1C 2C$' $T/root/config/pacman.dip" "config/pacman.dip holds the changed switch (Coinage 1C 2C)"
expect "grep -q 'DIP switch 0 -> 2' $T/root/logs/boot.log" "logged"
expect "[ -f $T/dump/flip00115.ppm ]" "the DIP screen was shown"
rc=$(MAXFLIPS=100 run "$T" "0:0" "" "$T/root/roms/pacman.zip")
expect "grep -q 'DIP switches from .*pacman.dip: 1 set' $T/root/logs/boot.log" "read back at the next start"
nosan "$T"
fi

if want 6; then
echo "== 6. button layouts: custom (button 1 on Triangle), fighting (button 1 on Square), L2 / R2 as game buttons"
T=$(newroot t6)
addset ponpoko "$T/root/roms"
printf 'layout=3\nbtn_1=3\n' >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$TRIANGLE;200:0;$(QUITAT 220)" "130,190" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null" "custom: Cross no longer presses button 1"
expect "$CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "custom: Triangle presses button 1"
T=$(newroot t6b)
addset ponpoko "$T/root/roms"
printf 'layout=2\n' >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$SQUARE;200:0;$(QUITAT 220)" "130,190" "$T/root/roms/ponpoko.zip")
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "fighting layout: button 1 is Square"
# L2 and R2 as game buttons (no hot keys on them since 1.7): button 1 on R2, then on L2
T=$(newroot t6d)
addset ponpoko "$T/root/roms"
printf 'layout=3\nbtn_1=10\n' >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$R2;200:0;$(QUITAT 220)" "130,190" "$T/root/roms/ponpoko.zip")
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "custom: button 1 on R2, R2 presses it"
T=$(newroot t6e)
addset ponpoko "$T/root/roms"
printf 'layout=3\nbtn_1=9\n' >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$L2;200:0;210:$L2UP;215:0;$(QUITAT 230)" "130,190" "$T/root/roms/ponpoko.zip")
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "custom: button 1 on L2, L2 presses it"
expect "! grep -q 'save state' $T/root/logs/boot.log" "L2 + Up saves nothing (no hot key)"
# the Fighting (R1 R2) layout: button 1 on Square
T=$(newroot t6f)
addset ponpoko "$T/root/roms"
printf 'layout=4\n' >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;100:$SQUARE;140:0;$(QUITAT 160)" "130" "$T/root/roms/ponpoko.zip")
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "Fighting (R1 R2): button 1 on Square"
nosan "$T"
# the settings screen: Button 1 row Right -> the layout becomes custom
T=$(newroot t6c)
addset ponpoko "$T/root/roms"
# Shader, then Down x10 to "Button 1" (Screen size, Aspect, Smooth, Scanlines, FPS, Sound, Volume, High scores, Button layout, Button 1)
P="0:0;30:$TRIANGLE;32:0"; t=40
for i in $(seq 1 10); do P="$P;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
P="$P;$t:$RIGHT;$((t + 2)):0;$((t + 10)):$CIRCLE;$((t + 12)):0;$(SHELFQUIT_AT $((t + 30)))"
rc=$(run "$T" "$P" "")
expect "grep -q '^layout=3$' $T/root/fbneo-ps5.ini && grep -q '^btn_1=1$' $T/root/fbneo-ps5.ini && grep -q '^btn_2=1$' $T/root/fbneo-ps5.ini" "the settings screen made a custom layout from the classic one (button 1: Cross -> Circle)"
expect "[ -z \"\$(ls $T/root/config/*.btn 2>/dev/null)\" ]" "from the shelf: the general layout, no game's file"
nosan "$T"
# 2.0 (fbneo-ps5#2): Auto gives a 6-button game Capcom's PlayStation layout, Fighting (R1 R2): heavy kick on R2, not L1
T=$(newroot t6g)
mkset sf2
addset sf2 "$T/root/roms"
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/sf2.zip")
expect "grep -q 'buttons (general layout) Auto = Fighting (R1 R2): 1=Square 2=Triangle 3=R1 4=Cross 5=Circle 6=R2 coin=Touchpad start=OPTIONS' $T/root/logs/boot.log" "Auto, a 6-button game (Street Fighter II): punches Square Triangle R1, kicks Cross Circle R2"
expect "grep -q '\[emu\] running sf2 .* 6 button(s)' $T/root/logs/boot.log" "(the game has six buttons)"
printf 'layout=2\n' >>"$T/root/fbneo-ps5.ini"
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/sf2.zip")
expect "grep -q 'buttons (general layout) Fighting (6 buttons): 1=Square 2=Triangle 3=R1 4=Cross 5=Circle 6=L1 coin=Touchpad start=OPTIONS' $T/root/logs/boot.log" "Fighting (6 buttons) chosen: still there (heavy kick on L1)"
nosan "$T"
# 2.0 (fbneo-ps5#2): a button layout per game. Ponpoko: pause menu > Settings > Button 1 Right (Cross -> Circle), kept
# for Ponpoko only (config/ponpoko.btn); Pac-Man keeps the general layout; Ponpoko again: Circle presses button 1; its
# clone Ponpoko (Venture Line) without a file of its own: Ponpoko's; "Default button layout": back to the general one
T=$(newroot t6h)
addset ponpoko "$T/root/roms"
addset pacman "$T/root/roms"
if [ ! -d "$SETS/v-ponpokov" ]; then
	mkdir -p "$SETS/v-ponpokov"
	ASAN_OPTIONS=detect_leaks=0 $FAKE ponpokov "$SETS/v-ponpokov" --put ppoko1.bin=tests/data/pacman-test.6e >/dev/null 2>&1
fi
cp "$SETS/v-ponpokov/ponpokov.zip" "$T/root/roms/" # its own ROMs: the shared ones (and the colours) come from ponpoko.zip
# the pause menu (L3 + R3) from frame $1: Down x7 to Settings, Cross; then Down x$2 from Shader, then $3 (Right or Cross);
# Circle (settings), Circle (resume)
PAUSE_SETTINGS() {
	local t=$1 p="$1:$L3R3;$(($1 + 2)):0"
	t=$((t + 10))
	for i in $(seq 1 7); do p="$p;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
	p="$p;$t:$CROSS;$((t + 2)):0"; t=$((t + 12))
	for i in $(seq 1 $2); do p="$p;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
	p="$p;$t:$3;$((t + 2)):0;$((t + 10)):$CIRCLE;$((t + 12)):0;$((t + 20)):$CIRCLE;$((t + 22)):0"
	echo "$p"
}
# Ponpoko: before the change (Settings, Down x10: Button 1, Right), Cross presses button 1; after it, Circle does
rc=$(run "$T" "0:0;30:$CROSS;50:0;$(PAUSE_SETTINGS 60 10 $RIGHT);300:$CIRCLE;340:0;360:$CROSS;400:0;$(QUITAT 420)" "45,330,390" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00045.ppm 960 540 green >/dev/null" "Ponpoko, the general layout (Auto = Classic): Cross presses button 1"
expect "grep -q '^layout=3$' $T/root/config/ponpoko.btn && grep -q '^btn_1=1$' $T/root/config/ponpoko.btn && grep -q '^btn_2=1$' $T/root/config/ponpoko.btn" "changed in the game: config/ponpoko.btn holds Ponpoko's layout (custom, button 1 on Circle)"
expect "grep -q '^layout=0$' $T/root/fbneo-ps5.ini && grep -q '^btn_1=0$' $T/root/fbneo-ps5.ini" "the general layout (fbneo-ps5.ini) is unchanged"
expect "grep -q 'button layout for ponpoko only: Custom' $T/root/logs/boot.log" "logged"
expect "$CHECK $T/dump/flip00330.ppm 960 540 green >/dev/null && $CHECK $T/dump/flip00390.ppm 960 540 red >/dev/null" "at once: Circle presses button 1, Cross no longer does"
# another game: the general layout
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/pacman.zip")
expect "grep -q 'button layout: the general one' $T/root/logs/boot.log && grep -q 'buttons (general layout) Auto = Classic: 1=Cross' $T/root/logs/boot.log && [ ! -f $T/root/config/pacman.btn ]" "Pac-Man: the general layout"
# Ponpoko again: its own layout
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$CIRCLE;200:0;$(QUITAT 220)" "130,190" "$T/root/roms/ponpoko.zip")
expect "grep -q 'button layout of this game: .*/config/ponpoko.btn' $T/root/logs/boot.log && grep -q \"buttons (this game's layout) Custom: 1=Circle\" $T/root/logs/boot.log" "Ponpoko again: its own layout (logged)"
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "Ponpoko again: Cross does nothing, Circle presses button 1"
# its clone, no file of its own: Ponpoko's
rc=$(run "$T" "0:0;100:$CROSS;140:0;160:$CIRCLE;200:0;$(QUITAT 220)" "130,190" "$T/root/roms/ponpokov.zip")
expect "grep -q 'running ponpokov' $T/root/logs/boot.log && grep -q 'button layout of the parent set: .*/config/ponpoko.btn' $T/root/logs/boot.log" "the clone (Ponpoko (Venture Line)) uses its parent's file"
expect "$CHECK $T/dump/flip00130.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00190.ppm 960 540 green >/dev/null" "the clone: Circle presses button 1"
# the clone's "Default button layout" (Settings, Down x18): the general layout for it, though its parent has one
rc=$(run "$T" "0:0;$(PAUSE_SETTINGS 30 18 $CROSS);300:$CROSS;340:0;$(QUITAT 360)" "330" "$T/root/roms/ponpokov.zip")
expect "grep -q '^layout=general$' $T/root/config/ponpokov.btn && grep -q 'ponpokov: back to the general button layout' $T/root/logs/boot.log" "the clone's Default button layout: config/ponpokov.btn says general"
expect "$CHECK $T/dump/flip00330.ppm 960 540 green >/dev/null" "the clone: Cross presses button 1 again"
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/ponpokov.zip")
expect "grep -q 'button layout: the general one (.*ponpokov.btn says so)' $T/root/logs/boot.log" "and keeps it at the next start"
# Ponpoko's "Default button layout": its file removed, the general layout back
rc=$(run "$T" "0:0;$(PAUSE_SETTINGS 30 18 $CROSS);300:$CROSS;340:0;$(QUITAT 360)" "330" "$T/root/roms/ponpoko.zip")
expect "[ ! -f $T/root/config/ponpoko.btn ] && grep -q 'ponpoko: back to the general button layout (.*ponpoko.btn removed)' $T/root/logs/boot.log" "Default button layout in Ponpoko: config/ponpoko.btn removed"
expect "$CHECK $T/dump/flip00330.ppm 960 540 green >/dev/null" "Ponpoko: Cross presses button 1 again (the general layout)"
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'button layout: the general one' $T/root/logs/boot.log" "and at the next start"
# a damaged file (no valid layout= line) is ignored: the general layout; one edited by hand with spaces is read
printf 'garbage\nlayout = x\nbtn_1=1\n' >"$T/root/config/ponpoko.btn"
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'ponpoko.btn has no valid layout= line: ignored' $T/root/logs/boot.log && grep -q 'button layout: the general one' $T/root/logs/boot.log" "a damaged ponpoko.btn is ignored: the general layout"
printf 'layout = 3\nbtn_1 = 1\n' >"$T/root/config/ponpoko.btn"
rc=$(MAXFLIPS=200 run "$T" "0:0;$(QUITAT 60)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q \"buttons (this game's layout) Custom: 1=Circle\" $T/root/logs/boot.log" "a ponpoko.btn edited by hand (spaces around =) is read"
nosan "$T"
fi

if want 7; then
echo "== 7. the sound stays fed and near the 60 ms target; a 57.55 Hz board is paced by the sound"
T=$(newroot t7)
addset pacman "$T/root/roms"
rc=$(FBNEO_HOST_REALTIME=1 run "$T" "0:0;$(QUITAT 760)" "100" "$T/root/roms/pacman.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
u2=$(grep -o 'audio queued .*underruns [0-9]*' "$T/root/logs/boot.log" | sed -n 2p | grep -o '[0-9]*$')
u3=$(grep -o 'audio queued .*underruns [0-9]*' "$T/root/logs/boot.log" | sed -n 3p | grep -o '[0-9]*$')
lat=$(grep -o 'audio queued [0-9]* ([0-9]* ms)' "$T/root/logs/boot.log" | sed -n 3p | grep -o '([0-9]*' | tr -d '(')
under=$(( ${u3:-999} - ${u2:-0} ))
expect "[ $under -le 2 ]" "audio underruns over 5 s of play: $under"
expect "[ ${lat:-0} -ge 15 ] && [ ${lat:-0} -le 130 ]" "audio latency ${lat:-?} ms"
nosan "$T"
T=$(newroot t7b)
mkset ddonpach
addset ddonpach "$T/root/roms"
start=$(date +%s.%N)
rc=$(run "$T" "0:0;$(QUITAT 300)" "" "$T/root/roms/ddonpach.zip")
end=$(date +%s.%N)
secs=$(python3 -c "print(round($end - $start, 2))")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'running ddonpach .*57.550 fps (paced by the sound)' $T/root/logs/boot.log" "DoDonPachi (57.55 Hz) is paced by the sound"
expect "python3 -c 'import sys; sys.exit(0 if 5.0 <= $secs <= 10.5 else 1)'" "300 frames took ${secs}s (~5.4 s at 57.55 fps)"
nosan "$T"
fi

if want 8; then
echo "== 8. little video memory: 720p scan-out; memory busy at first: retries"
T=$(newroot t8)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_DIRECT_MAX_MIB=12 run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 200)" "90,130" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'scan-out 1280x720' $T/root/logs/boot.log" "fell back to 1280x720"
expect "head -2 $T/dump/flip00090.ppm | grep -q '1280 720'" "the flips are 1280x720"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "game picture in place (720p)"
expect "$CHECK $T/dump/flip00130.ppm 170 360 green >/dev/null && $CHECK $T/dump/flip00130.ppm 1110 360 green >/dev/null" "720p: the picture turns green to its edges"
T=$(newroot t8b)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_DIRECT_FAIL_FIRST=9 run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'retrying (1)' $T/root/logs/boot.log && grep -q 'scan-out 1920x1080' $T/root/logs/boot.log" "retried, then got 1080p"
fi

if want 9; then
echo "== 9. FBNeoPS5.elf installs the app (packed eboot.bin and backgrounds unpacked), stays as the helper, repairs"
T=$(newroot t9)
APP=$T/homebrew/PPSA99924
inst() { # dir -> runs the installer; it stays running as the helper when the port is free
	FBNEO_PS5_ROOT=$1/root FBNEO_PS5_HOMEBREW=$1/homebrew FBNEO_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
stop_helpers
inst "$T" &
expect "waitfor $T/root/logs/installer.log 'listening on 127.0.0.1'" "the installer stays running as the helper"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin" "eboot.bin installed (unpacked from the payload)"
expect "cmp -s $APP/sce_sys/pic0.dds app/sce_sys/pic0.dds && cmp -s $APP/sce_sys/pic1.dds app/sce_sys/pic1.dds" "pic0.dds/pic1.dds unpacked"
expect "cmp -s $APP/sce_sys/icon0.png app/sce_sys/icon0.png && cmp -s $APP/sce_sys/param.json app/sce_sys/param.json" "icon0.png and param.json installed"
expect "cmp -s $APP/sce_module/libc.prx tests/fake-libc.prx" "sce_module/libc.prx installed"
expect "grep -q '\"titleId\": \"PPSA99924\"' $APP/sce_sys/param.json && grep -q 'FBNeo PS5' $APP/sce_sys/param.json" "the title is PPSA99924, FBNeo PS5"
expect "grep -q 'FBNeo PS5 [0-9.]* installed. Open it from the FBNeo PS5 icon' $T/inst.txt" "install notification says to open the icon"
expect "! ls $APP/*.part $APP/sce_sys/*.part $APP/sce_module/*.part 2>/dev/null | grep -q ." "no .part files left"
inst "$T"
expect "grep -q 'is up to date' $T/root/logs/installer.log && ! grep -q 'wrote' $T/root/logs/installer.log" "sent again: nothing rewritten"
expect "grep -q 'a helper is already running' $T/root/logs/installer.log" "sent again: the second copy leaves the helper to the first"
echo broken >$APP/eboot.bin
inst "$T"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin" "a damaged eboot.bin is put back"
META=$T/appmeta/PPSA99924
mkdir -p "$META" && echo old >"$META/icon0.png" && cp app/sce_sys/param.json "$META/"
inst "$T"
expect "cmp -s $META/pic0.dds app/sce_sys/pic0.dds && cmp -s $META/icon0.png app/sce_sys/icon0.png" "appmeta gets the background and the icon"
stop_helpers
expect "[ \$(find $T -path '*PPSA99009*' -o -path '*PPSA99010*' -o -path '*PPSA99011*' -o -path '*PPSA99203*' | wc -l) = 0 ]" "nothing written for Snes9x PS5, Mesen2 PS5, Genesis Plus GX PS5 or PS5SX2"
fi

if want 10; then
echo "== 10. covers: the flyer, the parent's flyer for a clone, a screenshot when there is no flyer, 404 remembered"
T=$(newroot t10)
REPO=FBNeo_-_Arcade_Games
mkdir -p "$T/srv/$REPO/Named_Boxarts" "$T/srv/$REPO/Named_Snaps" "$T/root/covers/FBNeo"
python3 - "$T/srv/$REPO" <<'PY'
import sys
from PIL import Image
r = sys.argv[1]
Image.new('RGB', (512, 700), (255, 0, 0)).save(r + '/Named_Boxarts/Puck Man (Japan, set 1).png')
Image.new('RGB', (512, 384), (0, 255, 0)).save(r + '/Named_Snaps/Ponpoko.png')
PY
addset pacman "$T/root/roms"
addset ponpoko "$T/root/roms"
mkset 1942
addset 1942 "$T/root/roms"
PORT=18090
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
sleep 1
# the shelf: 1942 under #; Down -> P: Pac-Man, Ponpoko, Puck Man
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/\${kind}/\${name}.png" FBNEO_HOST_REALTIME=1 run "$T" \
	"0:0;200:$DOWN;202:0;250:$RIGHT;252:0;300:$RIGHT;302:0;$(SHELFQUIT_AT 340)" "190,240,290,330")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "cmp -s '$T/root/covers/FBNeo/Puck Man (Japan, set 1).png' '$T/srv/$REPO/Named_Boxarts/Puck Man (Japan, set 1).png'" "Puck Man's flyer saved in covers/FBNeo"
expect "cmp -s '$T/root/covers/FBNeo/Pac-Man (Midway).png' '$T/srv/$REPO/Named_Boxarts/Puck Man (Japan, set 1).png'" "Pac-Man (a clone with no flyer) gets its parent's"
expect "cmp -s '$T/root/covers/FBNeo/Ponpoko.png' '$T/srv/$REPO/Named_Snaps/Ponpoko.png'" "Ponpoko (no flyer) gets its screenshot"
expect "ls $T/root/covers/FBNeo/ | grep -q '^1942.*\.missing$'" "1942: nothing on the server, remembered (.missing)"
expect "$CHECK $T/dump/flip00240.ppm 960 420 255 0 0 >/dev/null" "Pac-Man shows the red flyer"
expect "$CHECK $T/dump/flip00290.ppm 960 420 0 255 0 >/dev/null" "Ponpoko shows the green screenshot"
# 2.0 (genplusgxPS5 issue #4): the card of a game with no cover (1942) shows an arcade panel, top right -- a light grey
# plate, a stick (ball top, shaft) and six round buttons --, no longer the SNES pad's four coloured buttons (red,
# yellow, green, blue: sampled where they were)
P=$T/dump/flip00190.ppm
expect "! $CHECK $P 1201 227 225 60 70 40 >/dev/null && ! $CHECK $P 1238 195 240 200 60 40 >/dev/null && ! $CHECK $P 1275 227 70 180 90 40 >/dev/null && ! $CHECK $P 1238 258 70 110 230 40 >/dev/null" "the no-cover card no longer shows the SNES pad's four coloured buttons"
expect "$CHECK $P 1124 207 36 36 44 12 >/dev/null && $CHECK $P 1124 227 60 60 72 12 >/dev/null && $CHECK $P 1182 212 52 52 64 12 >/dev/null && $CHECK $P 1266 244 52 52 64 12 >/dev/null && $CHECK $P 1238 195 198 198 208 16 >/dev/null && $CHECK $P 1238 258 198 198 208 16 >/dev/null" "it shows the arcade panel: the stick's ball and shaft, the buttons, the grey plate"
nosan "$T"
# a title of four lines starts below the panel: no letter (white) over it
T=$(newroot t10long)
mkset ssideki2
addset ssideki2 "$T/root/roms"
printf 'covers_download=0\nlast_rom=%s\n' "$T/root/roms/ssideki2.zip" >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 70)" "60")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "python3 - $T/dump/flip00060.ppm <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB')
# the card's title (Super Sidekicks 2 - The World Championship / Tokuten Ou 2 - Real Fight Football: four lines) is shown
title = sum(1 for y in range(300, 560) for x in range(600, 1320) if min(a.getpixel((x, y))) >= 225)
white = sum(1 for y in range(186, 266) for x in range(1082, 1310) if min(a.getpixel((x, y))) >= 225)
sys.exit(0 if white == 0 and title > 2000 else 1)
PY" "a long title is drawn below the arcade panel, not over it"
nosan "$T"
fi

if want 11; then
echo "== 11. the shelf's tabs: a letter at a time (Up / Down), makers with L3 / R3 (L2 / R2 up to 2.0); settings hide the clones"
T=$(newroot t11)
addset pacman "$T/root/roms"
addset ponpoko "$T/root/roms"
mkset 1942
addset 1942 "$T/root/roms"
# letters: # (1942), P (Pac-Man, Ponpoko, Puck Man); makers: All games, Capcom (1942), Classics (the rest)
rc=$(run "$T" "0:0;30:$DOWN;32:0;40:$UP;42:0;50:$R3;52:0;60:$R3;62:0;80:$CROSS;82:0;$(QUITAT 180)" "25,35,75,170")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[shelf\] tab Internal, All games, P: 3 game(s)' $T/root/logs/boot.log && grep -q '\[shelf\] tab Internal, All games, #: 1 game(s)' $T/root/logs/boot.log" "Down: the P games (3), Up: back to # (1942)"
expect "grep -q '\[shelf\] tab Internal, Capcom, #: 1 game(s)' $T/root/logs/boot.log && grep -q '\[shelf\] tab Internal, Classics, P: 3 game(s)' $T/root/logs/boot.log" "R3: Capcom (its # tab), R3: Classics (no # game there: on to P)"
expect "grep -q 'loading .*pacman.zip' $T/root/logs/boot.log" "the first game of the tab was started"
expect "grep -q 'shelf_family=11' $T/root/fbneo-ps5.ini && grep -q 'shelf_letter=P' $T/root/fbneo-ps5.ini" "the maker and the letter are remembered"
echo "show_clones=0" >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;30:$CROSS;32:0;$(QUITAT 100)" "")
expect "grep -q 'loading .*ponpoko.zip' $T/root/logs/boot.log" "clones hidden: Pac-Man is gone from P, Ponpoko comes first"
nosan "$T"
fi

if want 12; then
echo "== 12. CRT shaders: CRT Easymode style by default, every shader draws a vertical and a horizontal game"
SHADERS=("Off" "CRT Easymode style" "crt-lottes" "crt-lottes-fast" "crt-1tap" "crt-2tap" "crt-hyllian-fast" "crt-nobody" "newpixie-mini" "crt-blurPi-sharp" "crt-blurPi-soft" "monoCRT" "ScaleFX + rAA + AA style")
T=$(newroot t12)
rm -f "$T/root/fbneo-ps5.ini" # a first start: no settings file yet
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 160)" "90,130" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'shader CRT Easymode style' $T/root/logs/boot.log" "a first start draws the game through CRT Easymode style"
expect "python3 - $T/dump/flip00090.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split()); px = d[3]
R = lambda x, y: px[(y * w + x) * 3]
col = [R(960, y) for y in range(400, 460)]
row = [R(x, 540) for x in range(900, 960)]
sys.exit(0 if max(col) > 150 and min(col) < 0.9 * max(col) and len(set(row)) > 1 else 1)  # bright red: a shallow dip
PY" "scanlines and a phosphor mask on the red picture"
expect "grep -q 'shader [0-9.]* ms' $T/root/logs/boot.log" "the shader's drawing time is logged"
nosan "$T"
for s in $(seq 1 12); do
	for g in ponpoko pacman; do
		T=$(newroot t12s$s$g)
		echo "shader=$s" >>"$T/root/fbneo-ps5.ini"
		addset $g "$T/root/roms"
		rc=$(run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/$g.zip")
		expect "[ $rc = 0 ] && grep -q 'shader ${SHADERS[$s]})' $T/root/logs/boot.log && python3 -c \"import sys; d=open('$T/dump/flip00090.ppm','rb').read().split(b'\\n',3); w=int(d[1].split()[0]); p=d[3]; sys.exit(0 if max(p[(y*w+960)*3] for y in range(500,580)) > 60 else 1)\" && ! grep -q 'runtime error\|AddressSanitizer' $T/out.txt" "${SHADERS[$s]} draws $g (no sanitizer reports)"
	done
done
fi

if want 13; then
echo "== 13. settings from the shelf (Triangle); R2 and L2 + R2 do nothing (no fast forward, no rewind); the pause menu saves a state and presses Service"
T=$(newroot t13)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_REALTIME=1 run "$T" "0:0;30:$TRIANGLE;32:0;36:$DOWN;38:0;40:$RIGHT;42:0;50:$CIRCLE;52:0;70:$CROSS;72:0;150:$R2;250:0;300:$L2R2;340:0;360:$L3R3;362:0;370:$DOWN;372:0;380:$CROSS;382:0;400:$CIRCLE;402:0;420:$L3R3;422:0;428:$DOWN;430:0;434:$DOWN;436:0;440:$DOWN;442:0;446:$DOWN;448:0;452:$DOWN;454:0;460:$CROSS;462:0;$(QUITAT 500)" "45")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^scale=1$' $T/root/fbneo-ps5.ini" "the settings screen changed the screen size"
expect "! grep -qi 'fast forward' $T/root/logs/boot.log && ! grep -q '^ff_speed' $T/root/fbneo-ps5.ini" "R2 held: no fast forward (gone, setting too)"
expect "! grep -qi 'rewind' $T/root/logs/boot.log && ! grep -qi 'rewind' $T/root/fbneo-ps5.ini" "no rewind: L2 + R2 does nothing, and the setting is gone"
expect "[ -f '$T/root/states/ponpoko.state1' ]" "the pause menu's Save state wrote slot 1"
expect "grep -q 'service button (pause menu)' $T/root/logs/boot.log" "the pause menu's Service button (it replaced L2 + OPTIONS)"
nosan "$T"
fi

if want 14; then
echo "== 14. the pad is shared with the system (handle 0x809b0081, as on the console)"
T=$(newroot t14)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_PAD_SHARED=1 FBNEO_HOST_REALTIME=1 run "$T" "0:0;100:$CROSS;140:0;$(QUITAT 300)" "130" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'using 809b0081 (shared with the system)' $T/root/logs/boot.log" "the system's handle is used"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "its buttons reach the game (Cross -> green)"
fi

if want 15; then
echo "== 15. the app asks the helper to let it out of the sandbox"
stop_helpers
T=$(newroot t15)
addset ponpoko "$T/root/roms"
QUIT=$(QUITAT 30)
FBNEO_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$QUIT" "" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'FBNeo helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper said yes"
expect "grep -q 'jailbreak: FBNeo helper' $T/root/logs/boot.log" "logged before /data was open, written to boot.log"
expect "grep -q 'letting it out' $T/hroot/logs/helper.log" "the helper let the app's process out"
stop_helpers
T=$(newroot t15b)
addset ponpoko "$T/root/roms"
FBNEO_HOST_JB_TITLE=PPSA99011 FBNEO_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$QUIT" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'title PPSA99011 is not FBNeo PS5' $T/hroot/logs/helper.log" "the helper lets out no other title (not even Genesis Plus GX PS5)"
stop_helpers

echo "== 16. no helper running: the app hands its own helper to the ELF loader, then asks it"
T=$(newroot t16)
addset ponpoko "$T/root/roms"
python3 - "$FBNEO_ELFLDR_PORT" "$T" <<'PY' &
import os, socket, subprocess, sys
port, t = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(1); s.settimeout(60)
c, _ = s.accept()
data = b''
while True:
    d = c.recv(65536)
    if not d: break
    data += d
path = t + '/received.elf'
open(path, 'wb').write(data); os.chmod(path, 0o755)
env = dict(os.environ, FBNEO_PS5_ROOT=t + '/hroot', ASAN_OPTIONS='detect_leaks=0')
p = subprocess.Popen(['timeout', '30', path], env=env, stdout=open(t + '/helper.txt', 'w'), stderr=subprocess.STDOUT)
open(t + '/helper.pid', 'w').write(str(p.pid))
PY
LPID=$!
sleep 0.5
rc=$(run "$T" "$QUIT" "" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "cmp -s $T/received.elf $HELPER" "the ELF loader got the helper built into the app"
expect "grep -q 'jailbreak: FBNeo helper (started by the app)' $T/root/logs/boot.log" "then the helper it started let it out"
wait $LPID 2>/dev/null
stop_helpers
fi

if want 17; then
echo "== 17. no /data even so: the screen says what to do; the screen busy at first: retries"
T=$(newroot t17)
rm -rf "$T/root" && echo "not a folder" >"$T/root"
rc=$(run "$T" "0:0;30:$CROSS;32:0" "20")
expect "[ $rc = 2 ]" "exit code 2 (got $rc)"
expect "grep -q 'no access to /data' $T/out.txt" "logged: no access to /data"
expect "[ -f $T/dump/flip00020.ppm ]" "a message was on the screen"
T=$(newroot t17b)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_VIDEO_BUSY=3 run "$T" "$(QUITAT 30)" "" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \$(grep -c 'sceVideoOutOpen -> .*80290009' $T/root/logs/boot.log) = 3 ] && grep -q 'scan-out 1920x1080' $T/root/logs/boot.log" "VideoOut busy three times, then opened"
fi

if want 18; then
echo "== 18. covers in the background: the helper downloads them while the shelf runs; an older helper: the prefetch"
stop_helpers
T=$(newroot t18)
SRV=$T/srv/FBNeo_-_Arcade_Games/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 -c "from PIL import Image; Image.new('RGB',(512,700),(255,0,0)).save('$SRV/Ponpoko.png')"
addset ponpoko "$T/root/roms"
PORT=18091
# the server answers after 2 s, so the shelf shows the card first, then the cover
SERVER_DELAY=2 python3 tests/https_server.py "$T/srv" $PORT >/dev/null 2>&1 &
SRVPID=$!
FBNEO_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
URL="http://127.0.0.1:$PORT/\${repo}/\${kind}/\${name}.png"
start=$(date +%s)
rc=$(OFFLINE= COVER_URL="$URL" FBNEO_HOST_REALTIME=1 run "$T" "0:0;$(SHELFQUIT_AT 300)" "20,280")
secs=$(( $(date +%s) - start ))
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'the helper downloads the covers while the app runs: nothing to wait for' $T/root/logs/boot.log && ! grep -q 'restarting' $T/root/logs/boot*.log" "the app starts at once: no download before /data, no restart"
expect "grep -q '^FBNeo/Ponpoko.png	' $T/root/covers/wanted.txt" "the missing cover goes to covers/wanted.txt"
expect "grep -q 'FBNeo/Ponpoko.png -> 200' $T/root/logs/helper.log && cmp -s '$T/root/covers/FBNeo/Ponpoko.png' '$SRV/Ponpoko.png'" "the helper downloaded it into covers/FBNeo"
expect "! $CHECK $T/dump/flip00020.ppm 960 420 255 0 0 >/dev/null 2>&1 && $CHECK $T/dump/flip00280.ppm 960 420 255 0 0 >/dev/null" "the shelf showed its card, then the cover once it landed"
expect "grep -q '^0 1 idle' $T/root/covers/progress.txt" "covers/progress.txt: nothing left, 1 fetched"
kill $SRVPID 2>/dev/null
stop_helpers
# an older helper (1.0's protocol: the list without "covers: background"): the prefetch before /data, as before
T=$(newroot t18b)
mkdir -p "$T/srv/FBNeo_-_Arcade_Games/Named_Boxarts" "$T/root/covers"
cp "$SRV/Ponpoko.png" "$T/srv/FBNeo_-_Arcade_Games/Named_Boxarts/"
addset ponpoko "$T/root/roms"
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
printf 'FBNeo/Ponpoko.png\thttp://127.0.0.1:%s/FBNeo_-_Arcade_Games/Named_Boxarts/Ponpoko.png\n' $PORT >"$T/wanted.txt"
python3 - "$FBNEO_HELPER_PORT" "$T/wanted.txt" <<'PY' &
import socket, struct, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', int(sys.argv[1]))); s.listen(4); s.settimeout(60)
text = open(sys.argv[2], 'rb').read()
end = time.time() + 60
while time.time() < end:
    try:
        c, _ = s.accept()
    except OSError:
        break
    req = b''
    while len(req) < 0xA10:
        d = c.recv(0xA10 - len(req))
        if not d: break
        req += d
    if len(req) == 0xA10:
        magic, cmd, pid, ret = struct.unpack_from('<IiiI', req, 0)
        out = bytearray(req)
        if cmd == 6:
            struct.pack_into('<i', out, 12, len(text)); c.sendall(bytes(out) + text)
        elif cmd == 5:
            struct.pack_into('<i', out, 12, 0); out[16:18] = b'ok'; c.sendall(bytes(out))
    c.close()
PY
OLDPID=$!
sleep 0.5
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/\${kind}/\${name}.png" run "$T" "0:0;$(SHELFQUIT_AT 30)" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[prefetch\] 1 of 1 fetched' $T/root/logs/boot.log && cmp -s '$T/root/covers/FBNeo/Ponpoko.png' '$SRV/Ponpoko.png'" "an older helper: the cover is prefetched before /data and saved"
kill $OLDPID $SRVPID 2>/dev/null
stop_helpers
fi

if want 19; then
echo "== 19. the helper: unknown titles refused, a slow client can't block it, wanted.txt must be a plain file"
stop_helpers
T=$(newroot t19)
addset ponpoko "$T/root/roms"
FBNEO_HOST_JB_TITLE= FBNEO_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'title unknown: not FBNeo PS5' $T/hroot/logs/helper.log && ! grep -q 'letting it out' $T/hroot/logs/helper.log" "a process of unknown title is refused (fail closed)"
stop_helpers
T=$(newroot t19b)
addset ponpoko "$T/root/roms"
FBNEO_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
python3 - "$FBNEO_HELPER_PORT" <<'PY' &
import socket, sys, time
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
try:
    for i in range(20):
        s.send(b'x'); time.sleep(1)
except OSError:
    pass
PY
DRIP=$!
sleep 0.5
start=$(date +%s)
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/ponpoko.zip")
secs=$(( $(date +%s) - start ))
expect "grep -q 'FBNeo helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "with a slow client connected, the app is still let out"
expect "[ $secs -lt 20 ]" "and without waiting for the slow client (${secs}s)"
kill $DRIP 2>/dev/null
stop_helpers
T=$(newroot t19c)
mkdir -p "$T/hroot/covers"
echo "secret" >"$T/secret.txt"
ln -s "$T/secret.txt" "$T/hroot/covers/wanted.txt"
FBNEO_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/hroot/logs/helper.log" "listening" || sleep 1
python3 - "$FBNEO_HELPER_PORT" "$T/answer.bin" <<'PY'
import socket, struct, sys
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
req = bytearray(0xA10)
struct.pack_into('<IiI', req, 0, 0xDEADBEEF, 6, 1234)
s.sendall(req)
data = b''
while True:
    d = s.recv(65536)
    if not d: break
    data += d
open(sys.argv[2], 'wb').write(data)
PY
expect "grep -q 'wanted-covers list, 0 bytes' $T/hroot/logs/helper.log && ! grep -q secret $T/answer.bin" "a wanted.txt that is a symbolic link is not followed"
stop_helpers
fi

if want 20; then
echo "== 20. debug logs off: nothing written (app and helper), earlier logs kept; the setting switches them at once"
T=$(newroot t20)
addset ponpoko "$T/root/roms"
echo "debug_logs=0" >>"$T/root/fbneo-ps5.ini"
mkdir -p "$T/root/logs" && echo "OLD RUN" >"$T/root/logs/boot.log"
rc=$(run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "$CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "the game runs"
expect "[ \"\$(cat $T/root/logs/boot.log)\" = 'OLD RUN' ] && [ ! -e $T/root/logs/boot.prev.log ]" "the earlier boot.log is kept as it was, nothing new written"
expect "! grep -q '\[burn\]' $T/out.txt" "nothing on stdout either (not even the core's lines)"
nosan "$T"
# the default (1.9.1): no debug_logs key and no test override -> nothing written (app and helper), the settings file says 0
T=$(newroot t20z)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_LOGS_DEFAULT= run "$T" "0:0;$(QUITAT 100)" "90" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ] && $CHECK $T/dump/flip00090.ppm 960 540 red >/dev/null" "no setting: the game runs"
expect "[ ! -e $T/root/logs/boot.log ] && ! grep -q '\[burn\]\|\[boot\]' $T/out.txt" "debug logs off by default: no boot.log, nothing on stdout"
expect "grep -q '^debug_logs=0' $T/root/fbneo-ps5.ini" "the default settings file has debug_logs=0"
nosan "$T"
stop_helpers
T=$(newroot t20y)
addset ponpoko "$T/root/roms"
mkdir -p "$T/hroot2"
FBNEO_LOGS_DEFAULT= FBNEO_PS5_ROOT=$T/hroot2 ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper0.txt" 2>&1 &
sleep 1.5
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'FBNeo helper (port [0-9]*): ret 0' $T/root/logs/boot.log && [ ! -e $T/hroot2/logs/helper.log ] && ! grep -q 'helper\]' $T/helper0.txt" "no setting: the helper lets the app out and writes no log either"
stop_helpers
# Settings (Triangle) -> Debug logs (Up three times from Shader: Back, Updates, then Debug logs) -> Off
T=$(newroot t20b)
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;65:$UP;67:0;70:$CROSS;72:0;90:$CIRCLE;92:0;$(SHELFQUIT_AT 120)" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^debug_logs=0' $T/root/fbneo-ps5.ini" "Debug logs Off saved"
expect "tail -1 $T/root/logs/boot.log | grep -q 'debug logs turned off'" "the last line of boot.log says the logs were turned off"
nosan "$T"
stop_helpers
T=$(newroot t20d)
addset ponpoko "$T/root/roms"
mkdir -p "$T/hroot" && echo "debug_logs=0" >"$T/hroot/fbneo-ps5.ini"
FBNEO_PS5_ROOT=$T/hroot ASAN_OPTIONS=detect_leaks=0 timeout 60 "$HELPER" >"$T/helper.txt" 2>&1 &
sleep 1.5
rc=$(run "$T" "$(QUITAT 30)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'FBNeo helper (port [0-9]*): ret 0' $T/root/logs/boot.log" "the helper still lets the app out"
expect "[ ! -e $T/hroot/logs/helper.log ] && ! grep -q 'helper\]' $T/helper.txt" "with debug logs off the helper writes no log"
stop_helpers
fi

if want 21; then
echo "== 21. the core across the boards: start, 120 frames, save a state, load it back (same picture), exit -- under ASan"
# one set per kind of hardware, stand-in ROMs (the CPUs run whatever the pattern decodes to). Left out: the drivers
# that garbage code upsets on its own (Psikyo SH-2: daraku jumps to an address with no handler; 1942: its state
# misses something only garbage code touches) -- with real ROMs they run as in FBNeo.
GAMES="sf2 sfa3 sfiii mslug kof98 aof dkong galaga ddonpach batsugun altbeast outrun tmnt ssriders raiden2 gunbird
	dariusg orlegend mk nbajam rtype bublbobl gaiden ninjakd2 bombjack pbobblen"
for g in $GAMES; do
	mkset $g
	o=$("$HEADLESS" run "$SETS/$g" $g 120 --state 2>"$WORK/h21-$g.err"); rc=$?
	expect "[ $rc = 0 ] && echo '$o' | grep -q 'state replay same' && ! grep -q 'AddressSanitizer\|runtime error' $WORK/h21-$g.err" "$g: $(echo "$o" | head -1 | cut -c1-60)"
done
# Cabal and Cobra Command build their palette only when the frontend asks (FBNeo's frontends do, after starting a driver): without
# that its screen stayed black. RAM filled with 0x5a, one frame: the picture has colours.
for g in cabal cobracom; do
	mkset $g
	o=$("$HEADLESS" palette "$SETS/$g" $g 2>"$WORK/h21-$g.err"); rc=$?
	lit=$(echo "$o" | sed -n 's/.* lit \([0-9]*\).*/\1/p')
	expect "[ $rc = 0 ] && [ \"\${lit:-0}\" -gt 0 ]" "$g: the palette is made at start (${lit:-0} pixels lit, 0 = black screen)"
done
# CPS-3 (the heaviest state): a frame and a save state, timed
mkset sfiii3
o=$("$HEADLESS" bench "$SETS/sfiii3" sfiii3 60 2>"$WORK/h21-bench.err"); rc=$?
expect "[ $rc = 0 ] && echo \"$o\" | grep -q 'save state'" "sfiii3: $(echo "$o" | tail -1)"
# the DIP switches are in the driver before it starts (the Neo Geo loads the BIOS they name as it starts)
for g in mslug cabal; do
	mkset $g
	o=$("$HEADLESS" dipstart "$SETS/$g" $g 2>"$WORK/h21-dip-$g.err"); rc=$?
	expect "[ $rc = 0 ] && echo '$o' | grep -q 'dips at start: defaults'" "$g: $(echo "$o" | tail -1)"
done
# every arcade driver's sets are listed, and the console drivers aren't
n=$("$HEADLESS" list 2>/dev/null | wc -l)
expect "[ $n -gt 8000 ]" "$n arcade drivers"
expect "! $HEADLESS list 2>/dev/null | cut -f6 | grep -qx 'Neo Geo CD\|Sega Mega Drive\|NES\|SNES'" "no console driver among them"
fi

if want 22; then
echo "== 22. NVRAM, hiscores and the version: written when they change, read at the next start"
T=$(newroot t22)
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;$(QUITAT 100)" "" "$T/root/roms/ponpoko.zip")
expect "grep -q 'FBNeo PS5 $VER, FBNeo 1.0.0.3' $T/root/logs/boot.log" "boot.log names the versions ($VER, FBNeo 1.0.0.3)"
expect "! ls $T/root/saves/*.part 2>/dev/null | grep -q ." "no .part files left in saves/"
mkset mk
rm -f /tmp/fbneo-headless/mk.nvram
o=$(TMPDIR=$WORK "$HEADLESS" nvram "$SETS/mk" mk 2>"$WORK/h22.err"); rc=$?
expect "[ $rc = 0 ] && echo '$o' | grep -q 'read back: same'" "Mortal Kombat's NVRAM: written when it changed, read back at the next start ($(echo $o | tr '\n' ' '))"
expect "[ -s $WORK/fbneo-headless/mk.nvram ] && ! ls $WORK/fbneo-headless/*.part 2>/dev/null | grep -q ." "saves/mk.nvram written (no .part left)"
expect "! grep -q 'AddressSanitizer' $WORK/h22.err" "no sanitizer reports"
fi

if want 23; then
echo "== 23. the audit's fixes: merged sets, clones of a hidden parent, the check's version, DIP names, oversized ROMs"
T=$(newroot t23)
# a merged set: Pac-Man's ROMs inside puckman.zip, no pacman.zip
python3 - "$SETS/pacman/pacman.zip" "$SETS/pacman/puckman.zip" "$T/root/roms/puckman.zip" <<'PY'
import sys, zipfile
names = set()
with zipfile.ZipFile(sys.argv[3], "w") as out:
    for src in sys.argv[1:3]:
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                if n not in names:
                    names.add(n)
                    out.writestr(n, z.read(n))
PY
rc=$(run "$T" "0:0;40:$CROSS;42:0;$(QUITAT 150)" "120")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[games\] 1 archive(s): 2 arcade set(s) (2 complete, 1 checked now), 0 BIOS set(s), 0 not FBNeo.s; 1 clone(s) in a parent.s archive' $T/root/logs/boot.log" "Pac-Man found inside puckman.zip (a merged set); clones that only match by name aren't listed"
expect "grep -q 'loading .*/roms/puckman.zip#pacman' $T/root/logs/boot.log" "Cross started the clone from its parent's zip"
expect "$CHECK $T/dump/flip00120.ppm 960 540 red >/dev/null" "the merged clone runs (its own program, red)"
expect "head -1 $T/root/config/romcheck.txt | grep -q '^# FBNeo 1.0.0.3, [0-9]* drivers$'" "romcheck.txt names the FBNeo that checked the sets"
sed -i '1s/.*/# FBNeo 0.9, 1 drivers/' "$T/root/config/romcheck.txt"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 30)" "")
expect "grep -q 'romcheck.txt is from another FBNeo' $T/root/logs/boot.log && grep -q '(2 complete, 1 checked now)' $T/root/logs/boot.log" "a check made by another FBNeo is done again"
# clones hidden, but the parent is incomplete: the clone still shows
T=$(newroot t23b)
cp "$SETS/pacman/pacman.zip" "$T/root/roms/"
python3 - "$SETS/pacman/puckman.zip" "$T/root/roms/puckman.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z, zipfile.ZipFile(sys.argv[2], "w") as out:
    for n in z.namelist():
        if n != "pm1_prg8.6p":
            out.writestr(n, z.read(n))
PY
echo "show_clones=0" >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;30:$CROSS;32:0;$(QUITAT 100)" "")
expect "grep -q 'puckman: 1 ROM(s) missing' $T/root/logs/boot.log && grep -q 'loading .*/roms/pacman.zip' $T/root/logs/boot.log" "Puck Man incomplete: Pac-Man shows (and starts) with clones hidden"
# a ROM bigger than the set says (a bad dump or an overdump): read up to the size FBNeo wants
T=$(newroot t23c)
python3 - "$SETS/pacman/pacman.zip" "$T/root/roms/pacman.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z, zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED) as out:
    for n in z.namelist():
        d = z.read(n)
        out.writestr(n, d + bytes(len(d)) if n == "pacman.6e" else d)
PY
cp "$SETS/pacman/puckman.zip" "$T/root/roms/"
rc=$(run "$T" "0:0;$(QUITAT 120)" "100" "$T/root/roms/pacman.zip")
expect "[ $rc = 0 ] && $CHECK $T/dump/flip00100.ppm 960 540 red >/dev/null" "a doubled pacman.6e: the first 4 KB used, the game runs"
nosan "$T"
# DIP switches: Acrobatic Dog-Fight has two groups called "Unused"; each keeps its own setting
mkset dogfgt
o=$(TMPDIR=$WORK "$HEADLESS" dips "$SETS/dogfgt" dogfgt 2>"$WORK/h23.err"); rc=$?
expect "[ $rc = 0 ] && echo '$o' | grep -q '1 repeated name(s), .* read back: same'" "repeated DIP names saved and read back apart ($(echo "$o" | tail -1))"
expect "grep -q '^Unused=' $WORK/fbneo-headless/dogfgt.dip" "the .dip file keeps the first group's plain name (older files still load)"
expect "! grep -q 'AddressSanitizer\|runtime error' $WORK/h23.err" "no sanitizer reports"
fi

if want 24; then
echo "== 24. Settings, BIOS sets: which BIOS sets your games need, and whether each is there and complete"
T=$(newroot t24)
addset pacman "$T/root/roms"
mkset mslug
cp "$SETS/mslug/mslug.zip" "$T/root/roms/" # Metal Slug without neogeo.zip
# Triangle -> Settings; Up x6 from Shader: Back, Updates, Debug logs, Language, Network shares (2.0), BIOS sets; Cross -> the
# BIOS screen; Circle x2; quit
PAD="0:0;30:$TRIANGLE;32:0;40:$UP;42:0;48:$UP;50:0;56:$UP;58:0;64:$UP;66:0;68:$UP;70:0;72:$UP;73:0;76:$CROSS;78:0;100:$CIRCLE;102:0;110:$CIRCLE;112:0;$(SHELFQUIT_AT 130)"
rc=$(run "$T" "$PAD" "90")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[bios\] neogeo.zip (Neo Geo): not found; needed by 1 of your games' $T/root/logs/boot.log" "neogeo.zip missing, needed by Metal Slug"
expect "grep -q '\[bios\] pgm.zip (.*): not found; needed by 0 of your games' $T/root/logs/boot.log" "the other BIOS sets are listed too (PGM: none of your games)"
expect "grep -q '\[bios\] screen: [0-9]* BIOS set(s)' $T/root/logs/boot.log" "Settings, BIOS sets opened the BIOS screen"
expect "[ -f $T/dump/flip00090.ppm ]" "the BIOS screen was drawn"
mkset neogeo
cp "$SETS/neogeo/neogeo.zip" "$T/root/roms/"
rc=$(run "$T" "$PAD" "")
expect "grep -q '\[bios\] neogeo.zip (Neo Geo): OK; needed by 1 of your games' $T/root/logs/boot.log" "neogeo.zip added: OK"
python3 - "$SETS/neogeo/neogeo.zip" "$T/root/roms/neogeo.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z, zipfile.ZipFile(sys.argv[2], "w") as out:
    for n in z.namelist():
        if n != "sfix.sfix":
            out.writestr(n, z.read(n))
PY
rc=$(run "$T" "$PAD" "")
expect "grep -q '\[bios\] neogeo.zip (Neo Geo): incomplete, 1 ROM(s) missing, first sfix.sfix' $T/root/logs/boot.log" "a neogeo.zip without sfix.sfix: incomplete, the ROM named"
nosan "$T"
fi

if want 25; then
echo "== 25. the helper's own HTTPS (Mbed TLS): certificate checked, kept connection, chunks, redirects, 404"
stop_helpers25() { pkill -f 'build/host/fbneo-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
stop_helpers25
C=$WORK/t25-certs; rm -rf "$C"; mkdir -p "$C"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/ca.key" -out "$C/ca.pem" -days 30 -subj "/CN=FBNeo Test CA" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/srv.key" -out "$C/srv.csr" -subj "/CN=localhost" 2>/dev/null
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' >"$C/ext.cnf"
openssl x509 -req -in "$C/srv.csr" -CA "$C/ca.pem" -CAkey "$C/ca.key" -CAcreateserial -out "$C/srv.pem" -days 30 -extfile "$C/ext.cnf" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$C/other.key" -out "$C/other.pem" -days 30 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" 2>/dev/null
SRVROOT=$WORK/t25-srv; rm -rf "$SRVROOT"; mkdir -p "$SRVROOT/Repo/Named_Boxarts"
python3 -c "
from PIL import Image
for n,c in [('A b',(255,0,0)),('B',(0,255,0)),('C',(0,0,255))]: Image.new('RGB',(512,357),c).save('$SRVROOT/Repo/Named_Boxarts/'+n+'.png')"
PORT=18443
tls_case() { # name cert key mode host -> $T with the helper's run in it
	T=$(newroot "t25-$1")
	mkdir -p "$T/root/covers"
	python3 tests/https_server.py "$SRVROOT" $PORT "$2" "$3" "$4" 2>"$T/server.txt" &
	local sp=$!
	sleep 0.6
	local u="https://$5:$PORT"
	printf 'FBNeo/A.png\t%s/Repo/Named_Boxarts/A%%20b.png\nFBNeo/B.png\t%s/chunked/Repo/Named_Boxarts/B.png\nFBNeo/C.png\t%s/moved/Repo/Named_Boxarts/C.png\nFBNeo/D.png\t%s/Repo/Named_Boxarts/D.png\n' "$u" "$u" "$u" "$u" >"$T/root/covers/wanted.txt"
	FBNEO_EXTRA_CA=$C/ca.pem FBNEO_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout ${6:-6} "$HELPER" >"$T/helper.txt" 2>&1
	kill $sp 2>/dev/null; wait $sp 2>/dev/null
	stop_helpers25
}
tls_case good "$C/srv.pem" "$C/srv.key" keep localhost
H="$T/root/logs/helper.log"; D="$T/root/covers/FBNeo"; S="$SRVROOT/Repo/Named_Boxarts"
expect "grep -q '\[https\] Mbed TLS 3\.6\.[0-9]*: [0-9]* CA certificate(s), + the extra CA' $H" "Mbed TLS set up with Mozilla's CA list (+ the test CA)"
expect "cmp -s '$D/A.png' '$S/A b.png'" "a cover over HTTPS (Content-Length)"
expect "cmp -s '$D/B.png' '$S/B.png'" "a chunked answer"
expect "cmp -s '$D/C.png' '$S/C.png' && grep -q 'GET /moved/' $T/server.txt" "a redirect followed"
expect "[ -f '$D/D.missing' ] && [ ! -f '$D/D.png' ]" "a 404: marked missing"
expect "[ \$(grep -c 'kept connection' $H) = 3 ]" "one connection for the four requests"
expect "! grep -q '\[http\] ' $H" "the helper doesn't use libSceHttp2"
expect "! grep -q 'runtime error\|AddressSanitizer' $T/helper.txt" "no sanitizer reports"
tls_case untrusted "$C/other.pem" "$C/other.key" keep localhost 4
H="$T/root/logs/helper.log"; D="$T/root/covers/FBNeo"
expect "grep -q 'Certificate verification failed' $H && [ -z \"\$(ls $D 2>/dev/null)\" ]" "a certificate from an unknown CA: refused, nothing saved, nothing marked missing"
tls_case wrongname "$C/srv.pem" "$C/srv.key" keep 127.0.0.1 4
H="$T/root/logs/helper.log"; D="$T/root/covers/FBNeo"
expect "grep -q 'Certificate verification failed' $H && [ -z \"\$(ls $D 2>/dev/null)\" ]" "a certificate for another name: refused"
tls_case closing "$C/srv.pem" "$C/srv.key" drop localhost
D="$T/root/covers/FBNeo"
expect "[ -f '$D/A.png' ] && [ -f '$D/B.png' ] && [ -f '$D/C.png' ]" "a server that closes kept connections: a new one each time"
fi

if want 26; then
echo "== 26. the new title ID (PPSA99924): nothing under the old one (PPSA99012) is touched; our old version is named"
NEWID=PPSA99924; OLDID=PPSA99012
OURS="UP9000-${OLDID}_00-FBNEOPS500000001"; OTHER="UP9000-${OLDID}_00-ANOTHERAPP000001"
stop26() { pkill -f 'build/host/fbneo-ps5-(installer|helper)' 2>/dev/null; sleep 0.3; }
waitfor26() { for i in $(seq 1 80); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
oldapp() { # dir content-id -> an app folder under the old ID: ours (as 1.8 left it) or another app's
	mkdir -p "$1/homebrew/$OLDID/sce_sys" "$1/homebrew/$OLDID/sce_module"
	sed "s/$NEWID/$OLDID/g; s/$OURS/$2/" app/sce_sys/param.json >"$1/homebrew/$OLDID/sce_sys/param.json"
	echo old-eboot >"$1/homebrew/$OLDID/eboot.bin"; echo old-icon >"$1/homebrew/$OLDID/sce_sys/icon0.png"
}
snap() { (cd "$1/homebrew/$OLDID" && find . -type f -exec md5sum {} + | sort); }
inst26() {
	FBNEO_PS5_ROOT=$1/root FBNEO_PS5_HOMEBREW=$1/homebrew FBNEO_PS5_APPMETA=$1/appmeta ASAN_OPTIONS=detect_leaks=0 timeout 60 "$INSTALLER" >>"$1/inst.txt" 2>&1
}
stop26
# a: our old version under the old ID: left exactly as it is, the new one installed beside it, the notification names it
T=$(newroot t26a); oldapp "$T" "$OURS"; mkdir -p "$T/appmeta/$OLDID"; echo old >"$T/appmeta/$OLDID/icon0.png"
snap "$T" >"$T/before.txt"
inst26 "$T" &
expect "waitfor26 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed, then stays as the helper"
expect "cmp -s $T/homebrew/$NEWID/eboot.bin tests/fake-eboot.bin && grep -q '\"titleId\": \"$NEWID\"' $T/homebrew/$NEWID/sce_sys/param.json" "the app installed as $NEWID"
expect "snap $T | cmp -s - $T/before.txt && grep -q old $T/appmeta/$OLDID/icon0.png" "the old folder $OLDID and its home-screen art: untouched"
expect "grep -q 'Its title ID is now $NEWID. The old version ($OLDID) is left alone: to remove it, delete its folder /data/homebrew/$OLDID' $T/inst.txt" "the notification names the old version and how to delete it"
stop26
# b: another app's folder under the old ID: untouched, and not named
T=$(newroot t26b); oldapp "$T" "$OTHER"; snap "$T" >"$T/before.txt"
inst26 "$T" &
expect "waitfor26 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "snap $T | cmp -s - $T/before.txt && grep -q 'another app.s folder, left as it is' $T/root/logs/installer.log" "another app's $OLDID folder: untouched"
expect "! grep -q 'title ID is now' $T/inst.txt && cmp -s $T/homebrew/$NEWID/eboot.bin tests/fake-eboot.bin" "nothing said about it; ours installed as $NEWID"
stop26
# c: nothing under the old ID: a plain install
T=$(newroot t26c)
inst26 "$T" &
expect "waitfor26 $T/root/logs/installer.log 'listening on 127.0.0.1'" "installed"
expect "[ ! -e $T/homebrew/$OLDID ] && ! grep -q 'title ID is now' $T/inst.txt" "no old version: nothing made or said under $OLDID"
expect "! grep -q 'runtime error\|AddressSanitizer' $WORK/t26a/inst.txt $WORK/t26b/inst.txt $T/inst.txt" "no sanitizer reports"
stop26
fi

if want 27; then
echo "== 27. updates from GitHub's releases (Settings, SYSTEM, Updates): the helper checks, downloads, checks every file, installs"
T=$(newroot t27)
UPD="python3 tests/update_client.py $FBNEO_HELPER_PORT"
stop27() { pkill -f 'build/host/fbneo-ps5-(installer|helper)' 2>/dev/null; pkill -f 'http.server 1[89][0-9][0-9][0-9]' 2>/dev/null; sleep 0.3; }
waitfor27() { for i in $(seq 1 80); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
stop27
SRV=$T/srv; mkdir -p "$SRV" "$T/appmeta/PPSA99924"; echo old >"$T/appmeta/PPSA99924/icon0.png"
SPORT=$((18000 + RANDOM % 1900))
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the release: the installed app with a new eboot, a newer param.json, a new file; the old manifest lists a file that goes
release() { # tag contentVersion [bad]
	rm -rf "$T/rel" && mkdir -p "$T/rel" && cp -r "$T/homebrew/PPSA99924" "$T/rel/PPSA99924" && rm -f "$T/rel/PPSA99924/manifest.sha256" "$T/rel/PPSA99924/sce_sys/old.bin"
	echo "eboot $1" >"$T/rel/PPSA99924/eboot.bin"; echo "new art" >"$T/rel/PPSA99924/sce_sys/icon0.png"; echo extra >"$T/rel/PPSA99924/sce_sys/extra.bin"
	sed -i "s/\"contentVersion\": \"[0-9.]*\"/\"contentVersion\": \"$2\"/" "$T/rel/PPSA99924/sce_sys/param.json"
	python3 tools/make_release_zip.py "$T/rel/PPSA99924" PPSA99924 "$SRV/FBNeoPS5-$1-PPSA99924.zip" >/dev/null
	local sha size
	sha=$(sha256sum "$SRV/FBNeoPS5-$1-PPSA99924.zip" | cut -c1-64); size=$(stat -c %s "$SRV/FBNeoPS5-$1-PPSA99924.zip")
	[ -n "${3:-}" ] && sha=0000${sha:4}
	printf '{"tag_name":"%s","name":"Genesis Plus GX PS5 %s","assets":[{"name":"FBNeoPS5-%s.elf","size":1,"browser_download_url":"http://127.0.0.1:%s/x.elf"},{"name":"FBNeoPS5-%s-PPSA99924.zip","size":%s,"digest":"sha256:%s","browser_download_url":"http://127.0.0.1:%s/FBNeoPS5-%s-PPSA99924.zip"}]}' \
		"$1" "$1" "$1" $SPORT "$1" $size $sha $SPORT "$1" >"$SRV/latest.json"
}
# the app installed by the payload (1.9), which then stays as the helper; the updater asks the test server
FBNEO_UPDATE_API=http://127.0.0.1:$SPORT/latest.json FBNEO_PS5_ROOT=$T/root FBNEO_PS5_HOMEBREW=$T/homebrew FBNEO_PS5_APPMETA=$T/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst.txt" 2>&1 &
expect "waitfor27 $T/root/logs/installer.log 'listening on 127.0.0.1'" "the payload installed 1.9 and serves as the helper"
expect "[ \"\$($UPD status 1.9 | cut -d'|' -f1)\" = idle ]" "the updater answers: idle"
release v1.9 01.090.000
expect "$UPD check 1.9 --until uptodate,failed | grep -q '^uptodate|1.9|'" "the newest release is this version: up to date"
echo gone >"$T/homebrew/PPSA99924/sce_sys/old.bin"
printf '%s *sce_sys/old.bin\n' "$(sha256sum <"$T/homebrew/PPSA99924/sce_sys/old.bin" | cut -c1-64)" >"$T/homebrew/PPSA99924/manifest.sha256"
release v9.9 99.000.000 bad
expect "$UPD check 1.9 --until available,failed | grep -q '^available|9.9|'" "a newer release (9.9): available, with its size"
cp "$T/homebrew/PPSA99924/eboot.bin" "$T/eboot.before"
expect "$UPD install 1.9 --until done,failed | grep -q \"^failed|.*SHA-256 doesn't match\"" "a download whose SHA-256 isn't GitHub's: refused"
expect "cmp -s $T/homebrew/PPSA99924/eboot.bin $T/eboot.before && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "and nothing was changed"
release v9.9 99.000.000
expect "$UPD check 1.9 --until available,failed | grep -q '^available|9.9|'" "checked again"
expect "$UPD install 1.9 --until done,failed | grep -q '^done|9.9|'" "downloaded, every file checked, installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99924/eboot.bin && grep -q '\"contentVersion\": \"99.000.000\"' $T/homebrew/PPSA99924/sce_sys/param.json" "the new eboot.bin and param.json in place"
expect "[ -f $T/homebrew/PPSA99924/sce_sys/extra.bin ] && [ ! -e $T/homebrew/PPSA99924/sce_sys/old.bin ] && grep -q 'sce_sys/extra.bin' $T/homebrew/PPSA99924/manifest.sha256" "a new file added, one the old manifest had removed, the manifest kept"
expect "grep -q 'new art' $T/appmeta/PPSA99924/icon0.png && [ -z \"\$(find $T/homebrew -name '*.new')\" ]" "the home screen's copy of the art refreshed; no .new files left"
expect "$UPD status 9.9 | grep -q '^done|'" "the result stays until the app is opened again"
stop27
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
# the old payload sent again (an autoload): the newer version is left alone
FBNEO_UPDATE_API=http://127.0.0.1:$SPORT/latest.json FBNEO_PS5_ROOT=$T/root FBNEO_PS5_HOMEBREW=$T/homebrew FBNEO_PS5_APPMETA=$T/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T/inst2.txt" 2>&1 &
expect "waitfor27 $T/inst2.txt 'a newer version is installed'" "an older payload sent again: says a newer version is installed"
expect "grep -q 'eboot v9.9' $T/homebrew/PPSA99924/eboot.bin && grep -q 'new art' $T/appmeta/PPSA99924/icon0.png" "and leaves the update (and its art) as they are"
expect "$UPD check 9.9 --until uptodate,available,failed | grep -q '^uptodate|9.9|'" "the app says which version it is: an older helper finds it up to date"
stop27
# the app: at start the daily check (the helper finds 9.9); Settings (Triangle), Up twice to "Updates" (it shows
# "Install 9.9"), Cross: the helper installs it
T2=$(newroot t27b)
(cd "$SRV" && exec python3 -m http.server $SPORT --bind 127.0.0.1 >/dev/null 2>&1) &
FBNEO_UPDATE_API=http://127.0.0.1:$SPORT/latest.json FBNEO_PS5_ROOT=$T2/root FBNEO_PS5_HOMEBREW=$T2/homebrew FBNEO_PS5_APPMETA=$T2/appmeta \
	ASAN_OPTIONS=detect_leaks=0 timeout 120 "$INSTALLER" >"$T2/inst.txt" 2>&1 &
waitfor27 $T2/root/logs/installer.log 'listening on 127.0.0.1'
addset ponpoko "$T2/root/roms"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_PS5_APPMETA=$T2/appmeta run "$T2" "0:0;30:$TRIANGLE;32:0;40:$UP;42:0;46:$UP;48:0;56:$CROSS;58:0;150:$CIRCLE;152:0;$(SHELFQUIT_AT 170)" "54,140")
expect "grep -q 'daily check asked of the helper' $T2/root/logs/boot.log && [ -f $T2/root/update-check.txt ]" "at start: the daily check asked of the helper"
expect "grep -q 'update install' $T2/root/logs/installer.log && grep -q 'eboot v9.9' $T2/homebrew/PPSA99924/eboot.bin" "Cross on \"Install 9.9\": the helper installs it"
expect "grep -q 'is available: Settings, SYSTEM, Updates' $T2/inst.txt $T2/root/logs/installer.log" "the daily check found 9.9: a notification says so"
nosan "$T2"
stop27
fi

if want 28; then
echo "== 28. a big library: the scan stays quick (each zip stat'ed once; the ROM check remembered); the screen says how far it is"
T=$(newroot t28)
python3 - ../src/burn/drv "$T/root/roms" <<'PY'
import glob, os, random, re, sys, zipfile
names = set()
for d in ("pre90s", "pst90s", "neogeo", "capcom", "konami", "cave", "taito", "sega", "toaplan", "irem", "dataeast"):
    for f in glob.glob(os.path.join(sys.argv[1], d, "*.cpp")):
        names.update(re.findall(r'struct BurnDriver BurnDrv\w+ = \{\s*"([a-z0-9_]+)"', open(f, errors="replace").read()))
random.seed(5)
for n in random.sample(sorted(names), 1500):
    with zipfile.ZipFile(os.path.join(sys.argv[2], n + ".zip"), "w") as z:
        z.writestr("dummy.bin", b"\0" * 1024)
PY
scan_time() { python3 - "$1" <<'PY'
import re, sys
t = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[\s*([0-9.]+)\] \[(boot|games)\] (FBNeo PS5 |1500 archive)', line)
    if m: t.setdefault(m.group(2), float(m.group(1)))
print('%.2f' % (t['games'] - t['boot']) if 'boot' in t and 'games' in t else '99')
PY
}
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 100)" "")
first=$(scan_time $T/root/logs/boot.log)
expect "[ $rc = 0 ] && grep -q '\[games\] 1500 archive(s): [0-9]* arcade set(s) (0 complete, [0-9]* checked now)' $T/root/logs/boot.log" "1500 zips found, their sets checked"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 100)" "")
second=$(scan_time $T/root/logs/boot.log)
expect "grep -q '\[games\] 1500 archive(s): [0-9]* arcade set(s) (0 complete, 0 checked now)' $T/root/logs/boot.log" "the next start: nothing checked again (the cache)"
expect "python3 -c 'import sys; sys.exit(0 if float(\"$first\") < 8 and float(\"$second\") < 3 else 1)'" "first scan ${first} s (under 8), the next ${second} s (under 3)"
nosan "$T"
fi

if want 29; then
echo "== 29. languages: English by default; Spanish, Portuguese (Brazil), French and German translated"
expect "python3 $PWD/tests/check_i18n.py $PWD/frontend/fe_i18n.cpp >/dev/null" "every translation has its five languages and the English format specifiers"
if [ ! -d "$SETS/v-ponpoko-drop" ]; then
	mkdir -p "$SETS/v-ponpoko-drop"
	ASAN_OPTIONS=detect_leaks=0 $FAKE ponpoko "$SETS/v-ponpoko-drop" --drop 2 >/dev/null 2>&1
fi
mkset mslug
for L in 1 2 3 4; do
	T=$(newroot t29_$L)
	echo "language=$L" >>"$T/root/fbneo-ps5.ini"
	addset ponpoko "$T/root/roms"
	cp "$SETS/mslug/mslug.zip" "$T/root/roms/" # Metal Slug without neogeo.zip: a BIOS set needed and missing
	# the settings screen: every row (Down x28 back to Shader), then Up x6 to BIOS sets, its screen, back out
	pad="0:0;20:$TRIANGLE;22:0"; f=30
	for i in $(seq 1 28); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	for i in $(seq 1 6); do pad="$pad;$f:$UP;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CROSS;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 6); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CIRCLE;$((f + 2)):0;$((f + 10)):$CIRCLE;$((f + 12)):0"; f=$((f + 30))
	# the game (Cross on the shelf), its pause menu: DIP switches, save a state, every row, load it, settings over it
	pad="$pad;$f:$CROSS;$((f + 2)):0"; f=$((f + 100))
	pad="$pad;$f:$L3R3;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 4); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CROSS;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 3); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CIRCLE;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 3); do pad="$pad;$f:$UP;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CROSS;$((f + 2)):0"; f=$((f + 20))
	for i in $(seq 1 13); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CROSS;$((f + 2)):0"; f=$((f + 60))
	pad="$pad;$f:$L3R3;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 7); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CROSS;$((f + 2)):0"; f=$((f + 10))
	for i in $(seq 1 4); do pad="$pad;$f:$DOWN;$((f + 2)):0"; f=$((f + 6)); done
	pad="$pad;$f:$CIRCLE;$((f + 2)):0;$((f + 10)):$CIRCLE;$((f + 12)):0;$(QUITAT $((f + 30)))"
	rm -f $T/missing.txt
	rc=$(FBNEO_HOST_I18N_MISSING=$T/missing.txt run "$T" "$pad" "")
	ok1=$([ $rc = 0 ] && grep -q 'load state 1: ok' $T/root/logs/boot.log && grep -q 'save state 1: ok' $T/root/logs/boot.log && grep -q '\[bios\] screen' $T/root/logs/boot.log && echo y)
	# a game that won't start (a ROM missing): its message box; then no games on the shelf, and its quit question
	T2=$(newroot t29b_$L)
	echo "language=$L" >>"$T2/root/fbneo-ps5.ini"
	mkdir -p "$T2/root/roms/bad"
	cp "$SETS/v-ponpoko-drop/ponpoko.zip" "$T2/root/roms/bad/"
	rc2=$(FBNEO_HOST_I18N_MISSING=$T/missing.txt MAXFLIPS=300 run "$T2" "0:0;60:$CROSS;62:0" "" "$T2/root/roms/bad/ponpoko.zip")
	rm -rf "$T2/root/roms/bad"
	cp "$SETS/mslug/mslug.zip" "$T2/root/roms/"
	rc3=$(FBNEO_HOST_I18N_MISSING=$T/missing.txt run "$T2" "0:0;$(SHELFQUIT_AT 60)" "")
	# what was shown untranslated: only names (the games', the boards', the DIP switches', the buttons', the shaders')
	python3 - $T/missing.txt $PWD/frontend/fe_i18n.cpp ../src/burn/drv/pre90s/d_pacman.cpp > $T/untranslated.txt <<'PY'
import re, sys
src = open(sys.argv[2], encoding='utf-8').read()
translated = set(re.findall(r'"((?:[^"\\]|\\.)*)"', src))
spec = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsp])')
# a text printed from a translated format: the format as a pattern
formats = [re.compile('^' + ''.join('.*' if spec.fullmatch(part) else re.escape(part)
                                    for part in re.split('(' + spec.pattern + ')', f)) + '$', re.S)
           for f in translated if '%' in f]
driver = set(re.findall(r'"([^"]*)"', open(sys.argv[3], errors='replace').read()))
names = {'L1', 'R1', 'L2', 'R2', 'OPTIONS', 'English', 'Español', 'Português (Brasil)', 'Français', 'Deutsch',
         'CRT Easymode style', 'Capcom', 'Neo Geo', 'Sega', 'Konami', 'Taito', 'Toaplan, Cave, Psikyo', 'Data East',
         'Irem', 'Midway', 'IGS'}
for line in open(sys.argv[1], encoding='utf-8'):
    t = line.rstrip('\n')
    if t in translated or t in names or not re.search('[A-Za-z]{2}', t):
        continue
    if any(f.match(t) for f in formats):
        continue
    d = t[:-2] if t.endswith(' *') else t
    if d in driver or re.search(r'ponpoko|Ponpoko|\.zip  -  ', t) or re.match(r'^\S+ \d  \(', t):
        continue
    print(t)
PY
	expect "[ \"$ok1\" = y ] && [ $rc2 = 0 ] && [ $rc3 = 0 ] && [ ! -s $T/untranslated.txt ]" "language $L: every menu text shown is translated ($(head -c 300 $T/untranslated.txt | tr '\n' '|'))"
	nosan "$T"
	nosan "$T2"
done
# the network screens (2.0): Settings, Network shares with a share that does not connect, Add a share, the keyboard on
# Server (Circle closes it), the share type switched to NFS, Circle asks to discard (Up, Cross: discard), Connect again;
# back to the shelf: R1 to its Network tab (the share's state), Square scans again, L1 back; a set on a share that is
# not checked yet, its card's line; quit. Network shares is Up five times from Shader (Back, Updates, Debug logs,
# Language, Network shares).
for L in 1 2 3 4; do
	T=$(newroot t29net_$L)
	echo "language=$L" >>"$T/root/fbneo-ps5.ini"
	echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
	addset ponpoko "$T/root/roms"
	key() { pad="$pad;$f:$1;$((f + 2)):0"; f=$((f + ${2:-6})); } # one press, then the next one $2 flips later
	pad="0:0;20:$TRIANGLE;22:0"; f=30
	for i in $(seq 1 5); do key $UP; done
	key $CROSS 20; key $DOWN; key $CROSS 20; key $CROSS 20; key $CIRCLE 20
	key $UP; key $RIGHT 20; key $CIRCLE 20; key $UP; key $CROSS 20
	key $DOWN; key $CROSS 20; key $CIRCLE 20; key $CIRCLE 30
	key $R1 30; key $SQUARE 60; key $L1 30
	key $OPTIONS 10; key $CROSS 10
	rm -f $T/missing.txt
	rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DIR=$T/nothing FBNEO_HOST_I18N_MISSING=$T/missing.txt run "$T" "$pad" "")
	# a set on a share, its check slow (2 s a read): its card says it is not checked yet; its BIOS sets screen row too
	mkset bstarsh
	T2=$(newroot t29netb_$L)
	echo "language=$L" >>"$T2/root/fbneo-ps5.ini"
	mkdir -p "$T2/share"
	addset bstarsh "$T2/share"
	echo "smb://10.0.0.5/games" >"$T2/root/shares.txt"
	pad="0:0"; f=200 # (the share connected and its sets listed by then; their check, 2 s a read, still running)
	key $TRIANGLE 20; for i in $(seq 1 6); do key $UP; done; key $CROSS 20; key $CIRCLE 10; key $CIRCLE 20
	key $OPTIONS 10; key $CROSS 10
	rc2=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DELAY_MS=2000 FBNEO_HOST_NETFS_DIR=$T2/share FBNEO_HOST_I18N_MISSING=$T/missing.txt run "$T2" "$pad" "")
	python3 - $T/missing.txt $PWD/frontend/fe_i18n.cpp > $T/untranslated.txt <<'PY'
import os, re, sys
src = open(sys.argv[2], encoding='utf-8').read()
translated = set(re.findall(r'"((?:[^"\\]|\\.)*)"', src))
spec = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXeEfgGcsp])')
formats = [re.compile('^' + ''.join('.*' if spec.fullmatch(part) else re.escape(part)
                                    for part in re.split('(' + spec.pattern + ')', f)) + '$', re.S)
           for f in translated if '%' in f]
names = {'L1', 'R1', 'L2', 'R2', 'L3', 'R3', 'OPTIONS', 'English', 'Español', 'Português (Brasil)', 'Français', 'Deutsch',
         'SMB (Windows, Samba, NAS)', 'NFS', 'USB', 'Capcom', 'Neo Geo', 'Classics', 'Other'}
for line in (open(sys.argv[1], encoding='utf-8') if os.path.exists(sys.argv[1]) else []):
    t = line.rstrip('\n')
    if t in translated or t in names or not re.search('[A-Za-z]{2}', t) or re.fullmatch(r'[\d%. ]+', t):
        continue
    if any(f.match(t) for f in formats):
        continue
    if re.search(r'ponpoko|Ponpoko|bstars|Baseball|\.zip  -  ', t):
        continue
    print(t)
PY
	expect "[ $rc = 0 ] && [ $rc2 = 0 ] && grep -q 'network tab: scan again' $T/root/logs/boot.log && grep -q '(not checked yet)' $T2/root/logs/boot.log && grep -q '\[bios\] screen' $T2/root/logs/boot.log && [ -s $T/missing.txt ] && [ ! -s $T/untranslated.txt ]" "language $L: the network screens, the Network tab and a set not checked yet are translated ($(head -c 200 $T/untranslated.txt | tr '\n' '|'))"
	nosan "$T"
	nosan "$T2"
done
# English stays the default: no setting, English on the screen (the texts pass through as they are)
T=$(newroot t29en)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_I18N_MISSING=$T/missing.txt run "$T" "0:0;20:$TRIANGLE;22:0;40:$CIRCLE;42:0;$(SHELFQUIT_AT 60)" "")
expect "[ $rc = 0 ] && [ ! -s $T/missing.txt ] && grep -q '^language=0$' $T/root/fbneo-ps5.ini" "English by default (language=0): nothing is looked up"
fi

# ---- network shares (2.0): netfs, sets on a share, the Network shares screen, the shelf's storage tabs -------------------
# From NES FCEUmm PS5 1.1's groups 29-33 (Beetle PCE PS5 1.1.1's, thatboialex's design in Snes9x PS5). The host build's
# share backend (host/netfs_dirbackend.cpp) answers every smb:// and nfs:// address with the folder FBNEO_HOST_NETFS_DIR
# names.
# the active storage tab in a dump: Internal or Network (the pill in the accent colour, left or right of the middle)
tab_on() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB')
xs = [x for x in range(0, 1920) if a.getpixel((x, 20)) == (78, 62, 170)]
dark = [x for x in range(0, 1920) if a.getpixel((x, 20)) == (32, 30, 58)]
if not xs:
    sys.exit(1)
mid = (min(xs) + max(xs)) / 2
other = (min(dark) + max(dark)) / 2 if dark else None
on = 'Internal' if other is None or mid < other else 'Network'
sys.exit(0 if on == sys.argv[2] else 1)
PY
}
mkset bstarsh
mkset mslug

if want 30; then
echo "== 30. netfs: share addresses and redaction, routing, the libc --wrap layer, minizip and zlib on a share, the watchdog (in-memory share)"
NETFS_OUT=$(ASAN_OPTIONS=detect_leaks=1 timeout 120 "$PWD/build/host/test-netfs" 2>&1)
NETFS_RC=$?
echo "$NETFS_OUT" | tail -3 | sed 's/^/  /'
expect "[ $NETFS_RC = 0 ]" "netfs: every check passes"
expect "! echo \"\$NETFS_OUT\" | grep -q 'runtime error\|Sanitizer'" "no sanitizer reports"
fi

if want 31; then
echo "== 31. sets on a share: listed at once, checked in the background, started from the shelf with their parent and BIOS"
# a. Pac-Man and its parent at the share's root; a Neo Geo clone, its parent and the BIOS in a folder of it
T=$(newroot t31)
mkdir -p "$T/share/neogeo"
addset pacman "$T/share"
addset bstarsh "$T/share/neogeo"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
before=$(find "$T/share" -type f | wc -l)
# the shelf opens on the Network tab (no game on the console): B (Baseball Stars Professional, the parent then its
# clone), Right, Cross; back to the list, Down to P (Pac-Man), Cross; Up held: green
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=900 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;196:$RIGHT;198:0;200:$CROSS;202:0;330:$L3R3;332:0;340:$UP;342:0;344:$UP;346:0;350:$CROSS;352:0;450:$DOWN;452:0;480:$CROSS;482:0;600:$UP;640:0;$(QUITAT 700)" "190,580,620")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[net\] 1 share(s) in shares.txt' $T/root/logs/boot.log && grep -q '\[net\] Connected to smb://10.0.0.5/games' $T/root/logs/boot.log" "shares.txt read, the share connected"
expect "grep -q '\[games\] 5 archive(s): 4 arcade set(s) (0 complete, 0 checked now), .* 0 .7z, 5 on network shares ([0-9]* set(s) left to the background check)' $T/root/logs/boot.log" "the scan lists the 4 sets on the share without opening an archive (their checks, the BIOS's and the merged clones': left to the background)"
expect "grep -q '\[bios\] neogeo.zip (Neo Geo): on a share, checked in the background' $T/root/logs/boot.log" "the BIOS on the share: not checked by the scan"
expect "grep -q '\[games\] checking [0-9]* set(s) on network shares in the background' $T/root/logs/boot.log && grep -q '\[games\] network check pass: [0-9]* set(s) checked\$' $T/root/logs/boot.log" "the background pass checks them"
expect "grep -q '\[games\] 5 archive(s): 4 arcade set(s) (4 complete, 0 checked now), .* 5 on network shares (0 set(s) left' $T/root/logs/boot.log" "the next scan: all 4 complete, from the cache"
expect "grep -q '\[bios\] neogeo.zip (Neo Geo): OK; needed by 2 of your games' $T/root/logs/boot.log" "then the BIOS on the share: checked, OK"
expect "grep -q 'loading /smb/10.0.0.5/games/neogeo/bstarsh.zip' $T/root/logs/boot.log && grep -q 'running bstarsh \"Baseball Stars Professional (NGH-002)\"' $T/root/logs/boot.log" "a Neo Geo clone on the share starts, its parent and the BIOS from the same share"
expect "grep -q 'loading /smb/10.0.0.5/games/pacman.zip' $T/root/logs/boot.log && $CHECK $T/dump/flip00580.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00620.ppm 960 540 green >/dev/null" "Pac-Man from the share runs (red; Up: green)"
expect "[ \$(find $T/share -type f | wc -l) = $before ] && [ -f $T/root/saves/bstarsh.nvram -o -d $T/root/saves ]" "nothing is written to the share (saves stay on the console)"
expect "grep -q 'last_rom=/smb/10.0.0.5/games/pacman.zip' $T/root/fbneo-ps5.ini && grep -q 'last_tab=2' $T/root/fbneo-ps5.ini" "the shelf remembers the game and its tab"
expect "grep -q '\[shelf\] tab Network: 4 game(s)' $T/root/logs/boot.log && tab_on $T/dump/flip00190.ppm Network" "the shelf shows the Network tab"
nosan "$T"
# the next start reads no archive of the share before the shelf: everything is in romcheck.txt
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=300 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;$(SHELFQUIT_AT 200)" "")
expect "[ $rc = 0 ] && ! grep -q 'checking .* set(s) on network shares' $T/root/logs/boot.log && grep -q '(4 complete, 0 checked now)' $T/root/logs/boot.log" "the next start: no background check needed"
nosan "$T"
# b. a set whose BIOS is on no drive and no share: hidden once checked; with Show incomplete sets, starting it gives the
# usual message (the ROMs missing, the archives looked in)
T=$(newroot t31b)
mkdir -p "$T/share"
cp "$SETS/mslug/mslug.zip" "$T/share/"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
echo "show_incomplete=1" >>"$T/root/fbneo-ps5.ini"
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=600 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;200:$CROSS;202:0;260:$CROSS;262:0;$(SHELFQUIT_AT 300)" "240")
expect "[ $rc = 0 ] && grep -q '\[games\] mslug: [0-9]* ROM(s) missing, first' $T/root/logs/boot.log" "the background check finds Metal Slug incomplete (no neogeo.zip)"
expect "grep -q '\[burn\] mslug: [0-9]* ROM(s) missing (.*), looked in mslug.zip, neogeo.zip' $T/root/logs/boot.log && grep -q 'could not start mslug.zip' $T/out.txt" "starting it: the usual message, the missing ROMs and the archives looked in"
nosan "$T"
# c. a big set (Metal Slug and its BIOS stored, 32 MiB) on a share with 20 ms on every read: read as the driver starts
T=$(newroot t31c)
mkdir -p "$T/share"
for z in mslug neogeo; do (cd "$SETS/mslug" && python3 -c "import zipfile,sys; s=zipfile.ZipFile(sys.argv[1]); d=zipfile.ZipFile(sys.argv[2],'w',zipfile.ZIP_STORED); [d.writestr(i.filename, s.read(i.filename)) for i in s.infolist()]; d.close()" $z.zip "$T/share/$z.zip"); done
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DELAY_MS=20 MAXFLIPS=1500 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;400:$CROSS;402:0;$(QUITAT 900)" "")
expect "[ $rc = 0 ] && [ \$(stat -c %s $T/share/mslug.zip) -gt 25000000 ] && grep -q 'running mslug \"Metal Slug - Super Vehicle-001\"' $T/root/logs/boot.log" "a 27 MB stored set on a slow share starts"
expect "! grep -q 'in realtime' $T/root/logs/boot.log" "and the running game never waits for the share"
nosan "$T"
# d. a read error while the share stays connected (a link that drops, the watchdog) is not the archive's fault: nothing is
# kept, the set stays listed, the next start checks it again
T=$(newroot t31d)
mkdir -p "$T/share"
addset pacman "$T/share"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_EIO=pacman.zip MAXFLIPS=300 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;$(SHELFQUIT_AT 250)" "")
expect "[ $rc = 0 ] && grep -q '\[games\] pacman: an archive could not be read (pacman.zip): checked again next time' $T/root/logs/boot.log" "a read error: the pass says so"
expect "! grep -q '^pacman	' $T/root/config/romcheck.txt && grep -q '^puckman	' $T/root/config/romcheck.txt" "and keeps nothing for that set (the parent's own check is kept)"
expect "[ \$(grep -c 'pacman.zip -> \"Pac-Man (Midway)\"' $T/root/logs/boot.log) -ge 2 ]" "the set is still listed after the pass"
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=300 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;$(SHELFQUIT_AT 250)" "")
expect "[ $rc = 0 ] && grep -q '^pacman	' $T/root/config/romcheck.txt && grep -q '(2 complete, 0 checked now)' $T/root/logs/boot.log" "the next start checks it again"
nosan "$T"
# e. Settings (Triangle) while the pass reads (3 s a read): the pass stops first (Settings > Network shares may connect
# the shares again, which needs no other netfs call running); the set it was on is checked once Settings is left
T=$(newroot t31e)
mkdir -p "$T/share"
addset pacman "$T/share"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DELAY_MS=3000 MAXFLIPS=1200 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;60:$TRIANGLE;62:0;90:$CIRCLE;92:0;$(SHELFQUIT_AT 1100)" "")
expect "grep -q 'network check pass: [01] set(s) checked (stopped)' $T/root/logs/boot.log" "Settings stops the pass"
expect "grep -q '^pacman	' $T/root/config/romcheck.txt && grep -q '^puckman	' $T/root/config/romcheck.txt" "and both sets are checked once Settings is left"
nosan "$T"
# f. a share that does not connect: the shelf still works and says why; shares.txt is written with its comments when
# missing, a bad line is named by its number, never its text, and no password reaches a log
T=$(newroot t31f)
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;100:$OPTIONS;102:0;110:$CROSS;112:0" "")
expect "[ $rc = 0 ] && grep -q '\[games\] 0 archive(s)' $T/root/logs/boot.log && grep -q '\[net\] The folder games is not on 10.0.0.5' $T/root/logs/boot.log" "a share that does not connect leaves the shelf working, and says why"
nosan "$T"
T=$(newroot t31g)
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 40)" "")
expect "[ $rc = 0 ] && grep -q '^# FBNeo PS5 network shares' $T/root/shares.txt && ! grep -q '^smb://\|^nfs://' $T/root/shares.txt" "a commented shares.txt is written when there is none"
printf 'smb://bob@10.0.0.5/games?password=Hunter2x\nftp://10.0.0.5/x?password=Hunter2y\n' >"$T/root/shares.txt"
rc=$(FBNEO_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;$(SHELFQUIT_AT 40)" "")
expect "[ $rc = 0 ] && grep -q 'shares.txt line 2: ' $T/root/logs/boot.log && ! grep -rq 'Hunter2' $T/root/logs $T/out.txt" "a bad line is named by its number; no password in a log or on stdout"
nosan "$T"
fi

if want 32; then
echo "== 32. the Network shares screen: add, change, test and remove a share with the on-screen keyboard (Settings, Network shares)"
# tests/osk_pad.py turns the steps into a pad script (the keyboard's layout is its model). Settings is Triangle on the
# shelf; Up five times from Shader reaches Network shares (Back, Updates, Debug logs, Language, Network shares). Every run
# ends: Circle twice, OPTIONS, Cross.
net_pad() { python3 tests/osk_pad.py 2>/dev/null; }
net_quit='press CIRCLE 2
press OPTIONS
wait 8
press CROSS'
# a. a new share: server, share, folder with a space, user, a password that needs Shift and both kinds of symbols
T=$(newroot t32)
mkdir -p "$T/share"
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 5
press CROSS
press CROSS
field 10.0.0.5
press DOWN
field games
press DOWN
field Arcade Sets
press DOWN
field jason
press DOWN
field Zq9@pw&1
press DOWN 6
press CROSS
wait 20
$net_quit
STEPS
)
rc=$(FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "$PAD" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(tail -n 1 $T/root/shares.txt)\" = 'smb://jason@10.0.0.5/games/Arcade%20Sets?password=Zq9%40pw%261' ]" "the share is written as one escaped line"
expect "grep -q '^# FBNeo PS5 network shares' $T/root/shares.txt" "after the comments the app wrote"
expect "grep -q '\[net\] shares.txt: saved smb://jason@10.0.0.5/games/Arcade%20Sets' $T/root/logs/boot.log" "the log says what was saved"
expect "grep -q '\[net\] Connected to smb://jason@10.0.0.5/games/Arcade%20Sets' $T/root/logs/boot.log" "and the share connects at once, without a restart"
expect "! grep -rqE 'Zq9|pw%26' $T/root/logs $T/out.txt" "the password is in no log and not on stdout"
nosan "$T"
# b. change one share (its note stays), test the connection, remove the other
T=$(newroot t32b)
mkdir -p "$T/share/Sub" && echo x >"$T/share/a.zip"
printf '# my shares\nsmb://old/x   # my note\nnfs://nas/export/arcade\n' >"$T/root/shares.txt"
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 5
press CROSS
press CROSS
press DOWN 3
field bob
press DOWN 6
press CROSS
wait 120
press DOWN
press CROSS
wait 20
press DOWN
press CROSS
press DOWN 7
press CROSS
wait 8
press UP
press CROSS
wait 20
$net_quit
STEPS
)
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "$PAD" "")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "[ \"\$(cat $T/root/shares.txt)\" = \"\$(printf '# my shares\nsmb://bob@old/x   # my note')\" ]" "one line changed (note kept), the other was removed, the comment stays"
expect "grep -q '\[net\] test of smb://bob@old/x: Connected. The folder has 1 file and 1 folder.' $T/root/logs/boot.log" "Test connection reports what is in the folder"
expect "grep -q '\[net\] shares.txt: removed nfs://nas/export/arcade' $T/root/logs/boot.log" "the removal is logged"
nosan "$T"
# c. Save with nothing filled in changes nothing
T=$(newroot t32c)
PAD=$(net_pad <<STEPS
frame 40
press TRIANGLE
press UP 5
press CROSS
press CROSS
press DOWN 10
press CROSS
wait 8
press CIRCLE
$net_quit
STEPS
)
rc=$(run "$T" "$PAD" "")
expect "[ $rc = 0 ] && ! grep -q '^smb://\|^nfs://' $T/root/shares.txt" "an empty form is not saved (and Circle on a form with no changes leaves it at once)"
nosan "$T"
# d. over a game (Settings from the pause menu) the row is there but can't be chosen (the game may be reading from a
# share): the pause menu, Down seven times to Settings; there Up six times from Shader passes Network shares and lands on
# Show incomplete sets (Back, Updates, Debug logs, Language, BIOS sets, Show incomplete sets), which Cross switches on
T=$(newroot t32d)
addset ponpoko "$T/root/roms"
pad="0:0"; f=100
key() { pad="$pad;$f:$1;$((f + 2)):0"; f=$((f + ${2:-8})); }
key $L3R3 12; for i in $(seq 1 7); do key $DOWN; done; key $CROSS 12
for i in $(seq 1 6); do key $UP; done; key $CROSS; key $CIRCLE 12; key $CIRCLE 20
pad="$pad;$(QUITAT $f)"
rc=$(MAXFLIPS=900 run "$T" "$pad" "" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ] && grep -q '^show_incomplete=1' $T/root/fbneo-ps5.ini" "over a game: Network shares is skipped (Up six times reaches Show incomplete sets)"
nosan "$T"
fi

if want 33; then
echo "== 33. shelf tabs: Internal and Network apart, L1 / R1 between them, the last tab kept, Square on Network scans again; L2 / R2 skip 10"
T=$(newroot t33)
mkdir -p "$T/share"
addset ponpoko "$T/root/roms"
addset pacman "$T/share"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;220:$R1;222:0;300:$SQUARE;302:0;380:$L1;382:0;420:$OPTIONS;422:0;430:$CROSS;432:0" "200,260,400")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "tab_on $T/dump/flip00200.ppm Internal" "the shelf opens on the Internal tab"
expect "tab_on $T/dump/flip00260.ppm Network" "R1 goes to the Network tab"
expect "tab_on $T/dump/flip00400.ppm Internal" "L1 comes back"
expect "grep -q '\[shelf\] tab Network: 2 game(s)' $T/root/logs/boot.log" "the Network tab holds the sets on the share (Pac-Man and its parent)"
expect "grep -q '\[shelf\] tab Internal: 1 game(s)' $T/root/logs/boot.log" "and the Internal tab only the console's"
expect "grep -q '\[shelf\] network tab: scan again\$' $T/root/logs/boot.log" "Square on the Network tab scans again"
expect "[ \$(grep -c '\[games\] 3 archive(s)' $T/root/logs/boot.log) -ge 2 ]" "and the scan found the sets again"
expect "grep -q 'last_tab=0' $T/root/fbneo-ps5.ini" "the tab left on is kept (Internal)"
nosan "$T"
T=$(newroot t33b)
mkdir -p "$T/share"
addset ponpoko "$T/root/roms"
addset pacman "$T/share"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
echo "last_tab=2" >>"$T/root/fbneo-ps5.ini"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;300:$OPTIONS;302:0;310:$CROSS;312:0" "250")
expect "[ $rc = 0 ] && tab_on $T/dump/flip00250.ppm Network" "with last_tab=2 and a share set up the shelf opens on Network"
expect "grep -q 'last_tab=2' $T/root/fbneo-ps5.ini" "and keeps the tab on quit"
nosan "$T"
T=$(newroot t33c)
addset ponpoko "$T/root/roms"
echo "last_tab=2" >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;60:$R1;62:0;70:$SQUARE;72:0;90:$L1;92:0;120:$OPTIONS;122:0;130:$CROSS;132:0" "40,80,110")
expect "[ $rc = 0 ] && tab_on $T/dump/flip00040.ppm Internal" "last_tab=2 without a share set up opens on the games instead"
expect "tab_on $T/dump/flip00080.ppm Network && grep -q '\[shelf\] tab Network: 0 game(s)' $T/root/logs/boot.log" "R1 shows the empty Network tab"
expect "grep -q '\[shelf\] network tab: scan again\$' $T/root/logs/boot.log" "Square on it scans again, with no share to ask"
expect "tab_on $T/dump/flip00110.ppm Internal && grep -q 'last_tab=0' $T/root/fbneo-ps5.ini" "L1 goes back and the tab is kept"
nosan "$T"
T=$(newroot t33d)
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DIR="$T/nothing" run "$T" "0:0;150:$SQUARE;152:0;330:$OPTIONS;332:0;340:$CROSS;342:0" "100")
expect "[ $rc = 0 ] && tab_on $T/dump/flip00100.ppm Network" "a share that is down leaves a shelf with no games on the Network tab"
expect "grep -q '\[shelf\] network tab: scan again, reconnecting' $T/root/logs/boot.log" "Square connects it again"
expect "[ \$(grep -c 'The folder games is not on 10.0.0.5' $T/root/logs/boot.log) -ge 2 ]" "and says why each time"
nosan "$T"
# the letters (Up / Down) and the makers (L3 / R3) work inside the storage tab; L2 / R2 skip 10 games (L1 / R1 did
# before 2.0): R2 from Pac-Man (the first of P: Pac-Man, Ponpoko, Puck Man) goes to Puck Man, the last
T=$(newroot t33e)
mkdir -p "$T/share"
addset pacman "$T/root/roms"
addset ponpoko "$T/root/roms"
mkset 1942
addset 1942 "$T/share"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
# (back to the shelf from a game: the pause menu, Up twice, Cross)
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=900 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;150:$R2;152:0;170:$CROSS;172:0;230:$L3R3;232:0;240:$UP;242:0;246:$UP;248:0;256:$CROSS;258:0;320:$R1;322:0;340:$R3;342:0;380:$CROSS;382:0;$(QUITAT 480)" "")
expect "[ $rc = 0 ] && grep -q 'loading .*/roms/puckman.zip' $T/root/logs/boot.log" "R2 skips ahead (Pac-Man -> Puck Man, the last of three)"
expect "grep -q '\[shelf\] tab Network, Capcom, #: 1 game(s)' $T/root/logs/boot.log && grep -q 'loading /smb/10.0.0.5/games/1942.zip' $T/root/logs/boot.log" "on the Network tab, R3 shows its maker (Capcom), whose game runs"
nosan "$T"
fi

# ---- 7-Zip (2.0): ROM sets in a .7z (frontend/fe_7z.cpp on FBNeo's lib7z); the archives are tests/7z/ ----------------
if want 34; then
echo "== 34. 7z: the reader on every kind of archive; a .7z set listed, checked, started and running; a .7z on a share; what is refused, and why"
SZ_OUT=$(ASAN_OPTIONS=detect_leaks=1 timeout 120 "$PWD/build/host/test-7z" "$PWD/tests/7z" 2>&1)
SZ_RC=$?
echo "$SZ_OUT" | tail -3 | sed 's/^/  /'
expect "[ $SZ_RC = 0 ]" "the 7z reader: LZMA, LZMA2, PPMd, BCJ, Delta, a solid block read as a set's ROMs; empty, truncated, corrupt, BZip2, AES, a PPMd model or a block too big: every check passes"
expect "! echo \"\$SZ_OUT\" | grep -q 'runtime error\|Sanitizer'" "no sanitizer reports"
# a. Pac-Man as a .7z (its parent puckman.zip beside it): listed and checked like a zip, started from the shelf, runs
T=$(newroot t34)
cp tests/7z/pacman.7z "$T/root/roms/"
cp "$SETS/pacman/puckman.zip" "$T/root/roms/"
rc=$(MAXFLIPS=600 run "$T" "0:0;40:$CROSS;42:0;150:$UP;190:0;$(QUITAT 300)" "120,170")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[games\] 2 archive(s): 2 arcade set(s) (2 complete, 2 checked now), .* 1 .7z, 0 on network shares' $T/root/logs/boot.log && grep -q 'roms/pacman.7z -> \"Pac-Man (Midway)\"' $T/root/logs/boot.log" "the .7z set is named and checked as a zip is"
expect "grep -q '^pacman	pacman.7z:' $T/root/config/romcheck.txt" "its check is kept (the signature says .7z)"
expect "grep -q 'loading .*/roms/pacman.7z' $T/root/logs/boot.log && grep -q 'running pacman \"Pac-Man (Midway)\"' $T/root/logs/boot.log" "Cross starts the set from the .7z"
expect "$CHECK $T/dump/flip00120.ppm 960 540 red >/dev/null && $CHECK $T/dump/flip00170.ppm 960 540 green >/dev/null" "and it runs (red; Up: green)"
nosan "$T"
# b. a Neo Geo clone as a .7z on a share, its parent and the BIOS as zips on the same share: checked in the background,
# started from the share
T=$(newroot t34b)
mkdir -p "$T/share"
cp tests/7z/bstarsh.7z "$T/share/"
cp "$SETS/bstarsh/bstars.zip" "$SETS/bstarsh/neogeo.zip" "$T/share/"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=600 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;196:$RIGHT;198:0;200:$CROSS;202:0;$(QUITAT 350)" "")
expect "[ $rc = 0 ] && grep -q '1 .7z, 3 on network shares' $T/root/logs/boot.log && grep -q 'network check pass: [0-9]* set(s) checked\$' $T/root/logs/boot.log" "listed and checked in the background"
expect "grep -q '^bstarsh	bstarsh.7z:' $T/root/config/romcheck.txt" "its check is kept"
expect "grep -q 'loading /smb/10.0.0.5/games/bstarsh.7z' $T/root/logs/boot.log && grep -q 'running bstarsh \"Baseball Stars Professional (NGH-002)\"' $T/root/logs/boot.log" "it starts from the share"
nosan "$T"
# c. what can't be started, and the screen says why: an encrypted set (its names are readable: it is listed), a set in a
# solid block over 512 MiB (listed: its directory is complete); in one folder a set's .zip is used, not its .7z
T=$(newroot t34c)
cp tests/7z/ponpoko.7z "$T/root/roms/"
cp tests/7z/puckmanbb.7z "$T/root/roms/puckman.7z"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 60)" "")
expect "[ $rc = 0 ] && grep -q '2 archive(s): 2 arcade set(s) (2 complete' $T/root/logs/boot.log" "both are listed (their directories hold every ROM)"
rc=$(MAXFLIPS=300 run "$T" "0:0;60:$CROSS;62:0" "50" "$T/root/roms/ponpoko.7z")
expect "grep -q 'could not start ponpoko.7z: FBNeo PS5 could not unpack ponpoko.7z. The archive is encrypted.' $T/root/logs/boot.log && [ -f $T/dump/flip00050.ppm ]" "ponpoko.7z: not started; the screen says it is encrypted"
nosan "$T"
rc=$(MAXFLIPS=300 run "$T" "0:0;60:$CROSS;62:0" "50" "$T/root/roms/puckman.7z")
expect "grep -q 'could not start puckman.7z: FBNeo PS5 could not unpack puckman.7z. The solid block it is packed in is too big (512 MiB at most)' $T/root/logs/boot.log && grep -q 'puckman.7z: can.t unpack .* (too big)' $T/root/logs/boot.log" "puckman.7z (a 513 MiB block): refused before anything is decoded, the screen says why"
nosan "$T"
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;$(SHELFQUIT_AT 60)" "")
expect "[ $rc = 0 ] && grep -q 'roms/ponpoko.zip -> \"Ponpoko\"' $T/root/logs/boot.log && ! grep -q 'roms/ponpoko.7z ->' $T/root/logs/boot.log" "ponpoko.zip beside ponpoko.7z: the zip is the one used"
nosan "$T"
fi

if want 35; then
echo "== 35. the background check after a share drops or reconnects; a zip directory cut by a read error; a cancel; .7z blocks let go"
# FBNEO_HOST_NETFS_CONNECT_MS: every connect takes that long; FBNEO_HOST_NETFS_DOWN: a server that never answers;
# FBNEO_HOST_NETFS_EIO_RANGE: the reads of some bytes of a file fail
# a. Square on the Network tab with one share down (10.0.0.6) while the pass reads (2 s a read): the shares connect
# again (3 s each), the library is scanned meanwhile without the share; the sets still queued are not checked against
# that (no archive found) and kept as incomplete: they are checked once the share is back, and found complete
T=$(newroot t35a)
mkdir -p "$T/share"
addset pacman "$T/share"
printf 'smb://10.0.0.5/games\nsmb://10.0.0.6/games\n' >"$T/root/shares.txt"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DELAY_MS=2000 FBNEO_HOST_NETFS_CONNECT_MS=3000 FBNEO_HOST_NETFS_DOWN=10.0.0.6 MAXFLIPS=1500 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;280:$SQUARE;282:0;$(SHELFQUIT_AT 1400)" "")
expect "[ $rc = 0 ] && grep -q 'network tab: scan again, reconnecting' $T/root/logs/boot.log" "Square while the pass reads: the shares connect again"
expect "grep -q '^puckman	[^	]*	0\$' $T/root/config/romcheck.txt && grep -q '^pacman	[^	]*	0\$' $T/root/config/romcheck.txt" "both sets kept as complete (none kept as incomplete while the share was away)"
expect "grep -q '(2 complete, 0 checked now)' $T/root/logs/boot.log" "and listed complete once the share is back"
nosan "$T"
# b. Settings > Network shares > Connect again while the pass reads: the pass goes on once the share has answered (it
# no longer gives the sets up as unreadable for the rest of the visit)
T=$(newroot t35b)
mkdir -p "$T/share"
addset pacman "$T/share"
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
pad="0:0"; f=200
key() { pad="$pad;$f:$1;$((f + 2)):0"; f=$((f + ${2:-8})); }
key $TRIANGLE 20; for i in $(seq 1 5); do key $UP; done; key $CROSS 30; key $DOWN; key $DOWN; key $CROSS 30; key $CIRCLE 20; key $CIRCLE 20
pad="$pad;$(SHELFQUIT_AT 1300)"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_DELAY_MS=2000 FBNEO_HOST_NETFS_CONNECT_MS=2000 MAXFLIPS=1400 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "$pad" "")
expect "[ $rc = 0 ] && grep -q '^puckman	[^	]*	0\$' $T/root/config/romcheck.txt && grep -q '^pacman	[^	]*	0\$' $T/root/config/romcheck.txt" "Connect again in Settings: both sets are checked once the share is back"
expect "grep -q '(2 complete, 0 checked now)' $T/root/logs/boot.log" "and listed complete in the same visit of the shelf"
nosan "$T"
# c. a zip whose directory sits across netfs's second block (512 KiB - 1 MiB), whose reads fail: a read error, not a
# zip with no ROMs; nothing kept, and the next start (no error) finds the set complete
T=$(newroot t35c)
mkdir -p "$T/share"
cp "$SETS/pacman/puckman.zip" "$T/share/"
python3 - "$SETS/pacman/pacman.zip" "$T/share/pacman.zip" <<'PY'
import sys, zipfile
src = zipfile.ZipFile(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_STORED) as z:
    z.writestr('padding.bin', b'\x55' * (700 * 1024))
    for i in src.infolist():
        z.writestr(i.filename, src.read(i.filename))
    for n in range(8000):
        z.writestr('e%05d' % n, b'')
PY
echo "smb://10.0.0.5/games" >"$T/root/shares.txt"
cdir=$(python3 -c "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); print(z.start_dir)" "$T/share/pacman.zip")
expect "[ $cdir -gt 524288 ] && [ $cdir -lt 1048576 ] && [ \$(stat -c %s $T/share/pacman.zip) -gt 1100000 ]" "the test zip: its directory starts in the second block, its end record in the third"
rc=$(FBNEO_HOST_REALTIME=1 FBNEO_HOST_NETFS_EIO_RANGE=pacman.zip:524288-1048575 MAXFLIPS=400 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;$(SHELFQUIT_AT 300)" "")
expect "[ $rc = 0 ] && grep -q 'pacman.zip: its directory could not be read to the end' $T/root/logs/boot.log && grep -q '\[games\] pacman: an archive could not be read (pacman.zip)' $T/root/logs/boot.log" "the directory's read error is a read error"
expect "! grep -q '^pacman	' $T/root/config/romcheck.txt" "and nothing is kept for the set"
rc=$(FBNEO_HOST_REALTIME=1 MAXFLIPS=400 FBNEO_HOST_NETFS_DIR="$T/share" run "$T" "0:0;$(SHELFQUIT_AT 300)" "")
expect "[ $rc = 0 ] && grep -q '^pacman	[^	]*	0\$' $T/root/config/romcheck.txt" "the next start: the set is checked again, complete"
nosan "$T"
# d. the check's cancel flag (NetCheck stopping): a zip's and a .7z's directory not read, a read error, not ROMs missing
T=$(newroot t35d)
cp "$SETS/pacman/puckman.zip" "$T/root/roms/" && cp tests/7z/pacman.7z "$T/root/roms/"
o1=$(ASAN_OPTIONS=detect_leaks=0 "$HEADLESS" check "$T/root/roms" pacman 2>/dev/null)
o2=$(ASAN_OPTIONS=detect_leaks=0 "$HEADLESS" check "$T/root/roms" pacman --cancelled 2>/dev/null)
expect "echo '$o1' | grep -q '^missing 0 read_error 0 stamps pacman.7z:[0-9]*:[0-9]*;puckman:[0-9]*:[0-9]*;'" "the check: complete, each archive's size and date ($o1)"
expect "echo '$o2' | grep -q 'read_error 1'" "cancelled: a read error ($o2)"
# e. a .7z set (its parent a .7z too, nothing read from it: the set's archive has every ROM): an archive lets go of its
# decoded block once the essential ROMs the driver needs from it are read, before the driver has started
T=$(newroot t35e)
cp tests/7z/pacman.7z tests/7z/puckman.7z "$T/root/roms/"
rc=$(MAXFLIPS=300 run "$T" "0:0;$(QUITAT 120)" "" "$T/root/roms/pacman.7z")
expect "[ $rc = 0 ] && grep -q 'running pacman' $T/root/logs/boot.log && awk '/pacman.7z: every ROM read from it, its block .* let go/{n++} /\[burn\] running pacman/{exit !(n == 1)}' $T/root/logs/boot.log" "pacman.7z lets go of its block once its ROMs are read, before the driver has started"
nosan "$T"
fi

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
