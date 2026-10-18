# ps5-telemetry

A PS5 payload that serves a live system-usage dashboard over your LAN.
Target: jailbroken PS5, firmware 13.60 (built with ps5-payload-sdk).

## Use
1. Download `telemetry.elf` from Releases or the Actions artifacts (or `make` locally).
2. Send it to your payload loader (default ELF loader port is usually 9021).
3. On the same network open `http://<PS5-IP>:9090` on a phone or PC.

Endpoints: `GET /` dashboard, `GET /stats` JSON.

## Build locally
Install ps5-payload-sdk, then: `export PS5_PAYLOAD_SDK=/path/to/sdk && make`

## Status
| Metric | State |
|---|---|
| CPU | implemented (`kern.cp_time`) |
| RAM | implemented via FreeBSD VM sysctls; may not reflect game memory pools |
| Frame time | receiver done: send a UDP packet to port 9091 per presented frame |
| GPU | stub, returns null |

Test frame time without a hook: `while true; do echo . | nc -u -w0 <PS5-IP> 9091; sleep 0.0166; done`

## Roadmap
- Frame hook: in-game plugin that hooks the flip call and sends one UDP packet per flip.
- GPU busy %: sample the GPU status busy bit at high rate and report the busy fraction (radeontop method). Firmware-specific.
- Per-core CPU via `kern.cp_times`.
