# Review todo

From a full read of the driver, OMS and the roof firmware on 2026-08-29.

Status key: **open** — to do; **done** — fixed; **closed** — answered, no work wanted.

---

## Done

**0. Driver aborts on a switch reported without a `"state"` key.** `indi-driver/src/oms.cpp:997`
A regression from the `/api/v1/status` refactor: the appliers took `const json &`, which
makes `data[sw.id]["state"]` nlohmann's *const* `operator[]` — that does not throw on a
missing key, it `JSON_ASSERT`s, and the driver is not built with `NDEBUG`. Reproduced as
`Assertion 'it != m_value.object->end()' failed`, exit 134 (SIGABRT, core dumped); under
indiserver it reads as `stderr EOF` / `restart #0`, i.e. a driver that randomly
disconnects. Fixed with `.at()`, which is what the rest of the file uses and throws
catchably. Every other const `operator[]` in the driver was audited — all 25 are guarded
by a `contains()` on the same key in the same expression.
**done**

---

## Driver — correctness

**1. Commands block the INDI event loop for up to `TRANSFER_TIMEOUT` (5 s).**
`sendRoofCommand`/`sendSwitchCommand` run on the main thread from `ISNewSwitch`
(`oms.cpp:283`), `Move`, `Park`, `UnPark` and `Abort` — the same loop that has to read an
Abort off the wire from indiserver. The poll thread exists precisely to keep that loop
free, and this path was widened from 2 s to 5 s while fixing the poll timeouts. Give POSTs
their own shorter budget: they hit endpoints that answer 202 as soon as the command is
posted to a mailbox.
**open**

**2. `CURLOPT_NOSIGNAL` is not set** while curl runs on two threads (`oms.cpp:1205` onward).
libcurl has AsynchDNS here, so the SIGALRM-for-timeouts path is gone, but libcurl still
installs a SIGPIPE handler by default and installing one from a non-main thread is
process-global.
**open**

**3. `m_statusPollFailures` and `m_weatherHave` survive a disconnect/reconnect.**
A session that ended with two failures reaches the error threshold after one more. Reset
them in `Connect()`.
**open**

## Driver — noise and hygiene

**4. `Error accessing environment fields` logs every poll.** `oms.cpp:1074`
When the environment sensor has never answered, `getEnvironment()` returns the keys with
`None` values, so the parse fails every two seconds. Measured 11 lines in 20 s — roughly
43,000 a day. Log once, or only on a state change. Pre-existing, not a regression.
**open**

**5. Typo: `"Could query URL"` → `"Could not query URL"`.** `oms.cpp:1269`
**open**

**6. Stale comment.** `oms.cpp:1274`
Still explains that `response` is set before the status check because
`/api/v1/environment` answers a handled 503 with the reading in the body. That endpoint is
no longer fetched — everything comes from `/api/v1/status`, which is always 200.
**open**

**7. No connection reuse.** A fresh easy handle, TCP connection and DNS lookup for
`oms.fritz.box` on every request. Matters less at one request per poll than it did at
four, but a shared handle or `CURLOPT_DNS_CACHE_TIMEOUT` is nearly free.
**open**

## Firmware

The firmware is frozen — it works and is not to be touched. All three items below are
closed on that basis, and 10 and 11 were answered outright.

**8. `runcmd()` is declared `bool` and never returns.** `roof/roof.ino:140`
Falling off a non-void function is undefined behaviour. Harmless in practice: the caller
ignores the value.
**closed — firmware not to be modified**

**9. Command decoding accepts any byte with a known bit set, in priority order, rather
than requiring an exact match.** `roof/roof.ino:140`
A corrupted byte can execute a *different* command and still return a valid-looking
status, so `__rejected()` does not catch it: there is no CRC and the board never echoes
what it received, so the only detection is "no known command bit at all".
**closed on the merits, not merely because the firmware is frozen** — the path exists in
the code but has no plausible physical trigger on this hardware. See below.

**10. Limit switches use plain `INPUT`, not `INPUT_PULLUP`.** `roof/roof.ino:162`
**closed — not an issue.** The switches are debounced in hardware with Schmitt triggers,
capacitors and resistors, so the inputs are not floating and `ABSENCE_STRIKES` is not
covering for a wiring problem.

**11. `Motor::retract()` leaves the retract pin HIGH indefinitely.** `roof/motor.cpp:19`
**closed — intentional.** The pin is held high deliberately, to make sure the rods
actually retract.

### On 9: how far a corrupted command gets

