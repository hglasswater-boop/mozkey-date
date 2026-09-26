#ifndef MOZC_SESSION_ZENZ_NAMED_PIPE_CLIENT_H_
#define MOZC_SESSION_ZENZ_NAMED_PIPE_CLIENT_H_

#include "session/zenz_conversion_service.h"

namespace mozc {
namespace session {

class ZenzNamedPipeClient final : public ZenzClient {
 public:
  ZenzNamedPipeClient() = default;
  ~ZenzNamedPipeClient() override = default;

  bool IsAvailable() const override;
  ZenzConversionResponse Convert(const ZenzConversionRequest& request) override;
};

}  // namespace session
}  // namespace mozc

#endif  // MOZC_SESSION_ZENZ_NAMED_PIPE_CLIENT_H_
