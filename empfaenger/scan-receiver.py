#!/usr/bin/env python3
"""Winziger Scan-Empfaenger: nimmt POST /scan?name=... entgegen und legt die
Datei in ~/scan-inbox/ ab. Nur zum Testen der Scan-Stick-Kette gedacht;
im Betrieb tritt hier die eigentliche Ablage an diese Stelle."""
import os, time, http.server, socketserver, urllib.parse

PORT = 8080
INBOX = os.path.expanduser("~/scan-inbox")
os.makedirs(INBOX, exist_ok=True)


class Handler(http.server.BaseHTTPRequestHandler):
    def _sicherer_name(self, roh):
        roh = os.path.basename(roh or "")
        roh = "".join(c for c in roh if c.isalnum() or c in "._- ")
        return roh or ("scan_%d.bin" % int(time.time()))

    def do_POST(self):
        q = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(q.query)
        name = self._sicherer_name(params.get("name", [""])[0])
        laenge = int(self.headers.get("Content-Length", 0))
        daten = self.rfile.read(laenge) if laenge else b""
        ziel = os.path.join(INBOX, name)
        # Kollision vermeiden
        basis, ext = os.path.splitext(ziel)
        n = 1
        while os.path.exists(ziel):
            ziel = "%s_%d%s" % (basis, n, ext); n += 1
        with open(ziel, "wb") as f:
            f.write(daten)
        print("[%s] empfangen: %s (%d Bytes) von %s" % (
            time.strftime("%H:%M:%S"), os.path.basename(ziel), len(daten), self.client_address[0]))
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
