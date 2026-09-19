# Scan-Stick

Ein USB-Stick, der Scans selbst ins Netz weiterreicht.

Ein LILYGO **T-Dongle-S3** steckt im USB-Anschluss eines Druckers und gibt sich als
gewöhnlicher USB-Speicher aus. Der Drucker scannt wie immer „an USB". Der Stick merkt,
wann eine Datei fertig geschrieben ist, gibt ihr einen Namen mit Zeitstempel, lädt sie
per HTTP an einen beliebigen Empfänger und räumt sie weg. Ein kleines Display zeigt,
was gerade passiert; eine Weboberfläche zeigt Zustand, Protokoll und die Dateien auf
der Karte.

Entstanden an einem **HP PageWide Color MFP 780**, im Prinzip aber an jedem Gerät
brauchbar, das „Scan an USB-Laufwerk" beherrscht.

## Warum nicht einfach „Scan in Netzwerkordner"?

Weil der Drucker dabei bei **jedem** Scan erst prüft, ob er sein Ablageziel erreicht —
das kostet spürbar Zeit, bevor überhaupt Papier eingezogen wird. Gegenüber dem Stick
ist das Laufwerk sofort da; der Netzwerkteil passiert erst danach und stört den
Bedienablauf nicht mehr. Außerdem bleibt das Ziel austauschbar: Der Stick kennt nur
eine URL, alles Weitere macht der Empfänger.

## Ablauf

```
Drucker schreibt Sektoren   →   Stick erkennt Schreibzugriffe
                                 ↓
                            liest das Dateiverzeichnis roh mit (stört den Drucker nicht)
                                 ↓
                            Datei vollständig?  (%%EOF vorhanden, 3 s Ruhe)
                                 ↓
                            Medium kurz abmelden  ← erst jetzt gefahrlos
                                 ↓
                            umbenennen → hochladen → löschen oder nach /gesendet
                                 ↓
                            Medium wieder anmelden
```

## Hardware

**LILYGO T-Dongle-S3** (ESP32-S3, 16 MB Flash, microSD im USB-A-Stecker, ST7735-Display,
APA102-LED). Eine microSD mit **FAT32** gehört hinein — 64 GB funktionieren, entgegen
mancher Behauptung; sie müssen nur FAT32 statt exFAT sein.

| Funktion | Pins |
|---|---|
| SD (SD_MMC, 4 Bit) | CLK 12, CMD 16, D0 14, D1 17, D2 21, D3 18 |
| Display ST7735 | CS 4, SDA 3, SCL 5, DC 2, RST 1, Licht 38 (aktiv low) |
| Status-LED APA102 | Daten 40, Takt 39, BGR |

## Installation

### 1. Karte vorbereiten

Eine microSD mit **FAT32**. Karten bis 32 GB sind ab Werk meist schon so formatiert und
können direkt hinein. Größere kommen als exFAT, das der Stick nicht lesen kann — sie
müssen einmalig auf FAT32 umformatiert werden, **in einem richtigen Kartenleser**:

```bash
# Linux/macOS, /dev/sdX durch das tatsächliche Gerät ersetzen – Vorsicht, löscht alles
sudo parted -s /dev/sdX mklabel msdos
sudo parted -s /dev/sdX mkpart primary fat32 1MiB 100%
sudo parted -s /dev/sdX set 1 lba on
sudo mkfs.vfat -F 32 -n SCANS /dev/sdX1
```

Unter Windows tut es ein Werkzeug wie „FAT32 Format", die Bordmittel bieten FAT32
oberhalb von 32 GB nicht an. **Nicht** über den eingesteckten Stick formatieren: Der
hält die Dauerschreiblast eines Formatiervorgangs nicht durch und bricht ab.

### 2. Firmware aufspielen

