# Modbus Slave Debug Logging & Test Procedure

> Purpose: pin down the cause of the observed malformed response
> `Tx:068-0A 40 00 2A FD` (slave 0x0A, FC 0x40 [not a valid exception FC],
> exception code 0x00 [not a standard code], CRC 0xFD2A [does not match
> CRC over `0A 40 00` = 0x0260]).
>
> The hand-verified CRC mismatch proves the bytes on the wire were either
> truncated mid-frame, had their CRC tail overwritten by another event, or
> were emitted from a UART driver that had already been torn down. The new
> logging is built to discriminate between those cases.

---

## 1. What the new logging tells you

All log lines below are produced by `main/app/modbus_slave_task.c` and the
esp-modbus library (tags `modbus_slave`, `MB_SERIAL`, `MB_CONTROLLER_SLAVE`,
`MBS_TIMER`). All timestamps `[%u]` are milliseconds since boot — same
counter across all tasks, so a `[12345]` from one task aligns with a
`[12345]` from another.

### 1.1 Firmware-level markers (`modbus_slave` tag)

| Log line | Meaning |
|---|---|
| `reconfigure: stack-down requested (rt=… rds=… s_up=…)` | Operator (LCD/console/MQTT/Modbus holding reg) asked the slave to rebuild with new comm params. Stack is going down. |
| `stack_destroy: enter / exit` | The RTU controller and UART driver are being torn down. Any bytes observed on the bus in between are unaccounted-for. |
| `stack_start: ready (addr=… baud_code=… port=…)` | New stack is up at the new comm params. |
| `stack_start: verbose … logs enabled` | Verbose esp-modbus logs (`MB_SERIAL`, `MB_CONTROLLER_SLAVE`, `MBS_TIMER`) were just re-armed at `DEBUG` for this session. |
| `diag[N]: s_up=… coils=… addr=… baud=…` | 2-second heartbeat. `s_up=0` means the stack is currently down (between destroy and start). |
| `master READ: type=0x… mb_offset=… size=…` | The slave served a read request — useful to confirm the slave actually processed a request that produced a response. |

### 1.2 esp-modbus library markers (DEBUG level, auto-armed)

| Tag / line | Meaning |
|---|---|
| `MB_SERIAL: RX: %u bytes` | Slave received N bytes from UART (good signal). |
| `MB_SERIAL: MB_TX_buffer send: (%u) bytes` | Slave emitted N bytes to UART (good signal). |
| `MB_SERIAL: Data event, length: %u` | Raw UART data event from the driver. |
| `MB_SERIAL: Timeout occured, processed: %u bytes` | T3.5 inter-frame gap fired — a full frame has been assembled. |
| `MB_SERIAL: hw fifo overflow` | Hardware FIFO overflowed — high bus traffic or slow task. |
| `MB_SERIAL: ring buffer full` | Driver RX ring buffer was full — packet may have been dropped. |
| `MB_SERIAL: uart rx break` | UART detected a break condition. |
| `MB_SERIAL: uart parity error` | Parity mismatch on a received byte (8N1 ⇒ should not happen). |
| `MB_SERIAL: uart frame error` | Start/stop bit framing error — bus noise or baud mismatch. |
| `MB_SERIAL: uart event type: %u` | Unhandled UART event type. |
| `MB_CONTROLLER_SLAVE: …` | Controller-layer diagnostics — anything here is suspicious. |
| `MBS_TIMER: …` | Frame-spacing timer (T3.5) diagnostics. |

---

## 2. Test procedure

### 2.1 Stable baseline — boot and observe

1. **Erase flash** so the firmware starts from a clean NVS (slave ID 0x0A,
   baud 9600 — Kconfig defaults).
   ```powershell
   idf.py -p COMx erase-flash
   idf.py -p COMx flash
   idf.py -p COMx monitor
   ```
