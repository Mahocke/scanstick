/**
 * Scan-Stick for LilyGo T-Dongle-S3
 * Presents itself to the printer as a USB stick (FAT32 "SCANS"). Detects when a scan
 * has been written completely (idle after the last sector write), uploads every new
 * file by HTTP POST to a configurable receiver and re-announces the stick
 * afterwards, so the printer sees an empty stick again.
 *
 * The diagnostic log goes by HTTP to the same receiver (scanlog-<millis>.txt),
 * NOT onto the card - otherwise our own FAT write destroys the printer's commit.
 *
 * Credentials do NOT belong in the code, they come from /wifi.cfg on the SD card:
 *   ssid=...
 *   pass=...
 *   endpoint=http://192.168.1.50:8080/scan      (example - the address of your own receiver)
 *
 * Board: ESP32S3 Dev Module, USB Mode = USB-OTG (TinyUSB), USB CDC on Boot = Enabled
 */
#include "USB.h"
#include "USBMSC.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <functional>
#include "mbedtls/md.h"
#include "tusb.h"
#include "esp_mac.h"

#define SD_D0  14
#define SD_D1  17
#define SD_D2  21
#define SD_D3  18
#define SD_CLK 12
#define SD_CMD 16

// Data types that appear in function signatures live up here: the Arduino
// builder creates prototypes before the first function and must know the types there.
struct RohEintrag { uint32_t start; uint32_t groesse; char name[64]; };
struct Fertig { uint32_t start, groesse, kennzahl; char name[40]; };
struct KartenBefund { int fatAbweichungen, kreuz, verwaist, gekuerzt; bool schmutzig, heillos, geaendert, zuGross; };

#define IDLE_VORGABE  45000    // idle until "scan done" - default, changeable via the web interface
static uint32_t g_idleMs   = IDLE_VORGABE;   // idle deadline, from the flash
static bool     g_loeschen = true;           // true = delete, false = move to /gesendet
#define IDLE_MS (g_idleMs)
#define RETRY_MS      30000    // nothing found -> wait this long, then look again
#define MAX_VERSUCHE  5        // look this many times before we give up
#define WIFI_TIMEOUT  20000

// ---- APA102 status LED (T-Dongle-S3: data 40, clock 39) ----
#define LED_DI 40
#define LED_CI 39
static void ledByte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(LED_DI, (b & 0x80) ? HIGH : LOW);
        digitalWrite(LED_CI, HIGH);
        digitalWrite(LED_CI, LOW);
        b <<= 1;
    }
}
static uint8_t g_ledR = 0, g_ledG = 0, g_ledB = 0;
// The APA102 has its own brightness byte (0..31). Full brightness as a
// permanent light next to a printer is simply too glaring - adjustable, 0 = off.
static uint8_t g_ledHell = 5;

static void ledRaw(uint8_t r, uint8_t g, uint8_t b) {
    ledByte(0); ledByte(0); ledByte(0); ledByte(0);   // start frame
    if (!g_ledHell) { r = g = b = 0; }                 // fully off
    ledByte(0xE0 | (g_ledHell & 0x1F));
    ledByte(b); ledByte(g); ledByte(r);               // BGR order
    ledByte(0xFF); ledByte(0xFF); ledByte(0xFF); ledByte(0xFF); // end frame
}
static void ledColor(uint8_t r, uint8_t g, uint8_t b) { g_ledR = r; g_ledG = g; g_ledB = b; ledRaw(r, g, b); }
static void ledHeartbeat() {
    if (!g_ledHell) return;   // off stays off, even for the heartbeat
    ledRaw(0, 0, 0); delay(40); ledRaw(g_ledR, g_ledG, g_ledB);
}
static void ledInit() { pinMode(LED_DI, OUTPUT); pinMode(LED_CI, OUTPUT); }

// ---- ST7735 display (T-Dongle-S3: CS4 SDA3 SCL5 DC2 RST1 Backlight38) ----
#define TFT_CS 4
#define TFT_SDA 3
#define TFT_SCL 5
#define TFT_DC 2
#define TFT_RST 1
#define TFT_BL 38
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
// States of the stick. Display background and status LED share one
// color per state - before, both were set separately and drifted apart.
#define Z_STROM   0
#define Z_BEREIT  1
#define Z_HOST    2
#define Z_SUCHT   3
#define Z_SDFEHL  4
#define Z_WARTE   5
#define Z_FRIST   6
#define Z_ANZAHL  7

static const char *Z_NAME[Z_ANZAHL] = {
    "Power on, starting", "Ready, waiting for the printer", "Printer is accessing",
    "Looking for new scans", "Card not readable", "Waiting for the commit",
    "Scan found, idle deadline running"
};
static const char *Z_TEXT[Z_ANZAHL] = { "POWER", "READY", "OK", "SEARCH", "SD?!", "WAIT", "SCAN" };
// Defaults as 0xRRGGBB, changeable via the web interface
static uint32_t g_farbe[Z_ANZAHL] = { 0xFFC000, 0x0044FF, 0x00C000, 0xCC00CC, 0xFF0000, 0xFFAA00, 0x00AACC };
static uint32_t g_farbeSendet = 0x00AAFF;   // during the transfer
static uint32_t g_farbeFertig = 0x00CC44;   // success message

static uint16_t rgb565(uint32_t rgb)
{
    uint8_t r = rgb >> 16, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static Arduino_DataBus *g_bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCL, TFT_SDA);
static Arduino_GFX *g_gfx = new Arduino_ST7735(g_bus, TFT_RST, 1 /*rotation*/, false /*ips*/, 80, 160, 26, 1, 26, 1);
static int g_screen = -1;   // current display state, redraw only on a change
static bool g_invertiert = true;    // display shows the colors inverted
static int g_wlanStufeLetzt = -1;   // last drawn signal level
static uint32_t g_wlanLetzt = 0;
static long     g_rssiMin = 0, g_rssiMax = 0, g_rssiLetztLast = 0;
static long     g_rssiSumme = 0;
static uint32_t g_rssiAnzahl = 0;

// draws a simple lightning bolt at position (x,y)
static void malBlitz(int x, int y, uint16_t farbe) {
    g_gfx->fillTriangle(x+14, y, x+2, y+20, x+12, y+18, farbe);
    g_gfx->fillTriangle(x+12, y+16, x+22, y+14, x+8, y+36, farbe);
}

// One state sets display AND LED - one color, one source.
static void zeigeScreen(int z)
{
    if (z == g_screen || z < 0 || z >= Z_ANZAHL) return;
    g_screen = z;
    uint32_t rgb = g_farbe[z];
    ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    g_gfx->fillScreen(rgb565(rgb));
    if (z == Z_STROM || z == Z_HOST) malBlitz(6, 24, z == Z_HOST ? COL_BLACK : COL_WHITE);
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->setTextSize(z == Z_HOST ? 5 : 3);
    g_gfx->setCursor(z == Z_HOST ? 60 : 30, 30);
    g_gfx->print(Z_TEXT[z]);
    g_wlanStufeLetzt = -1;   // fillScreen has erased the bars
}

// ---- signal bars top right ----
// Reception varies considerably at the printer location (inside the metal case -93 dBm,
// outside -61). You should see that on the device without opening the web interface.
// Record reception continuously: today it varied at the same position
// between -93 and -61 dBm. Without a record there is no telling
// whether that happens all the time or only under send load.
static void rssiErfassen(long r)
{
    if (r >= 0 || r < -110) return;
    if (!g_rssiAnzahl || r < g_rssiMin) g_rssiMin = r;
    if (!g_rssiAnzahl || r > g_rssiMax) g_rssiMax = r;
    g_rssiSumme += r;
    g_rssiAnzahl++;
}

static int wlanStufe()
{
    if (WiFi.status() != WL_CONNECTED) return 0;
    long r = WiFi.RSSI();
    rssiErfassen(r);
    if (r >= -55) return 5;
    if (r >= -65) return 4;
    if (r >= -72) return 3;
    if (r >= -80) return 2;
    if (r >= -90) return 1;
    return 0;
}

static void zeichneWlanBalken()
{
    int stufe = wlanStufe();
    if (stufe == g_wlanStufeLetzt) return;
    g_wlanStufeLetzt = stufe;

    int z = (g_screen >= 0 && g_screen < Z_ANZAHL) ? g_screen : Z_BEREIT;
    uint16_t hg = rgb565(g_farbe[z]);
    g_gfx->fillRect(110, 1, 49, 24, hg);          // clear the area

    for (int i = 0; i < 5; i++) {
        int hoehe = 4 + i * 4;                     // 4, 8, 12, 16, 20 pixels
        int x = 112 + i * 9;
        int y = 22 - hoehe;
        if (i < stufe) g_gfx->fillRect(x, y, 7, hoehe, COL_BLACK);   // filled = present
        else           g_gfx->drawRect(x, y, 7, hoehe, COL_BLACK);   // outline only
    }
    if (stufe == 0) {                              // no network: cross over the bars
        g_gfx->drawLine(112, 2, 156, 22, COL_BLACK);
        g_gfx->drawLine(112, 22, 156, 2, COL_BLACK);
    }
}

// ---- dynamic screens: found / sending / done ----
// In operation the display is the only feedback on the device, so you should
// see WHAT it sends and that it makes progress - not just "something is running".

// shorten file names to the display width (text size 1 = 6 px per character)
static String kurzName(const String &name, int max_zeichen)
{
    if ((int)name.length() <= max_zeichen) return name;
    return name.substring(0, max_zeichen - 3) + "...";
}

static int g_balkenProzent = -1;

static void zeigeSendenStart(const String &name, uint32_t gesamt)
{
    g_screen = -2;               // special state: loop() redraws when it is over
    g_balkenProzent = -1;
    uint32_t rgb = g_farbeSendet;
    ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    g_gfx->fillScreen(rgb565(rgb));
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->setTextSize(2);
    g_gfx->setCursor(4, 4);
    g_gfx->print("SENDING");
    g_gfx->setTextSize(1);
    g_gfx->setCursor(4, 24);
    g_gfx->print(kurzName(name, 25));
    g_gfx->setCursor(4, 64);
    g_gfx->printf("%u kB", (unsigned)(gesamt / 1024));
    g_gfx->drawRect(4, 38, 152, 18, COL_BLACK);   // frame of the bar
}

static void zeigeSendenFortschritt(uint32_t fertig, uint32_t gesamt)
{
    int proz = gesamt ? (int)((uint64_t)fertig * 100 / gesamt) : 100;
    if (proz == g_balkenProzent) return;          // draw only on a change, otherwise it flickers
    g_balkenProzent = proz;
    int breite = (150 * proz) / 100;
    g_gfx->fillRect(5, 39, breite, 16, COL_BLACK);
    g_gfx->setTextSize(1);
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->fillRect(100, 62, 56, 10, rgb565(g_farbeSendet));
    g_gfx->setCursor(100, 64);
    g_gfx->printf("%d%%", proz);
}

static void zeigeFertig(uint32_t bytes)
{
    g_screen = -2;
    uint32_t rgb = g_farbeFertig;
    ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    g_gfx->fillScreen(rgb565(rgb));
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->setTextSize(4);
    g_gfx->setCursor(20, 20);
    g_gfx->print("OK");
    g_gfx->setTextSize(1);
    g_gfx->setCursor(20, 62);
    g_gfx->printf("%u kB sent", (unsigned)(bytes / 1024));
}

// Idle deadline running: show the countdown large. Before, an unchanged image
// stood here for 45 seconds - you could not see whether it had the scan.
static int  g_fristLetzt   = -1;
static bool g_warteAnzeige = false;   // WAIT screen is up and must not be overwritten

// result of the raw reading - the display accesses it too
#define ROH_INTERVALL   1000    // look raw this often (ms)
#define ROH_RUHE        3000    // no write access for this long before we access
#define ROH_STABIL         3    // this many equal readings = file done
static uint32_t g_rohLetzt  = 0;
static uint32_t g_rohSumme  = 0;
static int      g_rohAnzahl = 0;
static int      g_rohStabil = 0;
// For the display only what is not yet sent counts - otherwise after every
// write access of the printer "SCAN FOUND" stood on the display with the size
// of long since sent files.
static int      g_zeigAnzahl  = 0;
static uint32_t g_zeigSumme   = 0;
static uint32_t g_rohGeprueft = 0;   // millis() of the last raw look
static int      g_fristModus  = -1;

static void zeigeFrist(uint32_t restSek)
{
    int modus = g_zeigAnzahl ? 1 : 0;
    if (g_screen != Z_FRIST) {
        g_screen = Z_FRIST;
        g_fristLetzt = -1;
        g_fristModus = -1;
        uint32_t rgb = g_farbe[Z_FRIST];
        ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
        g_gfx->fillScreen(rgb565(rgb));
        g_gfx->setTextColor(COL_BLACK);
        g_gfx->setTextSize(2);
        g_gfx->setCursor(5, 4);
        g_gfx->print("SCAN FOUND");
    }
    if (modus != g_fristModus) {
        g_fristModus = modus;
        g_fristLetzt = -1;
        g_gfx->fillRect(5, 66, 150, 12, rgb565(g_farbe[Z_FRIST]));
        g_gfx->setTextColor(COL_BLACK);
        g_gfx->setTextSize(1);
        g_gfx->setCursor(5, 68);
        g_gfx->print(modus ? "checking if done" : "waiting for idle");
    }
    // As soon as we see the new file raw, its size is the more honest figure
    // than a countdown that ends early anyway.
    int wert = modus ? (int)(g_zeigSumme / 1024) : (int)restSek;
    if (wert != g_fristLetzt) {
        g_fristLetzt = wert;
        g_gfx->fillRect(5, 26, 150, 36, rgb565(g_farbe[Z_FRIST]));
        g_gfx->setTextColor(COL_BLACK);
        g_gfx->setTextSize(modus ? 3 : 4);
        g_gfx->setCursor(5, 30);
        if (modus) g_gfx->printf("%d kB", wert);
        else       g_gfx->printf("%ds", wert);
    }
}

static void zeigeWarte(int versuch, int von)
{
    g_screen = -2;
    g_warteAnzeige = true;
    uint32_t rgb = g_farbe[Z_WARTE];
    ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    g_gfx->fillScreen(rgb565(rgb));
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->setTextSize(3);
    g_gfx->setCursor(12, 14);
    g_gfx->print("WAIT");
    g_gfx->setTextSize(1);
    g_gfx->setCursor(12, 50);
    g_gfx->printf("Attempt %d of %d", versuch, von);
    g_gfx->setCursor(12, 62);
    g_gfx->print("Printer not finished");
}

static void displayInit() {
    pinMode(TFT_BL, OUTPUT);
    g_gfx->begin();
    // Otherwise this module shows the colors as a negative: a configured blue
    // appears yellow, black appears white. That would leave every color picker
    // in the settings without effect, or exactly the wrong way round.
    g_gfx->invertDisplay(g_invertiert);
    digitalWrite(TFT_BL, LOW);   // backlight on (LILYGO: active low)
    g_gfx->fillScreen(COL_BLACK);
}

USBMSC MSC;

static volatile uint32_t g_lastWrite = 0;
static volatile uint32_t g_lastHost  = 0;   // when did the host last read/write
static volatile bool     g_dirty     = false;
static volatile uint32_t g_bytesGeschrieben = 0;   // since the last processing

static uint32_t g_naechsterVersuch = 0;   // 0 = due immediately
static int      g_versuche         = 0;

#define FW_VERSION "v40"

String cfgEndpoint;
// Known WiFi networks - several, so the same stick runs at different locations.
// During the scan the strongest access point across ALL known networks wins;
// WiFiMulti connects specifically with its identifier and channel.
#define MAX_NETZE 4
static String    cfgNetzSsid[MAX_NETZE], cfgNetzPass[MAX_NETZE];
static int       cfgNetze = 0;
static WiFiMulti g_wifiMulti;
// Device name with the last four digits of the radio address: otherwise two sticks
// in the same network were both called scanstick.local, and on 20.09.2026 the test
// rig promptly talked to the wrong one. The name is in the log and on the status page.
static String geraeteName()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // from the chip, not from the driver: that one delivers only zeros before the WiFi start
    char t[24];
    snprintf(t, sizeof t, "scanstick-%02x%02x", mac[4], mac[5]);
    return String(t);
}
String cfgWebPass;   // protection of the web interface; empty = open
String cfgPraefix = "scan";   // name prefix of the files, e.g. "buero-774"
String cfgSchluessel;         // device key: signs every upload; empty = without
static bool g_zeitOk        = false;   // NTP time available?
static bool g_einmalSchauen = false;   // button "Look now": once, without a wait cycle
static bool g_startGeprueft = false;   // look once after start-up

static WebServer  g_web(80);
static Preferences g_nvs;

// One lock for ALL raw card accesses, from the USB task as well as from the
// main loop. The card driver protects only single commands; a write however
// consists of the data AND the wait until the card is done. If a read of the
// main loop (directory, end marker, upload) pushes in between, the host's
// access can fail - for the 780 a media error (44.12.05), whereupon it cuts
// the power to the port.
static SemaphoreHandle_t g_sdRiegel = nullptr;
static bool sdLesen(uint8_t *b, uint32_t sektor)
{
    if (g_sdRiegel) xSemaphoreTake(g_sdRiegel, portMAX_DELAY);
    bool ok = SD_MMC.readRAW(b, sektor);
    if (g_sdRiegel) xSemaphoreGive(g_sdRiegel);
    return ok;
}
static bool sdSchreiben(uint8_t *b, uint32_t sektor)
{
    if (g_sdRiegel) xSemaphoreTake(g_sdRiegel, portMAX_DELAY);
    bool ok = SD_MMC.writeRAW(b, sektor);
    if (g_sdRiegel) xSemaphoreGive(g_sdRiegel);
    return ok;
}
static bool       g_webAn = false;
static bool       g_updateBegonnen = false;   // Update.begin() has actually run

