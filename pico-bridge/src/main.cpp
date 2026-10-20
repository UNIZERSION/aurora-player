#include <Arduino.h>
#include <Preferences.h>
#include <BluetoothA2DPSource.h>
#include <driver/i2s.h>
#include <esp_gap_bt_api.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <atomic>
#include <algorithm>
#include <climits>
#include <cmath>
#include "UseLease.hpp"
#include "RemoteControl.hpp"
#include "PcmGain.hpp"
#include "CommandLine.hpp"
#include "StreamPreroll.hpp"
#include "StreamHealth.hpp"
#ifdef PICO_NATIVE_IDF
#include "airpods.h"
#define PICO_VERSION "0.9.5"
#define PICO_AIRPODS_CAP 1
#else
#define PICO_VERSION "0.9.3"
#define PICO_AIRPODS_CAP 0
#endif

// Tab5 is I2S master: BCLK=26, WS=25, DIN=22. UART RX=32, TX=33.
// Bluetooth source consumes 44.1 kHz stereo 16-bit PCM from a bounded ring.
BluetoothA2DPSource bt;
A2DPNoVolumeControl unmodifiedPcm;
Preferences prefs;
constexpr size_t maxSaved = 8;
struct SavedDevice { uint8_t mac[6]; char name[80]; };
SavedDevice saved[maxSaved] = {};
size_t savedCount = 0;
size_t selectedSaved = 0;
bool bondImportDone = false;
uint32_t lastBondImport = 0;
struct Device { uint8_t mac[6]; char name[249]; int rssi; };
Device devices[24];
size_t deviceCount = 0;
portMUX_TYPE deviceLock = portMUX_INITIALIZER_UNLOCKED;
uint8_t target[6] = {};
bool hasTarget = false;
bool manualScan = false;
uint32_t manualScanStarted = 0;
bool bluetoothRunning = false, scanFinished = false;
UseLease useLease;
RTC_NOINIT_ATTR uint32_t restartIntent;
constexpr uint32_t resumeIntent = 0x50494341, sleepIntent = 0x50494353;
uint32_t volumeSaveDue = 0;
bool volumeDirty = false;
std::atomic<uint32_t> discoveryRevision{0};

void saveVolume();
void sleepNow();
void restartActive();
void startBluetooth();

int savedIndex(const uint8_t mac[6]) {
  for (size_t i = 0; i < savedCount; ++i) if (!memcmp(saved[i].mac, mac, 6)) return int(i);
  return -1;
}

bool savePeers() {
  if (savedCount && prefs.putBytes("peers", saved, savedCount * sizeof(SavedDevice)) != savedCount * sizeof(SavedDevice)) return false;
  if (!savedCount) {
    prefs.remove("peers"); prefs.remove("selected"); prefs.remove("target"); prefs.remove("name");
    hasTarget = false;
    return true;
  }
  prefs.putUChar("selected", uint8_t(selectedSaved));
  // Keep the old single-device keys so v0.5.0 can still be restored.
  prefs.putBytes("target", saved[selectedSaved].mac, 6);
  prefs.putString("name", saved[selectedSaved].name);
  memcpy(target, saved[selectedSaved].mac, 6);
  hasTarget = true;
  return true;
}

void loadPeers() {
  const size_t bytes = prefs.getBytesLength("peers");
  if (bytes && bytes <= sizeof(saved) && bytes % sizeof(SavedDevice) == 0 && prefs.getBytes("peers", saved, bytes) == bytes) {
    savedCount = bytes / sizeof(SavedDevice);
    for (size_t i = 0; i < savedCount; ++i) saved[i].name[sizeof(saved[i].name)-1] = '\0';
    selectedSaved = std::min<size_t>(prefs.getUChar("selected", 0), savedCount - 1);
  } else if (prefs.getBytesLength("target") == 6) {
    prefs.getBytes("target", saved[0].mac, 6);
    const String oldName = prefs.getString("name", "Zapisane urzadzenie");
    snprintf(saved[0].name, sizeof(saved[0].name), "%s", oldName.c_str());
    savedCount = 1;
    selectedSaved = 0;
    savePeers();
  }
  if (savedCount) { memcpy(target, saved[selectedSaved].mac, 6); hasTarget = true; }
}

