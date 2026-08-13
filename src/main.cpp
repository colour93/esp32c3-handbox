#include <Arduino.h>
#include <EspBleConfig.h>
#include <U8g2lib.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>

#include "app_config.h"
#include "astra_ssd1306_spi.h"
#include "astra_ui.h"
#include "board_pins.h"
#include "build_meta.h"
#include "ci_service.h"
#include "ec11_input.h"

namespace {

constexpr uint32_t kFrameIntervalMs = 1000U / 30U;
constexpr uint32_t kDeviceNameRestartDelayMs = 1500;
constexpr uint32_t kCiScrollDelayMs = 1000;
constexpr uint32_t kCiScrollStepMs = 60;
constexpr size_t kMaxCiTargets = 10;

enum class ScreenTransition : uint8_t { kIdle, kSleeping, kWaking };

AstraSsd1306Spi display(
    U8G2_R0, BoardPins::kOledClock, BoardPins::kOledData,
    BoardPins::kOledChipSelect, BoardPins::kOledDataCommand,
    BoardPins::kOledReset);
Ec11Input encoder(Ec11Config(BoardPins::kEncoderS1, BoardPins::kEncoderS2,
                            BoardPins::kEncoderKey, false, 25, 700));
EspBleConfig::Manager deviceConfig;
CiService ciService;

String configManifest;
String currentDeviceName;
AppConfig::CiTarget ciTargets[kMaxCiTargets];
size_t ciTargetCount = 0;
size_t ciTargetIndex = 0;

astra_list_item_t* ciPageItem = nullptr;
bool bleWindowEnabled = false;
bool encoderReverse = false;
int16_t brightness = 128;
int16_t screenTimeoutSecs = 60;
bool screenSleeping = false;
ScreenTransition screenTransition = ScreenTransition::kIdle;
float screenCurtainHeight = -8.0F;
uint32_t lastInputAt = 0;
uint32_t lastFrameAt = 0;
uint32_t restartAt = 0;
uint32_t previousMillis = 0;
uint32_t ciSelectionChangedAt = 0;
uint64_t uptimeMillis = 0;
CiService::Stage lastNotifiedCiStage = CiService::Stage::kIdle;
char notificationText[40] = {};
wl_status_t lastLoggedWifiStatus = WL_NO_SHIELD;
bool lastLoggedBleProvisioning = false;
bool lastLoggedBleConnected = false;
bool bleStateLogged = false;

bool timeReached(uint32_t target) {
  return target != 0 && static_cast<int32_t>(millis() - target) >= 0;
}

void updateUptime() {
  const uint32_t now = millis();
  uptimeMillis += static_cast<uint32_t>(now - previousMillis);
  previousMillis = now;
}

void setScreenSleeping(bool sleeping) {
  if (sleeping) {
    if (screenSleeping || screenTransition == ScreenTransition::kSleeping) {
      return;
    }
    if (screenTransition == ScreenTransition::kIdle) {
      screenCurtainHeight = -8.0F;
    }
    screenTransition = ScreenTransition::kSleeping;
    Serial.println("[display] sleep transition started");
    return;
  }
  if (!screenSleeping && screenTransition == ScreenTransition::kIdle) {
    return;
  }
  if (screenSleeping) {
    display.setPowerSave(0);
    screenSleeping = false;
    screenCurtainHeight = 72.0F;
  }
  screenTransition = ScreenTransition::kWaking;
  Serial.println("[display] wake transition started");
}

void animateScreenCurtain(float target) {
  if (std::fabs(screenCurtainHeight - target) <= 1.0F) {
    screenCurtainHeight = target;
    return;
  }
  screenCurtainHeight += (target - screenCurtainHeight) / 6.0F;
}

bool drawScreenTransition() {
  if (screenTransition == ScreenTransition::kIdle) {
    return false;
  }
  const int16_t curtain = static_cast<int16_t>(screenCurtainHeight);
  display.setDrawColor(0);
  if (curtain > 0) {
    display.drawBox(
        0, 0, 128,
        static_cast<uint8_t>(std::min<int16_t>(curtain, 64)));
  }
  display.setDrawColor(1);
  if (curtain >= 0 && curtain < 69) {
    for (uint8_t line = 0; line < 4; ++line) {
      display.drawHLine(0, curtain + line, 128);
    }
    for (int16_t x = 0; x < 128; x += 2) {
      for (int16_t y = curtain - 5; y < curtain; ++y) {
        if (y >= 0 && y < 64) {
          display.drawPixel(x + ((y & 1) == 0), y);
        }
      }
    }
  }

  const float target = screenTransition == ScreenTransition::kSleeping
                           ? 72.0F
                           : -8.0F;
  animateScreenCurtain(target);
  if (screenCurtainHeight != target) {
    return false;
  }
  if (screenTransition == ScreenTransition::kWaking) {
    screenTransition = ScreenTransition::kIdle;
    Serial.println("[display] wake transition complete");
    return false;
  }
  return true;
}

void renderFrame() {
  display.clearBuffer();
  astra_ui_main_core();
  astra_ui_widget_core();
  const bool sleepAfterFrame = drawScreenTransition();
  display.sendBuffer();
  if (sleepAfterFrame) {
    display.setPowerSave(1);
    screenSleeping = true;
    screenTransition = ScreenTransition::kIdle;
    Serial.println("[display] sleep transition complete");
  }
}

void drawLine(U8G2& oled, int16_t y, const String& text) {
  oled.drawUTF8(2, y, text.c_str());
}

String truncateUtf8(const String& value, size_t maxBytes) {
  if (value.length() <= maxBytes) {
    return value;
  }
  size_t length = maxBytes;
  while (length > 0 &&
         (static_cast<uint8_t>(value[length]) & 0xc0) == 0x80) {
    length--;
  }
  return value.substring(0, length);
}

void applyConfiguration(const EspBleConfig::ConfigChange* change = nullptr) {
  brightness = static_cast<int16_t>(
      constrain(deviceConfig.get<int>("brightness", 128), 0, 255));
  screenTimeoutSecs = static_cast<int16_t>(
      constrain(deviceConfig.get<int>("screen_timeout_secs", 60), 0, 600));
  encoderReverse = deviceConfig.get<bool>("encoder_reverse", false);
  encoder.setReverse(encoderReverse);
  display.setContrast(static_cast<uint8_t>(brightness));

  const String nextDeviceName = AppConfig::deviceName(deviceConfig);
  if (change != nullptr && nextDeviceName != currentDeviceName) {
    restartAt = millis() + kDeviceNameRestartDelayMs;
  }
  currentDeviceName = nextDeviceName;
}

void onConfigApplied(const EspBleConfig::ConfigChange& change) {
  Serial.printf("[config] applied revision=%lu fields=%u restart=%s\n",
                static_cast<unsigned long>(change.revision),
                static_cast<unsigned>(change.fields.size()),
                change.restartRequired ? "yes" : "no");
  for (const String& field : change.fields) {
    Serial.printf("[config] changed: %s\n", field.c_str());
  }
  applyConfiguration(&change);
}

void initializeBrightness() {
  brightness = static_cast<int16_t>(
      constrain(deviceConfig.get<int>("brightness", 128), 0, 255));
}

void saveBrightness() {
  const EspBleConfig::SetResult result =
      deviceConfig.set("brightness", static_cast<int>(brightness));
  if (!result) {
    astra_push_pop_up("亮度保存失败", 1400);
    initializeBrightness();
  }
}

void initializeScreenTimeout() {
  screenTimeoutSecs = static_cast<int16_t>(constrain(
      deviceConfig.get<int>("screen_timeout_secs", 60), 0, 600));
}

void saveScreenTimeout() {
  const EspBleConfig::SetResult result = deviceConfig.set(
      "screen_timeout_secs", static_cast<int>(screenTimeoutSecs));
  if (!result) {
    astra_push_pop_up("息屏保存失败", 1400);
    initializeScreenTimeout();
  }
}

void initializeEncoderReverse() {
  encoderReverse = deviceConfig.get<bool>("encoder_reverse", false);
}

void saveEncoderReverse() {
  const EspBleConfig::SetResult result =
      deviceConfig.set("encoder_reverse", encoderReverse);
  if (!result) {
    encoderReverse = !encoderReverse;
    astra_push_pop_up("方向保存失败", 1400);
  }
  encoder.setReverse(encoderReverse);
}

void initializeBleWindow() {
  bleWindowEnabled = deviceConfig.isProvisioning();
}

void toggleBleWindow() {
  if (bleWindowEnabled) {
    deviceConfig.startProvisioning();
    bleWindowEnabled = deviceConfig.isProvisioning();
    astra_push_info_bar(bleWindowEnabled ? "BLE 配置 5 分钟" : "BLE 开启失败",
                        1600);
  } else {
    if (!deviceConfig.hasValue("wifi")) {
      bleWindowEnabled = true;
      astra_push_info_bar("首次配置需保持 BLE", 1600);
      return;
    }
    deviceConfig.stopProvisioning();
    astra_push_info_bar("BLE 配置已关闭", 1400);
  }
}

void logServiceState() {
  const wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != lastLoggedWifiStatus) {
    lastLoggedWifiStatus = wifiStatus;
    if (wifiStatus == WL_CONNECTED) {
      Serial.printf("[wifi] connected ssid=%s ip=%s rssi=%d\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
    } else {
      Serial.printf("[wifi] status=%d\n", static_cast<int>(wifiStatus));
    }
  }

  const bool provisioning = deviceConfig.isProvisioning();
  const bool connected = deviceConfig.isConnected();
  if (!bleStateLogged || provisioning != lastLoggedBleProvisioning ||
      connected != lastLoggedBleConnected) {
    bleStateLogged = true;
    lastLoggedBleProvisioning = provisioning;
    lastLoggedBleConnected = connected;
    Serial.printf("[ble] connected=%s advertising=%s\n",
                  connected ? "yes" : "no", provisioning ? "yes" : "no");
  }
}

void drawDeviceNamePage() {
  U8G2& oled = display;
  oled.setFont(u8g2_font_wqy12_t_gb2312);
  drawLine(oled, 13, "设备名称");
  oled.drawHLine(2, 17, 124);
  drawLine(oled, 37, truncateUtf8(currentDeviceName, 24));
  drawLine(oled, 59, "长按返回");
}

String formatUptime() {
  uint64_t seconds = uptimeMillis / 1000ULL;
  const uint64_t days = seconds / 86400ULL;
  seconds %= 86400ULL;
  const uint32_t hours = seconds / 3600ULL;
  seconds %= 3600ULL;
  const uint32_t minutes = seconds / 60ULL;
  seconds %= 60ULL;
  char text[30] = {};
  snprintf(text, sizeof(text), "%llud %02u:%02u:%02llu",
           static_cast<unsigned long long>(days), hours, minutes,
           static_cast<unsigned long long>(seconds));
  return String(text);
}

void drawStatusPage() {
  U8G2& oled = display;
  oled.setFont(u8g2_font_wqy12_t_gb2312);
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const bool online = wifiConnected && WiFi.localIP() != IPAddress();
  String bleState = "关闭";
  if (deviceConfig.isConnected()) {
    bleState = "已连接";
  } else if (deviceConfig.isProvisioning()) {
    bleState = "广播中";
  }
  drawLine(oled, 12, "运行 " + formatUptime());
  drawLine(oled, 25, "名称 " + truncateUtf8(currentDeviceName, 16));
  drawLine(oled, 38, "BLE " + bleState);
  if (wifiConnected) {
    drawLine(oled, 51,
             "WiFi " + truncateUtf8(WiFi.SSID(), 12) + " " +
                 String(WiFi.RSSI()));
  } else {
    drawLine(oled, 51, "WiFi 未连接");
  }
  drawLine(oled, 64, online ? "联网 已获取 IP" : "联网 不可用");
}

void drawAboutPage() {
  U8G2& oled = display;
  oled.setFont(u8g2_font_wqy12_t_gb2312);
  drawLine(oled, 12, "ESP32-C3 Handbox");
  drawLine(oled, 27, "固件 " + String(AppConfig::kFirmwareVersion));
  drawLine(oled, 42, "Commit " + String(HANDBOX_GIT_SHA));
  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2, 58, AppConfig::kProjectName);
}

