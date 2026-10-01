#define _GNU_SOURCE
#include "ipc.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned messages;
static bool received(void *data, uint32_t type, const char *body) {
  assert(type == 4 || type == (0x80000000u | 3));
  assert(!strcmp(body, "{}"));
  messages++;
  return true;
}

static void pair(Ipc *ipc, int *peer) {
  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
  *ipc = (Ipc){.fd = fds[0]};
  *peer = fds[1];
}

static void frame(unsigned char *out, uint32_t size, uint32_t type) {
  memcpy(out, "i3-ipc", 6);
  memcpy(out + 6, &size, 4);
  memcpy(out + 10, &type, 4);
  memcpy(out + 14, "{}", 2);
}

int main(void) {
  Ipc ipc;
  int peer;
  unsigned char bytes[16], request[14];
  pair(&ipc, &peer);
  assert(ipc_request(&ipc, 4, NULL));
  assert(!ipc_request(&ipc, 4, NULL));
  assert(ipc_write(&ipc));
  assert(read(peer, request, sizeof(request)) == sizeof(request));
  assert(!memcmp(request, "i3-ipc", 6));
  /* Every possible byte boundary: incomplete input must return immediately. */
  frame(bytes, 2, 4);
  for (unsigned i = 0; i < sizeof(bytes); i++) {
    assert(write(peer, bytes + i, 1) == 1);
    assert(ipc_read(&ipc, received, NULL));
    assert(messages == (i == sizeof(bytes) - 1 ? 1u : 0u));
  }
  assert(!ipc.pending);
  frame(bytes, 2, 0x80000000u | 3);
  for (int i = 0; i < 100; i++)
    assert(write(peer, bytes, sizeof(bytes)) == sizeof(bytes));
  assert(ipc_read(&ipc, received, NULL));
  assert(messages == 65); /* Fairness limit is 64 messages per dispatch. */
  assert(ipc_read(&ipc, received, NULL));
  assert(messages == 101);
  close(peer);
  assert(!ipc_read(&ipc, received, NULL));
  ipc_close(&ipc);

  for (int invalid = 0; invalid < 3; invalid++) {
    pair(&ipc, &peer);
    frame(bytes, invalid == 1 ? IPC_MAX_PAYLOAD + 1 : 2, 4);
    if (invalid == 0)
      bytes[0] = 'x';
    /* Case 2 is a valid frame with an unsolicited reply type. */
    assert(write(peer, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(!ipc_read(&ipc, received, NULL));
    close(peer);
    ipc_close(&ipc);
  }

  pair(&ipc, &peer);
  assert(ipc_request(&ipc, 4, NULL));
  assert(!ipc_expired(&ipc));
  ipc.deadline = monotonic_ms() - 1;
  assert(ipc_expired(&ipc));
  close(peer);
  assert(!ipc_write(&ipc)); /* A broken pipe must not kill the process. */
  ipc_close(&ipc);

  /* Force partial writes and EAGAIN, then reconstruct the exact wire frame. */
  pair(&ipc, &peer);
  int small = 1024;
  assert(setsockopt(ipc.fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
  size_t size = 65536;
  char *payload = malloc(size + 1), *wire = malloc(size + 14);
  assert(payload && wire);
  memset(payload, 'a', size);
  payload[size] = 0;
  assert(ipc_request(&ipc, 4, payload));
  assert(ipc_write(&ipc) && ipc.tx_size);
  size_t used = 0;
  while (used < size + 14) {
    ssize_t n = read(peer, wire + used, size + 14 - used);
    if (n > 0)
      used += n;
    else
      assert(n < 0 && errno == EAGAIN);
    assert(ipc_write(&ipc));
  }
  assert(!memcmp(wire, "i3-ipc", 6));
  assert(!memcmp(wire + 14, payload, size));
  free(payload);
  free(wire);
  close(peer);
  ipc_close(&ipc);
  puts("IPC fragmentation, validation, fairness, deadlines, and backpressure "
       "passed.");
}
