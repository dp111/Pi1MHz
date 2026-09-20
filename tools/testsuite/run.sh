#!/bin/bash
# run.sh - drive the Pi1MHz test suite from the host.  See README.md.
#
#   run.sh [--build] [--upload] [--only A,B] [--skip A,B] [--keep-notube] [--no-restore]
#
# Needs: the Pico keyboard on COM9 (claude-tmp/beeb-run.sh), the Pi on WiFi
# (claude-tmp/pi-http.sh, PI_IP), and the test disc uploaded to /BeebSCSI$JUKE.
# After a CTRL-BREAK the Master is in its CMOS filing system (VFS on the bench),
# so the disc is re-mounted with *ADFS; the jukebox set itself survives a BREAK.
# Assumes nothing about the Master: it CTRL-BREAKs, arms the serial echo,
# fixes CAPS, turns the co-processor off (*CONFIGURE NOTUBE) and selects the
# test jukebox set itself.  It restores the Tube setting and jukebox set 0 at
# the end unless told not to, and says what it changed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=${PI1MHZ_TOOLS:-/mnt/c/Archlinux/claude-tmp}
export PI_IP=${PI_IP:-192.168.0.42}
JUKE=${JUKE:-9}
M5000_FX=${M5000_FX:-3}      # Music 5000's index in the emulator table (see TM5000)
NSLOOK_HOST=${NSLOOK_HOST:-www.google.com}   # *NSLOOK target; needs DNS, skips without it
JUKE_RESTORE=${JUKE_RESTORE:-0}
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"
LOG=$OUT/run-$(date +%Y%m%d-%H%M%S).log
RES=$OUT/results.txt
: > "$RES"
DO_BUILD=0 DO_UPLOAD=0 ONLY="" SKIP="" KEEP_NOTUBE=0 RESTORE=1
while [ $# -gt 0 ]; do
  case $1 in
    --build) DO_BUILD=1 DO_UPLOAD=1;;
    --upload) DO_UPLOAD=1;;
    --only) ONLY=$2; shift;;
    --skip) SKIP=$2; shift;;
    --keep-notube) KEEP_NOTUBE=1;;
    --no-restore) RESTORE=0;;
    *) echo "unknown option $1"; exit 2;;
  esac; shift
done
BASIC_TESTS="TINFO TFRED TRAM TDISC TVDU TNET TM5000 TTTX TAUN TBURST TFAT TFATJ"
HOST_TESTS="MOUSE WAV REDIR VIDEO VFS STRESS MMFS ROM WIFI BREAK"
TUBE_CHANGED=0
CHANGES=()

log() { echo "$*" | tee -a "$LOG"; }
die() { log "ABORT: $*"; exit 1; }
want() { # want TEST -> 0 if the test is selected
  local t=$1
  [ -n "$ONLY" ] && { [[ ",$ONLY," == *",$t,"* ]] || return 1; }
  [ -n "$SKIP" ] && { [[ ",$SKIP," == *",$t,"* ]] && return 1; }
  return 0
}
result() { echo "$*" >> "$RES"; log "  $*"; }

# ---- Beeb over COM9 -------------------------------------------------------
TEXT="" MATCH=""
send() { # send <beeb-run.ps1 args>; fills TEXT (echo) and MATCH (yes/no)
  local out
  out=$("$TMP/beeb-run.sh" "$@" 2>&1 | tr -d '\r')
  MATCH=$(printf '%s\n' "$out" | sed -n 2p | awk '{print $2}')
  TEXT=$(printf '%s\n' "$out" | tail -n +3)
  { echo "--- send $*"; printf '%s\n' "$out"; } >> "$LOG"
}
lines() { send -Lines "$1" -Until "${2:-}" -Timeout "${3:-5000}"; }
key() { send -Hex "$1" -Timeout "${2:-1500}"; }
escape_key() { key 1B0D 1500; }    # Pico: ESC + unknown byte = Escape key, then Return

# ---- Pi over HTTP ---------------------------------------------------------
pi_up() { "$TMP/pi-status.sh" 'Boot' > /dev/null 2>&1; }
pi_row() { "$TMP/pi-status.sh" "$1" 2>/dev/null; }
pi_get() { "$TMP/pi-http.sh" -s --max-time "${2:-30}" "$1"; }
bmp_hash() { pi_get /framebuffer.bmp 60 | md5sum | cut -c1-32; }
break_inits() { pi_row BREAK | sed -n 's/.*inits \([0-9]*\).*/\1/p'; }
bus_ovr() { pi_row 'Bus diag' | sed -n 's/.*ovr \([0-9]*\).*/\1/p'; }