String ciStageText(CiService::Stage stage) {
  switch (stage) {
    case CiService::Stage::kIdle:
      return "旋转选择，短按预览";
    case CiService::Stage::kLoadingPreview:
      return "正在读取最近构建";
    case CiService::Stage::kPreviewReady:
      return "再次短按确认";
    case CiService::Stage::kTriggering:
      return "正在触发构建";
    case CiService::Stage::kMonitoring:
      return "正在监控构建";
    case CiService::Stage::kFinished:
      return "构建已结束";
    case CiService::Stage::kError:
      return "操作失败";
  }
  return "";
}

bool sameCiTarget(const AppConfig::CiTarget& left,
                  const AppConfig::CiTarget& right) {
  return left.namespaceName == right.namespaceName && left.repo == right.repo &&
         left.branch == right.branch;
}

void enterCiPage() {
  ciTargetCount =
      AppConfig::loadCiTargets(deviceConfig, ciTargets, kMaxCiTargets);
  ciTargetIndex = 0;
  ciSelectionChangedAt = millis();
  ciService.requestUser(AppConfig::droneConfig(deviceConfig));
  const CiService::Snapshot state = ciService.snapshot();
  for (size_t index = 0; index < ciTargetCount; ++index) {
    if (sameCiTarget(state.target, ciTargets[index])) {
      ciTargetIndex = index;
      break;
    }
  }
}

