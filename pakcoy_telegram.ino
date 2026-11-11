/*
  =====================================================================
   PENYIRAMAN OTOMATIS PAKCOY + MONITORING TANDON (ESP32 + TELEGRAM)
  =====================================================================

  RANGKAIAN PIN
  ---------------------------------------------------------------------
  Sensor kelembapan tanah : VCC -> 3V3   | GND -> GND | AO -> D34
  Sensor ketinggian air   : +   -> 3V3   | -   -> GND | S  -> D35
  Modul relay             : VCC -> 3V3 (BUKAN VIN) | GND -> GND | IN -> D26
  Pompa (terminal relay)  : Baterai(+) -> COM 
                            NO         -> Pompa(+)
                            Pompa(-)   -> Baterai(-)   (NC dibiarkan kosong)
  Daya ESP32              : kabel microUSB ke laptop / charger HP 2A
                            (terpisah dari baterai pompa)

  LIBRARY (Tools > Manage Libraries)
  ---------------------------------------------------------------------
  - UniversalTelegramBot (Brian Lough)
  - ArduinoJson (Benoit Blanchon)

  CARA KERJA
  ---------------------------------------------------------------------
  - Tanah < 50%  -> pompa menyiram 3 detik, lalu jeda 5 detik agar air
                    meresap, lalu cek lagi. Diulang sampai tanah >= 70%.
  - Air tandon <= 10% -> pompa tidak boleh nyala + notif "Air Akan Habis".
  - Kalau sudah 10 putaran tapi tanah belum 70%, penyiraman dihentikan
    demi keamanan (kemungkinan sensor lepas atau selang bocor).
  - Penyiraman otomatis tetap jalan walaupun WiFi putus.
  =====================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ===================== 1. WAJIB DIISI =====================
const char* WIFI_SSID = "nama wifi user";        // ESP32 tidak bisa WiFi 5 GHz
const char* WIFI_PASS = "password wifi user";
#define BOT_TOKEN "bot token yang user dapat"     // pakai token BARU setelah /revoke
#define CHAT_ID   "tanya AI"

// ===================== 2. PIN =====================
const int PIN_SOIL  = 34;   // AO sensor tanah
const int PIN_WATER = 35;   // S sensor air
const int PIN_RELAY = 26;   // IN relay

// Relay kamu aktif LOW: sinyal LOW = pompa nyala, HIGH = pompa mati
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ===================== 3. KALIBRASI SENSOR =====================
// Angka mentah ADC (0 - 4095). Ganti dengan hasil kalibrasimu sendiri.
const int SOIL_KERING = 4095;   // sensor di udara / tanah kering
const int SOIL_BASAH  = 1500;   // sensor di tanah yang baru disiram
const int AIR_KOSONG  = 0;      // sensor air tidak terendam
const int AIR_PENUH   = 1850;   // sensor air terendam sampai batas atas

// ===================== 4. ATURAN PENYIRAMAN =====================
const int BATAS_MULAI_SIRAM = 50;   // % tanah, di bawah ini pompa mulai
const int BATAS_STOP_SIRAM  = 70;   // % tanah, sampai sini pompa berhenti
const int BATAS_AIR_KRITIS  = 10;   // % tandon, di bawah ini pompa dilarang nyala
const int BATAS_AIR_AMAN    = 25;   // % tandon, dianggap sudah diisi ulang

const unsigned long LAMA_POMPA_MS        = 3000;   // sekali semprot
const unsigned long LAMA_RESAP_MS        = 5000;   // jeda agar air meresap
const int           MAKS_PUTARAN         = 10;     // pengaman pompa
const unsigned long LAMA_SIRAM_MANUAL_MS = 5000;   // tombol Siram Manual

const unsigned long INTERVAL_BOT_MS    = 1000;   // cek pesan Telegram
const unsigned long INTERVAL_SERIAL_MS = 2000;   // cetak data ke Serial Monitor
const unsigned long INTERVAL_WIFI_MS   = 10000;  // coba sambung ulang WiFi

// ===================== VARIABEL SISTEM =====================
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

enum Fase { DIAM, POMPA_AUTO, RESAP, POMPA_MANUAL };
Fase fase = DIAM;
unsigned long faseMulai = 0;
int putaran = 0;

bool pompaNyala = false;
bool modeOtomatis = true;
bool notifAirHabisTerkirim = false;

int tanahPersen = 0;
int airPersen = 0;

unsigned long terakhirCekBot = 0;
unsigned long terakhirSerial = 0;
unsigned long terakhirCobaWiFi = 0;

const String KEYBOARD =
  "[[\"📊 Cek Status\", \"💧 Siram Manual\"],"
  " [\"🛑 Stop Pompa\"],"
  " [\"🟢 Auto ON\", \"⚪ Auto OFF\"]]";

// ===================== FUNGSI POMPA =====================
void pompa(bool nyala) {
  digitalWrite(PIN_RELAY, nyala ? RELAY_ON : RELAY_OFF);
  pompaNyala = nyala;
}

void pindahFase(Fase baru) {
  fase = baru;
  faseMulai = millis();
}

// ===================== FUNGSI SENSOR =====================
int bacaRataRata(int pin) {
  long total = 0;
  for (int i = 0; i < 10; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return total / 10;
}

int bacaTanahPersen() {
  int nilai = bacaRataRata(PIN_SOIL);
  int persen = map(nilai, SOIL_KERING, SOIL_BASAH, 0, 100);
  return constrain(persen, 0, 100);
}

int bacaAirPersen() {
  int nilai = bacaRataRata(PIN_WATER);
  int persen = map(nilai, AIR_KOSONG, AIR_PENUH, 0, 100);
  return constrain(persen, 0, 100);
}

String kondisiTanah(int persen) {
  if (persen < BATAS_MULAI_SIRAM) return "Kering 🏜️";
  if (persen <= 80) return "Lembap (Ideal) 🌱";
  return "Sangat Basah 💦";
}

String kondisiAir(int persen) {
  if (persen <= BATAS_AIR_KRITIS) return "Hampir Habis ⚠️";
  if (persen < 50) return "Sedang";
  return "Aman ✅";
}

// ===================== FUNGSI TELEGRAM =====================
bool wifiOK() {
  return WiFi.status() == WL_CONNECTED;
}

void kirim(const String& pesan) {
  if (wifiOK()) bot.sendMessage(CHAT_ID, pesan, "HTML");
}

String teksStatus(const String& judul) {
  String p = "<b>" + judul + "</b>\n\n";
  p += "🌱 <b>Tanah :</b> " + String(tanahPersen) + "% (" + kondisiTanah(tanahPersen) + ")\n";
  p += "🪣 <b>Air Tandon :</b> " + String(airPersen) + "% (" + kondisiAir(airPersen) + ")\n";
  p += "🚰 <b>Pompa :</b> " + String(pompaNyala ? "NYALA 🔵" : "MATI ⚪") + "\n";
  p += "⚙️ <b>Mode Otomatis :</b> " + String(modeOtomatis ? "AKTIF 🟢" : "NONAKTIF ⚪") + "\n";
  p += "📶 <b>Sinyal WiFi :</b> " + String(WiFi.RSSI()) + " dBm";
  return p;
}

bool cocok(const String& teks, const char* perintah, const char* tombol) {
  return teks == perintah || teks.indexOf(tombol) >= 0;
}

void prosesPesan(int jumlah) {
  for (int i = 0; i < jumlah; i++) {
    String chatId = bot.messages[i].chat_id;
    String teks = bot.messages[i].text;

    if (chatId != CHAT_ID) {
      bot.sendMessage(chatId, "⛔ Akses ditolak.", "");
      continue;
    }

    if (teks == "/start") {
      bot.sendMessageWithReplyKeyboard(CHAT_ID,
        "<b>🤖 Bot Penyiraman Pakcoy</b>\n\nPilih menu di bawah.",
        "HTML", KEYBOARD, true);
    }
    else if (cocok(teks, "/status", "Cek Status")) {
      kirim(teksStatus("📊 STATUS KEBUN"));
    }
    else if (cocok(teks, "/siram", "Siram Manual")) {
      if (airPersen <= BATAS_AIR_KRITIS) {
        kirim("<b>⛔ Tidak bisa menyiram</b>\n\nAir tandon tinggal " + String(airPersen) +
              "%. Isi ulang dulu agar pompa tidak menyedot angin.");
      } else {
        pompa(true);
        pindahFase(POMPA_MANUAL);
        kirim("<b>💧 SIRAM MANUAL</b>\n\nPompa menyala selama " +
              String(LAMA_SIRAM_MANUAL_MS / 1000) + " detik...");
      }
    }
    else if (cocok(teks, "/stop", "Stop Pompa")) {
      pompa(false);
      pindahFase(DIAM);
      modeOtomatis = false;
      kirim(teksStatus("🛑 POMPA DIHENTIKAN") +
            "\n\n<i>Mode otomatis ikut dimatikan. Tekan Auto ON untuk menyalakan lagi.</i>");
    }
    else if (cocok(teks, "/auto_on", "Auto ON")) {
      modeOtomatis = true;
      kirim(teksStatus("🟢 MODE OTOMATIS AKTIF") +
            "\n\n<i>Pompa menyiram sendiri saat tanah di bawah " +
            String(BATAS_MULAI_SIRAM) + "%.</i>");
    }
    else if (cocok(teks, "/auto_off", "Auto OFF")) {
      modeOtomatis = false;
      if (fase == POMPA_AUTO || fase == RESAP) {
        pompa(false);
        pindahFase(DIAM);
      }
      kirim(teksStatus("⚪ MODE OTOMATIS NONAKTIF") +
            "\n\n<i>Pompa tidak akan menyiram sendiri.</i>");
    }
    else {
      kirim("Perintah tidak dikenal. Ketik /start untuk memunculkan tombol menu.");
    }
  }
}

// ===================== FUNGSI WIFI =====================
void sambungWiFi() {
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long mulai = millis();
  while (!wifiOK() && millis() - mulai < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (wifiOK()) {
    Serial.print("WiFi TERHUBUNG! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi GAGAL. Cek nama/password & pastikan 2.4 GHz.");
    Serial.println("Penyiraman otomatis tetap jalan tanpa Telegram.");
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_RELAY, OUTPUT);
  pompa(false);                 // pompa dipastikan mati saat baru menyala

  Serial.println("\n========================================");
  Serial.println(" SISTEM PENYIRAMAN PAKCOY");
  Serial.println("========================================");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);
  client.setInsecure();
  sambungWiFi();

  tanahPersen = bacaTanahPersen();
  airPersen = bacaAirPersen();

  if (wifiOK()) {
    bot.sendMessageWithReplyKeyboard(CHAT_ID,
      teksStatus("🤖 SISTEM PAKCOY ONLINE"), "HTML", KEYBOARD, true);
  }
}

// ===================== LOOP =====================
void loop() {
  unsigned long sekarang = millis();

  // --- Jaga koneksi WiFi ---
  if (!wifiOK() && sekarang - terakhirCobaWiFi > INTERVAL_WIFI_MS) {
    terakhirCobaWiFi = sekarang;
    Serial.println("WiFi terputus, mencoba sambung ulang...");
    WiFi.reconnect();
  }

  // --- Cek pesan Telegram ---
  if (wifiOK() && sekarang - terakhirCekBot > INTERVAL_BOT_MS) {
    terakhirCekBot = sekarang;
    int jumlah = bot.getUpdates(bot.last_message_received + 1);
    while (jumlah) {
      prosesPesan(jumlah);
      jumlah = bot.getUpdates(bot.last_message_received + 1);
    }
  }

  // --- Baca sensor ---
  tanahPersen = bacaTanahPersen();
  airPersen = bacaAirPersen();

  // --- Pengaman tandon ---
  if (airPersen <= BATAS_AIR_KRITIS) {
    if (pompaNyala) {
      pompa(false);
      pindahFase(DIAM);
    }
    if (!notifAirHabisTerkirim) {
      kirim("<b>🚨 AIR AKAN HABIS</b>\n\n🪣 <b>Air Tandon :</b> " + String(airPersen) +
            "%\n\n<i>Pompa dikunci sampai tandon diisi ulang.</i>");
      notifAirHabisTerkirim = true;
    }
  } else if (airPersen >= BATAS_AIR_AMAN && notifAirHabisTerkirim) {
    kirim("<b>💦 TANDON TERISI KEMBALI</b>\n\n🪣 <b>Air Tandon :</b> " + String(airPersen) + "%");
    notifAirHabisTerkirim = false;
  }

  // --- Logika penyiraman ---
  switch (fase) {
    case DIAM:
      if (modeOtomatis && tanahPersen < BATAS_MULAI_SIRAM && airPersen > BATAS_AIR_KRITIS) {
        putaran = 0;
        pompa(true);
        pindahFase(POMPA_AUTO);
        kirim(teksStatus("⚙️ PENYIRAMAN OTOMATIS DIMULAI"));
      }
      break;

    case POMPA_AUTO:
      if (sekarang - faseMulai >= LAMA_POMPA_MS) {
        pompa(false);
        pindahFase(RESAP);
      }
      break;

    case RESAP:
      if (sekarang - faseMulai >= LAMA_RESAP_MS) {
        putaran++;
        if (tanahPersen >= BATAS_STOP_SIRAM) {
          pindahFase(DIAM);
          kirim(teksStatus("✅ PENYIRAMAN SELESAI"));
        } else if (putaran >= MAKS_PUTARAN) {
          pindahFase(DIAM);
          modeOtomatis = false;
          kirim(teksStatus("⚠️ PENYIRAMAN DIHENTIKAN") +
                "\n\n<i>Sudah " + String(MAKS_PUTARAN) + " putaran tapi tanah belum " +
                String(BATAS_STOP_SIRAM) + "%. Cek sensor tanah &amp; selang, lalu tekan Auto ON.</i>");
        } else if (airPersen > BATAS_AIR_KRITIS) {
          pompa(true);
          pindahFase(POMPA_AUTO);
        } else {
          pindahFase(DIAM);
        }
      }
      break;

    case POMPA_MANUAL:
      if (sekarang - faseMulai >= LAMA_SIRAM_MANUAL_MS) {
        pompa(false);
        pindahFase(DIAM);
        kirim(teksStatus("✅ SIRAM MANUAL SELESAI"));
      }
      break;
  }

  // --- Tampilkan data di Serial Monitor ---
  if (sekarang - terakhirSerial > INTERVAL_SERIAL_MS) {
    terakhirSerial = sekarang;
    Serial.print("Tanah: ");   Serial.print(tanahPersen);
    Serial.print("% | Air: "); Serial.print(airPersen);
    Serial.print("% | Pompa: "); Serial.print(pompaNyala ? "NYALA" : "MATI");
    Serial.print(" | Auto: "); Serial.print(modeOtomatis ? "ON" : "OFF");
    Serial.print(" | WiFi: "); Serial.println(wifiOK() ? "OK" : "PUTUS");
  }

  delay(50);
}