2. Boot log should show, in order:
   - `[N] stack_start: verbose MB_SERIAL/MB_CONTROLLER_SLAVE/MBS_TIMER logs enabled`
   - `MB_CONTROLLER_SLAVE: …` (init line from the library)
   - `slave UART in normal mode (auto-direction RS485, no DE/RE pin)`
   - `[M] stack_start: ready (addr=0x0A baud_code=0 port=1)`
   - `Modbus RTU slave started: addr=0x0A baud_code=0 port=1`
3. From Modbus Poll (or your PC master), open a connection to slave 0x0A at
   9600 8N1. Issue a FC01 read coils request at address 0, count 1.
4. Expect in the log (every poll cycle):
   - `MB_SERIAL: RX: 8 bytes`
   - `MB_SERIAL: Timeout occured, processed: 8 bytes`
   - `master READ: type=0x… mb_offset=…`
   - `MB_SERIAL: MB_TX_buffer send: (6) bytes`
   - `diag[N]: s_up=1 coils=0x00 addr=0x0A baud=0 reads=…`
5. Verify on the master side: response is `0A 01 01 00 51 88` (FC01, 1 byte,
   data 0x00, CRC 0x8851) — note this is the expected response **with both
   relays OFF at boot**. If you see `0A 01 01 01 92 6C` (data 0x01), relay 0
   is already latched on from a previous session.

### 2.2 Reproduce the malformed response — trigger a reconfig under poll

Goal: see whether `0A 40 00 2A FD` correlates with a stack teardown.

1. With Modbus Poll scanning at 200 ms interval (or whatever quick cadence
   you can sustain without dropping frames), force a reconfigure via the
   **LCD**: Settings → RTU Slave → ID (or Baud) → change value → press CENTER
   to save.
2. Watch the log around the reconfig:
   - `[A] reconfigure: stack-down requested (rt=… rds=… s_up=1)`
   - `[A] stack_destroy: enter (s_stack_up=1)`
   - `[A] stack_destroy: exit (was_up=1)`
   - `[B] stack_start: verbose … logs enabled`
   - `[B] stack_start: ready (addr=0x?? baud_code=? port=1)`
3. Look for in-flight `MB_SERIAL: RX:` / `MB_TX_buffer send:` lines that
   overlap with the `[A]` window. **If a `MB_TX_buffer send:` line is missing
   or its byte count is below the expected 6 (FC01) / 7 (FC03 single
   register), the destroy is racing the response — that is the root cause
   of `0A 40 00 2A FD`.**

### 2.3 Reproduce via Modbus HR_APPLY_CONFIG

1. Connect a PC master that can write multiple holding registers in one
   transaction. In Modbus Poll: Setup → Write/Read or use Function 16.
2. Write holding registers 0..10 as a single write so the order is atomic:
   - HR_SLAVE_ADDRESS = 11 (0x0B)
   - HR_BAUD_CODE     = 1 (19200)
   - HR_PARITY_CODE   = 0
   - HR_APPLY_CONFIG  = 1
3. Expected log:
   - `master READ: type=0x…` (or `MB_EVENT_HOLDING_REG_WR` event)
   - `slave comm reconfig via Modbus: addr=0x0B baud_code=1`
   - `[N] reconfigure: stack-down requested …`
   - `[N] stack_destroy: enter …` / `[N] stack_destroy: exit …`
   - `[M] stack_start: ready (addr=0x0B baud_code=1 port=1)`
4. Verify the master connection now needs **slave 0x0B at 19200 8N1** to
   poll successfully. If you reconfigure back via the LCD the cycle repeats.

### 2.4 Hardware-side checks (run these regardless of log outcome)

- **Bus termination**: 120 Ω at each end of the RS485 bus, no terminators
  in the middle. The MAX3485-style transceivers used on most of these
  boards need termination to avoid ringing that produces frame errors.
- **Wiring**: A↔A, B↔B; check that A and B are **not** swapped at any
  node. Auto-direction transceivers are robust to swap, but they only
  detect direction — they can't fix a polarity-inverted link.