`runcmd()` tests bits in this order — STATUS(0), FANS_ON(1), FANS_OFF(2), EAST_EXTEND(5),
EAST_RETRACT(6), WEST_EXTEND(3), WEST_RETRACT(4) — and runs the first that matches. Only a
byte with *none* of bits 0–6 set reaches `cmdUnkn()`, which is the one case
`Roof.__rejected()` can see. So a corruption that clears every known bit is caught; one
that *adds* a bit is silently executed as whatever comes first in that order.

Worked example: `WEST_RETRACT` is `0x10`. A flip of bit 3 makes it `0x18`, which matches
`WEST_EXTEND` first — the rod goes out when OMS asked for it to come back, and the board
returns an ordinary status word.

What already contains it:

- The extend is verified. `serviceRoofOnce()` does
  `replied = extendRod(half)` then `if rodExtendRequested(half, status=replied)`, and logs
  "the rod did not take its extend command" when the reply does not confirm it
  (`oms/oms:2882`). A corrupted extend is therefore noticed.
- The retract is *not* verified, but it self-corrects: `retractRoofRods()` only sends a
  retract when `rodExtendRequested()` is true (`oms/oms:2557`), and it re-tests that every
  tick — so a retract that was executed as an extend is simply retried on the next pass.
- `CLOSE_RODCLEAR` waits on `rodRetracted()` before closing, so a rod still out delays or
  times out the close rather than closing a half onto it.

Net effect of the worst realistic case: a longer rod stroke and a close that waits, and if
it repeats, a close that times out and faults — not a physical hazard.

Why there is no CRC, and why that is right. The UART runs a few millimetres from the
microcontroller to the USB-serial converter: at 9600 baud over that distance there is no
realistic corruption mechanism to defend against. The rest of the path is the USB cable,
which carries a CRC16 per packet with hardware retry, so the long and exposed half is
already protected. A CRC on a one-byte command would double the payload to defend the one
segment that cannot plausibly fail.

The residual risk on a UART is framing rather than bit corruption — a stale or dropped
byte shifting the stream by one — and a command CRC would not have addressed that either.
That case is already handled: `__transact()` flushes the input buffer before every write
for exactly this reason, `writeCmd()` retries and then reconnects a wedged bridge, and the
DTR reset is blocked in hardware so a reopen cannot restart the board.

Optional, no reflash needed: check the retract's reply the way the extend's is checked,
and treat a reply that still shows REQ set as a failed exchange. Three lines in
`retractRoofRods()`. Worth doing only if a rod is ever seen going out during a close.

## OMS — decisions taken

**12. An unreachable weather station stands the rain interlock down.** `rainReading()`
returns `None`, and `None` is not rain, so the roof stays openable by the UI, the API and
Ekos. The INDI driver reports the observatory unsafe in that case, so Ekos will not ask,
but OMS itself would not refuse an open.
**done** — `rainOpenRefusalReason()` now refuses an *open* when there is no usable rain
reading, while `rainInterlockReason()` keeps its old answer so a missing reading still
never causes a close. The override lifts it, which is the point: it is the operator saying
they have looked outside themselves. Wired into `requestRoofMotion()`, the API's open
command, the Open button and its label, and reported as `rain.openRefused` /
`rain.openRefusedReason` on `/api/v1/roof` and `/api/v1/status`. The driver needs no
change: it already reports the observatory unsafe whenever the weather reading is
unusable, off the same staleness rule, so Ekos reaches the same conclusion independently.

**13. A disengaged roof is never closed on rain.** The right call with somebody at the
hardware; it does mean a roof disengaged while open stays open in the rain.
**done** — behaviour kept, warning added. `warnDisengagedInTheRain()` says so about once a
minute while the roof is disengaged, not closed, and rain is being reported, to both the
log and the page. Judged on `rainReason()` so an override does not silence it, and silent
on a roof that is already shut so a long disengage over a closed roof says nothing.

---

## What held up

`settingBounds` covers every `numericSetting` call site, so there is no `KeyError` path.
The roof pins are range- and duplicate-checked before a `Roof` is built.
`OwnerTrackingLock.release()` clears `_owner` before releasing the lock, so a new owner's
record cannot be clobbered. `drive()` is genuinely break-before-make with a dead time.
`stopHalf()`/`stopMotion()` attempt every relay before reporting failures. The API
validates every path parameter before use. `Motor::extend()`/`retract()` are
break-before-make and the millis arithmetic is rollover-safe.
