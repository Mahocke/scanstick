#!/usr/bin/env bash
# Pruefstand fuer den Scan-Stick - ohne Drucker.
#
# Laeuft auf einem Linux-Rechner (etwa einem Raspberry Pi), an dem der Stick als
# USB-Laufwerk haengt. Der Rechner spielt den Drucker: er mountet den Stick,
# schreibt Test-PDFs, wirft aus. Ein Empfaenger auf demselben Rechner prueft,
# ob jede Datei vollstaendig und genau einmal ankommt.
#
#   sudo test/stresstest.sh [Stick-Adresse] [Partition]
#     Stick-Adresse  http://scanstick.local  (Vorgabe)
#     Partition      /dev/sda1               (Vorgabe)
#
#   Umgebung: SCAN_WEBPASS=...  Passwort der Weboberflaeche (Benutzer scan)
#             SCAN_PORT=8090    Port des Test-Empfaengers
#             SCAN_SZENARIEN=ABCD  welche Szenarien laufen (E extra: Karte beschaedigen)
#
# Das Upload-Ziel des Sticks wird fuer die Dauer des Tests auf diesen Rechner
# umgestellt und am Ende zurueckgesetzt. Die Partition wird nur angefasst, wenn
# sie zu einem Espressif-USB-Geraet gehoert und SCANS heisst.
#
# Szenarien:
#   A  eine Datei
#   B  zwei Dateien in einem Zug (der Drucker legt sie nacheinander ab)
#   C  zweite Datei, sobald das Medium nach dem ersten Upload wieder da ist -
#      der engste Zeitpunkt, den ein Drucker real treffen kann
#   D  fuenf Jobs Schlag auf Schlag, jeder in eigenem Einhaengen und mit den
#      Namen des Druckers; zaehlt, wie oft das Medium gerade weg war - genau
#      diese Jobs haette ein Drucker abgewiesen
#   E  (nur auf Wunsch) Karte absichtlich beschaedigen - Kette laeuft in eine
#      andere, verwaiste Cluster, Schmutzmarke - und den Stick heilen lassen;
#      fsck muss danach sauber sein
#
# Der Linux-Treiber schreibt Verzeichniseintraege frueher und anders als ein
# Drucker. Der Pruefstand ersetzt den Drucktest nicht, macht aber Regressionen
# wiederholbar sichtbar.
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
fail() { log "FEHLER: $*"; FEHLER=$((FEHLER + 1)); }

aufraeumen() {
    mountpoint -q "$MNT" && umount "$MNT"
    if [ -n "$ALTES_ZIEL" ]; then
        "${CURL[@]}" --data-urlencode "endpoint=$ALTES_ZIEL" "$STICK/einstellungen" >/dev/null \
            && log "Upload-Ziel zurueckgesetzt auf $ALTES_ZIEL" \
            || log "WARNUNG: Upload-Ziel konnte nicht zurueckgesetzt werden (war $ALTES_ZIEL)"
    fi
    [ -n "$EMPFAENGER_PID" ] && kill "$EMPFAENGER_PID" 2>/dev/null
    log "Arbeitsordner: $ARBEIT (Empfaenger-Log, Inbox)"
}
trap aufraeumen EXIT

[ "$(id -u)" = 0 ] || { echo "bitte mit sudo starten (mount/umount)"; exit 2; }

