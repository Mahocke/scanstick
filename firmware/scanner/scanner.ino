/**
 * Scan-Stick fuer LilyGo T-Dongle-S3
 * Gibt sich am Drucker als USB-Stick (FAT32 "SCANS") aus. Erkennt, wann ein Scan
 * fertig geschrieben ist (Ruhe nach dem letzten Sektor-Write), laedt jede neue
 * Datei per HTTP-POST an einen konfigurierbaren Empfaenger und meldet den Stick
 * danach neu an, damit der Drucker wieder einen leeren Stick sieht.
 *
 * Diagnose-Log geht per HTTP an denselben Empfaenger (scanlog-<millis>.txt),
 * NICHT auf die Karte - sonst zerstoert der eigene FAT-Write den Commit des Druckers.
 *
 * Zugangsdaten kommen NICHT in den Code, sondern aus /wifi.cfg auf der SD-Karte:
 *   ssid=...
 *   pass=...
 *   endpoint=http://192.168.1.50:8080/scan
 *
 * Board: ESP32S3 Dev Module, USB Mode = USB-OTG (TinyUSB), USB CDC on Boot = Enabled
 */
#include "USB.h"
#include "USBMSC.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>

#define SD_D0  14
#define SD_D1  17
#define SD_D2  21
#define SD_D3  18
#define SD_CLK 12
#define SD_CMD 16

#define IDLE_VORGABE  45000    // Ruhe bis "Scan fertig" - Vorgabe, per Weboberflaeche aenderbar
static uint32_t g_idleMs   = IDLE_VORGABE;   // Ruhefrist, aus dem Flash
static bool     g_loeschen = true;           // true = loeschen, false = nach /gesendet verschieben
#define IDLE_MS (g_idleMs)
#define RETRY_MS      30000    // nichts gefunden -> so lange warten, dann nochmal schauen
#define MAX_VERSUCHE  5        // so oft nachschauen, bevor wir aufgeben
#define WIFI_TIMEOUT  20000

// ---- APA102 Status-LED (T-Dongle-S3: Daten 40, Takt 39) ----
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
// Die APA102 hat ein eigenes Helligkeitsbyte (0..31). Volle Helligkeit ist als
// Dauerlicht neben einem Drucker schlicht zu grell - einstellbar, 0 = aus.
static uint8_t g_ledHell = 5;

static void ledRaw(uint8_t r, uint8_t g, uint8_t b) {
    ledByte(0); ledByte(0); ledByte(0); ledByte(0);   // Start-Frame
    if (!g_ledHell) { r = g = b = 0; }                 // ganz aus
    ledByte(0xE0 | (g_ledHell & 0x1F));
    ledByte(b); ledByte(g); ledByte(r);               // BGR-Reihenfolge
    ledByte(0xFF); ledByte(0xFF); ledByte(0xFF); ledByte(0xFF); // End-Frame
}
static void ledColor(uint8_t r, uint8_t g, uint8_t b) { g_ledR = r; g_ledG = g; g_ledB = b; ledRaw(r, g, b); }
static void ledHeartbeat() {
    if (!g_ledHell) return;   // aus bleibt aus, auch beim Lebenszeichen
    ledRaw(0, 0, 0); delay(40); ledRaw(g_ledR, g_ledG, g_ledB);
}
static void ledInit() { pinMode(LED_DI, OUTPUT); pinMode(LED_CI, OUTPUT); }

// ---- ST7735-Display (T-Dongle-S3: CS4 SDA3 SCL5 DC2 RST1 Backlight38) ----
#define TFT_CS 4
#define TFT_SDA 3
#define TFT_SCL 5
#define TFT_DC 2
#define TFT_RST 1
#define TFT_BL 38
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
// Zustaende des Sticks. Display-Hintergrund und Status-LED teilen sich eine
// Farbe pro Zustand - vorher wurden beide getrennt gesetzt und liefen auseinander.
#define Z_STROM   0
#define Z_BEREIT  1
#define Z_HOST    2
#define Z_SUCHT   3
#define Z_SDFEHL  4
#define Z_WARTE   5
#define Z_FRIST   6
#define Z_ANZAHL  7

static const char *Z_NAME[Z_ANZAHL] = {
    "Strom da, startet", "Bereit, wartet auf den Drucker", "Drucker greift zu",
    "Sucht neue Scans", "Karte nicht lesbar", "Wartet auf den Commit",
    "Scan erkannt, Ruhefrist laeuft"
};
static const char *Z_TEXT[Z_ANZAHL] = { "STROM", "BEREIT", "OK", "SUCHE", "SD?!", "WARTE", "SCAN" };
// Vorgaben als 0xRRGGBB, per Weboberflaeche aenderbar
static uint32_t g_farbe[Z_ANZAHL] = { 0xFFC000, 0x0044FF, 0x00C000, 0xCC00CC, 0xFF0000, 0xFFAA00, 0x00AACC };
static uint32_t g_farbeSendet = 0x00AAFF;   // waehrend der Uebertragung
static uint32_t g_farbeFertig = 0x00CC44;   // Erfolgsmeldung

static uint16_t rgb565(uint32_t rgb)
{
    uint8_t r = rgb >> 16, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static Arduino_DataBus *g_bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCL, TFT_SDA);
static Arduino_GFX *g_gfx = new Arduino_ST7735(g_bus, TFT_RST, 1 /*Rotation*/, false /*ips*/, 80, 160, 26, 1, 26, 1);
static int g_screen = -1;   // aktueller Anzeigezustand, nur bei Wechsel neu zeichnen
static bool g_invertiert = true;    // Display stellt die Farben negativ dar
static int g_wlanStufeLetzt = -1;   // zuletzt gezeichnete Empfangsstufe
static uint32_t g_wlanLetzt = 0;
static long     g_rssiMin = 0, g_rssiMax = 0, g_rssiLetztLast = 0;
static long     g_rssiSumme = 0;
static uint32_t g_rssiAnzahl = 0;

// zeichnet einen einfachen Blitz an Position (x,y)
static void malBlitz(int x, int y, uint16_t farbe) {
    g_gfx->fillTriangle(x+14, y, x+2, y+20, x+12, y+18, farbe);
    g_gfx->fillTriangle(x+12, y+16, x+22, y+14, x+8, y+36, farbe);
}

// Ein Zustand setzt Display UND LED - eine Farbe, eine Quelle.
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
    g_wlanStufeLetzt = -1;   // fillScreen hat die Balken geloescht
}

// ---- Empfangsbalken oben rechts ----
// Der Empfang schwankt am Druckerstandort erheblich (im Metallgehaeuse -93 dBm,
// aussen -61). Man soll das am Geraet sehen, ohne die Weboberflaeche zu oeffnen.
// Empfang laufend mitschreiben: heute schwankte er bei gleicher Position
// zwischen -93 und -61 dBm. Ohne Aufzeichnung ist nicht zu unterscheiden,
// ob das dauernd passiert oder nur unter Sendelast.
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
    g_gfx->fillRect(110, 1, 49, 24, hg);          // Bereich freiraeumen

    for (int i = 0; i < 5; i++) {
        int hoehe = 4 + i * 4;                     // 4, 8, 12, 16, 20 Pixel
        int x = 112 + i * 9;
        int y = 22 - hoehe;
        if (i < stufe) g_gfx->fillRect(x, y, 7, hoehe, COL_BLACK);   // voll = vorhanden
        else           g_gfx->drawRect(x, y, 7, hoehe, COL_BLACK);   // nur Umriss
    }
    if (stufe == 0) {                              // kein Netz: Kreuz ueber die Balken
        g_gfx->drawLine(112, 2, 156, 22, COL_BLACK);
        g_gfx->drawLine(112, 22, 156, 2, COL_BLACK);
    }
}

// ---- dynamische Anzeigen: gefunden / sendet / fertig ----
// Das Display ist im Betrieb die einzige Rueckmeldung am Geraet, deshalb soll
// man sehen, WAS er sendet und dass es vorangeht - nicht nur "irgendwas laeuft".

// Dateinamen auf die Displaybreite kuerzen (Textgroesse 1 = 6 px pro Zeichen)
static String kurzName(const String &name, int max_zeichen)
{
    if ((int)name.length() <= max_zeichen) return name;
    return name.substring(0, max_zeichen - 3) + "...";
}

static int g_balkenProzent = -1;

static void zeigeSendenStart(const String &name, uint32_t gesamt)
{
    g_screen = -2;               // Sonderzustand: loop() zeichnet neu, wenn es vorbei ist
    g_balkenProzent = -1;
    uint32_t rgb = g_farbeSendet;
    ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    g_gfx->fillScreen(rgb565(rgb));
    g_gfx->setTextColor(COL_BLACK);
    g_gfx->setTextSize(2);
    g_gfx->setCursor(4, 4);
    g_gfx->print("SENDET");
    g_gfx->setTextSize(1);
    g_gfx->setCursor(4, 24);
    g_gfx->print(kurzName(name, 25));
    g_gfx->setCursor(4, 64);
    g_gfx->printf("%u kB", (unsigned)(gesamt / 1024));
    g_gfx->drawRect(4, 38, 152, 18, COL_BLACK);   // Rahmen des Balkens
}

