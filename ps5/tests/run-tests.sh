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
echo "== 1. a vertical game from the command line (Pac-Man): picture turned upright, input, quick save, quit"
T=$(newroot t1)
addset pacman "$T/root/roms"
rc=$(run "$T" "0:0;100:$UP;140:0;200:$L2UP;205:0;$(QUITAT 220)" "90,130" "$T/root/roms/pacman.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'running pacman \"Pac-Man (Midway)\" (Pac-man), 60.000 fps (at 60 Hz, vsync), aspect 0.7500' $T/root/logs/boot.log" "FBNeo runs the set (60 Hz, a 3:4 monitor)"
expect "grep -q 'picture 224x288' $T/root/logs/boot.log" "the 288x224 screen is turned upright: 224x288"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "red picture where the log says, black around it"
expect "grep -q -- '-> [0-9]*,0 810x1080' $T/root/logs/boot.log" "a tall picture: 810x1080 in the middle of the screen"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "Up -> the game sees the joystick (green)"
expect "[ -f $T/root/states/pacman.state1 ]" "L2 + Up wrote states/pacman.state1"
expect "grep -q 'save state 1: ok' $T/root/logs/boot.log" "logged the save"
expect "grep -q '\[burn\] FBNeo 1.0.0.3, [0-9]* drivers' $T/root/logs/boot.log" "the core's own log reaches boot.log"
nosan "$T"
fi

if want 2; then
echo "== 2. a horizontal game with a button (Ponpoko): Cross is button 1; load state with L2 + Down"
T=$(newroot t2)
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;60:$L2UP;62:0;100:$CROSS;140:0;160:$L2DOWN;162:0;$(QUITAT 200)" "90,130" "$T/root/roms/ponpoko.zip")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q 'running ponpoko \"Ponpoko\" (Pac-man), 60.000 fps (at 60 Hz, vsync), aspect 1.3333, 1 button(s)' $T/root/logs/boot.log" "a 4:3 game with one button"
expect "$RECT $T/dump/flip00090.ppm $T/root/logs/boot.log red >/dev/null" "red picture where the log says"
expect "grep -q -- '-> 240,0 1440x1080' $T/root/logs/boot.log" "a 4:3 picture: 1440x1080"
expect "$CHECK $T/dump/flip00130.ppm 960 540 green >/dev/null" "Cross -> button 1 (green)"
expect "grep -q 'load state 1: ok' $T/root/logs/boot.log" "L2 + Down loaded the state"
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
expect "grep -q '\[games\] 5 zip(s): 4 arcade set(s) (3 complete, 4 checked now), 0 BIOS set(s), 1 not FBNeo.s' $T/root/logs/boot.log" "5 zips: 4 sets, 3 complete, 1 unknown"
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
expect "grep -q '6 zip(s): 4 arcade set(s) (4 complete, 1 checked now), 1 BIOS set(s)' $T/root/logs/boot.log" "neogeo.zip added (another folder): Metal Slug checked again, complete; the BIOS isn't a game"
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
echo "== 6. button layouts: custom (button 1 on Triangle), fighting (button 1 on Square)"
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
# the settings screen: Button 1 row Right -> the layout becomes custom
T=$(newroot t6c)
addset ponpoko "$T/root/roms"
# Shader, then Down x11 to "Button 1" (Screen size, Aspect, Smooth, Scanlines, FPS, Sound, Volume, FF, Rewind, High scores, Button layout, Button 1)
P="0:0;30:$TRIANGLE;32:0"; t=40
for i in $(seq 1 12); do P="$P;$t:$DOWN;$((t + 2)):0"; t=$((t + 6)); done
P="$P;$t:$RIGHT;$((t + 2)):0;$((t + 10)):$CIRCLE;$((t + 12)):0;$(SHELFQUIT_AT $((t + 30)))"
rc=$(run "$T" "$P" "")
expect "grep -q '^layout=3$' $T/root/fbneo-ps5.ini && grep -q '^btn_1=1$' $T/root/fbneo-ps5.ini && grep -q '^btn_2=1$' $T/root/fbneo-ps5.ini" "the settings screen made a custom layout from the classic one (button 1: Cross -> Circle)"
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
APP=$T/homebrew/PPSA99012
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
expect "grep -q '\"titleId\": \"PPSA99012\"' $APP/sce_sys/param.json && grep -q 'FBNeo PS5' $APP/sce_sys/param.json" "the title is PPSA99012, FBNeo PS5"
expect "grep -q 'FBNeo PS5 [0-9.]* installed. Open it from the FBNeo PS5 icon' $T/inst.txt" "install notification says to open the icon"
expect "! ls $APP/*.part $APP/sce_sys/*.part $APP/sce_module/*.part 2>/dev/null | grep -q ." "no .part files left"
inst "$T"
expect "grep -q 'is up to date' $T/root/logs/installer.log && ! grep -q 'wrote' $T/root/logs/installer.log" "sent again: nothing rewritten"
expect "grep -q 'a helper is already running' $T/root/logs/installer.log" "sent again: the second copy leaves the helper to the first"
echo broken >$APP/eboot.bin
inst "$T"
expect "cmp -s $APP/eboot.bin tests/fake-eboot.bin" "a damaged eboot.bin is put back"
META=$T/appmeta/PPSA99012
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
# shelf order: 1942, Pac-Man, Ponpoko, Puck Man
rc=$(OFFLINE= COVER_URL="http://127.0.0.1:$PORT/\${repo}/\${kind}/\${name}.png" FBNEO_HOST_REALTIME=1 run "$T" \
	"0:0;200:$RIGHT;202:0;250:$RIGHT;252:0;300:$RIGHT;302:0;$(SHELFQUIT_AT 340)" "190,240,290,330")
kill $SRVPID 2>/dev/null
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "cmp -s '$T/root/covers/FBNeo/Puck Man (Japan, set 1).png' '$T/srv/$REPO/Named_Boxarts/Puck Man (Japan, set 1).png'" "Puck Man's flyer saved in covers/FBNeo"
expect "cmp -s '$T/root/covers/FBNeo/Pac-Man (Midway).png' '$T/srv/$REPO/Named_Boxarts/Puck Man (Japan, set 1).png'" "Pac-Man (a clone with no flyer) gets its parent's"
expect "cmp -s '$T/root/covers/FBNeo/Ponpoko.png' '$T/srv/$REPO/Named_Snaps/Ponpoko.png'" "Ponpoko (no flyer) gets its screenshot"
expect "ls $T/root/covers/FBNeo/ | grep -q '^1942.*\.missing$'" "1942: nothing on the server, remembered (.missing)"
expect "$CHECK $T/dump/flip00240.ppm 960 420 255 0 0 >/dev/null" "Pac-Man shows the red flyer"
expect "$CHECK $T/dump/flip00290.ppm 960 420 0 255 0 >/dev/null" "Ponpoko shows the green screenshot"
nosan "$T"
fi

if want 11; then
echo "== 11. the shelf's tabs: Down shows one maker; the game picked there runs; settings hide the clones"
T=$(newroot t11)
addset pacman "$T/root/roms"
mkset 1942
addset 1942 "$T/root/roms"
# tabs: All games, Capcom (1942), Classics (Pac-Man, Puck Man)
rc=$(run "$T" "0:0;30:$DOWN;32:0;40:$DOWN;42:0;60:$CROSS;62:0;$(QUITAT 160)" "25,55,150")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '\[shelf\] tab Capcom: 1 game(s)' $T/root/logs/boot.log && grep -q '\[shelf\] tab Classics: 2 game(s)' $T/root/logs/boot.log" "Down: Capcom (1 game), Down: Classics (2 games)"
expect "grep -q 'loading .*pacman.zip' $T/root/logs/boot.log" "the first Classics game was started from its tab"
expect "grep -q 'shelf_family=11' $T/root/fbneo-ps5.ini" "the tab is remembered"
echo "show_clones=0" >>"$T/root/fbneo-ps5.ini"
rc=$(run "$T" "0:0;30:$CROSS;32:0;$(QUITAT 100)" "")
expect "grep -q 'loading .*puckman.zip' $T/root/logs/boot.log" "clones hidden: the Classics tab starts with Puck Man"
nosan "$T"
fi

if want 12; then
echo "== 12. CRT shaders: CRT Easymode style by default, every shader draws a vertical and a horizontal game"
SHADERS=("Off" "CRT Easymode style" "crt-lottes" "crt-lottes-fast" "crt-1tap" "crt-2tap" "crt-hyllian-fast" "crt-nobody" "newpixie-mini" "crt-blurPi-sharp" "crt-blurPi-soft" "monoCRT")
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
for s in $(seq 1 11); do
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
echo "== 13. settings from the shelf (Triangle); fast forward (R2) and rewind (L2 + R2); the pause menu saves a state"
T=$(newroot t13)
addset ponpoko "$T/root/roms"
rc=$(FBNEO_HOST_REALTIME=1 run "$T" "0:0;30:$TRIANGLE;32:0;36:$DOWN;38:0;40:$RIGHT;42:0;50:$CIRCLE;52:0;70:$CROSS;72:0;150:$R2;250:0;300:$L2R2;340:0;360:$L3R3;362:0;370:$DOWN;372:0;380:$CROSS;382:0;400:$CIRCLE;402:0;$(QUITAT 440)" "45")
expect "[ $rc = 0 ]" "exit code 0 (got $rc)"
expect "grep -q '^scale=1$' $T/root/fbneo-ps5.ini" "the settings screen changed the screen size"
expect "grep -q 'fast forward on' $T/root/logs/boot.log && grep -q 'fast forward off' $T/root/logs/boot.log" "R2 held: fast forward"
expect "grep -q 'rewind on' $T/root/logs/boot.log && grep -q 'rewind off' $T/root/logs/boot.log" "L2 + R2 held: rewind"
expect "[ -f '$T/root/states/ponpoko.state1' ]" "the pause menu's Save state wrote slot 1"
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
echo "== 18. covers as PS5SX2: prefetched before asking for /data; new games restart the app to fetch them"
stop_helpers
T=$(newroot t18)
SRV=$T/srv/FBNeo_-_Arcade_Games/Named_Boxarts; mkdir -p "$SRV" "$T/root/covers"
python3 -c "from PIL import Image; Image.new('RGB',(512,700),(255,0,0)).save('$SRV/Ponpoko.png')"
addset ponpoko "$T/root/roms"
PORT=18091
(cd "$T/srv" && exec python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1) &
SRVPID=$!
FBNEO_PS5_ROOT=$T/root ASAN_OPTIONS=detect_leaks=0 timeout 120 "$HELPER" >"$T/helper.txt" 2>&1 &
waitfor "$T/root/logs/helper.log" "listening" || sleep 1
URL="http://127.0.0.1:$PORT/\${repo}/\${kind}/\${name}.png"
SHELFQUIT="0:0;$(SHELFQUIT_AT 30)"
rc=$(OFFLINE= COVER_URL="$URL" run "$T" "$SHELFQUIT" "")
expect "grep -q '^FBNeo/Ponpoko.png	' $T/root/covers/wanted.txt" "first start: the missing cover goes to covers/wanted.txt"
expect "grep -q 'restarting .* so the prefetch gets them' $T/root/logs/boot*.log" "first start: the app restarts itself for the new cover"
rm -f "$T/root/covers/restart.stamp"
rc=$(OFFLINE= COVER_URL="$URL" run "$T" "$SHELFQUIT" "")
expect "[ $rc = 0 ]" "second start: exit code 0 (got $rc)"
expect "cmp -s '$T/root/covers/FBNeo/Ponpoko.png' '$SRV/Ponpoko.png'" "second start: the cover was prefetched and saved"
expect "awk '/\[prefetch\] 1 of 1 fetched/{p=NR} /\[jailbreak\] pid/{j=NR} END{exit !(p && j && p<j)}' $T/root/logs/boot.log" "the download happened before the request for /data"
expect "[ ! -s $T/root/covers/wanted.txt ]" "second start: the wanted list is empty"
kill $SRVPID 2>/dev/null
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
# Settings (Triangle) -> Debug logs (Up twice from Shader: Back, then Debug logs) -> Off
T=$(newroot t20b)
addset ponpoko "$T/root/roms"
rc=$(run "$T" "0:0;30:$TRIANGLE;32:0;50:$UP;52:0;60:$UP;62:0;70:$CROSS;72:0;90:$CIRCLE;92:0;$(SHELFQUIT_AT 120)" "")
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

echo
echo "passed $PASS, failed $FAIL  (work dir $WORK)"
[ $FAIL = 0 ]
