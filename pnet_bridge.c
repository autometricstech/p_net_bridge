/*******************************************************************************
 * p_net_bridge — Profinet IO-device glue: p-net stack <-> relay_core frames.
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
 * Pure byte relay: no business logic here. The controller's OUTPUT data
 * (controller -> device) is relayed to the mapper as 'O' frames when it
 * changes; 'T' frames from the mapper refresh the device INPUT data
 * (controller reads). If the mapper heartbeat stalls, the active AR is
 * aborted so the controller faults — never a frozen input image.
 *
 * Environment (the entrypoint derives these from system_config.json):
 *   BRIDGE_O2T_SIZE          controller output data length (default 64)
 *   BRIDGE_T2O_SIZE          device input data length (default 160)
 *   BRIDGE_NIC               network interface for Profinet (default eth0)
 *   BRIDGE_STATION_NAME      initial DCP station name (default p-net-bridge)
 *   BRIDGE_PNET_DATA_DIR     writable dir for DCP persistence (default /var/lib/pnet)
 *   + the BRIDGE_SOCKET_* / BRIDGE_HEARTBEAT_TIMEOUT_MS slice (relay_core.h)
 *
 * NOTE (tier 0): written against the pinned p-net tag in ./p-net; exact
 * callback signatures and cfg fields are validated during bring-up builds —
 * treat compile errors here as pin-mismatch, fix against the pinned headers.
 ******************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pnal.h"
#include "pnet_api.h"
#include "relay_core.h"

/* one DAP + one IO module in slot 1 subslot 1; idents must match the GSDML */
#define APP_API 0
#define APP_SLOT_DAP 0
#define APP_SLOT_IO 1
#define APP_SUBSLOT_IO 1
#define APP_MODULE_ID_IO 0x00000100
#define APP_SUBMODULE_ID_IO 0x00000101

#define APP_TICK_US 1000

static pnet_t *g_net = NULL;
static uint32_t g_arep = UINT32_MAX;
static bool g_ar_ready = false;

static uint8_t g_o2t_data[RELAY_MAX_IMAGE]; /* controller output -> mapper */
static uint8_t g_t2o_data[RELAY_MAX_IMAGE]; /* mapper -> controller input */
static uint16_t g_o2t_size = 64;
static uint16_t g_t2o_size = 160;

/* ---- relay callbacks ------------------------------------------------- */

static void AppOnT2oFrame(const uint8_t *data, size_t length) {
  /* refresh the device input image from the mapper's 'T' frame */
  size_t count = length < (size_t) g_t2o_size ? length : (size_t) g_t2o_size;
  memcpy(g_t2o_data, data, count);
}

static void AppOnClientLost(void) {
  /* mapper hard-dead: abort the AR so the controller faults */
  if (g_net != NULL && g_arep != UINT32_MAX) {
    printf("p_net_bridge: aborting AR after mapper loss\n");
    fflush(stdout);
    pnet_ar_abort(g_net, g_arep);
  }
}

/* ---- p-net callbacks (all no-op except lifecycle + plugging) ---------- */

static int AppConnectInd(pnet_t *net, void *arg, uint32_t arep,
                         pnet_result_t *result) {
  /* controller opened an AR */
  (void) net; (void) arg; (void) result;
  g_arep = arep;
  g_ar_ready = false;
  printf("p_net_bridge: AR connect (arep %u)\n", (unsigned) arep);
  fflush(stdout);
  return 0;
}

static int AppReleaseInd(pnet_t *net, void *arg, uint32_t arep,
                         pnet_result_t *result) {
  /* controller released the AR */
  (void) net; (void) arg; (void) result;
  if (arep == g_arep) {
    g_arep = UINT32_MAX;
    g_ar_ready = false;
  }
  printf("p_net_bridge: AR release\n");
  fflush(stdout);
  return 0;
}

static int AppDcontrolInd(pnet_t *net, void *arg, uint32_t arep,
                          pnet_control_command_t control_command,
                          pnet_result_t *result) {
  (void) net; (void) arg; (void) arep; (void) control_command; (void) result;
  return 0;
}

static int AppCcontrolCnf(pnet_t *net, void *arg, uint32_t arep,
                          pnet_result_t *result) {
  (void) net; (void) arg; (void) arep; (void) result;
  return 0;
}

static int AppStateInd(pnet_t *net, void *arg, uint32_t arep,
                       pnet_event_values_t event) {
  /* signal application-ready at PRMEND; clear state on ABORT */
  (void) arg;
  if (event == PNET_EVENT_PRMEND) {
    pnet_input_set_data_and_iops(net, APP_API, APP_SLOT_IO, APP_SUBSLOT_IO,
                                 g_t2o_data, g_t2o_size, PNET_IOXS_GOOD);
    pnet_set_provider_state(net, true);
    pnet_application_ready(net, arep);
    g_ar_ready = true;
    printf("p_net_bridge: AR ready — cyclic exchange running\n");
    fflush(stdout);
  } else if (event == PNET_EVENT_ABORT) {
    if (arep == g_arep) {
      g_arep = UINT32_MAX;
      g_ar_ready = false;
    }
    printf("p_net_bridge: AR abort\n");
    fflush(stdout);
  }
  return 0;
}

