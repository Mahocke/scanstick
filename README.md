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
                            liest die Datei roh entlang der Belegungskette und lädt sie hoch
                            — der Drucker hat das Medium die ganze Zeit
                                 ↓
                            merkt sich im Flash: gesendet (Startcluster, Größe, Kennzahl)
                                 ↓
                            später, wenn der Drucker n Minuten nichts angefasst hat:
                            Medium für unter eine Sekunde weg, Gesendetes löschen
                            oder nach /gesendet, Geister austragen
```

**Im Betrieb wird das Medium nie abgemeldet.** Der Stick fasst weder den Dateisystem-Treiber
noch die Karte an, solange gescannt wird: Er liest Verzeichnis, Belegungstabelle und
Datenblöcke direkt aus den Sektoren. Umbenennen ist unnötig. Was gesendet ist, steht in einer
Merkliste im Flash (Startblock, Größe, Kennzahl) und wird nicht erneut gesendet. Liegt die
gesendete Datei noch da, fragt der 780 beim nächsten Scan „Datei bereits vorhanden" —
„Ersetzen" ist richtig, die neue Fassung gilt als neu.

Aufgeräumt wird nur in einer Ruhephase: nach einer einstellbaren Zeit ohne jeden Zugriff des
Druckers (Vorgabe 3 Minuten — der 780 fasst den Stick zwischen Jobs nicht an), wenn die Merkliste voll wird, oder auf Knopfdruck in der
Weboberfläche. Das ist der einzige Moment, in dem das Medium kurz weg ist, und dann steht
niemand am Gerät. Frühere Fassungen nahmen dem Drucker das Medium bei jedem Scan weg, zuerst
für die Dauer des Uploads, zuletzt für eine halbe Sekunde; jeder Job, der genau dann startete
oder während der Drucker den Stick neu einhängte, scheiterte.

Die Karte sollte **klein partitioniert** sein, etwa 4 GB mit 32-kB-Clustern: Nach jedem
Aufräumen hängt der Drucker den Stick neu ein und liest dabei die Belegungstabelle. Bei
64 GB sind das 60 MB über USB, bei 4 GB mit großen Clustern 512 kB.

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
sudo parted -s /dev/sdX mkpart primary fat32 1MiB 4097MiB   # 4 GB reichen, siehe Ablauf
sudo parted -s /dev/sdX set 1 lba on
sudo mkfs.vfat -F 32 -s 64 -n SCANS /dev/sdX1                 # 32-kB-Cluster: kleine Belegungstabelle
```

Unter Windows tut es ein Werkzeug wie „FAT32 Format", die Bordmittel bieten FAT32
oberhalb von 32 GB nicht an. Eine kleine Partition lässt sich auch über den eingesteckten
Stick anlegen (4 GB mit großen Clustern in vier Sekunden); eine 64-GB-Partition dagegen
nicht, das Formatieren schreibt dann minutenlang und der Stick bricht ab.

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

Mehrere Netze sind erlaubt, bis zu vier: Jede `ssid=`-Zeile beginnt ein neues Netz, das
folgende `pass=` gehört dazu. Beim Suchlauf gewinnt über alle bekannten Netze hinweg der
stärkste Zugangspunkt, so läuft derselbe Stick an mehreren Standorten oder am Hotspot.

**Nur 2,4 GHz.** Der ESP32 sieht keine 5-GHz-Netze. Ein iPhone-Hotspot sendet standardmäßig
auf 5 GHz und bleibt für den Stick unsichtbar, ohne jede Fehlermeldung; erst mit
*Kompatibilität maximieren* in den Hotspot-Einstellungen wechselt er auf 2,4 GHz. Bei
Dual-Band-Routern muss das Netz ebenfalls auf 2,4 GHz sichtbar sein.

```
ssid=Buero
pass=geheim1
ssid=Zuhause
pass=geheim2
endpoint=http://192.168.1.50:8080/scan
```

3. Laufwerk auswerfen, Stick abziehen

Beim nächsten Start liest er die Datei und **spiegelt die Zugangsdaten in seinen
Flash-Speicher**. Danach kommen WLAN und Weboberfläche auch dann hoch, wenn die Karte
fehlt oder unlesbar ist — sonst hätte man genau dann keine Diagnose, wenn man sie
braucht. Die Datei bleibt liegen. Übernommen wird sie nur, wenn sich ihr Inhalt seit dem
letzten Mal **geändert** hat; sonst gelten die Werte aus dem Flash, also auch alles, was in
der Weboberfläche eingestellt wurde. Zum Ändern des Netzes genügt es, sie zu überschreiben.

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