void drawScrollingCiValue(U8G2& oled, int16_t baselineY, const char* label,
                          const String& value) {
  constexpr int16_t kLabelX = 2;
  constexpr int16_t kValueX = 40;
  constexpr int16_t kValueRight = 126;
  constexpr int16_t kScrollGap = 18;

  oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(kLabelX, baselineY - 2, label);
  oled.setFont(u8g2_font_wqy12_t_gb2312);
  const int16_t textWidth = oled.getUTF8Width(value.c_str());
  int16_t textX = kValueX;
  const uint32_t elapsed = millis() - ciSelectionChangedAt;
  if (textWidth > kValueRight - kValueX && elapsed > kCiScrollDelayMs) {
    const uint32_t offset =
        ((elapsed - kCiScrollDelayMs) / kCiScrollStepMs) %
        static_cast<uint32_t>(textWidth + kScrollGap);
    textX -= static_cast<int16_t>(offset);
  }

  oled.setClipWindow(kValueX, baselineY - 13, kValueRight, baselineY + 2);
  oled.drawUTF8(textX, baselineY, value.c_str());
  if (textWidth > kValueRight - kValueX && textX + textWidth < kValueRight) {
    oled.drawUTF8(textX + textWidth + kScrollGap, baselineY, value.c_str());
  }
  oled.setMaxClipWindow();
}