static int AppExpModuleInd(pnet_t *net, void *arg, uint32_t api,
                           uint16_t slot, uint32_t module_ident) {
  /* accept the expected module by plugging it */
  (void) arg;
  return pnet_plug_module(net, api, slot, module_ident);
}

static int AppExpSubmoduleInd(pnet_t *net, void *arg, uint32_t api,
                              uint16_t slot, uint16_t subslot,
                              uint32_t module_ident, uint32_t submodule_ident,
                              const pnet_data_cfg_t *exp_data) {
  /* accept the expected submodule with the negotiated data lengths */
  (void) arg;
  return pnet_plug_submodule(net, api, slot, subslot, module_ident,
                             submodule_ident, exp_data->data_dir,
                             exp_data->insize, exp_data->outsize);
}

static int AppReadInd(pnet_t *net, void *arg, uint32_t arep, uint32_t api,
                      uint16_t slot, uint16_t subslot, uint16_t idx,
                      uint16_t sequence_number, uint8_t **read_data,
                      uint16_t *read_length, pnet_result_t *result) {
  (void) net; (void) arg; (void) arep; (void) api; (void) slot; (void) subslot;
  (void) idx; (void) sequence_number; (void) read_data; (void) read_length;
  (void) result;
  return -1; /* no application-specific records */
}

static int AppWriteInd(pnet_t *net, void *arg, uint32_t arep, uint32_t api,
                       uint16_t slot, uint16_t subslot, uint16_t idx,
                       uint16_t sequence_number, uint16_t write_length,
                       const uint8_t *write_data, pnet_result_t *result) {
  (void) net; (void) arg; (void) arep; (void) api; (void) slot; (void) subslot;
  (void) idx; (void) sequence_number; (void) write_length; (void) write_data;
  (void) result;
  return -1; /* no application-specific records */
}

static int AppNewDataStatusInd(pnet_t *net, void *arg, uint32_t arep,
                               uint32_t crep, uint8_t changes,
                               uint8_t data_status) {
  (void) net; (void) arg; (void) arep; (void) crep; (void) changes;
  (void) data_status;
  return 0;
}

static int AppAlarmInd(pnet_t *net, void *arg, uint32_t arep,
                       const pnet_alarm_argument_t *alarm_argument,
                       uint16_t data_len, uint16_t data_usi,
                       const uint8_t *data) {
  (void) net; (void) arg; (void) arep; (void) alarm_argument; (void) data_len;
  (void) data_usi; (void) data;
  return 0;
}

static int AppAlarmCnf(pnet_t *net, void *arg, uint32_t arep,
                       const pnet_pnio_status_t *status) {
  (void) net; (void) arg; (void) arep; (void) status;
  return 0;
}

static int AppAlarmAckCnf(pnet_t *net, void *arg, uint32_t arep, int res) {
  (void) net; (void) arg; (void) arep; (void) res;
  return 0;
}

static int AppResetInd(pnet_t *net, void *arg, bool should_reset_application,
                       uint16_t reset_mode) {
  (void) net; (void) arg; (void) should_reset_application; (void) reset_mode;
  return 0;
}

static int AppSignalLedInd(pnet_t *net, void *arg, bool led_state) {
  /* DCP "flash LED" — log it so commissioning engineers get feedback */
  (void) net; (void) arg;
  printf("p_net_bridge: signal LED %s\n", led_state ? "ON" : "OFF");
  fflush(stdout);
  return 0;
}

/* ---- main ------------------------------------------------------------ */

static void AppCopyIp(pnet_cfg_ip_addr_t *destination, pnal_ipaddr_t ip) {
  /* pnal addresses are host-order uint32; a is the most significant byte */
  destination->a = (ip >> 24) & 0xFF;
  destination->b = (ip >> 16) & 0xFF;
  destination->c = (ip >> 8) & 0xFF;
  destination->d = ip & 0xFF;
}