**Der Drucker schreibt seine alte Sicht sogar nach dem Wiederanmelden zurück.** Beim
nächsten Scan taucht der vorige Scan als „Geist" erneut in der Wurzel auf: ein
Verzeichniseintrag, der auf die Blöcke der schon gesendeten Datei zeigt. Ihn per
Dateisystem zu löschen gäbe diese Blöcke frei, also die Daten der anderen Datei; ihn zu
verschieben hinterließe zwei Einträge auf denselben Blöcken (Kreuzverkettung). Der Stick
lädt ihn deshalb hoch, der Empfänger erkennt ihn an der Kennung als Duplikat, und der
Stick trägt daraufhin nur den Verzeichniseintrag roh aus, ohne die Blockzuordnung
anzufassen. Ein eigener Empfänger muss das Wort `Duplikat` in seiner Antwort tragen.

**Die Dateigröße ist kein Zeichen für „fertig".** Der 780 trägt die endgültige Größe
ein, *bevor* er schreibt, und reserviert den Platz. Wer darauf vertraut, lädt eine
halb beschriebene Datei hoch, deren hinterer Teil aus Leerbytes besteht. Deshalb prüft
der Stick die Endmarke `%%EOF`.

**Das Verzeichnis zum Nachsehen ab- und wieder anzuhängen ist gefährlich.** Läuft das
parallel zu einem Lesezugriff des Druckers, greift der USB-Teil auf einen abgeräumten
Kartentreiber zu und das Gerät startet neu. Ein Blick auf die Weboberfläche darf den
Betrieb nie gefährden — deshalb liest der Stick das Verzeichnis roh mit.

**„Medium abmelden" stoppt keinen laufenden Zugriff.** `mediaPresent(false)` weist nur
*neue* Kommandos ab. Ein Lesevorgang, der gerade läuft, läuft weiter — ein Linux-Host
liest beim Aushängen die FAT in 120-kB-Blöcken, das dauert länger als jede feste
Wartezeit. Wird der Kartentreiber in dieser Zeit abgebaut, hängt der USB-Teil, und nach
fünf Sekunden startet der Task-Watchdog das Gerät neu (im Kernel-Log des Hosts steht dann
`cmd_age=5s`). Der Stick zählt deshalb laufende Zugriffe mit und fasst die Karte erst an,
wenn keiner mehr offen ist. Aus demselben Grund gilt als „Ruhe" erst, wenn der Host weder
schreibt **noch liest**.

**Ein Neustart am Drucker ist ein Stromschnitt.** Meldet sich der Stick per USB ab, schaltet
der 780 dem Port kurz die Versorgung weg. Nach einem Firmware-Update über die
Weboberfläche kam die neue Firmware dadurch nie dazu, sich als gültig zu bestätigen — der
Bootloader fiel zweimal auf die alte zurück. Der Stick meldet deshalb erst das Medium ab,
trennt USB, wartet den Stromschnitt ab und startet erst dann neu (`sanftNeustarten`).
Auf der Statusseite steht danach „Strom eingeschaltet" als Startgrund — das ist normal.

**Der 780 überschreibt eine gleichnamige Datei.** Bleibt `[Untitled].pdf` nach dem Senden
auf der Karte liegen, hängt er beim nächsten Scan keinen Zeitstempel an, sondern schreibt
dieselbe Datei neu. Der Stick erkennt eine Datei deshalb nicht am Namen, sondern an
Startblock, Größe und Kennzahl — die neue Fassung gilt damit als neu und wird gesendet.

**Nicht jeder Schreibzugriff ist ein Scan.** Beim Öffnen des Scan-Dialogs schreibt der
780 eine Testdatei von einem Block und löscht sie gleich wieder (rund 4 kB: FAT, Wurzel,
ein Datenblock, wieder Wurzel und FAT). Die Anzeige zählt deshalb nur, was noch nicht
gesendet ist; die Statusseite zeigt die letzten Schreibzugriffe des Hosts nach Bereich.