- **GND reference**: every node on the bus must share the same ground
  reference, otherwise the receivers can sit in the indeterminate band and
  garble every byte.
- **Baud match**: confirm PC and ESP32 agree on 9600 vs 19200 vs …
- **Half-duplex turnaround**: some MAX3485 boards have a slow direction
  loop; if you see intermittent frame errors at high baud, add 1 ms guard
  between TX and RX (handled by esp-modbus by default at standard baud).

---

## 3. Interpreting the log around the malformed response

The malformed `0A 40 00 2A FD` was captured at the PC master. Here is the
matrix of what the new logs will tell you, per root cause.

### 3.1 Stack teardown during response (most likely)

Log pattern around the bad response:
```
[…A] MB_SERIAL: RX: 8 bytes
[…A] MB_SERIAL: Timeout occured, processed: 8 bytes
[A]   reconfigure: stack-down requested (rt=… s_up=1)
[A]   stack_destroy: enter (s_stack_up=1)
                ← MISSING: MB_TX_buffer send log
[A]   stack_destroy: exit (was_up=1)
[B]   stack_start: verbose … logs enabled
[B]   stack_start: ready (…)
```

**Interpretation:** the slave accepted the request and assembled the
response, but `mbc_slave_destroy()` ran before `xMBPortSerialTxPoll()`
could drain the TX FIFO. The UART driver's transmit completion never
fires, so the bytes the PC sees are a partial frame followed by whatever
the re-init wrote next. The truncated 3 bytes + 2 stray CRC bytes is
exactly what `0A 40 00 2A FD` would look like if the UART were reset
mid-byte.

**Fix:** the reconfigure path must wait for any in-flight response to
flush before calling `mbc_slave_destroy()`. Plumb a "no in-flight
transaction" guarantee into `slave_stack_destroy()` so the LCD/console
"save" path either (a) blocks until the next T3.5 silence window before
triggering destroy, or (b) queues the reconfigure to fire at the next
idle window. Tracked separately.

### 3.2 UART noise

Log pattern:
```
[…A] MB_SERIAL: uart frame error
[…A] MB_SERIAL: ring buffer full      ← noise bursts can fill the ring buffer
[…A] MB_SERIAL: RX: 8 bytes
```

**Interpretation:** bus noise produced a frame error or parity error
that the driver handled by `uart_flush_input()` and `xQueueReset()`. If
the reset happened between the slave starting to TX and finishing, you
get exactly the truncated-frame signature.

**Fix:** hardware (Section 2.4). The logging won't fix this but will
let you identify it as the cause rather than a firmware race.

### 3.3 Driver-level error during init/destroy

Log pattern:
```
MB_CONTROLLER_SLAVE: mb stack initialization failure, eMBInit() returns (0x…).
```
or
```
MB_CONTROLLER_SLAVE: mb stack disable failure.
MB_CONTROLLER_SLAVE: mb stack close failure returned (0x…).
```

**Interpretation:** the controller errored during init or teardown.
Anything in this tag during normal polling is a defect worth reporting.

### 3.4 Library bug

If Sections 3.1–3.3 are ruled out (no `stack_destroy` around the bad
frame, no UART error events, no controller errors), file an issue
against `espressif__esp-modbus@1.0.18` with the captured `MB_SERIAL`
log and the timing of the bad response.

---

## 4. What to send back for diagnosis

When you can reproduce the malformed response, please capture:

1. **10 seconds of log around the bad frame** — copy the chunk that
   contains the `MB_SERIAL: RX:` for the request, the missing or
   incomplete `MB_TX_buffer send:` if any, and the surrounding
   `stack_destroy` / `stack_start` markers.
2. **The PC-side capture** of the same window — Modbus Poll's traffic
   log, with timestamps.
3. **What you were doing when it happened** — idle polling, or one of the
   reconfig triggers above.

That gives us three time-aligned views to localize whether the bug is
on the wire, in the esp-modbus driver, or in our reconfigure path.
