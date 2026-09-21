# Scan-Stick

A USB stick that passes scans on to the network by itself.

A LILYGO **T-Dongle-S3** sits in the USB port of a printer and presents itself as an
ordinary USB storage device. The printer scans "to USB" as always. The stick notices
when a file has finished writing, gives it a name with a timestamp, uploads it over
HTTP to an arbitrary receiver and clears it away. A small display shows what is
happening right now; a web interface shows state, log and the files on the card.

Developed on an **HP PageWide Color MFP 780**. Other devices that can do "scan to USB
drive" may work, but not all of them do: some printers ignore the stick completely.
Please read **Printer compatibility** before you buy hardware for this.

## Why not just "scan to network folder"?

Because the printer then checks on **every** scan whether it can reach its destination -
that costs noticeable time before any paper is even pulled in. Compared to the stick,
the drive is there immediately; the network part only happens afterwards and no longer
disturbs the workflow. The destination also stays interchangeable: the stick only knows
a URL, the receiver does everything else.

## Flow

```
Printer writes sectors      →   Stick detects write accesses
                                 ↓
                            reads the directory raw alongside (does not disturb the printer)
                                 ↓
                            file complete?  (%%EOF present, 3 s of quiet)
                                 ↓
                            reads the file raw along the allocation chain and uploads it
                            - the printer has the medium the whole time
                                 ↓
                            records in flash: sent (start cluster, size, checksum)
                                 ↓
                            later, once the printer has not touched anything for n minutes:
                            medium gone for less than a second, delete what was sent,
                            remove ghosts, check and heal the card
```

**During operation the medium is never unmounted.** The stick touches neither the file system
driver nor the card while scanning is going on: it reads directory, allocation table and
data blocks directly from the sectors. Renaming is unnecessary. What has been sent is kept in a
list in flash (start block, size, checksum) and is not sent again. If the sent file is still
there, the 780 asks "file already exists" on the next scan - "replace" is the right answer, the
new version counts as new.

Cleanup only happens in a quiet phase: after a configurable time without any access by the
printer (default 3 minutes - the 780 does not touch the stick between jobs), when the sent list
fills up, or at the push of a button in the web interface. That is the only moment in which the
medium is briefly gone, and then nobody is standing at the device. Earlier versions took the
medium away from the printer on every scan, at first for the duration of the upload, later for
half a second; every job that started exactly then, or while the printer was remounting the
stick, failed.

The card should be **partitioned small**, about 4 GB with 32 kB clusters: after every
cleanup the printer remounts the stick and reads the allocation table while doing so. With
64 GB that is 60 MB over USB, with 4 GB and large clusters 512 kB.

## Printer compatibility

**Not every printer accepts the stick.** It is an ESP32-S3 pretending to be a USB drive,
and some printer USB ports are pickier than a PC is. Known so far:

| Printer | Firmware | Result |
|---|---|---|
| HP PageWide Color MFP 780 | FutureSmart 5.9.2.3 | works, development device |
| HP PageWide Color MFP 774 | FutureSmart 5.9.2.3 | works, same firmware family as the 780 |
| HP PageWide Pro 377dw | 2506A | **refuses it** - the port never enumerates the stick at all |

On the 377dw the stick is not merely rejected after mounting: the port never configures
it. No sector accesses arrive, and the USB stack never reports a `STARTED` event, while
the very same firmware starts within a second on a PC or a Raspberry Pi. It made no
difference whether the serial console was disabled, the reported capacity was capped at
8 GB, the current draw was declared as 100 mA, or the descriptor pretended to be an
ordinary SanDisk Cruzer Blade. "Scan to USB" and mass storage were enabled in the
printer's own settings.

The most likely cause is speed. The ESP32-S3 has a **full-speed** USB device port
(12 Mbit/s, USB 1.1); every off-the-shelf stick is high-speed. A walk-up port that only
expects high-speed devices will simply stay quiet. That is a property of the chip and
**cannot be fixed in firmware**.