**Der Drucker lehnt eine beschädigte Karte ab.** Nach abgebrochenen Jobs und
zurückgeschriebenen Verzeichnissen blieben Kreuzverkettungen, verwaiste Blöcke und die
Schmutzmarke auf der Karte — und der 780 zeigte nur noch „USB-Stick anschließen". Der
Stick prüft die Karte deshalb selbst, beim Start (bevor der Drucker sie sieht) und im
Aufräumfenster: beide Zuordnungstabellen abgleichen, jede Kette ablaufen, doppelt belegte
Blöcke dem zuerst gefundenen Eintrag lassen und den zweiten austragen, Waisen freigeben, zu
kurze Ketten kürzen, Schmutzmarke setzen. Ist das Dateisystem zweimal in Folge unlesbar,
legt er die Partition neu an (`POST /formatieren` tut das auch auf Knopfdruck, nur wenn
nichts Ungesendetes liegt). Das Ergebnis steht auf der Statusseite unter „Kartenprüfung".
Der Prüfstand hat dafür Szenario E.

**Nach dem Senden löschen, nicht verschieben.** Ein nach `/gesendet` verschobener Eintrag
behält seine Blöcke. Schreibt der Drucker danach seinen alten Wurzeleintrag zurück und
ersetzt ihn, gibt er genau diese Blöcke frei — Kreuzverkettung. Gelöschte Blöcke sind
frei, da kann ein Geist nichts mehr anrichten. Die Kopien liegen ohnehin beim Empfänger.

**Bei mehreren Zugangspunkten mit derselben Kennung** nimmt `WiFi.begin(ssid, pass)`
den erstbesten, nicht den stärksten. Der Stick sucht deshalb vorher (`WiFiMulti`) und
verbindet sich gezielt mit der besten Station. Die Kehrseite: Fällt genau diese Station
aus, versucht der automatische Wiederverbinder nur sie. Nach zwei Minuten ohne Netz sucht
der Stick deshalb von vorn, über alle bekannten Netze.

## Weboberfläche

Erreichbar über `http://scanstick.local/` oder die angezeigte Adresse.

| Seite | Inhalt |
|---|---|
| Status | Karte, Empfang samt gewähltem Zugangspunkt, offene Schreibvorgänge, Uhrzeit, Laufzeit |
| Protokoll | vollständiger Ablauf seit dem Start |
| Dateien | Wurzel roh gelesen mit Stand (gesendet, offen, Geist), Ordner, einzeln herunterladbar |
| Roh | was der Stick ohne Dateisystem-Treiber sieht (Diagnose) |
| Einstellungen | siehe unten |
| Firmware | Aktualisierung über WLAN |

Ein **Passwort** lässt sich setzen (Benutzername `scan`). Ohne kann jedes Gerät im
selben Netz die Scans herunterladen.

