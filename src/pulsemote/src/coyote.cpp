// DG-Lab estim box support.
// References:
// https://rezreal.github.io/coyote/web-bluetooth-example.html
// https://github.com/OpenDGLab/OpenDGLab-Connect/blob/master/src/services/DGLab.js

#include <functional>
#include <NimBLEDevice.h>

#include "coyote.h"
#include "coyote-modes.h"

constexpr uint8_t COYOTE_SERVICE_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x0b, 0x18, 0x5a, 0x95 };
constexpr uint8_t CONFIG_CHAR_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x07, 0x15, 0x5a, 0x95 };
constexpr uint8_t POWER_CHAR_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x04, 0x15, 0x5a, 0x95 };
constexpr uint8_t A_CHAR_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x06, 0x15, 0x5a, 0x95 };
constexpr uint8_t B_CHAR_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x05, 0x15, 0x5a, 0x95 };
constexpr uint8_t BATTERY_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x0a, 0x18, 0x5a, 0x95 };
constexpr uint8_t BATTERY_CHAR_UUID[] = { 0xad, 0xe8, 0xf3, 0xd4, 0xb8, 0x84, 0x94, 0xa0, 0xaa, 0xf5, 0xe2, 0x0f, 0x00, 0x15, 0x5a, 0x95 };

NimBLEUUID COYOTE_SERVICE_BLEUUID(COYOTE_SERVICE_UUID, 16, false);
NimBLEUUID CONFIG_CHAR_BLEUUID(CONFIG_CHAR_UUID, 16, false);
NimBLEUUID POWER_CHAR_BLEUUID(POWER_CHAR_UUID, 16, false);
NimBLEUUID A_CHAR_BLEUUID(A_CHAR_UUID, 16, false);
NimBLEUUID B_CHAR_BLEUUID(B_CHAR_UUID, 16, false);
NimBLEUUID BATTERY_SERVICE_BLEUUID(BATTERY_UUID, 16, false);
NimBLEUUID BATTERY_CHAR_BLEUUID(BATTERY_CHAR_UUID, 16, false);

std::map<TimerHandle_t, Coyote*> coyote_timer_map;

CoyoteChannel::CoyoteChannel(Coyote* c, std::string name) : parent(c), channel_name(name) {}

bool Coyote::is_coyote(NimBLEAdvertisedDevice* advertisedDevice) {
  auto ble_manufacturer_specific_data = advertisedDevice->getManufacturerData();
  if (ble_manufacturer_specific_data.length() > 2 && ble_manufacturer_specific_data[1] == 0x19 && ble_manufacturer_specific_data[0] == 0x96) {
    Serial.printf("Found DG-LAB\n");
    return true;
  }
  return false;
}

bool Coyote::get_isconnected() {
  return coyote_connected;
}

// go up or down in steps of 1%, but never above 70% of max
void CoyoteChannel::put_power_diff(short change) {
  if (change > 5) return;  // max 5% in one go, sanity check
  int pc = (parent->coyote_maxPower / 100);
  int max = parent->coyote_maxPower;
  if (change < 0 && coyote_power_wanted < -change * pc)
    return;
  coyote_power_wanted += change * pc;
  if (coyote_power_wanted > max) {
    coyote_power_wanted = max;
  }
}

int CoyoteChannel::get_power_pc() const {
  return int((coyote_power * 100 / parent->coyote_maxPower));
}

// go to a specific percentage. Be careful about this...
void CoyoteChannel::put_power_pc(short power_percent) {
  if (power_percent<0 || power_percent > 100)
    return;
  coyote_power_wanted = (parent->coyote_maxPower * power_percent)/100;

  if (coyote_power_wanted > parent->coyote_maxPower) {
    coyote_power_wanted = parent->coyote_maxPower;
  }
}

uint8_t Coyote::get_batterylevel() {
  return coyote_batterylevel;
}

void CoyoteChannel::put_setmode(coyote_mode mode) {
  wanted_mode = mode;
  wavemode_changed = true;
  Serial.printf("Set WaveMode %s %d\n", channel_name.c_str(), mode);
}