void drawCiTarget(U8G2& oled, const AppConfig::CiTarget& target) {
  drawScrollingCiValue(oled, 13, "NS", target.namespaceName);
  oled.drawHLine(2, 17, 124);
  drawScrollingCiValue(oled, 30, "REPO", target.repo);
  oled.drawHLine(2, 34, 124);
  drawScrollingCiValue(oled, 47, "BRANCH", target.branch);
}

void drawCiFooter(U8G2& oled) {
  oled.setFont(u8g2_font_5x8_tf);
  const String position =
      String(ciTargetIndex + 1) + "/" + String(ciTargetCount);
  const int16_t positionX = 126 - oled.getStrWidth(position.c_str());
  const String username = ciService.username();
  if (!username.isEmpty()) {
    const String displayName = "@" + username;
    oled.setClipWindow(2, 53, std::max<int16_t>(2, positionX - 4), 64);
    oled.drawUTF8(2, 63, displayName.c_str());
    oled.setMaxClipWindow();
  }
  oled.drawStr(positionX, 63, position.c_str());
}

void drawCiPage() {
  U8G2& oled = display;
  oled.setFont(u8g2_font_wqy12_t_gb2312);
  if (ciTargetCount == 0) {
    drawLine(oled, 13, "CI 目标未配置");
    drawLine(oled, 34, "请通过 BLE 配置");
    drawLine(oled, 59, "长按返回");
    return;
  }
  const AppConfig::CiTarget& selected = ciTargets[ciTargetIndex];
  const CiService::Snapshot state = ciService.snapshot();
  const bool stateMatchesSelection = sameCiTarget(state.target, selected);
  const CiService::Stage visibleStage =
      stateMatchesSelection ? state.stage : CiService::Stage::kIdle;
  if (visibleStage == CiService::Stage::kIdle) {
    drawCiTarget(oled, selected);
  } else if (visibleStage == CiService::Stage::kPreviewReady) {
    drawLine(oled, 13,
             truncateUtf8(selected.namespaceName + "/" + selected.repo, 21));
    drawLine(oled, 31, state.shortSha + " " + truncateUtf8(state.author, 12));
    drawLine(oled, 49, truncateUtf8(state.title, 22));
  } else if (visibleStage == CiService::Stage::kMonitoring ||
             visibleStage == CiService::Stage::kFinished) {
    drawLine(oled, 13,
             truncateUtf8(selected.namespaceName + "/" + selected.repo, 21));
    drawLine(oled, 31, "Build #" + String(state.buildNumber));
    drawLine(oled, 49, "状态 " + truncateUtf8(state.status, 15));
  } else if (visibleStage == CiService::Stage::kError) {
    drawLine(oled, 15,
             truncateUtf8(selected.namespaceName + "/" + selected.repo, 21));
    drawLine(oled, 39, truncateUtf8(state.message, 22));
  } else {
    drawLine(oled, 17,
             truncateUtf8(selected.namespaceName + "/" + selected.repo, 21));
    drawLine(oled, 43, ciStageText(visibleStage));
  }
  drawCiFooter(oled);
}