Alles, was etwas verändert (Einstellungen, „Jetzt schauen", Neustart, Firmware), geht
nur per `POST` und nur von der eigenen Seite: Schickt ein Browser eine `Origin`- oder
`Referer`-Kopfzeile mit, muss sie zum Stick passen. Sonst könnte eine beliebige fremde
Webseite im selben Browser das Upload-Ziel umbiegen oder den Stick mitten im Upload neu
starten, denn ein gespeichertes Passwort schickt der Browser automatisch mit. Werkzeuge
wie `curl` schicken keine `Origin`-Kopfzeile und funktionieren weiter:

```bash
curl -u scan:PASSWORT --data-urlencode 'endpoint=http://192.168.1.50:8080/scan' http://scanstick.local/einstellungen
curl -u scan:PASSWORT -X POST http://scanstick.local/neustart
```

## Einstellungen

Upload-Ziel · Namensanfang der Dateien (z. B. Standortkennung) · Ruhefrist · Aufräumen nach n Minuten Ruhe ·
nach dem Senden löschen oder nach `/gesendet` verschieben · Passwort ·
Geräteschlüssel für den Upload · Helligkeit der Status-LED (0 = aus) ·
Farbumkehr des Displays · acht Zustandsfarben für Display und LED

Der **Geräteschlüssel** signiert jeden Upload (HMAC-SHA256 über Name, Kennung und Länge,
Kopfzeile `X-Scan-Auth`). Derselbe Schlüssel gehört in den Empfänger (`SCAN_KEY`), der dann
alles ohne gültige Signatur abweist. Er lässt sich auch in `wifi.cfg` als `schluessel=`
hinterlegen. Bewusst kein TLS: Im LAN reicht das, ein TLS-Kontext kostet auf dem Stick rund
40 kB Arbeitsspeicher und jeden Upload spürbar Zeit. Wiederholungen brauchen keinen
Zeitstempel, der Empfänger erkennt sie an der Kennung.

Liegt eine Datei nach einem Fehlversuch noch auf der Karte (Empfänger nicht erreichbar,
WLAN weg), sieht der Stick **alle zehn Minuten** roh nach und versucht es erneut.

## Empfänger

`empfaenger/scan-receiver.py` ist ein Beispiel in reinem Python ohne Abhängigkeiten:
nimmt `POST /scan?name=…` entgegen und legt die Datei ab.

```bash
python3 empfaenger/scan-receiver.py    # lauscht auf Port 8080, legt in ~/scan-inbox ab
SCAN_PORT=9000 SCAN_INBOX=/srv/scans SCAN_KEY=geheim python3 empfaenger/scan-receiver.py
```

Mit `SCAN_KEY` verlangt er zu jedem Upload die Signatur des Sticks (siehe Einstellungen)
und antwortet sonst mit 401.

Der Empfänger nimmt eine Datei nur an, wenn sie **vollständig** ist: Die Länge muss der
Ankündigung entsprechen, eine PDF muss auf `%%EOF` enden. Sonst antwortet er mit 400
und merkt sich nichts, der Stick versucht es später erneut. Das ist wichtig, weil der
Stick Wiederholungen über eine Kennung meldet und der Empfänger sie verwirft: Würde
er einen abgerissenen Upload verkürzt ablegen und die Kennung merken, gälte die
Wiederholung als Duplikat, und der Stick löschte daraufhin das einzige vollständige
Exemplar. Das Protokoll des Sticks (`scanlog-*.txt`) landet im Unterordner `protokoll/`.

Für den Produktivbetrieb tritt hier etwas anderes an die Stelle — Ablage in einer
Cloud, einem Dokumentensystem oder einem Ordner. Die Firmware kennt bewusst nur eine
URL, damit das Ziel austauschbar bleibt. Wer einen eigenen Empfänger schreibt, sollte
die drei Regeln übernehmen: Länge prüfen, bei PDF `%%EOF` prüfen, Kennung erst nach
erfolgreicher Ablage merken.

## Prüfstand ohne Drucker

`test/stresstest.sh` läuft auf einem Linux-Rechner, an dem der Stick steckt (ein
Raspberry Pi eignet sich). Der Rechner spielt den Drucker: mounten, Test-PDFs
schreiben, auswerfen. Ein Empfänger auf demselben Rechner prüft, ob jede Datei
vollständig und genau einmal ankommt. Drei Szenarien: eine Datei, zwei in einem Zug,
und eine zweite Datei im engsten Moment, sobald das Medium nach dem ersten Upload
wieder da ist.

```bash
sudo SCAN_WEBPASS=PASSWORT test/stresstest.sh http://scanstick.local /dev/sda1
```

Das Upload-Ziel des Sticks wird für die Dauer des Tests umgestellt und danach
zurückgesetzt. Die Partition wird nur angefasst, wenn sie zu einem Espressif-USB-Gerät
gehört und `SCANS` heißt. Der Linux-Treiber schreibt Verzeichniseinträge früher und
anders als ein Drucker; der Prüfstand ersetzt den Drucktest nicht, macht aber
Regressionen wiederholbar sichtbar.

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
ExecStart=/usr/bin/python3 -u /opt/scanstick/empfaenger/scan-receiver.py
Environment=SCAN_PORT=8080
Environment=SCAN_INBOX=/srv/scans
Environment=SCAN_KEY=geheim
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
- Selbsttätiger Wechsel des Zugangspunkts, wenn der Empfang längere Zeit schwach bleibt
  (bei komplettem Ausfall sucht er nach zwei Minuten neu; bei nur schwachem Empfang noch nicht)
- Auch andere Dateitypen als PDF auf Vollständigkeit prüfen
- Weitere Boards, insbesondere solche ohne Kartensteckplatz (dort müsste der interne
  Flash als Speicher dienen)

## Lizenz

MIT, siehe `LICENSE`.