ctrl_break() { # only against a Pi that has just answered /status
  pi_up || die "Pi not answering /status; refusing to BREAK the Beeb"
  key 1B5B76 3000
  sleep 2
}
arm_echo() { # F11 arms the Pico's *FX8,8 + *FX3,1 redirect; then fix CAPS
  key 1B5B57 3000
  lines 'a' 'Mistake' 4000
  [ -n "$TEXT" ] || die "no echo from the Beeb after F11 (Pico wedged, or Beeb not at a prompt)"
  case "$TEXT" in A*) key 1B41 1000; log "CAPS was on: sent ALT+A";; esac
}
beeb_page() { lines 'PRINT ~PAGE' '\n *[0-9A-F]+\n' 4000; printf '%s\n' "$TEXT" | sed -n 's/^ *\([0-9A-F]\{3,4\}\)$/\1/p' | tail -1; }
ensure_notube() {
  local p; p=$(beeb_page)
  log "PAGE=&$p"
  [ "$p" = "800" ] || return 0
  log "co-processor is on: *CONFIGURE NOTUBE + CTRL-BREAK"
  lines '*CONFIGURE NOTUBE' '' 2000
  TUBE_CHANGED=1; CHANGES+=("*CONFIGURE NOTUBE (Tube was on)")
  ctrl_break; arm_echo
  p=$(beeb_page); [ "$p" != "800" ] || die "still on the Tube after *CONFIGURE NOTUBE"
}
mount_test_disc() {
  # *FX147,65 changes the SCSI or the VFS set depending on which ROM last
  # selected the Pi, so make an ADFS access right before the poke.
  lines '*ADFS|*CAT' 'Option' 15000
  lines "*BYE|*FX147,65,$JUKE" '' 2000; sleep 1   # the swap remounts the FAT asynchronously
  lines '*MOUNT 0|*CAT' 'Option' 15000
  printf '%s\n' "$TEXT" | grep -q PI1MHZTEST || die "test disc not mounted (no PI1MHZTEST in *CAT):"$'\n'"$TEXT"
  [[ " ${CHANGES[*]:-} " == *"set $JUKE selected"* ]] || CHANGES+=("jukebox set $JUKE selected")
}
upload_disc() {
  local dir=/BeebSCSI$JUKE
  "$TMP/pi-http.sh" -s -o /dev/null -X MKCOL "$dir/" || true
  for f in scsi0.dat; do
    local size; size=$(stat -c %s "$OUT/$f")
    "$TMP/pi-http.sh" -s -o /dev/null -H 'Expect:' --max-time 300 -T "$OUT/$f" "$dir/$f" || die "PUT $f failed"
    local got; got=$("$TMP/pi-http.sh" -s -I --max-time 20 "$dir/$f" | tr -d '\r' | sed -n 's/^[Cc]ontent-[Ll]ength: //p' | tail -1)
    [ "$got" = "$size" ] || die "$dir/$f is $got bytes after PUT, expected $size"
  done
  log "uploaded $dir/scsi0.dat ($size bytes)"
}

# ---- BASIC tests ----------------------------------------------------------
blind_probe() { # the echo went silent: is the Beeb hung, or only the Pico's echo lost?
  # Type a command with a Pi-visible side effect (start+stop the WAV recorder)
  # and look for the file: a hung Beeb (SCSI handshake, IRQs off) cannot run it.
  local before after; before=$(list_wavs)
  lines '*FX147,202,3|*FX147,203,1' '' 1500; sleep 1
  lines '*FX147,202,3|*FX147,203,0' '' 1500; sleep 3
  after=$(list_wavs)
  local new; new=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
  if [ -n "$new" ]; then
    result "I:$1:silence:Beeb alive, Pico echo lost (blind *FX147 made $new)"
    "$TMP/pi-http.sh" -s -o /dev/null -X DELETE "$new"
  else
    result "I:$1:silence:Beeb not executing commands (hung) - BREAK row: $(pi_row BREAK | cut -c1-60)"
  fi
}
parse_results() { printf '%s\n' "$TEXT" | grep -E '^(T|I|S):' | while IFS= read -r l; do result "$l"; done; }
run_basic() {
  local t=$1 timeout=$2 pre="" post=""
  log "== $t"
  case $t in
    TVDU)  pre=$(bmp_hash);;
    TNET)  local url; url=${NET_URL:-HTTP://$(pi_row Gateway | awk '{print $2}')/}
           lines "\$&7000=\"$url\"" '' 2000;;
    TRAM|TDISC|TBURST|TFAT|TFATJ) pre=$(bus_ovr);;
    TM5000) pre=$(list_wavs);;
  esac
  lines "Z%=0:CHAIN\"$t\"" '##DONE' "$timeout"
  parse_results
  if [ "$MATCH" != "yes" ]; then
    local err; err=$(printf '%s\n' "$TEXT" | grep -m1 -E ' at line [0-9]+|^(Mistake|Syntax error|No such|Bad )')
    if [ -n "$err" ]; then
      result "T:${t#T}:completed:FAIL:program error: $err"
    else
      result "T:${t#T}:completed:FAIL:no ##DONE within ${timeout} ms"
      escape_key
      [ -z "$TEXT" ] && blind_probe "${t#T}"
    fi
  fi
  case $t in
    TVDU)  post=$(bmp_hash); [ "$post" != "$pre" ] && result "T:VDU:framebuffer changed:PASS" || result "T:VDU:framebuffer changed:FAIL:$pre == $post";;
    TDISC) pi_get "/BeebSCSI$JUKE/scsi0.dat" 180 > "$OUT/scsi0-after.dat"
           if python3 "$HERE/build.py" verify-wrote "$OUT/scsi0-after.dat" >> "$LOG" 2>&1; then result "T:DISC:WROTE reached the image:PASS"; else result "T:DISC:WROTE reached the image:FAIL:see log"; fi;;
    TM5000) sleep 2; post=$(list_wavs); local new n w1 w2 r
            new=$(comm -13 <(echo "$pre") <(echo "$post")); n=$(printf '%s\n' "$new" | grep -c wav)
            if [ "$n" -ge 2 ]; then
              w1=$(printf '%s\n' "$new" | sed -n 1p); w2=$(printf '%s\n' "$new" | sed -n 2p)
              pi_get "$w1" 60 > "$OUT/m5000-tone.wav"; pi_get "$w2" 60 > "$OUT/m5000-silence.wav"
              if r=$(python3 "$HERE/wavcheck.py" "$OUT/m5000-tone.wav" 1000 2>&1); then result "T:M5000:1 kHz tone in the recording:PASS"; else result "T:M5000:1 kHz tone in the recording:FAIL:$r"; fi
              result "I:M5000:tone wav:$r"
              if r=$(python3 "$HERE/wavcheck.py" "$OUT/m5000-silence.wav" 0 2>&1); then result "T:M5000:silence after amp 0:PASS"; else result "T:M5000:silence after amp 0:FAIL:$r"; fi
              result "I:M5000:silence wav:$r"
              for w in $new; do "$TMP/pi-http.sh" -s -o /dev/null -X DELETE "$w"; done
            else result "T:M5000:two recordings made:FAIL:$n new wav(s)"; fi;;
  esac
  case $t in TRAM|TDISC|TBURST|TFAT|TFATJ) post=$(bus_ovr); [ "$post" = "$pre" ] && result "T:${t#T}:no bus overruns:PASS" || result "T:${t#T}:no bus overruns:FAIL:ovr $pre -> $post";; esac
  result "I:${t#T}:bus:$(bus_ovr) ovr"
  pi_up || die "Pi stopped answering after $t"
}

