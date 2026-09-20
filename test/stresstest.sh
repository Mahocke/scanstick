#!/usr/bin/env bash
# Test rig for the Scan-Stick - without a printer.
#
# Runs on a Linux machine (a Raspberry Pi for example) with the stick attached as
# a USB drive. The machine plays the printer: it mounts the stick, writes test
# PDFs, ejects. A receiver on the same machine checks whether every file arrives
# complete and exactly once.
#
#   sudo test/stresstest.sh [stick address] [partition]
#     stick address  http://scanstick-1a2b.local  (default scanstick.local; better your own name or the IP,
#                    with two sticks on the network the name hits the wrong one)
#     partition      /dev/sda1               (default)
#
#   Environment: SCAN_WEBPASS=...  password of the web interface (user scan)
#                SCAN_PORT=8090    port of the test receiver
#                SCAN_SZENARIEN=ABCD  which scenarios run (E extra: damage the card)
#
# The upload target of the stick is switched to this machine for the duration of
# the test and reset at the end. The partition is only touched if it belongs to
# an Espressif USB device and is named SCANS.
#
# Scenarios:
#   A  one file
#   B  two files in one go (the printer drops them one after another)
#   C  second file as soon as the medium is back after the first upload -
#      the tightest moment a real printer can hit
#   D  five jobs back to back, each with its own mount and with the printer's
#      names; counts how often the medium was gone - exactly those jobs a
#      printer would have rejected
#   W  (on request only) twice the same size in a row, cleanup in between:
#      same start block, new content - must arrive; then the same content
#      once more - the receiver must see it as a repeat
#   E  (on request only) damage the card on purpose - chain runs into another
#      one, orphaned clusters, dirty flag - and let the stick heal it;
#      fsck must be clean afterwards
#
# The Linux driver writes directory entries earlier and differently than a
# printer. The test rig does not replace the print test, but it makes
# regressions repeatably visible.
set -u
export PATH="/sbin:/usr/sbin:$PATH"

STICK="${1:-http://scanstick.local}"
DEV="${2:-/dev/sda1}"
PORT="${SCAN_PORT:-8090}"
PASS="${SCAN_WEBPASS:-}"
SZENARIEN="${SCAN_SZENARIEN:-ABCD}"
HIER="$(cd "$(dirname "$0")/.." && pwd)"
ARBEIT="$(mktemp -d /tmp/stresstest.XXXX)"
INBOX="$ARBEIT/inbox"
MNT="$ARBEIT/mnt"
mkdir -p "$INBOX" "$MNT"
BLOCK="/dev/$(basename "$DEV" | sed 's/[0-9]*$//')"
FEHLER=0
GESCHRIEBEN=0
ALTES_ZIEL=""
EMPFAENGER_PID=""

CURL=(curl -s -m 15)
[ -n "$PASS" ] && CURL+=(-u "scan:$PASS")