// ---- MSC: the host accesses the SD, every sector runs through us ----
// Handover of the card between USB task and main loop. mediaPresent(false)
// holds off only NEW commands - a read that is already running keeps running.
// On unmount a Linux host reads the FAT in 120 kB blocks, that takes longer
// than the 200 ms we waited before SD_MMC.end(). The USB task then hung on the
// torn-down driver, after 5 s the task watchdog struck (on the test rig twice
// in a row, in the kernel log "cmd_age=5s").
// Therefore: set the lock, wait until no access is running, only then touch it.
static volatile int  g_usbZugriffe = 0;      // onRead/onWrite currently running
static volatile bool g_sdGesperrt  = false;  // main loop has the card
// Diagnosis: the last write accesses of the host (sector, count) - they show
// whether it writes the directory, the allocation table or data.
#define SPUR_ANZAHL 32
static volatile uint32_t g_spurLba[SPUR_ANZAHL], g_spurN[SPUR_ANZAHL], g_spurT[SPUR_ANZAHL];
static volatile int      g_spurIdx = 0;
// Rejected host accesses: the answer -1 means "media error" to the printer.
// If that happens in the middle of a scan, it aborts silently - so count them
// and show them in the log and on the status page.
static volatile uint32_t g_usbFehlerLesen = 0, g_usbFehlerSchreiben = 0;
static volatile uint32_t g_usbFehlerLba = 0, g_usbFehlerZeit = 0;
static uint32_t          g_usbFehlerGemeldet = 0;
static volatile bool     g_usbFehlerNeu = false;
static String            g_usbFehlerAlt;   // finding from the flash: what happened before the last restart
static void logZeile(const String &msg);     // further below
static bool remount();                       // likewise
static void geloeschtMerken(const Fertig &f); // likewise
static void logFlushNetz();                  // likewise

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    // A return of 0 means "busy, try again shortly" to TinyUSB - with a real
    // card error that hangs the host forever. Negative = clean error.
    if (g_sdGesperrt) { g_usbFehlerSchreiben++; g_usbFehlerLba = lba; g_usbFehlerZeit = millis(); g_usbFehlerNeu = true; g_lastHost = millis(); return -1; }
    g_usbZugriffe++;
    int32_t ergebnis = -1;
    uint32_t sec = SD_MMC.sectorSize();
    if (sec) {
        ergebnis = bufsize;
        for (uint32_t x = 0; x < bufsize / sec; x++) {
            if (g_sdGesperrt || !sdSchreiben(buffer + sec * x, lba + x)) { ergebnis = -1; break; }
        }
    }
    if (ergebnis > 0) {
        g_lastWrite = millis();
        g_dirty = true;
        g_bytesGeschrieben += bufsize;   // measurement: does any scan data arrive at all?
        int i = g_spurIdx;
        g_spurLba[i] = lba; g_spurN[i] = bufsize / (sec ? sec : 512); g_spurT[i] = millis();
        g_spurIdx = (i + 1) % SPUR_ANZAHL;
    }
    if (ergebnis < 0) { g_usbFehlerSchreiben++; g_usbFehlerLba = lba; g_usbFehlerZeit = millis(); g_usbFehlerNeu = true; }
    g_lastHost = millis();
    g_usbZugriffe--;
    return ergebnis;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (g_sdGesperrt) { g_usbFehlerLesen++; g_usbFehlerLba = lba; g_usbFehlerZeit = millis(); g_usbFehlerNeu = true; g_lastHost = millis(); return -1; }
    g_usbZugriffe++;
    int32_t ergebnis = -1;
    uint32_t sec = SD_MMC.sectorSize();
    if (sec) {
        ergebnis = bufsize;
        for (uint32_t x = 0; x < bufsize / sec; x++) {
            if (g_sdGesperrt || !sdLesen((uint8_t *)buffer + x * sec, lba + x)) { ergebnis = -1; break; }
        }
    }
    if (ergebnis < 0) { g_usbFehlerLesen++; g_usbFehlerLba = lba; g_usbFehlerZeit = millis(); g_usbFehlerNeu = true; }
    g_lastHost = millis();
    g_usbZugriffe--;
    return ergebnis;
}

// Take the card away from the host and wait until really nobody accesses it.
// Returns false if an access is still running after 5 s - then better not
// touch it and let the host keep it.
static bool karteUebernehmen()
{
    MSC.mediaPresent(false);
    g_sdGesperrt = true;
    uint32_t t0 = millis();
    while (millis() - t0 < 5000) {
        if (g_usbZugriffe == 0 && millis() - g_lastHost > 300) return true;
        delay(10);
    }
    logZeile(String("[usb] host does not let go (") + (int)g_usbZugriffe + " accesses open)");
    g_sdGesperrt = false;
    MSC.mediaPresent(true);
    return false;
}

static void karteZurueckgeben()
{
    g_sdGesperrt = false;
    MSC.mediaPresent(true);
}

// A restart that survives at the printer too. The 780 briefly cuts the power to
// the USB port as soon as a device unregisters. If the stick restarts at once,
// this power cut hits the firmware that has only just booted, before it has
// reported itself as valid to the bootloader - and that one falls back to the
// previous one (rollback is switched on in the core). Twice v27 stayed this way
// although v28 had been installed cleanly. Therefore: unregister from USB first,
// wait out the power cut in the OLD firmware, then restart.
static void sanftNeustarten()
{
    logFlushNetz();
    MSC.mediaPresent(false);
    tud_disconnect();
    delay(2500);
    ESP.restart();
}

// SCSI START STOP UNIT. Devices often send this at the end of a job ("eject",
// "flush the buffer"). If the printer does that, we have an immediate
// done signal and do not have to wait 45 s for silence.
static volatile uint32_t g_letztesStop = 0;
static volatile bool     g_stopNeu      = false;
static volatile bool     g_stopStart    = false;
static volatile bool     g_stopEject    = false;
static volatile uint8_t  g_stopPc       = 0;

static bool onStartStop(uint8_t pc, bool start, bool eject)
{
    // Deliberately nothing expensive here: the callback runs in the USB context,
    // strings and file access have no business in this place.
    g_letztesStop = millis();
    g_stopStart = start;
    g_stopEject = eject;
    g_stopPc = pc;
    g_stopNeu = true;
    return true;
}

// ---- Diagnostic log: collected in RAM, goes out over WiFi ----
// DELIBERATELY not onto the SD: as long as the printer holds the medium, any
// FAT write of our own would destroy its still open commit - that is exactly the
// operation we want to observe. The USB CDC drops away when WiFi starts,
// so the network is the only reliable channel.
static String g_logPuffer;
static size_t g_logGesendet = 0;   // up to here the buffer has already reached the receiver

static void logZeile(const String &msg)
{
    Serial.println(msg);
    g_logPuffer += String(millis()) + " " + msg + "\n";
    if (g_logPuffer.length() > 8000) {                             // cap, RAM is tight
        g_logPuffer.remove(0, 4000);
        g_logGesendet = g_logGesendet > 4000 ? g_logGesendet - 4000 : 0;
    }
}

// Device authentication: HMAC-SHA256 over name, id and length with the
// device key, as header X-Scan-Auth. The receiver rejects everything without
// a valid signature. A timestamp is not needed: a repeat of the
// same upload is harmless, the receiver deduplicates by id.
// No TLS - on the LAN that is enough, and a TLS context costs the stick about
// 40 kB of memory and every upload a noticeable amount of time.
static String uploadSignatur(const String &name, const String &id, uint32_t laenge)
{
    if (cfgSchluessel.isEmpty()) return "";
    String nachricht = name + "\n" + id + "\n" + String(laenge);
    uint8_t mac[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        (const uint8_t *)cfgSchluessel.c_str(), cfgSchluessel.length(),
                        (const uint8_t *)nachricht.c_str(), nachricht.length(), mac) != 0) return "";
    String hex;
    for (int i = 0; i < 32; i++) { char t[3]; snprintf(t, sizeof t, "%02x", mac[i]); hex += t; }
    return hex;
}

// Send away only what is new since the last time. Does not touch the SD card.
// Before, the whole buffer went out every time - one log file per scan
// with the entire history. The buffer itself stays in place, the
// web interface is meant to show the full history.
static void logFlushNetz()
{
    if (g_logGesendet >= g_logPuffer.length() || cfgEndpoint.isEmpty()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    String teil = g_logPuffer.substring(g_logGesendet);
    HTTPClient http;
    String dateiname = "scanlog-" + String(millis()) + ".txt";
    String url = cfgEndpoint;
    url += (url.indexOf('?') < 0) ? "?name=" : "&name=";
    url += dateiname;
    if (!http.begin(url)) return;
    http.addHeader("Content-Type", "text/plain");
    String sig = uploadSignatur(dateiname, "", teil.length());
    if (sig.length()) http.addHeader("X-Scan-Auth", sig);
    int code = http.POST(teil);
    http.end();
    if (code >= 200 && code < 300) g_logGesendet = g_logPuffer.length();
}

// ---- Set the MBR partition type to FAT32-LBA (0x0C) ----
// ESP format sometimes leaves 0x07 behind (macOS reads that as NTFS and will not mount)
static void fixMbrTyp()
{
    uint8_t *s = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!s) return;
    if (sdLesen(s, 0) && s[510] == 0x55 && s[511] == 0xAA) {
        if (s[446 + 4] == 0x07) {
            s[446 + 4] = 0x0C;
            if (sdSchreiben(s, 0)) Serial.println("[mbr] type 0x07 -> 0x0C corrected");
        }
    }
    free(s);
}

// ================= Reading along in the directory, raw =================
// Until now the stick had to unmount the card briefly and mount it again to
// see what the printer had written - during those milliseconds it is gone for
// the printer. That is why it was only allowed to look rarely and had to wait
// for silence. If it reads the sectors itself instead, it disturbs nobody and
// may look every second.
//
// FAT32 in brief: boot sector (layout) -> allocation table (which cluster belongs
// to which file) -> directory (32 bytes per entry with name and SIZE).
// The writer only enters the final size on closing - that is exactly
// how we recognize that a file is done.

#define ROH_MIN_GROESSE 2048   // below that: helper files like wifi.cfg, no scan
#define MAX_FUND 24            // this many files per pass

// What counts as a scan? ONE yardstick for the raw reader (sees only the 8.3 short name)
// and for the collector (sees the long name). Before, the raw reader counted every
// file from 2 kB up, but the collector skipped everything with a leading dot. A
// "._wifi.cfg" from the Mac (4 kB, short name "_WIFI~1.CFG") was visible raw, but
// not in the collecting pass - upon which the stick unmounted and remounted the
// medium five times in a row, after every start and after every write.
static bool istScanEndung(const uint8_t *e)   // 3 characters from the short name, uppercase
{
    return !memcmp(e, "PDF", 3) || !memcmp(e, "JPG", 3) || !memcmp(e, "JPE", 3) ||
           !memcmp(e, "TIF", 3);
}

static bool istScanName(const String &basis)
{
    int p = basis.lastIndexOf('.');
    if (p < 0) return false;
    String e = basis.substring(p + 1);
    e.toUpperCase();
    return e == "PDF" || e == "JPG" || e == "JPEG" || e == "TIF" || e == "TIFF";
}

struct FatLage {
    bool     gueltig;
    uint8_t  sektorenProCluster;
    uint32_t fatStart;
    uint32_t ersterDatenSektor;
    uint32_t rootCluster;
    uint32_t partStart;        // for the self-check and for formatting
    uint32_t partSektoren;
    uint32_t fatSektoren;
    uint8_t  anzahlFats;
    uint32_t gesamtCluster;    // data clusters (numbers 2 .. total+1)
    uint16_t fsInfoSektor;
};
static FatLage g_fat = { false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static uint32_t le32(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static void fatLageLesen()
{
    g_fat.gueltig = false;
    uint8_t *s = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!s) return;

    uint32_t partStart = 0, partSektoren = 0;
    if (sdLesen(s, 0) && s[510] == 0x55 && s[511] == 0xAA) {
        partStart    = le32(s + 446 + 8);       // start sector of the first partition
        partSektoren = le32(s + 446 + 12);
    }
    if (!sdLesen(s, partStart)) { free(s); return; }

    uint16_t bytesProSektor = (uint16_t)s[11] | ((uint16_t)s[12] << 8);
    uint8_t  spc            = s[13];
    uint16_t reserviert     = (uint16_t)s[14] | ((uint16_t)s[15] << 8);
    uint8_t  anzahlFats     = s[16];
    uint32_t sektoren       = le32(s + 32);
    uint32_t fatGroesse     = le32(s + 36);
    uint32_t rootCluster    = le32(s + 44);
    uint16_t fsInfo         = (uint16_t)s[48] | ((uint16_t)s[49] << 8);
    bool     signatur       = s[510] == 0x55 && s[511] == 0xAA;
    free(s);

    if (!signatur || bytesProSektor != 512 || !spc || !fatGroesse || rootCluster < 2 || !anzahlFats) return;
    g_fat.sektorenProCluster = spc;
    g_fat.fatStart           = partStart + reserviert;
    g_fat.ersterDatenSektor  = partStart + reserviert + (uint32_t)anzahlFats * fatGroesse;
    g_fat.rootCluster        = rootCluster;
    g_fat.partStart          = partStart;
    g_fat.partSektoren       = partSektoren ? partSektoren : sektoren;
    g_fat.fatSektoren        = fatGroesse;
    g_fat.anzahlFats         = anzahlFats;
    g_fat.fsInfoSektor       = fsInfo;
    uint32_t daten = sektoren > reserviert + anzahlFats * fatGroesse ? sektoren - reserviert - anzahlFats * fatGroesse : 0;
    g_fat.gesamtCluster      = daten / spc;
    g_fat.gueltig            = true;
}

static uint32_t clusterSektor(uint32_t c)
{
    return g_fat.ersterDatenSektor + (c - 2) * g_fat.sektorenProCluster;
}

static uint32_t fatNaechster(uint32_t c, uint8_t *puffer)
{
    uint32_t versatz = c * 4;
    if (!sdLesen(puffer, g_fat.fatStart + versatz / 512)) return 0x0FFFFFFF;
    return le32(puffer + (versatz % 512)) & 0x0FFFFFFF;
}

// Counts files in the root directory and sums up their sizes.
// If count and sum stay the same over several looks, nobody is writing any more.
static bool rohVerzeichnis(int &anzahl, uint32_t &summe)
{
    anzahl = 0;
    summe = 0;
    if (!g_fat.gueltig) fatLageLesen();
    if (!g_fat.gueltig) return false;

    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return false; }

    bool fertig = false;
    uint32_t cl = g_fat.rootCluster;
    for (int schutz = 0; !fertig && cl >= 2 && cl < 0x0FFFFFF8 && schutz < 128; schutz++) {
        for (uint8_t i = 0; i < g_fat.sektorenProCluster && !fertig; i++) {
            if (!sdLesen(sek, clusterSektor(cl) + i)) { fertig = true; break; }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { fertig = true; break; }   // end of the directory
                if (d[0] == 0xE5) continue;                   // deleted entry
                uint8_t attr = d[11];
                if (attr == 0x0F) continue;                   // part of a long name
                if (attr & 0x18) continue;                    // folder or volume label
                if (!istScanEndung(d + 8)) continue;          // extension in the short name: byte 8-10
                uint32_t gr = le32(d + 28);
                if (gr < ROH_MIN_GROESSE) continue;           // helper file, no scan
                anzahl++;
                summe += gr;
            }
        }
        if (!fertig) cl = fatNaechster(cl, fatBuf);
    }
    free(sek);
    free(fatBuf);
    return true;
}

// First cluster of a directory entry (bytes 20/21 high, 26/27 low)
static uint32_t eintragCluster(const uint8_t *d)
{
    return ((uint32_t)(d[20] | (d[21] << 8)) << 16) | (uint32_t)(d[26] | (d[27] << 8));
}

// Mark an entry in a directory as deleted, raw (first byte
// 0xE5), WITHOUT touching the block allocation. Long names sit in pieces of
// 13 characters BEFORE the short entry, in reverse order; those are marked
// as well. The search goes by name (if given) or by start cluster.
// Only call with the medium unmounted and BEFORE the next mount, otherwise
// the file system driver writes its buffered sector over it again.
static bool rohEintragLoeschenIn(uint32_t dirCluster, const String &name, uint32_t groesse, uint32_t startCluster)
{
    if (!g_fat.gueltig) fatLageLesen();
    if (!g_fat.gueltig) return false;
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return false; }

    String ziel = name;
    ziel.toLowerCase();
    String lang;
    uint32_t lfnSektor[20];
    int      lfnOffset[20];
    int      lfnAnzahl = 0;
    bool getroffen = false, fertig = false;
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

    uint32_t cl = dirCluster ? dirCluster : g_fat.rootCluster;
    for (int schutz = 0; !fertig && !getroffen && cl >= 2 && cl < 0x0FFFFFF8 && schutz < 128; schutz++) {
        for (uint8_t i = 0; i < g_fat.sektorenProCluster && !fertig && !getroffen; i++) {
            uint32_t sektor = clusterSektor(cl) + i;
            if (!sdLesen(sek, sektor)) { fertig = true; break; }
            for (int e = 0; e < 512 && !getroffen; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { fertig = true; break; }
                if (d[0] == 0xE5) { lang = ""; lfnAnzahl = 0; continue; }
                if (d[11] == 0x0F) {
                    String stueck;
                    for (int k = 0; k < 13; k++) {
                        uint16_t ch = d[pos[k]] | ((uint16_t)d[pos[k] + 1] << 8);
                        if (ch == 0 || ch == 0xFFFF) break;
                        stueck += (ch < 128) ? (char)ch : '?';
                    }
                    lang = stueck + lang;
                    if (lfnAnzahl < 20) { lfnSektor[lfnAnzahl] = sektor; lfnOffset[lfnAnzahl] = e; lfnAnzahl++; }
                    continue;
                }
                String kandidat = lang;
                if (kandidat.isEmpty()) {                       // short name only: "NAME    EXT"
                    for (int k = 0; k < 8 && d[k] != ' '; k++) kandidat += (char)d[k];
                    if (d[8] != ' ') { kandidat += '.'; for (int k = 8; k < 11 && d[k] != ' '; k++) kandidat += (char)d[k]; }
                }
                kandidat.toLowerCase();
                bool trifft = ziel.length() ? (kandidat == ziel) : (eintragCluster(d) == startCluster);
                if (!(d[11] & 0x18) && trifft && le32(d + 28) == groesse) {
                    d[0] = 0xE5;
                    for (int k = 0; k < lfnAnzahl; k++) if (lfnSektor[k] == sektor) sek[lfnOffset[k]] = 0xE5;
                    getroffen = sdSchreiben(sek, sektor);
                    for (int k = 0; k < lfnAnzahl && getroffen; k++) {   // pieces in earlier sectors
                        if (lfnSektor[k] == sektor) continue;
                        if (!sdLesen(sek, lfnSektor[k])) { getroffen = false; break; }
                        for (int m = 0; m < lfnAnzahl; m++) if (lfnSektor[m] == lfnSektor[k]) sek[lfnOffset[m]] = 0xE5;
                        if (!sdSchreiben(sek, lfnSektor[k])) getroffen = false;
                        sektor = lfnSektor[k];   // this group is done
                    }
                    break;
                }
                lang = "";
                lfnAnzahl = 0;
            }
        }
        if (!fertig && !getroffen) cl = fatNaechster(cl, fatBuf);
    }
    free(sek);
    free(fatBuf);
    return getroffen;
}

// Cluster of a subfolder of the root, by 8.3 name (11 characters, uppercase,
// padded with spaces). 0 = not present.
static uint32_t rohOrdnerCluster(const char *name83)
{
    if (!g_fat.gueltig) fatLageLesen();
    if (!g_fat.gueltig) return 0;
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return 0; }
    uint32_t ergebnis = 0;
    bool fertig = false;
    uint32_t cl = g_fat.rootCluster;
    for (int schutz = 0; !fertig && !ergebnis && cl >= 2 && cl < 0x0FFFFFF8 && schutz < 128; schutz++) {
        for (uint8_t i = 0; i < g_fat.sektorenProCluster && !fertig && !ergebnis; i++) {
            if (!sdLesen(sek, clusterSektor(cl) + i)) { fertig = true; break; }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { fertig = true; break; }
                if (d[0] == 0xE5 || d[11] == 0x0F || !(d[11] & 0x10)) continue;
                if (!memcmp(d, name83, 11)) { ergebnis = eintragCluster(d); break; }
            }
        }
        if (!fertig && !ergebnis) cl = fatNaechster(cl, fatBuf);
    }
    free(sek);
    free(fatBuf);
    return ergebnis;
}

// Collect start clusters and sizes of all files of a directory, raw.
// Returns the count. dirCluster 0 = root.
static int rohEintraege(uint32_t dirCluster, uint32_t *starts, uint32_t *groessen, int max, bool nurScans)
{
    if (!g_fat.gueltig) fatLageLesen();
    if (!g_fat.gueltig) return 0;
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return 0; }
    int n = 0;
    bool fertig = false;
    uint32_t cl = dirCluster ? dirCluster : g_fat.rootCluster;
    for (int schutz = 0; !fertig && n < max && cl >= 2 && cl < 0x0FFFFFF8 && schutz < 128; schutz++) {
        for (uint8_t i = 0; i < g_fat.sektorenProCluster && !fertig && n < max; i++) {
            if (!sdLesen(sek, clusterSektor(cl) + i)) { fertig = true; break; }
            for (int e = 0; e < 512 && n < max; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { fertig = true; break; }
                if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 0x18)) continue;
                if (nurScans && (!istScanEndung(d + 8) || le32(d + 28) < ROH_MIN_GROESSE)) continue;
                starts[n] = eintragCluster(d);
                groessen[n] = le32(d + 28);
                n++;
            }
        }
        if (!fertig && n < max) cl = fatNaechster(cl, fatBuf);
    }
    free(sek);
    free(fatBuf);
    return n;
}