void importBonds() {
  if (!bluetoothRunning || bondImportDone || millis() - lastBondImport < 3000) return;
  lastBondImport = millis();
  if (!hasTarget) { bondImportDone = true; return; }
  const int bonded = esp_bt_gap_get_bond_device_num();
  if (bonded < 0) return; // Bluetooth stack is not ready; retry later.
  bondImportDone = true;
  if (!bonded || savedCount >= maxSaved) return;
  esp_bd_addr_t bonds[16] = {};
  int count = std::min(bonded, 16);
  if (esp_bt_gap_get_bond_device_list(&count, bonds) != ESP_OK) { bondImportDone = false; return; }
  bool changed = false;
  for (int i = 0; i < count && savedCount < maxSaved; ++i) {
    if (savedIndex(bonds[i]) >= 0) continue;
    SavedDevice &entry = saved[savedCount++];
    memcpy(entry.mac, bonds[i], 6);
    snprintf(entry.name, sizeof(entry.name), "Sparowane %02X:%02X:%02X:%02X:%02X:%02X",
      bonds[i][0], bonds[i][1], bonds[i][2], bonds[i][3], bonds[i][4], bonds[i][5]);
    changed = true;
  }
  if (changed) { savePeers(); Serial.printf("Imported %u saved Bluetooth devices\n", unsigned(savedCount)); }
}
remote::Inbox remoteInbox;
std::atomic<uint32_t> remotePresses{0},remoteReleases{0},remoteAccepted{0};
std::atomic<unsigned> lastRemoteKey{0},lastRemoteRelease{0};
portMUX_TYPE remoteLock = portMUX_INITIALIZER_UNLOCKED;
static_assert(ESP_AVRC_PT_CMD_PLAY == 0x44 && ESP_AVRC_PT_CMD_STOP == 0x45 &&
              ESP_AVRC_PT_CMD_PAUSE == 0x46 && ESP_AVRC_PT_CMD_FORWARD == 0x4B &&
              ESP_AVRC_PT_CMD_BACKWARD == 0x4C && ESP_AVRC_PT_CMD_STATE_RELEASED == 1,
              "Remote operation IDs must match this SDK");

std::atomic<int> connection{ESP_A2D_CONNECTION_STATE_DISCONNECTED};
std::atomic<int> audioState{ESP_A2D_AUDIO_STATE_STOPPED};
// The UART protocol predates native IDF: 2 means started, 0 means inactive.
// IDF 5.x changed STARTED from 2 to 1; never expose SDK enum values on UART.
constexpr int protocolAudioState(int state) {
  return state == ESP_A2D_AUDIO_STATE_STARTED ? 2 : 0;
}
std::atomic<uint32_t> toneRequest{0};
std::atomic<bool> abortTone{true};
std::atomic<bool> toneFinished{false}, testPending{false};
std::atomic<uint32_t> toneFinishedRequest{0};
std::atomic<bool> streamEnabled{false};
std::atomic<bool> toneRunning{false}, hasStreamToken{false};
std::atomic<uint32_t> streamToken{0};
std::atomic<int> i2sError{ESP_OK};
std::atomic<uint32_t> i2sBlocks{0};
std::atomic<uint32_t> receivedBlocks{0}, audioCallbacks{0}, consumedFrames{0};
std::atomic<uint32_t> receivedAt{0}, callbackAt{0};
std::atomic<uint32_t> recoveredStreams{0}, inputStalls{0}, sdkFlushes{0};
StreamHealth streamHealth;
std::atomic<uint32_t> droppedFrames{0};
std::atomic<uint32_t> emptyFrames{0};
std::atomic<uint32_t> streamUnderruns{0};
std::atomic<uint32_t> ringEpoch{0};
std::atomic<int> volumePercent{31};
constexpr size_t ringCapacity = 4096;
Frame ring[ringCapacity];
size_t ringRead = 0, ringWrite = 0, ringUsed = 0;
portMUX_TYPE ringLock = portMUX_INITIALIZER_UNLOCKED;

void saveVolume() {
  if (volumeDirty) { prefs.putUChar("volume", volumePercent.load()); volumeDirty = false; }
}