# ---- host-driven tests ----------------------------------------------------
t_help() { # helper 0: the help screen (ends in a BRK, so it cannot run inside a program)
  log "== HELP"
  lines '*FX147,136,0' '' 1500
  lines '*GO FD00' 'M5000 record' 15000
  local v; v=$(printf '%s\n' "$TEXT" | grep -m1 '^Pi1MHz V')
  [ -n "$v" ] && result "T:INFO:helper 0 help screen:PASS" || result "T:INFO:helper 0 help screen:FAIL:$(printf '%s' "$TEXT" | tail -2 | tr '\n' ' ')"
  result "I:INFO:firmware:$v $(printf '%s\n' "$TEXT" | grep -m1 '^Built')"
  result "I:INFO:pi:$(printf '%s\n' "$TEXT" | grep -m1 '^Pi [0-9a-f]')"
  result "I:INFO:bus:$(bus_ovr) ovr"
}
t_mouse() {
  log "== MOUSE"
  local h0 h1 h2
  h0=$(bmp_hash)
  lines '?&FCAC=100:?&FCAD=1:?&FCAE=44:?&FCAF=1' '' 1000; sleep 1; h1=$(bmp_hash)
  lines '?&FCAF=&41' '' 1000; sleep 1; h2=$(bmp_hash)
  [ "$h1" != "$h0" ] && result "T:MOUSE:pointer on changes screen:PASS" || result "T:MOUSE:pointer on changes screen:FAIL"
  [ "$h2" != "$h1" ] && result "T:MOUSE:pointer off changes screen:PASS" || result "T:MOUSE:pointer off changes screen:FAIL"
  result "I:MOUSE:bus:$(bus_ovr) ovr"
}
list_wavs() { "$TMP/pi-http.sh" -s --max-time 20 -X PROPFIND -H 'Depth: 1' / | grep -o '<D:href>/Musics[0-9]*\.wav' | sed 's/<D:href>//' | sort; }
t_wav() {
  log "== WAV"
  local before after new
  before=$(list_wavs)
  lines '*FX147,202,3|*FX147,203,1' '' 1500; sleep 1.5
  lines '*FX147,202,3|*FX147,203,0' '' 1500; sleep 3
  after=$(list_wavs)
  new=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
  if [ -n "$new" ]; then
    local size; size=$("$TMP/pi-http.sh" -s -I --max-time 20 "$new" | tr -d '\r' | sed -n 's/^[Cc]ontent-[Ll]ength: //p' | tail -1)
    result "I:WAV:file:$new $size bytes"
    [ "${size:-0}" -gt 44 ] && result "T:WAV:recorder wrote a wav:PASS" || result "T:WAV:recorder wrote a wav:FAIL:size $size"
    "$TMP/pi-http.sh" -s -o /dev/null -X DELETE "$new" && log "  deleted $new"
  else
    result "T:WAV:recorder wrote a wav:FAIL:no new Musics*.wav in the SD root"
  fi
  result "I:WAV:bus:$(bus_ovr) ovr"
}
t_vfs() {
  log "== VFS"
  local set; set=${VFS_SET:-$("$TMP/pi-http.sh" -s --max-time 20 -X PROPFIND -H 'Depth: 1' / | grep -o '<D:href>/BeebVFS[0-9]*' | sed 's/.*BeebVFS//' | sort -n | head -1)}
  [ -n "$set" ] || { result "T:VFS:mount:SKIP:no /BeebVFSn on the card"; return; }
  result "I:VFS:set:$set"
  lines '*BYE' '' 2000
  lines "*FX147,65,$set" '' 2000
  lines '*VFS|*MOUNT 0|*CAT' '\n>' 20000
  printf '%s\n' "$TEXT" | grep -qiE 'Option|Drive|Dir' && result "T:VFS:mount and *CAT:PASS" || result "T:VFS:mount and *CAT:FAIL:$(printf '%s' "$TEXT" | tail -3 | tr '\n' ' ')"
  lines '*FCODE ?T' '\n>' 8000
  local fc; fc=$(pi_row 'F-code')
  printf '%s' "$fc" | grep -q 'tx ?T' && result "T:VFS:fcode ?T reached the Pi:PASS" || result "T:VFS:fcode ?T reached the Pi:FAIL:$fc"
  result "I:VFS:fcode row:${fc#F-code: }"
  result "I:VFS:video player:$(pi_row 'Video player' | cut -c1-80)"
  lines '*ADFS|*CAT' 'Option' 15000
  lines "*BYE|*FX147,65,$JUKE" '' 2000
  lines '*MOUNT 0|*CAT' 'Option' 15000
  printf '%s\n' "$TEXT" | grep -q PI1MHZTEST && result "T:VFS:back to the test disc:PASS" || result "T:VFS:back to the test disc:FAIL"
  result "I:VFS:bus:$(bus_ovr) ovr"
}
t_wifi() { # helper 16 = the shipped ROM (1MHz-WiFi with WiCFS merged in)
  log "== WIFI"
  local ip; ip=$(pi_row 'IP address' | sed -n 's/^IP address: //p')
  lines '*ROMS' 'ROM 0' 10000
  if ! printf '%s\n' "$TEXT" | grep -qi '1MHz-Wi'; then
    lines '*FX147,136,16' '' 2000; sleep 2
    lines '*GO FD00' 'No SWR|No ROM' 45000     # the loader prints no prompt the echo can see
    case "$TEXT" in
      *"No ROM"*)
        result "T:WIFI:helper 16 load:SKIP:No ROM - /Pi1MHz/1mhz-wicfs.rom is not on the card"
        mount_test_disc; return;;
      *"No SWR"*)
        result "T:WIFI:helper 16 load:SKIP:No SWR - sideways RAM is full, power-cycle the Beeb"
        mount_test_disc; return;;
    esac
    ctrl_break; arm_echo
    lines '*ROMS' 'ROM 0' 10000
  fi
  local rom; rom=$(printf '%s\n' "$TEXT" | grep -m1 -i '1MHz-Wi')
  [ -n "$rom" ] && result "T:WIFI:ROM in *ROMS after load + BREAK:PASS" || {
      result "T:WIFI:ROM in *ROMS after load + BREAK:FAIL"; mount_test_disc; return; }
  result "I:WIFI:roms line:$rom"

  # The extended vector table. The ROM used to keep its workspace at &0D90,
  # which runs into it at &0D9F, and the machine then died on the next OS
  # call made through a claimed vector - after the command had finished.
  lines 'FORI%=&D9F TO &DAF:PRINT~?I%;" ";:NEXT:PRINT' '>' 15000
  local before; before=$(printf '%s\n' "$TEXT" | tr -s ' ' | grep -E '^ *[0-9A-F]+ ' | tail -1)

  lines '*VERSION' '>' 25000
  printf '%s\n' "$TEXT" | grep -q '1MHz-WiFi' && result "T:WIFI:*VERSION answers:PASS" \
                                               || result "T:WIFI:*VERSION answers:FAIL"
  # the firmware's own reply only comes back when the service is enabled
  if printf '%s\n' "$TEXT" | grep -q 'kernel V'; then
    result "I:WIFI:service:$(printf '%s\n' "$TEXT" | grep -m1 'kernel V')"
  else
    result "I:WIFI:service:no firmware reply - wifi_service_enable=1 not set in Pi1MHz.cfg?"
  fi

  lines 'FORI%=&D9F TO &DAF:PRINT~?I%;" ";:NEXT:PRINT' '>' 15000
  local after; after=$(printf '%s\n' "$TEXT" | tr -s ' ' | grep -E '^ *[0-9A-F]+ ' | tail -1)
  [ -n "$before" ] && [ "$before" = "$after" ] \
      && result "T:WIFI:extended vector table unchanged by a command:PASS" \
      || result "T:WIFI:extended vector table unchanged by a command:FAIL:$before -> $after"

  # Alive check that does not depend on the echo.  This is the fault that has
  # to be caught: when the ROM corrupted the extended vectors the machine
  # printed its reply and THEN stopped executing, so a test that only reads
  # the echo calls that a pass.
  #
  # It has to be a side effect only the Beeb can cause.  The Pi's framebuffer
  # is no good - the screen redirector mirrors Beeb output, so the picture
  # changes whether or not this particular command ran - and *FCODE needs VFS
  # selected.  Starting and stopping the Music 5000 recorder writes a file to
  # the card, which is unambiguous and works under any filing system.
  local w0 w1 wnew
  w0=$(list_wavs)
  lines "?&FCCA=$M5000_FX:?&FCCB=1" '>' 10000
  lines "?&FCCA=$M5000_FX:?&FCCB=0" '>' 10000
  sleep 3
  w1=$(list_wavs); wnew=$(comm -13 <(echo "$w0") <(echo "$w1") | head -1)
  if [ -n "$wnew" ]; then
    result "T:WIFI:machine still executes after a command:PASS"
    "$TMP/pi-http.sh" -s -o /dev/null -X DELETE "$wnew"
  else
    result "T:WIFI:machine still executes after a command:FAIL:the Beeb no longer reaches the bus"
  fi

  lines '*ONLINE' '>' 30000
  if [ -n "$ip" ] && printf '%s\n' "$TEXT" | grep -q "$ip"; then
    result "T:WIFI:*ONLINE reports the Pi's address:PASS"
  else
    result "T:WIFI:*ONLINE reports the Pi's address:SKIP:no address in the reply (service off?)"
  fi
  local gw; gw=$(pi_row 'Gateway' | sed -n 's/^Gateway: //p')
  if [ -n "$gw" ]; then
    lines "*PING $gw" '>' 40000
    printf '%s\n' "$TEXT" | grep -q 'Received response' && result "T:WIFI:*PING the gateway:PASS" \
                                                        || result "T:WIFI:*PING the gateway:SKIP:no response line"
  fi

  # The RAM disk: self-contained, no network, so it is the part of this ROM
  # that can be checked properly.  Write a pattern, save it, catalogue it,
  # load it back somewhere else and compare.
  # The RAM disc can be built out (INCLUDE_RAMDISK=0, to make room for a
  # second ROM in the bank), so ask the ROM before testing it.
  lines '*HELP WIFI' '>' 20000
  if ! printf '%s\n' "$TEXT" | grep -qi 'RDCAT'; then
    result "T:WIFI:RAM disk:SKIP:this ROM was built without it"
  else
  lines 'FORI%=0 TO 15:?(&2000+I%)=I%+65:NEXT:?&3000=0' '>' 15000
  lines '*RDINIT' '>' 15000
  lines '*RDSAVE WTEST 2000 2010' '>' 20000
  lines '*RDCAT' '>' 20000
  printf '%s\n' "$TEXT" | grep -qi 'WTEST' && result "T:WIFI:RAM disk *RDCAT lists the saved file:PASS" \
                                            || result "T:WIFI:RAM disk *RDCAT lists the saved file:FAIL"
  lines 'FORI%=0 TO 15:?(&3000+I%)=0:NEXT' '>' 15000
  lines '*RDLOAD WTEST 3000' '>' 20000
  lines 'P.;:FORI%=0 TO 15:P.~?(&3000+I%);:NEXT:P.' '>' 20000
  local got; got=$(printf '%s\n' "$TEXT" | tr -d ' ' | grep -oE '414243444546474849[0-9A-F]*' | tail -1)
  [ -n "$got" ] && result "T:WIFI:RAM disk round trip (save, load elsewhere, compare):PASS" \
                || result "T:WIFI:RAM disk round trip (save, load elsewhere, compare):FAIL:$(printf '%s' "$TEXT" | tail -2 | tr -d '\n')"
  fi

  # A name lookup and a fetch, both against the LAN so no internet is needed.
  # Either can legitimately be unavailable, so neither failure is fatal.
  local gw; gw=$(pi_row 'Gateway' | sed -n 's/^Gateway: //p')
  lines "*NSLOOK $NSLOOK_HOST" '>' 40000
  printf '%s\n' "$TEXT" | grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' \
      && result "T:WIFI:*NSLOOK resolves $NSLOOK_HOST:PASS" \
      || result "T:WIFI:*NSLOOK resolves $NSLOOK_HOST:SKIP:no address - no DNS on this network?"
  if [ -n "$gw" ]; then
    lines "*WGET http://$gw/" '>' 45000
    printf '%s\n' "$TEXT" | grep -qiE 'not found|error|refused|failed' \
        && result "T:WIFI:*WGET the gateway:SKIP:$(printf '%s' "$TEXT" | grep -im1 -oE 'not found|error|refused|failed')" \
        || result "T:WIFI:*WGET the gateway:PASS"
  fi
  # *LAP rescans on the radio that is carrying this session, so it is only
  # run when asked for.  *JOIN is never run here: it would re-associate the
  # Pi and take the link this suite is talking over with it.
  if [ "${WIFI_SCAN:-0}" = 1 ]; then
    lines '*LAP' '>' 60000
    printf '%s\n' "$TEXT" | grep -qE '[0-9]' && result "T:WIFI:*LAP scan returns:PASS" \
                                              || result "T:WIFI:*LAP scan returns:SKIP:no scan output"
  else
    result "I:WIFI:scan:*LAP skipped (WIFI_SCAN=1 to run it); *JOIN is never run, it would drop the link"
  fi

  # WiCFS, in the merged image only: *WGET -U leaves a normalised UEF in the
  # JIM window and the filing system streams it back out as if it were tape.
  lines '*HELP WIFI' '>' 20000
  if printf '%s\n' "$TEXT" | grep -qiE 'WICFS|UEF'; then
    lines '*UEF' '>' 20000
    printf '%s\n' "$TEXT" | grep -qiE 'syntax|usage|filename' \
        && result "T:WIFI:WiCFS *UEF answers:PASS" \
        || result "T:WIFI:WiCFS *UEF answers:FAIL:$(printf '%s' "$TEXT" | tail -1)"
  else
    result "T:WIFI:WiCFS:SKIP:this image has no cassette filing system"
  fi

  result "I:WIFI:bus:$(bus_ovr) ovr"
  mount_test_disc
}

