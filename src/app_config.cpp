#include "app_config.h"

#include <esp_mac.h>

namespace AppConfig {
namespace {

constexpr char kManifestTemplate[] PROGMEM = R"json(
{
  "schemaVersion": 1,
  "device": {
    "name": "ESP32-C3 Handbox",
    "model": "ESP32-C3 Super Mini",
    "firmwareVersion": "0.1.0"
  },
  "fields": [
    { "id": "wifi", "type": "wifi", "label": "Wi-Fi", "required": true },
    { "id": "device_name", "type": "text", "label": "设备名称", "default": "%s", "required": true, "maxLength": 24, "restartRequired": true },
    { "id": "brightness", "type": "integer", "label": "亮度", "default": 128, "min": 0, "max": 255, "step": 1 },
    { "id": "screen_timeout_secs", "type": "integer", "label": "自动息屏", "default": 60, "min": 0, "max": 600, "step": 15 },
    { "id": "encoder_reverse", "type": "boolean", "label": "旋钮方向反转", "default": false },
    { "id": "drone_base_url", "type": "text", "label": "Drone 地址", "required": true, "maxLength": 200 },
    { "id": "drone_token", "type": "password", "label": "Drone Token", "required": true, "maxLength": 512 },
    { "id": "drone_verify_ssl", "type": "boolean", "label": "验证 HTTPS", "default": true },
    {
      "id": "ci_targets",
      "type": "objectList",
      "label": "CI 目标",
      "minItems": 1,
      "maxItems": 10,
      "default": [{ "namespace": "colour93", "repo": "esp32c3-handbox", "branch": "main" }],
      "itemFields": [
        { "id": "namespace", "type": "text", "label": "命名空间", "required": true, "maxLength": 64 },
        { "id": "repo", "type": "text", "label": "仓库", "required": true, "maxLength": 64 },
        { "id": "branch", "type": "text", "label": "分支", "required": true, "maxLength": 128 }
      ]
    }
  ]
}
)json";

}  // namespace

String defaultDeviceName() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char name[17] = {};
  snprintf(name, sizeof(name), "Handbox-%02X%02X", mac[4], mac[5]);
  return String(name);
}

String buildManifest(const String& deviceName) {
  String manifest;
  manifest.reserve(strlen_P(kManifestTemplate) + deviceName.length() + 1);
  char* buffer = nullptr;
  const int length = asprintf(&buffer, kManifestTemplate, deviceName.c_str());
  if (length > 0 && buffer != nullptr) {
    manifest = buffer;
  }
  free(buffer);
  return manifest;
}

String deviceName(const EspBleConfig::Manager& config) {
  return config.get<String>("device_name", defaultDeviceName());
}

DroneConfig droneConfig(const EspBleConfig::Manager& config) {
  DroneConfig result;
  result.baseUrl = config.get<String>("drone_base_url", "");
  result.token = config.get<String>("drone_token", "");
  result.verifySsl = config.get<bool>("drone_verify_ssl", true);
  return result;
}

size_t loadCiTargets(const EspBleConfig::Manager& config, CiTarget* targets,
                     size_t capacity) {
  if (targets == nullptr || capacity == 0) {
    return 0;
  }
  JsonArrayConst values =
      config.get<JsonArrayConst>("ci_targets", JsonArrayConst());
  size_t count = 0;
  for (JsonObjectConst value : values) {
    if (count >= capacity) {
      break;
    }
    const String namespaceName = value["namespace"] | "";
    const String repo = value["repo"] | "";
    const String branch = value["branch"] | "";
    if (namespaceName.isEmpty() || repo.isEmpty() || branch.isEmpty()) {
      continue;
    }
    targets[count++] = {namespaceName, repo, branch};
  }
  return count;
}

}  // namespace AppConfig
