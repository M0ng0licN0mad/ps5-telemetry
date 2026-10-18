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

## Diagnostics
`GET /diag` tries a list of system counters and reports which ones this console allows
(`ok:true` with raw bytes, or `ok:false` with the errno). Run `curl http://<PS5-IP>:9090/diag`
and use the result to decide which sources work. Read-only.

Reported by ps5-exporter on 13.60: the `hw.physmem` and `hw.usermem` sysctls are refused for
payloads, so RAM may show n/a until a working source is found. CPU and GPU load, FPS and fan
speed are reported to come from kernel memory at firmware-specific offsets.

On 13.60 the kernel has no `kern.cp_time`, `hw.physmem` or `vm.stats.*` sysctls (errno 2, not found),
so CPU % and memory come from the process table (`KERN_PROC_PROC`): the process with the largest resident memory is treated as the game, CPU % is its run-time delta over total logical CPU capacity, and memory is its resident pages. 13.60 returns 1096-byte process records (SDK struct is 1088), so records are read by their declared length. Approach follows TacHen (GPL-3.0); no code copied.
- `GET /procs`: process-table probe (struct size check, top processes by run time and by resident memory).
- `GET /sysctls?q=mem`: lists sysctl names containing `mem` (names only). Try `q=gpu`, `q=cpu`, `q=dmem`.

## Roadmap
- Frame hook: in-game plugin that hooks the flip call and sends one UDP packet per flip.
- GPU busy %: sample the GPU status busy bit at high rate and report the busy fraction (radeontop method). Firmware-specific.
- Per-core CPU via `kern.cp_times`.
