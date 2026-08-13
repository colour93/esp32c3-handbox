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
constexpr uint8_t kMaxConsecutivePollErrors = 3;

class SemaphoreLock {
 public:
  explicit SemaphoreLock(void* mutex)
      : mutex_(static_cast<SemaphoreHandle_t>(mutex)) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }

  ~SemaphoreLock() { xSemaphoreGive(mutex_); }

  SemaphoreLock(const SemaphoreLock&) = delete;
  SemaphoreLock& operator=(const SemaphoreLock&) = delete;

 private:
  SemaphoreHandle_t mutex_;
};

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

bool hostMatches(String configured, String requested) {
  configured.trim();
  requested.trim();
  while (configured.endsWith(".")) {
    configured.remove(configured.length() - 1);
  }
  while (requested.endsWith(".")) {
    requested.remove(requested.length() - 1);
  }
  configured.toLowerCase();
  requested.toLowerCase();
  return configured == requested;
}

bool resolveConfiguredHost(const AppConfig::DroneConfig& config,
                           const char* host, IPAddress& address) {
  for (size_t index = 0; index < config.hostCount; ++index) {
    if (!hostMatches(config.hosts[index].hostname, host)) {
      continue;
    }
    if (!address.fromString(config.hosts[index].address)) {
      Serial.printf("[ci] hosts entry ignored: %s has invalid address\n", host);
      return false;
    }
    Serial.printf("[ci] hosts override: %s -> %s\n", host,
                  address.toString().c_str());
    return true;
  }
  return false;
}

class HostsClient : public NetworkClient {
 public:
  explicit HostsClient(const AppConfig::DroneConfig& config) : config_(config) {}

  int connect(const char* host, uint16_t port, int32_t timeout) override {
    IPAddress address;
    if (resolveConfiguredHost(config_, host, address)) {
      return NetworkClient::connect(address, port, timeout);
    }
    return NetworkClient::connect(host, port, timeout);
  }

 private:
  const AppConfig::DroneConfig& config_;
};

class HostsSecureClient : public NetworkClientSecure {
 public:
  explicit HostsSecureClient(const AppConfig::DroneConfig& config)
      : config_(config) {}

  int connect(const char* host, uint16_t port, int32_t timeout) override {
    IPAddress address;
    if (!resolveConfiguredHost(config_, host, address)) {
      return NetworkClientSecure::connect(host, port, timeout);
    }
    _timeout = timeout;
    return NetworkClientSecure::connect(address, port, host, nullptr, nullptr,
                                        nullptr);
  }

 private:
  const AppConfig::DroneConfig& config_;
};

bool syncClock() {
  if (time(nullptr) > 1700000000) {
    return true;
  }
  Serial.println("[ci] synchronizing system time for TLS");
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  const uint32_t startedAt = millis();
  while (time(nullptr) <= 1700000000 &&
         millis() - startedAt < kTimeSyncTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return time(nullptr) > 1700000000;
}

struct HttpSession {
  explicit HttpSession(const AppConfig::DroneConfig& config)
      : plain(config), secure(config) {}

  HostsClient plain;
  HostsSecureClient secure;
  HTTPClient http;

  ~HttpSession() {
    http.end();
    secure.stop();
    plain.stop();
  }

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

int peekNextNonWhitespace(Stream& stream, uint32_t timeoutMs = 15000) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    const int character = stream.peek();
    if (character < 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    if (character == ' ' || character == '\t' || character == '\r' ||
        character == '\n') {
      stream.read();
      continue;
    }
    return character;
  }
  return -1;
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
  networkMutex_ = xSemaphoreCreateMutex();
  queue_ = xQueueCreate(3, sizeof(Command*));
  if (mutex_ == nullptr || networkMutex_ == nullptr || queue_ == nullptr) {
    return false;
  }
  const bool started =
      xTaskCreate(taskEntry, "handbox-ci", 12288, this, 1, nullptr) == pdPASS;
  Serial.printf("[ci] worker %s\n", started ? "started" : "start failed");
  return started;
}

bool CiService::requestUser(const AppConfig::DroneConfig& config) {
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress() ||
      config.baseUrl.isEmpty() || config.token.isEmpty()) {
    return false;
  }
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (userRequestInFlight_) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return true;
  }
  userRequestInFlight_ = true;
  username_ = "";
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));

  UserTaskContext* context = new UserTaskContext{this, config};
  if (context == nullptr ||
      xTaskCreate(userTaskEntry, "handbox-user", 12288, context, 1, nullptr) !=
          pdPASS) {
    delete context;
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    userRequestInFlight_ = false;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    Serial.println("[ci] user task unavailable");
    return false;
  }
  Serial.println("[ci] current user requested");
  return true;
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
  Serial.printf("[ci] preview requested: %s/%s branch=%s hosts=%u ssl=%s\n",
                target.namespaceName.c_str(), target.repo.c_str(),
                target.branch.c_str(), static_cast<unsigned>(config.hostCount),
                config.verifySsl ? "verify" : "insecure");
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
  Serial.printf("[ci] trigger confirmed: %s/%s branch=%s\n",
                command->target.namespaceName.c_str(),
                command->target.repo.c_str(), command->target.branch.c_str());
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

