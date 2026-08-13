#pragma once

#include <Arduino.h>

#include "app_config.h"

class CiService {
 public:
  enum class Stage : uint8_t {
    kIdle,
    kLoadingPreview,
    kPreviewReady,
    kTriggering,
    kMonitoring,
    kFinished,
    kError,
  };

  struct Snapshot {
    Stage stage = Stage::kIdle;
    AppConfig::CiTarget target;
    String shortSha;
    String author;
    String title;
    String status;
    String message;
    uint64_t buildNumber = 0;
  };

  bool begin();
  bool requestPreview(const AppConfig::CiTarget& target,
                      const AppConfig::DroneConfig& config);
  bool confirmTrigger();
  Snapshot snapshot() const;
  bool isMonitoring() const;

 private:
  enum class CommandType : uint8_t { kPreview, kTrigger };

  struct Command {
    CommandType type;
    AppConfig::CiTarget target;
    AppConfig::DroneConfig config;
  };

  static void taskEntry(void* argument);
  void taskLoop();
  void runPreview(const Command& command);
  void runTrigger(const Command& command);
  void update(const Snapshot& next);
  void fail(const String& message);

  void* queue_ = nullptr;
  void* mutex_ = nullptr;
  Snapshot snapshot_;
  Command previewCommand_;
  bool hasPreviewCommand_ = false;
};