void handleCiInput(uint8_t events) {
  const CiService::Snapshot state = ciService.snapshot();
  const bool selectionAllowed =
      state.stage != CiService::Stage::kLoadingPreview &&
      state.stage != CiService::Stage::kTriggering &&
      state.stage != CiService::Stage::kMonitoring;
  if (selectionAllowed && ciTargetCount > 0) {
    if ((events & kEc11Next) != 0) {
      ciTargetIndex = (ciTargetIndex + 1) % ciTargetCount;
      ciSelectionChangedAt = millis();
    }
    if ((events & kEc11Previous) != 0) {
      ciTargetIndex = ciTargetIndex == 0 ? ciTargetCount - 1 : ciTargetIndex - 1;
      ciSelectionChangedAt = millis();
    }
  }
  if ((events & kEc11Click) != 0 && ciTargetCount > 0) {
    if (state.stage == CiService::Stage::kPreviewReady &&
        sameCiTarget(state.target, ciTargets[ciTargetIndex])) {
      ciService.confirmTrigger();
    } else if (selectionAllowed) {
      ciService.requestPreview(ciTargets[ciTargetIndex],
                               AppConfig::droneConfig(deviceConfig));
    }
  }
  if ((events & kEc11LongPress) != 0) {
    astra_selector_exit_current_item();
  }
}

void buildMenu() {
  astra_list_item_t* root = astra_get_root_list();
  astra_list_item_t* settings = astra_new_list_item("设置", list_icon);
  ciPageItem = astra_new_user_item("CI", enterCiPage, drawCiPage, nullptr,
                                  power_icon);
  astra_push_item_to_list(root, ciPageItem);
  astra_push_item_to_list(root, settings);
  astra_push_item_to_list(
      root, astra_new_user_item("状态", nullptr, drawStatusPage, nullptr,
                                user_icon));
  astra_push_item_to_list(
      root, astra_new_user_item("关于", nullptr, drawAboutPage, nullptr,
                                flag_icon));

  astra_push_item_to_list(
      settings,
      astra_new_slider_item("亮度", &brightness, 8, 0, 255,
                            initializeBrightness, saveBrightness, slider_icon));
  astra_push_item_to_list(
      settings,
      astra_new_slider_item("自动息屏", &screenTimeoutSecs, 15, 0, 600,
                            initializeScreenTimeout, saveScreenTimeout,
                            slider_icon));
  astra_push_item_to_list(
      settings,
      astra_new_user_item("设备名称", nullptr, drawDeviceNamePage, nullptr,
                          user_icon));
  astra_push_item_to_list(
      settings,
      astra_new_switch_item("旋钮方向", &encoderReverse,
                            initializeEncoderReverse, saveEncoderReverse,
                            switch_icon));
  astra_push_item_to_list(
      settings,
      astra_new_switch_item("BLE 配置", &bleWindowEnabled,
                            initializeBleWindow, toggleBleWindow, switch_icon));
}