t_rom() { # helper 6 = BSRom into sideways RAM, then a BREAK registers it
  log "== ROM"
  lines '*ROMS' 'ROM 0' 10000
  if printf '%s\n' "$TEXT" | grep -q 'BeebSCSI'; then
    result "I:ROM:note:BeebSCSI utilities already resident, helper load skipped"
  else
    lines '*FX147,136,6' '' 2000; sleep 2
    lines '*GO FD00' 'No SWR|No ROM' 45000     # ~20 s; the loader prints no prompt the echo can see
    printf '%s\n' "$TEXT" | grep -qE 'No SWR|No ROM' && { result "T:ROM:helper load:FAIL:$(printf '%s' "$TEXT" | grep -E 'No SWR|No ROM')"; return; }
    ctrl_break; arm_echo
  fi
  lines '*ROMS' 'ROM 0' 10000
  local rom; rom=$(printf '%s\n' "$TEXT" | grep -m1 'BeebSCSI')
  [ -n "$rom" ] && result "T:ROM:BeebSCSI utilities in *ROMS after load + BREAK:PASS" || result "T:ROM:BeebSCSI utilities in *ROMS after load + BREAK:FAIL"
  result "I:ROM:roms line:$rom"
  mount_test_disc
  result "I:ROM:bus:$(bus_ovr) ovr"
}
t_break() {
  log "== BREAK"
  local n=5 i0 i1 o0 o1 row
  i0=$(break_inits); o0=$(bus_ovr)
  for i in $(seq $n); do ctrl_break; done
  arm_echo
  i1=$(break_inits); o1=$(bus_ovr); row=$(pi_row BREAK)
  result "I:BREAK:row:${row#BREAK: }"
  [ "$((i1 - i0))" = "$n" ] && result "T:BREAK:$n inits counted:PASS" || result "T:BREAK:$n inits counted:FAIL:inits $i0 -> $i1"
  printf '%s' "$row" | grep -q 'helper [0-9]' && result "T:BREAK:rom selected the helper:PASS" || result "T:BREAK:rom selected the helper:FAIL:$row"
  [ "$o1" = "$o0" ] && result "T:BREAK:no bus overruns:PASS" || result "T:BREAK:no bus overruns:FAIL:ovr $o0 -> $o1"
  lines '*ADFS|*BYE' '' 3000
  lines "*FX147,65,$JUKE" '' 2000
  lines '*MOUNT 0|*CAT' 'Option' 15000
  printf '%s\n' "$TEXT" | grep -q PI1MHZTEST && result "T:BREAK:test disc mounts after BREAK:PASS" || result "T:BREAK:test disc mounts after BREAK:FAIL"
}

