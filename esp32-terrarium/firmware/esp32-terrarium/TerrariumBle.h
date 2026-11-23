#pragma once

#include <BLEDevice.h>
#include <BLESecurity.h>
#include <esp_ota_ops.h>
#include <esp_random.h>

namespace TerrariumBle {
constexpr const char* serviceId = "6bdc5100-b75f-462a-a377-8dcecc0d415d";
constexpr const char* quotaId = "6bdc5101-b75f-462a-a377-8dcecc0d415d";
constexpr const char* logId = "6bdc5102-b75f-462a-a377-8dcecc0d415d";
constexpr const char* updateId = "6bdc5103-b75f-462a-a377-8dcecc0d415d";
constexpr const char* progressId = "6bdc5104-b75f-462a-a377-8dcecc0d415d";
constexpr uint32_t codeLifetime = 60000, idleTimeout = 15000;
constexpr size_t packetSize = 180;

struct Packet {
  char kind;
  uint16_t size;
  uint8_t data[packetSize];
};

QueueHandle_t inbox = nullptr;
BLECharacteristic *logs = nullptr, *progress = nullptr;
uint32_t updateCode = 0, armedAt = 0, lastPacket = 0, rebootAt = 0;
uint32_t imageSize = 0, written = 0;
esp_ota_handle_t updateHandle = 0;
const esp_partition_t* target = nullptr;

class Receiver : public BLECharacteristicCallbacks {
public:
  explicit Receiver(char kind) : kind(kind) {}
  void onWrite(BLECharacteristic* characteristic) override {
    const String value = characteristic->getValue();
    if (!value.length() || value.length() > packetSize) return;
    Packet packet{};
    packet.kind = kind;
    packet.size = value.length();
    memcpy(packet.data, value.c_str(), packet.size);
    xQueueSend(inbox, &packet, 0);
  }
private:
  const char kind;
};

class Connection : public BLEServerCallbacks {
  void onDisconnect(BLEServer*) override {
    Packet packet{};
    packet.kind = 'D';
    xQueueSend(inbox, &packet, 0);
    BLEDevice::startAdvertising();
  }
};

void begin() {
  inbox = xQueueCreate(8, sizeof(Packet));
  if (!inbox || !BLEDevice::init("Terrarium")) {
    Serial.println("BLE INIT FAILED");
    return;
  }
  BLESecurity::setCapability(ESP_IO_CAP_NONE);
  BLESecurity::setAuthenticationMode(true, false, true);
  BLEDevice::setMTU(247);
  auto* server = BLEDevice::createServer();
  server->setCallbacks(new Connection());
  auto* service = server->createService(BLEUUID(serviceId), 16);
  auto* quota = service->createCharacteristic(quotaId, BLECharacteristic::PROPERTY_WRITE);
  quota->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  quota->setCallbacks(new Receiver('Q'));
  logs = service->createCharacteristic(logId, BLECharacteristic::PROPERTY_READ);
  logs->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  auto* update = service->createCharacteristic(updateId, BLECharacteristic::PROPERTY_WRITE);
  update->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  update->setCallbacks(new Receiver('U'));
  progress = service->createCharacteristic(progressId, BLECharacteristic::PROPERTY_READ);
  progress->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  progress->setValue("IDLE");
  service->start();
  auto* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(serviceId);
  advertising->setScanResponse(true);
  advertising->start();
  Serial.println("BLE READY");
}

void arm(uint32_t now) {
  if (!progress || target || rebootAt) return;
  updateCode = 100000 + esp_random() % 900000;
  armedAt = now;
  progress->setValue("ARMED");
}

void cancel(const char* reason) {
  if (target) esp_ota_abort(updateHandle);
  target = nullptr;
  progress->setValue(reason);
}

void update(const Packet& packet, uint32_t now) {
  if (rebootAt) return;
  const uint8_t* data = packet.data;
  if (data[0] == 1 && packet.size == 9 && !target) {
    uint32_t code;
    memcpy(&code, data + 1, sizeof(code));
    memcpy(&imageSize, data + 5, sizeof(imageSize));
    if (!updateCode || code != updateCode || now - armedAt >= codeLifetime) {
      progress->setValue("ERR CODE");
      return;
    }
    updateCode = 0;
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!next || !imageSize || imageSize > next->size ||
        esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &updateHandle) != ESP_OK) {
      progress->setValue("ERR BEGIN");
      return;
    }
    target = next;
    written = 0;
    lastPacket = now;
    progress->setValue("BYTES 0");
  } else if (data[0] == 2 && packet.size > 5 && target) {
    uint32_t offset;
    memcpy(&offset, data + 1, sizeof(offset));
    const size_t count = packet.size - 5;
    if (offset != written || count > imageSize - written ||
        esp_ota_write(updateHandle, data + 5, count) != ESP_OK) {
      cancel("ERR WRITE");
      return;
    }
    written += count;
    lastPacket = now;
    progress->setValue(String("BYTES ") + written);
  } else if (data[0] == 3 && packet.size == 1 && target) {
    if (written != imageSize) {
      cancel("ERR SIZE");
      return;
    }
    const esp_partition_t* next = target;
    target = nullptr;
    if (esp_ota_end(updateHandle) != ESP_OK || esp_ota_set_boot_partition(next) != ESP_OK) {
      progress->setValue("ERR IMAGE");
      return;
    }
    progress->setValue("REBOOT");
    rebootAt = now;
  }
}