Einmalig über Kabel, siehe [Bauen und Flashen](#bauen-und-flashen). Fertige Abbilder
liegen unter [Releases](../../releases).

### 3. WLAN-Zugangsdaten eintragen

Dafür ist **kein Kartenleser nötig** — der Stick ist ja selbst ein USB-Laufwerk:

1. Stick in den Computer stecken, es erscheint ein Laufwerk namens **SCANS**
2. darauf eine Textdatei **`wifi.cfg`** anlegen (Vorlage: `wifi.cfg.beispiel`):

```
ssid=MeinWLAN
pass=MeinPasswort
endpoint=http://192.168.1.50:8080/scan
```

3. Laufwerk auswerfen, Stick abziehen

Beim nächsten Start liest er die Datei und **spiegelt die Zugangsdaten in seinen
Flash-Speicher**. Danach kommen WLAN und Weboberfläche auch dann hoch, wenn die Karte
fehlt oder unlesbar ist — sonst hätte man genau dann keine Diagnose, wenn man sie
braucht. Die Datei bleibt liegen und hat weiterhin Vorrang; zum Ändern des Netzes
genügt es, sie zu überschreiben.

> Einen Einrichtungsassistenten über ein eigenes WLAN des Sticks gibt es noch nicht,
> siehe [Offene Punkte](#offene-punkte).

### 4. In den Drucker

Stick in den USB-Anschluss, etwa zehn Sekunden warten, bis das Display **BEREIT** zeigt,
dann am Gerät „Scan an USB-Laufwerk" wählen. Die Adresse der Weboberfläche steht im
Protokoll und wird beim Start angezeigt.

## Bauen und Flashen

```bash
arduino-cli compile -b "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled,FlashMode=qio,CPUFreq=240" --export-binaries firmware/scanner
```

`default_8MB` ist wichtig: zwei Programmbereiche, damit Aktualisierungen über WLAN
möglich sind. Mit `app3M_fat9M_16MB` gäbe es nur einen und jede abgebrochene
Übertragung wäre fatal.

Flashen über Kabel:

```bash
# 1. Die laufende Firmware in den Flash-Modus holen (Port kurz mit 1200 Baud öffnen)
python3 -c 'import serial,time; s=serial.Serial("/dev/ttyACM0",1200); s.dtr=False; time.sleep(0.3); s.close()'
sleep 4
# 2. Vollständig schreiben
esptool --chip esp32s3 --port /dev/ttyACM0 --no-stub --before default_reset --after no_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     firmware/scanner/build/*/scanner.ino.bootloader.bin \
  0x8000  firmware/scanner/build/*/scanner.ino.partitions.bin \
  0xe000  firmware/scanner/build/*/boot_app0.bin \
  0x10000 firmware/scanner/build/*/scanner.ino.bin
# 3. Stick abziehen, 5 s warten, wieder einstecken
```

Danach geht es bequemer: Weboberfläche → **Firmware** → `scanner.ino.bin` hochladen.

## Fallen, die uns Stunden gekostet haben

**Nach dem Flashen hilft nur echtes Abziehen.** Der ESP32-S3 bleibt sonst im Flash-Modus
stecken: kein Laufwerk, keine serielle Ausgabe, keine Firmware. Weder ein Software-Reset
noch das Abschalten der Anschlussspannung per `uhubctl` holen ihn heraus — auch nicht
nach zwanzig Sekunden. Nur physisch ausstecken und wieder einstecken.
Test auf diesen Zustand: `esptool --before no_reset read_mac` — **verbindet** er sich,
läuft **keine** Firmware.

**Serielle Ausgaben gibt es im Betrieb nicht.** Die Konsole gehört im
Massenspeicher-Betrieb dem USB-Stack. Deshalb protokolliert der Stick in den
Arbeitsspeicher und zeigt es über die Weboberfläche.

**Dem Drucker das Medium zu entziehen, während er schreibt, zerstört den Scan.**
Die Cluster sind dann belegt, der Verzeichniseintrag fehlt — die Datei existiert nie.
Erst **nachdem** eine fertige Datei gefunden wurde, ist das Abmelden gefahrlos.

**Umgekehrt darf man die Karte nicht ändern, während der Drucker sie sieht.**
Er hält eine eigene, gepufferte Sicht auf das Verzeichnis und schreibt sie später
zurück — Umbenennungen und ganze Ordner verschwinden dadurch wieder.

**Die Dateigröße ist kein Zeichen für „fertig".** Der 780 trägt die endgültige Größe
ein, *bevor* er schreibt, und reserviert den Platz. Wer darauf vertraut, lädt eine
halb beschriebene Datei hoch, deren hinterer Teil aus Leerbytes besteht. Deshalb prüft
der Stick die Endmarke `%%EOF`.

**Das Verzeichnis zum Nachsehen ab- und wieder anzuhängen ist gefährlich.** Läuft das
parallel zu einem Lesezugriff des Druckers, greift der USB-Teil auf einen abgeräumten
Kartentreiber zu und das Gerät startet neu. Ein Blick auf die Weboberfläche darf den
Betrieb nie gefährden — deshalb liest der Stick das Verzeichnis roh mit.

**Bei mehreren Zugangspunkten mit derselben Kennung** nimmt `WiFi.begin(ssid, pass)`
den erstbesten, nicht den stärksten. Der Stick sucht deshalb vorher und verbindet sich
gezielt mit der besten Station.

## Weboberfläche

Erreichbar über `http://scanstick.local/` oder die angezeigte Adresse.

| Seite | Inhalt |
|---|---|
| Status | Karte, Empfang samt gewähltem Zugangspunkt, offene Schreibvorgänge, Uhrzeit, Laufzeit |
| Protokoll | vollständiger Ablauf seit dem Start |
| Dateien | Inhalt der Karte, einzeln herunterladbar |
| Roh | was der Stick ohne Dateisystem-Treiber sieht (Diagnose) |
| Einstellungen | siehe unten |
| Firmware | Aktualisierung über WLAN |

Ein **Passwort** lässt sich setzen (Benutzername `scan`). Ohne kann jedes Gerät im
selben Netz die Scans herunterladen.

## Einstellungen

Upload-Ziel · Namensanfang der Dateien (z. B. Standortkennung) · Ruhefrist ·
nach dem Senden löschen oder nach `/gesendet` verschieben · Passwort ·
Helligkeit der Status-LED (0 = aus) · Farbumkehr des Displays ·
acht Zustandsfarben für Display und LED

## Empfänger

`empfaenger/scan-receiver.py` ist ein Beispiel in reinem Python ohne Abhängigkeiten:
nimmt `POST /scan?name=…` entgegen und legt die Datei ab.

```bash
python3 empfaenger/scan-receiver.py    # lauscht auf Port 8080, legt in ~/scan-inbox ab
```

Für den Produktivbetrieb tritt hier etwas anderes an die Stelle — Ablage in einer
Cloud, einem Dokumentensystem oder einem Ordner. Die Firmware kennt bewusst nur eine
URL, damit das Ziel austauschbar bleibt.

### Wo der Empfänger laufen sollte

Das Skript läuft ohne Zusatzpakete auf Linux, macOS und Windows. Entscheidend ist aber
nicht das Betriebssystem, sondern die **Verfügbarkeit**: Der Empfänger muss laufen,
wenn jemand scannt. Schläft der Rechner, bleibt die Datei auf der Karte liegen und wird
beim nächsten Anlauf erneut versucht — angekommen ist sie aber nicht.

| Gastgeber | Eignung |
|---|---|
| Raspberry Pi | ideal: läuft durch, wenig Strom, als Dienst einrichtbar |
| NAS | sehr gut, Ablage direkt am Ziel |
| Server / VM | gut, sofern vom Stick erreichbar |
| Arbeitsplatzrechner | nur solange er wach ist |

### Firewall und Autostart

**macOS** — beim ersten Start erscheint „Eingehende Netzwerkverbindungen zulassen?",
das muss erlaubt werden (nachträglich unter *Systemeinstellungen → Netzwerk → Firewall
→ Optionen*). Port 8080 braucht keine Administratorrechte. Dauerhaft über einen
LaunchAgent in `~/Library/LaunchAgents/`; der Rechner darf dann nicht in den
Ruhezustand gehen.

**Windows** — die Defender-Firewall fragt beim ersten Start nach; Haken bei *Privates
Netzwerk*, öffentliche Netzwerke nicht. Nachträglich: *Eingehende Regel → Port → TCP
8080 → zulassen*, Profil „Privat". Dauerhaft über die Aufgabenplanung („Beim Start des
Computers") oder als Dienst.

**Linux** — falls eine Firewall aktiv ist: `sudo ufw allow 8080/tcp` beziehungsweise
`firewall-cmd --add-port=8080/tcp --permanent`. Dauerhaft als systemd-Unit:

```ini
# /etc/systemd/system/scan-receiver.service
[Unit]
Description=Scan-Stick Empfaenger
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 /opt/scanstick/scan-receiver.py
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now scan-receiver
```

**Zwei Dinge, die man leicht vergisst:**

1. Der Empfänger braucht eine **feste Adresse** — im Router reserviert oder statisch
   vergeben. Bekommt er per DHCP eine neue, zeigt `endpoint=` ins Leere, und zwar
   irgendwann mitten im Betrieb.
2. Er nimmt **alles** entgegen, was an ihn geschickt wird. Im Heimnetz ist das
   vertretbar, aus dem Internet erreichbar sollte er nicht sein.

## Anzeige

| Anzeige | Bedeutung |
|---|---|
| STROM | fährt hoch |
| BEREIT | wartet, mit Empfangsbalken |
| OK | Drucker greift gerade zu |
| SCAN ERKANNT | Schreibvorgang bemerkt, zeigt Countdown oder erkannte Größe |
| SENDET | Fortschrittsbalken mit Prozent und Dateiname |
| WARTE | Drucker noch nicht fertig, mit Versuchszähler |
| SD?! | Karte nicht lesbar |

## Offene Punkte

- **Einrichtungsassistent**: Sind keine Zugangsdaten hinterlegt, soll der Stick ein
  eigenes WLAN aufspannen, in dem man Netz und Ziel im Browser einträgt — dann braucht
  es gar keine Datei mehr
- **Mehrere WLAN-Netze** hinterlegen und beim Suchlauf über alle bekannten hinweg das
  stärkste wählen — damit derselbe Stick an verschiedenen Standorten läuft
- Selbsttätiger Wechsel des Zugangspunkts, wenn der Empfang längere Zeit schwach bleibt
- Beim Start einmal nachsehen, ob eine Datei liegengeblieben ist
- Auch andere Dateitypen als PDF auf Vollständigkeit prüfen
- Weitere Boards, insbesondere solche ohne Kartensteckplatz (dort müsste der interne
  Flash als Speicher dienen)

## Lizenz

MIT, siehe `LICENSE`.
