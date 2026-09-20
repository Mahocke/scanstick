#!/usr/bin/env python3
"""Winziger Scan-Empfaenger: nimmt POST /scan?name=... entgegen und legt die
Datei in ~/scan-inbox/ ab. Nur zum Testen der Scan-Stick-Kette gedacht;
im Betrieb tritt hier die eigentliche Ablage an diese Stelle.

Port und Ablageordner lassen sich ueber die Umgebung setzen:
    SCAN_PORT=8080 SCAN_INBOX=~/scan-inbox python3 scan-receiver.py
"""
import os, time, hmac, hashlib, threading, http.server, socketserver, urllib.parse

PORT = int(os.environ.get("SCAN_PORT", "8080"))
INBOX = os.path.expanduser(os.environ.get("SCAN_INBOX", "~/scan-inbox"))
# Geraeteschluessel: ist er gesetzt, muss jeder Upload eine gueltige Kopfzeile
# X-Scan-Auth tragen (HMAC-SHA256 ueber "name\nid\nlaenge", derselbe Schluessel
# wie in den Einstellungen des Sticks). Ohne Schluessel nimmt der Empfaenger
# alles an - im Heimnetz vertretbar, fuer den Dauerbetrieb nicht.
SCHLUESSEL = os.environ.get("SCAN_KEY", "").encode()
# Der Stick schickt sein Protokoll an denselben Endpunkt (scanlog-*.txt). Das
# gehoert nicht zwischen die Scans, sondern in einen eigenen Ordner.
PROTOKOLL = os.path.join(INBOX, "protokoll")
os.makedirs(PROTOKOLL, exist_ok=True)

# Der Stick haengt an jeden Upload eine Kennung der Datei. Kommt dieselbe
# Kennung ein zweites Mal, ist es eine Wiederholung - etwa nach einem
# abgebrochenen Upload oder weil der Drucker seine alte Verzeichnissicht
# zurueckgeschrieben hat und die Datei erneut auftauchte. Wir bestaetigen
# solche Uploads, legen sie aber nicht noch einmal ab.
IDLISTE = os.path.join(INBOX, ".empfangene-ids")


def id_bekannt(scan_id):
    if not scan_id:
        return False
    try:
        with open(IDLISTE) as f:
            return scan_id in f.read().split()
    except FileNotFoundError:
        return False


def id_merken(scan_id):
    if not scan_id:
        return
    with open(IDLISTE, "a") as f:
        f.write(scan_id + "\n")
        f.flush()
        os.fsync(f.fileno())