vfs_sets() { "$TMP/pi-http.sh" -s --max-time 20 -X PROPFIND -H 'Depth: 1' / | grep -o '<D:href>/BeebVFS[0-9]*' | sed 's/.*BeebVFS//' | sort -n; }
vfs_video_set() { # first /BeebVFSn holding a video.pvf the firmware will play
  local s; for s in $(vfs_sets); do
    [ "$("$TMP/pi-http.sh" -s -o /dev/null -w '%{http_code}' -I --max-time 10 "/BeebVFS$s/video.pvf")" = 200 ] && { echo "$s"; return; }
  done
}
t_redir() { # helper 2: the screen redirector hooks the Beeb's VDU output onto the Pi screen
  log "== REDIR"
  lines '*FX147,136,2' '' 1500
  lines '*GO FD00' '>|No SWR|No ROM' 20000
  printf '%s\n' "$TEXT" | grep -qE 'No SWR|No ROM' && { result "T:REDIR:helper 2 load:FAIL:$(printf '%s' "$TEXT" | grep -E 'No SWR|No ROM')"; return; }
  local h0 h1; h0=$(bmp_hash)
  lines 'PRINT "PI1MHZ SCREEN REDIRECTOR TEST":PRINT STRING$(30,"#")' '' 3000; sleep 1; h1=$(bmp_hash)
  [ "$h1" != "$h0" ] && result "T:REDIR:beeb output reaches the Pi screen:PASS" || result "T:REDIR:beeb output reaches the Pi screen:FAIL"
  result "I:REDIR:bus:$(bus_ovr) ovr"
}
t_video() { # F-code frame seeks on a VFS set with video.pvf, judged by the Video player status row
  log "== VIDEO"
  local set; set=${VIDEO_SET:-$(vfs_video_set)}
  [ -n "$set" ] || { result "T:VIDEO:play:SKIP:no /BeebVFSn/video.pvf on the card"; return; }
  result "I:VIDEO:set:$set"
  lines '*BYE' '' 2000; lines "*FX147,65,$set" '' 2000
  lines '*VFS|*MOUNT 0' '>' 20000
  lines '*FCODE F1000R' '>' 8000; sleep 4
  local row; row=$(pi_row 'Video player'); result "I:VIDEO:row:${row#Video player: }"
  printf '%s' "$row" | grep -q 'pic 1000' && result "T:VIDEO:frame 1000 shown after F1000R:PASS" || result "T:VIDEO:frame 1000 shown after F1000R:FAIL:$(printf '%s' "$row" | cut -c15-70)"
  lines '*FCODE F1050R' '>' 8000; sleep 3
  row=$(pi_row 'Video player'); printf '%s' "$row" | grep -q 'pic 1050' && result "T:VIDEO:seek to 1050:PASS" || result "T:VIDEO:seek to 1050:FAIL:$(printf '%s' "$row" | cut -c15-70)"
  result "I:VIDEO:fcode row:$(pi_row 'F-code' | cut -c9-60)"
  lines '*ADFS|*CAT' 'Option' 15000; lines "*BYE|*FX147,65,$JUKE" '' 2000
  lines '*MOUNT 0|*CAT' 'Option' 15000
  printf '%s\n' "$TEXT" | grep -q PI1MHZTEST && result "T:VIDEO:back to the test disc:PASS" || result "T:VIDEO:back to the test disc:FAIL"
  result "I:VIDEO:bus:$(bus_ovr) ovr"
}
t_stress() { # RAM burst + DISC test + a CTRL-BREAK while the host hammers HTTP (9 MB snapshots, 2 MB uploads)
  log "== STRESS"
  local o0 o1 t f row lat; o0=$(bus_ovr)
  head -c 2097152 /dev/urandom > "$OUT/stress2m.bin"; rm -f "$OUT/stress.stop"
  ( while [ ! -f "$OUT/stress.stop" ]; do
      pi_get /framebuffer.bmp 60 > /dev/null
      "$TMP/pi-http.sh" -s -o /dev/null -H 'Expect:' --max-time 60 -T "$OUT/stress2m.bin" /Transfer/ts-stress.bin
    done ) & local bg=$!
  for t in TRAM TDISC; do
    lines "Z%=0:CHAIN\"$t\"" '##DONE' 240000
    f=$(printf '%s\n' "$TEXT" | grep -c ':FAIL')
    if [ "$MATCH" = yes ] && [ "$f" = 0 ]; then result "T:STRESS:$t under host load:PASS"; else result "T:STRESS:$t under host load:FAIL:done=$MATCH fails=$f"; fi
    [ "$MATCH" = yes ] || { escape_key; blind_probe STRESS; }
    result "I:STRESS:$t bus:$(bus_ovr) ovr"
  done
  ctrl_break; arm_echo
  row=$(pi_row BREAK); lat=$(printf '%s' "$row" | sed -n 's/.*rst->init \([0-9]*\) us.*/\1/p')
  result "I:STRESS:break row:${row#BREAK: }"
  [ "${lat:-99999}" -lt 100 ] && result "T:STRESS:reset latency under load < 100 us:PASS" || result "T:STRESS:reset latency under load < 100 us:FAIL:${lat:-?} us"
  local ini; ini=$(printf '%s' "$row" | sed -n 's/.*| rst->init [0-9]* us, init \([0-9]*\) us.*/\1/p')
  [ "${ini:-999999}" -lt 150000 ] && result "T:STRESS:re-init under load < 150 ms:PASS" || result "T:STRESS:re-init under load < 150 ms:FAIL:${ini:-?} us (ROM probes ~150 ms after release)"
  touch "$OUT/stress.stop"; wait $bg 2>/dev/null; rm -f "$OUT/stress.stop" "$OUT/stress2m.bin"
  "$TMP/pi-http.sh" -s -o /dev/null -X DELETE /Transfer/ts-stress.bin
  o1=$(bus_ovr)
  [ "$o1" = "$o0" ] && result "T:STRESS:no bus overruns:PASS" || result "T:STRESS:no bus overruns:FAIL:ovr $o0 -> $o1"
  mount_test_disc
}
t_mmfs() { # helper 5 = MMFS2 into sideways RAM (FAT service); *DIN a loose .ssd, save and delete on it
  log "== MMFS"
  lines '*ROMS' 'ROM 0' 10000
  if printf '%s\n' "$TEXT" | grep -q 'MMFS2'; then
    result "I:MMFS:note:MMFS2 already resident, helper load skipped"
  else
    lines '*FX147,136,5' '' 1500
    lines '*GO FD00' 'No SWR|No ROM' 45000     # ~20 s; the loader prints no prompt the echo can see
    printf '%s\n' "$TEXT" | grep -qE 'No SWR|No ROM' && { result "T:MMFS:helper 5 load:FAIL:$(printf '%s' "$TEXT" | grep -E 'No SWR|No ROM')"; return; }
    ctrl_break; arm_echo
    lines '*ROMS' 'ROM 0' 10000
    printf '%s\n' "$TEXT" | grep -q 'MMFS2' && result "T:MMFS:helper 5 loaded MMFS2 into sideways RAM:PASS" || result "T:MMFS:helper 5 loaded MMFS2 into sideways RAM:FAIL:$(printf '%s' "$TEXT" | grep -E 'ROM [4-7]' | tr '\n' ' ')"
  fi
  lines '*MMFS|*DIN 0 NET|*CAT' 'Option' 20000
  printf '%s\n' "$TEXT" | grep -q 'NETDEMO' && result "T:MMFS:*DIN NET + *CAT:PASS" || result "T:MMFS:*DIN NET + *CAT:FAIL:$(printf '%s' "$TEXT" | tail -3 | tr '\n' ' ')"
  lines '*SAVE TSAVE 3000 +100' '>' 15000
  pi_get /NET.ssd 60 > "$OUT/net-after.ssd"
  python3 "$HERE/build.py" dfs "$OUT/net-after.ssd" | grep -q 'TSAVE' && result "T:MMFS:save reached NET.ssd on the card:PASS" || result "T:MMFS:save reached NET.ssd on the card:FAIL"
  lines '*DELETE TSAVE' '>' 15000
  pi_get /NET.ssd 60 > "$OUT/net-after.ssd"
  python3 "$HERE/build.py" dfs "$OUT/net-after.ssd" | grep -q 'TSAVE' && result "T:MMFS:delete reached NET.ssd:FAIL" || result "T:MMFS:delete reached NET.ssd:PASS"
  lines '*ADFS' '' 2000
  mount_test_disc
  result "I:MMFS:bus:$(bus_ovr) ovr"
}

