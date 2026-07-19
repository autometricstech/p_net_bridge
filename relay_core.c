/*******************************************************************************
 * p_net_bridge — relay_core implementation. See relay_core.h for the wire
 * contract and configuration.
 *
 * Copyright (C) 2026 AutoMetrics Manufacturing Technologies Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "relay_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RELAY_RECV_BUF 2048
#define RELAY_FRAME_MAX 1024

#define FRAME_O2T 'O'
#define FRAME_T2O 'T'

#ifdef MSG_NOSIGNAL
#define RELAY_SEND_FLAGS MSG_NOSIGNAL
#else
#define RELAY_SEND_FLAGS 0
#endif

static RelayCallbacks g_callbacks;
static int g_listen_socket = -1;
static int g_client_socket = -1;
static unsigned char g_recv_buffer[RELAY_RECV_BUF];
static size_t g_recv_fill = 0;
static long long g_last_t_frame_ms = 0;
static int g_heartbeat_timeout_ms = 1000;
static uint8_t g_o2t_last_sent[RELAY_MAX_IMAGE];
static int g_o2t_baseline_valid = 0;
static long long g_o2t_last_sent_ms = 0;

/* resend an unchanged image at this interval so the mapper can judge
 * fieldbus liveness by frame recency (a static controller image would
 * otherwise go silent on the socket while the AR is alive) */
#define RELAY_O2T_KEEPALIVE_MS 1000

static long long RelayNowMs(void) {
  /* monotonic milliseconds for the heartbeat watchdog */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int RelayEnvInt(const char *name, int fallback) {
  /* read an integer env var, keeping the fallback on absence or garbage */
  const char *value = getenv(name);
  if (value == NULL || *value == '\0') {
    return fallback;
  }
  return atoi(value);
}

const char *RelayEnvStr(const char *name, const char *fallback) {
  /* read a string env var with a fallback */
  const char *value = getenv(name);
  return (value != NULL && *value != '\0') ? value : fallback;
}

static void RelaySetNonBlocking(int sock) {
  /* the cyclic thread must never block on the relay socket */
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
}

static int RelayOpenListenSocket(void) {
  /* bind the mapper-facing server socket per the configured transport */
  const char *transport = RelayEnvStr("BRIDGE_SOCKET_TRANSPORT", "uds");
  if (strcmp(transport, "uds") == 0) {
    const char *path = RelayEnvStr("BRIDGE_SOCKET_PATH",
                                   "/run/controller_bridge/bridge.sock");
    struct sockaddr_un addr;
    g_listen_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_socket < 0) {
      return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path);
    if (bind(g_listen_socket, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(g_listen_socket, 1) != 0) {
      close(g_listen_socket);
      g_listen_socket = -1;
      return -1;
    }
    printf("p_net_bridge: mapper socket listening on uds %s\n", path);
    fflush(stdout);
    RelaySetNonBlocking(g_listen_socket);
    return 0;
  }
  if (strcmp(transport, "tcp") == 0) {
    int port = RelayEnvInt("BRIDGE_SOCKET_PORT", 42223);
    struct sockaddr_in addr;
    int reuse = 1;
    g_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_socket < 0) {
      return -1;
    }
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char *) &reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((unsigned short) port);
    if (bind(g_listen_socket, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(g_listen_socket, 1) != 0) {
      close(g_listen_socket);
      g_listen_socket = -1;
      return -1;
    }
    printf("p_net_bridge: mapper socket listening on tcp 127.0.0.1:%d\n", port);
    fflush(stdout);
    RelaySetNonBlocking(g_listen_socket);
    return 0;
  }
  fprintf(stderr, "p_net_bridge: unsupported socket transport '%s'\n", transport);
  return -1;
}

static void RelayDropClient(void) {
  /* mapper gone (clean close, error, or bad frame) = hard-dead per the
   * failure model: notify the glue so it drops the fieldbus connection and
   * the controller faults via its own watchdog instead of reading a frozen
   * input image */
  if (g_client_socket >= 0) {
    close(g_client_socket);
    g_client_socket = -1;
    g_recv_fill = 0;
    printf("p_net_bridge: mapper disconnected\n");
    fflush(stdout);
    if (g_callbacks.on_client_lost != NULL) {
      g_callbacks.on_client_lost();
    }
  }
}

