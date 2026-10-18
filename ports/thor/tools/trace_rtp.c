#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

/* Test-only observer: no packet modification, buffering or forwarding. */
static ssize_t (*real_sendto)(int, const void*, size_t, int,
                              const struct sockaddr*, socklen_t);
static ssize_t (*real_recv)(int, void*, size_t, int);
static int log_fd = -1;
struct record {
  uint64_t ns;
  uint32_t bytes;
  uint16_t port;
  uint16_t header_bytes;
  unsigned char header[128];
};
_Static_assert(sizeof(struct record) == 144, "Unexpected record layout");

__attribute__((constructor)) static void initialize(void) {
  real_sendto = dlsym(RTLD_NEXT, "sendto");
  real_recv = dlsym(RTLD_NEXT, "recv");
  const char* path = getenv("MLVC_BENCH_PACKET_LOG");
  if (path)
    log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (path && log_fd < 0) abort();
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags,
               const struct sockaddr* address, socklen_t address_length) {
  ssize_t result =
      real_sendto(fd, buffer, length, flags, address, address_length);
  int saved_errno = errno;
  if (result >= 0 && log_fd >= 0 && address && address->sa_family == AF_INET) {
    const struct sockaddr_in* peer = (const struct sockaddr_in*)address;
    uint16_t port = ntohs(peer->sin_port);
    if (port >= 39480 && port <= 39504) {
      struct record item = {0};
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      item.ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
      item.bytes = (uint32_t)result;
      item.port = port;
      item.header_bytes = result < 128 ? result : 128;
      memcpy(item.header, buffer, item.header_bytes);
      if (write(log_fd, &item, sizeof(item)) != sizeof(item)) abort();
    }
  }
  errno = saved_errno;
  return result;
}

ssize_t recv(int fd, void* buffer, size_t length, int flags) {
  ssize_t result = real_recv(fd, buffer, length, flags);
  int saved_errno = errno;
  const unsigned char* head = buffer;
  if (result >= 36 && log_fd >= 0 && head[0] == 128 && head[12] == 1) {
    struct sockaddr_in local;
    socklen_t local_length = sizeof(local);
    if (getsockname(fd, (struct sockaddr*)&local, &local_length) == 0 &&
        local.sin_family == AF_INET) {
      uint16_t port = ntohs(local.sin_port);
      if (port >= 39480 && port <= 39489) {
        struct record item = {0};
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        item.ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
        item.bytes = (uint32_t)result;
        item.port = port;
        item.header_bytes = result < 128 ? result : 128;
        memcpy(item.header, buffer, item.header_bytes);
        if (write(log_fd, &item, sizeof(item)) != sizeof(item)) abort();
      }
    }
  }
  errno = saved_errno;
  return result;
}