static void zeigeSendenFortschritt(uint32_t fertig, uint32_t gesamt)
{
    int proz = gesamt ? (int)((uint64_t)fertig * 100 / gesamt) : 100;
    if (proz == g_balkenProzent) return;          // nur bei Aenderung zeichnen, sonst flackert es
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
    g_gfx->printf("%u kB gesendet", (unsigned)(bytes / 1024));
}

// Ruhefrist laeuft: gross den Countdown zeigen. Vorher stand hier 45 Sekunden
// lang ein unveraendertes Bild - man konnte nicht sehen, ob er den Scan hat.
static int  g_fristLetzt   = -1;
static bool g_warteAnzeige = false;   // WARTE-Bild steht und darf nicht ueberschrieben werden

// Ergebnis des rohen Mitlesens - auch die Anzeige greift darauf zu
#define ROH_INTERVALL   1000    // so oft roh nachsehen (ms)
#define ROH_RUHE        3000    // so lange kein Schreibzugriff, bevor wir zugreifen
#define ROH_STABIL         3    // so viele gleiche Messungen = Datei fertig
static uint32_t g_rohLetzt  = 0;
static uint32_t g_rohSumme  = 0;
static int      g_rohAnzahl = 0;
static int      g_rohStabil = 0;

static void zeigeFrist(uint32_t restSek)
{
    if (g_screen != Z_FRIST) {
        g_screen = Z_FRIST;
        g_fristLetzt = -1;
        uint32_t rgb = g_farbe[Z_FRIST];
        ledColor(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
        g_gfx->fillScreen(rgb565(rgb));
        g_gfx->setTextColor(COL_BLACK);
        g_gfx->setTextSize(2);
        g_gfx->setCursor(5, 4);
        g_gfx->print("SCAN ERKANNT");
        g_gfx->setTextSize(1);
        g_gfx->setCursor(5, 68);
        g_gfx->print(g_rohAnzahl ? "pruefe ob fertig" : "warte auf Ruhe");
    }
    // Sobald wir die Datei roh sehen, ist ihre Groesse die ehrlichere Angabe
    // als ein Countdown, der ohnehin vorzeitig endet.
    int wert = g_rohAnzahl ? (int)(g_rohSumme / 1024) : (int)restSek;
    if (wert != g_fristLetzt) {
        g_fristLetzt = wert;
        g_gfx->fillRect(5, 26, 150, 36, rgb565(g_farbe[Z_FRIST]));
        g_gfx->setTextColor(COL_BLACK);
        g_gfx->setTextSize(g_rohAnzahl ? 3 : 4);
        g_gfx->setCursor(5, 30);
        if (g_rohAnzahl) g_gfx->printf("%d kB", wert);
        else             g_gfx->printf("%ds", wert);
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
    g_gfx->print("WARTE");
    g_gfx->setTextSize(1);
    g_gfx->setCursor(12, 50);
    g_gfx->printf("Versuch %d von %d", versuch, von);
    g_gfx->setCursor(12, 62);
    g_gfx->print("Drucker nicht fertig");
}

static void displayInit() {
    pinMode(TFT_BL, OUTPUT);
    g_gfx->begin();
    // Dieses Modul zeigt die Farben sonst als Negativ: eingestelltes Blau
    // erscheint gelb, Schwarz erscheint weiss. Damit waeren alle Farbwaehler
    // in den Einstellungen wirkungslos bzw. genau verkehrt herum.
    g_gfx->invertDisplay(g_invertiert);
    digitalWrite(TFT_BL, LOW);   // Backlight an (LILYGO: aktiv-low)
    g_gfx->fillScreen(COL_BLACK);
}

USBMSC MSC;

static volatile uint32_t g_lastWrite = 0;
static volatile uint32_t g_lastHost  = 0;   // wann hat der Host zuletzt gelesen/geschrieben
static volatile bool     g_dirty     = false;
static volatile uint32_t g_bytesGeschrieben = 0;   // seit dem letzten Verarbeiten

static uint32_t g_naechsterVersuch = 0;   // 0 = sofort faellig
static int      g_versuche         = 0;

#define FW_VERSION "v19"

String cfgSsid, cfgPass, cfgEndpoint;
String cfgWebPass;   // Schutz der Weboberflaeche; leer = offen
String cfgPraefix = "scan";   // Namensanfang der Dateien, z.B. "buero-774"
static bool g_zeitOk        = false;   // NTP-Zeit vorhanden?
static bool g_einmalSchauen = false;   // Knopf "Jetzt schauen": einmal, ohne Wartezyklus
static bool g_startGeprueft = false;   // nach dem Hochlaufen einmal nachsehen

static WebServer  g_web(80);
static Preferences g_nvs;
static bool       g_webAn = false;
static bool       g_updateBegonnen = false;   // Update.begin() ist tatsaechlich gelaufen

// ---- MSC: der Host greift auf die SD zu, jeder Sektor laeuft durch uns ----
static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    // Rueckgabe 0 heisst fuer TinyUSB "beschaeftigt, gleich nochmal" - bei einem
    // echten Kartenfehler haengt der Host damit endlos. Negativ = sauberer Fehler.
    uint32_t sec = SD_MMC.sectorSize();
    if (!sec) return -1;
    for (uint32_t x = 0; x < bufsize / sec; x++) {
        if (!SD_MMC.writeRAW(buffer + sec * x, lba + x)) return -1;
    }
    g_lastWrite = millis();
    g_lastHost = millis();
    g_dirty = true;
    g_bytesGeschrieben += bufsize;   // Messung: kommen ueberhaupt Scandaten an?
    return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    uint32_t sec = SD_MMC.sectorSize();
    if (!sec) return -1;
    for (uint32_t x = 0; x < bufsize / sec; x++) {
        if (!SD_MMC.readRAW((uint8_t *)buffer + x * sec, lba + x)) return -1;
    }
    g_lastHost = millis();
    return bufsize;
}

// SCSI START STOP UNIT. Geraete senden das oft am Jobende ("auswerfen",
// "Puffer rausschreiben"). Wenn der Drucker das tut, haben wir ein sofortiges
// Fertig-Signal und muessen nicht 45 s auf Stille warten.
static volatile uint32_t g_letztesStop = 0;
static volatile bool     g_stopNeu      = false;
static volatile bool     g_stopStart    = false;
static volatile bool     g_stopEject    = false;
static volatile uint8_t  g_stopPc       = 0;

static bool onStartStop(uint8_t pc, bool start, bool eject)
{
    // Bewusst nichts Aufwendiges hier: der Rueckruf laeuft im USB-Zusammenhang,
    // Zeichenketten und Dateizugriffe haben hier nichts verloren.
    g_letztesStop = millis();
    g_stopStart = start;
    g_stopEject = eject;
    g_stopPc = pc;
    g_stopNeu = true;
    return true;
}

// ---- Diagnose-Log: sammelt im RAM, geht per WLAN raus ----
// BEWUSST nicht auf die SD: solange der Drucker das Medium hat, wuerde jeder
// eigene FAT-Write seinen noch offenen Commit zerstoeren - also genau den
// Vorgang, den wir beobachten wollen. Der USB-CDC reisst beim WLAN-Start ab,
// deshalb ist das Netz der einzige verlaessliche Kanal.
static String g_logPuffer;

static void logZeile(const String &msg)
{
    Serial.println(msg);
    g_logPuffer += String(millis()) + " " + msg + "\n";
    if (g_logPuffer.length() > 8000) g_logPuffer.remove(0, 4000);   // Deckel, RAM ist knapp
}

// Puffer wegschicken. Beruehrt die SD-Karte nicht.
static void logFlushNetz()
{
    if (g_logPuffer.isEmpty() || cfgEndpoint.isEmpty()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = cfgEndpoint;
    url += (url.indexOf('?') < 0) ? "?name=" : "&name=";
    url += "scanlog-" + String(millis()) + ".txt";
    if (!http.begin(url)) return;
    http.addHeader("Content-Type", "text/plain");
    int code = http.POST(g_logPuffer);
    http.end();
    // Puffer bleibt stehen: die Weboberflaeche soll den ganzen Verlauf zeigen
    (void)code;
}

// ---- MBR-Partitionstyp auf FAT32-LBA (0x0C) setzen ----
// ESP-Format hinterlaesst manchmal 0x07 (macOS liest das als NTFS und mountet nicht)
static void fixMbrTyp()
{
    uint8_t *s = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!s) return;
    if (SD_MMC.readRAW(s, 0) && s[510] == 0x55 && s[511] == 0xAA) {
        if (s[446 + 4] == 0x07) {
            s[446 + 4] = 0x0C;
            if (SD_MMC.writeRAW(s, 0)) Serial.println("[mbr] Typ 0x07 -> 0x0C korrigiert");
        }
    }
    free(s);
}

// ================= Das Verzeichnis roh mitlesen =================
// Bisher musste der Stick die Karte kurz abhaengen und wieder anhaengen, um zu
// sehen, was der Drucker geschrieben hat - in diesen Millisekunden ist sie fuer
// den Drucker weg. Deshalb durfte er nur selten nachsehen und musste auf Stille
// warten. Liest er die Sektoren dagegen selbst, stoert er niemanden und darf
// jede Sekunde schauen.
//
// FAT32 kurz: Bootsektor (Layout) -> Zuordnungstabelle (welcher Cluster gehoert
// zu welcher Datei) -> Verzeichnis (32 Byte je Eintrag mit Name und GROESSE).
// Die endgueltige Groesse traegt der Schreiber erst beim Schliessen ein - genau
// daran erkennen wir, dass eine Datei fertig ist.

#define ROH_MIN_GROESSE 2048   // darunter: Hilfsdateien wie wifi.cfg, kein Scan

// Was gilt als Scan? EIN Massstab fuer den Rohleser (sieht nur den 8.3-Kurznamen)
// und den Sammler (sieht den langen Namen). Vorher zaehlte der Rohleser jede
// Datei ab 2 kB, der Sammler uebersprang aber alles mit fuehrendem Punkt. Eine
// "._wifi.cfg" vom Mac (4 kB, Kurzname "_WIFI~1.CFG") war roh sichtbar, im
// Sammellauf aber nicht - der Stick meldete das Medium daraufhin fuenfmal in
// Folge ab und wieder an, nach jedem Start und nach jedem Schreibzugriff.
static bool istScanEndung(const uint8_t *e)   // 3 Zeichen aus dem Kurznamen, gross
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
};
static FatLage g_fat = { false, 0, 0, 0, 0 };