# ---- Sicherheitsnetz: nur den Stick anfassen ----
VID=$(udevadm info -q property -n "$BLOCK" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
LABEL=$(lsblk -no LABEL "$DEV" 2>/dev/null)
if [ "$VID" != "303a" ] || [ "$LABEL" != "SCANS" ]; then
    echo "$DEV ist kein Scan-Stick (USB-Hersteller '$VID', Name '$LABEL') - Abbruch"
    exit 2
fi

# ---- Stick erreichbar? ----
STATUS=$("${CURL[@]}" "$STICK/") || { echo "Weboberflaeche $STICK nicht erreichbar"; exit 2; }
VERSION=$(printf '%s' "$STATUS" | grep -o 'Scan-Stick v[0-9]*' | head -1)
ALTES_ZIEL=$(printf '%s' "$STATUS" | grep -o 'Ziel fuer Uploads</td><td>[^<]*' | sed 's/.*<td>//')
STICK_IP=$(getent hosts "${STICK#http://}" | awk '{print $1}')
[ -z "$STICK_IP" ] && STICK_IP="${STICK#http://}"
MEINE_IP=$(ip -o route get "$STICK_IP" | sed -n 's/.*src \([0-9.]*\).*/\1/p')
log "Stick: $VERSION unter $STICK, bisheriges Ziel: $ALTES_ZIEL"
log "Pruefstand: $(hostname) $MEINE_IP, Empfaenger auf Port $PORT"

# ---- Empfaenger starten und Stick umstellen ----
SCAN_PORT="$PORT" SCAN_INBOX="$INBOX" python3 -u "$HIER/empfaenger/scan-receiver.py" \
    >"$ARBEIT/empfaenger.log" 2>&1 &
EMPFAENGER_PID=$!
sleep 1
curl -s -m 3 "http://127.0.0.1:$PORT/" >/dev/null || { echo "Empfaenger startet nicht, siehe $ARBEIT/empfaenger.log"; exit 2; }

NEUES_ZIEL="http://$MEINE_IP:$PORT/scan"
"${CURL[@]}" --data-urlencode "endpoint=$NEUES_ZIEL" "$STICK/einstellungen" >/dev/null
JETZT=$("${CURL[@]}" "$STICK/" | grep -o 'Ziel fuer Uploads</td><td>[^<]*' | sed 's/.*<td>//')
[ "$JETZT" = "$NEUES_ZIEL" ] || { echo "Upload-Ziel liess sich nicht setzen (steht auf '$JETZT')"; exit 2; }
log "Upload-Ziel fuer den Test: $NEUES_ZIEL"

# ---- Hilfsfunktionen ----
medium_da() { dd if="$BLOCK" of=/dev/null bs=512 count=1 2>/dev/null; }

warte_medium() {   # warte_medium da|weg Sekunden
    local ziel=$1 frist=$2 t=0
    while [ $t -lt "$frist" ]; do
        if [ "$ziel" = da ]; then
            if medium_da; then partprobe "$BLOCK" 2>/dev/null; sleep 1; [ -b "$DEV" ] && return 0; fi
            sleep 1; t=$((t + 1))
        else
            # Das Fenster ist seit v25 nur noch ein bis zwei Sekunden lang - eng abtasten
            medium_da || return 0
            sleep 0.2; t=$((t + 1))
        fi
    done
    return 1
}

# erzeugt eine PDF mit gueltigem Kopf, Zufallsinhalt und %%EOF am Ende
mach_pdf() {   # mach_pdf Datei Bytes
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

# schreibt Dateien wie ein Drucker: mounten, ablegen, sync, auswerfen
schreibe() {   # schreibe Datei...
    warte_medium da 120 || { fail "Medium kam nicht zurueck"; return 1; }
    mount -t vfat "$DEV" "$MNT" || { fail "mount $DEV"; return 1; }
    for f in "$@"; do
        cp "$f" "$MNT/[Untitled].pdf" 2>/dev/null && mv "$MNT/[Untitled].pdf" "$MNT/$(basename "$f")"
        log "  geschrieben: $(basename "$f") ($(stat -c %s "$f") Bytes)"
        GESCHRIEBEN=$((GESCHRIEBEN + 1))
    done
    sync
    umount "$MNT" || fail "umount"
}

# wartet, bis eine Datei mit dieser Pruefsumme im Posteingang liegt
warte_ankunft() {   # warte_ankunft Datei Sekunden
    local soll t=0
    soll=$(sha256sum "$1" | cut -d' ' -f1)
    while [ $t -lt "$2" ]; do
        if ls "$INBOX"/*.pdf >/dev/null 2>&1 && sha256sum "$INBOX"/*.pdf | grep -q "^$soll "; then
            log "  angekommen: $(basename "$1") nach ${t}s"
            return 0
        fi
        sleep 2; t=$((t + 2))
    done
    fail "$(basename "$1") kam innerhalb von $2 s nicht vollstaendig an"
    return 1
}

# ---- Szenario A: eine Datei ----
if [[ $SZENARIEN == *A* ]]; then
log "Szenario A: eine Datei (300 kB)"
mach_pdf "$ARBEIT/a1.pdf" 300000
schreibe "$ARBEIT/a1.pdf"
warte_ankunft "$ARBEIT/a1.pdf" 180
fi

# ---- Szenario B: zwei Dateien in einem Zug ----
if [[ $SZENARIEN == *B* ]]; then
log "Szenario B: zwei Dateien nacheinander im selben Mount (800 kB + 150 kB)"
mach_pdf "$ARBEIT/b1.pdf" 800000
mach_pdf "$ARBEIT/b2.pdf" 150000
schreibe "$ARBEIT/b1.pdf" "$ARBEIT/b2.pdf"
warte_ankunft "$ARBEIT/b1.pdf" 240
warte_ankunft "$ARBEIT/b2.pdf" 120
fi

# ---- Szenario C: zweite Datei, sobald das Medium zurueck ist ----
if [[ $SZENARIEN == *C* ]]; then
log "Szenario C: grosse Datei (2 MB), zweite sofort nach Rueckkehr des Mediums"
mach_pdf "$ARBEIT/c1.pdf" 2000000
mach_pdf "$ARBEIT/c2.pdf" 250000
schreibe "$ARBEIT/c1.pdf"
if warte_medium weg 300; then      # 300 Abtastungen zu 0,2 s = 60 s
    log "  Stick hat das Medium genommen"
else
    log "  Fenster nicht beobachtet (zu kurz?) - schreibe c2 trotzdem, mitten in den Upload"
fi
schreibe "$ARBEIT/c2.pdf"         # wartet selbst, bis das Medium wieder da ist
warte_ankunft "$ARBEIT/c1.pdf" 300
warte_ankunft "$ARBEIT/c2.pdf" 300
fi


# ---- Szenario D: fuenf Jobs Schlag auf Schlag ----
# So arbeitet der Drucker: ein Job = einhaengen, schreiben, auswerfen, und der
# naechste sofort hinterher. Ist das Medium beim Einhaengen gerade weg, haette
# der Drucker den Job abgewiesen - wir zaehlen das und lassen den Job ausfallen.
VERPASST=0
if [[ $SZENARIEN == *D* ]]; then
log "Szenario D: fuenf Jobs Schlag auf Schlag, je eigenes Einhaengen, Druckernamen"
for i in 1 2 3 4 5; do
    mach_pdf "$ARBEIT/d$i.pdf" $((250000 + i * 50000))
    name="[Untitled].pdf"; [ $i -gt 1 ] && name="[Untitled]_$(date +%Y%m%d%H%M%S)0$i.pdf"
    partprobe "$BLOCK" 2>/dev/null
    if [ -b "$DEV" ] && mount -t vfat "$DEV" "$MNT" 2>/dev/null; then
        cp "$ARBEIT/d$i.pdf" "$MNT/$name"; sync; umount "$MNT" || fail "umount Job $i"
        log "  Job $i geschrieben als $name"
        GESCHRIEBEN=$((GESCHRIEBEN + 1))
    else
        VERPASST=$((VERPASST + 1))
        log "  Job $i: Medium nicht da - der Drucker haette den Job abgewiesen"
        rm -f "$ARBEIT/d$i.pdf"
        warte_medium da 60 >/dev/null
    fi
done
for i in 1 2 3 4 5; do [ -f "$ARBEIT/d$i.pdf" ] && warte_ankunft "$ARBEIT/d$i.pdf" 300; done
log "  Szenario D: $VERPASST von 5 Jobs haette der Drucker abgewiesen"
fi

# ---- Szenario E: beschaedigte Karte heilen ----
# Der Drucker hinterlaesst nach abgebrochenen Jobs und zurueckgeschriebenen
# Verzeichnissen Kreuzverkettungen, verwaiste Cluster und die Schmutzmarke -
# und lehnt so eine Karte irgendwann ab. Wir richten genau das an, lassen den
# Stick aufraeumen und pruefen mit fsck, ob die Karte danach sauber ist.
if [[ $SZENARIEN == *E* ]]; then
log "Szenario E: Karte beschaedigen (Kreuzverkettung, Waise, Schmutzmarke) und heilen lassen"
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
# 1. Kreuzverkettung mitten in der Kette: e2 laeuft nach dem ersten Cluster in e1 hinein
#    (die Geisterjagd sieht nur gleiche Startcluster, das hier findet erst die Selbstpruefung)
fat_set(s2, s1)
# 2. Waise: eine Kette, die niemandem gehoert
w = max(s1, s2) + 40
fat_set(w, 0x0FFFFFFF); fat_set(w + 1, 0x0FFFFFFF)
# 3. Schmutzmarke
fat_set(1, fat_get(1) & ~0x08000000)
f.flush(); f.close()
print("  angerichtet: Kette von e2 (Cluster %d) laeuft in e1 (Cluster %d), Waisen %d+%d, Schmutzmarke" % (s2, s1, w, w + 1))
PY
sync
fsck.fat -n "$DEV" 2>&1 | grep -qE "share clusters|not marked|dirty" && log "  fsck sieht den Schaden" || fail "Schaden nicht angerichtet?"
sleep 12                                   # der Stick sieht die Schreibzugriffe, findet nichts Neues
"${CURL[@]}" -X POST "$STICK/aufraeumen" >/dev/null
sleep 6
warte_medium da 60 >/dev/null
BEFUND=$("${CURL[@]}" "$STICK/log" | sed -e 's/<[^>]*>/\n/g' | grep '\[karte\] im Aufraeumfenster' | tail -1)
log "  Stick: ${BEFUND#* }"
printf '%s' "$BEFUND" | grep -q "Kreuzverkettung" || fail "Stick hat die Kreuzverkettung nicht gemeldet"
printf '%s' "$BEFUND" | grep -q "verwaiste"       || fail "Stick hat die Waisen nicht gemeldet"
printf '%s' "$BEFUND" | grep -q "Schmutzmarke"    || fail "Stick hat die Schmutzmarke nicht gemeldet"
if fsck.fat -n "$DEV" >"$ARBEIT/fsck.txt" 2>&1 && ! grep -qE "share clusters|not marked|dirty|Truncat" "$ARBEIT/fsck.txt"; then
    log "  fsck danach: sauber"
else
    fail "fsck findet nach dem Heilen noch Schaeden (siehe $ARBEIT/fsck.txt)"
fi
fi

# ---- Bilanz ----
ANGEKOMMEN=$(ls "$INBOX"/*.pdf 2>/dev/null | wc -l)
ABGELEHNT=$(grep -c ABGELEHNT "$ARBEIT/empfaenger.log" || true)
DUPLIKATE=$(grep -c "schon empfangen" "$ARBEIT/empfaenger.log" || true)
log "Bilanz: $GESCHRIEBEN geschrieben, $ANGEKOMMEN angekommen, $DUPLIKATE Duplikate verworfen, $ABGELEHNT abgelehnt, $VERPASST Jobs bei fehlendem Medium"
[ "$ANGEKOMMEN" -eq "$GESCHRIEBEN" ] || fail "Anzahl stimmt nicht"
[ "$ABGELEHNT" -eq 0 ] || fail "Empfaenger hat unvollstaendige Uploads abgelehnt"

warte_medium da 120 || fail "Medium am Ende nicht zurueck"

if [ "$FEHLER" -eq 0 ]; then log "ERGEBNIS: alles bestanden"; exit 0; fi
log "ERGEBNIS: $FEHLER Fehler"
exit 1
