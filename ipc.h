#ifndef BAR_IPC_H
#define BAR_IPC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPC_MAX_PAYLOAD (16u * 1024u * 1024u)
typedef struct {
  int fd;
  unsigned char header[14];
  size_t header_used, body_used, tx_used, tx_size;
  uint32_t body_size, type, expected;
  char *body, *tx;
  bool pending;
  int64_t deadline;
} Ipc;

typedef bool (*IpcMessage)(void *, uint32_t, const char *);
int64_t monotonic_ms(void);
bool ipc_request(Ipc *ipc, uint32_t type, const char *payload);
bool ipc_write(Ipc *ipc);
bool ipc_read(Ipc *ipc, IpcMessage message, void *data);
bool ipc_expired(const Ipc *ipc);
void ipc_close(Ipc *ipc);
#endif
