#ifndef MOZC_SESSION_ZENZ_CONVERSION_SERVICE_H_
#define MOZC_SESSION_ZENZ_CONVERSION_SERVICE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <condition_variable>
#include <mutex>

#include "absl/time/time.h"

namespace mozc {
namespace session {

struct ZenzConversionRequest {
  uint32_t generation = 0;

  // Original Mozc reading, usually hiragana.
  std::string key;

  // zenz prompt:
  //   U+EE02 + left_context + U+EE00 + reading_katakana + U+EE01
  std::string prompt;

  std::string reading_katakana;
  std::string left_context;

  // Standard Mozc candidate retained as the fallback result.
  std::string mozc_value;

  // Runtime options.
  std::string pipe_name;
  uint32_t timeout_msec = 180;
  uint32_t max_output_chars = 128;

  absl::Time issued_at;
};

struct ZenzConversionResponse {
  uint32_t generation = 0;
  std::string key;
  std::string value;
  std::string debug;

  bool ok = false;
  bool timeout = false;

  absl::Duration latency = absl::ZeroDuration();
};

class ZenzClient {
 public:
  virtual ~ZenzClient() = default;

  virtual bool IsAvailable() const = 0;

  virtual ZenzConversionResponse Convert(const ZenzConversionRequest& request) = 0;
};

class ZenzConversionService {
 public:
  explicit ZenzConversionService(std::unique_ptr<ZenzClient> client);
  ~ZenzConversionService();

  ZenzConversionService(const ZenzConversionService&) = delete;
  ZenzConversionService& operator=(const ZenzConversionService&) = delete;

  void Start();
  void Stop();

  // Latest-only submission. A queued old request is overwritten.
  void Submit(ZenzConversionRequest request);

  // Clears queued request/result. Running inference is not forcibly cancelled;
  // stale discard is handled by generation check.
  void CancelPending();

  // Returns the latest result only if its generation matches.
  std::optional<ZenzConversionResponse> TakeResult(uint32_t generation);

 private:
  void WorkerLoop();

  std::unique_ptr<ZenzClient> client_;

  std::mutex mu_;
  std::condition_variable cv_;

  bool started_ = false;
  bool stop_ = false;

  std::optional<ZenzConversionRequest> latest_request_;
  std::optional<ZenzConversionResponse> latest_result_;

  std::thread worker_;
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ZENZ_CONVERSION_SERVICE_H_
