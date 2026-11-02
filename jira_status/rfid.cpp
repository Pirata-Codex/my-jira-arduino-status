#include "rfid.h"
#include "config.h"
#include "settings.h"
#include "model.h"
#include "fetch.h"
#include "ui.h"
#include "led.h"

#if RFID_ENABLED
#include <SPI.h>
#include <MFRC522.h>  // library "MFRC522" by GithubCommunity
#include <driver/gpio.h>

// The reader has its own SPI bus (Arduino's global SPI, FSPI on the S3); the display uses HSPI.
static MFRC522 reader(RFID_SS, RFID_RST);
static bool present = false;
static uint32_t lastPoll = 0, lastTap = 0, lastCheck = 0;
static String lastUid;

static String uidHex(const MFRC522::Uid& u) {
  String s;
  for (byte i = 0; i < u.size; i++) {
    if (u.uidByte[i] < 0x10) s += '0';
    s += String(u.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Your badges: settings field "Badge ids", comma or space separated.
static bool isMine(const String& uid) {
  String list = gCfg.rfidUids;
  list.toUpperCase();
  list.replace(" ", ",");
  return (list.length() && ("," + list + ",").indexOf("," + uid + ",") >= 0);
}

// A reader that isn't there answers 0x00 or 0xFF (MISO is pulled up, so an open pogo pin reads 0xFF).
static bool answers() {
  byte v = reader.PCD_ReadRegister(MFRC522::VersionReg);
  return v != 0x00 && v != 0xFF;
}

// Every 2 s: a missing reader is looked for again (the board was docked), a present one is asked
// for its version (the board was lifted out).
static void checkReader() {
  if (lastCheck && millis() - lastCheck < 2000) return;
  lastCheck = millis();
  if (present) {
    if (answers()) return;
    present = false;
    LOG("rfid: reader gone (undocked)");
    return;
  }
  reader.PCD_Init();
  if (!answers()) return;
  present = true;
  LOG("rfid: reader found (docked), version 0x%02X", reader.PCD_ReadRegister(MFRC522::VersionReg));
}

void rfid_begin() {
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  gpio_pullup_en((gpio_num_t)RFID_MISO);  // keeps the SPI function, adds the pull-up
  checkReader();
  if (!present) LOG("rfid: no reader yet (not docked)");
}

bool rfid_present() { return present; }

void rfid_loop() {
  checkReader();
  if (!present || millis() - lastPoll < RFID_POLL_MS) return;
  lastPoll = millis();
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) return;
  String uid = uidHex(reader.uid);
  reader.PICC_HaltA();
  // A card held on the reader is read again and again: one tap per badge every few seconds.
  if (uid == lastUid && millis() - lastTap < RFID_REPEAT_MS) return;
  lastUid = uid;
  lastTap = millis();
  if (!isMine(uid)) {
    LOG("rfid: badge %s is not in the settings (Badge ids)", uid.c_str());
    ui_toast("UNKNOWN BADGE " + uid, 6000);
    led_notify(LED_AMBER, LED_CROSS);
    return;
  }
  // Arriving or leaving: whatever the board shows now, the tap flips between IN and LEFT.
  model_lock();
  String code = gData.status.code;
  model_unlock();
  bool here = code.isEmpty() || code == "IN";
  const char* next = here ? "LEFT" : "IN";
  LOG("rfid: badge %s, %s -> %s", uid.c_str(), code.length() ? code.c_str() : "(none)", next);
  fetch_set_status(next);
  ui_toast(here ? "SEE YOU, " + gCfg.ownerName : "WELCOME BACK, " + gCfg.ownerName, 5000);
  led_notify(here ? LED_VIOLET : LED_LIME, here ? LED_FILL : LED_CHECK);
}

#else
void rfid_begin() {}
void rfid_loop() {}
bool rfid_present() { return false; }
#endif
