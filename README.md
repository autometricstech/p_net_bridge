# p_net_bridge

A minimal Profinet IO-device byte relay. It exposes one output and one input
data module to a Profinet IO-controller and relays the raw bytes over a local
stream socket to a separate mapper process — nothing more. It contains no
application logic: no data interpretation, no protocol semantics beyond
Profinet itself.

Copyright (C) 2026 AutoMetrics Manufacturing Technologies Inc.
Licensed under the GNU General Public License v3.0 (see `LICENSE`).
The [p-net](https://github.com/rtlabs-com/p-net) Profinet stack (GPLv3) is
included as a pinned git submodule.

## How it works

```
Profinet IO-controller (PLC)
      |  cyclic Profinet RT
[p_net_bridge]   this program: p-net IO-device + framed-socket relay
      |  local stream socket ('O'/'T' frames, see below)
[mapper]         a separate program supplied by the user of this bridge
```

- Controller **output** data (controller -> device) is sent to the mapper as
  an `'O'` frame whenever it changes.
- `'T'` frames received from the mapper refresh the device **input** data
  (controller reads it cyclically). `'T'` frames double as the mapper's
  heartbeat: if they stall, the bridge aborts the active AR so the controller
  faults on its own watchdog — a stale input image is never served.

### Wire contract (the socket interface)

Stream socket; this program is the server (unix domain socket by default,
`127.0.0.1` TCP optional for development). Each frame, both directions:

```
4 bytes  little-endian length  (= 1 + payload length)
1 byte   frame type: 'O' (output image) or 'T' (input image)
N bytes  raw data image
```

Communication over this interface is ordinary inter-process communication;
programs on the other end of the socket are separate works.

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `BRIDGE_O2T_SIZE` | 64 | controller output data length (must match the GSDML module) |
| `BRIDGE_T2O_SIZE` | 160 | device input data length (must match the GSDML module) |
| `BRIDGE_NIC` | `eth0` | network interface for Profinet |
| `BRIDGE_STATION_NAME` | `p-net-bridge` | initial DCP station name |
| `BRIDGE_PNET_DATA_DIR` | `/var/lib/pnet` | writable dir for DCP persistence |
| `BRIDGE_SOCKET_TRANSPORT` | `uds` | `uds` or `tcp` |
| `BRIDGE_SOCKET_PATH` | `/run/controller_bridge/bridge.sock` | uds path |
| `BRIDGE_SOCKET_PORT` | `42223` | tcp loopback port (dev) |
| `BRIDGE_HEARTBEAT_TIMEOUT_MS` | `1000` | mapper-stall watchdog, 0 = off |

The station name assigned by the engineering tool over DCP takes precedence
and is persisted in `BRIDGE_PNET_DATA_DIR` (standard Profinet behavior).

## Build

```bash
git clone --recurse-submodules https://github.com/autometricstech/p_net_bridge.git
cd p_net_bridge && ./build.sh
# -> build/p_net_bridge
```

Requires cmake, make, gcc on a Linux host. Runs with `CAP_NET_RAW` (Profinet
RT is raw layer-2 traffic). The binary prints its git tag, the pinned p-net
commit, and this repository URL at startup.

## GSDML

`gsdml/` contains the device description the controller's engineering tool
imports. The module data lengths in it must match `BRIDGE_O2T_SIZE` /
`BRIDGE_T2O_SIZE`. The vendor/device identification is a placeholder pending
a PROFIBUS & PROFINET International vendor ID.

## Releases

Every released binary is built from a public tag of this repository; tags are
permanent. Corresponding Source for any distributed binary is this repository
at the tag the binary reports at startup.
