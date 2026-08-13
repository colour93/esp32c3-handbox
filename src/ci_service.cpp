#include "ci_service.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include <memory>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern const uint8_t x509_crt_bundle_start[]
    asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace {

constexpr uint32_t kPollIntervalMs = 3000;
constexpr uint32_t kMonitorTimeoutMs = 30UL * 60UL * 1000UL;
constexpr uint32_t kTimeSyncTimeoutMs = 15000;

String normalizedBaseUrl(String value) {
  value.trim();
  while (value.endsWith("/")) {
    value.remove(value.length() - 1);
  }
  if (!value.startsWith("http://") && !value.startsWith("https://")) {
    value = "https://" + value;
  }
  return value;
}

String urlEncode(const String& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_' || character == '.' || character == '~') {
      encoded += static_cast<char>(character);
    } else {
      encoded += '%';
      encoded += kHex[character >> 4];
      encoded += kHex[character & 0x0f];
    }
  }
  return encoded;
}

String repositoryPath(const AppConfig::CiTarget& target) {
  return "/api/repos/" + urlEncode(target.namespaceName) + "/" +
         urlEncode(target.repo) + "/builds";
}

bool isHttps(const String& url) { return url.startsWith("https://"); }

bool syncClock() {
  if (time(nullptr) > 1700000000) {
    return true;
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  const uint32_t startedAt = millis();
  while (time(nullptr) <= 1700000000 &&
         millis() - startedAt < kTimeSyncTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return time(nullptr) > 1700000000;
}

struct HttpSession {
  HTTPClient http;
  NetworkClient plain;
  NetworkClientSecure secure;

  bool begin(const String& url, bool verifySsl, String& error) {
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!isHttps(url)) {
      if (!http.begin(plain, url)) {
        error = "HTTP 地址无效";
        return false;
      }
      return true;
    }
    if (verifySsl) {
      if (!syncClock()) {
        error = "系统时间同步失败";
        return false;
      }
      secure.setCACertBundle(x509_crt_bundle_start,
                             x509_crt_bundle_end - x509_crt_bundle_start);
    } else {
      secure.setInsecure();
    }
    secure.setHandshakeTimeout(15);
    if (!http.begin(secure, url)) {
      error = "HTTPS 地址无效";
      return false;
    }
    return true;
  }
};

void authorize(HTTPClient& http, const String& token) {
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/json");
}

String httpError(int code) {
  if (code == 401 || code == 403) {
    return "Drone 认证失败";
  }
  if (code < 0) {
    return "网络或 TLS 错误: " + HTTPClient::errorToString(code);
  }
  return "Drone HTTP " + String(code);
}

bool successfulHttpCode(int code) { return code >= 200 && code < 300; }

bool branchMatches(JsonObjectConst build, const String& branch) {
  const String fullRef = "refs/heads/" + branch;
  for (const char* field : {"ref", "source", "target"}) {
    const String candidate = build[field] | "";
    if (candidate == branch || candidate == fullRef) {
      return true;
    }
  }
  return false;
}

String firstLine(const String& value) {
  const int newline = value.indexOf('\n');
  return newline < 0 ? value : value.substring(0, newline);
}

String shortSha(const String& value) {
  return value.length() > 8 ? value.substring(0, 8) : value;
}

bool isTerminalStatus(String status) {
  status.toLowerCase();
  return status == "success" || status == "failure" || status == "killed" ||
         status == "canceled" || status == "cancelled";
}

bool parseBuild(Stream& stream, JsonDocument& output, String& error) {
  JsonDocument filter;
  filter["number"] = true;
  filter["status"] = true;
  filter["after"] = true;
  filter["message"] = true;
  filter["author_name"] = true;
  filter["author_login"] = true;
  filter["ref"] = true;
  filter["source"] = true;
  filter["target"] = true;
  const DeserializationError jsonError = deserializeJson(
      output, stream, DeserializationOption::Filter(filter));
  if (jsonError) {
    error = "Drone 响应解析失败";
    return false;
  }
  return true;
}

}  // namespace

bool CiService::begin() {
  mutex_ = xSemaphoreCreateMutex();
  queue_ = xQueueCreate(3, sizeof(Command*));
  if (mutex_ == nullptr || queue_ == nullptr) {
    return false;
  }
  return xTaskCreate(taskEntry, "handbox-ci", 12288, this, 1, nullptr) ==
         pdPASS;
}

bool CiService::requestPreview(const AppConfig::CiTarget& target,
                               const AppConfig::DroneConfig& config) {
  const Snapshot current = snapshot();
  if (current.stage == Stage::kLoadingPreview ||
      current.stage == Stage::kTriggering || current.stage == Stage::kMonitoring) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress()) {
    fail("Wi-Fi 未联网");
    return false;
  }
  if (config.baseUrl.isEmpty() || config.token.isEmpty()) {
    fail("Drone 尚未配置");
    return false;
  }
  Snapshot next;
  next.stage = Stage::kLoadingPreview;
  next.target = target;
  next.message = "正在读取最近构建";
  update(next);

  Command* command = new Command{CommandType::kPreview, target, config};
  if (command == nullptr ||
      xQueueSend(static_cast<QueueHandle_t>(queue_), &command, 0) != pdPASS) {
    delete command;
    fail("CI 任务队列繁忙");
    return false;
  }
  return true;
}

bool CiService::confirmTrigger() {
  if (snapshot().stage != Stage::kPreviewReady) {
    return false;
  }
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool available = hasPreviewCommand_;
  Command* command = available ? new Command(previewCommand_) : nullptr;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  if (!available || command == nullptr) {
    delete command;
    fail("无法触发 CI 任务");
    return false;
  }
  Snapshot next = snapshot();
  next.stage = Stage::kTriggering;
  next.message = "正在触发构建";
  update(next);
  if (xQueueSend(static_cast<QueueHandle_t>(queue_), &command, 0) != pdPASS) {
    delete command;
    fail("无法触发 CI 任务");
    return false;
  }
  return true;
}