// Ghost hunt in the root - raw, with the medium unmounted, BEFORE the mount.
// After remounting, the 780 writes its old view of the directory back:
// the previous scan then sits in the root again as [Untitled].pdf and points
// at blocks that either already belong to a file in /senden or /gesendet
// or have long been free. Such entries are merely struck out,
// the block allocation stays untouched. Returns the number of ghosts.
static int geisterJagen()
{
    uint32_t fremd[64];
    uint32_t fremdGr[64];
    int nFremd = 0;
    uint32_t cs = rohOrdnerCluster("SENDEN     ");
    if (cs) nFremd += rohEintraege(cs, fremd + nFremd, fremdGr + nFremd, 64 - nFremd, false);
    uint32_t cg = rohOrdnerCluster("GESENDET   ");
    if (cg) nFremd += rohEintraege(cg, fremd + nFremd, fremdGr + nFremd, 64 - nFremd, false);

    uint32_t wurzel[MAX_FUND], wurzelGr[MAX_FUND];
    int nWurzel = rohEintraege(0, wurzel, wurzelGr, MAX_FUND, true);
    if (!nWurzel) return 0;

    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!fatBuf) return 0;
    uint32_t clusterBytes = (uint32_t)g_fat.sektorenProCluster * 512;
    int geister = 0;
    for (int i = 0; i < nWurzel; i++) {
        uint32_t st = wurzel[i];
        const char *grund = nullptr;
        if (st < 2 || fatNaechster(st, fatBuf) == 0) grund = "points at free blocks";
        for (int k = 0; !grund && k < nFremd; k++)
            if (fremd[k] == st) grund = "points at blocks of a file already cleared away";
        for (int k = 0; !grund && k < nWurzel; k++) {
            if (k == i || wurzel[k] != st) continue;
            // Two root entries on the same blocks: the ghost is the one
            // whose size does not match the chain length.
            uint32_t n = 0, c = st;
            while (c >= 2 && c < 0x0FFFFFF8 && n < 300000) { n++; c = fatNaechster(c, fatBuf); }
            uint32_t passt = (wurzelGr[i] + clusterBytes - 1) / clusterBytes;
            if (passt != n) grund = "shares blocks with another entry and does not match the chain";
        }
        if (!grund) continue;
        bool ok = rohEintragLoeschenIn(0, "", wurzelGr[i], st);
        logZeile(String("[geist] root entry with ") + (wurzelGr[i] / 1024) + " kB " + grund +
                 (ok ? " - struck out" : " - striking out failed"));
        if (ok) geister++;
    }
    free(fatBuf);
    return geister;
}

// ---- Raw reading of whole files and the watch list (see the flow from v26 below) ----

// List a directory raw, with long names. dirCluster 0 = root.
static int rohListe(uint32_t dirCluster, RohEintrag *liste, int max, bool nurScans)
{
    if (!g_fat.gueltig) fatLageLesen();
    if (!g_fat.gueltig) return 0;
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return 0; }
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    String lang;
    int n = 0;
    bool fertig = false;
    uint32_t cl = dirCluster ? dirCluster : g_fat.rootCluster;
    for (int schutz = 0; !fertig && cl >= 2 && cl < 0x0FFFFFF8 && schutz < 128; schutz++) {
        for (uint8_t i = 0; i < g_fat.sektorenProCluster && !fertig; i++) {
            if (!sdLesen(sek, clusterSektor(cl) + i)) { fertig = true; break; }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { fertig = true; break; }
                if (d[0] == 0xE5) { lang = ""; continue; }
                if (d[11] == 0x0F) {
                    String stueck;
                    for (int k = 0; k < 13; k++) {
                        uint16_t ch = d[pos[k]] | ((uint16_t)d[pos[k] + 1] << 8);
                        if (ch == 0 || ch == 0xFFFF) break;
                        stueck += (ch < 128) ? (char)ch : '?';
                    }
                    lang = stueck + lang;
                    continue;
                }
                if (d[11] & 0x18) { lang = ""; continue; }
                bool scan = istScanEndung(d + 8) && le32(d + 28) >= ROH_MIN_GROESSE;
                if (nurScans && !scan) { lang = ""; continue; }
                String name = lang;
                if (name.isEmpty()) {
                    for (int k = 0; k < 8 && d[k] != ' '; k++) name += (char)d[k];
                    if (d[8] != ' ') { name += '.'; for (int k = 8; k < 11 && d[k] != ' '; k++) name += (char)d[k]; }
                }
                lang = "";
                if (n < max) {
                    liste[n].start = eintragCluster(d);
                    liste[n].groesse = le32(d + 28);
                    strncpy(liste[n].name, name.c_str(), 63);
                    liste[n].name[63] = 0;
                    n++;
                }
            }
        }
        if (!fertig) cl = fatNaechster(cl, fatBuf);
    }
    free(sek);
    free(fatBuf);
    return n;
}

// Absolute sector number sektorIndex of a file from its start cluster, 0 = error
static uint32_t rohSektor(uint32_t start, uint32_t sektorIndex, uint8_t *fatBuf)
{
    uint32_t cl = start;
    for (uint32_t i = 0; i < sektorIndex / g_fat.sektorenProCluster; i++) {
        cl = fatNaechster(cl, fatBuf);
        if (cl < 2 || cl >= 0x0FFFFFF8) return 0;
    }
    if (cl < 2 || cl >= 0x0FFFFFF8) return 0;
    return clusterSektor(cl) + sektorIndex % g_fat.sektorenProCluster;
}

// Finished writing? With a PDF, %%EOF sits in the last 1024 bytes - read raw.
static bool rohVollstaendig(const RohEintrag &e)
{
    String n = e.name;
    n.toLowerCase();
    if (!n.endsWith(".pdf")) return true;
    if (e.groesse < 32) return false;
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return false; }
    char puffer[1025];
    int len = 0;
    uint32_t letzter = (e.groesse - 1) / 512;
    for (uint32_t idx = letzter >= 1 ? letzter - 1 : 0; idx <= letzter; idx++) {
        uint32_t sk = rohSektor(e.start, idx, fatBuf);
        if (!sk || !sdLesen(sek, sk)) { len = 0; break; }
        uint32_t im = (idx == letzter) ? ((e.groesse - 1) % 512 + 1) : 512;
        memcpy(puffer + len, sek, im);
        len += im;
    }
    free(sek);
    free(fatBuf);
    for (int i = 0; i + 4 < len; i++) if (!memcmp(puffer + i, "%%EOF", 5)) return true;
    return false;
}

// Fingerprint from size, first 256 and last up to 256 bytes - read raw
static uint32_t rohKennzahl(const RohEintrag &e)
{
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!sek || !fatBuf) { if (sek) free(sek); if (fatBuf) free(fatBuf); return 0; }
    uint32_t summe = e.groesse;
    uint32_t sk = rohSektor(e.start, 0, fatBuf);
    if (sk && sdLesen(sek, sk)) {
        uint32_t n = e.groesse < 256 ? e.groesse : 256;
        for (uint32_t i = 0; i < n; i++) summe = summe * 31u + sek[i];
    }
    if (e.groesse > 512) {
        sk = rohSektor(e.start, (e.groesse - 1) / 512, fatBuf);
        if (sk && sdLesen(sek, sk)) {
            uint32_t im = (e.groesse - 1) % 512 + 1;
            for (uint32_t i = im > 256 ? im - 256 : 0; i < im; i++) summe = summe * 31u + sek[i];
        }
    }
    if (e.groesse > 2048) {                         // and a piece from the middle (image data)
        sk = rohSektor(e.start, e.groesse / 1024, fatBuf);
        if (sk && sdLesen(sek, sk))
            for (uint32_t i = 0; i < 256; i++) summe = summe * 31u + sek[i];
    }
    free(sek);
    free(fatBuf);
    return summe;
}

// Reads a file raw along the allocation chain, sector by sector
struct RohLeser {
    uint32_t groesse = 0, pos = 0, cluster = 0;
    uint8_t  idx = 0;
    uint8_t *sek = nullptr, *fatBuf = nullptr;
    bool beginnen(uint32_t start, uint32_t g)
    {
        groesse = g; pos = 0; cluster = start; idx = 0;
        sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
        fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
        return sek && fatBuf;
    }
    int lesen(uint8_t *ziel, size_t n)
    {
        size_t gelesen = 0;
        while (gelesen + 512 <= n && pos < groesse) {
            if (cluster < 2 || cluster >= 0x0FFFFFF8) return gelesen ? (int)gelesen : -1;
            if (!sdLesen(sek, clusterSektor(cluster) + idx)) return -1;
            size_t k = groesse - pos < 512 ? groesse - pos : 512;
            memcpy(ziel + gelesen, sek, k);
            gelesen += k;
            pos += k;
            if (++idx >= g_fat.sektorenProCluster) { idx = 0; cluster = fatNaechster(cluster, fatBuf); }
        }
        return (int)gelesen;
    }
    void ende() { if (sek) free(sek); if (fatBuf) free(fatBuf); sek = fatBuf = nullptr; }
};

// ---- Memo list: sent, still lying on the card ----
#define MAX_FERTIG    64
#define MAX_GELOESCHT 16
static Fertig g_fertig[MAX_FERTIG];
static int    g_fertigAnzahl = 0;
static Fertig g_geloescht[MAX_GELOESCHT];   // Signatures of cleared-away files: that is how their ghosts are spotted
static int    g_geloeschtAnzahl = 0;

static void fertigSpeichern()
{
    g_nvs.begin("scanstick", false);
    if (g_fertigAnzahl) g_nvs.putBytes("fertig", g_fertig, g_fertigAnzahl * sizeof(Fertig));
    else g_nvs.remove("fertig");
    if (g_geloeschtAnzahl) g_nvs.putBytes("geloescht", g_geloescht, g_geloeschtAnzahl * sizeof(Fertig));
    else g_nvs.remove("geloescht");
    g_nvs.end();
}

static void fertigLaden()
{
    g_nvs.begin("scanstick", true);
    size_t n = g_nvs.getBytesLength("fertig");
    if (n && n <= sizeof g_fertig && n % sizeof(Fertig) == 0) { g_nvs.getBytes("fertig", g_fertig, n); g_fertigAnzahl = n / sizeof(Fertig); }
    n = g_nvs.getBytesLength("geloescht");
    if (n && n <= sizeof g_geloescht && n % sizeof(Fertig) == 0) { g_nvs.getBytes("geloescht", g_geloescht, n); g_geloeschtAnzahl = n / sizeof(Fertig); }
    g_nvs.end();
    if (g_fertigAnzahl) logZeile(String("[merk] ") + g_fertigAnzahl + " sent file(s) are still on the card according to flash");
}

static int fertigIndex(uint32_t start, uint32_t groesse)
{
    for (int i = 0; i < g_fertigAnzahl; i++) if (g_fertig[i].start == start && g_fertig[i].groesse == groesse) return i;
    return -1;
}

static bool istGeloescht(uint32_t start, uint32_t groesse)
{
    for (int i = 0; i < g_geloeschtAnzahl; i++) if (g_geloescht[i].start == start && g_geloescht[i].groesse == groesse) return true;
    return false;
}

// Start block and size alone are NOT enough: after the cleanup the card is
// empty, the next scan lands on the same start block, and the same sheet
// twice has the same size. On 20.09.2026 such a new file counted as a ghost
// and was never sent. Only the checksum taken from the content tells the
// return of an old file apart from a new one in the same place.
static int fertigIndexE(const RohEintrag &e)
{
    int i = fertigIndex(e.start, e.groesse);
    if (i >= 0 && g_fertig[i].kennzahl && rohKennzahl(e) != g_fertig[i].kennzahl) return -1;
    return i;
}

static bool istGeloeschtE(const RohEintrag &e)
{
    for (int i = 0; i < g_geloeschtAnzahl; i++)
        if (g_geloescht[i].start == e.start && g_geloescht[i].groesse == e.groesse)
            return !g_geloescht[i].kennzahl || rohKennzahl(e) == g_geloescht[i].kennzahl;
    return false;
}

static void fertigMerken(const RohEintrag &e, uint32_t kennzahl, const String &sendeName)
{
    int alt = fertigIndex(e.start, e.groesse);
    if (alt >= 0 && g_fertig[alt].kennzahl == kennzahl) return;
    if (alt >= 0) {
        // Same start block, same size, different content: the entry now belongs
        // to the new file. If the old checksum stayed, the new file would count
        // as new on every look and would be uploaded endlessly.
        geloeschtMerken(g_fertig[alt]);
        memmove(g_fertig + alt, g_fertig + alt + 1, (g_fertigAnzahl - alt - 1) * sizeof(Fertig));
        g_fertigAnzahl--;
    }
    if (g_fertigAnzahl >= MAX_FERTIG) { memmove(g_fertig, g_fertig + 1, (MAX_FERTIG - 1) * sizeof(Fertig)); g_fertigAnzahl = MAX_FERTIG - 1; }
    Fertig &f = g_fertig[g_fertigAnzahl++];
    f.start = e.start; f.groesse = e.groesse; f.kennzahl = kennzahl;
    strncpy(f.name, sendeName.c_str(), 39); f.name[39] = 0;
    fertigSpeichern();
}

static void geloeschtMerken(const Fertig &f)
{
    if (g_geloeschtAnzahl >= MAX_GELOESCHT) { memmove(g_geloescht, g_geloescht + 1, (MAX_GELOESCHT - 1) * sizeof(Fertig)); g_geloeschtAnzahl = MAX_GELOESCHT - 1; }
    g_geloescht[g_geloeschtAnzahl++] = f;
}

// Is there a scan in the root that is neither sent nor known as a ghost?
// Is there anything in the root still to be sent? Ghosts do not count:
// entries pointing at free blocks or at the blocks of another entry are
// business of the cleanup window - else the watcher would circle on them.
static bool rohOffen()
{
    RohEintrag liste[MAX_FUND];
    int n = rohListe(0, liste, MAX_FUND, true);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    bool offen = false;
    for (int i = 0; i < n && !offen; i++) {
        RohEintrag &e = liste[i];
        if (fertigIndexE(e) >= 0 || istGeloeschtE(e)) continue;
        if (e.start < 2 || (fatBuf && fatNaechster(e.start, fatBuf) == 0)) continue;
        bool geteilt = false;
        for (int k = 0; k < n; k++) if (k != i && liste[k].start == e.start) geteilt = true;
        if (!geteilt) offen = true;
    }
    if (fatBuf) free(fatBuf);
    return offen;
}

// What in the root is NOT yet sent - for the display.
static void rohOffenSumme(int &anzahl, uint32_t &summe)
{
    anzahl = 0; summe = 0;
    RohEintrag liste[MAX_FUND];
    int n = rohListe(0, liste, MAX_FUND, true);
    for (int i = 0; i < n; i++)
        if (fertigIndexE(liste[i]) < 0 && !istGeloeschtE(liste[i])) {
            anzahl++;
            summe += liste[i].groesse;
        }
}


// ---- Card self-check: a small fsck on the raw sectors ----
// The 780 writes its old directory view back and aborts jobs. What is left
// behind - doubly used blocks, orphaned chains, chains that are too short,
// the dirty flag - made it reject the card completely on 20.09.2026
// ("connect USB stick"). Runs only when the printer does not see the card:
// at start before attaching, and in the cleanup window.
static KartenBefund g_befund = {};
static uint32_t     g_befundZeit = 0;
static int          g_heillosFolge = 0;

static inline bool bitDa(const uint8_t *bits, uint32_t c) { return bits[c >> 3] & (1 << (c & 7)); }
static inline void bitSetzen(uint8_t *bits, uint32_t c)   { bits[c >> 3] |= (1 << (c & 7)); }
static inline void bitLoeschen(uint8_t *bits, uint32_t c) { bits[c >> 3] &= ~(1 << (c & 7)); }

// Write one table entry into both tables (the upper 4 bits stay)
static bool fatSetzen(uint32_t c, uint32_t wert, uint8_t *fatBuf)
{
    uint32_t versatz = c * 4, sek = versatz / 512, off = versatz % 512;
    if (!sdLesen(fatBuf, g_fat.fatStart + sek)) return false;
    fatBuf[off] = wert & 0xFF; fatBuf[off + 1] = (wert >> 8) & 0xFF; fatBuf[off + 2] = (wert >> 16) & 0xFF;
    fatBuf[off + 3] = (fatBuf[off + 3] & 0xF0) | ((wert >> 24) & 0x0F);
    bool ok = sdSchreiben(fatBuf, g_fat.fatStart + sek);
    if (ok && g_fat.anzahlFats > 1) ok = sdSchreiben(fatBuf, g_fat.fatStart + g_fat.fatSektoren + sek);
    return ok;
}

// Mark the chain from 'start' in the allocation map. sollCluster > 0: that is
// the maximum length of the file, beyond it the chain is cut off. Returns the
// number of marked clusters. kreuz: a cluster already belonged to someone else.
// kaputt: the chain ran into nothing or was too long (repaired on 'heilen').
static uint32_t ketteMarkieren(uint32_t start, uint32_t sollCluster, uint8_t *bits, uint8_t *fatBuf,
                               bool heilen, bool &kreuz, bool &kaputt)
{
    kreuz = false; kaputt = false;
    uint32_t n = 0, c = start, vorher = 0;
    while (c >= 2 && c < g_fat.gesamtCluster + 2) {
        if (bitDa(bits, c)) { kreuz = true; return n; }
        bitSetzen(bits, c);
        n++;
        vorher = c;
        uint32_t next = fatNaechster(c, fatBuf);
        if (sollCluster && n == sollCluster) {
            if (next < 0x0FFFFFF8) { kaputt = true; if (heilen) fatSetzen(c, 0x0FFFFFFF, fatBuf); }
            return n;
        }
        if (n > g_fat.gesamtCluster) { kaputt = true; break; }
        c = next;
    }
    if (c >= 0x0FFFFFF8) return n;                 // clean end
    kaputt = true;                                  // 0, reserved or out of range
    if (heilen && vorher) fatSetzen(vorher, 0x0FFFFFFF, fatBuf);
    return n;
}

static void ketteEntmarkieren(uint32_t start, uint32_t n, uint8_t *bits, uint8_t *fatBuf)
{
    uint32_t c = start;
    for (uint32_t i = 0; i < n && c >= 2 && c < g_fat.gesamtCluster + 2; i++) {
        bitLoeschen(bits, c);
        c = fatNaechster(c, fatBuf);
    }
}

