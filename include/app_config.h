#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <EspBleConfig.h>

namespace AppConfig {

constexpr char kFirmwareVersion[] = "0.1.0";
constexpr char kProjectName[] = "colour93/esp32c3-handbox";

struct CiTarget {
  String namespaceName;
  String repo;
  String branch;
};

struct DroneConfig {
  String baseUrl;
  String token;
  bool verifySsl = true;
};

String defaultDeviceName();
String buildManifest(const String& deviceName);
String deviceName(const EspBleConfig::Manager& config);
DroneConfig droneConfig(const EspBleConfig::Manager& config);
size_t loadCiTargets(const EspBleConfig::Manager& config, CiTarget* targets,
                     size_t capacity);

}  // namespace AppConfig