void Coyote::set_callback(coyote_callback c) {
  update_callback = c;
}

void Coyote::notify (coyote_type_of_change change) {
  if ( update_callback )
    update_callback(change);
}

void Coyote::parse_power(const std::vector<uint8_t> buf) {
  // notify/write: 3 bytes: flipFirstAndThirdByte(zero(2) ~ uint(11).as("powerLevelB") ~uint(11).as("powerLevelA")
  channel_a->coyote_power = (buf[2] * 256 + buf[1]) >> 3;
  channel_b->coyote_power = (buf[1] * 256 + buf[0]) & 0b0000011111111111;
  Serial.printf("coyote power A=%d (%d%%) B=%d (%d%%)\n", channel_a->coyote_power, int(channel_a->coyote_power * 100 / coyote_maxPower), channel_b->coyote_power, int(channel_b->coyote_power * 100 / coyote_maxPower));
}

std::vector<uint8_t> Coyote::encode_power(int xpowerA, int xpowerB) {
  std::vector<uint8_t> buf(3);
  // notify/write: 3 bytes: flipFirstAndThirdByte(zero(2) ~ uint(11).as("powerLevelB") ~uint(11).as("powerLevelA")
  buf[2] = (xpowerA & 0b11111100000) >> 5;
  buf[1] = (xpowerA & 0b11111) << 3 | (xpowerB & 0b11100000000) >> 8;
  buf[0] = xpowerB & 0xff;
  Serial.printf("coyote power A=%d B=%d\n", xpowerA, xpowerB);
  return buf;
}

std::vector<uint8_t> Coyote::encode_pattern(coyote_pattern pattern) {
  std::vector<uint8_t> buf(3);
  // flipFirstAndThirdByte(zero(4) ~ uint(5).as("az") ~ uint(10).as("ay") ~ uint(5).as("ax"))
  buf[2] = (pattern.amplitude & 0b11111) >> 1;
  buf[1] = ((pattern.amplitude & 0b1) * 128) | ((pattern.pause_length & 0b1111111000) >> 3);
  buf[0] = (pattern.pulse_length & 0b11111) | ((pattern.pause_length & 0b111) << 5);
  //Serial.printf("encode pattern %02x %02x %02x ax=%d ay=%d az=%d\n", buf[2],buf[1],buf[0],ax,ay,az);
  return buf;
}

coyote_pattern Coyote::parse_pattern(const std::vector<uint8_t> buf) {
  coyote_pattern out;
  // flipFirstAndThirdByte(zero(4) ~ uint(5).as("az") ~ uint(10).as("ay") ~ uint(5).as("ax"))
  out.amplitude = ((buf[2] * 256 + buf[1]) & 0b0000111110000000) >> 7;
  out.pause_length = ((buf[2] * 256 * 256 + buf[1] * 256 + buf[0]) & 0b000000000111111111100000) >> 5;
  out.pulse_length = (buf[0]) & 0b11111;
  //Serial.printf("parse pattern %02x %02x %02x ax=%d ay=%d az=%d\n", buf[2],buf[1],buf[0],*ax,*ay,*az);
  return out;
}

void coyote_timer_callback(TimerHandle_t xTimerID) {
  try {
    auto instance = coyote_timer_map.at(xTimerID);
    instance->timer_callback(xTimerID);
  } catch ( const std::out_of_range& e ) {
    Serial.println("Could not find timer callback instance");
  }
}

void CoyoteChannel::update_pattern() {
  if ((wavemode == M_NONE || waveclock == 0) && (wanted_mode != wavemode)) {
    wavemode = wanted_mode;
    waveclock = 0;
    cyclecount = 0;
    parent->notify(C_WAVEMODE_A);
  }

  if (coyote_power > 0 && wavemode != M_NONE) {
    switch ( wavemode ) {
      case M_BREATH:
        pattern = coyote_mode_breath(waveclock, cyclecount);
        break;
      case M_WAVES:
        pattern = coyote_mode_waves(waveclock, cyclecount);
        break;
    }
  }
}