// Walk one directory: mark every file and every subfolder, count the damage
// and fix it on 'heilen'. On a double claim the entry found first wins -
// the root, that is the printer's file, comes first.
static bool pruefeVerzeichnis(uint32_t dirCluster, int tiefe, uint8_t *bits, uint8_t *sek, uint8_t *fatBuf,
                              bool heilen, KartenBefund &b)
{
    if (tiefe > 3) return true;
    uint32_t clusterBytes = (uint32_t)g_fat.sektorenProCluster * 512;
    uint32_t lfnSektor[20]; int lfnOffset[20]; int lfnN = 0;
    uint32_t cl = dirCluster;
    for (int schutz = 0; cl >= 2 && cl < 0x0FFFFFF8 && schutz < 1024; schutz++) {
        for (uint8_t i = 0; i < g_fat.sektorenProCluster; i++) {
            uint32_t sektor = clusterSektor(cl) + i;
            if (!sdLesen(sek, sektor)) return false;
            bool geaendert = false;
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { if (geaendert) sdSchreiben(sek, sektor); return true; }
                if (d[0] == 0xE5) { lfnN = 0; continue; }
                if (d[11] == 0x0F) { if (lfnN < 20) { lfnSektor[lfnN] = sektor; lfnOffset[lfnN] = e; lfnN++; } continue; }
                if ((d[11] & 0x08) || d[0] == '.') { lfnN = 0; continue; }   // volume label, . and ..
                uint32_t start = eintragCluster(d), groesse = le32(d + 28);
                bool istOrdner = d[11] & 0x10;
                if (start < 2) { lfnN = 0; continue; }                        // empty file
                bool kreuz, kaputt;
                uint32_t soll = istOrdner ? 0 : (groesse + clusterBytes - 1) / clusterBytes;
                uint32_t n = ketteMarkieren(start, soll, bits, fatBuf, heilen, kreuz, kaputt);
                if (kreuz) {
                    b.kreuz++;
                    if (heilen) {
                        ketteEntmarkieren(start, n, bits, fatBuf);   // what stays free is cleared by the orphan pass
                        d[0] = 0xE5;
                        for (int k = 0; k < lfnN; k++) {
                            if (lfnSektor[k] == sektor) { sek[lfnOffset[k]] = 0xE5; continue; }
                            bool schon = false;
                            for (int m = 0; m < k; m++) if (lfnSektor[m] == lfnSektor[k]) schon = true;
                            if (schon || !sdLesen(fatBuf, lfnSektor[k])) continue;
                            for (int m = 0; m < lfnN; m++) if (lfnSektor[m] == lfnSektor[k]) fatBuf[lfnOffset[m]] = 0xE5;
                            sdSchreiben(fatBuf, lfnSektor[k]);
                        }
                        geaendert = true;
                        b.geaendert = true;
                    }
                } else {
                    if (kaputt) { b.gekuerzt++; if (heilen) b.geaendert = true; }
                    if (!istOrdner && n < soll) {                                // chain shorter than the size
                        if (!kaputt) b.gekuerzt++;
                        if (heilen) {
                            uint32_t neu = n * clusterBytes;
                            d[28] = neu & 0xFF; d[29] = (neu >> 8) & 0xFF; d[30] = (neu >> 16) & 0xFF; d[31] = (neu >> 24) & 0xFF;
                            geaendert = true;
                            b.geaendert = true;
                        }
                    }
                    if (istOrdner) {
                        if (geaendert) { sdSchreiben(sek, sektor); geaendert = false; }
                        if (!pruefeVerzeichnis(start, tiefe + 1, bits, sek, fatBuf, heilen, b)) return false;
                        if (!sdLesen(sek, sektor)) return false;
                    }
                }
                lfnN = 0;
            }
            if (geaendert) sdSchreiben(sek, sektor);
        }
        cl = fatNaechster(cl, fatBuf);
    }
    return true;
}

static String befundText(const KartenBefund &b)
{
    if (b.zuGross) return "partition too large for the check";
    if (b.heillos) return "file system not readable";
    String t;
    if (b.fatAbweichungen) t += String(b.fatAbweichungen) + " table sector(s) differing, ";
    if (b.kreuz)           t += String(b.kreuz) + " cross-link(s), ";
    if (b.verwaist)        t += String(b.verwaist) + " orphaned clusters, ";
    if (b.gekuerzt)        t += String(b.gekuerzt) + " chain(s) shortened, ";
    if (b.schmutzig)       t += "dirty flag, ";
    if (t.isEmpty()) return "in order";
    t.remove(t.length() - 2);
    return t + (b.geaendert ? " - fixed" : "");
}

// The actual check. Only call it when the host does not see the card.
static void kartePruefen(bool heilen, const char *anlass)
{
    uint32_t t0 = millis();
    KartenBefund b = {};
    g_fat.gueltig = false;
    fatLageLesen();
    if (!g_fat.gueltig || g_fat.gesamtCluster < 16) {
        b.heillos = true;
        g_heillosFolge++;
        g_befund = b; g_befundZeit = millis();
        logZeile(String("[karte] ") + anlass + ": file system not readable (" + g_heillosFolge + " time(s) in a row)");
        return;
    }
    uint32_t bytes = (g_fat.gesamtCluster + 2 + 7) / 8;
    if (bytes > 65536) {
        b.zuGross = true;
        g_befund = b; g_befundZeit = millis();
        logZeile(String("[karte] ") + anlass + ": " + g_fat.gesamtCluster + " clusters - too large for the check, make the partition smaller");
        return;
    }
    uint8_t *bits   = (uint8_t *)calloc(bytes, 1);
    uint8_t *sek    = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!bits || !sek || !fatBuf) {
        if (bits) free(bits); if (sek) free(sek); if (fatBuf) free(fatBuf);
        logZeile("[karte] no memory for the check");
        return;
    }

    // 1. Compare both tables - the first one counts
    if (g_fat.anzahlFats > 1) {
        for (uint32_t x = 0; x < g_fat.fatSektoren; x++) {
            if (!sdLesen(sek, g_fat.fatStart + x) || !sdLesen(fatBuf, g_fat.fatStart + g_fat.fatSektoren + x)) break;
            if (memcmp(sek, fatBuf, 512)) {
                b.fatAbweichungen++;
                if (heilen && sdSchreiben(sek, g_fat.fatStart + g_fat.fatSektoren + x)) b.geaendert = true;
            }
        }
    }

    // 2. Walk the root and everything below it
    bool kreuz, kaputt;
    ketteMarkieren(g_fat.rootCluster, 0, bits, fatBuf, heilen, kreuz, kaputt);
    if (kaputt) { b.gekuerzt++; if (heilen) b.geaendert = true; }
    if (!pruefeVerzeichnis(g_fat.rootCluster, 0, bits, sek, fatBuf, heilen, b)) b.heillos = true;

    // 3. Orphaned clusters: in use, but reached by nobody. Count the free ones
    //    on the way - the free counter in the FSInfo sector is fixed along with it.
    uint32_t frei = 0, ersterFrei = 0;
    if (!b.heillos) {
        for (uint32_t x = 0; x < g_fat.fatSektoren; x++) {
            if (!sdLesen(sek, g_fat.fatStart + x)) break;
            bool geaendert = false;
            for (int j = 0; j < 128; j++) {
                uint32_t c = x * 128 + j;
                if (c < 2 || c >= g_fat.gesamtCluster + 2) continue;
                bool belegt = (le32(sek + j * 4) & 0x0FFFFFFF) != 0;
                if (belegt && !bitDa(bits, c)) {
                    b.verwaist++;
                    if (heilen) { sek[j * 4] = 0; sek[j * 4 + 1] = 0; sek[j * 4 + 2] = 0; sek[j * 4 + 3] &= 0xF0; geaendert = true; belegt = false; }
                }
                if (!belegt) { frei++; if (!ersterFrei) ersterFrei = c; }
            }
            if (geaendert) {
                if (sdSchreiben(sek, g_fat.fatStart + x)) b.geaendert = true;
                if (g_fat.anzahlFats > 1) sdSchreiben(sek, g_fat.fatStart + g_fat.fatSektoren + x);
            }
        }
        if (heilen && g_fat.fsInfoSektor && sdLesen(sek, g_fat.partStart + g_fat.fsInfoSektor) && le32(sek) == 0x41615252) {
            if (le32(sek + 488) != frei) {
                uint32_t v = frei;                  memcpy(sek + 488, &v, 4);
                v = ersterFrei ? ersterFrei : 2;    memcpy(sek + 492, &v, 4);
                sdSchreiben(sek, g_fat.partStart + g_fat.fsInfoSektor);
            }
        }
    }

    // 4. Dirty flag, twice over: bit 27 "cleanly unmounted" and bit 26 "no
    //    errors" in the second table entry (Windows), and bit 0 in byte 65 of
    //    the boot sector (Linux sets it on mounting, clears it on unmounting;
    //    if the medium drops out before that it stays - fsck: "Dirty bit is set").
    if (!b.heillos && sdLesen(sek, g_fat.fatStart)) {
        uint32_t v = le32(sek + 4);
        if ((v & 0x0C000000) != 0x0C000000) {
            b.schmutzig = true;
            if (heilen) {
                v |= 0x0C000000;
                sek[4] = v & 0xFF; sek[5] = (v >> 8) & 0xFF; sek[6] = (v >> 16) & 0xFF; sek[7] = (v >> 24) & 0xFF;
                if (sdSchreiben(sek, g_fat.fatStart)) b.geaendert = true;
                if (g_fat.anzahlFats > 1) sdSchreiben(sek, g_fat.fatStart + g_fat.fatSektoren);
            }
        }
    }
    if (!b.heillos && sdLesen(sek, g_fat.partStart) && (sek[65] & 0x01)) {
        b.schmutzig = true;
        if (heilen) {
            sek[65] &= ~0x01;
            if (sdSchreiben(sek, g_fat.partStart)) b.geaendert = true;
            uint16_t sicherung = (uint16_t)sek[50] | ((uint16_t)sek[51] << 8);   // backup copy of the boot sector
            if (sicherung && sicherung < 32) sdSchreiben(sek, g_fat.partStart + sicherung);
        }
    }

    g_heillosFolge = b.heillos ? g_heillosFolge + 1 : 0;
    g_befund = b; g_befundZeit = millis();
    logZeile(String("[karte] ") + anlass + ", " + (millis() - t0) + " ms: " + befundText(b));
    free(bits); free(sek); free(fatBuf);
}

// Last resort: lay out the partition freshly as FAT32 (32 kB clusters like
// mkfs.vfat -s 64). Only when nothing unsent is left on it - the settings
// live in flash, nothing is lost on the card that would not already be at
// the receiver.
static bool karteFormatieren(const char *anlass)
{
    uint8_t *s = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!s) return false;
    uint32_t partStart = 0, partSektoren = 0;
    if (sdLesen(s, 0) && s[510] == 0x55 && s[511] == 0xAA) {
        partStart = le32(s + 446 + 8); partSektoren = le32(s + 446 + 12);
    }
    uint32_t karte = SD_MMC.sectorSize() ? (uint32_t)(SD_MMC.cardSize() / SD_MMC.sectorSize()) : 0;
    if (!partStart || !partSektoren || partStart + partSektoren > karte) { free(s); logZeile("[karte] format: no usable partition"); return false; }
    const uint8_t spc = 64; const uint16_t rsv = 32; const uint8_t nfat = 2;
    uint32_t fatSek = 0;
    for (int i = 0; i < 4; i++) {                     // settle the table size
        uint32_t cl = (partSektoren - rsv - nfat * fatSek) / spc;
        fatSek = ((cl + 2) * 4 + 511) / 512;
    }
    uint32_t cluster = (partSektoren - rsv - nfat * fatSek) / spc;
    if (cluster < 65525) { free(s); logZeile("[karte] format: partition too small for FAT32"); return false; }
    logZeile(String("[karte] formatting (") + anlass + "): " + (partSektoren / 2048) + " MB, " + cluster + " clusters");
    uint32_t t0 = millis();
    bool ok = true;
    // Clear the tables
    memset(s, 0, 512);
    for (uint32_t x = 0; x < nfat * fatSek && ok; x++) ok = sdSchreiben(s, partStart + rsv + x);
    // Clear the root (one cluster)
    for (uint32_t x = 0; x < spc && ok; x++) ok = sdSchreiben(s, partStart + rsv + nfat * fatSek + x);
    // First table sector: media byte, clean flags, root end
    const uint8_t kopf[12] = { 0xF8, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F };
    memset(s, 0, 512); memcpy(s, kopf, 12);
    for (int f = 0; f < nfat && ok; f++) ok = sdSchreiben(s, partStart + rsv + f * fatSek);
    // Boot sector
    memset(s, 0, 512);
    const uint8_t bs[] = { 0xEB, 0x58, 0x90, 'M','S','W','I','N','4','.','1' };
    memcpy(s, bs, sizeof(bs));
    s[11] = 0x00; s[12] = 0x02; s[13] = spc; s[14] = rsv & 0xFF; s[15] = rsv >> 8; s[16] = nfat;
    s[21] = 0xF8; s[24] = 32; s[26] = 64;
    uint32_t v = partStart;    memcpy(s + 28, &v, 4);
    v = partSektoren;          memcpy(s + 32, &v, 4);
    v = fatSek;                memcpy(s + 36, &v, 4);
    v = 2;                     memcpy(s + 44, &v, 4);
    s[48] = 1; s[50] = 6; s[64] = 0x80; s[66] = 0x29;
    v = esp_random();          memcpy(s + 67, &v, 4);
    memcpy(s + 71, "SCANS      ", 11); memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
    if (ok) ok = sdSchreiben(s, partStart) && sdSchreiben(s, partStart + 6);
    // FSInfo
    memset(s, 0, 512);
    v = 0x41615252; memcpy(s, &v, 4);
    v = 0x61417272; memcpy(s + 484, &v, 4);
    v = cluster - 1; memcpy(s + 488, &v, 4);
    v = 3;           memcpy(s + 492, &v, 4);
    s[510] = 0x55; s[511] = 0xAA;
    if (ok) ok = sdSchreiben(s, partStart + 1) && sdSchreiben(s, partStart + 7);
    free(s);
    g_fat.gueltig = false;
    g_fertigAnzahl = 0; g_geloeschtAnzahl = 0; fertigSpeichern();
    logZeile(String("[karte] formatted in ") + (millis() - t0) + " ms" + (ok ? "" : " - WITH ERRORS"));
    return ok;
}

#define AUFRAEUM_VORGABE 3           // Minutes without printer access (the 780 does not touch the stick between jobs at all)
static uint32_t g_aufraeumMin      = AUFRAEUM_VORGABE;
static bool     g_aufraeumJetzt    = false;
static uint32_t g_letztesAufraeumen = 0;


// ---- Read the configuration from the SD card ----
// The file only counts when it has CHANGED since it was last taken over.
// Before, it won at every start - whoever switched the upload target in the
// web interface silently got the old value from the file back at the next
// restart. The test rig failed on exactly that. Returns true when the
// values were taken over.
static bool ladeConfig()
{
    File f = SD_MMC.open("/wifi.cfg");
    if (!f) { logZeile("[cfg] /wifi.cfg missing"); return false; }
    String inhalt = f.readString();
    f.close();

    uint32_t stand = 2166136261u;                    // FNV-1a over the file content
    for (unsigned i = 0; i < inhalt.length(); i++) { stand ^= (uint8_t)inhalt[i]; stand *= 16777619u; }
    g_nvs.begin("scanstick", true);
    uint32_t bekannt = g_nvs.getUInt("cfgstand", 0);
    g_nvs.end();
    if (stand == bekannt) {
        logZeile("[cfg] /wifi.cfg unchanged - the settings from flash count");
        return false;
    }

    // Every ssid= line starts a new network, pass= belongs to the last ssid.
    String neuSsid[MAX_NETZE], neuPass[MAX_NETZE];
    int neu = 0;
    int von = 0;
    while (von < (int)inhalt.length()) {
        int bis = inhalt.indexOf('\n', von);
        if (bis < 0) bis = inhalt.length();
        String line = inhalt.substring(von, bis);
        von = bis + 1;
        line.trim();
        int eq = line.indexOf('=');
        if (eq < 1) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        if (k == "ssid") { if (neu < MAX_NETZE) { neuSsid[neu] = v; neuPass[neu] = ""; neu++; } }
        else if (k == "pass") { if (neu) neuPass[neu - 1] = v; }
        else if (k == "endpoint") cfgEndpoint = v;
        else if (k == "schluessel") cfgSchluessel = v;
    }
    if (neu) {
        cfgNetze = neu;
        for (int i = 0; i < neu; i++) { cfgNetzSsid[i] = neuSsid[i]; cfgNetzPass[i] = neuPass[i]; }
    }
    g_nvs.begin("scanstick", false);
    g_nvs.putUInt("cfgstand", stand);
    g_nvs.end();
    String liste;
    for (int i = 0; i < cfgNetze; i++) liste += (i ? ", " : "") + cfgNetzSsid[i];
    logZeile(String("[cfg] /wifi.cfg newly taken over: networks=") + liste + " endpoint=" + cfgEndpoint);
    return true;
}

// Why did we start? Without this line a restart during operation cannot be
// told apart from a power failure - and brownout, crash and watchdog each
// demand completely different countermeasures.
static const char *g_startGrund = "unknown";
static const char *resetGrund()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power switched on";
        case ESP_RST_SW:       return "software restart";
        case ESP_RST_PANIC:    return "crash (panic)";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout (voltage dip)";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_EXT:      return "external reset";
        case ESP_RST_USB:      return "USB reset";
        default:               return "unknown";
    }
}

// ---- Credentials also kept in the NVS flash ----
// They live on the SD card (/wifi.cfg). If the card does not mount, without a
// copy in flash there would be no WiFi and no web interface - that is, no
// diagnostics exactly when you need them. So we mirror them.
static void cfgAusNvs()
{
    g_nvs.begin("scanstick", true);
    // Networks as lines "ssid<TAB>pass". Older versions only know ssid/pass.
    String netze = g_nvs.getString("netze", "");
    cfgNetze = 0;
    if (netze.isEmpty() && !g_nvs.isKey("netze")) {   // only if the list never existed: old single values
        String s = g_nvs.getString("ssid", "");
        if (s.length()) { cfgNetzSsid[0] = s; cfgNetzPass[0] = g_nvs.getString("pass", ""); cfgNetze = 1; }
    } else {
        int von = 0;
        while (von < (int)netze.length() && cfgNetze < MAX_NETZE) {
            int bis = netze.indexOf('\n', von);
            if (bis < 0) bis = netze.length();
            String zeile = netze.substring(von, bis);
            von = bis + 1;
            int tab = zeile.indexOf('\t');
            if (tab < 1) continue;
            cfgNetzSsid[cfgNetze] = zeile.substring(0, tab);
            cfgNetzPass[cfgNetze] = zeile.substring(tab + 1);
            cfgNetze++;
        }
    }
    cfgEndpoint = g_nvs.getString("endpoint", "");
    cfgWebPass  = g_nvs.getString("webpass", "");
    cfgPraefix  = g_nvs.getString("praefix", "scan");
    cfgSchluessel = g_nvs.getString("schluessel", "");
    g_idleMs    = g_nvs.getUInt("idle", IDLE_VORGABE);
    g_aufraeumMin = g_nvs.getUInt("aufraeum", AUFRAEUM_VORGABE);
    g_loeschen  = true;   // "move to /gesendet" no longer exists (cross-linked clusters, 20.09.2026)
    g_invertiert = g_nvs.getBool("invers", true);
    g_ledHell   = (uint8_t)g_nvs.getUChar("ledhell", 5);
    for (int i = 0; i < Z_ANZAHL; i++) {
        char k[10];
        snprintf(k, sizeof k, "farbe%d", i);
        g_farbe[i] = g_nvs.getUInt(k, g_farbe[i]);
    }
    g_farbeSendet = g_nvs.getUInt("fsendet", g_farbeSendet);
    g_farbeFertig = g_nvs.getUInt("ffertig", g_farbeFertig);
    g_nvs.end();
    if (cfgNetze) logZeile(String("[nvs] ") + cfgNetze + " network(s) from flash, first: " + cfgNetzSsid[0]);
}