def ordner_sichern(pfad):
    """Verzeichniseintrag auf die Platte zwingen (POSIX); anderswo egal."""
    try:
        fd = os.open(pfad, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def ablegen(ordner, name, daten):
    """Datei absturzsicher ablegen: erst in eine temporaere Datei schreiben,
    auf die Platte zwingen, dann unter dem endgueltigen Namen einhaengen.
    Stirbt der Rechner mittendrin, bleibt hoechstens eine .teil-Datei uebrig,
    nie eine halbe PDF unter richtigem Namen. Erst danach wird die Kennung
    gemerkt - eine gemerkte Kennung ohne sicher liegende Datei waere die
    einzige Kombination, die einen Scan wirklich verlieren kann.
    Liefert den endgueltigen Pfad."""
    tmp = os.path.join(ordner, "." + name + ".teil")
    with open(tmp, "wb") as f:
        f.write(daten)
        f.flush()
        os.fsync(f.fileno())
    basis, ext = os.path.splitext(os.path.join(ordner, name))
    ziel, n = basis + ext, 1
    while True:
        try:
            os.link(tmp, ziel)          # atomar und exklusiv: scheitert, wenn es den Namen gibt
            break
        except FileExistsError:
            ziel = "%s_%d%s" % (basis, n, ext); n += 1
        except OSError:
            os.replace(tmp, ziel)       # Dateisystem ohne harte Verweise
            ordner_sichern(ordner)
            return ziel
    os.unlink(tmp)
    ordner_sichern(ordner)
    return ziel


# Zwei gleichzeitige Uploads derselben Kennung (Wiederholung des Sticks waehrend
# der erste noch laeuft) duerfen nicht beide durchkommen.
SPERRE = threading.Lock()


def vollstaendig(name, daten, angekuendigt):
    """Nur eine vollstaendige Datei darf abgelegt und als empfangen gemerkt
    werden. Vorher wurde ein abgerissener Upload verkuerzt gespeichert und
    seine Kennung gemerkt - die Wiederholung des Sticks galt dann als Duplikat,
    und der Stick loeschte daraufhin das einzige vollstaendige Exemplar."""
    if len(daten) != angekuendigt:
        return "nur %d von %d Bytes angekommen" % (len(daten), angekuendigt)
    if name.lower().endswith(".pdf") and b"%%EOF" not in daten[-1024:]:
        return "PDF ohne Endmarke %%EOF"
    return None


class Handler(http.server.BaseHTTPRequestHandler):
    def _sicherer_name(self, roh):
        roh = os.path.basename(roh or "")
        roh = "".join(c for c in roh if c.isalnum() or c in "._- ")
        return roh or ("scan_%d.bin" % int(time.time()))

    def _antwort(self, code, text, duplikat=False):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        if duplikat:
            # Der Stick liest das Wort "Duplikat" aus der Antwort und traegt
            # seinen Geist-Eintrag dann roh aus, statt ihn zu verschieben.
            self.send_header("X-Scan-Duplikat", "ja")
        self.end_headers()
        self.wfile.write(text.encode() + b"\n")

    def do_POST(self):
        q = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(q.query)
        name_roh = params.get("name", [""])[0]
        id_roh = params.get("id", [""])[0]
        name = self._sicherer_name(name_roh)
        scan_id = self._sicherer_name(id_roh)
        laenge = int(self.headers.get("Content-Length", 0))
        daten = self.rfile.read(laenge) if laenge else b""
        stempel = time.strftime("%H:%M:%S")

        if SCHLUESSEL:
            nachricht = ("%s\n%s\n%d" % (name_roh, id_roh, laenge)).encode()
            soll = hmac.new(SCHLUESSEL, nachricht, hashlib.sha256).hexdigest()
            ist = self.headers.get("X-Scan-Auth", "")
            if not hmac.compare_digest(soll, ist):
                print("[%s] ABGELEHNT %s von %s: Signatur %s" % (
                    stempel, name, self.client_address[0], "fehlt" if not ist else "falsch"))
                self._antwort(401, "abgelehnt: Signatur fehlt oder falsch")
                return

        fehler = vollstaendig(name, daten, laenge)
        if fehler:
            print("[%s] ABGELEHNT %s: %s" % (stempel, name, fehler))
            self._antwort(400, "abgelehnt: " + fehler)
            return

        ordner = PROTOKOLL if name.startswith("scanlog-") else INBOX
        with SPERRE:
            if id_bekannt(scan_id):
                print("[%s] schon empfangen (%s), verworfen: %s (%d Bytes)" % (
                    stempel, scan_id, name, len(daten)))
                self._antwort(200, "OK (Duplikat, nicht erneut abgelegt)", duplikat=True)
                return
            ziel = ablegen(ordner, name, daten)   # Datei sicher, dann erst die Kennung
            id_merken(scan_id)
        print("[%s] empfangen: %s (%d Bytes, id %s) von %s" % (
            stempel, os.path.basename(ziel), len(daten),
            scan_id or "ohne", self.client_address[0]))
        self._antwort(200, "OK")

    def do_GET(self):
        self._antwort(200, "scan-receiver laeuft")

    def log_message(self, *a):
        pass  # eigene Ausgabe reicht


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    print("scan-receiver auf 0.0.0.0:%d, Ablage %s" % (PORT, INBOX))
    Server(("0.0.0.0", PORT), Handler).serve_forever()