// This is called every 100mS to provide the coyote box with what to do next
//
void Coyote::timer_callback(TimerHandle_t xTimerID) {
  if (!coyote_connected)
    return;

  // this sets the power to where we want it (also stop you changing it on the rocker switches)
  if (channel_a->coyote_power != channel_a->coyote_power_wanted || channel_b->coyote_power != channel_b->coyote_power_wanted) {
    auto enc = encode_power(channel_a->coyote_power_wanted, channel_b->coyote_power_wanted);
    if (!powerCharacteristic->writeValue(enc))
      Serial.printf("Failed to write power %u %u %u\n", enc[0], enc[1], enc[2]);
  }
  // Do WaveA
  channel_a->update_pattern();
  if ( channel_a->coyote_power > 0 && channel_a->wavemode != M_NONE ) {
    auto enc = encode_pattern(channel_a->pattern);
    if (!patternACharacteristic->writeValue(enc))
      Serial.println("Failed to write pattern A");
  }

  // Do WaveB
  channel_b->update_pattern();
  if ( channel_b->coyote_power > 0 && channel_b->wavemode != M_NONE ) {
    auto enc = encode_pattern(channel_b->pattern);
    if (!patternBCharacteristic->writeValue(enc))
      Serial.println("Failed to write pattern B");
  }
}

void Coyote::connected_callback() {
    Serial.println("Client onConnect");
}

void Coyote::disconnected_callback() {
    coyote_connected = false;
    xTimerStop(coyoteTimer, 0);
    bleClient->deleteServices();
    //NimBLEDevice::deleteClient(bleClient);
    //bleClient = nullptr;
    Serial.println("Client onDisconnect");
    notify(C_DISCONNECTED);
}

class CoyoteNimBLEClientCallback : public NimBLEClientCallbacks {
public:
  CoyoteNimBLEClientCallback(Coyote* instance) {
    coyote_instance = instance;
  }

  void onConnect(NimBLEClient* pclient) {
    coyote_instance->connected_callback();
  }

  void onDisconnect(NimBLEClient* pclient) {
    coyote_instance->disconnected_callback();
  }

private:
  Coyote* coyote_instance;
};

bool Coyote::getService(NimBLERemoteService*& service, NimBLEUUID uuid) {
  Serial.printf("Getting service %s\n", uuid.toString().c_str());
  service = bleClient->getService(uuid);
  if (service == nullptr) {
    Serial.print("Failed to find service UUID: ");
    Serial.println(uuid.toString().c_str());
    return false;
  }
  return true;
}

bool getCharacteristic(NimBLERemoteService* service, NimBLERemoteCharacteristic*& c, NimBLEUUID uuid, notify_callback notifyCallback = nullptr) {
  Serial.printf("Getting characteristic %s\n", uuid.toString().c_str());
  c = service->getCharacteristic(uuid);
  if (c == nullptr) {
    Serial.print("Failed to find characteristic UUID: ");
    Serial.println(uuid.toString().c_str());
    return false;
  }

  if (!notifyCallback)
    return true;

  // we want notifications
  if (c->canNotify() && c->subscribe(true, notifyCallback))
    return true;
  else {
    Serial.print("Failed to register for notifications for characteristic UUID: ");
    Serial.println(uuid.toString().c_str());
    return false;
  }
}

void Coyote::batterylevel_callback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  coyote_batterylevel = data[0];
  Serial.printf("coyote batterycb: %d\n", coyote_batterylevel);
}

void Coyote::power_callback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  if (length >= 3) {
    parse_power({data, data+length});
    notify(C_POWER);
  }
  else
    Serial.println("Power callback with incorrect length");
}

Coyote::Coyote() {
  channel_a = std::unique_ptr<CoyoteChannel>(new CoyoteChannel(this, "a"));
  channel_b = std::unique_ptr<CoyoteChannel>(new CoyoteChannel(this, "b"));
}