static void cfgNachNvs()
{
    g_nvs.begin("scanstick", false);
    String netze;
    for (int i = 0; i < cfgNetze; i++) netze += cfgNetzSsid[i] + "\t" + cfgNetzPass[i] + "\n";
    g_nvs.putString("netze", netze);
    g_nvs.remove("ssid"); g_nvs.remove("pass");   // old single values: otherwise a removed network comes back
    g_nvs.putString("endpoint", cfgEndpoint);
    g_nvs.putString("webpass", cfgWebPass);
    g_nvs.putString("praefix", cfgPraefix);
    g_nvs.putString("schluessel", cfgSchluessel);
    g_nvs.putUInt("idle", g_idleMs);
    g_nvs.putUInt("aufraeum", g_aufraeumMin);
    g_nvs.putBool("loeschen", g_loeschen);
    g_nvs.putBool("invers", g_invertiert);
    g_nvs.putUChar("ledhell", g_ledHell);
    for (int i = 0; i < Z_ANZAHL; i++) {
        char k[10];
        snprintf(k, sizeof k, "farbe%d", i);
        g_nvs.putUInt(k, g_farbe[i]);
    }
    g_nvs.putUInt("fsendet", g_farbeSendet);
    g_nvs.putUInt("ffertig", g_farbeFertig);
    g_nvs.end();
}

// Only kick WiFi off, do not wait for the connection: the printer should see
// the stick as a drive at once, not only after the WiFi timeout.
static String  g_apKennung;      // BSSID of the chosen access point
static int     g_apKanal  = 0;
static long    g_apRssi   = 0;
static uint32_t g_wlanVerloren = 0;   // since when without a network (0 = connected)
#define WLAN_NEUSUCHE 120000          // this long without a network, then search again

// Several access points can carry the same SSID (single APs, no mesh), and the
// stick knows several networks. WiFiMulti searches across all known networks,
// takes the strongest access point and connects to it deliberately by its
// BSSID and channel. Blocks until connected or WIFI_TIMEOUT - which is why
// this only runs once the printer already sees the stick as a drive.
//
// The fixed BSSID has a downside: if exactly that access point fails, the
// auto-reconnect only tries that one. So loop() searches from scratch after
// two minutes without a network - then another access point is allowed too.
static void wlanStarten()
{
    if (!cfgNetze) { logZeile("[wifi] no network known"); return; }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(geraeteName().c_str());
    g_wifiMulti.APlistClean();
    for (int i = 0; i < cfgNetze; i++) g_wifiMulti.addAP(cfgNetzSsid[i].c_str(), cfgNetzPass[i].c_str());

    if (g_wifiMulti.run(WIFI_TIMEOUT) == WL_CONNECTED) {
        g_apKennung = WiFi.BSSIDstr();
        g_apKanal   = WiFi.channel();
        g_apRssi    = WiFi.RSSI();
        g_wlanVerloren = 0;
        logZeile(String("[wifi] ") + WiFi.SSID() + " via " + g_apKennung + " on channel " +
                 g_apKanal + " with " + g_apRssi + " dBm, IP " + WiFi.localIP().toString());
    } else {
        g_apKennung = "";
        logZeile("[wifi] no known network reachable");
    }
}

static bool wifiVerbinden()
{
    if (WiFi.status() == WL_CONNECTED) return true;
    wlanStarten();
    return WiFi.status() == WL_CONNECTED;
}

// ---- Setup without a card in the reader ----
// If the stick knows no network or reaches none, it opens a WiFi of its own
// (name = device name, open) and answers every name lookup with itself -
// phone or computer then open the setup page by themselves, like in a hotel.
// As soon as a known network is reachable, that WiFi goes off again. The scan
// path is untouched by this: USB runs, uploads merely wait.
static DNSServer g_dns;
static bool      g_einrichtung = false;
static bool      g_einrichtungGezeichnet = false;

static void zeigeEinrichtung()
{
    if (g_einrichtungGezeichnet) return;
    g_einrichtungGezeichnet = true;
    g_screen = -2;
    uint32_t rgb = 0xFF6600;
    ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    g_gfx->fillScreen(rgb565(rgb));
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->setTextSize(2);
    g_gfx->setCursor(4, 4);
    g_gfx->print("WIFI SETUP");
    g_gfx->setTextSize(1);
    g_gfx->setCursor(4, 30);
    g_gfx->print("Connect to this network:");
    g_gfx->setTextSize(2);
    g_gfx->setCursor(4, 42);
    g_gfx->print(geraeteName());
    g_gfx->setTextSize(1);
    g_gfx->setCursor(4, 66);
    g_gfx->print("then http://192.168.4.1/");
}

static void einrichtungStarten(const char *grund)
{
    if (g_einrichtung) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(geraeteName().c_str());
    delay(100);
    g_dns.start(53, "*", WiFi.softAPIP());
    g_einrichtung = true;
    g_einrichtungGezeichnet = false;
    logZeile(String("[einrichtung] ") + grund + " - own WiFi \"" + geraeteName() + "\" open, page http://" +
             WiFi.softAPIP().toString() + "/");
}

static void einrichtungBeenden()
{
    if (!g_einrichtung) return;
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_einrichtung = false;
    g_screen = -1;
    logZeile("[einrichtung] network found - own WiFi off again");
}

// split http://host[:port]/path
static bool urlTeile(const String &url, String &host, uint16_t &port, String &pfad)
{
    if (!url.startsWith("http://")) return false;   // deliberately http only, no TLS on the stick
    String rest = url.substring(7);
    int sl = rest.indexOf('/');
    String hostteil = (sl < 0) ? rest : rest.substring(0, sl);
    pfad = (sl < 0) ? "/" : rest.substring(sl);
    int dp = hostteil.indexOf(':');
    if (dp < 0) { host = hostteil; port = 80; }
    else { host = hostteil.substring(0, dp); port = (uint16_t)hostteil.substring(dp + 1).toInt(); }
    return host.length() > 0;
}

// Upload in blocks. We write HTTP ourselves because HTTPClient swallows the
// file in one go and reports no progress - the display needs it, and on weak
// WiFi it is the only way to see whether things move at all or hang.
// ---- Clock via NTP ----
// Only for file names: "Untitled_7.pdf" is worthless in the filing target,
// "scan-20260919-1432.pdf" sorts itself.
#define ZEIT_WIEDERHOLUNG 120000   // try again every 2 minutes until it works
static uint32_t g_zeitVersuch = 0;

static void zeitHolen()
{
    g_zeitVersuch = millis();
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
    struct tm t;
    if (getLocalTime(&t, 5000)) {
        g_zeitOk = true;
        char s[40];
        strftime(s, sizeof s, "%d.%m.%Y %H:%M:%S", &t);
        logZeile(String("[zeit] ") + s);
    } else {
        logZeile("[zeit] no NTP reply - file names get the uptime");
    }
}

// Unique name with time stamp, the extension is kept.
static String neuerName(const String &alt)
{
    String endung;
    int punkt = alt.lastIndexOf('.');
    if (punkt > 0) endung = alt.substring(punkt);
    char stempel[32];
    struct tm t;
    if (g_zeitOk && getLocalTime(&t, 200)) strftime(stempel, sizeof stempel, "%Y%m%d-%H%M%S", &t);
    else snprintf(stempel, sizeof stempel, "after%lus", (unsigned long)(millis() / 1000));
    return cfgPraefix + "-" + stempel + endung;
}

// For links: brackets, spaces and umlauts have to be encoded, otherwise the
// download link points nowhere.
static String urlKodiert(const String &s)
{
    String r;
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') r += c;
        else { char t[5]; snprintf(t, sizeof t, "%%%02X", (unsigned char)c); r += t; }
    }
    return r;
}

// Is the file written completely? For PDF the file itself says so: it ends
// with "%%EOF". That is proof - unlike silence or file size, because the 780
// enters the final size before writing and reserves the space. Without this
// check we have uploaded a half-written file whose tail consisted only of
// empty bytes.
static bool dateiVollstaendig(const String &pfad, uint32_t groesse)
{
    if (!pfad.endsWith(".pdf") && !pfad.endsWith(".PDF")) return true;   // only PDF can be checked
    if (groesse < 32) return false;
    File f = SD_MMC.open(pfad);
    if (!f) return false;
    uint32_t start = groesse > 1024 ? groesse - 1024 : 0;
    f.seek(start);
    char puffer[1025];
    int n = f.read((uint8_t *)puffer, sizeof puffer - 1);
    f.close();
    if (n <= 0) return false;
    for (int i = 0; i + 4 < n; i++)
        if (memcmp(puffer + i, "%%EOF", 5) == 0) return true;
    return false;
}

// Identifier of the file for the receiver. After a move the printer likes to
// restore its old directory view - then "[Untitled].pdf" points at the same
// data again and we upload it a second time. The receiver recognises the
// repeat by this identifier.
//
// Short check number from size plus first and last block - enough to recognise
// the same file again, and it costs hardly any read time. For DELETING without
// an upload it deliberately does not suffice (see sendeGefundene).
static uint32_t dateiKennzahl(const String &pfad, uint32_t groesse)
{
    File f = SD_MMC.open(pfad);
    if (!f) return 0;
    uint32_t summe = groesse;
    uint8_t puffer[256];
    int n = f.read(puffer, sizeof puffer);
    for (int i = 0; i < n; i++) summe = summe * 31u + puffer[i];
    if (groesse > 512) {
        f.seek(groesse - 256);
        n = f.read(puffer, sizeof puffer);
        for (int i = 0; i < n; i++) summe = summe * 31u + puffer[i];
    }
    f.close();
    return summe;
}

#define UP_FEHLER   0
#define UP_OK       1
#define UP_DUPLIKAT 2   // accepted, but the receiver already knew the file
// Upload from any source: lese(puffer, n) delivers bytes, 0 = end, negative =
// error. File via the file system or raw along the allocation chain.
static int ladeHochQuelle(const String &name, const String &id, uint32_t len,
                          std::function<int(uint8_t *, size_t)> lese)
{
    if (!len) { logZeile("[up] " + name + " is empty, skipped"); return UP_FEHLER; }
    String host, ziel;
    uint16_t port;
    if (!urlTeile(cfgEndpoint, host, port, ziel)) {
        logZeile("[up] target address unusable: " + cfgEndpoint);
        return UP_FEHLER;
    }
    ziel += (ziel.indexOf('?') < 0) ? "?name=" : "&name=";
    ziel += name;
    // Unique identifier of the same file. With it the receiver can spot and
    // discard duplicate transfers - for instance when an upload breaks off and
    // is repeated later, or when the printer writes back its old directory
    // view and the file shows up again because of that.
    if (id.length()) { ziel += "&id="; ziel += id; }

    // On weak radio the first connection attempt likes to fail. Giving up once
    // used to mean: the file stays put, the next try only comes with the next
    // scan.
    WiFiClient c;
    bool verbunden = false;
    for (int v = 1; v <= 3 && !verbunden; v++) {
        verbunden = c.connect(host.c_str(), port);
        if (!verbunden) {
            logZeile(String("[up] no connection to ") + host + ":" + port +
                     " (attempt " + v + " of 3)");
            delay(1500);
        }
    }
    if (!verbunden) return UP_FEHLER;
    String sig = uploadSignatur(name, id, len);
    c.print(String("POST ") + ziel + " HTTP/1.1\r\n" +
            "Host: " + host + ":" + port + "\r\n" +
            "Content-Type: application/octet-stream\r\n" +
            "Content-Length: " + len + "\r\n" +
            (sig.length() ? "X-Scan-Auth: " + sig + "\r\n" : String("")) +
            "Connection: close\r\n\r\n");

    zeigeSendenStart(name, len);
    uint8_t puffer[1024];
    uint32_t geschickt = 0, t0 = millis();
    while (geschickt < len) {
        // The printer is writing the next scan: then do not read from the card
        // now. Before v26 the medium was gone during the upload, since v26 we
        // read alongside - and exactly in that overlap the 780 aborted twice
        // with 44.12.05. So abort and catch up once it is done; an upload that
        // arrived twice is a duplicate at the receiver.
        if ((int32_t)(g_lastWrite - t0) > 0) { logZeile("[up] printer is writing - upload aborted, will be caught up"); break; }
        int gelesen = lese(puffer, sizeof puffer);
        if (gelesen <= 0) { logZeile("[up] card delivers no more data"); break; }
        int raus = c.write(puffer, gelesen);
        if (raus != gelesen) { logZeile("[up] connection broke while sending"); break; }
        geschickt += raus;
        zeigeSendenFortschritt(geschickt, len);
        if (millis() - t0 > 180000) { logZeile("[up] timeout while sending"); break; }
    }

    if (geschickt != len) { c.stop(); return UP_FEHLER; }

    int code = 0;
    bool duplikat = false;
    uint32_t tw = millis();
    while (c.connected() && !c.available() && millis() - tw < 15000) delay(10);
    if (c.available()) {
        String zeile = c.readStringUntil('\n');          // "HTTP/1.1 200 OK"
        int sp = zeile.indexOf(' ');
        if (sp > 0) code = zeile.substring(sp + 1, sp + 4).toInt();
        // Rest of the response: if the receiver says "duplicate" (or the older
        // "Duplikat"), it already knew the file - our entry is a ghost (see sendeGefundene).
        uint32_t tr = millis();
        while ((c.connected() || c.available()) && millis() - tr < 3000) {
            if (!c.available()) { delay(10); continue; }
            String z = c.readStringUntil('\n');
            if (z.indexOf("Duplikat") >= 0 || z.indexOf("uplicate") >= 0) duplikat = true;
        }
    }
    c.stop();
    if (WiFi.status() == WL_CONNECTED) { g_rssiLetztLast = WiFi.RSSI(); rssiErfassen(g_rssiLetztLast); }
    logZeile(String("[up] ") + name + " " + (geschickt / 1024) + " kB HTTP " + code +
             (duplikat ? " (duplicate)" : "") + " in " + ((millis() - t0) / 1000) + " s");
    if (code >= 200 && code < 300) { zeigeFertig(geschickt); delay(1200); return duplikat ? UP_DUPLIKAT : UP_OK; }
    return UP_FEHLER;
}

// Upload via the file system (for leftovers in /senden from v25)
static int ladeHoch(fs::FS &fs, const String &pfad, const String &name, const String &id)
{
    File f = fs.open(pfad);
    if (!f) { logZeile("[up] " + pfad + " cannot be opened"); return UP_FEHLER; }
    uint32_t len = f.size();
    int erg = ladeHochQuelle(name, id, len, [&](uint8_t *b, size_t n) { return f.read(b, n); });
    f.close();
    return erg;
}

// ================= Web interface =================
// The stick sits in the printer: no Serial (in MSC mode the CDC belongs to the
// TinyUSB stack), display only for whoever stands in front of it. The web page
// is the only channel that is open from anywhere.

static String menschlich(uint64_t b)
{
    char t[32];
    if (b >= 1024ULL * 1024 * 1024) snprintf(t, sizeof t, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)      snprintf(t, sizeof t, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)             snprintf(t, sizeof t, "%.1f kB", b / 1024.0);
    else                            snprintf(t, sizeof t, "%llu B", b);
    return String(t);
}

static String spurText(bool html);

static String dauer(uint32_t ms)
{
    char t[32];
    uint32_t sek = ms / 1000;
    if (sek < 90) snprintf(t, sizeof t, "%u s", sek);
    else if (sek < 5400) snprintf(t, sizeof t, "%u min %u s", sek / 60, sek % 60);
    else snprintf(t, sizeof t, "%u h %u min", sek / 3600, (sek % 3600) / 60);
    return String(t);
}

// The host's last write accesses as text: for the status page (html) and for
// the log, when a cycle has uploaded nothing.
static String spurText(bool html)
{
    String spur;
    for (int k = 0; k < SPUR_ANZAHL; k++) {
        int i = (g_spurIdx + k) % SPUR_ANZAHL;
        if (!g_spurT[i]) continue;
        uint32_t lba = g_spurLba[i];
        const char *wo = !g_fat.gueltig ? "?" : lba < g_fat.fatStart ? "Boot"
                       : lba < g_fat.ersterDatenSektor ? "FAT"
                       : lba < clusterSektor(g_fat.rootCluster) + g_fat.sektorenProCluster ? "Root" : "Data";
        if (html) spur += String("") + dauer(millis() - g_spurT[i]) + " ago: " + lba + " +" + g_spurN[i] + " (" + wo + ")<br>";
        else      spur += String(lba) + "+" + g_spurN[i] + wo[0] + " ";
    }
    return spur;
}

// The page hands out scanned mail for download. Without a password every
// device on the WiFi can read along - hence Basic Auth as soon as one is set.
static bool webAuth()
{
    if (cfgWebPass.isEmpty()) return true;
    if (g_web.authenticate("scan", cfgWebPass.c_str())) return true;
    g_web.requestAuthentication(BASIC_AUTH, "Scan-Stick");
    return false;
}

static String hexFarbe(uint32_t rgb)
{
    char t[10];
    snprintf(t, sizeof t, "#%06X", (unsigned)(rgb & 0xFFFFFF));
    return String(t);
}

static String htmlKopf(const String &titel)
{
    return String("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Scan-Stick</title><style>"
        "body{font-family:system-ui,sans-serif;margin:0;padding:1rem;background:#15171a;color:#e8e8e8}"
        "h1{font-size:1.15rem;margin:0 0 .2rem}"
        "small{color:#8a94a0}"
        "table{border-collapse:collapse;width:100%;font-size:.92rem;margin-top:.6rem}"
        "td,th{text-align:left;padding:.35rem .5rem;border-bottom:1px solid #2a2e34}"
        "td:first-child{color:#93a2b4;width:48%}"
        "a{color:#6cc3ff}"
        ".ok{color:#7ddc8a}.warn{color:#ffc857}.bad{color:#ff7a7a}"
        "nav{margin:.7rem 0 0}nav a{margin-right:1.1rem}"
        "pre{background:#0b0d10;padding:.6rem;overflow:auto;font-size:.78rem;line-height:1.45}"
        ".btn{display:inline-block;background:#2a4d6e;color:#fff;padding:.45rem .8rem;"
        "border-radius:6px;text-decoration:none;margin:.2rem .4rem .2rem 0}"
        "</style></head><body><h1>") + titel + "</h1>"
        "<nav><a href=\"/\">Status</a><a href=\"/log\">Log</a>"
        "<a href=\"/dateien\">Files</a><a href=\"/roh\">Raw</a>"
        "<a href=\"/einstellungen\">Settings</a>"
        "<a href=\"/update\">Firmware</a></nav>";
}

static String htmlFuss() { return "</body></html>"; }

static String zl(const String &k, const String &v) { return "<tr><td>" + k + "</td><td>" + v + "</td></tr>"; }