void tick(uint32_t now) {
  if (!progress) return;
  if (updateCode && now - armedAt >= codeLifetime) updateCode = 0;
  if (target && now - lastPacket >= idleTimeout) cancel("ERR TIMEOUT");
  if (rebootAt && now - rebootAt >= 2000) ESP.restart();
  Packet packet;
  for (int handled = 0; handled < 4 && xQueueReceive(inbox, &packet, 0); ++handled) {
    if (packet.kind == 'U') {
      update(packet, now);
    } else if (packet.kind == 'D' && target) {
      cancel("ERR DISCONNECTED");
    } else if (packet.kind == 'Q' && packet.size < 48) {
      packet.data[packet.size] = 0;
      if (acceptQuota(reinterpret_cast<const char*>(packet.data), now)) {
        Serial.println("BLE QUOTA OK");
      } else if (acceptClock(reinterpret_cast<const char*>(packet.data))) {
        Serial.println("BLE TIME OK");
      } else if (acceptResets(reinterpret_cast<const char*>(packet.data))) {
        Serial.println("BLE RESET OK");
      }
    }
  }
}

void publishStatus() {
  if (!logs) return;
  const auto* running = esp_ota_get_running_partition();
  char value[180];
  snprintf(value, sizeof(value), "%s | %s | day=%d water=%d weather=%d bat=%d charge=%d quota=%d/%d clock=%d screen=%s reset=%lld/%lld page=%s",
      running ? running->label : "?", __TIME__, day, water, weather,
      batteryPercent, charging, quotaRemaining[0], quotaRemaining[1], clockSynced, screenOff ? "off" : "on",
      (long long)quotaResets[0], (long long)quotaResets[1], quotaPage ? "codex" : "bottle");
  logs->setValue(value);
}

void drawUpdate() {
  if (!updateCode && !target && !rebootAt) return;
  screen.fillRoundRect(52, 96, 136, 46, 5, rgb(26, 38, 32));
  screen.drawRoundRect(52, 96, 136, 46, 5, rgb(145, 172, 116));
  screen.setTextColor(rgb(217, 214, 177));
  screen.setCursor(68, 106);
  screen.print("BLUETOOTH UPDATE");
  screen.setCursor(68, 124);
  if (updateCode) screen.printf("CODE %06lu", (unsigned long)updateCode);
  else if (target) screen.printf("WRITING %lu%%", (unsigned long)(100 * written / imageSize));
  else screen.print("RESTARTING");
}
}  // namespace TerrariumBle
