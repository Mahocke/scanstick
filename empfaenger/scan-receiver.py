#!/usr/bin/env python3
"""Tiny scan receiver: accepts POST /scan?name=... and stores the file in
~/scan-inbox/. Meant for testing the Scan-Stick chain; in production the
real archiving step takes its place here.

Port and inbox folder can be set through the environment:
    SCAN_PORT=8080 SCAN_INBOX=~/scan-inbox python3 scan-receiver.py
"""
import os, time, hmac, hashlib, threading, http.server, socketserver, urllib.parse

PORT = int(os.environ.get("SCAN_PORT", "8080"))
INBOX = os.path.expanduser(os.environ.get("SCAN_INBOX", "~/scan-inbox"))
# Device key: if set, every upload must carry a valid X-Scan-Auth header
# (HMAC-SHA256 over "name\nid\nlength", same key as in the stick's settings).
# Without a key the receiver accepts everything - acceptable on a home
# network, not for permanent use.
SCHLUESSEL = os.environ.get("SCAN_KEY", "").encode()
# The stick sends its log to the same endpoint (scanlog-*.txt). That does
# not belong between the scans but in a folder of its own.
PROTOKOLL = os.path.join(INBOX, "protokoll")
os.makedirs(PROTOKOLL, exist_ok=True)

# The stick attaches an id of the file to every upload. If the same id
# arrives a second time it is a repeat - after an aborted upload, or because
# the printer wrote back its stale directory view and the file reappeared.
# We acknowledge such uploads but do not store them again.
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
    """Force the directory entry to disk (POSIX); harmless elsewhere."""
    try:
        fd = os.open(pfad, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        pass


def ablegen(ordner, name, daten):
    """Store a file crash-safely: write to a temporary file first, force it
    to disk, then link it under its final name. If the machine dies halfway,
    at most a .teil file is left behind, never a half PDF under the real
    name. Only after that is the id remembered - a remembered id without a
    safely stored file would be the one combination that really loses a scan.
    Returns the final path."""
    tmp = os.path.join(ordner, "." + name + ".teil")
    with open(tmp, "wb") as f:
        f.write(daten)
        f.flush()
        os.fsync(f.fileno())
    basis, ext = os.path.splitext(os.path.join(ordner, name))
    ziel, n = basis + ext, 1
    while True:
        try:
            os.link(tmp, ziel)          # atomic and exclusive: fails if the name exists
            break
        except FileExistsError:
            ziel = "%s_%d%s" % (basis, n, ext); n += 1
        except OSError:
            os.replace(tmp, ziel)       # file system without hard links
            ordner_sichern(ordner)
            return ziel
    os.unlink(tmp)
    ordner_sichern(ordner)
    return ziel


# Two simultaneous uploads with the same id (the stick retrying while the
# first one is still running) must not both get through.
SPERRE = threading.Lock()


def vollstaendig(name, daten, angekuendigt):
    """Only a complete file may be stored and remembered as received. Earlier
    a torn upload was stored truncated and its id remembered - the stick's
    retry then counted as a duplicate, and the stick deleted the only
    complete copy."""
    if len(daten) != angekuendigt:
        return "only %d of %d bytes arrived" % (len(daten), angekuendigt)
    if name.lower().endswith(".pdf") and b"%%EOF" not in daten[-1024:]:
        return "PDF without %%EOF end marker"
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
            # The stick looks for the word "duplicate" (or the older German
            # "Duplikat") in the reply and then drops its ghost entry raw
            # instead of moving it.
            self.send_header("X-Scan-Duplicate", "yes")
        self.end_headers()
        self.wfile.write(text.encode() + b"\n")

    def do_POST(self):
        q = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(q.query)
        name_roh = params.get("name", [""])[0]
        id_roh = params.get("id", [""])[0]
        name = self._sicherer_name(name_roh)
        scan_id = self._sicherer_name(id_roh) if id_roh else ""   # nothing to remember without an id
        laenge = int(self.headers.get("Content-Length", 0))
        daten = self.rfile.read(laenge) if laenge else b""
        stempel = time.strftime("%H:%M:%S")

        if SCHLUESSEL:
            nachricht = ("%s\n%s\n%d" % (name_roh, id_roh, laenge)).encode()
            soll = hmac.new(SCHLUESSEL, nachricht, hashlib.sha256).hexdigest()
            ist = self.headers.get("X-Scan-Auth", "")
            if not hmac.compare_digest(soll, ist):
                print("[%s] REJECTED %s from %s: signature %s" % (
                    stempel, name, self.client_address[0], "missing" if not ist else "wrong"))
                self._antwort(401, "rejected: signature missing or wrong")
                return

        fehler = vollstaendig(name, daten, laenge)
        if fehler:
            print("[%s] REJECTED %s: %s" % (stempel, name, fehler))
            self._antwort(400, "rejected: " + fehler)
            return

        ordner = PROTOKOLL if name.startswith("scanlog-") else INBOX
        with SPERRE:
            if id_bekannt(scan_id):
                print("[%s] already received (%s), discarded: %s (%d bytes)" % (
                    stempel, scan_id, name, len(daten)))
                self._antwort(200, "OK (duplicate, not stored again)", duplikat=True)
                return
            ziel = ablegen(ordner, name, daten)   # file safe first, then the id
            id_merken(scan_id)
        print("[%s] received: %s (%d bytes, id %s) from %s" % (
            stempel, os.path.basename(ziel), len(daten),
            scan_id or "none", self.client_address[0]))
        self._antwort(200, "OK")

    def do_GET(self):
        self._antwort(200, "scan-receiver is running")

    def log_message(self, *a):
        pass  # our own output is enough


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    print("scan-receiver on 0.0.0.0:%d, inbox %s" % (PORT, INBOX))
    Server(("0.0.0.0", PORT), Handler).serve_forever()