### If your printer ignores the stick

1. **Check the printer's settings first.** "Scan to USB drive" and USB mass storage are
   switched off by default on many devices, and on some only in the embedded web server,
   not on the panel.
2. **Make sure it is a walk-up host port.** Some front USB sockets only accept firmware
   updates or service tools.
3. **Try a powered USB 2.0 hub in between.** A hub does the speed translation, so a
   full-speed device can appear behind it. This is the most promising workaround, but it
   is so far untested - if you try it, please report back in an issue.
4. **A normal USB stick working in that port proves little.** It only shows that the port
   carries walk-up storage at high speed, not that it talks to a full-speed device.

If the port stays silent, the printer is not going to work with this project. The
fallback is the classic one: let the printer **scan into a network folder** (SMB) and
have the receiving machine watch that folder. You lose the instant-drive advantage
described above, but everything downstream of the upload stays the same.

Reports about other printers are very welcome - see **Contributing**.

## Hardware

**LILYGO T-Dongle-S3** (ESP32-S3, 16 MB flash, microSD in the USB-A plug, ST7735 display,
APA102 LED). A microSD with **FAT32** belongs in it - 64 GB does work, contrary to some
claims; it only has to be FAT32 instead of exFAT.

| Function | Pins |
|---|---|
| SD (SD_MMC, 4 bit) | CLK 12, CMD 16, D0 14, D1 17, D2 21, D3 18 |
| Display ST7735 | CS 4, SDA 3, SCL 5, DC 2, RST 1, backlight 38 (active low) |
| Status LED APA102 | data 40, clock 39, BGR |

## Installation

### 1. Prepare the card

A microSD with **FAT32**. Cards up to 32 GB usually come formatted that way from the factory
and can go straight in. Larger ones arrive as exFAT, which the stick cannot read - they have
to be reformatted to FAT32 once, **in a real card reader**:

```bash
# Linux/macOS, replace /dev/sdX with the actual device - careful, erases everything
sudo parted -s /dev/sdX mklabel msdos
sudo parted -s /dev/sdX mkpart primary fat32 1MiB 4097MiB   # 4 GB is enough, see Flow
sudo parted -s /dev/sdX set 1 lba on
sudo mkfs.vfat -F 32 -s 64 -n SCANS /dev/sdX1                 # 32 kB clusters: small allocation table
```

On Windows a tool such as "FAT32 Format" does the job, the built-in tools do not offer FAT32
above 32 GB. A small partition can also be created through the plugged-in stick (4 GB with
large clusters in four seconds); a 64 GB partition cannot, formatting then writes for minutes
and the stick aborts.

### 2. Flash the firmware