static void webStatus()
{
    if (!webAuth()) return;
    uint32_t sec  = SD_MMC.sectorSize();
    bool     sdOk = sec > 0;
    String h = htmlKopf("Scan-Stick " FW_VERSION);
    h += "<table>";
    h += zl("Card", sdOk ? "<span class=\"ok\">mounted</span>"
                          : "<span class=\"bad\">NOT mounted</span>");
    if (sdOk) {
        uint64_t roh = SD_MMC.cardSize();
        h += zl("Card size (raw)", menschlich(roh) + " / " + String((uint32_t)(roh / sec)) + " sectors");
        h += zl("File system", menschlich(SD_MMC.totalBytes()));
    }
    uint32_t seitHost = millis() - g_lastHost;
    h += zl("Last host access", g_lastHost ? ("" + dauer(seitHost)) : "none yet");
    h += zl("Unprocessed writes", g_dirty ? "<span class=\"warn\">yes</span>" : "no");
    h += zl("Written by the host", menschlich(g_bytesGeschrieben) +
            " <small>(since the last run)</small>");
    h += zl("Raw detected", g_rohAnzahl ? (String(g_rohAnzahl) + " file(s), " +
            menschlich(g_rohSumme) + ", " + String(g_rohStabil) + "/" + String(ROH_STABIL) +
            " stable") : "nothing");
    h += zl("Last completion command", g_letztesStop
            ? ("" + dauer(millis() - g_letztesStop))
            : "<span class=\"warn\">none yet - the printer does not report its job end</span>");
    if (g_dirty) {
        h += zl("Waiting for commit, attempt", String(g_versuche) + " of " + String(MAX_VERSUCHE));
        uint32_t rest = (millis() - g_lastWrite < IDLE_MS) ? (IDLE_MS - (millis() - g_lastWrite)) : 0;
        h += zl("Next look in", rest ? dauer(rest) : "right away");
    }
    h += zl("WiFi", WiFi.status() == WL_CONNECTED
              ? ("<span class=\"ok\">" + WiFi.SSID() + "</span>, " + WiFi.localIP().toString() +
                 ", " + String(WiFi.RSSI()) + " dBm")
              : "<span class=\"bad\">not connected</span>");
    if (g_apKennung.length())
        h += zl("Access point", g_apKennung + ", channel " + String(g_apKanal) +
                ", at selection " + String(g_apRssi) + " dBm <small>(strongest across all "
                "known networks)</small>");
    {
        String liste;
        for (int i = 0; i < cfgNetze; i++) liste += (i ? ", " : "") + cfgNetzSsid[i];
        h += zl("Known networks", cfgNetze ? liste : "<span class=\"bad\">none</span>");
    }
    h += zl("Upload target", cfgEndpoint.length() ? cfgEndpoint : "<span class=\"bad\">not set</span>");
    h += zl("Idle time until \"done\"", dauer(g_idleMs));
    h += zl("Sent, still on the card", String(g_fertigAnzahl) + " file(s)" +
            (g_fertigAnzahl ? " <small>(will be cleared in the next idle phase)</small>" : ""));
    h += zl("Cleanup", String("after ") + g_aufraeumMin + " min without printer access" +
            (g_letztesAufraeumen ? ", last run " + dauer(millis() - g_letztesAufraeumen) : ", never yet"));
    h += zl("After sending", "delete from the card <small>(the copy is at the receiver)</small>");
    h += zl("Web interface", cfgWebPass.length() ? "<span class=\"ok\">password protected</span>"
                                                  : "<span class=\"bad\">open, anyone on the WiFi can read the scans</span>");
    h += zl("Upload signature", cfgSchluessel.length() ? "<span class=\"ok\">device key set</span>"
                                                       : "<span class=\"warn\">none, the receiver accepts everything</span>");
    h += zl("Clock", g_zeitOk ? "<span class=\"ok\">set via NTP</span>"
                                 : "<span class=\"warn\">unknown, names with uptime</span>");
    h += zl("Name scheme", cfgPraefix + "-YYYYMMDD-HHMMSS.pdf");
    if (g_rssiAnzahl)
        h += zl("Signal worst / average / best",
                String(g_rssiMin) + " / " + String(g_rssiSumme / (long)g_rssiAnzahl) + " / " +
                String(g_rssiMax) + " dBm <small>(" + String(g_rssiAnzahl) + " measurements)</small>");
    if (g_rssiLetztLast)
        h += zl("Signal at the end of the last upload", String(g_rssiLetztLast) + " dBm");
    {
        String spur = spurText(true);
        h += zl("Last write accesses of the host", spur.length() ? "<small>" + spur + "</small>" : "none");
        String fehler = String((uint32_t)g_usbFehlerLesen) + " read, " + String((uint32_t)g_usbFehlerSchreiben) + " write";
        if (g_usbFehlerZeit) fehler += " <small>(last " + dauer(millis() - g_usbFehlerZeit) + " ago, sector " + String((uint32_t)g_usbFehlerLba) + ")</small>";
        h += zl("Rejected host accesses", (g_usbFehlerLesen || g_usbFehlerSchreiben) ? "<span class=warn>" + fehler + "</span>" : "none");
        if (g_usbFehlerAlt.length()) h += zl("Before the last restart", "<span class=bad>" + g_usbFehlerAlt + "</span>");
        if (g_befundZeit) {
            String t = befundText(g_befund);
            bool schlecht = g_befund.heillos || g_befund.zuGross || (t != "in order" && !g_befund.geaendert);
            h += zl("Card check", String(schlecht ? "<span class=bad>" : "") + t + (schlecht ? "</span>" : "") +
                    " <small>(" + dauer(millis() - g_befundZeit) + " ago)</small>");
        }
    }
    h += zl("Device name", geraeteName() + ".local");
    h += zl("Uptime", dauer(millis()));
    h += zl("Last boot reason", g_startGrund);
    h += zl("Free memory", menschlich(ESP.getFreeHeap()));
    h += "</table><p>"
         "<form method=\"post\" action=\"/jetzt-schauen\" style=\"display:inline\">"
         "<button class=\"btn\" type=\"submit\">Look for scans now</button></form>"
         "<form method=\"post\" action=\"/aufraeumen\" style=\"display:inline\">"
         "<button class=\"btn\" type=\"submit\">Clean up now</button></form>"
         "<form method=\"post\" action=\"/neustart\" style=\"display:inline\">"
         "<button class=\"btn\" type=\"submit\">Restart</button></form></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webLog()
{
    if (!webAuth()) return;
    String h = htmlKopf("Log");
    h += "<p><small>Number at the start of a line = milliseconds since boot. "
         "The buffer lives in RAM, a restart clears it.</small></p><pre>";
    h += g_logPuffer.length() ? g_logPuffer : String("(still empty)");
    h += "</pre>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webDateienListe(const String &pfad, String &h, int tiefe)
{
    File dir = SD_MMC.open(pfad.length() ? pfad : "/");
    if (!dir || !dir.isDirectory()) return;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        String voll = e.path();
        if (e.isDirectory()) {
            h += "<tr><td>" + String(tiefe ? "&nbsp;&nbsp;&nbsp;&nbsp;" : "") +
                 "\xF0\x9F\x93\x81 " + voll + "/</td><td>Folder</td></tr>";
            if (tiefe < 3) webDateienListe(voll, h, tiefe + 1);
        } else {
            h += "<tr><td>" + String(tiefe ? "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;" : "") +
                 "<a href=\"/holen?p=" + urlKodiert(voll) + "\">" + voll + "</a></td><td>" +
                 menschlich(e.size()) + "</td></tr>";
        }
    }
}

static void webDateien()
{
    if (!webAuth()) return;
    String h = htmlKopf("Files on the card");
    if (!SD_MMC.sectorSize()) {
        h += "<p class=\"bad\">Card is not mounted.</p>";
    } else {
        // DELIBERATELY no unmounting and remounting of the card any more. That ran
        // here in the main loop while the USB part served read accesses of the
        // printer in parallel - if both met, the USB part touched a card driver
        // that was just being torn down and the stick restarted. A mere look at
        // this page must never put operation at risk.
        // Root read raw - that is how the printer really sees it right now
        RohEintrag liste[MAX_FUND];
        int n = rohListe(0, liste, MAX_FUND, false);
        h += "<table><tr><th>Root (raw)</th><th>Size</th><th>State</th></tr>";
        for (int i = 0; i < n; i++) {
            String nm = liste[i].name;
            String stand = fertigIndexE(liste[i]) >= 0 ? "<span class=\"ok\">sent</span>"
                         : istGeloeschtE(liste[i]) ? "<span class=\"warn\">ghost</span>"
                         : istScanName(nm) && liste[i].groesse >= ROH_MIN_GROESSE ? "<span class=\"warn\">open</span>" : "";
            h += "<tr><td><a href=\"/holen?p=" + urlKodiert("/" + nm) + "\">" + nm + "</a></td><td>" +
                 menschlich(liste[i].groesse) + "</td><td>" + stand + "</td></tr>";
        }
        h += "</table>";
        h += "<table><tr><th>Folder</th><th>Size</th></tr>";
        webDateienListe("/gesendet", h, 1);
        webDateienListe("/senden", h, 1);
        h += "</table>";
    }
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webHolen()
{
    if (!webAuth()) return;
    String pfad = g_web.arg("p");
    if (!pfad.startsWith("/")) { g_web.send(400, "text/plain; charset=utf-8", "path missing\n"); return; }
    if (pfad.lastIndexOf('/') == 0) {
        // Root: read raw, the mount from startup does not know fresh files
        RohEintrag liste[MAX_FUND];
        int n = rohListe(0, liste, MAX_FUND, false);
        for (int i = 0; i < n; i++) {
            if (pfad.substring(1) != liste[i].name) continue;
            String nm = liste[i].name;
            String typ = nm.endsWith(".pdf") ? "application/pdf" : "application/octet-stream";
            RohLeser rl;
            if (!rl.beginnen(liste[i].start, liste[i].groesse)) break;
            g_web.sendHeader("Content-Disposition", "inline; filename=\"" + nm + "\"");
            g_web.setContentLength(liste[i].groesse);
            g_web.send(200, typ, "");
            uint8_t puffer[1024];
            WiFiClient cl = g_web.client();
            for (int k; (k = rl.lesen(puffer, sizeof puffer)) > 0;) if (cl.write(puffer, k) != (size_t)k) break;
            rl.ende();
            return;
        }
        g_web.send(404, "text/plain; charset=utf-8", "not found\n");
        return;
    }
    File f = SD_MMC.open(pfad);
    if (!f || f.isDirectory()) { g_web.send(404, "text/plain; charset=utf-8", "not found\n"); return; }
    // Without a filename the download was called "holen" in the browser, and
    // without a matching type Chrome rated it unsafe and blocked it. With name
    // and type the browser simply shows the PDF.
    String basis = pfad;
    int sl = basis.lastIndexOf('/');
    if (sl >= 0) basis = basis.substring(sl + 1);
    String typ = basis.endsWith(".pdf") ? "application/pdf"
               : basis.endsWith(".txt") || basis.endsWith(".cfg") ? "text/plain; charset=utf-8"
               : "application/octet-stream";
    g_web.sendHeader("Content-Disposition", "inline; filename=\"" + basis + "\"");
    g_web.streamFile(f, typ);
    f.close();
}

// Accept forms only from our own page. On a submission from a foreign page a
// browser always sends the header Origin along - if it does not match our
// host, it was not our page. Without that, any web page in the same browser
// could bend the upload target or restart the stick in the middle of an
// upload; a stored password is sent along by the browser automatically. Tools
// like curl send no Origin and may pass, they have no password stored in the
// browser anyway.
static bool herkunftOk()
{
    String eigen = "http://" + g_web.hostHeader();
    String origin = g_web.header("Origin");
    if (origin.length()) return origin == eigen;
    String referer = g_web.header("Referer");
    if (referer.length()) return referer.startsWith(eigen + "/");
    return true;
}

static bool herkunftPruefen()
{
    if (herkunftOk()) return true;
    g_web.send(403, "text/plain; charset=utf-8", "request did not come from this page\n");
    return false;
}

static void webJetztSchauen()
{
    if (!webAuth() || !herkunftPruefen()) return;
    // Look once. Earlier g_dirty was set here - that triggered the retry
    // mechanism for the printer commit, which is not what is meant here.
    g_einmalSchauen = true;
    logZeile("[web] search triggered manually");
    g_web.sendHeader("Location", "/log");
    g_web.send(303, "text/plain; charset=utf-8", "");
}

static void webUpdateSeite()
{
    if (!webAuth()) return;
    String h = htmlKopf("Update firmware");
    h += "<p>Select the file <code>scanner.ino.bin</code>. The image is written to the "
         "second program area; only when it is complete and checked does the stick "
         "start with it. If the transfer breaks off, the previous firmware keeps "
         "running unchanged.</p>";
    h += "<form method=\"post\" action=\"/update\" enctype=\"multipart/form-data\">"
         "<p><input type=\"file\" name=\"firmware\" accept=\".bin\"></p>"
         "<p><button class=\"btn\" type=\"submit\">Upload and restart</button></p></form>";
    h += "<p><small>Currently running: " FW_VERSION "</small></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

// Check page for the raw reader: shows what it sees without a filesystem driver.
static void webRoh()
{
    if (!webAuth()) return;
    int anz = 0;
    uint32_t summe = 0;
    fatLageLesen();
    bool ok = rohVerzeichnis(anz, summe);
    String h = htmlKopf("Raw reading");
    h += "<table>";
    h += zl("Layout detected", g_fat.gueltig ? "<span class=\"ok\">yes</span>"
                                            : "<span class=\"bad\">no</span>");
    if (g_fat.gueltig) {
        h += zl("Sectors per cluster", String(g_fat.sektorenProCluster));
        h += zl("Allocation table from sector", String(g_fat.fatStart));
        h += zl("Data area from sector", String(g_fat.ersterDatenSektor));
        h += zl("Root directory in cluster", String(g_fat.rootCluster));
    }
    h += zl("Read successful", ok ? "yes" : "<span class=\"bad\">no</span>");
    h += zl("Files found", String(anz) + " <small>(from " +
            String(ROH_MIN_GROESSE / 1024) + " kB; smaller ones count as helper files)</small>");
    h += zl("Sum of the sizes", menschlich(summe));
    h += zl("Stable readings in a row", String(g_rohStabil) + " of " + String(ROH_STABIL));
    h += "</table><p><small>This look goes straight to the sectors of the card and "
         "does not disturb the printer - unlike the unmounting and remounting that "
         "used to be needed.</small></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webNeustart()
{
    if (!webAuth() || !herkunftPruefen()) return;
    g_web.send(200, "text/html; charset=utf-8",
               htmlKopf("Restart") + "<p>The stick is restarting. This page will be back in about "
               "15 seconds.</p>" + htmlFuss());
    delay(300);
    sanftNeustarten();
}

static void webEinstellungen()
{
    if (!webAuth()) return;

    if (g_web.method() == HTTP_POST) {
        if (!herkunftPruefen()) return;
        if (g_web.hasArg("endpoint")) cfgEndpoint = g_web.arg("endpoint");
        // Known networks: remove via checkbox, add via field. Takes effect at the
        // next scan (restart or two minutes without a network).
        for (int i = MAX_NETZE - 1; i >= 0; i--) {
            char k[12];
            snprintf(k, sizeof k, "netzweg%d", i);
            if (g_web.hasArg(k) && i < cfgNetze) {
                for (int j = i; j + 1 < cfgNetze; j++) { cfgNetzSsid[j] = cfgNetzSsid[j + 1]; cfgNetzPass[j] = cfgNetzPass[j + 1]; }
                cfgNetze--;
            }
        }
        if (g_web.hasArg("ssid_neu") && g_web.arg("ssid_neu").length()) {
            String neuS = g_web.arg("ssid_neu"), neuP = g_web.arg("pass_neu");
            int vorhanden = -1;
            for (int i = 0; i < cfgNetze; i++) if (cfgNetzSsid[i] == neuS) vorhanden = i;
            if (vorhanden >= 0) cfgNetzPass[vorhanden] = neuP;                       // refresh password
            else if (cfgNetze < MAX_NETZE) { cfgNetzSsid[cfgNetze] = neuS; cfgNetzPass[cfgNetze] = neuP; cfgNetze++; }
        }
        if (g_web.hasArg("aufraeum")) {
            uint32_t m = g_web.arg("aufraeum").toInt();
            if (m >= 1 && m <= 1440) g_aufraeumMin = m;
        }
        if (g_web.hasArg("idle")) {
            uint32_t sek = g_web.arg("idle").toInt();
            if (sek >= 5 && sek <= 600) g_idleMs = sek * 1000;
        }
        if (g_web.hasArg("speichern")) {
            bool neu = g_web.hasArg("invers");
            if (neu != g_invertiert) { g_invertiert = neu; g_gfx->invertDisplay(g_invertiert); g_screen = -1; }
        }
        if (g_web.hasArg("ledhell")) {
            int v = g_web.arg("ledhell").toInt();
            if (v >= 0 && v <= 31) { g_ledHell = (uint8_t)v; ledColor(g_ledR, g_ledG, g_ledB); }
        }
        if (g_web.hasArg("praefix")) {
            String v = g_web.arg("praefix");
            v.trim();
            if (v.length() && v.length() < 24) cfgPraefix = v;
        }
        // An empty password field means "leave unchanged", not "protection off" -
        // otherwise a careless save switches the protection off.
        if (g_web.hasArg("webpass") && g_web.arg("webpass").length()) cfgWebPass = g_web.arg("webpass");
        if (g_web.hasArg("passweg") && g_web.arg("passweg") == "ja") cfgWebPass = "";
        if (g_web.hasArg("schluessel") && g_web.arg("schluessel").length()) cfgSchluessel = g_web.arg("schluessel");
        if (g_web.hasArg("schluesselweg") && g_web.arg("schluesselweg") == "ja") cfgSchluessel = "";
        for (int i = 0; i < Z_ANZAHL; i++) {
            char k[10];
            snprintf(k, sizeof k, "f%d", i);
            if (g_web.hasArg(k)) g_farbe[i] = strtoul(g_web.arg(k).c_str() + 1, nullptr, 16);
        }
        if (g_web.hasArg("fsendet")) g_farbeSendet = strtoul(g_web.arg("fsendet").c_str() + 1, nullptr, 16);
        if (g_web.hasArg("ffertig")) g_farbeFertig = strtoul(g_web.arg("ffertig").c_str() + 1, nullptr, 16);
        cfgNachNvs();
        logZeile("[web] settings saved");
        g_screen = -1;            // redraw the display with the new colors
        g_web.sendHeader("Location", "/einstellungen?ok=1");
        g_web.send(303, "text/plain; charset=utf-8", "");
        return;
    }

    String h = htmlKopf("Settings");
    if (g_web.hasArg("ok")) h += "<p class=\"ok\">Saved.</p>";
    h += "<form method=\"post\" action=\"/einstellungen\">"
         "<input type=\"hidden\" name=\"speichern\" value=\"1\"><table>";
    h += "<tr><td>Brightness of the status LED<br><small>0 = off, 31 = maximum. "
         "Fully turned up it is very glaring as a steady light.</small></td>"
         "<td><input name=\"ledhell\" type=\"number\" min=\"0\" max=\"31\" value=\"" +
         String(g_ledHell) + "\"></td></tr>";
    h += String("<tr><td>Invert display colors<br><small>otherwise this module shows everything as a "
         "negative - blue set here appears yellow</small></td><td>"
         "<label><input type=\"checkbox\" name=\"invers\" value=\"1\"") +
         (g_invertiert ? " checked" : "") + "> invert</label></td></tr>";
    h += "<tr><td>Upload target</td><td><input name=\"endpoint\" size=\"34\" value=\"" + cfgEndpoint + "\"></td></tr>";
    {
        String netze;
        for (int i = 0; i < cfgNetze; i++)
            netze += String("<label><input type=\"checkbox\" name=\"netzweg") + i + "\" value=\"1\"> " + cfgNetzSsid[i] +
                     " <small>remove</small></label><br>";
        if (!cfgNetze) netze = "<span class=\"bad\">none stored</span><br>";
        netze += "<small>new:</small> <input name=\"ssid_neu\" placeholder=\"Network name\" size=\"14\"> "
                 "<input name=\"pass_neu\" type=\"password\" placeholder=\"Password\" size=\"14\">";
        h += "<tr><td>Known Wi-Fi networks<br><small>up to " + String(MAX_NETZE) + "; the strongest reachable "
             "access point wins. Changes apply from the next scan (restart).</small></td><td>" + netze + "</td></tr>";
    }
    h += "<tr><td>Start of the file names<br><small>gives e.g. <code>" + cfgPraefix +
         "-20260919-143205.pdf</code>; with several sticks enter the location here</small></td>"
         "<td><input name=\"praefix\" size=\"16\" value=\"" + cfgPraefix + "\"></td></tr>";
    h += "<tr><td>Clean up after minutes of quiet<br><small>the printer must not have touched the "
         "stick for this long before sent files are cleared off the card - that is the only moment "
         "in which the medium is briefly gone</small></td><td><input name=\"aufraeum\" type=\"number\" "
         "min=\"1\" max=\"1440\" value=\"" + String(g_aufraeumMin) + "\"></td></tr>";
    h += "<tr><td>Quiet period in seconds<br><small>this much silence until a scan counts as done "
         "(the 780 needs 45)</small></td><td><input name=\"idle\" type=\"number\" min=\"5\" max=\"600\" value=\"" +
         String(g_idleMs / 1000) + "\"></td></tr>";
    h += "<tr><td>Password of the web interface<br><small>the user name is <b>scan</b>. "
         "Leave empty = unchanged.</small></td><td><input name=\"webpass\" type=\"password\" size=\"18\">"
         "<br><label><small><input type=\"checkbox\" name=\"passweg\" value=\"ja\"> remove protection</small></label></td></tr>";
    h += String("<tr><td>Device key for the upload<br><small>signs every upload "
         "(HMAC-SHA256); the same key belongs in the receiver (<code>SCAN_KEY</code>). "
         "Leave empty = unchanged. At present: ") + (cfgSchluessel.length() ? "set" : "none") +
         "</small></td><td><input name=\"schluessel\" type=\"password\" size=\"18\">"
         "<br><label><small><input type=\"checkbox\" name=\"schluesselweg\" value=\"ja\"> remove key</small></label></td></tr>";
    for (int i = 0; i < Z_ANZAHL; i++) {
        char k[10];
        snprintf(k, sizeof k, "f%d", i);
        h += String("<tr><td>Color: ") + Z_NAME[i] + " <small>(" + Z_TEXT[i] + ")</small></td>"
             "<td><input type=\"color\" name=\"" + k + "\" value=\"" + hexFarbe(g_farbe[i]) + "\"></td></tr>";
    }
    h += "<tr><td>Color: sending right now</td><td><input type=\"color\" name=\"fsendet\" value=\"" +
         hexFarbe(g_farbeSendet) + "\"></td></tr>";
    h += "<tr><td>Color: sent successfully</td><td><input type=\"color\" name=\"ffertig\" value=\"" +
         hexFarbe(g_farbeFertig) + "\"></td></tr>";
    h += "</table><p><button class=\"btn\" type=\"submit\">Save</button></p></form>";
    h += "<p><small>Wi-Fi credentials still come from <code>/wifi.cfg</code> on the card and "
         "are mirrored into the flash.</small></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

// Setup page: choose or enter a network, target, key, password. Always reachable,
// on our own Wi-Fi it is the start page. Saves into the flash and restarts -
// after that the stick looks for the network that was entered.
static void webEinrichten()
{
    if (!webAuth()) return;
    if (g_web.method() == HTTP_POST) {
        if (!herkunftPruefen()) return;
        String ssid = g_web.arg("ssid"), pass = g_web.arg("pass");
        if (ssid.isEmpty()) ssid = g_web.arg("ssid_liste");
        if (ssid.length()) {
            int vorhanden = -1;
            for (int i = 0; i < cfgNetze; i++) if (cfgNetzSsid[i] == ssid) vorhanden = i;
            if (vorhanden >= 0) cfgNetzPass[vorhanden] = pass;
            else {
                if (cfgNetze >= MAX_NETZE) cfgNetze = MAX_NETZE - 1;          // oldest one drops out
                cfgNetzSsid[cfgNetze] = ssid; cfgNetzPass[cfgNetze] = pass; cfgNetze++;
            }
        }
        if (g_web.hasArg("endpoint") && g_web.arg("endpoint").length()) cfgEndpoint = g_web.arg("endpoint");
        if (g_web.hasArg("schluessel") && g_web.arg("schluessel").length()) cfgSchluessel = g_web.arg("schluessel");
        if (g_web.hasArg("webpass") && g_web.arg("webpass").length()) cfgWebPass = g_web.arg("webpass");
        cfgNachNvs();
        logZeile("[einrichtung] saved: network " + ssid + ", target " + cfgEndpoint + " - restart");
        g_web.send(200, "text/html; charset=utf-8", htmlKopf("Saved") +
                   "<p class=\"ok\">The stick restarts and connects to <b>" + ssid + "</b>. "
                   "Its own Wi-Fi disappears while doing so. After that it is reachable on the home network at <b>http://" + geraeteName() +
                   ".local/</b>; the address is shown on the display as well.</p>" + htmlFuss());
        delay(1500);
        sanftNeustarten();
        return;
    }
    // Offer networks nearby - on our own Wi-Fi you cannot type what you cannot see
    String liste;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 15; i++) {
        String ss = WiFi.SSID(i);
        if (ss.isEmpty()) continue;
        liste += "<option value=\"" + ss + "\">" + ss + " (" + WiFi.RSSI(i) + " dBm)</option>";
    }
    String h = htmlKopf("Set up the Scan-Stick");
    h += "<p>Choose or enter a network, give the target, save. The stick then restarts.</p>";
    h += "<form method=\"post\" action=\"/einrichten\"><table>";
    h += "<tr><td>Networks found</td><td><select name=\"ssid_liste\"><option value=\"\">- please choose -</option>" + liste + "</select></td></tr>";
    h += "<tr><td>or network name by hand</td><td><input name=\"ssid\" size=\"24\"></td></tr>";
    h += "<tr><td>Wi-Fi password</td><td><input name=\"pass\" type=\"password\" size=\"24\"></td></tr>";
    h += "<tr><td>Upload target<br><small>address of the receiver</small></td><td><input name=\"endpoint\" size=\"34\" value=\"" +
         (cfgEndpoint.length() ? cfgEndpoint : String("http://192.168.1.50:8080/scan")) + "\"></td></tr>";
    h += String("<tr><td>Device key<br><small>optional, same as SCAN_KEY on the receiver</small></td><td><input name=\"schluessel\" type=\"password\" size=\"24\"") +
         (cfgSchluessel.length() ? " placeholder=\"set - empty = keep\"" : "") + "></td></tr>";
    h += String("<tr><td>Password of the web interface<br><small>user scan</small></td><td><input name=\"webpass\" type=\"password\" size=\"24\"") +
         (cfgWebPass.length() ? " placeholder=\"set - empty = keep\"" : "") + "></td></tr>";
    h += "</table><p><button class=\"btn\" type=\"submit\">Save and restart</button></p></form>";
    if (cfgNetze) {
        h += "<p><small>Known: ";
        for (int i = 0; i < cfgNetze; i++) h += (i ? ", " : "") + cfgNetzSsid[i];
        h += "</small></p>";
    }
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webStarten()
{
    g_web.on("/", webStatus);
    g_web.on("/log", webLog);
    g_web.on("/dateien", webDateien);
    g_web.on("/roh", webRoh);
    g_web.on("/einstellungen", HTTP_GET, webEinstellungen);
    g_web.on("/einstellungen", HTTP_POST, webEinstellungen);
    g_web.on("/holen", webHolen);
    // Side effects only via POST: a GET can be slipped in by any foreign page as
    // an image address, a POST cannot without the Origin header.
    g_web.on("/jetzt-schauen", HTTP_POST, webJetztSchauen);
    g_web.on("/neustart", HTTP_POST, webNeustart);
    g_web.on("/formatieren", HTTP_POST, []() {
        if (!webAuth() || !herkunftPruefen()) return;
        if (g_dirty || rohOffen()) { g_web.send(409, "text/plain", "There is still something unsent on the card"); return; }
        if (!karteUebernehmen()) { g_web.send(503, "text/plain", "The host will not let go of the card"); return; }
        bool ok = karteFormatieren("on button press");
        remount();
        karteZurueckgeben();
        logFlushNetz();
        g_web.sendHeader("Location", "/");
        g_web.send(303, "text/plain", ok ? "formatted" : "failed");
    });
    g_web.on("/aufraeumen", HTTP_POST, []() {
        if (!webAuth() || !herkunftPruefen()) return;
        g_aufraeumJetzt = true;
        logZeile("[web] cleanup requested");
        g_web.sendHeader("Location", "/log");
        g_web.send(303, "text/plain; charset=utf-8", "");
    });
    g_web.on("/update", HTTP_GET, webUpdateSeite);
    g_web.on("/update", HTTP_POST,
        []() {
            // The finish used to run without a password check and restarted the
            // stick even when no image had been written at all. A "curl -X POST
            // /update" from anybody on the Wi-Fi was therefore enough to shoot
            // down a running upload.
            if (!webAuth() || !herkunftPruefen()) return;
            bool ok = g_updateBegonnen && Update.isFinished() && !Update.hasError();
            g_updateBegonnen = false;
            g_web.send(ok ? 200 : 400, "text/html; charset=utf-8",
                       htmlKopf(ok ? "Update installed" : "Update failed") +
                       (ok ? "<p class=\"ok\">The stick is restarting now. This page will be back in "
                             "about 10 seconds.</p>"
                           : "<p class=\"bad\">No complete image was written, "
                             "the previous firmware keeps running.</p>") + htmlFuss());
            delay(600);
            if (ok) sanftNeustarten();
        },
        []() {
            // Only check here, do not answer - the answer is given by the finish.
            // A password is set and missing: do not even accept the image.
            if (cfgWebPass.length() && !g_web.authenticate("scan", cfgWebPass.c_str())) return;
            if (!herkunftOk()) return;
            HTTPUpload &up = g_web.upload();
            if (up.status == UPLOAD_FILE_START) {
                logZeile("[update] start: " + up.filename);
                g_updateBegonnen = Update.begin(UPDATE_SIZE_UNKNOWN);
                if (!g_updateBegonnen) logZeile("[update] begin failed");
            } else if (up.status == UPLOAD_FILE_WRITE) {
                if (Update.write(up.buf, up.currentSize) != up.currentSize)
                    logZeile("[update] write error");
            } else if (up.status == UPLOAD_FILE_END) {
                if (Update.end(true)) logZeile(String("[update] done, ") + up.totalSize + " bytes");
                else logZeile("[update] finish failed");
            }
        });
    g_web.on("/einrichten", HTTP_GET, webEinrichten);
    g_web.on("/einrichten", HTTP_POST, webEinrichten);
    // On our own Wi-Fi every foreign address (hotspot detection of iOS, Android,
    // Windows) lands on the setup page; otherwise on the status.
    g_web.onNotFound([]() {
        g_web.sendHeader("Location", g_einrichtung ? "http://" + WiFi.softAPIP().toString() + "/einrichten" : String("/"));
        g_web.send(302, "text/plain", "");
    });
    // The web server keeps only requested headers; Host it always remembers.
    const char *kopf[] = { "Origin", "Referer" };
    g_web.collectHeaders(kopf, 2);
    g_web.begin();
    if (MDNS.begin(geraeteName().c_str())) MDNS.addService("http", "tcp", 80);
    g_webAn = true;
    logZeile(String("[web] reachable at http://") + WiFi.localIP().toString() +
             "/ and http://" + geraeteName() + ".local/");
}

// ---- after the end of a scan: upload new files ----
// Walk through folders recursively, upload files; logs every entry (diagnostics)
static String g_fund[MAX_FUND];
static int    g_fundAnzahl = 0;
// Ghost entries (see sendeGefundene) that are removed raw after the remount
#define MAX_GEISTER 8
static String   g_geisterName[MAX_GEISTER];
static uint32_t g_geisterGroesse[MAX_GEISTER];
static int      g_geisterAnzahl = 0;

// STEP 1: only search. Pure reading - that may happen without danger
// while the printer still has the medium.
static void sammleDateien(const String &pfad, int tiefe)
{
    File dir = SD_MMC.open(pfad.length() ? pfad : "/");
    if (!dir || !dir.isDirectory()) return;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        String voll = e.path();
        bool istDir = e.isDirectory();
        uint32_t sz = e.size();
        String basis = voll;
        int sl = basis.lastIndexOf('/');
        if (sl >= 0) basis = basis.substring(sl + 1);
        logZeile(String("[fund] ") + (istDir ? "DIR " : "FIL ") + voll + " " + sz + "B");
        if (istDir) {
            if (tiefe < 3 && !basis.startsWith(".") && !basis.equalsIgnoreCase("gesendet") &&
                !basis.equalsIgnoreCase("senden"))
                sammleDateien(voll, tiefe + 1);
            continue;
        }
        if (!istScanName(basis)) continue;   // the same yardstick as in the raw reader
        if (!sz) continue;
        if (g_fundAnzahl < MAX_FUND) g_fund[g_fundAnzahl++] = voll;
    }
}

// =============== Flow since v26: the medium stays with the printer ===============
// In operation the medium is NEVER ejected. The stick reads finished scans raw -
// directory, cluster chain, data blocks - and uploads them while the
// printer still has the card. What has been sent it remembers in flash
// (start cluster, size, checksum). Renaming is unnecessary: the printer
// names the next scan [Untitled]_<time>.pdf by itself. Cleanup happens
// only in an idle phase - when the printer has not touched anything for a while,
// the memo list fills up or somebody presses the button. Only then is the
// medium gone for less than a second, and nobody is standing at the device.
// v25 had the window on every scan; v18-v24 even for the duration of the upload.

// ---- Leftovers from v25: /senden ----
static String g_erledigtPfad[MAX_FUND];   // in /senden, successfully uploaded
static int    g_erledigtAnzahl = 0;

static bool sendenLeer()
{
    File dir = SD_MMC.open("/senden");
    if (!dir) return true;
    bool leer = true;
    for (File e = dir.openNextFile(); e && leer; e = dir.openNextFile())
        if (!e.isDirectory() && istScanName(String(e.name()))) leer = false;
    return leer;
}

static bool remount()
{
    SD_MMC.end();
    delay(80);
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    return SD_MMC.begin();
}

static void sendenAusSenden(int &hoch, int &fehler)
{
    File dir = SD_MMC.open("/senden");
    if (!dir) return;
    String namen[MAX_FUND];
    int n = 0;
    for (File e = dir.openNextFile(); e && n < MAX_FUND; e = dir.openNextFile())
        if (!e.isDirectory() && istScanName(String(e.name())) && e.size()) namen[n++] = String(e.name());
    dir.close();
    for (int i = 0; i < n; i++) {
        String pfad = "/senden/" + namen[i];
        bool schonErledigt = false;
        for (int k = 0; k < g_erledigtAnzahl; k++) if (g_erledigtPfad[k] == pfad) schonErledigt = true;
        if (schonErledigt) continue;
        uint32_t groesse = 0;
        { File pf = SD_MMC.open(pfad); if (pf) { groesse = pf.size(); pf.close(); } }
        uint32_t kennzahl = dateiKennzahl(pfad, groesse);
        char idText[32];
        snprintf(idText, sizeof idText, "%08x-%08x", (unsigned)groesse, (unsigned)kennzahl);
        int erg = ladeHoch(SD_MMC, pfad, namen[i], String(idText));
        if (erg == UP_OK || erg == UP_DUPLIKAT) {
            if (g_erledigtAnzahl < MAX_FUND) g_erledigtPfad[g_erledigtAnzahl++] = pfad;
            hoch++;
        } else fehler++;
    }
}

// ---- Processing: read raw and upload, the medium stays with the printer ----
static void verarbeiteRoh(bool manuell)
{
    uint32_t begonnen = millis();
    g_warteAnzeige = false;
    logZeile(manuell ? "[scan] manual search" : "[scan] scan finished, processing");
    zeigeScreen(Z_SUCHT);

    RohEintrag liste[MAX_FUND];
    int n = rohListe(0, liste, MAX_FUND, true);
    int hoch = 0, fehler = 0, bekannt = 0, geister = 0;
    bool wifiGeprueft = false, wifi = false;
    uint8_t *fatBuf = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);

    for (int i = 0; i < n && fatBuf; i++) {
        RohEintrag &e = liste[i];
        if (fertigIndexE(e) >= 0) { bekannt++; continue; }
        if (istGeloeschtE(e)) {
            logZeile(String("[geist] ") + e.name + " is the return of a cleaned-up file - waits for cleanup");
            geister++;
            continue;
        }
        if (e.start < 2 || fatNaechster(e.start, fatBuf) == 0) {
            logZeile(String("[geist] ") + e.name + " points to free blocks - waits for cleanup");
            geister++;
            continue;
        }
        bool geteilt = false;
        for (int k = 0; k < n; k++) if (k != i && liste[k].start == e.start) geteilt = true;
        if (geteilt) {
            logZeile(String("[geist] ") + e.name + " shares blocks with another entry - waits for cleanup");
            geister++;
            continue;
        }
        if (!rohVollstaendig(e)) {
            logZeile(String("[warte] ") + e.name + " (" + (e.groesse / 1024) + " kB) has no end marker yet - the printer is still writing");
            fehler++;
            continue;
        }
        if (!wifiGeprueft) { wifi = wifiVerbinden(); wifiGeprueft = true; logFlushNetz(); }
        uint32_t kennzahl = rohKennzahl(e);
        String sendeName = neuerName(String(e.name));
        char idText[32];
        snprintf(idText, sizeof idText, "%08x-%08x", (unsigned)e.groesse, (unsigned)kennzahl);
        logZeile(String("[roh] ") + e.name + " (" + (e.groesse / 1024) + " kB) -> " + sendeName);
        RohLeser rl;
        int erg = UP_FEHLER;
        if (wifi && rl.beginnen(e.start, e.groesse))
            erg = ladeHochQuelle(sendeName, String(idText), e.groesse, [&](uint8_t *b, size_t k) { return rl.lesen(b, k); });
        rl.ende();
        if (erg == UP_OK || erg == UP_DUPLIKAT) { fertigMerken(e, kennzahl, sendeName); hoch++; }
        else fehler++;
    }
    if (fatBuf) free(fatBuf);

    // Leftovers from v25 in /senden - the mount from startup knows them
    if (!sendenLeer()) sendenAusSenden(hoch, fehler);

    logZeile(String("[scan] done: ") + hoch + " uploaded, " + bekannt + " already known, " + fehler +
             " errors, written by host: " + (g_bytesGeschrieben / 1024) + " kB");
    if (hoch == 0) {
        // The printer wrote, but no new file is there - what exactly
        // did it write? (sector+count, F=FAT W=root D=data B=boot)
        String spur = spurText(false);
        if (spur.length()) logZeile("[spur] " + spur);
    }
    if (hoch > 0) g_bytesGeschrieben = 0;

    if ((int32_t)(g_lastWrite - begonnen) > 0) {
        logZeile("[scan] new write access during processing - staying on it");
        g_versuche = 0;
        g_naechsterVersuch = 0;
        logFlushNetz();
        return;                       // g_dirty stays set
    }

    g_versuche++;
    if (hoch == 0 && fehler == 0 && geister > 0) {
        // Only ghosts: nothing more will come, the cleanup window handles them.
        // A retry cycle would just write the same line every 30 s.
        logZeile(String("[scan] only ") + geister + " ghost(s) - waits for cleanup");
        g_versuche = MAX_VERSUCHE;
    }
    if (hoch == 0 && fehler == 0) {
        if (manuell) {
            logZeile("[scan] nothing new");
            g_versuche = 0;
            logFlushNetz();
            return;
        }
        if (g_versuche < MAX_VERSUCHE) {
            g_naechsterVersuch = millis() + RETRY_MS;
            g_dirty = true;
            logZeile(String("[warte] nothing new (attempt ") + g_versuche + "/" + MAX_VERSUCHE +
                  "), next look in " + (RETRY_MS / 1000) + "s");
            zeigeWarte(g_versuche, MAX_VERSUCHE);
            logFlushNetz();
            return;
        }
        logZeile(String("[warte] nothing new after ") + g_versuche + " attempts - gave up");
    }
    g_dirty = false;
    g_versuche = 0;
    g_naechsterVersuch = 0;
    g_rohStabil = 0;
    g_rohAnzahl = 0;
    g_rohSumme = 0;
    logFlushNetz();
}

// ---- Cleanup: the only window in which the card is modified ----
static bool aufraeumenNoetig()
{
    return g_fertigAnzahl > 0 || g_erledigtAnzahl > 0 || g_geisterAnzahl > 0;
}

static void aufraeumFenster(const String &grund)
{
    uint32_t t0 = millis();
    logZeile("[aufraeumen] " + grund);
    zeigeScreen(Z_SUCHT);
    if (!karteUebernehmen()) { logZeile("[aufraeumen] did not get the card, later"); return; }

    // raw, before the mount
    int geister = geisterJagen();
    uint32_t cs = rohOrdnerCluster("SENDEN     ");
    for (int i = 0; i < g_geisterAnzahl; i++) {
        bool ok = cs && rohEintragLoeschenIn(cs, g_geisterName[i], g_geisterGroesse[i], 0);
        logZeile(String("[geist] /senden/") + g_geisterName[i] + (ok ? " removed" : " NOT found"));
    }
    g_geisterAnzahl = 0;

    // Self-check: fix now whatever damage the printer left behind -
    // here it cannot see the card. If the file system is unreadable for the
    // second time in a row, there is nothing left to save: create it anew.
    kartePruefen(true, "in the cleanup window");
    if (g_befund.heillos && g_heillosFolge >= 2) {
        karteFormatieren("file system unreadable twice in a row");
        remount();
        karteZurueckgeben();
        g_letztesAufraeumen = millis();
        logFlushNetz();
        return;
    }

    RohEintrag liste[MAX_FUND];
    int n = rohListe(0, liste, MAX_FUND, true);

    if (!remount()) { logZeile("[aufraeumen] mount failed"); karteZurueckgeben(); return; }

    int weg = 0;
    // Memo list entries whose file is no longer there: the printer has
    // overwritten the file ("Replace") or deleted it. They have to go - otherwise
    // aufraeumenNoetig() stays true forever and the window ran in circles, every
    // 300 ms with the medium gone and back (on 20.09.2026 for over half an hour,
    // the 780 then aborted the next scan with "Error writing multi-page image file").
    for (int fi = g_fertigAnzahl - 1; fi >= 0; fi--) {
        bool da = false;
        for (int i = 0; i < n && !da; i++)
            da = liste[i].start == g_fertig[fi].start && liste[i].groesse == g_fertig[fi].groesse &&
                 (!g_fertig[fi].kennzahl || rohKennzahl(liste[i]) == g_fertig[fi].kennzahl);
        if (da) continue;
        logZeile(String("[aufraeumen] memo list: ") + g_fertig[fi].name + " (" + (g_fertig[fi].groesse / 1024) +
                 " kB) is no longer on the card - entry removed");
        geloeschtMerken(g_fertig[fi]);
        memmove(g_fertig + fi, g_fertig + fi + 1, (g_fertigAnzahl - fi - 1) * sizeof(Fertig));
        g_fertigAnzahl--;
    }
    for (int i = 0; i < n; i++) {
        int fi = fertigIndexE(liste[i]);
        if (fi < 0) {
            // Not sent and no end marker: an aborted scan (power cut,
            // write error of the printer). Otherwise the printer asks on the next
            // job "File already exists" - and after the idle period it surely
            // will not write there any more.
            if (!istGeloeschtE(liste[i]) && !rohVollstaendig(liste[i])) {
                String pfad = "/" + String(liste[i].name);
                bool ok = SD_MMC.remove(pfad);
                logZeile(String("[aufraeumen] unfinished file ") + liste[i].name + " (" + (liste[i].groesse / 1024) +
                         " kB, no end marker) " + (ok ? "discarded" : "could not be deleted"));
                if (ok) weg++;
            }
            continue;
        }
        String pfad = "/" + String(liste[i].name);
        bool ok;
        if (g_loeschen) {
            ok = SD_MMC.remove(pfad);
        } else {
            if (!SD_MMC.exists("/gesendet")) SD_MMC.mkdir("/gesendet");
            String basis = g_fertig[fi].name[0] ? String(g_fertig[fi].name) : String(liste[i].name);
            String ziel = "/gesendet/" + basis;
            for (int k = 1; SD_MMC.exists(ziel) && k < 100; k++) {
                int punkt = basis.lastIndexOf('.');
                ziel = "/gesendet/" + (punkt > 0 ? basis.substring(0, punkt) : basis) + "_" + k +
                       (punkt > 0 ? basis.substring(punkt) : String(""));
            }
            ok = SD_MMC.rename(pfad, ziel);
            if (!ok) { logZeile("[aufraeumen] move failed, deleting " + pfad); ok = SD_MMC.remove(pfad); }
        }
        if (ok) {
            geloeschtMerken(g_fertig[fi]);
            memmove(g_fertig + fi, g_fertig + fi + 1, (g_fertigAnzahl - fi - 1) * sizeof(Fertig));
            g_fertigAnzahl--;
            weg++;
        } else {
            logZeile("[aufraeumen] " + pfad + " could not be cleaned up");
        }
    }
    // Leftovers from v25
    for (int i = 0; i < g_erledigtAnzahl; i++) {
        if (g_loeschen) SD_MMC.remove(g_erledigtPfad[i]);
        else {
            String basis = g_erledigtPfad[i].substring(g_erledigtPfad[i].lastIndexOf('/') + 1);
            if (!SD_MMC.exists("/gesendet")) SD_MMC.mkdir("/gesendet");
            if (!SD_MMC.rename(g_erledigtPfad[i], "/gesendet/" + basis)) SD_MMC.remove(g_erledigtPfad[i]);
        }
        weg++;
    }
    g_erledigtAnzahl = 0;

    remount();
    karteZurueckgeben();
    fertigSpeichern();
    g_letztesAufraeumen = millis();
    logZeile(String("[aufraeumen] ") + (millis() - t0) + " ms: " + weg + " cleaned up, " + geister +
             " ghosts, " + g_fertigAnzahl + " sent still on the card");
    logFlushNetz();
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Scan-Stick " FW_VERSION " ===");
    g_startGrund = resetGrund();
    logZeile(String("[boot] " FW_VERSION ", reason: ") + g_startGrund + ", device " + geraeteName());

    ledInit();
    ledColor(60, 60, 60);   // WHITE = power there, booting
    displayInit();
    zeigeScreen(0);         // POWER + flash

    // Credentials from flash first. That way WiFi and the web UI come up
    // even when the card is missing or does not mount - then the stick can
    // report what it is missing instead of staying mute.
    cfgAusNvs();
    fertigLaden();

    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (!g_sdRiegel) g_sdRiegel = xSemaphoreCreateMutex();
    bool sdOk = SD_MMC.begin("/sdcard", false, false);
    if (!sdOk) { logZeile("[sd] mount failed"); zeigeScreen(Z_SDFEHL); }
    else fixMbrTyp();   // touch the MBR only with the card mounted

    if (sdOk && ladeConfig()) {          // only a CHANGED /wifi.cfg wins
        cfgNachNvs();
        logZeile("[nvs] taken from /wifi.cfg");
    }
    g_nvs.begin("scanstick", false);
    if (g_nvs.getUInt("usbFehler", 0)) {
        g_usbFehlerAlt = String(g_nvs.getUInt("usbFehler", 0)) + " rejected host access(es), last sector " +
                         g_nvs.getUInt("usbFehlerLba", 0) + " at " + g_nvs.getString("usbFehlerWann", "?");
        String spur = g_nvs.getString("usbFehlerSpur", "");
        logZeile("[usb] BEFORE THE LAST RESTART: " + g_usbFehlerAlt);
        if (spur.length()) logZeile("[usb] write trace before that: " + spur);
        g_nvs.remove("usbFehler"); g_nvs.remove("usbFehlerLba"); g_nvs.remove("usbFehlerWann"); g_nvs.remove("usbFehlerSpur");
    }
    g_nvs.end();
    if (sdOk) {                          // heal the card while nobody sees it yet
        kartePruefen(true, "at startup");
        if (g_befund.geaendert) sdOk = remount();
    }

    MSC.vendorID("DIY");
    MSC.productID("ScanStick");
    MSC.productRevision("1.0");
    MSC.onStartStop(onStartStop);
    MSC.onRead(onRead);
    MSC.onWrite(onWrite);
    // Without a card report "no medium" instead of a drive with 0 sectors. That
    // way the host sees an empty card reader, and from outside the state can be
    // told apart from the earlier boot hang.
    MSC.mediaPresent(sdOk);
    MSC.isWritable(true);
    uint32_t sec = SD_MMC.sectorSize();
    uint32_t rawSectors = sec ? (uint32_t)(SD_MMC.cardSize() / sec) : 0;
    Serial.printf("[usb] MSC sectors(raw)=%u sec=%u\n", rawSectors, sec);
    MSC.begin(rawSectors, sec ? sec : 512);
    USB.begin();
    Serial.println("[usb] started as USB storage");

    // WiFi only now: the scan blocks for a few seconds, and the printer
    // should see the stick as a drive right away, not only after the WiFi.
    wlanStarten();
    if (WiFi.status() != WL_CONNECTED) {
        einrichtungStarten(cfgNetze ? "no known network in range" : "no network configured");
        webStarten();
    }
    if (g_screen != Z_SDFEHL) zeigeScreen(Z_BEREIT);
}

void loop()
{
    // Without a clock the files are named "scan-after173s.pdf" instead of with a
    // date. A single failed attempt at startup must not settle that forever.
    if (!g_zeitOk && WiFi.status() == WL_CONNECTED && !g_dirty &&
        millis() - g_zeitVersuch > ZEIT_WIEDERHOLUNG) {
        zeitHolen();
    }

    // A power failure or restart in the middle of the flow must not leave a
    // file behind: the stick remembers write accesses only in RAM,
    // so after a restart it knows nothing about what is already there.
    if (!g_startGeprueft && WiFi.status() == WL_CONNECTED && millis() > 8000) {
        g_startGeprueft = true;
        int a = 0;
        uint32_t gr = 0;
        (void)a; (void)gr;
        if (rohOffen() || !sendenLeer()) {
            logZeile("[start] something unsent is still on the card - taking it along");
            g_dirty = true;
            g_lastWrite = millis() - IDLE_MS - 1;   // due immediately
            g_versuche = 0;
            g_naechsterVersuch = 0;
        }
    }

    // Is something left that did not go out - receiver unreachable, WiFi
    // missing? So far the next attempt only came with the next scan.
    // Now look raw every ten minutes, without disturbing the printer.
    #define NACHSCHAU_MS 600000
    static uint32_t nachschau = 0;
    if (!g_dirty && g_startGeprueft && WiFi.status() == WL_CONNECTED &&
        millis() - nachschau > NACHSCHAU_MS) {
        nachschau = millis();
        int a = 0;
        uint32_t gr = 0;
        (void)a; (void)gr;
        if (rohOffen() || !sendenLeer()) {
            logZeile("[nachschau] something unsent is still on the card - new attempt");
            g_dirty = true;
            g_lastWrite = millis() - IDLE_MS - 1;   // due immediately
            g_versuche = 0;
            g_naechsterVersuch = 0;
        }
    }

    // Cleanup only when idle: the printer has not touched anything for long, the
    // memo list is filling up, or somebody has pressed the button.
    if (g_aufraeumJetzt && g_startGeprueft && millis() - g_lastHost > 5000) {
        // Button press: the human knows that nothing is being scanned right now
        g_aufraeumJetzt = false;
        aufraeumFenster("on button press");
        g_dirty = false;
        g_versuche = 0;
        g_naechsterVersuch = 0;
    } else if (!g_dirty && g_startGeprueft && aufraeumenNoetig()) {
        uint32_t ruhe = millis() - g_lastHost;
        // One window per idle phase: if something stays behind afterwards that
        // cannot be cleaned up, the window must not open again right away -
        // only once the printer has accessed the card since then.
        bool neuSeitdem = g_letztesAufraeumen == 0 || (int32_t)(g_lastHost - g_letztesAufraeumen) > 0;
        if (neuSeitdem && ruhe > g_aufraeumMin * 60000UL) aufraeumFenster(String("printer ") + dauer(ruhe) + " without access");
        else if (neuSeitdem && g_fertigAnzahl >= MAX_FERTIG - 8 && ruhe > 60000) aufraeumFenster("memo list almost full");
    }

    if (g_stopNeu) {
        g_stopNeu = false;
        logZeile(String("[scsi] START STOP UNIT: start=") + (g_stopStart ? "yes" : "no") +
                 " eject=" + (g_stopEject ? "yes" : "no") + " pc=" + g_stopPc);
    }
    // Network gone? The auto reconnect clings to the fixed id of the one
    // access point. After two minutes without network search again, all networks.
    if (WiFi.status() != WL_CONNECTED && cfgNetze) {
        if (!g_wlanVerloren) g_wlanVerloren = millis() ? millis() : 1;
        else if (millis() - g_wlanVerloren > WLAN_NEUSUCHE) {
            logZeile("[wifi] two minutes without network - searching again");
            // Drop the old connection first: as long as the driver still clings
            // to the vanished access point and wants to reconnect, the scan
            // does not happen at all (in the test: "no network" after 3 ms,
            // only the second attempt two minutes later found the home network).
            WiFi.disconnect(false, false);
            delay(200);
            g_wlanVerloren = 0;
            wlanStarten();
            if (WiFi.status() != WL_CONNECTED) {
                // Failure: do not wait two minutes again, retry in 30 s
                uint32_t jetzt = millis() ? millis() : 1;
                g_wlanVerloren = jetzt - (WLAN_NEUSUCHE - 30000);
            }
        }
    }
    if (WiFi.status() == WL_CONNECTED && g_einrichtung) einrichtungBeenden();
    if (WiFi.status() == WL_CONNECTED && !g_webAn) { webStarten(); zeitHolen(); }
    if (g_einrichtung) g_dns.processNextRequest();
    if (g_webAn) g_web.handleClient();

    if (g_usbFehlerNeu) {
        // Right away, not only when idle: after a media error the 780 cuts
        // the power to the port, and then everything in RAM would be lost. So
        // additionally into the flash - on the next start it gets reported.
        g_usbFehlerNeu = false;
        char wann[16] = "?";
        if (g_zeitOk) { time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm); strftime(wann, sizeof wann, "%H:%M:%S", &tm); }
        g_nvs.begin("scanstick", false);
        g_nvs.putUInt("usbFehler", g_nvs.getUInt("usbFehler", 0) + 1);
        g_nvs.putUInt("usbFehlerLba", g_usbFehlerLba);
        g_nvs.putString("usbFehlerWann", wann);
        g_nvs.putString("usbFehlerSpur", spurText(false).substring(0, 400));
        g_nvs.end();
        logZeile(String("[usb] rejected host accesses: ") + (uint32_t)g_usbFehlerLesen + " read, " +
                 (uint32_t)g_usbFehlerSchreiben + " write, last sector " + (uint32_t)g_usbFehlerLba + " at " + wann);
        logFlushNetz();
    }

    uint32_t nun = millis();

    // Look raw as long as something is open. Pure reading - the printer notices
    // nothing of it, so it may happen every second. If count and
    // size stay the same several times, the printer has closed the file.
    // The reported size alone is NOT enough: the 780 enters the final
    // size already before writing. If we trusted that, we would rename
    // in the middle of a running scan - the printer then writes back its old
    // directory view and the rename is gone. So wait for quiet on top of that.
    // Quiet means: neither writing NOR reading. A host that still reads after
    // writing (Linux while unmounting, a printer while verifying) is not
    // done - taking the medium away now only causes errors on its side.
    // ... but not faster than the wait cycle allows: if only known files are
    // in the root, this otherwise ran in circles every four seconds.
    if (g_dirty && !g_warteAnzeige && nun - g_lastHost > ROH_RUHE &&
        nun - g_rohLetzt > ROH_INTERVALL &&
        (g_naechsterVersuch == 0 || (int32_t)(nun - g_naechsterVersuch) >= 0)) {
        g_rohLetzt = nun;
        int anz = 0;
        uint32_t summe = 0;
        if (rohVerzeichnis(anz, summe)) {
            rohOffenSumme(g_zeigAnzahl, g_zeigSumme);
            g_rohGeprueft = nun;
            if (anz == g_rohAnzahl && summe == g_rohSumme) {
                if (++g_rohStabil >= ROH_STABIL) {
                    g_rohStabil = 0;
                    g_rohAnzahl = 0;
                    g_rohSumme = 0;
                    if (!rohOffen()) {
                        // The printer wrote, but there is nothing new -
                        // its directory data for instance. Before, this ran every
                        // four seconds in circles, with one log upload per round.
                        logZeile(anz ? String("[roh] ") + anz + " file(s) stable, all already sent - nothing to do"
                                     : String("[roh] no scan file - the printer only wrote directory data, nothing to do"));
                        g_dirty = false;
                        g_versuche = 0;
                        g_naechsterVersuch = 0;
                        logFlushNetz();
                        return;
                    }
                    logZeile(String("[roh] ") + anz + " file(s), " + (summe / 1024) +
                             " kB, size stable - processing now");
                    verarbeiteRoh(false);
                    return;
                }
            } else {
                g_rohStabil = 0;
                g_rohAnzahl = anz;
                g_rohSumme = summe;
            }
        }
    }

    if (g_einmalSchauen) {
        g_einmalSchauen = false;
        verarbeiteRoh(true);
    } else if (nun - g_lastWrite < IDLE_MS) {
        // The host is still writing: start the observation over
        g_versuche = 0;
        g_naechsterVersuch = 0;
        g_warteAnzeige = false;
    } else if (g_dirty && (g_naechsterVersuch == 0 || (int32_t)(nun - g_naechsterVersuch) >= 0)) {
        verarbeiteRoh(false);
    }
    // Only the WAIT display is protected. This used to read !g_dirty - that
    // froze EVERY display as soon as a write was open, so across the
    // whole idle period.
    if (g_einrichtung && !g_dirty && g_screen != Z_SDFEHL && !g_warteAnzeige) {
        zeigeEinrichtung();
    } else if (g_screen != Z_SDFEHL && !g_warteAnzeige) {
        if (g_einrichtungGezeichnet) { g_einrichtungGezeichnet = false; g_screen = -1; }
        uint32_t seitWrite = nun - g_lastWrite;
        // Not looked raw since the last write access, or there really is
        // something unsent: then "SCAN FOUND". Only the printer's
        // directory data: just "Host active".
        bool ungeprueft = (int32_t)(g_rohGeprueft - g_lastWrite) < 0;
        if (g_dirty && seitWrite < IDLE_MS && (ungeprueft || g_zeigAnzahl > 0))
            zeigeFrist((IDLE_MS - seitWrite + 999) / 1000);
        else if (nun - g_lastHost < 3000) zeigeScreen(Z_HOST);
        else zeigeScreen(Z_BEREIT);
    }
    if ((g_screen == Z_BEREIT || g_screen == Z_HOST) &&
        (g_wlanStufeLetzt < 0 || nun - g_wlanLetzt > 3000)) {
        g_wlanLetzt = nun;
        zeichneWlanBalken();
    }

    static uint32_t hb = 0;
    if (millis() - hb > 2000) { hb = millis(); ledHeartbeat(); }   // short blink every 2s
    delay(20);   // short, so the web UI answers smoothly
}