String CiService::username() const {
  if (mutex_ == nullptr) {
    return "";
  }
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const String result = username_;
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

void CiService::userTaskEntry(void* argument) {
  UserTaskContext* context = static_cast<UserTaskContext*>(argument);
  context->service->runUser(context->config);
  delete context;
  vTaskDelete(nullptr);
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

void CiService::runUser(const AppConfig::DroneConfig& config) {
  SemaphoreLock networkLock(networkMutex_);
  const String url = normalizedBaseUrl(config.baseUrl) + "/api/user";
  HttpSession session(config);
  String error;
  String login;
  if (session.begin(url, config.verifySsl, error)) {
    authorize(session.http, config.token);
    const int code = session.http.GET();
    Serial.printf("[ci] current user HTTP %d\n", code);
    if (successfulHttpCode(code)) {
      JsonDocument filter;
      filter["login"] = true;
      JsonDocument user;
      const DeserializationError jsonError = deserializeJson(
          user, session.http.getStream(), DeserializationOption::Filter(filter));
      if (!jsonError && user["login"].is<const char*>()) {
        login = user["login"].as<String>();
      } else {
        error = "current user response parse failed";
      }
    } else {
      error = httpError(code);
    }
  }

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (!login.isEmpty()) {
    username_ = login;
  }
  userRequestInFlight_ = false;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  if (!login.isEmpty()) {
    Serial.printf("[ci] current user: %s\n", login.c_str());
  } else {
    Serial.printf("[ci] current user unavailable: %s\n",
                  error.isEmpty() ? "unknown error" : error.c_str());
  }
}

void CiService::runPreview(const Command& command) {
  SemaphoreLock networkLock(networkMutex_);
  const String baseUrl = normalizedBaseUrl(command.config.baseUrl);
  const String url = baseUrl + repositoryPath(command.target) +
                     "?page=1&per_page=50";
  HttpSession session(command.config);
  String error;
  if (!session.begin(url, command.config.verifySsl, error)) {
    fail(error);
    return;
  }
  authorize(session.http, command.config.token);
  const int code = session.http.GET();
  Serial.printf("[ci] preview HTTP %d length=%d\n", code,
                session.http.getSize());
  if (!successfulHttpCode(code)) {
    fail(httpError(code));
    return;
  }

  JsonDocument filter;
  for (const char* field : {"ref", "source", "target", "after", "message",
                            "author_name", "author_login"}) {
    filter[field] = true;
  }

  Stream& input = session.http.getStream();
  if (!input.find("[")) {
    Serial.println("[ci] preview JSON array start not found");
    fail("最近构建解析失败");
    return;
  }

  size_t parsedCount = 0;
  while (parsedCount < 50) {
    const int next = peekNextNonWhitespace(input);
    if (next == ']') {
      input.read();
      break;
    }
    if (next != '{') {
      Serial.printf("[ci] preview unexpected JSON token=%d parsed=%u\n", next,
                    static_cast<unsigned>(parsedCount));
      fail("最近构建解析失败");
      return;
    }

    JsonDocument buildDocument;
    const DeserializationError jsonError = deserializeJson(
        buildDocument, input, DeserializationOption::Filter(filter));
    if (jsonError || !buildDocument.is<JsonObject>()) {
      Serial.printf("[ci] preview JSON error=%s parsed=%u\n",
                    jsonError.c_str(), static_cast<unsigned>(parsedCount));
      fail("最近构建解析失败");
      return;
    }
    parsedCount++;
    const JsonObjectConst build = buildDocument.as<JsonObjectConst>();
    if (!branchMatches(build, command.target.branch)) {
      const int delimiter = peekNextNonWhitespace(input);
      if (delimiter == ',') {
        input.read();
        continue;
      }
      if (delimiter == ']') {
        input.read();
        break;
      }
      Serial.printf("[ci] preview JSON delimiter=%d parsed=%u\n", delimiter,
                    static_cast<unsigned>(parsedCount));
      fail("最近构建解析失败");
      return;
    } else {
      Snapshot preview;
      preview.stage = Stage::kPreviewReady;
      preview.target = command.target;
      preview.shortSha = shortSha(build["after"] | "unknown");
      preview.author =
          build["author_name"] | build["author_login"] | "unknown";
      preview.title = firstLine(build["message"] | "(no title)");
      preview.message = "再按一次确认构建";
      Serial.printf("[ci] preview ready: sha=%s author=%s parsed=%u\n",
                    preview.shortSha.c_str(), preview.author.c_str(),
                    static_cast<unsigned>(parsedCount));
      xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
      previewCommand_ = command;
      previewCommand_.type = CommandType::kTrigger;
      hasPreviewCommand_ = true;
      snapshot_ = preview;
      xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
      return;
    }
  }
  Serial.printf("[ci] preview no branch match parsed=%u\n",
                static_cast<unsigned>(parsedCount));
  fail("最近 50 条中无该分支记录");
}

void CiService::runTrigger(const Command& command) {
  const String baseUrl = normalizedBaseUrl(command.config.baseUrl);
  const String buildPath = repositoryPath(command.target);
  const String triggerUrl = baseUrl + buildPath +
                            "?branch=" + urlEncode(command.target.branch);
  String error;
  uint64_t buildNumber = 0;
  String initialStatus = "pending";
  {
    SemaphoreLock networkLock(networkMutex_);
    HttpSession triggerSession(command.config);
    if (!triggerSession.begin(triggerUrl, command.config.verifySsl, error)) {
      fail(error);
      return;
    }
    authorize(triggerSession.http, command.config.token);
    triggerSession.http.addHeader("Content-Type", "application/json");
    const int triggerCode = triggerSession.http.POST("");
    Serial.printf("[ci] trigger HTTP %d heap=%u max=%u\n", triggerCode,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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
    buildNumber = triggered["number"].as<uint64_t>();
    initialStatus = triggered["status"] | "pending";
  }
  Serial.printf("[ci] monitoring build #%llu\n",
                static_cast<unsigned long long>(buildNumber));
  Snapshot monitoring = snapshot();
  monitoring.stage = Stage::kMonitoring;
  monitoring.buildNumber = buildNumber;
  monitoring.status = initialStatus;
  monitoring.message = "构建正在运行";
  update(monitoring);

  const uint32_t startedAt = millis();
  uint8_t consecutivePollErrors = 0;
  while (millis() - startedAt < kMonitorTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    if (WiFi.status() != WL_CONNECTED) {
      fail("监控期间 Wi-Fi 断开");
      return;
    }
    const String pollUrl = baseUrl + buildPath + "/" + String(buildNumber);
    JsonDocument build;
    int pollCode = 0;
    {
      SemaphoreLock networkLock(networkMutex_);
      HttpSession pollSession(command.config);
      if (!pollSession.begin(pollUrl, command.config.verifySsl, error)) {
        pollCode = HTTPC_ERROR_CONNECTION_REFUSED;
      } else {
        authorize(pollSession.http, command.config.token);
        pollCode = pollSession.http.GET();
        if (successfulHttpCode(pollCode) &&
            !parseBuild(pollSession.http.getStream(), build, error)) {
          fail(error);
          return;
        }
      }
    }
    if (!successfulHttpCode(pollCode)) {
      if (pollCode < 0 &&
          ++consecutivePollErrors <= kMaxConsecutivePollErrors) {
        Serial.printf(
            "[ci] poll transport error=%s retry=%u/%u heap=%u max=%u\n",
            HTTPClient::errorToString(pollCode).c_str(),
            static_cast<unsigned>(consecutivePollErrors),
            static_cast<unsigned>(kMaxConsecutivePollErrors),
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        continue;
      }
      fail(httpError(pollCode));
      return;
    }
    consecutivePollErrors = 0;
    monitoring.status = build["status"] | "unknown";
    monitoring.message = "构建 #" + String(buildNumber);
    if (monitoring.status != snapshot().status) {
      Serial.printf("[ci] build #%llu status=%s\n",
                    static_cast<unsigned long long>(buildNumber),
                    monitoring.status.c_str());
    }
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
  Serial.printf("[ci] error: %s\n", message.c_str());
  Snapshot next = snapshot();
  next.stage = Stage::kError;
  next.message = message;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  snapshot_ = next;
  hasPreviewCommand_ = false;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}