void deepStandby() {
  // UART idle is HIGH. Wake on the opposite level of the existing G32 wire.
  // The player holds its TX LOW briefly before sending USE; UART2 itself
  // cannot wake ESP32, but G32 is an RTC GPIO and supports EXT0 wakeup.
  Serial.flush(); Serial2.flush(); Serial2.end();
  rtc_gpio_init(GPIO_NUM_32);
  rtc_gpio_set_direction(GPIO_NUM_32, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis(GPIO_NUM_32); rtc_gpio_pullup_en(GPIO_NUM_32);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_32, rtc_gpio_get_level(GPIO_NUM_32) ? 0 : 1);
  esp_deep_sleep_start();
}

void sleepNow() {
  saveVolume(); abortTone.store(true); streamEnabled.store(false);
  Serial2.println("STANDBY"); Serial2.flush();
  // A clean reset shuts down A2DP/I2S without waiting on a discovery task.
  // The following boot enters sleep before starting either peripheral.
  restartIntent = sleepIntent; ESP.restart();
}

void restartActive() {
  saveVolume(); restartIntent = resumeIntent;
  Serial2.flush(); Serial.flush(); delay(30); ESP.restart();
}

void clearRing() {
  portENTER_CRITICAL(&ringLock);
  ringRead = ringWrite = ringUsed = 0;
  ringEpoch.fetch_add(1);
  portEXIT_CRITICAL(&ringLock);
}