# ---- main -----------------------------------------------------------------
log "Pi1MHz test suite $(date) PI_IP=$PI_IP JUKE=$JUKE log=$LOG"
[ $DO_BUILD = 1 ] && { python3 "$HERE/build.py" build "$OUT" | tee -a "$LOG" || die "build failed"; }
pi_up || die "Pi not answering /status at $PI_IP"
result "I:PI:boot:$(pi_row 'Boot time' | cut -c1-80)"
result "I:PI:bus at start:$(pi_row 'Bus diag' | cut -c1-80)"
ctrl_break; arm_echo
ensure_notube
lines '*ADFS|*BYE' '' 3000
[ $DO_UPLOAD = 1 ] && upload_disc
mount_test_disc

for t in $BASIC_TESTS; do
  want "$t" || continue
  case $t in TDISC) to=240000;; TRAM|TNET|TM5000|TAUN) to=90000;; *) to=40000;; esac
  if [ "$t" = TM5000 ] && ! pi_row Audio | grep -q 'Music 5000'; then result "T:M5000:synth:SKIP:Music 5000 not enabled ($(pi_row Audio | cut -c1-40))"; continue; fi
  run_basic "$t" "$to"
  [ "$t" = TINFO ] && t_help
done
want MOUSE && t_mouse
want WAV && t_wav
want REDIR && t_redir
want VIDEO && t_video
want VFS && t_vfs
want STRESS && t_stress
want MMFS && t_mmfs
want ROM && t_rom
want WIFI && t_wifi
want BREAK && t_break

