/*******************************************************************************
 * p_net_bridge — relay_core: framed-socket byte relay between a fieldbus
 * adapter process and an external mapper process.
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
 *
 * Wire contract (fixed; the mapper on the other end is a separate program):
 *   stream socket (unix domain, or 127.0.0.1 tcp for dev), this process is
 *   the server. Frames both ways: 4-byte little-endian length (= 1 type byte
 *   + payload), then 'O' or 'T', then raw bytes.
 *     'O'  adapter -> mapper: fieldbus output image (controller writes)
 *     'T'  mapper -> adapter: fieldbus input image (controller reads);
 *          also the mapper's heartbeat — if it stalls, the adapter must
 *          drop the fieldbus connection (on_client_lost) so the controller
 *          faults rather than reading a stale image.
 *
 * Environment configuration:
 *   BRIDGE_SOCKET_TRANSPORT      "uds" (default) or "tcp"
 *   BRIDGE_SOCKET_PATH           uds path (default /run/controller_bridge/bridge.sock)
 *   BRIDGE_SOCKET_PORT           tcp loopback port (default 42223)
 *   BRIDGE_HEARTBEAT_TIMEOUT_MS  mapper-stall watchdog (default 1000, 0=off)
 ******************************************************************************/

#ifndef RELAY_CORE_H
#define RELAY_CORE_H

#include <stddef.h>
#include <stdint.h>

#define RELAY_MAX_IMAGE 512

typedef struct RelayCallbacks {
  /* a complete 'T' frame arrived (mapper -> fieldbus input image) */
  void (*on_t2o_frame)(const uint8_t *data, size_t length);
  /* mapper disconnected, sent garbage, or its heartbeat stalled */
  void (*on_client_lost)(void);
} RelayCallbacks;

/* read env config and open the listening socket; returns 0 on success */
int RelayInit(const RelayCallbacks *callbacks);

/* service accept/receive/watchdog; call from the cyclic loop, never blocks */
void RelayService(void);

/* send the output image as an 'O' frame if it changed since the last send;
 * the change baseline resets whenever a client (re)connects */
void RelayPublishO2T(const uint8_t *data, size_t length);

/* non-zero while a mapper client is attached */
int RelayClientConnected(void);

/* env helpers shared with the glue layer */
int RelayEnvInt(const char *name, int fallback);
const char *RelayEnvStr(const char *name, const char *fallback);

#endif /* RELAY_CORE_H */