void handleInput() {
  const uint8_t events = encoder.update();
  if (events == kEc11None) {
    return;
  }
  lastInputAt = millis();
  if (screenSleeping || screenTransition != ScreenTransition::kIdle) {
    setScreenSleeping(false);
    return;
  }
  if (astra_is_transitioning()) {
    return;
  }
  if (astra_is_in_user_item() &&
      astra_get_selector()->selected_item == ciPageItem) {
    handleCiInput(events);
    return;
  }
  if ((events & kEc11Next) != 0) {
    astra_selector_go_next_item();
  }
  if ((events & kEc11Previous) != 0) {
    astra_selector_go_prev_item();
  }
  if ((events & kEc11Click) != 0) {
    astra_selector_jump_to_selected_item();
  }
  if ((events & kEc11LongPress) != 0) {
    astra_selector_exit_current_item();
  }
}

void manageScreenTimeout() {
  if (screenSleeping || screenTransition != ScreenTransition::kIdle ||
      screenTimeoutSecs == 0 || ciService.isMonitoring()) {
    return;
  }
  if (millis() - lastInputAt >=
      static_cast<uint32_t>(screenTimeoutSecs) * 1000UL) {
    setScreenSleeping(true);
  }
}

void notifyBackgroundCiResult() {
  const CiService::Snapshot state = ciService.snapshot();
  if (state.stage == lastNotifiedCiStage) {
    return;
  }
  lastNotifiedCiStage = state.stage;
  if ((state.stage != CiService::Stage::kFinished &&
       state.stage != CiService::Stage::kError) ||
      (astra_is_in_user_item() &&
       astra_get_selector()->selected_item == ciPageItem)) {
    return;
  }
  const String text = state.stage == CiService::Stage::kFinished
                          ? "CI: " + state.status
                          : "CI: " + state.message;
  snprintf(notificationText, sizeof(notificationText), "%s",
           truncateUtf8(text, sizeof(notificationText) - 1).c_str());
  setScreenSleeping(false);
  lastInputAt = millis();
  astra_push_pop_up(notificationText, 3000);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.printf("\n[handbox] boot version=%s commit=%s repo=%s\n",
                AppConfig::kFirmwareVersion, HANDBOX_GIT_SHA,
                AppConfig::kProjectName);
  previousMillis = millis();
  lastInputAt = previousMillis;
  astra_driver_begin(display, u8g2_font_wqy12_t_gb2312);
  encoder.begin();

  const String defaultName = AppConfig::defaultDeviceName();
  configManifest = AppConfig::buildManifest(defaultName);
  EspBleConfig::Options options;
  options.deviceName = defaultName;
  options.deviceNameFieldId = "device_name";
  options.preferencesNamespace = "handbox";
  deviceConfig.onConfigApplied(onConfigApplied);
  const EspBleConfig::BeginResult configResult =
      deviceConfig.begin(configManifest.c_str(), options);
  if (!configResult) {
    Serial.printf("[config] BLE initialization failed: %s\n",
                  configResult.message.c_str());
    currentDeviceName = defaultName;
  } else {
    applyConfiguration();
    Serial.printf("[config] ready revision=%lu name=%s advertising=%s\n",
                  static_cast<unsigned long>(deviceConfig.revision()),
                  currentDeviceName.c_str(),
                  deviceConfig.isProvisioning() ? "yes" : "no");
  }
  if (!ciService.begin()) {
    Serial.println("[ci] worker unavailable");
  }

  buildMenu();
  astra_init_core();
  display.setContrast(static_cast<uint8_t>(brightness));
}

void loop() {
  updateUptime();
  deviceConfig.loop();
  bleWindowEnabled = deviceConfig.isProvisioning();
  logServiceState();
  handleInput();
  manageScreenTimeout();
  notifyBackgroundCiResult();
  if (timeReached(restartAt)) {
    ESP.restart();
  }

  const uint32_t now = millis();
  if (!screenSleeping && now - lastFrameAt >= kFrameIntervalMs) {
    lastFrameAt = now;
    renderFrame();
  }
  delay(1);
}