static uint32_t le32(const uint8_t *d)
{
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static void fatLageLesen()
{
    g_fat.gueltig = false;
    uint8_t *s = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA);
    if (!s) return;

    uint32_t partStart = 0;
    if (SD_MMC.readRAW(s, 0) && s[510] == 0x55 && s[511] == 0xAA)
        partStart = le32(s + 446 + 8);          // Startsektor der ersten Partition
    if (!SD_MMC.readRAW(s, partStart)) { free(s); return; }

    uint16_t bytesProSektor = (uint16_t)s[11] | ((uint16_t)s[12] << 8);
    uint8_t  spc            = s[13];
    uint16_t reserviert     = (uint16_t)s[14] | ((uint16_t)s[15] << 8);
    uint8_t  anzahlFats     = s[16];
    uint32_t fatGroesse     = le32(s + 36);
    uint32_t rootCluster    = le32(s + 44);
    free(s);

    if (bytesProSektor != 512 || !spc || !fatGroesse || rootCluster < 2) return;
    g_fat.sektorenProCluster = spc;
    g_fat.fatStart           = partStart + reserviert;
    g_fat.ersterDatenSektor  = partStart + reserviert + (uint32_t)anzahlFats * fatGroesse;
    g_fat.rootCluster        = rootCluster;
    g_fat.gueltig            = true;
}

static uint32_t clusterSektor(uint32_t c)
{
    return g_fat.ersterDatenSektor + (c - 2) * g_fat.sektorenProCluster;
}

static uint32_t fatNaechster(uint32_t c, uint8_t *puffer)
{
    uint32_t versatz = c * 4;
    if (!SD_MMC.readRAW(puffer, g_fat.fatStart + versatz / 512)) return 0x0FFFFFFF;
    return le32(puffer + (versatz % 512)) & 0x0FFFFFFF;
}

// Zaehlt Dateien im Wurzelverzeichnis und summiert ihre Groessen.
// Bleiben Anzahl und Summe ueber mehrere Blicke gleich, schreibt niemand mehr.
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
            if (!SD_MMC.readRAW(sek, clusterSektor(cl) + i)) { fertig = true; break; }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *d = sek + e;
                if (d[0] == 0x00) { fertig = true; break; }   // Ende des Verzeichnisses
                if (d[0] == 0xE5) continue;                   // geloeschter Eintrag
                uint8_t attr = d[11];
                if (attr == 0x0F) continue;                   // Teil eines langen Namens
                if (attr & 0x18) continue;                    // Ordner oder Datentraegername
                if (!istScanEndung(d + 8)) continue;          // Endung im Kurznamen: Byte 8-10
                uint32_t gr = le32(d + 28);
                if (gr < ROH_MIN_GROESSE) continue;           // Hilfsdatei, kein Scan
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

// ---- Konfiguration von der SD lesen ----
static void ladeConfig()
{
    File f = SD_MMC.open("/wifi.cfg");
    if (!f) { Serial.println("[cfg] /wifi.cfg fehlt"); return; }
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq < 1) continue;
        String k = line.substring(0, eq); k.trim();
        String v = line.substring(eq + 1); v.trim();
        if (k == "ssid") cfgSsid = v;
        else if (k == "pass") cfgPass = v;
        else if (k == "endpoint") cfgEndpoint = v;
    }
    f.close();
    logZeile(String("[cfg] ssid=") + cfgSsid + " endpoint=" + cfgEndpoint);
}

// ---- Zugangsdaten zusaetzlich im NVS-Flash ----
// Sie stehen auf der SD (/wifi.cfg). Mountet die Karte nicht, gaebe es ohne
// Kopie im Flash kein WLAN und keine Weboberflaeche - also genau dann keine
// Diagnose, wenn man sie braucht. Darum spiegeln.
static void cfgAusNvs()
{
    g_nvs.begin("scanstick", true);
    cfgSsid     = g_nvs.getString("ssid", "");
    cfgPass     = g_nvs.getString("pass", "");
    cfgEndpoint = g_nvs.getString("endpoint", "");
    cfgWebPass  = g_nvs.getString("webpass", "");
    cfgPraefix  = g_nvs.getString("praefix", "scan");
    g_idleMs    = g_nvs.getUInt("idle", IDLE_VORGABE);
    g_loeschen  = g_nvs.getBool("loeschen", true);
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
    if (cfgSsid.length()) logZeile(String("[nvs] Zugangsdaten aus Flash: ssid=") + cfgSsid);
}

static void cfgNachNvs()
{
    g_nvs.begin("scanstick", false);
    g_nvs.putString("ssid", cfgSsid);
    g_nvs.putString("pass", cfgPass);
    g_nvs.putString("endpoint", cfgEndpoint);
    g_nvs.putString("webpass", cfgWebPass);
    g_nvs.putString("praefix", cfgPraefix);
    g_nvs.putUInt("idle", g_idleMs);
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

// WLAN nur anstossen, nicht auf die Verbindung warten: der Drucker soll den
// Stick sofort als Laufwerk sehen, nicht erst nach dem WLAN-Timeout.
static String  g_apKennung;      // Kennung des gewaehlten Zugangspunkts
static int     g_apKanal  = 0;
static long    g_apRssi   = 0;
static int     g_apAnzahl = 0;   // wie viele tragen dieselbe SSID

// Mehrere Zugangspunkte koennen dieselbe SSID tragen (einzelne APs, kein Mesh).
// WiFi.begin(ssid, pass) nimmt dann den erstbesten - nicht den staerksten.
// Deshalb vorher suchen und gezielt zur besten Kennung verbinden.
static void wlanStarten()
{
    if (cfgSsid.isEmpty()) { logZeile("[wifi] keine SSID bekannt"); return; }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("scanstick");

    int gefunden = WiFi.scanNetworks(false, false);
    int besterIndex = -1;
    long bestesRssi = -1000;
    g_apAnzahl = 0;
    for (int i = 0; i < gefunden; i++) {
        if (WiFi.SSID(i) != cfgSsid) continue;
        g_apAnzahl++;
        if (WiFi.RSSI(i) > bestesRssi) { bestesRssi = WiFi.RSSI(i); besterIndex = i; }
    }

    if (besterIndex >= 0) {
        g_apKennung = WiFi.BSSIDstr(besterIndex);
        g_apKanal   = WiFi.channel(besterIndex);
        g_apRssi    = bestesRssi;
        logZeile(String("[wifi] ") + g_apAnzahl + " Zugangspunkt(e) mit dieser SSID, nehme " +
                 g_apKennung + " auf Kanal " + g_apKanal + " mit " + g_apRssi + " dBm");
        WiFi.begin(cfgSsid.c_str(), cfgPass.c_str(), g_apKanal, WiFi.BSSID(besterIndex));
    } else {
        logZeile(String("[wifi] ") + cfgSsid + " nicht gefunden, versuche es trotzdem");
        g_apKennung = "";
        WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    }
    WiFi.scanDelete();
}

static bool wifiVerbinden()
{
    if (WiFi.status() == WL_CONNECTED) return true;
    if (cfgSsid.isEmpty()) { Serial.println("[wifi] keine SSID"); return false; }
    Serial.printf("[wifi] verbinde mit %s ...\n", cfgSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        logZeile(String("[wifi] verbunden, IP=") + WiFi.localIP().toString());
        return true;
    }
    logZeile("[wifi] Zeitueberschreitung");
    return false;
}

// http://host[:port]/pfad zerlegen
static bool urlTeile(const String &url, String &host, uint16_t &port, String &pfad)
{
    if (!url.startsWith("http://")) return false;   // bewusst nur http, kein TLS auf dem Stick
    String rest = url.substring(7);
    int sl = rest.indexOf('/');
    String hostteil = (sl < 0) ? rest : rest.substring(0, sl);
    pfad = (sl < 0) ? "/" : rest.substring(sl);
    int dp = hostteil.indexOf(':');
    if (dp < 0) { host = hostteil; port = 80; }
    else { host = hostteil.substring(0, dp); port = (uint16_t)hostteil.substring(dp + 1).toInt(); }
    return host.length() > 0;
}

// Upload in Bloecken. Wir schreiben HTTP selbst, weil HTTPClient die Datei in
// einem Zug schluckt und keinen Fortschritt meldet - den braucht das Display,
// und bei schwachem WLAN sieht man so ueberhaupt, ob es vorangeht oder haengt.
// ---- Uhrzeit per NTP ----
// Nur fuer Dateinamen: "Untitled_7.pdf" ist im Ablageziel wertlos,
// "scan-20260919-1432.pdf" sortiert sich von selbst.
#define ZEIT_WIEDERHOLUNG 120000   // alle 2 Minuten erneut versuchen, bis es klappt
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
        logZeile("[zeit] keine NTP-Antwort - Dateinamen bekommen die Laufzeit");
    }
}