Once over cable, see [Building and flashing](#building-and-flashing). Ready-made images
are available under [Releases](../../releases).

### 3. Enter the WiFi credentials

**The easy way: through the stick's own WiFi.** If the stick knows no network, or cannot
reach one, it opens its own WiFi named `scanstick-XXXX` (open; XXXX is also shown on the
display). Connect a phone or computer to it - the setup page opens by itself, as with a hotel
WiFi, otherwise open `http://192.168.4.1/`. There, pick a network from the list, enter
password, receiver address, optionally device key and web password, and save. The stick
restarts and is then reachable on the home network at `http://scanstick-XXXX.local/`.
For a network the stick already knows, an empty password field keeps the stored one.
Further networks can be added or removed later in the Settings.

**The second way: a file on the card.** For this too **no card reader is needed** - the
stick is itself a USB drive:

1. plug the stick into the computer, a drive named **SCANS** appears
2. create a text file **`wifi.cfg`** on it (template: `wifi.cfg.example`):

```
ssid=MyWiFi
pass=MyPassword
endpoint=http://192.168.1.50:8080/scan
```

(The keys are German by design: they are the file-format contract and stay as they are.)

Several networks are allowed, up to four: every `ssid=` line starts a new network, the
`pass=` that follows belongs to it. During the scan the strongest access point across all
known networks wins, so the same stick works at several locations or on a hotspot.

**2.4 GHz only.** The ESP32 does not see 5 GHz networks. An iPhone hotspot transmits on
5 GHz by default and stays invisible to the stick, without any error message; only with
*Maximize Compatibility* in the hotspot settings does it switch to 2.4 GHz. With dual-band
routers the network must likewise be visible on 2.4 GHz.

```
ssid=Office
pass=secret1
ssid=Home
pass=secret2
endpoint=http://192.168.1.50:8080/scan
```

3. eject the drive, unplug the stick

On the next start it reads the file and **mirrors the credentials into its flash
memory**. After that WiFi and web interface come up even when the card is missing or
unreadable - otherwise there would be no diagnostics exactly when they are needed. The
file stays where it is. It is only adopted if its content has **changed** since last
time; otherwise the values from flash apply, including everything that was set in the
web interface. To change the network it is enough to overwrite it.

### 4. Into the printer

Plug the stick into the USB port, wait about ten seconds until the display shows **READY**,
then choose "scan to USB drive" at the device. The address of the web interface is in the
log and is shown at startup.

## Building and flashing

```bash
arduino-cli compile -b "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=disabled,FlashMode=qio,CPUFreq=240" --export-binaries firmware/scanner
```

`default_8MB` matters: two program areas, so that updates over WiFi are possible. With
`app3M_fat9M_16MB` there would be only one and every aborted transfer would be fatal.

Flashing over cable:

```bash
# 1. Put the running firmware into flash mode (briefly open the port at 1200 baud)
python3 -c 'import serial,time; s=serial.Serial("/dev/ttyACM0",1200); s.dtr=False; time.sleep(0.3); s.close()'
sleep 4
# 2. Write completely
esptool --chip esp32s3 --port /dev/ttyACM0 --no-stub --before default_reset --after no_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0     firmware/scanner/build/*/scanner.ino.bootloader.bin \
  0x8000  firmware/scanner/build/*/scanner.ino.partitions.bin \
  0xe000  firmware/scanner/build/*/boot_app0.bin \
  0x10000 firmware/scanner/build/*/scanner.ino.bin
# 3. Unplug the stick, wait 5 s, plug it in again
```

After that it is more convenient: web interface -> **Firmware** -> upload `scanner.ino.bin`.

## Pitfalls that cost us hours

**After flashing, only a real unplug helps.** Otherwise the ESP32-S3 stays stuck in flash
mode: no drive, no serial output, no firmware. Neither a software reset nor switching off
the port power with `uhubctl` gets it out - not even after twenty seconds. Only physically
unplugging and plugging it in again.
Test for this state: `esptool --before no_reset read_mac` - if it **connects**, **no**
firmware is running.

**There is no serial output during operation.** In mass storage mode the console belongs
to the USB stack. That is why the stick logs into memory and shows it through the web
interface.

**Taking the medium away from the printer while it is writing destroys the scan.**
The clusters are then allocated, the directory entry is missing - the file never exists.
Only **after** a complete file has been found is unmounting safe.

**Conversely, the card must not be changed while the printer sees it.**
It keeps its own buffered view of the directory and writes it back later - renames and
whole folders disappear again as a result.

**The printer even writes its old view back after remounting.** On the next scan the
previous scan shows up in the root again as a "ghost": a directory entry that points at
the blocks of the file that was already sent. Deleting it through the file system would
free those blocks, that is the data of the other file; moving it would leave two entries
on the same blocks (cross-linking). The stick therefore uploads it, the receiver
recognizes it as a duplicate by its identifier, and the stick then only removes the
directory entry raw, without touching the block allocation. A custom receiver must carry
the word `Duplikat` in its reply.

**The file size is no sign of "finished".** The 780 writes the final size *before* it
writes, and reserves the space. Whoever relies on it uploads a half-written file whose
tail consists of empty bytes. That is why the stick checks for the end marker `%%EOF`.

**Unmounting and remounting the directory just to look is dangerous.** If that runs
in parallel with a read access of the printer, the USB part accesses a torn-down card
driver and the device restarts. A look at the web interface must never endanger
operation - that is why the stick reads the directory raw alongside.

**"Unmount medium" does not stop an access in progress.** `mediaPresent(false)` only
rejects *new* commands. A read that is currently running keeps running - a Linux host
reads the FAT in 120 kB blocks when unmounting, which takes longer than any fixed wait
time. If the card driver is torn down during that time, the USB part hangs, and after
five seconds the task watchdog restarts the device (the host's kernel log then shows
`cmd_age=5s`). The stick therefore counts accesses in progress and only touches the card
once none is open any more. For the same reason "quiet" only applies once the host is
neither writing **nor reading**.

**A restart at the printer is a power cut.** When the stick deregisters over USB, the 780
briefly cuts the power to the port. After a firmware update through the web interface the
new firmware therefore never got around to confirming itself as valid - the bootloader
fell back to the old one twice. The stick therefore first unmounts the medium, disconnects
USB, waits out the power cut and only then restarts (`sanftNeustarten`). The status page
afterwards shows "Power on" as the reset reason - that is normal.

**Only answer the dialog "Keep settings for the next job?" once the stick is done.**
The 780 keeps writing large scans long after the paper has gone through, and already shows
this dialog while doing so. Answering it with delete or cancel while the printer is still
writing aborts your own job: event 44.12.05 "Error writing multi-page image file".
After that the 780 briefly cuts the power to the USB port, and its USB host reads
**nothing at all** any more - not even an ordinary USB stick - until the printer is
restarted. Replugging does not help. As long as the stick's display shows "SCAN FOUND"
or the countdown, the printer is still writing; only at "SENDING" or "READY" is it done.

**The 780 overwrites a file of the same name.** If `[Untitled].pdf` stays on the card
after sending, the printer does not append a timestamp on the next scan but writes the
same file anew. The stick therefore recognizes a file not by its name but by start block,
size and checksum - so the new version counts as new and is sent.

**Not every write access is a scan.** When the scan dialog is opened, the 780 writes a
test file of one block and deletes it again right away (about 4 kB: FAT, root, one data
block, root and FAT again). The display therefore only counts what has not been sent yet;
the status page shows the host's last write accesses by area.

**The printer rejects a damaged card.** After aborted jobs and written-back directories,
cross-links, orphaned blocks and the dirty flag remained on the card - and the 780 only
showed "connect USB stick" any more. The stick therefore checks the card itself, at startup
(before the printer sees it) and in the cleanup window: compare both allocation tables, walk
every chain, leave doubly allocated blocks to the entry found first and remove the second,
free orphans, shorten chains that are too short, clear the dirty flag. If the file system is
unreadable twice in a row, it recreates the partition (`POST /formatieren` does that at the
push of a button too, only when nothing unsent is present). The result is shown on the status
page under "Card check". The test bench has scenario E for this.

**After sending, files are deleted, not moved.** An entry moved to `/gesendet` keeps its
blocks. If the printer then writes its old root entry back and replaces it, it frees exactly
those blocks - cross-linking. Deleted blocks are free, a ghost can do no more harm there.
That is why the option "move to /gesendet" no longer exists since v37; the copy is at the
receiver.

**With several access points using the same SSID**, `WiFi.begin(ssid, pass)` takes the
first one it finds, not the strongest. The stick therefore scans beforehand (`WiFiMulti`)
and connects to the best station on purpose. The downside: if exactly that station fails,
the automatic reconnector only tries that one. After two minutes without a network the
stick therefore scans again from scratch, across all known networks.

## Web interface

Reachable at `http://scanstick-XXXX.local/` (XXXX = the last four digits of the MAC
address, shown in the log and on the status page) or the displayed IP address. That way
several sticks on the same network do not get in each other's way.

| Page | Content |
|---|---|
| Status | card, reception including the chosen access point, open writes, time, uptime |
| Log | complete sequence since startup |
| Files | root read raw with state (sent, open, ghost), folders, individually downloadable |
| Raw | what the stick sees without a file system driver (diagnostics) |
| Settings | see below |
| Firmware | update over WiFi |

A **password** can be set (user name `scan`). Without one, every device on the same
network can download the scans.

Everything that changes something (settings, "Check now", restart, firmware) goes only
by `POST` and only from the stick's own page: if a browser sends an `Origin` or
`Referer` header, it has to match the stick. Otherwise any foreign web page in the same
browser could redirect the upload target or restart the stick in the middle of an upload,
because the browser sends a stored password automatically. Tools like `curl` send no
`Origin` header and keep working:

```bash
curl -u scan:PASSWORD --data-urlencode 'endpoint=http://192.168.1.50:8080/scan' http://scanstick-1a2b.local/einstellungen
curl -u scan:PASSWORD -X POST http://scanstick-1a2b.local/neustart
```

## Settings

Upload target - file name prefix (e.g. a site code) - quiet period - cleanup after n minutes of quiet -
delete after sending or move to `/gesendet` - password -
device key for the upload - brightness of the status LED (0 = off) -
color inversion of the display - eight state colors for display and LED

The **device key** signs every upload (HMAC-SHA256 over name, identifier and length,
header `X-Scan-Auth`). The same key belongs in the receiver (`SCAN_KEY`), which then
rejects everything without a valid signature. It can also be stored in `wifi.cfg` as
`schluessel=`. Deliberately no TLS: on the LAN that is enough, a TLS context costs about
40 kB of memory on the stick and noticeable time per upload. Repeats need no timestamp,
the receiver recognizes them by the identifier.

If a file is still on the card after a failed attempt (receiver unreachable, WiFi gone),
the stick looks again raw **every ten minutes** and retries.

## Receiver

`empfaenger/scan-receiver.py` is an example in pure Python without dependencies:
it accepts `POST /scan?name=...` and stores the file.

```bash
python3 empfaenger/scan-receiver.py    # listens on port 8080, stores in ~/scan-inbox
SCAN_PORT=9000 SCAN_INBOX=/srv/scans SCAN_KEY=secret python3 empfaenger/scan-receiver.py
```

With `SCAN_KEY` it requires the stick's signature for every upload (see Settings) and
otherwise answers with 401.

The receiver only accepts a file if it is **complete**: the length has to match the
announcement, a PDF has to end with `%%EOF`. Otherwise it answers with 400 and
remembers nothing, and the stick retries later. That matters because the stick reports
repeats via an identifier and the receiver discards them: if it stored a broken-off
upload truncated and remembered the identifier, the repeat would count as a duplicate,
and the stick would then delete the only complete copy. The stick's log
(`scanlog-*.txt`) ends up in the subfolder `protokoll/`.

In production something else takes its place here - storage in a cloud, a document
system or a folder. The firmware deliberately knows only a URL, so that the destination
stays interchangeable. Anyone writing their own receiver should adopt the three rules:
check the length, check `%%EOF` for PDFs, remember the identifier only after successful
storage.

## Test bench without a printer

`test/stresstest.sh` runs on a Linux machine with the stick plugged in (a Raspberry Pi
works well). The machine plays the printer: mount, write test PDFs, eject. A receiver on
the same machine checks whether every file arrives complete and exactly once. Three
scenarios: one file, two in one go, and a second file at the tightest moment, as soon as
the medium is back after the first upload.

```bash
sudo SCAN_WEBPASS=PASSWORD test/stresstest.sh http://scanstick-1a2b.local /dev/sda1
```

The stick's upload target is switched over for the duration of the test and reset
afterwards. The partition is only touched if it belongs to an Espressif USB device and
is named `SCANS`. The Linux driver writes directory entries earlier and differently than
a printer; the test bench does not replace the print test, but it does make regressions
repeatably visible.

### Where the receiver should run

The script runs without extra packages on Linux, macOS and Windows. What matters is not
the operating system, however, but **availability**: the receiver has to be running when
somebody scans. If the machine is asleep, the file stays on the card and is retried on
the next attempt - but it has not arrived.

| Host | Suitability |
|---|---|
| Raspberry Pi | ideal: runs continuously, little power, can be set up as a service |
| NAS | very good, storage right at the destination |
| Server / VM | good, as long as it is reachable from the stick |
| Workstation | only while it is awake |

### Firewall and autostart

**macOS** - on the first start "Allow incoming network connections?" appears, which has
to be allowed (afterwards under *System Settings -> Network -> Firewall -> Options*).
Port 8080 needs no administrator rights. Permanently via a LaunchAgent in
`~/Library/LaunchAgents/`; the machine must then not go to sleep.

**Windows** - the Defender firewall asks on the first start; tick *Private network*, not
public networks. Afterwards: *inbound rule -> port -> TCP 8080 -> allow*, profile
"Private". Permanently via Task Scheduler ("at computer startup") or as a service.

**Linux** - if a firewall is active: `sudo ufw allow 8080/tcp` or
`firewall-cmd --add-port=8080/tcp --permanent`. Permanently as a systemd unit:

```ini
# /etc/systemd/system/scan-receiver.service
[Unit]
Description=Scan-Stick receiver
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 -u /opt/scanstick/empfaenger/scan-receiver.py
Environment=SCAN_PORT=8080
Environment=SCAN_INBOX=/srv/scans
Environment=SCAN_KEY=secret
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now scan-receiver
```

**Two things that are easy to forget:**

1. The receiver needs a **fixed address** - reserved in the router or assigned
   statically. If it gets a new one by DHCP, `endpoint=` points nowhere, and it happens
   at some point in the middle of operation.
2. It accepts **everything** that is sent to it. On a home network that is acceptable,
   but it should not be reachable from the internet.

## Display

| Display | Meaning |
|---|---|
| POWER | booting |
| READY | waiting, with signal bar |
| OK | printer is accessing right now |
| SCAN FOUND | write noticed, shows countdown or detected size |
| SENDING | progress bar with percentage and file name |
| WAIT | printer not finished yet, with attempt counter |
| SD?! | card not readable |

## Open points

- Automatic switching of the access point when reception stays weak for a longer time
  (on a complete outage it scans again after two minutes; on merely weak reception not yet)
- Check file types other than PDF for completeness as well
- Further boards, in particular ones without a card slot (there the internal flash would
  have to serve as storage)
- Test whether a USB 2.0 hub in between makes the stick usable on printers that refuse a
  full-speed device (see "Printer compatibility")

## Contributing

The stick has been developed on a single printer, a single receiver and a single board.
Three areas are waiting for experience from other setups - please open an issue, the
templates ask for what is needed:

- **Other printers.** Every printer talks to a USB stick a little differently (see
  "Pitfalls"). If yours behaves differently, the stick log (`/log`) during a scan and the
  printer model and firmware version are the most valuable input.
- **Other destinations.** The stick only knows a URL; `empfaenger/scan-receiver.py` is a
  minimal example. Receivers for cloud storage, mail, DMS or a NAS are welcome - as a
  separate script or as a pointer to your own repository.
- **Other boards.** Requirements: an ESP32-S3 (native USB for the mass-storage role),
  a card slot on SD_MMC and ideally a USB-A plug so it fits a printer directly. Pin
  tables and build settings (FQBN) for further boards are welcome.

Pull requests are welcome too. Please keep them small and describe how you tested; the
CI build runs automatically. Do not put private IPs, hostnames or credentials into the
code or the pull request text.

## License

MIT, see `LICENSE`.