log()  { printf '%s  %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { log "ERROR: $*"; FEHLER=$((FEHLER + 1)); }

aufraeumen() {
    mountpoint -q "$MNT" && umount "$MNT"
    if [ -n "$ALTES_ZIEL" ]; then
        "${CURL[@]}" --data-urlencode "endpoint=$ALTES_ZIEL" "$STICK/einstellungen" >/dev/null \
            && log "Upload target reset to $ALTES_ZIEL" \
            || log "WARNING: could not reset the upload target (was $ALTES_ZIEL)"
    fi
    [ -n "$EMPFAENGER_PID" ] && kill "$EMPFAENGER_PID" 2>/dev/null
    log "Work directory: $ARBEIT (receiver log, inbox)"
}
trap aufraeumen EXIT

[ "$(id -u)" = 0 ] || { echo "please start with sudo (mount/umount)"; exit 2; }

# ---- Safety net: touch only the stick ----
VID=$(udevadm info -q property -n "$BLOCK" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
LABEL=$(lsblk -no LABEL "$DEV" 2>/dev/null)
if [ "$VID" != "303a" ] || [ "$LABEL" != "SCANS" ]; then
    echo "$DEV is not a Scan-Stick (USB vendor '$VID', name '$LABEL') - aborting"
    exit 2
fi

# ---- Stick reachable? ----
STATUS=$("${CURL[@]}" "$STICK/") || { echo "web interface $STICK not reachable"; exit 2; }
VERSION=$(printf '%s' "$STATUS" | grep -o 'Scan-Stick v[0-9]*' | head -1)
ALTES_ZIEL=$(printf '%s' "$STATUS" | grep -o 'Upload target</td><td>[^<]*' | sed 's/.*<td>//')
STICK_IP=$(getent hosts "${STICK#http://}" | awk '{print $1}')
[ -z "$STICK_IP" ] && STICK_IP="${STICK#http://}"
MEINE_IP=$(ip -o route get "$STICK_IP" | sed -n 's/.*src \([0-9.]*\).*/\1/p')
log "Stick: $VERSION at $STICK, previous target: $ALTES_ZIEL"
log "Test rig: $(hostname) $MEINE_IP, receiver on port $PORT"

# ---- Start the receiver and switch the stick over ----
SCAN_PORT="$PORT" SCAN_INBOX="$INBOX" python3 -u "$HIER/empfaenger/scan-receiver.py" \
    >"$ARBEIT/empfaenger.log" 2>&1 &
EMPFAENGER_PID=$!
sleep 1
curl -s -m 3 "http://127.0.0.1:$PORT/" >/dev/null || { echo "receiver does not start, see $ARBEIT/empfaenger.log"; exit 2; }

NEUES_ZIEL="http://$MEINE_IP:$PORT/scan"
"${CURL[@]}" --data-urlencode "endpoint=$NEUES_ZIEL" "$STICK/einstellungen" >/dev/null
JETZT=$("${CURL[@]}" "$STICK/" | grep -o 'Upload target</td><td>[^<]*' | sed 's/.*<td>//')
[ "$JETZT" = "$NEUES_ZIEL" ] || { echo "could not set the upload target (is '$JETZT')"; exit 2; }
log "Upload target for the test: $NEUES_ZIEL"

# ---- Helper functions ----
medium_da() { dd if="$BLOCK" of=/dev/null bs=512 count=1 2>/dev/null; }

warte_medium() {   # warte_medium da|weg seconds
    local ziel=$1 frist=$2 t=0
    while [ $t -lt "$frist" ]; do
        if [ "$ziel" = da ]; then
            if medium_da; then partprobe "$BLOCK" 2>/dev/null; sleep 1; [ -b "$DEV" ] && return 0; fi
            sleep 1; t=$((t + 1))
        else
            # Since v25 the window is only one or two seconds long - sample tightly
            medium_da || return 0
            sleep 0.2; t=$((t + 1))
        fi
    done
    return 1
}

# creates a PDF with a valid header, random content and %%EOF at the end
mach_pdf() {   # mach_pdf file bytes
    python3 - "$1" "$2" <<'PY'
import os, sys
pfad, groesse = sys.argv[1], int(sys.argv[2])
kopf = (b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
        b"1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        b"2 0 obj << /Type /Pages /Kids [] /Count 0 >> endobj\n"
        b"3 0 obj << /Length 0 >> stream\n")
fuss = b"\nendstream endobj\ntrailer << /Root 1 0 R >>\n%%EOF\n"
with open(pfad, "wb") as f:
    f.write(kopf)
    f.write(os.urandom(max(0, groesse - len(kopf) - len(fuss))))
    f.write(fuss)
PY
}

# writes files like a printer: mount, drop, sync, eject
schreibe() {   # schreibe file...
    warte_medium da 120 || { fail "medium did not come back"; return 1; }
    mount -t vfat "$DEV" "$MNT" || { fail "mount $DEV"; return 1; }
    for f in "$@"; do
        cp "$f" "$MNT/[Untitled].pdf" 2>/dev/null && mv "$MNT/[Untitled].pdf" "$MNT/$(basename "$f")"
        log "  written: $(basename "$f") ($(stat -c %s "$f") bytes)"
        GESCHRIEBEN=$((GESCHRIEBEN + 1))
    done
    sync
    umount "$MNT" || fail "umount"
}

# waits until a file with this checksum is in the inbox
warte_ankunft() {   # warte_ankunft file seconds
    local soll t=0
    soll=$(sha256sum "$1" | cut -d' ' -f1)
    while [ $t -lt "$2" ]; do
        if ls "$INBOX"/*.pdf >/dev/null 2>&1 && sha256sum "$INBOX"/*.pdf | grep -q "^$soll "; then
            log "  arrived: $(basename "$1") after ${t}s"
            return 0
        fi
        sleep 2; t=$((t + 2))
    done
    fail "$(basename "$1") did not arrive completely within $2 s"
    return 1
}

# ---- Scenario A: one file ----
if [[ $SZENARIEN == *A* ]]; then
log "Scenario A: one file (300 kB)"
mach_pdf "$ARBEIT/a1.pdf" 300000
schreibe "$ARBEIT/a1.pdf"
warte_ankunft "$ARBEIT/a1.pdf" 180
fi

# ---- Scenario B: two files in one go ----
if [[ $SZENARIEN == *B* ]]; then
log "Scenario B: two files in a row in the same mount (800 kB + 150 kB)"
mach_pdf "$ARBEIT/b1.pdf" 800000
mach_pdf "$ARBEIT/b2.pdf" 150000
schreibe "$ARBEIT/b1.pdf" "$ARBEIT/b2.pdf"
warte_ankunft "$ARBEIT/b1.pdf" 240
warte_ankunft "$ARBEIT/b2.pdf" 120
fi

# ---- Scenario C: second file as soon as the medium is back ----
if [[ $SZENARIEN == *C* ]]; then
log "Scenario C: large file (2 MB), second one right after the medium returns"
mach_pdf "$ARBEIT/c1.pdf" 2000000
mach_pdf "$ARBEIT/c2.pdf" 250000
schreibe "$ARBEIT/c1.pdf"
if warte_medium weg 300; then      # 300 samples of 0.2 s = 60 s
    log "  stick has taken the medium"
else
    log "  window not observed (too short?) - writing c2 anyway, right into the upload"
fi
schreibe "$ARBEIT/c2.pdf"         # waits by itself until the medium is back
warte_ankunft "$ARBEIT/c1.pdf" 300
warte_ankunft "$ARBEIT/c2.pdf" 300
fi


# ---- Scenario D: five jobs back to back ----
# This is how the printer works: one job = mount, write, eject, and the next one
# right after. If the medium is gone at mount time, the printer would have
# rejected the job - we count that and let the job drop out.
VERPASST=0
if [[ $SZENARIEN == *D* ]]; then
log "Scenario D: five jobs back to back, each with its own mount, printer names"
for i in 1 2 3 4 5; do
    mach_pdf "$ARBEIT/d$i.pdf" $((250000 + i * 50000))
    name="[Untitled].pdf"; [ $i -gt 1 ] && name="[Untitled]_$(date +%Y%m%d%H%M%S)0$i.pdf"
    partprobe "$BLOCK" 2>/dev/null
    if [ -b "$DEV" ] && mount -t vfat "$DEV" "$MNT" 2>/dev/null; then
        cp "$ARBEIT/d$i.pdf" "$MNT/$name"; sync; umount "$MNT" || fail "umount job $i"
        log "  job $i written as $name"
        GESCHRIEBEN=$((GESCHRIEBEN + 1))
    else
        VERPASST=$((VERPASST + 1))
        log "  job $i: medium not there - the printer would have rejected the job"
        rm -f "$ARBEIT/d$i.pdf"
        warte_medium da 60 >/dev/null
    fi
done
for i in 1 2 3 4 5; do [ -f "$ARBEIT/d$i.pdf" ] && warte_ankunft "$ARBEIT/d$i.pdf" 300; done
log "  Scenario D: $VERPASST of 5 jobs would have been rejected by the printer"
fi

# ---- Scenario W: same size, same start block, new content ----
# After the cleanup the card is empty, the next scan lands on the same start
# block as the previous one - and the same sheet twice has the same size.
# The stick must not take that for the return of the old file.
if [[ $SZENARIEN == *W* ]]; then
log "Scenario W: twice 300 kB in a row, cleanup in between"
mach_pdf "$ARBEIT/w1.pdf" 300000
schreibe "$ARBEIT/w1.pdf"
warte_ankunft "$ARBEIT/w1.pdf" 180
sleep 8
"${CURL[@]}" -X POST "$STICK/aufraeumen" >/dev/null
sleep 6
mach_pdf "$ARBEIT/w2.pdf" 300000                # same size, different content
schreibe "$ARBEIT/w2.pdf"
warte_ankunft "$ARBEIT/w2.pdf" 180
# and the same content once more: the stick sees a new file (different start
# block), the receiver must recognize it as a repeat by its identifier
sleep 8
cp "$ARBEIT/w2.pdf" "$ARBEIT/w3.pdf"
warte_medium da 60 >/dev/null
mount -t vfat "$DEV" "$MNT" && cp "$ARBEIT/w3.pdf" "$MNT/w3.pdf" && sync && umount "$MNT"
log "  written: w3.pdf (copy of w2, does not count as new)"
t=0; while [ $t -lt 120 ] && ! grep -q "already received" "$ARBEIT/empfaenger.log"; do sleep 2; t=$((t + 2)); done
if grep -q "already received" "$ARBEIT/empfaenger.log"; then log "  receiver recognized w3 as a repeat (after ${t}s)"
else fail "w3 was not recognized as a repeat"; fi
[ "$(ls "$INBOX"/w*.pdf 2>/dev/null | wc -l)" -le 2 ] || fail "w3 was stored anyway"
fi

# ---- Scenario E: heal a damaged card ----
# After aborted jobs and written-back directories the printer leaves behind
# cross-links, orphaned clusters and the dirty flag - and at some point rejects
# such a card. We create exactly that, let the stick clean up and check with
# fsck whether the card is clean afterwards.
if [[ $SZENARIEN == *E* ]]; then
log "Scenario E: damage the card (cross-link, orphan, dirty flag) and let it heal"
mach_pdf "$ARBEIT/e1.pdf" 120000
mach_pdf "$ARBEIT/e2.pdf" 90000
schreibe "$ARBEIT/e1.pdf" "$ARBEIT/e2.pdf"
warte_ankunft "$ARBEIT/e1.pdf" 180
warte_ankunft "$ARBEIT/e2.pdf" 180
warte_medium da 60 >/dev/null
python3 - "$DEV" <<'PY'
import sys, struct
dev = sys.argv[1]
f = open(dev, "r+b")
bs = f.read(512)
bps, spc, rsv, nfat = struct.unpack_from("<HBHB", bs, 11)
spf = struct.unpack_from("<I", bs, 36)[0]; root = struct.unpack_from("<I", bs, 44)[0]
data0 = rsv + nfat * spf
def csec(c): return data0 + (c - 2) * spc
def fat_get(c):
    f.seek(rsv * bps + c * 4); return struct.unpack("<I", f.read(4))[0]
def fat_set(c, v):
    for k in range(nfat):
        f.seek((rsv + k * spf) * bps + c * 4); f.write(struct.pack("<I", v))
f.seek(csec(root) * bps); d = bytearray(f.read(spc * bps))
starts = {}
for i in range(0, len(d), 32):
    e = d[i:i+32]
    if e[0] == 0: break
    if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x18: continue
    name = e[:11].decode("latin1")
    st = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
    starts[name] = (i, st)
i1, s1 = starts["E1      PDF"]; i2, s2 = starts["E2      PDF"]
# 1. cross-link in the middle of the chain: after its first cluster e2 runs into e1
#    (the ghost hunt only sees equal start clusters, this one the self-check finds)
fat_set(s2, s1)
# 2. orphan: a chain that belongs to nobody
w = max(s1, s2) + 40
fat_set(w, 0x0FFFFFFF); fat_set(w + 1, 0x0FFFFFFF)
# 3. dirty flag
fat_set(1, fat_get(1) & ~0x08000000)
f.flush(); f.close()
print("  created: chain of e2 (cluster %d) runs into e1 (cluster %d), orphans %d+%d, dirty flag" % (s2, s1, w, w + 1))
PY
sync
fsck.fat -n "$DEV" 2>&1 | grep -qE "share clusters|not marked|dirty" && log "  fsck sees the damage" || fail "damage not created?"
sleep 12                                   # the stick sees the writes, finds nothing new
"${CURL[@]}" -X POST "$STICK/aufraeumen" >/dev/null
sleep 6
warte_medium da 60 >/dev/null
BEFUND=$("${CURL[@]}" "$STICK/log" | sed -e 's/<[^>]*>/\n/g' | grep '\[karte\] in the cleanup window' | tail -1)
log "  Stick: ${BEFUND#* }"
printf '%s' "$BEFUND" | grep -q "cross-link" || fail "stick did not report the cross-link"
printf '%s' "$BEFUND" | grep -q "orphaned"   || fail "stick did not report the orphans"
printf '%s' "$BEFUND" | grep -q "dirty flag" || fail "stick did not report the dirty flag"
if fsck.fat -n "$DEV" >"$ARBEIT/fsck.txt" 2>&1 && ! grep -qE "share clusters|not marked|dirty|Truncat" "$ARBEIT/fsck.txt"; then
    log "  fsck afterwards: clean"
else
    fail "fsck still finds damage after the healing (see $ARBEIT/fsck.txt)"
fi
fi

# ---- Summary ----
ANGEKOMMEN=$(ls "$INBOX"/*.pdf 2>/dev/null | wc -l)
ABGELEHNT=$(grep -c REJECTED "$ARBEIT/empfaenger.log" || true)
DUPLIKATE=$(grep -c "already received" "$ARBEIT/empfaenger.log" || true)
log "Summary: $GESCHRIEBEN written, $ANGEKOMMEN arrived, $DUPLIKATE duplicates discarded, $ABGELEHNT rejected, $VERPASST jobs with missing medium"
[ "$ANGEKOMMEN" -eq "$GESCHRIEBEN" ] || fail "count does not match"
[ "$ABGELEHNT" -eq 0 ] || fail "receiver rejected incomplete uploads"

warte_medium da 120 || fail "medium not back at the end"

if [ "$FEHLER" -eq 0 ]; then log "RESULT: all passed"; exit 0; fi
log "RESULT: $FEHLER errors"
exit 1