void i2sReader(void *) {
  Frame block[256];
  for (;;) {
    // A blocking DMA read can straddle STOP/STREAM, reconnect or an A2DP
    // restart. Tag it before waiting; only its original live session may
    // publish those samples. Checking just streamEnabled after read is unsafe.
    const uint32_t readEpoch = ringEpoch.load();
    const bool readEnabled = streamEnabled.load() &&
        audioState.load() == ESP_A2D_AUDIO_STATE_STARTED;
    size_t bytes = 0;
    const esp_err_t result = i2s_read(I2S_NUM_0, block, sizeof(block), &bytes,
                                      pdMS_TO_TICKS(1000));
    if (result != ESP_OK) { i2sError.store(result); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    if (!bytes) continue;
    receivedBlocks.fetch_add(1, std::memory_order_relaxed);
    receivedAt.store(millis(), std::memory_order_relaxed);
    i2sError.store(ESP_OK);
    const size_t count = bytes / sizeof(Frame);
    portENTER_CRITICAL(&ringLock);
    if (!readEnabled || readEpoch != ringEpoch.load() ||
        !streamEnabled.load() || audioState.load() != ESP_A2D_AUDIO_STATE_STARTED) {
      portEXIT_CRITICAL(&ringLock);
      continue;
    }
    i2sBlocks.fetch_add(1);
    const size_t dropped = count > ringCapacity - ringUsed ? count - (ringCapacity - ringUsed) : 0;
    ringRead = (ringRead + dropped) % ringCapacity; ringUsed -= dropped;
    const size_t first = std::min(count, ringCapacity - ringWrite);
    memcpy(ring + ringWrite, block, first * sizeof(Frame));
    if (count > first) memcpy(ring, block + first, (count - first) * sizeof(Frame));
    ringWrite = (ringWrite + count) % ringCapacity; ringUsed += count;
    portEXIT_CRITICAL(&ringLock);
    if (dropped) droppedFrames.fetch_add(dropped);
  }
}

bool startI2s() {
  i2s_config_t config = {};
  config.mode = i2s_mode_t(I2S_MODE_SLAVE | I2S_MODE_RX);
  config.sample_rate = 44100;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  const esp_err_t installed = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  if (installed != ESP_OK) { i2sError.store(installed); return false; }
  i2s_pin_config_t pins = {};
  pins.bck_io_num = 26;
  pins.ws_io_num = 25;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = 22;
  const esp_err_t assigned = i2s_set_pin(I2S_NUM_0, &pins);
  if (assigned != ESP_OK) { i2sError.store(assigned); return false; }
  if (xTaskCreatePinnedToCore(i2sReader, "i2s-reader", 4096, nullptr, 2,
                              nullptr, 0) != pdPASS) {
    i2sError.store(ESP_ERR_NO_MEM); return false;
  }
  return true;
}

bool found(const char *name, esp_bd_addr_t mac, int rssi) {
  portENTER_CRITICAL(&deviceLock);
  size_t i = 0;
  for (; i < deviceCount; ++i) if (!memcmp(devices[i].mac, mac, 6)) break;
  if (i < 24) {
    if (i == deviceCount) ++deviceCount;
    memcpy(devices[i].mac, mac, 6);
    snprintf(devices[i].name, sizeof(devices[i].name), "%s", name ? name : "(no name)");
    for(char &c:devices[i].name)if(c && uint8_t(c)<32)c=' ';
    devices[i].rssi = rssi;
  }
  portEXIT_CRITICAL(&deviceLock);
  discoveryRevision.fetch_add(1);
  // Never select the first available device. Only the user-selected MAC matches.
  return hasTarget && !manualScan && !scanFinished && !memcmp(target, mac, 6);
}

void connectionChanged(esp_a2d_connection_state_t state, void *) {
  portENTER_CRITICAL(&remoteLock);
  remoteInbox.connected(state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  portEXIT_CRITICAL(&remoteLock);
  connection.store(state);
  hasStreamToken.store(false);
  abortTone.store(true);
  // Preserve compatibility; Tab5 sends STREAM/STOP when playback changes.
  streamEnabled.store(state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  bt.set_media_active(state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  clearRing();
}
void remoteKey(uint8_t key, bool released) {
  portENTER_CRITICAL(&remoteLock);
  const bool accepted = remoteInbox.key(key, released, millis());
  portEXIT_CRITICAL(&remoteLock);
  lastRemoteKey.store(key);lastRemoteRelease.store(released ? 1u : 0u);
  if(released)remoteReleases.fetch_add(1);else remotePresses.fetch_add(1);
  if(accepted)remoteAccepted.fetch_add(1);
}
void drainRemote() {
  for (size_t i = 0; i < remote::Inbox::capacity; ++i) {
    portENTER_CRITICAL(&remoteLock);
    const auto action = remoteInbox.pop(millis());
    portEXIT_CRITICAL(&remoteLock);
    if (action == remote::Action::None) break;
    Serial2.println(remote::line(action)); Serial.println(remote::line(action));
  }
}
void audioChanged(esp_a2d_audio_state_t state, void *) {
  audioState.store(state);
  clearRing();
  if (state == ESP_A2D_AUDIO_STATE_STARTED) {
    droppedFrames.store(0);
    emptyFrames.store(0);
    streamUnderruns.store(0);
  }
  if (state != ESP_A2D_AUDIO_STATE_STARTED) abortTone.store(true);
}

int32_t audio(Frame *frames, int32_t count) {
  if (count < 0) { clearRing(); sdkFlushes.fetch_add(1); return 0; }
  if (!frames || count <= 0) return 0;
  audioCallbacks.fetch_add(1, std::memory_order_relaxed);
  callbackAt.store(millis(), std::memory_order_relaxed);
  static uint32_t seenRequest = 0;
  static uint32_t position = 0;
  static bool active = false;
  const uint32_t request = toneRequest.load();
  if (request != seenRequest) {
    seenRequest = request;
    position = 0;
    active = true;
  }
  if (abortTone.load()) active = false;
  toneRunning.store(active);
  const bool wasPlayingTest = active;
  constexpr uint32_t rate = 44100;
  // 3 seconds maximum, left / silence / right. Peak ~ -26 dBFS,
  // plus Bluetooth/software volume attenuation. Short ramps prevent clicks.
  if (active) memset(frames, 0, count * sizeof(Frame));
  for (int32_t i = 0; active && i < count; ++i) {
    if (!active) continue;
    uint32_t local = position % rate;
    float envelope = fminf(1.0f, fminf(float(local) / 441.0f,
                                      float(rate - 1 - local) / 441.0f));
    const int16_t sample = int16_t(1600.0f * envelope *
      sinf(6.28318530718f * 440.0f * float(local) / rate));
    if (position < rate) frames[i].channel1 = sample;
    if (position >= 2 * rate) frames[i].channel2 = sample;
    if (++position >= 3 * rate) { active = false; toneRunning.store(false); toneFinishedRequest.store(seenRequest); toneFinished.store(true); }
  }
  if (!wasPlayingTest && streamEnabled.load()) {
    static StreamPreroll preroll;
    portENTER_CRITICAL(&ringLock);
    if(!preroll.ready(ringUsed,size_t(count),ringEpoch.load())) {
      portEXIT_CRITICAL(&ringLock);
      memset(frames,0,count*sizeof(Frame));
      return count;
    }
    const size_t available = std::min<size_t>(count, ringUsed);
    const size_t first = std::min(available, ringCapacity - ringRead);
    memcpy(frames, ring + ringRead, first * sizeof(Frame));
    if (available > first) memcpy(frames + first, ring, (available - first) * sizeof(Frame));
    ringRead = (ringRead + available) % ringCapacity; ringUsed -= available;
    portEXIT_CRITICAL(&ringLock);
    consumedFrames.fetch_add(uint32_t(available), std::memory_order_relaxed);
    if (available < size_t(count)) {
      memset(frames + available, 0, (count - available) * sizeof(Frame));
      emptyFrames.fetch_add(count - available);streamUnderruns.fetch_add(1);preroll.underflow();
    }
  } else if (!wasPlayingTest) memset(frames, 0, count * sizeof(Frame));
  static PcmGain gain;
  gain.target(volumePercent.load());
  gain.block(frames,size_t(count));
  return count;
}

void help(Print &out) {
  out.println("TAB5 PICO BT BRIDGE " PICO_VERSION " | ESP32-PICO-D4");
  out.println("USE | SLEEP | LIST | SELECT <number> | SELECTMAC <mac> | STATUS | VOLUME <0-100> | TEST | STREAM | STOP | SCAN | RECONNECT | HELP");
  out.println("TEST: LEFT / silence / RIGHT. I2S starts on connection. STOP: mute and suspend A2DP; STREAM resumes.");
}

void list(Print &out) {
  unsigned number = 1;
  // Paired devices stay visible even when they are not discoverable.
  for (size_t i = 0; i < savedCount; ++i) {
    const SavedDevice &d = saved[i];
    out.printf("DEVICE %u %02X:%02X:%02X:%02X:%02X:%02X -127 1 %d %s\n",
      number++, d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5], int(i == selectedSaved), d.name);
  }
  // New devices found during manual search follow the paired devices.
  for (size_t i = 0; i < 24; ++i) {
    Device d;
    portENTER_CRITICAL(&deviceLock);
    bool valid = i < deviceCount;
    if (valid) d = devices[i];
    portEXIT_CRITICAL(&deviceLock);
    if (!valid) break;
    if (savedIndex(d.mac) >= 0) continue;
    out.printf("DEVICE %u %02X:%02X:%02X:%02X:%02X:%02X %d 0 0 %s\n",
      number++, d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5], d.rssi, d.name);
  }
  out.println("END_LIST");
}

void command(const char *line, Print &out) {
#ifdef PICO_NATIVE_IDF
  if(airpods_command(line,out))return;
#endif
  if (!strcmp(line,"USE") || !strcmp(line,"WAKE")) { useLease.renew(millis()); if (!bluetoothRunning) startBluetooth(); return; }
  if (!strcmp(line,"SLEEP")) { out.println("STANDBY"); out.flush(); sleepNow(); return; }
  if (!strcmp(line,"LIST")) { list(out); return; }
  if (!strcmp(line,"HELP")) { help(out); return; }
  if (!strncmp(line,"VOLUME ",7)) {
    unsigned percent=0;
    if (!commandNumber(line+7,100,percent)) { out.println("ERROR Volume must be 0-100"); return; }
    if(volumePercent.exchange(int(percent))!=int(percent)) {
      volumeDirty = true; volumeSaveDue = millis() + 1000;
    }
    out.printf("VOLUME %d\n", percent);
    return;
  }
  if (!strcmp(line,"STATUS")) {
    size_t buffered;
    portENTER_CRITICAL(&ringLock);
    buffered = ringUsed;
    portEXIT_CRITICAL(&ringLock);
    out.printf("STATUS connection=%d audio=%d target=%s stream=%d i2s_error=%d blocks=%u buffered=%u dropped=%u empty=%u heap=%u volume=%d scan=%d saved=%u selected=%u standby=%d scandone=%d version=" PICO_VERSION " underruns=%u remote_key=%u remote_release=%u remote_presses=%u remote_releases=%u remote_accepted=%u rx_blocks=%u callbacks=%u consumed=%u recoveries=%u input_stalls=%u flushes=%u rx_age=%u cb_age=%u stream_token=%u codec=SBC cap_airpods=%d\n",
      connection.load(), protocolAudioState(audioState.load()), hasTarget ? "selected" : "none",
      streamEnabled.load(), i2sError.load(), i2sBlocks.load(), unsigned(buffered),
      droppedFrames.load(), emptyFrames.load(), ESP.getFreeHeap(), volumePercent.load(), manualScan,
      unsigned(savedCount), savedCount ? unsigned(selectedSaved + 1) : 0, !bluetoothRunning, scanFinished,streamUnderruns.load(),lastRemoteKey.load(),lastRemoteRelease.load(),remotePresses.load(),remoteReleases.load(),remoteAccepted.load(),receivedBlocks.load(),audioCallbacks.load(),consumedFrames.load(),recoveredStreams.load(),inputStalls.load(),sdkFlushes.load(),uint32_t(millis()-receivedAt.load()),uint32_t(millis()-callbackAt.load()),hasStreamToken.load()?streamToken.load():0u,PICO_AIRPODS_CAP);
#ifdef PICO_NATIVE_IDF
    airpods_status(out);
    pico_codec_status(out);
#endif
    return;
  }
  if (!strcmp(line,"TEST")) {
    if (connection.load() != ESP_A2D_CONNECTION_STATE_CONNECTED) {
      out.println("ERROR Headphones not connected"); return;
    }
    // A paused stream is genuinely suspended now. Start the diagnostic only
    // after any in-flight SUSPEND/START ACK and the actual audio-start event.
    testPending.store(true);bt.set_media_active(true);
    out.println("TEST START: LEFT / silence / RIGHT"); return;
  }
  if (!strcmp(line,"STREAM") || !strncmp(line,"STREAM ",7)) {
    const bool tagged=line[6]==' ';unsigned token=0;
    if(tagged && !commandNumber(line+7,UINT32_MAX,token)) {
      out.println("ERROR Invalid stream token");return;
    }
    if (connection.load() != ESP_A2D_CONNECTION_STATE_CONNECTED) {
      out.println("ERROR Headphones not connected"); return;
    }
    // Retransmission of an unacknowledged UART request is not a new song.
    if (tagged && hasStreamToken.load() && streamEnabled.load() &&
        streamToken.load() == token) {
      out.printf("STREAM_ACK %u\n", token); return;
    }
    abortTone.store(true);
    clearRing();
    streamEnabled.store(true);testPending.store(false);
    streamToken.store(token); hasStreamToken.store(tagged); streamHealth.session();
    bt.set_media_active(true);
    if(tagged)out.printf("STREAM_ACK %u\n",token);
    else out.println("STREAM ON: Tab5 I2S to Bluetooth");
    return;
  }
  if (!strcmp(line,"STOP")) {
    abortTone.store(true);testPending.store(false);streamEnabled.store(false);clearRing();
    hasStreamToken.store(false);
    bt.set_media_active(false);
    out.println("SILENT"); return;
  }
  if (!strcmp(line,"SCAN")) {
    abortTone.store(true); streamEnabled.store(false);
    prefs.putBool("scan", true);
    out.println("RESTART scanning; saved target preserved"); out.flush();
    restartActive(); return;
  }
  if (!strcmp(line,"RECONNECT")) {
    if (!hasTarget) { out.println("ERROR No saved device; use SCAN and SELECT"); return; }
    prefs.remove("scan");
    out.println("RESTART reconnecting to saved device"); out.flush();
    restartActive(); return;
  }
  if (!strncmp(line,"SELECT ",7) || !strncmp(line,"SELECTMAC ",10)) {
    uint8_t chosenMac[6] = {};
    bool byMac = !strncmp(line,"SELECTMAC ",10);
    if (byMac) {
      if (!commandMac(line+10,chosenMac)) { out.println("ERROR Invalid MAC"); return; }
    }
    unsigned n=0;
    if (!byMac && (!commandNumber(line+7,32,n) || n<1)) { out.println("ERROR Use LIST, then SELECT <number>"); return; }
    size_t chosen = maxSaved;
    if (byMac && savedIndex(chosenMac) >= 0) chosen = size_t(savedIndex(chosenMac));
    else if (!byMac && n <= savedCount) chosen = size_t(n - 1);
    else {
      size_t row = savedCount;
      for (size_t i = 0; i < 24; ++i) {
        Device d;
        portENTER_CRITICAL(&deviceLock);
        const bool valid = i < deviceCount;
        if (valid) d = devices[i];
        portEXIT_CRITICAL(&deviceLock);
        if (!valid) break;
        if (savedIndex(d.mac) >= 0) continue;
        ++row;
        if (byMac ? memcmp(d.mac, chosenMac, 6) != 0 : row != size_t(n)) continue;
        if (savedCount >= maxSaved) { out.println("ERROR Saved-device list is full (8)"); return; }
        SavedDevice &entry = saved[savedCount];
        memcpy(entry.mac, d.mac, 6);
        snprintf(entry.name, sizeof(entry.name), "%.79s", d.name);
        chosen = savedCount++;
        break;
      }
    }
    if (chosen >= savedCount) { out.println("ERROR Use LIST, then SELECT <number>"); return; }
    if (chosen == selectedSaved && !manualScan && connection.load() == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      out.println("SELECTED Already connected"); return;
    }
    abortTone.store(true);
    selectedSaved = chosen;
    if (!savePeers()) { out.println("ERROR Could not save devices"); return; }
    prefs.remove("scan");
    out.printf("SELECTED %s; restarting to connect\n", saved[chosen].name); out.flush();
    restartActive(); return;
  }
  if (*line) out.println("ERROR Unknown command; use HELP");
}

void readCommands(HardwareSerial &port, CommandLine &line) {
  unsigned budget=256;
  while (port.available() && budget--) {
    const auto result=line.put(char(port.read()));
    if(result==CommandLine::Result::More)continue;
    if(result==CommandLine::Result::Ready)command(line.text(),port);
    else port.println("ERROR Invalid or oversized command");
    line.reset();
  }
}

void startBluetooth() {
  if (bluetoothRunning) return;
  bluetoothRunning = true; useLease.renew(millis());
  Serial.println(manualScan ? "Scanning for new devices" : hasTarget ? "Connecting directly to saved device" : "No selected device; use SCAN");
  Serial.println(startI2s() ? "I2S RX ready on G26/G25/G22" : "ERROR I2S RX init failed");
  bt.set_local_name("Tab5-Pico-Player");
  // Reconnect to the address selected by the user, never to another old peer.
  // AirPods may become available only after opening the case or leaving a phone.
  if (hasTarget && !manualScan) bt.set_auto_reconnect(target, INT_MAX);
  else bt.set_auto_reconnect(false);
  bt.set_ssp_enabled(true);
  bt.set_discovery_enabled(manualScan);
  bt.set_ssid_callback(found);
  bt.set_on_connection_state_changed(connectionChanged);
  bt.set_on_audio_state_changed(audioChanged);
  bt.set_avrc_passthru_command_callback(remoteKey);
  bt.set_data_callback_in_frames(audio);
  // Keep host PCM attenuation independent of the headphones' hardware volume.
  // AVRCP notifications must not replace the level chosen on the Tab5.
  bt.set_volume_control(&unmodifiedPcm);
  bt.start();
#ifdef PICO_NATIVE_IDF
  if(hasTarget)airpods_start(target,saved[selectedSaved].name);
#endif
  manualScanStarted = millis();
}

void setup() {
  Serial.begin(115200);
  rtc_gpio_deinit(GPIO_NUM_32);
  Serial2.begin(115200, SERIAL_8N1, 32, 33);
  const uint32_t intent = esp_reset_reason() == ESP_RST_SW ? restartIntent : 0;
  restartIntent = 0;
  if (intent == sleepIntent) deepStandby();
  help(Serial);
  if (!prefs.begin("tab5-bt", false)) Serial.println("ERROR Preferences unavailable");
  manualScan = prefs.getBool("scan", false);
  if (manualScan) prefs.remove("scan");
  loadPeers(); volumePercent.store(std::min(100, int(prefs.getUChar("volume", 31))));
  useLease.renew(millis());
#ifdef PICO_NATIVE_IDF
  Serial2.println("READY version=" PICO_VERSION " cap_airpods=1");
#else
  Serial2.println("READY version=" PICO_VERSION " cap_airpods=0");
#endif
  if (intent == resumeIntent) startBluetooth();
  else Serial.println("Standby: waiting for USE from Tab5 or the USB console");
}

void loop() {
  static CommandLine usbLine, tabLine;
  static int previousConnection = -1, previousAudio = -1;
  readCommands(Serial, usbLine);
  readCommands(Serial2, tabLine);
  importBonds();
  // Merge real discovery names into entries imported from the bond store.
  static uint32_t seenDiscovery = 0;
  const uint32_t revision = discoveryRevision.load();
  if (revision != seenDiscovery) {
    seenDiscovery = revision; bool changed = false;
    for (size_t i = 0; i < 24; ++i) {
      Device d; portENTER_CRITICAL(&deviceLock); const bool valid = i < deviceCount;
      if (valid) d = devices[i];
      portEXIT_CRITICAL(&deviceLock);
      if (!valid) break;
      const int index = savedIndex(d.mac);
      if (index >= 0 && d.name[0] && strcmp(d.name, "(no name)")) {
        char name[80] = {}; snprintf(name, sizeof(name), "%.79s", d.name);
        if (strcmp(name, saved[index].name)) { memcpy(saved[index].name, name, sizeof(name)); changed = true; }
      }
    }
    if (changed) savePeers();
  }
  int c = connection.load(), a = audioState.load();
  if (c != previousConnection || a != previousAudio) {
    Serial.printf("STATE connection=%d audio=%d\n", c, protocolAudioState(a));
    Serial2.printf("STATE connection=%d audio=%d\n", c, protocolAudioState(a));
    previousConnection = c; previousAudio = a;
  }
  drainRemote();
  static uint32_t lastMediaService = 0;
  if (bluetoothRunning && uint32_t(millis() - lastMediaService) >= 100) {
    lastMediaService = millis(); bt.service_media();
    const auto health = streamHealth.update(millis(), streamEnabled.load(),
        connection.load() == ESP_A2D_CONNECTION_STATE_CONNECTED,
        testPending.load() || toneRunning.load(),
        audioState.load() == ESP_A2D_AUDIO_STATE_STARTED,
        receivedBlocks.load(), audioCallbacks.load(), consumedFrames.load());
    if (health == StreamHealth::Action::RecoverTransport) {
      recoveredStreams.fetch_add(1);
      bt.recover_media();
      const uint32_t token=hasStreamToken.load()?streamToken.load():0u;
      Serial.printf("RECOVERY %u TRANSPORT_STALLED\n",token);
      Serial2.printf("RECOVERY %u TRANSPORT_STALLED\n",token);
    } else if (health == StreamHealth::Action::ReportInputStall) {
      inputStalls.fetch_add(1);
      const uint32_t token=hasStreamToken.load()?streamToken.load():0u;
      Serial.printf("STREAM_FAULT %u INPUT_STALLED\n",token);
      Serial2.printf("STREAM_FAULT %u INPUT_STALLED\n",token);
    } else if (health == StreamHealth::Action::TransportFailed) {
      const uint32_t token=hasStreamToken.load()?streamToken.load():0u;
      streamEnabled.store(false);clearRing();bt.set_media_active(false);
      Serial.printf("STREAM_FAULT %u TRANSPORT_FAILED\n",token);
      Serial2.printf("STREAM_FAULT %u TRANSPORT_FAILED\n",token);
    }
  }
  if (testPending.load() && connection.load() != ESP_A2D_CONNECTION_STATE_CONNECTED) testPending.store(false);
  if (toneFinished.exchange(false)) {
    // An old callback may finish after another TEST was queued or started.
    // Its completion must never suspend the new diagnostic's transport.
    const uint32_t completed = toneFinishedRequest.load();
    if (!testPending.load() && completed == toneRequest.load()) {
      Serial.println("TEST DONE; silent");bt.set_media_active(streamEnabled.load());
    }
  }
  if (testPending.load() && bt.is_media_active() && audioState.load() == ESP_A2D_AUDIO_STATE_STARTED) {
    testPending.store(false);abortTone.store(false);toneRequest.fetch_add(1);
  }
  if (bluetoothRunning && manualScan && millis() - manualScanStarted > 20000) {
    bt.set_discovery_enabled(false); manualScan = false; scanFinished = true;
    Serial2.println("SCAN_DONE"); Serial.println("Scan finished; results retained. Select a device.");
  }
  if (volumeDirty && int32_t(millis() - volumeSaveDue) >= 0) saveVolume();
  if (useLease.expired(millis())) sleepNow();
  delay(bluetoothRunning ? 5 : 15);
}
