#ifndef MLVC_IO_RTSP_INPUT_H_
#define MLVC_IO_RTSP_INPUT_H_

#include <string_view>

namespace mlvc::io {

inline bool IsRtspInputUrl(std::string_view url) {
  std::size_t prefix = 0;
  if (url.substr(0, 7) == "rtsp://") prefix = 7;
  if (url.substr(0, 8) == "rtsps://") prefix = 8;
  if (prefix == 0 || url.find_first_of(" \t\r\n") != url.npos) return false;
  auto authority = url.substr(prefix);
  authority = authority.substr(0, authority.find_first_of("/?#"));
  const auto user = authority.rfind('@');
  if (user != authority.npos) authority.remove_prefix(user + 1);
  if (authority.empty()) return false;
  if (authority.front() == '[') {
    const auto end = authority.find(']');
    return end != authority.npos && end > 1 &&
           (end + 1 == authority.size() || authority[end + 1] == ':');
  }
  return !authority.substr(0, authority.find(':')).empty();
}

}  // namespace mlvc::io

#endif  // MLVC_IO_RTSP_INPUT_H_
