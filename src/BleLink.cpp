#include "BleLink.h"

#include <NimBLEDevice.h>

#include <algorithm>

BleLink Ble;

namespace {

const char* kServiceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const char* kRxUuid      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // celular -> ESP32
const char* kTxUuid      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // ESP32 -> celular

constexpr size_t kMaxLine = 4096;
constexpr uint16_t kNoConn = 0xFFFF;

NimBLEServer* gServer = nullptr;
NimBLECharacteristic* gTx = nullptr;
BleLink::LineHandler gOnLine;
std::string gRx;
volatile bool gConnected = false;
volatile uint16_t gConnHandle = kNoConn;
SemaphoreHandle_t gTxMutex = nullptr;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    gConnHandle = desc->conn_handle;
    gConnected = true;
    gRx.clear();
    // Intervalo 15–30 ms: respostas chegam rápido sem gastar muito rádio
    server->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
    Serial.println("[BLE] celular conectado");
  }

  void onDisconnect(NimBLEServer* server) override {
    gConnected = false;
    gConnHandle = kNoConn;
    Serial.println("[BLE] celular desconectado — anunciando novamente");
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
    Serial.printf("[BLE] MTU = %u\n", mtu);
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    auto value = c->getValue();
    const char* data = reinterpret_cast<const char*>(value.data());
    size_t len = value.length();
    for (size_t i = 0; i < len; i++) {
      char ch = data[i];
      if (ch == '\n') {
        if (!gRx.empty() && gOnLine) gOnLine(std::move(gRx));
        gRx.clear();
      } else if (ch != '\r' && gRx.size() < kMaxLine) {
        gRx += ch;
      }
    }
  }
};

}  // namespace

void BleLink::begin(const char* deviceName, LineHandler onLine) {
  gOnLine = std::move(onLine);
  gTxMutex = xSemaphoreCreateMutex();

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new ServerCallbacks());

  NimBLEService* service = gServer->createService(kServiceUuid);
  gTx = service->createCharacteristic(kTxUuid, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* rx = service->createCharacteristic(
      kRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  service->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kServiceUuid);
  adv->setScanResponse(true);  // nome do dispositivo vai na scan response
  adv->start();
  Serial.printf("[BLE] anunciando como \"%s\"\n", deviceName);
}

bool BleLink::send(const std::string& line) {
  if (!gConnected || !gTx) return false;

  xSemaphoreTake(gTxMutex, portMAX_DELAY);
  std::string data = line;
  data += '\n';

  uint16_t mtu = gServer->getPeerMTU(gConnHandle);
  if (mtu < 23) mtu = 23;
  size_t chunk = std::min<size_t>(mtu - 3, 244);

  for (size_t i = 0; i < data.size() && gConnected; i += chunk) {
    size_t n = std::min(chunk, data.size() - i);
    gTx->setValue(reinterpret_cast<const uint8_t*>(data.data()) + i, n);
    gTx->notify();
    vTaskDelay(pdMS_TO_TICKS(12));  // evita estourar os buffers do stack BLE
  }
  xSemaphoreGive(gTxMutex);
  return true;
}

bool BleLink::isConnected() const { return gConnected; }