CiService::Snapshot CiService::snapshot() const {
  if (mutex_ == nullptr) {
    return {};
  }
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  Snapshot result = snapshot_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return result;
}

bool CiService::isMonitoring() const {
  const Stage stage = snapshot().stage;
  return stage == Stage::kTriggering || stage == Stage::kMonitoring;
}

void CiService::taskEntry(void* argument) {
  static_cast<CiService*>(argument)->taskLoop();
}

void CiService::taskLoop() {
  while (true) {
    Command* command = nullptr;
    if (xQueueReceive(static_cast<QueueHandle_t>(queue_), &command,
                      portMAX_DELAY) != pdPASS ||
        command == nullptr) {
      continue;
    }
    if (command->type == CommandType::kPreview) {
      runPreview(*command);
    } else {
      runTrigger(*command);
    }
    delete command;
  }
}

void CiService::runPreview(const Command& command) {
  const String baseUrl = normalizedBaseUrl(command.config.baseUrl);
  const String url = baseUrl + repositoryPath(command.target) +
                     "?page=1&per_page=50";
  HttpSession session;
  String error;
  if (!session.begin(url, command.config.verifySsl, error)) {
    fail(error);
    return;
  }
  authorize(session.http, command.config.token);
  const int code = session.http.GET();
  if (!successfulHttpCode(code)) {
    fail(httpError(code));
    return;
  }

  JsonDocument filter;
  JsonObject itemFilter = filter[0].to<JsonObject>();
  for (const char* field : {"ref", "source", "target", "after", "message",
                            "author_name", "author_login"}) {
    itemFilter[field] = true;
  }
  JsonDocument builds;
  const DeserializationError jsonError = deserializeJson(
      builds, session.http.getStream(), DeserializationOption::Filter(filter));
  if (jsonError || !builds.is<JsonArray>()) {
    fail("最近构建解析失败");
    return;
  }
  for (JsonObjectConst build : builds.as<JsonArrayConst>()) {
    if (!branchMatches(build, command.target.branch)) {
      continue;
    }
    Snapshot next;
    next.stage = Stage::kPreviewReady;
    next.target = command.target;
    next.shortSha = shortSha(build["after"] | "unknown");
    next.author = build["author_name"] | build["author_login"] | "unknown";
    next.title = firstLine(build["message"] | "(no title)");
    next.message = "再按一次确认构建";
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    previewCommand_ = command;
    previewCommand_.type = CommandType::kTrigger;
    hasPreviewCommand_ = true;
    snapshot_ = next;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return;
  }
  fail("最近 50 条中无该分支记录");
}

void CiService::runTrigger(const Command& command) {
  const String baseUrl = normalizedBaseUrl(command.config.baseUrl);
  const String buildPath = repositoryPath(command.target);
  const String triggerUrl = baseUrl + buildPath +
                            "?branch=" + urlEncode(command.target.branch);
  HttpSession triggerSession;
  String error;
  if (!triggerSession.begin(triggerUrl, command.config.verifySsl, error)) {
    fail(error);
    return;
  }
  authorize(triggerSession.http, command.config.token);
  triggerSession.http.addHeader("Content-Type", "application/json");
  const int triggerCode = triggerSession.http.POST("");
  if (!successfulHttpCode(triggerCode)) {
    fail("触发失败: " + httpError(triggerCode));
    return;
  }
  JsonDocument triggered;
  if (!parseBuild(triggerSession.http.getStream(), triggered, error) ||
      !triggered["number"].is<uint64_t>()) {
    fail(error.isEmpty() ? "触发响应缺少构建编号" : error);
    return;
  }
  const uint64_t buildNumber = triggered["number"].as<uint64_t>();
  Snapshot monitoring = snapshot();
  monitoring.stage = Stage::kMonitoring;
  monitoring.buildNumber = buildNumber;
  monitoring.status = triggered["status"] | "pending";
  monitoring.message = "构建正在运行";
  update(monitoring);

  const uint32_t startedAt = millis();
  while (millis() - startedAt < kMonitorTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    if (WiFi.status() != WL_CONNECTED) {
      fail("监控期间 Wi-Fi 断开");
      return;
    }
    const String pollUrl = baseUrl + buildPath + "/" + String(buildNumber);
    HttpSession pollSession;
    if (!pollSession.begin(pollUrl, command.config.verifySsl, error)) {
      fail(error);
      return;
    }
    authorize(pollSession.http, command.config.token);
    const int pollCode = pollSession.http.GET();
    if (!successfulHttpCode(pollCode)) {
      fail(httpError(pollCode));
      return;
    }
    JsonDocument build;
    if (!parseBuild(pollSession.http.getStream(), build, error)) {
      fail(error);
      return;
    }
    monitoring.status = build["status"] | "unknown";
    monitoring.message = "构建 #" + String(buildNumber);
    update(monitoring);
    if (isTerminalStatus(monitoring.status)) {
      monitoring.stage = Stage::kFinished;
      monitoring.message = "构建已结束";
      update(monitoring);
      return;
    }
  }
  fail("构建监控超时（30 分钟）");
}

void CiService::update(const Snapshot& next) {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  snapshot_ = next;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

void CiService::fail(const String& message) {
  Snapshot next = snapshot();
  next.stage = Stage::kError;
  next.message = message;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  snapshot_ = next;
  hasPreviewCommand_ = false;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}
