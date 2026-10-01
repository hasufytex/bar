#define _GNU_SOURCE
#include "ipc.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool ipc_request(Ipc *ipc, uint32_t type, const char *payload) {
  size_t size = payload ? strlen(payload) : 0;
  if (ipc->pending || size > IPC_MAX_PAYLOAD)
    return false;
  ipc->tx = malloc(14 + size);
  if (!ipc->tx)
    return false;
  uint32_t len = size;
  memcpy(ipc->tx, "i3-ipc", 6);
  memcpy(ipc->tx + 6, &len, 4);
  memcpy(ipc->tx + 10, &type, 4);
  if (size)
    memcpy(ipc->tx + 14, payload, size);
  ipc->tx_used = 0;
  ipc->tx_size = 14 + size;
  ipc->expected = type;
  ipc->pending = true;
  ipc->deadline = monotonic_ms() + 3000;
  return true;
}

bool ipc_write(Ipc *ipc) {
  while (ipc->tx_used < ipc->tx_size) {
    ssize_t n = send(ipc->fd, ipc->tx + ipc->tx_used,
                     ipc->tx_size - ipc->tx_used, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true;
    if (n <= 0)
      return false;
    ipc->tx_used += n;
  }
  free(ipc->tx);
  ipc->tx = NULL;
  ipc->tx_size = ipc->tx_used = 0;
  return true;
}

bool ipc_read(Ipc *ipc, IpcMessage message, void *data) {
  /* Bound work per poll iteration, including very large partial messages. */
  size_t budget = 256 * 1024;
  for (unsigned count = 0; count < 64 && budget;) {
    bool header = ipc->header_used < sizeof(ipc->header);
    size_t left = header ? sizeof(ipc->header) - ipc->header_used
                         : ipc->body_size - ipc->body_used;
    if (left) {
      void *dest = header ? (void *)(ipc->header + ipc->header_used)
                          : (void *)(ipc->body + ipc->body_used);
      ssize_t n = recv(ipc->fd, dest, left < budget ? left : budget, 0);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return true;
      if (n <= 0)
        return false;
      budget -= n;
      if (header)
        ipc->header_used += n;
      else
        ipc->body_used += n;
    }
    if (ipc->header_used < sizeof(ipc->header))
      continue;
    if (!ipc->body) {
      if (memcmp(ipc->header, "i3-ipc", 6))
        return false;
      memcpy(&ipc->body_size, ipc->header + 6, 4);
      memcpy(&ipc->type, ipc->header + 10, 4);
      if (ipc->body_size > IPC_MAX_PAYLOAD)
        return false;
      ipc->body = malloc((size_t)ipc->body_size + 1);
      if (!ipc->body)
        return false;
    }
    if (ipc->body_used != ipc->body_size)
      continue;
    ipc->body[ipc->body_size] = '\0';
    if (!(ipc->type & 0x80000000u)) {
      if (!ipc->pending || ipc->type != ipc->expected || ipc->tx_size)
        return false;
      ipc->pending = false;
    }
    bool ok = message(data, ipc->type, ipc->body);
    free(ipc->body);
    ipc->body = NULL;
    ipc->header_used = ipc->body_used = ipc->body_size = 0;
    if (!ok)
      return false;
    count++;
  }
  return true;
}

bool ipc_expired(const Ipc *ipc) {
  return ipc->pending && monotonic_ms() >= ipc->deadline;
}

void ipc_close(Ipc *ipc) {
  if (ipc->fd >= 0)
    close(ipc->fd);
  free(ipc->body);
  free(ipc->tx);
  *ipc = (Ipc){.fd = -1};
}
