#include "rfid.h"
#include "config.h"
#include "settings.h"
#include "model.h"
#include "fetch.h"
#include "ui.h"

#if RFID_ENABLED
#include <SPI.h>
#include <MFRC522.h>  // library "MFRC522" by GithubCommunity

// The reader has its own SPI bus (Arduino's global SPI, FSPI on the S3); the display uses HSPI.
static MFRC522 reader(RFID_SS, RFID_RST);
static bool present = false;
static uint32_t lastPoll = 0, lastTap = 0;
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

void rfid_begin() {
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  reader.PCD_Init();
  byte v = reader.PCD_ReadRegister(MFRC522::VersionReg);
  present = v != 0x00 && v != 0xFF;
  LOG("rfid: %s (version 0x%02X)", present ? "reader found" : "no reader", v);
}

void rfid_loop() {
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
}

#else
void rfid_begin() {}
void rfid_loop() {}
#endif