# ---- teardown -------------------------------------------------------------
log "== teardown"
lines '*ADFS|*CAT' 'Option' 15000; lines '*BYE' '' 2000
if [ $RESTORE = 1 ]; then
  lines "*FX147,65,$JUKE_RESTORE" '' 2000; lines '*MOUNT 0' '' 5000
  CHANGES+=("jukebox set restored to $JUKE_RESTORE")
fi
if [ $TUBE_CHANGED = 1 ] && [ $RESTORE = 1 ] && [ $KEEP_NOTUBE = 0 ]; then
  lines '*CONFIGURE TUBE' '' 2000; ctrl_break; arm_echo
  CHANGES+=("*CONFIGURE TUBE restored (CTRL-BREAK done)")
fi

result "I:PI:bus at end:$(pi_row 'Bus diag' | cut -c1-80)"
# ---- summary --------------------------------------------------------------
log ""
log "RESULTS ($RES)"
python3 - "$RES" <<'EOF' | tee -a "$LOG"
import sys, collections
c = collections.OrderedDict()
for l in open(sys.argv[1]):
    if not l.startswith('T:'): continue
    _, t, name, verdict, *rest = l.rstrip('\n').split(':', 4) + ['']
    d = c.setdefault(t, collections.Counter())
    d[verdict.split(':')[0]] += 1
    if verdict.startswith('FAIL'): print('  FAIL  %s: %s %s' % (t, name, rest[0] if rest else ''))
    if verdict.startswith('SKIP'): print('  skip  %s: %s %s' % (t, name, rest[0] if rest else ''))
print('%-8s %5s %5s %5s' % ('test', 'pass', 'fail', 'skip'))
tp = tf = ts = 0
for t, d in c.items():
    print('%-8s %5d %5d %5d' % (t, d['PASS'], d['FAIL'], d['SKIP'])); tp += d['PASS']; tf += d['FAIL']; ts += d['SKIP']
print('%-8s %5d %5d %5d' % ('total', tp, tf, ts))
sys.exit(1 if tf else 0)
EOF
rc=${PIPESTATUS[0]}
log "state changed on the bench: ${CHANGES[*]:-none}"
exit $rc
