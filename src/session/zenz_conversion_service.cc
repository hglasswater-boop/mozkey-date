#include "session/zenz_conversion_service.h"

#include <utility>

#include "absl/log/log.h"

namespace mozc {
namespace session {

ZenzConversionService::ZenzConversionService(std::unique_ptr<ZenzClient> client)
    : client_(std::move(client)) {}

ZenzConversionService::~ZenzConversionService() {
  Stop();
}

void ZenzConversionService::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) {
    return;
  }
  stop_ = false;
  started_ = true;
  worker_ = std::thread(&ZenzConversionService::WorkerLoop, this);
}

void ZenzConversionService::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) {
      return;
    }
    stop_ = true;
    latest_request_.reset();
    latest_result_.reset();
  }
  cv_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }

  std::lock_guard<std::mutex> lock(mu_);
  started_ = false;
}

void ZenzConversionService::Submit(ZenzConversionRequest request) {
  Start();

  {
    std::lock_guard<std::mutex> lock(mu_);
    latest_request_ = std::move(request);
    latest_result_.reset();
  }
  cv_.notify_one();
}

void ZenzConversionService::CancelPending() {
  std::lock_guard<std::mutex> lock(mu_);
  latest_request_.reset();
  latest_result_.reset();
}

std::optional<ZenzConversionResponse> ZenzConversionService::TakeResult(
    uint32_t generation) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!latest_result_.has_value()) {
    return std::nullopt;
  }
  if (latest_result_->generation != generation) {
    latest_result_.reset();
    return std::nullopt;
  }

  std::optional<ZenzConversionResponse> result = std::move(latest_result_);
  latest_result_.reset();
  return result;
}

void ZenzConversionService::WorkerLoop() {
  while (true) {
    std::optional<ZenzConversionRequest> request;

    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] {
        return stop_ || latest_request_.has_value();
      });

      if (stop_) {
        return;
      }

      request = std::move(latest_request_);
      latest_request_.reset();
    }

    if (!request.has_value()) {
      continue;
    }

    ZenzConversionResponse response;
    response.generation = request->generation;
    response.key = request->key;

    if (!client_ || !client_->IsAvailable()) {
      response.ok = false;
      response.debug = "zenz_client_unavailable";
    } else {
      response = client_->Convert(*request);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);

      // If a newer request is already queued, this result is still stored;
      // Session will drop it by generation. Keeping it is useful for debugging.
      latest_result_ = std::move(response);
    }
  }
}

}  // namespace session
}  // namespace mozc