Coyote::~Coyote() {
  if ( coyoteTimer ) {
    coyote_timer_map.erase(coyoteTimer);
    xTimerDelete(coyoteTimer, 100);
  }
  bleClient->deleteServices(); // deletes all services, which should delete all characteristics
  NimBLEDevice::deleteClient(bleClient); // will also disconnect
}

bool Coyote::connect_to_device(NimBLEAdvertisedDevice* coyote_device) {
  if (coyote_connected)
    return false;

  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new CoyoteNimBLEClientCallback(this));
  }

  notify(C_CONNECTING);

  Serial.printf("Will try to connect to Coyote at %s\n", coyote_device->getAddress().toString().c_str());
  if (!bleClient->connect(coyote_device)) {
    Serial.println("Connection failed");
    return false;
  }
  Serial.println("Connection established");

  bool res = true;
  res &= getService(coyoteService, COYOTE_SERVICE_BLEUUID);
  res &= getService(batteryService, BATTERY_SERVICE_BLEUUID);

  if (res == false) {
    Serial.println("Missing service");
    bleClient->disconnect();
    return false;
  }
  
  res &= getCharacteristic(batteryService, batteryLevelCharacteristic, BATTERY_CHAR_BLEUUID, std::bind(&Coyote::batterylevel_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
  res &= getCharacteristic(coyoteService, configCharacteristic, CONFIG_CHAR_BLEUUID);
  res &= getCharacteristic(coyoteService, powerCharacteristic, POWER_CHAR_BLEUUID, std::bind(&Coyote::power_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
  res &= getCharacteristic(coyoteService, patternACharacteristic, A_CHAR_BLEUUID);
  res &= getCharacteristic(coyoteService, patternBCharacteristic, B_CHAR_BLEUUID);

  if (res == false) {
    Serial.println("Missing characteristic");
    bleClient->disconnect();
    return false;
  }

  Serial.println("Found services and characteristics");
  coyote_connected = true;

  coyote_batterylevel = batteryLevelCharacteristic->readValue<uint8_t>();
  Serial.printf("Battery level: %u\n", coyote_batterylevel);

  auto configData = configCharacteristic->readValue();
  coyote_maxPower = (configData[2] & 0xf) * 256 + configData[1];
  // scale the max power to what we want it to be...
  Serial.printf("coyote maxPower: %d\n", coyote_maxPower);
  coyote_maxPower *= (double)coyote_max_power_percent/100;
  Serial.printf("coyote maxPower clamped to: %d\n", coyote_maxPower);
  coyote_powerStep = configData[0];
  Serial.printf("coyote powerStep: %d\n", coyote_powerStep);

  auto powerData = powerCharacteristic->readValue();
  if (powerData.size() >= 3)
    parse_power({powerData.begin(), powerData.end()});

  Serial.printf("read pattern a\n");
  auto patternAData = patternACharacteristic->readValue();
  Serial.printf("read pattern b\n");
  auto patternBData = patternBCharacteristic->readValue();
  if (patternAData.size() < 3 || patternBData.size() < 3) {
    Serial.println("No pattern data?");
  }
  channel_a->pattern = parse_pattern({patternAData.begin(), patternAData.end()});
  channel_b->pattern = parse_pattern({patternBData.begin(), patternBData.end()});

  auto buf = encode_power(start_power, start_power);
  if (!powerCharacteristic->writeValue(buf))
    Serial.println("Failed to write powerCharacteristic");

  channel_a->wavemode = M_NONE;
  channel_b->wavemode = M_NONE;

  if (!coyoteTimer) {
    coyoteTimer = xTimerCreate(nullptr, pdMS_TO_TICKS(100), true, nullptr, coyote_timer_callback);
    coyote_timer_map[coyoteTimer] = this;
  }
  xTimerStart(coyoteTimer, 0);

  Serial.println("coyote connected");
  notify(C_CONNECTED);
  return true;
}