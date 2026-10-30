# Optical Transceiver Telemetry & Alarm Agent

[![CI](https://github.com/prafullf/optical-telemetry-agent/actions/workflows/ci.yml/badge.svg)](https://github.com/prafullf/optical-telemetry-agent/actions/workflows/ci.yml)

A multi-threaded **C++17** agent that polls Digital Diagnostic Monitoring (DDM)
telemetry from optical transceiver ports — Rx/Tx optical power, OSNR,
pre-FEC BER and module temperature — and raises/clears network alarms the way
an optical network element does: with **hysteresis**, **debouncing** and
**root-cause masking** (a Loss of Signal hides the alarms it causes).

Ports are simulated (seeded, reproducible, with scripted fault injection) behind
an abstract `Transceiver` interface, so a real driver reading module registers
over I2C/MDIO could be dropped in without touching the pipeline.

## Architecture

```
                 ┌──────────────┐  shard by port   ┌───────────────────┐
  Transceiver ──▶│ poller thread│──(port % W)─────▶│ BoundedQueue<Sample>│──▶ worker 0 ─┐
  ports 0..N-1   │   × P        │                  │   × W (one/worker)  │──▶ worker 1 ─┤
  (DDM reads)    └──────────────┘                  └───────────────────┘   ...        │
                         │                                         each owns an AlarmEngine
                         └──▶ BoundedQueue<Sample> ─▶ recorder ─▶ telemetry.jsonl     │
                                                                                      ▼
                         ActiveAlarmTable (shared_mutex) ◀── BoundedQueue<AlarmEvent> ─▶ publisher ─▶ alarms.jsonl
```

Design decisions worth calling out:

| Decision | Why |
|---|---|
| **Port-affinity sharding** (`port % workers`) | All samples of a port go to the same worker, so per-port alarm state needs **no locks** and alarm order per port is guaranteed. |
| **Bounded blocking queues** (mutex + 2 condition variables) | Back-pressure instead of unbounded memory growth when consumers fall behind; `pop_batch` drains up to 256 items per lock acquisition. |
| **Ordered shutdown** | Stop pollers → close & drain worker queues → close & drain alarm queue. A SIGINT/SIGTERM never loses an in-flight sample (verified by tests: `polled == processed`). |
| **Hysteresis + soak counter** | Separate raise/clear limits and N consecutive samples (default 3) stop alarms flapping on noisy readings. |
| **LOS masking** | When LOS is active, `RX_POWER_LOW`, `OSNR_LOW` and `PRE_FEC_BER_HIGH` are suppressed — operators see the root cause, not a storm of symptoms. |
| **Per-worker latency histograms** | Lock-free recording on the hot path; merged once after `join()`. |
| **`shared_mutex` active-alarm table** | Status queries (SIGUSR1) read concurrently while workers write. |

### Alarms

| Alarm | Metric | Raise | Clear | Severity |
|---|---|---|---|---|
| `LOS` | Rx power | < −30 dBm | > −28 dBm | CRITICAL |
| `RX_POWER_LOW` | Rx power | < −18 dBm | > −16 dBm | MAJOR |
| `RX_POWER_HIGH` | Rx power | > 3 dBm | < 2 dBm | MINOR |
| `TX_POWER_LOW` | Tx power | < −6 dBm | > −4 dBm | MAJOR |
| `OSNR_LOW` | OSNR | < 15 dB | > 16 dB | MAJOR |
| `PRE_FEC_BER_HIGH` | pre-FEC BER | > 1e-4 | < 5e-5 | MAJOR |
| `TEMP_HIGH` | temperature | > 70 °C | < 65 °C | MINOR |

Injectable faults: `los` (fibre cut), `rx-degrade` (dirty connector / bend),
`ber-degrade` (rising BER, falling OSNR), `over-temp`, `tx-fail` (laser failure).

## Build & test

Requirements: CMake ≥ 3.16, a C++17 compiler (GCC/Clang), Python 3.8+ for the
analyzer. GoogleTest is fetched automatically if it is not installed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure          # 29 GoogleTest unit tests
pip install pytest && pytest -q tests/integration   # 6 end-to-end tests

# Sanitizer builds (used in CI)
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DOTA_SANITIZER=thread
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOTA_SANITIZER=address
```

CI (GitHub Actions) builds with GCC and Clang in Release and Debug, runs unit and
integration tests, and runs the tests plus a fault-injection stress run under
**ThreadSanitizer, AddressSanitizer and UBSan**.

## Run

```bash
# 16 ports at 10 Hz for 30 s, fibre cut on port 3 from sample 50 for 40 samples
./build/ota-agent --ports 16 --rate 10 --duration 30 \
    --inject 3:los:50:40 --inject 7:ber-degrade:20:150 --alarm-log -
```

```
{"t_ms":5203.114,"seq":52,"port":3,"alarm":"LOS","severity":"CRITICAL","state":"RAISED","masked":false,"value":-39.9}
...
```

Send `SIGUSR1` to print the active-alarm table, `SIGINT`/`SIGTERM` for a graceful
drain. `--help` lists every option (`--workers`, `--pollers`, `--queue`, `--soak`,
`--seed`, `--samples`, `--telemetry-log`, `--summary-json`).

## Offline analysis and early warning

`tools/analyze.py` (standard library only) summarises the alarm log and runs a
**rolling z-score drift detector** on the telemetry, flagging a port that is
drifting away from its own recent baseline *before* the fixed threshold fires:

```bash
./build/ota-agent --ports 16 --rate 0 --samples 400 \
    --alarm-log alarms.jsonl --telemetry-log telemetry.jsonl \
    --inject 1:los:50:40 --inject 4:ber-degrade:100:150 \
    --inject 8:over-temp:60:100 --inject 10:rx-degrade:150:80
python3 tools/analyze.py --alarms alarms.jsonl --telemetry telemetry.jsonl
```

```
Early warnings (rolling z-score drift detector):
  port   4  pre_fec_ber  at seq 108    -> 14 samples before PRE_FEC_BER_HIGH
  port   8  temp_c       at seq 62     -> 26 samples before TEMP_HIGH
  port  10  rx_dbm       at seq 151    -> 21 samples before RX_POWER_LOW
```

On gradual faults the detector warned **12–26 samples ahead** of the threshold
alarm, with no warnings on healthy ports.

## Performance

Measured on a 2-vCPU Linux VM, Release build:

| Scenario | Result |
|---|---|
| 256 ports, unpaced, 2 workers | **~2.7 M samples/s**, 1.28 M samples, zero loss |
| 256 ports × 100 Hz, 4 workers, 10 s, faults injected | p50 **30 µs**, p99 **289 µs** capture-to-alarm-decision latency |

```bash
./build/ota-agent --ports 256 --pollers 4 --workers 2 --rate 0 --samples 5000
./build/ota-agent --ports 256 --pollers 4 --workers 4 --rate 100 --duration 10
```

Throughput stops scaling past 2 workers on this machine because it has only
2 cores; numbers will differ on other hardware.

## Layout

```
include/ota/   types, bounded_queue, transceiver, alarm_engine, latency_histogram, agent
src/           implementations + main.cpp (CLI, signals)
tests/         GoogleTest unit tests, tests/integration (pytest, end-to-end)
tools/         analyze.py — alarm report + drift detector
```

## License

MIT
