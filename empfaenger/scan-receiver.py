#!/usr/bin/env python3
"""Winziger Scan-Empfaenger: nimmt POST /scan?name=... entgegen und legt die
Datei in ~/scan-inbox/ ab. Nur zum Testen der Scan-Stick-Kette gedacht;
im Betrieb tritt hier die eigentliche Ablage an diese Stelle."""
import os, time, http.server, socketserver, urllib.parse

PORT = 8080
INBOX = os.path.expanduser("~/scan-inbox")
os.makedirs(INBOX, exist_ok=True)

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


class Handler(http.server.BaseHTTPRequestHandler):
    def _sicherer_name(self, roh):
        roh = os.path.basename(roh or "")
        roh = "".join(c for c in roh if c.isalnum() or c in "._- ")
        return roh or ("scan_%d.bin" % int(time.time()))

    def do_POST(self):
        q = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(q.query)
        name = self._sicherer_name(params.get("name", [""])[0])
        scan_id = self._sicherer_name(params.get("id", [""])[0])
        laenge = int(self.headers.get("Content-Length", 0))
        daten = self.rfile.read(laenge) if laenge else b""

        if id_bekannt(scan_id):
            print("[%s] schon empfangen (%s), verworfen: %s (%d Bytes)" % (
                time.strftime("%H:%M:%S"), scan_id, name, len(daten)))
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"OK (Duplikat, nicht erneut abgelegt)\n")
            return

        ziel = os.path.join(INBOX, name)
        # Kollision vermeiden
        basis, ext = os.path.splitext(ziel)
        n = 1
        while os.path.exists(ziel):
            ziel = "%s_%d%s" % (basis, n, ext); n += 1
        with open(ziel, "wb") as f:
            f.write(daten)
        id_merken(scan_id)
        print("[%s] empfangen: %s (%d Bytes, id %s) von %s" % (
            time.strftime("%H:%M:%S"), os.path.basename(ziel), len(daten),
            scan_id or "ohne", self.client_address[0]))
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"OK\n")

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"scan-receiver laeuft\n")

    def log_message(self, *a):
        pass  # eigene Ausgabe reicht


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    print("scan-receiver auf 0.0.0.0:%d, Ablage %s" % (PORT, INBOX))
    Server(("0.0.0.0", PORT), Handler).serve_forever()