void RelayPublishO2T(const uint8_t *data, size_t length) {
  /* one length-prefixed 'O' frame when the image changed (or as a 1 s
   * keepalive); frames are tiny, a would-block send is dropped rather than
   * ever blocking the cyclic loop */
  unsigned char frame[5 + RELAY_MAX_IMAGE];
  unsigned long body = (unsigned long) length + 1;
  if (g_client_socket < 0 || length > RELAY_MAX_IMAGE) {
    return;
  }
  long long now_ms = RelayNowMs();
  if (g_o2t_baseline_valid && memcmp(g_o2t_last_sent, data, length) == 0 &&
      (now_ms - g_o2t_last_sent_ms) < RELAY_O2T_KEEPALIVE_MS) {
    return;
  }
  memcpy(g_o2t_last_sent, data, length);
  g_o2t_baseline_valid = 1;
  g_o2t_last_sent_ms = now_ms;
  frame[0] = (unsigned char) (body & 0xFF);
  frame[1] = (unsigned char) ((body >> 8) & 0xFF);
  frame[2] = (unsigned char) ((body >> 16) & 0xFF);
  frame[3] = (unsigned char) ((body >> 24) & 0xFF);
  frame[4] = FRAME_O2T;
  memcpy(&frame[5], data, length);
  if (send(g_client_socket, (const char *) frame, 5 + length,
           RELAY_SEND_FLAGS) < 0) {
    RelayDropClient();
  }
}

static void RelayHandleFrame(unsigned char kind, const unsigned char *payload,
                             size_t length) {
  /* 'T' frames carry the fieldbus input image and feed the heartbeat */
  if (kind == FRAME_T2O) {
    g_last_t_frame_ms = RelayNowMs();
    if (g_callbacks.on_t2o_frame != NULL) {
      g_callbacks.on_t2o_frame(payload, length);
    }
  }
}

int RelayClientConnected(void) {
  /* non-zero while a mapper client is attached */
  return g_client_socket >= 0;
}

void RelayService(void) {
  /* accept/read the mapper link; called from the cyclic loop */
  long long now = RelayNowMs();

  if (g_client_socket < 0 && g_listen_socket >= 0) {
    g_client_socket = accept(g_listen_socket, NULL, NULL);
    if (g_client_socket >= 0) {
      RelaySetNonBlocking(g_client_socket);
      g_recv_fill = 0;
      g_last_t_frame_ms = now;
      g_o2t_baseline_valid = 0; /* new client gets a full image on next publish */
      printf("p_net_bridge: mapper connected\n");
      fflush(stdout);
    }
  }
  if (g_client_socket < 0) {
    return;
  }

  for (;;) {
    ssize_t received = recv(g_client_socket,
                            (char *) g_recv_buffer + g_recv_fill,
                            sizeof(g_recv_buffer) - g_recv_fill, 0);
    if (received > 0) {
      g_recv_fill += (size_t) received;
      /* drain complete frames: 4-byte LE length, then type byte + payload */
      for (;;) {
        unsigned long body;
        if (g_recv_fill < 4) {
          break;
        }
        body = (unsigned long) g_recv_buffer[0] |
               ((unsigned long) g_recv_buffer[1] << 8) |
               ((unsigned long) g_recv_buffer[2] << 16) |
               ((unsigned long) g_recv_buffer[3] << 24);
        if (body == 0 || body > RELAY_FRAME_MAX) {
          printf("p_net_bridge: bad frame length %lu, dropping mapper\n", body);
          fflush(stdout);
          RelayDropClient();
          return;
        }
        if (g_recv_fill < 4 + body) {
          break;
        }
        RelayHandleFrame(g_recv_buffer[4], &g_recv_buffer[5], body - 1);
        memmove(g_recv_buffer, g_recv_buffer + 4 + body, g_recv_fill - 4 - body);
        g_recv_fill -= 4 + body;
      }
      continue;
    }
    if (received == 0) {
      RelayDropClient();
      return;
    }
    break; /* would-block: nothing more to read this tick */
  }

  /* mapper heartbeat watchdog: stale 'T' frames while connected mean the
   * mapper is hung — drop it so the glue aborts the fieldbus connection */
  if (g_heartbeat_timeout_ms > 0 &&
      now - g_last_t_frame_ms > g_heartbeat_timeout_ms) {
    printf("p_net_bridge: mapper heartbeat lost (> %d ms)\n",
           g_heartbeat_timeout_ms);
    fflush(stdout);
    RelayDropClient();
  }
}

int RelayInit(const RelayCallbacks *callbacks) {
  /* read the env config slice and open the mapper-facing socket */
  g_callbacks = *callbacks;
  g_heartbeat_timeout_ms = RelayEnvInt("BRIDGE_HEARTBEAT_TIMEOUT_MS", 1000);
  if (RelayOpenListenSocket() != 0) {
    fprintf(stderr, "p_net_bridge: could not open the mapper socket\n");
    return -1;
  }
  return 0;
}