// Eindeutiger Name mit Zeitstempel, Endung bleibt erhalten.
static String neuerName(const String &alt)
{
    String endung;
    int punkt = alt.lastIndexOf('.');
    if (punkt > 0) endung = alt.substring(punkt);
    char stempel[32];
    struct tm t;
    if (g_zeitOk && getLocalTime(&t, 200)) strftime(stempel, sizeof stempel, "%Y%m%d-%H%M%S", &t);
    else snprintf(stempel, sizeof stempel, "nach%lus", (unsigned long)(millis() / 1000));
    return cfgPraefix + "-" + stempel + endung;
}

// Fuer Links: Klammern, Leerzeichen und Umlaute muessen kodiert werden,
// sonst zeigt der Download-Link ins Leere.
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

// Ist die Datei fertig geschrieben? Bei PDF steht das in der Datei selbst:
// sie endet mit "%%EOF". Das ist ein Beweis - anders als Stille oder Dateigroesse,
// denn der 780 traegt die endgueltige Groesse schon vor dem Schreiben ein und
// reserviert den Platz. Ohne diese Pruefung haben wir eine halb geschriebene
// Datei hochgeladen, deren hinterer Teil nur aus Leerbytes bestand.
static bool dateiVollstaendig(const String &pfad, uint32_t groesse)
{
    if (!pfad.endsWith(".pdf") && !pfad.endsWith(".PDF")) return true;   // nur PDF pruefbar
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

// Kennung der Datei fuer den Empfaenger. Der Drucker stellt nach dem
// Verschieben gern seine alte Verzeichnissicht wieder her - dann zeigt
// "[Untitled].pdf" erneut auf dieselben Daten und wir laden sie ein zweites
// Mal hoch. Der Empfaenger erkennt die Wiederholung an dieser Kennung.
//
// Kurze Kennzahl aus Groesse plus erstem und letztem Block - reicht, um
// dieselbe Datei wiederzuerkennen, und kostet kaum Lesezeit. Zum LOESCHEN ohne
// Upload taugt sie bewusst nicht (siehe sendeGefundene).
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

static bool ladeHoch(fs::FS &fs, const String &pfad, const String &name, const String &id)
{
    File f = fs.open(pfad);
    if (!f) { logZeile("[up] " + pfad + " nicht oeffenbar"); return false; }
    uint32_t len = f.size();
    if (!len) { f.close(); logZeile("[up] " + name + " ist leer, uebersprungen"); return false; }

    String host, ziel;
    uint16_t port;
    if (!urlTeile(cfgEndpoint, host, port, ziel)) {
        logZeile("[up] Ziel-Adresse unbrauchbar: " + cfgEndpoint);
        f.close();
        return false;
    }
    ziel += (ziel.indexOf('?') < 0) ? "?name=" : "&name=";
    ziel += name;
    // Eindeutige Kennung derselben Datei. Damit kann der Empfaenger doppelte
    // Uebertragungen erkennen und verwerfen - etwa wenn ein Upload abbricht und
    // spaeter wiederholt wird, oder wenn der Drucker seine alte Verzeichnissicht
    // zurueckschreibt und die Datei dadurch erneut auftaucht.
    if (id.length()) { ziel += "&id="; ziel += id; }

    // Bei schwachem Funk scheitert der erste Verbindungsaufbau gern mal.
    // Einmal aufgeben hiess bisher: Datei bleibt liegen, naechster Anlauf erst
    // beim naechsten Scan.
    WiFiClient c;
    bool verbunden = false;
    for (int v = 1; v <= 3 && !verbunden; v++) {
        verbunden = c.connect(host.c_str(), port);
        if (!verbunden) {
            logZeile(String("[up] keine Verbindung zu ") + host + ":" + port +
                     " (Versuch " + v + " von 3)");
            delay(1500);
        }
    }
    if (!verbunden) { f.close(); return false; }
    c.print(String("POST ") + ziel + " HTTP/1.1\r\n" +
            "Host: " + host + ":" + port + "\r\n" +
            "Content-Type: application/octet-stream\r\n" +
            "Content-Length: " + len + "\r\n" +
            "Connection: close\r\n\r\n");

    zeigeSendenStart(name, len);
    uint8_t puffer[1024];
    uint32_t geschickt = 0, t0 = millis();
    while (geschickt < len) {
        int gelesen = f.read(puffer, sizeof puffer);
        if (gelesen <= 0) { logZeile("[up] Karte liefert keine Daten mehr"); break; }
        int raus = c.write(puffer, gelesen);
        if (raus != gelesen) { logZeile("[up] Verbindung brach beim Senden ab"); break; }
        geschickt += raus;
        zeigeSendenFortschritt(geschickt, len);
        if (millis() - t0 > 180000) { logZeile("[up] Zeitueberschreitung beim Senden"); break; }
    }
    f.close();

    if (geschickt != len) { c.stop(); return false; }

    int code = 0;
    uint32_t tw = millis();
    while (c.connected() && !c.available() && millis() - tw < 15000) delay(10);
    if (c.available()) {
        String zeile = c.readStringUntil('\n');          // "HTTP/1.1 200 OK"
        int sp = zeile.indexOf(' ');
        if (sp > 0) code = zeile.substring(sp + 1, sp + 4).toInt();
    }
    c.stop();
    if (WiFi.status() == WL_CONNECTED) { g_rssiLetztLast = WiFi.RSSI(); rssiErfassen(g_rssiLetztLast); }
    logZeile(String("[up] ") + name + " " + (geschickt / 1024) + " kB HTTP " + code +
             " in " + ((millis() - t0) / 1000) + " s");
    if (code >= 200 && code < 300) { zeigeFertig(geschickt); delay(1200); return true; }
    return false;
}

// ================= Weboberflaeche =================
// Der Stick steckt im Drucker: kein Serial (der CDC gehoert im MSC-Betrieb dem
// TinyUSB-Stack), Display nur fuer den, der davorsteht. Die Webseite ist der
// einzige Kanal, der von ueberall offen ist.

static String menschlich(uint64_t b)
{
    char t[32];
    if (b >= 1024ULL * 1024 * 1024) snprintf(t, sizeof t, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)      snprintf(t, sizeof t, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)             snprintf(t, sizeof t, "%.1f kB", b / 1024.0);
    else                            snprintf(t, sizeof t, "%llu B", b);
    return String(t);
}

static String dauer(uint32_t ms)
{
    char t[32];
    uint32_t sek = ms / 1000;
    if (sek < 90) snprintf(t, sizeof t, "%u s", sek);
    else if (sek < 5400) snprintf(t, sizeof t, "%u min %u s", sek / 60, sek % 60);
    else snprintf(t, sizeof t, "%u h %u min", sek / 3600, (sek % 3600) / 60);
    return String(t);
}

// Die Seite gibt gescannte Post zum Herunterladen frei. Ohne Passwort kann
// jedes Geraet im WLAN mitlesen - deshalb Basic-Auth, sobald eines gesetzt ist.
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
    return String("<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
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
        "<nav><a href=\"/\">Status</a><a href=\"/log\">Protokoll</a>"
        "<a href=\"/dateien\">Dateien</a><a href=\"/roh\">Roh</a>"
        "<a href=\"/einstellungen\">Einstellungen</a>"
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
    h += zl("Karte", sdOk ? "<span class=\"ok\">gemountet</span>"
                          : "<span class=\"bad\">NICHT gemountet</span>");
    if (sdOk) {
        uint64_t roh = SD_MMC.cardSize();
        h += zl("Kartengroesse (roh)", menschlich(roh) + " / " + String((uint32_t)(roh / sec)) + " Sektoren");
        h += zl("Dateisystem", menschlich(SD_MMC.totalBytes()));
    }
    uint32_t seitHost = millis() - g_lastHost;
    h += zl("Letzter Zugriff des Hosts", g_lastHost ? ("vor " + dauer(seitHost)) : "noch keiner");
    h += zl("Unverarbeitete Schreibvorgaenge", g_dirty ? "<span class=\"warn\">ja</span>" : "nein");
    h += zl("Vom Host geschrieben", menschlich(g_bytesGeschrieben) +
            " <small>(seit dem letzten Durchlauf)</small>");
    h += zl("Roh erkannt", g_rohAnzahl ? (String(g_rohAnzahl) + " Datei(en), " +
            menschlich(g_rohSumme) + ", " + String(g_rohStabil) + "/" + String(ROH_STABIL) +
            " stabil") : "nichts");
    h += zl("Letztes Abschlusskommando", g_letztesStop
            ? ("vor " + dauer(millis() - g_letztesStop))
            : "<span class=\"warn\">noch keines - der Drucker meldet sein Jobende nicht</span>");
    if (g_dirty) {
        h += zl("Wartet auf Commit, Versuch", String(g_versuche) + " von " + String(MAX_VERSUCHE));
        uint32_t rest = (millis() - g_lastWrite < IDLE_MS) ? (IDLE_MS - (millis() - g_lastWrite)) : 0;
        h += zl("Naechster Blick in", rest ? dauer(rest) : "gleich");
    }
    h += zl("WLAN", WiFi.status() == WL_CONNECTED
              ? ("<span class=\"ok\">" + WiFi.SSID() + "</span>, " + WiFi.localIP().toString() +
                 ", " + String(WiFi.RSSI()) + " dBm")
              : "<span class=\"bad\">nicht verbunden</span>");
    if (g_apKennung.length())
        h += zl("Zugangspunkt", g_apKennung + ", Kanal " + String(g_apKanal) +
                ", bei der Wahl " + String(g_apRssi) + " dBm <small>(" + String(g_apAnzahl) +
                " mit dieser SSID gefunden, staerkster genommen)</small>");
    h += zl("Ziel fuer Uploads", cfgEndpoint.length() ? cfgEndpoint : "<span class=\"bad\">nicht gesetzt</span>");
    h += zl("Ruhefrist bis \"fertig\"", dauer(g_idleMs));
    h += zl("Nach dem Senden", g_loeschen ? "loeschen" : "nach /gesendet verschieben");
    h += zl("Weboberflaeche", cfgWebPass.length() ? "<span class=\"ok\">passwortgeschuetzt</span>"
                                                  : "<span class=\"bad\">offen, jeder im WLAN kann die Scans lesen</span>");
    h += zl("Uhrzeit", g_zeitOk ? "<span class=\"ok\">per NTP gestellt</span>"
                                 : "<span class=\"warn\">unbekannt, Namen mit Laufzeit</span>");
    h += zl("Namensschema", cfgPraefix + "-JJJJMMTT-HHMMSS.pdf");
    if (g_rssiAnzahl)
        h += zl("Empfang schlechtester / mittlerer / bester",
                String(g_rssiMin) + " / " + String(g_rssiSumme / (long)g_rssiAnzahl) + " / " +
                String(g_rssiMax) + " dBm <small>(" + String(g_rssiAnzahl) + " Messungen)</small>");
    if (g_rssiLetztLast)
        h += zl("Empfang am Ende des letzten Uploads", String(g_rssiLetztLast) + " dBm");
    h += zl("Laufzeit", dauer(millis()));
    h += zl("Freier Speicher", menschlich(ESP.getFreeHeap()));
    h += "</table><p>"
         "<form method=\"post\" action=\"/jetzt-schauen\" style=\"display:inline\">"
         "<button class=\"btn\" type=\"submit\">Jetzt nach Scans schauen</button></form>"
         "<form method=\"post\" action=\"/neustart\" style=\"display:inline\">"
         "<button class=\"btn\" type=\"submit\">Neu starten</button></form></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webLog()
{
    if (!webAuth()) return;
    String h = htmlKopf("Protokoll");
    h += "<p><small>Zahl am Zeilenanfang = Millisekunden seit dem Start. "
         "Der Puffer liegt im RAM, ein Neustart loescht ihn.</small></p><pre>";
    h += g_logPuffer.length() ? g_logPuffer : String("(noch leer)");
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
                 "\xF0\x9F\x93\x81 " + voll + "/</td><td>Ordner</td></tr>";
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
    String h = htmlKopf("Dateien auf der Karte");
    if (!SD_MMC.sectorSize()) {
        h += "<p class=\"bad\">Karte ist nicht gemountet.</p>";
    } else {
        // BEWUSST kein Ab- und Anhaengen der Karte mehr. Das lief hier in der
        // Hauptschleife, waehrend der USB-Teil parallel Lesezugriffe des Druckers
        // bediente - trafen beide zusammen, griff der USB-Teil auf einen gerade
        // abgeraeumten Kartentreiber zu und der Stick startete neu. Ein blosser
        // Blick auf diese Seite durfte den Betrieb nie gefaehrden.
        h += "<table><tr><th>Name</th><th>Groesse</th></tr>";
        webDateienListe("/", h, 0);
        h += "</table>";

        // Fuer den frischen Stand brauchen wir kein Anhaengen: der Rohleser
        // schaut direkt auf die Sektoren.
        int anz = 0;
        uint32_t summe = 0;
        if (rohVerzeichnis(anz, summe) && anz > 0)
            h += "<p class=\"warn\">Roh gelesen liegen gerade " + String(anz) +
                 " Datei(en) mit zusammen " + menschlich(summe) + " in der Wurzel - "
                 "frisch vom Drucker und oben moeglicherweise noch nicht sichtbar.</p>";
        h += "<p><small>Die Liste zeigt den Stand, den der Stick selbst kennt. Ganz frische "
             "Schreibvorgaenge des Druckers erscheinen erst nach dem naechsten Durchlauf.</small></p>";
    }
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webHolen()
{
    if (!webAuth()) return;
    String pfad = g_web.arg("p");
    if (!pfad.startsWith("/")) { g_web.send(400, "text/plain; charset=utf-8", "Pfad fehlt\n"); return; }
    File f = SD_MMC.open(pfad);
    if (!f || f.isDirectory()) { g_web.send(404, "text/plain; charset=utf-8", "nicht gefunden\n"); return; }
    // Ohne Dateinamen hiess der Download im Browser "holen" und ohne passenden
    // Typ stufte Chrome ihn als unsicher ein und blockierte ihn. Mit Name und
    // Typ zeigt der Browser das PDF einfach an.
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

// Formulare nur von der eigenen Seite annehmen. Ein Browser schickt bei einer
// Absendung von einer fremden Seite immer die Kopfzeile Origin mit - stimmt sie
// nicht mit unserem Host ueberein, war es nicht unsere Seite. So konnte eine
// beliebige Webseite im selben Browser das Upload-Ziel umbiegen oder den Stick
// mitten im Upload neu starten; ein gespeichertes Passwort schickt der Browser
// automatisch mit. Werkzeuge wie curl schicken kein Origin und duerfen weiter,
// sie haben ohnehin kein im Browser gespeichertes Passwort.
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
    g_web.send(403, "text/plain; charset=utf-8", "Anfrage kam nicht von dieser Seite\n");
    return false;
}

static void webJetztSchauen()
{
    if (!webAuth() || !herkunftPruefen()) return;
    // Einmal nachschauen. Frueher wurde hier g_dirty gesetzt - das loeste den
    // Wiederhol-Mechanismus fuer den Drucker-Commit aus, der hier nicht gemeint ist.
    g_einmalSchauen = true;
    logZeile("[web] Suche manuell ausgeloest");
    g_web.sendHeader("Location", "/log");
    g_web.send(303, "text/plain; charset=utf-8", "");
}

static void webUpdateSeite()
{
    if (!webAuth()) return;
    String h = htmlKopf("Firmware aktualisieren");
    h += "<p>Datei <code>scanner.ino.bin</code> auswaehlen. Das Abbild wird in den "
         "zweiten Programmbereich geschrieben; erst wenn es vollstaendig und gepruefte "
         "ist, startet der Stick damit. Bricht die Uebertragung ab, laeuft die "
         "bisherige Firmware unveraendert weiter.</p>";
    h += "<form method=\"post\" action=\"/update\" enctype=\"multipart/form-data\">"
         "<p><input type=\"file\" name=\"firmware\" accept=\".bin\"></p>"
         "<p><button class=\"btn\" type=\"submit\">Hochladen und neu starten</button></p></form>";
    h += "<p><small>Laeuft gerade: " FW_VERSION "</small></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

// Pruefseite fuer den Rohleser: zeigt, was er ohne Dateisystem-Treiber sieht.
static void webRoh()
{
    if (!webAuth()) return;
    int anz = 0;
    uint32_t summe = 0;
    fatLageLesen();
    bool ok = rohVerzeichnis(anz, summe);
    String h = htmlKopf("Rohes Mitlesen");
    h += "<table>";
    h += zl("Layout erkannt", g_fat.gueltig ? "<span class=\"ok\">ja</span>"
                                            : "<span class=\"bad\">nein</span>");
    if (g_fat.gueltig) {
        h += zl("Sektoren je Cluster", String(g_fat.sektorenProCluster));
        h += zl("Zuordnungstabelle ab Sektor", String(g_fat.fatStart));
        h += zl("Datenbereich ab Sektor", String(g_fat.ersterDatenSektor));
        h += zl("Wurzelverzeichnis in Cluster", String(g_fat.rootCluster));
    }
    h += zl("Lesen erfolgreich", ok ? "ja" : "<span class=\"bad\">nein</span>");
    h += zl("Gefundene Dateien", String(anz) + " <small>(ab " +
            String(ROH_MIN_GROESSE / 1024) + " kB; kleinere gelten als Hilfsdateien)</small>");
    h += zl("Summe der Groessen", menschlich(summe));
    h += zl("Stabile Messungen in Folge", String(g_rohStabil) + " von " + String(ROH_STABIL));
    h += "</table><p><small>Dieser Blick geht direkt auf die Sektoren der Karte und "
         "stoert den Drucker nicht - anders als das Ab- und Anhaengen, das bisher "
         "noetig war.</small></p>";
    g_web.send(200, "text/html; charset=utf-8", h + htmlFuss());
}

static void webNeustart()
{
    if (!webAuth() || !herkunftPruefen()) return;
    g_web.send(200, "text/html; charset=utf-8",
               htmlKopf("Neustart") + "<p>Der Stick startet neu. Diese Seite ist in etwa "
               "10 Sekunden wieder da.</p>" + htmlFuss());
    delay(300);
    ESP.restart();
}

static void webEinstellungen()
{
    if (!webAuth()) return;

    if (g_web.method() == HTTP_POST) {
        if (!herkunftPruefen()) return;
        if (g_web.hasArg("endpoint")) cfgEndpoint = g_web.arg("endpoint");
        if (g_web.hasArg("idle")) {
            uint32_t sek = g_web.arg("idle").toInt();
            if (sek >= 5 && sek <= 600) g_idleMs = sek * 1000;
        }
        if (g_web.hasArg("nachher")) g_loeschen = (g_web.arg("nachher") == "loeschen");
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
        // Leeres Passwortfeld heisst "unveraendert lassen", nicht "Schutz aus" -
        // sonst schaltet ein unbedachtes Speichern den Schutz ab.
        if (g_web.hasArg("webpass") && g_web.arg("webpass").length()) cfgWebPass = g_web.arg("webpass");
        if (g_web.hasArg("passweg") && g_web.arg("passweg") == "ja") cfgWebPass = "";
        for (int i = 0; i < Z_ANZAHL; i++) {
            char k[10];
            snprintf(k, sizeof k, "f%d", i);
            if (g_web.hasArg(k)) g_farbe[i] = strtoul(g_web.arg(k).c_str() + 1, nullptr, 16);
        }
        if (g_web.hasArg("fsendet")) g_farbeSendet = strtoul(g_web.arg("fsendet").c_str() + 1, nullptr, 16);
        if (g_web.hasArg("ffertig")) g_farbeFertig = strtoul(g_web.arg("ffertig").c_str() + 1, nullptr, 16);
        cfgNachNvs();
        logZeile("[web] Einstellungen gespeichert");
        g_screen = -1;            // Anzeige mit den neuen Farben neu zeichnen
        g_web.sendHeader("Location", "/einstellungen?ok=1");
        g_web.send(303, "text/plain; charset=utf-8", "");
        return;
    }

    String h = htmlKopf("Einstellungen");
    if (g_web.hasArg("ok")) h += "<p class=\"ok\">Gespeichert.</p>";
    h += "<form method=\"post\" action=\"/einstellungen\">"
         "<input type=\"hidden\" name=\"speichern\" value=\"1\"><table>";
    h += "<tr><td>Helligkeit der Status-LED<br><small>0 = aus, 31 = maximal. "
         "Voll aufgedreht ist sie als Dauerlicht sehr grell.</small></td>"
         "<td><input name=\"ledhell\" type=\"number\" min=\"0\" max=\"31\" value=\"" +
         String(g_ledHell) + "\"></td></tr>";
    h += String("<tr><td>Displayfarben umkehren<br><small>dieses Modul stellt sonst alles als "
         "Negativ dar - eingestelltes Blau erscheint gelb</small></td><td>"
         "<label><input type=\"checkbox\" name=\"invers\" value=\"1\"") +
         (g_invertiert ? " checked" : "") + "> umkehren</label></td></tr>";
    h += "<tr><td>Upload-Ziel</td><td><input name=\"endpoint\" size=\"34\" value=\"" + cfgEndpoint + "\"></td></tr>";
    h += "<tr><td>Namensanfang der Dateien<br><small>ergibt z.B. <code>" + cfgPraefix +
         "-20260919-143205.pdf</code>; bei mehreren Sticks den Standort hier eintragen</small></td>"
         "<td><input name=\"praefix\" size=\"16\" value=\"" + cfgPraefix + "\"></td></tr>";
    h += "<tr><td>Ruhefrist in Sekunden<br><small>so lange Stille, bis ein Scan als fertig gilt "
         "(der 780 braucht 45)</small></td><td><input name=\"idle\" type=\"number\" min=\"5\" max=\"600\" value=\"" +
         String(g_idleMs / 1000) + "\"></td></tr>";
    h += String("<tr><td>Nach dem Senden</td><td>"
         "<label><input type=\"radio\" name=\"nachher\" value=\"loeschen\"") + (g_loeschen ? " checked" : "") +
         "> von der Karte loeschen</label><br>"
         "<label><input type=\"radio\" name=\"nachher\" value=\"aufheben\"" + (g_loeschen ? "" : " checked") +
         "> nach /gesendet verschieben</label></td></tr>";
    h += "<tr><td>Passwort der Weboberflaeche<br><small>Benutzername ist <b>scan</b>. "
         "Leer lassen = unveraendert.</small></td><td><input name=\"webpass\" type=\"password\" size=\"18\">"
         "<br><label><small><input type=\"checkbox\" name=\"passweg\" value=\"ja\"> Schutz entfernen</small></label></td></tr>";
    for (int i = 0; i < Z_ANZAHL; i++) {
        char k[10];
        snprintf(k, sizeof k, "f%d", i);
        h += String("<tr><td>Farbe: ") + Z_NAME[i] + " <small>(" + Z_TEXT[i] + ")</small></td>"
             "<td><input type=\"color\" name=\"" + k + "\" value=\"" + hexFarbe(g_farbe[i]) + "\"></td></tr>";
    }
    h += "<tr><td>Farbe: sendet gerade</td><td><input type=\"color\" name=\"fsendet\" value=\"" +
         hexFarbe(g_farbeSendet) + "\"></td></tr>";
    h += "<tr><td>Farbe: erfolgreich gesendet</td><td><input type=\"color\" name=\"ffertig\" value=\"" +
         hexFarbe(g_farbeFertig) + "\"></td></tr>";
    h += "</table><p><button class=\"btn\" type=\"submit\">Speichern</button></p></form>";
    h += "<p><small>WLAN-Zugangsdaten kommen weiter aus <code>/wifi.cfg</code> auf der Karte und "
         "werden in den Flash gespiegelt.</small></p>";
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
    // Nebenwirkungen nur per POST: ein GET laesst sich von jeder fremden Seite
    // als Bild-Adresse unterschieben, ein POST nicht ohne Origin-Kopfzeile.
    g_web.on("/jetzt-schauen", HTTP_POST, webJetztSchauen);
    g_web.on("/neustart", HTTP_POST, webNeustart);
    g_web.on("/update", HTTP_GET, webUpdateSeite);
    g_web.on("/update", HTTP_POST,
        []() {
            // Der Abschluss lief frueher ohne Passwortpruefung und startete den
            // Stick auch dann neu, wenn gar kein Abbild geschrieben worden war.
            // Ein "curl -X POST /update" von irgendwem im WLAN genuegte damit,
            // einen laufenden Upload abzuschiessen.
            if (!webAuth() || !herkunftPruefen()) return;
            bool ok = g_updateBegonnen && Update.isFinished() && !Update.hasError();
            g_updateBegonnen = false;
            g_web.send(ok ? 200 : 400, "text/html; charset=utf-8",
                       htmlKopf(ok ? "Update eingespielt" : "Update fehlgeschlagen") +
                       (ok ? "<p class=\"ok\">Der Stick startet jetzt neu. Diese Seite ist in "
                             "etwa 10 Sekunden wieder da.</p>"
                           : "<p class=\"bad\">Es wurde kein vollstaendiges Abbild geschrieben, "
                             "die bisherige Firmware laeuft weiter.</p>") + htmlFuss());
            delay(600);
            if (ok) ESP.restart();
        },
        []() {
            // Hier nur pruefen, nicht antworten - die Antwort gibt der Abschluss.
            // Ein Passwort ist gesetzt und fehlt: Abbild gar nicht erst annehmen.
            if (cfgWebPass.length() && !g_web.authenticate("scan", cfgWebPass.c_str())) return;
            if (!herkunftOk()) return;
            HTTPUpload &up = g_web.upload();
            if (up.status == UPLOAD_FILE_START) {
                logZeile("[update] Start: " + up.filename);
                g_updateBegonnen = Update.begin(UPDATE_SIZE_UNKNOWN);
                if (!g_updateBegonnen) logZeile("[update] begin fehlgeschlagen");
            } else if (up.status == UPLOAD_FILE_WRITE) {
                if (Update.write(up.buf, up.currentSize) != up.currentSize)
                    logZeile("[update] Schreibfehler");
            } else if (up.status == UPLOAD_FILE_END) {
                if (Update.end(true)) logZeile(String("[update] fertig, ") + up.totalSize + " Bytes");
                else logZeile("[update] Abschluss fehlgeschlagen");
            }
        });
    g_web.onNotFound([]() { g_web.sendHeader("Location", "/"); g_web.send(303, "text/plain", ""); });
    // Der WebServer behaelt nur angeforderte Kopfzeilen; Host merkt er sich immer.
    const char *kopf[] = { "Origin", "Referer" };
    g_web.collectHeaders(kopf, 2);
    g_web.begin();
    if (MDNS.begin("scanstick")) MDNS.addService("http", "tcp", 80);
    g_webAn = true;
    logZeile(String("[web] erreichbar unter http://") + WiFi.localIP().toString() +
             "/ und http://scanstick.local/");
}

// ---- nach Scan-Ende: neue Dateien hochladen ----
// Ordner rekursiv durchgehen, Dateien hochladen; loggt jeden Eintrag (Diagnose)
#define MAX_FUND 24
static String g_fund[MAX_FUND];
static int    g_fundAnzahl = 0;

// SCHRITT 1: nur suchen. Reines Lesen - das darf gefahrlos passieren,
// waehrend der Drucker das Medium noch hat.
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
        logZeile(String("[fund] ") + (istDir ? "DIR " : "DAT ") + voll + " " + sz + "B");
        if (istDir) {
            if (tiefe < 3 && !basis.startsWith(".") && !basis.equalsIgnoreCase("gesendet"))
                sammleDateien(voll, tiefe + 1);
            continue;
        }
        if (!istScanName(basis)) continue;   // derselbe Massstab wie im Rohleser
        if (!sz) continue;
        if (g_fundAnzahl < MAX_FUND) g_fund[g_fundAnzahl++] = voll;
    }
}

// SCHRITT 2: umbenennen, senden, aufraeumen. Hier wird die FAT veraendert -
// der Aufrufer MUSS dem Host das Medium vorher entzogen haben, sonst schreibt
// der Drucker seine alte Sicht darueber und die Aenderungen sind weg.
static void sendeGefundene(bool wifi, int &hoch, int &fehler)
{
    for (int i = 0; i < g_fundAnzahl; i++) {
        String voll = g_fund[i];
        String basis = voll;
        int sl = basis.lastIndexOf('/');
        if (sl >= 0) basis = basis.substring(sl + 1);

        // Ist das dieselbe Datei, die wir gerade eben gesendet haben?
        uint32_t groesse = 0;
        {
            File pf = SD_MMC.open(voll);
            if (pf) { groesse = pf.size(); pf.close(); }
        }
        if (!dateiVollstaendig(voll, groesse)) {
            logZeile("[warte] " + basis + " (" + (groesse / 1024) +
                     " kB) hat noch keine Endmarke - der Drucker schreibt noch");
            fehler++;
            continue;
        }
        // Die Kennzahl geht als Kennung mit; ob es eine Wiederholung ist,
        // entscheidet der Empfaenger. Frueher verglich der Stick hier selbst
        // mit der zuletzt gesendeten Datei und LOESCHTE bei Gleichstand ohne
        // Upload - die Kennzahl deckt aber nur Anfang und Ende ab, zwei echte
        // Scans desselben Formulars haetten so still verschwinden koennen.
        uint32_t kennzahl = dateiKennzahl(voll, groesse);

        // Umbenennen, bevor gesendet wird: der Drucker nennt jeden Scan gleich
        // ("[Untitled].pdf"), der Name muss sofort wieder frei sein.
        String sendePfad = voll, sendeName = basis;
        if (basis.startsWith(cfgPraefix + "-")) {
            // Traegt schon unseren Namen: ein frueherer Upload ist gescheitert,
            // die Datei liegt noch. Nicht erneut umbenennen.
            logZeile("[um] " + basis + " hat schon unseren Namen, zweiter Anlauf");
        } else {
            String ordner = (sl >= 0) ? voll.substring(0, sl + 1) : String("/");
            String zielName = neuerName(basis);
            String zielPfad = ordner + zielName;
            if (SD_MMC.rename(voll, zielPfad)) {
                sendePfad = zielPfad;
                sendeName = zielName;
                logZeile("[um] " + basis + " -> " + zielName);
            } else {
                logZeile("[um] Umbenennen fehlgeschlagen, sende unter " + basis);
            }
        }

        char idText[32];
        snprintf(idText, sizeof idText, "%08x-%08x", (unsigned)groesse, (unsigned)kennzahl);
        if (wifi && ladeHoch(SD_MMC, sendePfad, sendeName, String(idText))) {
            if (g_loeschen) {
                SD_MMC.remove(sendePfad);
            } else {
                // Aufheben statt loeschen. Aus der Wurzel raus, sonst findet der
                // naechste Durchlauf die Datei wieder und schickt sie erneut.
                if (!SD_MMC.exists("/gesendet")) SD_MMC.mkdir("/gesendet");
                String ziel = "/gesendet/" + sendeName;
                for (int n = 1; SD_MMC.exists(ziel) && n < 100; n++) {
                    int punkt = sendeName.lastIndexOf('.');
                    ziel = "/gesendet/" + (punkt > 0 ? sendeName.substring(0, punkt) : sendeName) +
                           "_" + n + (punkt > 0 ? sendeName.substring(punkt) : String(""));
                }
                if (SD_MMC.rename(sendePfad, ziel)) logZeile("[up] aufgehoben als " + ziel);
                else { logZeile("[up] Verschieben fehlgeschlagen, loesche"); SD_MMC.remove(sendePfad); }
            }
            hoch++;
        } else {
            fehler++;
        }
    }
}

static void verarbeiteScans(bool manuell)
{
    uint32_t begonnen = millis();
    g_warteAnzeige = false;
    logZeile(manuell ? "[scan] manuelle Suche" : "[scan] Scan fertig, verarbeite");
    zeigeScreen(Z_SUCHT);

    // SCHRITT 1: roh nachsehen. Reines Lesen von Sektoren - das stoert den
    // Drucker nicht und fasst den Kartentreiber nicht an. Ist nichts da, sparen
    // wir uns alles Weitere und damit jedes Risiko. (Der Rohleser sieht nur das
    // Wurzelverzeichnis; genau dort legt der Drucker seine Scans ab.)
    int rohAnz = 0;
    uint32_t rohGroesse = 0;
    rohVerzeichnis(rohAnz, rohGroesse);

    int hoch = 0, fehler = 0;
    if (rohAnz > 0) {
        // SCHRITT 2: ERST den Host abmelden, DANN die Karte neu anhaengen.
        // Vorher lief das Ab- und Anhaengen bei angemeldetem Medium. Traf in
        // dieses Fenster ein Zugriff des Druckers, griff der USB-Teil auf einen
        // gerade abgeraeumten Kartentreiber zu und der Stick startete neu -
        // derselbe Fehler, der frueher in der Dateien-Seite steckte. Gefahrlos
        // ist der Entzug hier, weil roh bereits eine fertige Datei zu sehen war.
        MSC.mediaPresent(false);
        delay(200);

        SD_MMC.end();
        delay(100);
        SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
        if (!SD_MMC.begin()) {
            logZeile("[scan] remount fehlgeschlagen");
            MSC.mediaPresent(true);          // Medium auf keinen Fall verlieren
            return;
        }

        // SCHRITT 3: jetzt sind wir allein auf der Karte
        g_fundAnzahl = 0;
        sammleDateien("/", 0);
        bool wifi = wifiVerbinden();
        sendeGefundene(wifi, hoch, fehler);

        // SCHRITT 4: Cache schreiben, dem Drucker eine frische Sicht geben
        SD_MMC.end();
        delay(80);
        SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
        SD_MMC.begin();
        MSC.mediaPresent(true);
        logZeile("[usb] Stick neu angemeldet");
    } else {
        logZeile("[scan] roh nachgesehen: nichts zu holen");
    }
    logZeile(String("[scan] fertig: ") + hoch + " hochgeladen, " + fehler + " Fehler, " +
             "vom Host geschrieben: " + (g_bytesGeschrieben / 1024) + " kB");
    if (hoch > 0) g_bytesGeschrieben = 0;

    // Hat der Drucker waehrend unserer Arbeit geschrieben? Ein Upload kann eine
    // Minute dauern - ohne diese Pruefung faellt ein in der Zeit entstandener
    // Scan hinten runter, weil g_dirty blind geloescht wuerde.
    if ((int32_t)(g_lastWrite - begonnen) > 0) {
        logZeile("[scan] neuer Schreibzugriff waehrend der Verarbeitung - bleibe dran");
        g_versuche = 0;
        g_naechsterVersuch = 0;
        logFlushNetz();
        return;                       // g_dirty bleibt stehen
    }

    g_versuche++;
    if (hoch == 0 && fehler == 0) {
        if (manuell) {
            // Von Hand ausgeloest und nichts gefunden: keinen Wartezyklus starten
            logZeile("[scan] nichts gefunden");
            g_versuche = 0;
            logFlushNetz();
            return;
        }
        // Nichts da. Der Drucker hat den Scan vermutlich noch nicht committet:
        // dranbleiben statt aufgeben, g_dirty bleibt stehen.
        if (g_versuche < MAX_VERSUCHE) {
            g_naechsterVersuch = millis() + RETRY_MS;
            g_dirty = true;                 // dranbleiben
            logZeile(String("[warte] nichts gefunden (Versuch ") + g_versuche + "/" + MAX_VERSUCHE +
                  "), neuer Blick in " + (RETRY_MS / 1000) + "s");
            zeigeWarte(g_versuche, MAX_VERSUCHE);
            logFlushNetz();
            return;
        }
        logZeile(String("[warte] nach ") + g_versuche + " Versuchen nichts gefunden - aufgegeben");
    }
    g_dirty = false;
    g_versuche = 0;
    g_naechsterVersuch = 0;
    g_rohStabil = 0;
    g_rohAnzahl = 0;
    g_rohSumme = 0;
    logFlushNetz();   // Anzeige macht wieder loop(): BEREIT oder OK
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Scan-Stick " FW_VERSION " ===");

    ledInit();
    ledColor(60, 60, 60);   // WEISS = Strom da, bootet
    displayInit();
    zeigeScreen(0);         // STROM + Blitz

    // Zugangsdaten zuerst aus dem Flash. Damit kommen WLAN und Weboberflaeche
    // auch hoch, wenn die Karte fehlt oder nicht mountet - dann kann der Stick
    // selbst melden, was ihm fehlt, statt stumm zu bleiben.
    cfgAusNvs();
    wlanStarten();

    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    bool sdOk = SD_MMC.begin("/sdcard", false, false);
    if (!sdOk) { logZeile("[sd] Mount fehlgeschlagen"); zeigeScreen(Z_SDFEHL); }
    else fixMbrTyp();   // MBR nur bei gemounteter Karte anfassen

    if (sdOk) {
        String altS = cfgSsid, altP = cfgPass, altE = cfgEndpoint;
        ladeConfig();                     // /wifi.cfg hat Vorrang
        if (cfgSsid != altS || cfgPass != altP || cfgEndpoint != altE) {
            cfgNachNvs();
            logZeile("[nvs] aus /wifi.cfg aktualisiert");
            if (WiFi.status() != WL_CONNECTED) wlanStarten();
        }
    }

    MSC.vendorID("DIY");
    MSC.productID("ScanStick");
    MSC.productRevision("1.0");
    MSC.onStartStop(onStartStop);
    MSC.onRead(onRead);
    MSC.onWrite(onWrite);
    // Ohne Karte "kein Medium" melden statt eines Laufwerks mit 0 Sektoren. So
    // sieht der Host einen leeren Kartenleser, und von aussen ist der Zustand
    // vom frueheren Boot-Haenger zu unterscheiden.
    MSC.mediaPresent(sdOk);
    MSC.isWritable(true);
    uint32_t sec = SD_MMC.sectorSize();
    uint32_t rawSectors = sec ? (uint32_t)(SD_MMC.cardSize() / sec) : 0;
    Serial.printf("[usb] MSC-Sektoren(roh)=%u sec=%u\n", rawSectors, sec);
    MSC.begin(rawSectors, sec ? sec : 512);
    USB.begin();
    Serial.println("[usb] als USB-Speicher gestartet");
    if (g_screen != Z_SDFEHL) zeigeScreen(Z_BEREIT);
}

void loop()
{
    // Ohne Uhr heissen die Dateien "scan-nach173s.pdf" statt mit Datum. Ein
    // einzelner Fehlversuch beim Start soll das nicht fuer immer festlegen.
    if (!g_zeitOk && WiFi.status() == WL_CONNECTED && !g_dirty &&
        millis() - g_zeitVersuch > ZEIT_WIEDERHOLUNG) {
        zeitHolen();
    }

    // Ein Stromausfall oder Neustart mitten im Ablauf darf keine Datei
    // liegenlassen: Schreibzugriffe merkt sich der Stick nur im Arbeitsspeicher,
    // nach dem Start weiss er also nichts von dem, was schon da ist.
    if (!g_startGeprueft && WiFi.status() == WL_CONNECTED && millis() > 8000) {
        g_startGeprueft = true;
        int a = 0;
        uint32_t gr = 0;
        if (rohVerzeichnis(a, gr) && a > 0) {
            logZeile(String("[start] ") + a + " Datei(en) mit " + (gr / 1024) +
                     " kB lagen noch da - nehme sie mit");
            g_dirty = true;
            g_lastWrite = millis() - IDLE_MS - 1;   // sofort faellig
            g_versuche = 0;
            g_naechsterVersuch = 0;
        }
    }

    if (g_stopNeu) {
        g_stopNeu = false;
        logZeile(String("[scsi] START STOP UNIT: start=") + (g_stopStart ? "ja" : "nein") +
                 " eject=" + (g_stopEject ? "ja" : "nein") + " pc=" + g_stopPc);
    }
    if (WiFi.status() == WL_CONNECTED && !g_webAn) { webStarten(); zeitHolen(); }
    if (g_webAn) g_web.handleClient();

    uint32_t nun = millis();

    // Roh nachsehen, solange etwas offen ist. Reines Lesen - der Drucker merkt
    // davon nichts, deshalb darf es jede Sekunde passieren. Bleiben Anzahl und
    // Groesse mehrfach gleich, hat der Drucker die Datei geschlossen.
    // Die erkannte Groesse allein genuegt NICHT: der 780 traegt die endgueltige
    // Groesse schon vor dem Schreiben ein. Wuerden wir darauf vertrauen, benennen
    // wir mitten im laufenden Scan um - der Drucker schreibt dann seine alte
    // Verzeichnissicht zurueck und die Umbenennung ist weg. Also zusaetzlich Ruhe abwarten.
    if (g_dirty && !g_warteAnzeige && nun - g_lastWrite > ROH_RUHE &&
        nun - g_rohLetzt > ROH_INTERVALL) {
        g_rohLetzt = nun;
        int anz = 0;
        uint32_t summe = 0;
        if (rohVerzeichnis(anz, summe) && anz > 0) {
            if (anz == g_rohAnzahl && summe == g_rohSumme) {
                if (++g_rohStabil >= ROH_STABIL) {
                    logZeile(String("[roh] ") + anz + " Datei(en), " + (summe / 1024) +
                             " kB, Groesse stabil - verarbeite sofort");
                    g_rohStabil = 0;
                    g_rohAnzahl = 0;
                    g_rohSumme = 0;
                    verarbeiteScans(false);
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
        verarbeiteScans(true);
    } else if (nun - g_lastWrite < IDLE_MS) {
        // Der Host schreibt noch: Beobachtung von vorn beginnen
        g_versuche = 0;
        g_naechsterVersuch = 0;
        g_warteAnzeige = false;
    } else if (g_dirty && (g_naechsterVersuch == 0 || (int32_t)(nun - g_naechsterVersuch) >= 0)) {
        verarbeiteScans(false);
    }
    // Nur die WARTE-Anzeige ist geschuetzt. Frueher stand hier !g_dirty - das
    // fror JEDE Anzeige ein, sobald ein Schreibvorgang offen war, also ueber die
    // gesamte Ruhefrist hinweg.
    if (g_screen != Z_SDFEHL && !g_warteAnzeige) {
        uint32_t seitWrite = nun - g_lastWrite;
        if (g_dirty && seitWrite < IDLE_MS)
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
    if (millis() - hb > 2000) { hb = millis(); ledHeartbeat(); }   // alle 2s kurz blinken
    delay(20);   // kurz, damit die Weboberflaeche fluessig antwortet
}