int main(void) {
  pnet_cfg_t cfg;
  const char *nic = RelayEnvStr("BRIDGE_NIC", "eth0");
  const char *station_name = RelayEnvStr("BRIDGE_STATION_NAME", "p-net-bridge");
  const char *data_dir = RelayEnvStr("BRIDGE_PNET_DATA_DIR", "/var/lib/pnet");
  RelayCallbacks relay_callbacks = {AppOnT2oFrame, AppOnClientLost};

  g_o2t_size = (uint16_t) RelayEnvInt("BRIDGE_O2T_SIZE", 64);
  g_t2o_size = (uint16_t) RelayEnvInt("BRIDGE_T2O_SIZE", 160);
  if (g_o2t_size == 0 || g_o2t_size > RELAY_MAX_IMAGE ||
      g_t2o_size == 0 || g_t2o_size > RELAY_MAX_IMAGE) {
    fprintf(stderr, "p_net_bridge: data sizes out of range (max %d)\n",
            RELAY_MAX_IMAGE);
    return EXIT_FAILURE;
  }

  printf("p_net_bridge %s (p-net %s) — source: %s\n",
         PNET_BRIDGE_VERSION, PNET_BRIDGE_PNET_SHA, PNET_BRIDGE_SOURCE_URL);
  printf("p_net_bridge: nic=%s station=%s o2t=%u t2o=%u\n",
         nic, station_name, (unsigned) g_o2t_size, (unsigned) g_t2o_size);
  fflush(stdout);

  if (RelayInit(&relay_callbacks) != 0) {
    return EXIT_FAILURE;
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.state_cb = AppStateInd;
  cfg.connect_cb = AppConnectInd;
  cfg.release_cb = AppReleaseInd;
  cfg.dcontrol_cb = AppDcontrolInd;
  cfg.ccontrol_cb = AppCcontrolCnf;
  cfg.read_cb = AppReadInd;
  cfg.write_cb = AppWriteInd;
  cfg.exp_module_cb = AppExpModuleInd;
  cfg.exp_submodule_cb = AppExpSubmoduleInd;
  cfg.new_data_status_cb = AppNewDataStatusInd;
  cfg.alarm_ind_cb = AppAlarmInd;
  cfg.alarm_cnf_cb = AppAlarmCnf;
  cfg.alarm_ack_cnf_cb = AppAlarmAckCnf;
  cfg.reset_cb = AppResetInd;
  cfg.signal_led_cb = AppSignalLedInd;
  cfg.tick_us = APP_TICK_US;
  snprintf(cfg.station_name, sizeof(cfg.station_name), "%s", station_name);
  snprintf(cfg.file_directory, sizeof(cfg.file_directory), "%s", data_dir);
  cfg.min_device_interval = 32; /* 1 ms, must match the GSDML */
  cfg.num_physical_ports = 1;
  cfg.if_cfg.main_netif_name = nic;
  cfg.if_cfg.physical_ports[0].netif_name = nic;
  {
    /* take the interface's current addressing as the initial IP config */
    pnal_ipaddr_t ip = pnal_get_ip_address(nic);
    pnal_ipaddr_t netmask = pnal_get_netmask(nic);
    pnal_ipaddr_t gateway = pnal_get_gateway(nic);
    AppCopyIp(&cfg.if_cfg.ip_cfg.ip_addr, ip);
    AppCopyIp(&cfg.if_cfg.ip_cfg.ip_mask, netmask);
    AppCopyIp(&cfg.if_cfg.ip_cfg.ip_gateway, gateway);
  }
  /* identification: placeholder vendor/device until a PI vendor id exists */
  cfg.device_id.vendor_id_hi = 0xFE;
  cfg.device_id.vendor_id_lo = 0xED;
  cfg.device_id.device_id_hi = 0xBE;
  cfg.device_id.device_id_lo = 0xEF;
  snprintf(cfg.product_name, sizeof(cfg.product_name), "p_net_bridge");
  snprintf(cfg.im_0_data.im_order_id, sizeof(cfg.im_0_data.im_order_id),
           "P-NET-BRIDGE");
  snprintf(cfg.im_0_data.im_serial_number,
           sizeof(cfg.im_0_data.im_serial_number), "00001");

  g_net = pnet_init(&cfg);
  if (g_net == NULL) {
    fprintf(stderr, "p_net_bridge: pnet_init failed (nic %s)\n", nic);
    return EXIT_FAILURE;
  }

  for (;;) {
    usleep(APP_TICK_US);
    pnet_handle_periodic(g_net);
    RelayService();

    if (g_ar_ready) {
      /* controller output -> mapper (on change) */
      uint8_t iops = PNET_IOXS_BAD;
      uint16_t length = g_o2t_size;
      bool is_new = false;
      if (pnet_output_get_data_and_iops(g_net, APP_API, APP_SLOT_IO,
                                        APP_SUBSLOT_IO, &is_new,
                                        g_o2t_data, &length, &iops) == 0 &&
          iops == PNET_IOXS_GOOD) {
        RelayPublishO2T(g_o2t_data, length);
      }
      /* mapper image -> controller input (every tick, doubles as freshness) */
      pnet_input_set_data_and_iops(g_net, APP_API, APP_SLOT_IO,
                                   APP_SUBSLOT_IO, g_t2o_data, g_t2o_size,
                                   PNET_IOXS_GOOD);
    }
  }

  return EXIT_SUCCESS;
}
