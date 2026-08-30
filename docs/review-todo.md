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
free, and this path was widened from 2 s to 5 s while fixing the poll timeouts.
**open — but now sized by measurement rather than guesswork.** See the measurements below.
A shorter budget for POSTs is still the change; 2 s is the defensible number now (3× the
worst observed reply, 10× the p99) where before the fixes it would have been wrong,
because the tail then reached 2.9 s and 2 s would have failed commands OMS was carrying
out. Left open deliberately rather than closed: with the tail where it is, nothing is
being harmed by the 5 s ceiling.

**2. `CURLOPT_NOSIGNAL` was not set** while curl runs on two threads — the poll thread's
GET every two seconds and the main thread's commands — and libcurl's signal use is
process-wide.
**done** — set, with the reasoning recorded at the call site. Neither thing it guards was
actually reachable: the `SIGALRM`/`siglongjmp` name-resolution timeout belongs to the
synchronous resolver and this libcurl reports `AsynchDNS`; and the SIGPIPE
save/ignore/restore race cannot kill the driver under indiserver, which calls
`noSIGPIPE()` (`sigaction`, `SIG_IGN`) in `main()` before forking any driver, with POSIX
keeping `SIG_IGN` across `exec` — confirmed on the live process, `SigIgn` mask
`0000000000001006`, bit 13 set. Set anyway so the driver does not depend on another
process for it, and because running the binary standalone does *not* inherit that. Checked
that `CONNECT_TIMEOUT` still works with it set: a connect to a black-holed address returns
in 2.0 s, not at the 5 s transfer budget.

**3. Per-connection state survived a disconnect/reconnect.** `oms.cpp`
`m_statusPollFailures`, `m_weatherHave`, `m_weatherReported` and `m_roofHeldShut` all
carried over, so a session that ended with two failed polls reached the error threshold
after a single miss, and a roof that really was held shut on reconnect logged nothing
because the flag had not changed as far as the driver knew.
**done** — all four are cleared in `Connect()`. This also turned up a false recovery
notice: `m_weatherReported` started `false`, meaning "the last publish said unusable",
when nothing had been published at all — so a first connect to a healthy OMS announced
*"The weather station is reporting again"* for a failure that never happened. It starts
`true` now, which is right both ways: a driver that comes up healthy says nothing, one
whose first reading is already unusable says so. Verified across all three first-connect
cases (healthy, roof held, reading stale).

**3a. A settled roof state could be reported for one tick immediately after a motion was
commanded.** `oms.cpp` `applyRoofState()`
Found on the first live run under Ekos, 2026-08-30: an open was issued and the roof was
reported as at rest a moment later, while it was in fact still travelling.

A command and a reading race, and the reading is older. The poll thread fetches
`/api/v1/status` every `ROOF_POLL_MS` and `TimerHit()` applies whatever it last fetched,
so the first reading applied after a command has left the main thread can be one taken up
to a full poll period *before* it — and OMS needs a moment of its own besides, since
`requestRoofMotion()` posts to the roof worker's mailbox rather than driving anything on
the API's thread. Both windows report the roof exactly as it was: still `closed` when an
open has just been asked for, still `open` when a close has.

Reported straight through, that reading is not merely stale, it is a *settled* state — and
`closed` means `DOME_PARKED` and `open` means `DOME_UNPARKED`, which is `ParkSP` in
`IPS_OK`, which is this driver telling every client the roof is where it was asked to be.
So an unpark correctly reported as `IPS_BUSY` flipped back to parked (and a park to
unparked) for one tick at the worst possible moment: the tick every client is watching to
learn whether the roof moved.

**done** — `Move()` records what it asked for in `m_pendingMotion`; `applyRoofState()`
holds a settled reading that contradicts it, reporting `opening`/`closing` instead, until
OMS's own answer catches up (`opening`/`closing`, or the target position for a motion that
finished inside the window). `fault` and `disengaged` clear the flag at once — neither can
be mistaken for the roof having arrived, and both are things a client must hear
immediately. `Abort()` clears it, and so does `Connect()`, per **3**. A motion OMS never
starts is bounded by `MOTION_START_GRACE` (10 s, five of this driver's polls and twenty of
OMS's roof worker's), after which the reading is believed and the write-off logged.

Two adjacent fixes went in with it. `DomeMotionSP` was left in `IPS_BUSY` after a motion
that ended on the `setDomeState()` branch rather than through `SetParked()` — the latter
clears it on its way through `DOME_IDLE`, the former does not — and `ISD::Dome::isMoving()`
is exactly that property's state, which the scheduler holds a job on after every slew. And
every branch now re-publishes only when what the client has been told does not already
match — `setDomeState()` applies `ParkSP` unconditionally, and a roof takes 65–95 s to
travel, so the unguarded `opening`/`closing` branches were a set on the wire every two
seconds for the whole of a motion saying nothing the first one didn't. The guard tests the
dome state *and* `ParkSP`, because a failed `Abort()` resets `ParkSP` without touching the
dome state, and the wrong one to trust there is the one the client cannot see.

## Driver — noise and hygiene

**4. `Error accessing environment fields` logged every poll.** `oms.cpp`
When the environment sensor has never answered, `getEnvironment()` returns the keys with
`None` values, so the parse failed every two seconds — 11 lines in 20 s, roughly 43,000 a
day. Pre-existing, not a regression.
**done** — rate limited to one line a minute (`ENVIRONMENT_ERROR_INTERVAL`), with the flag
cleared on a readable reading so a sensor that breaks, recovers and breaks again reports
the second failure straight away rather than waiting out an interval that started before
the recovery. Measured after: 2 lines in 70 s of a steadily absent sensor, and an
immediate line after a recovery.

**5. Typo: `"Could query URL"`.** `oms.cpp`
**done** — now `"Could not query URL"`. It is the message an operator sees on every failed
request, and the one quoted in the report that started this review.

**6. Stale comment.** `oms.cpp`
It explained that `response` is set before the status check because `/api/v1/environment`
answers a handled 503 with the reading in the body — an endpoint no longer fetched.
**done** — replaced rather than deleted. The line stays, because handing back what arrived
beats discarding it, but the comment now says that nothing reads an error body today and
that the case which made it load bearing went away with the move to `/api/v1/status`.

**7. No connection reuse.** A fresh easy handle, TCP connection and DNS lookup for
`oms.fritz.box` on every request.
**done** — one curl handle per thread, reused via `curl_easy_reset()`, which clears the
options but keeps the live connections and the DNS cache. Per thread rather than shared
because an easy handle cannot be used by two threads at once and a mutex would serialize
the main thread behind the poll — the opposite of what the poll thread is for. Measured
over 24 s: 12 requests → 12 TCP connections before, 12 requests → 0 after. Recovery from a
dead cached connection checked by killing and restarting OMS: one new connection, no
errors.

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


---

## Measurements against the real observatory, 2026-08-29/30

Taken from konrad — the machine the driver runs on — so the numbers are over the driver's
own network path. 1005 samples across a roof open and close, then three controlled runs
after the fixes. `/api/v1/id` does no real work, so its latency is the queueing delay on
OMS's event loop; `/api/v1/status` is the aggregate the driver actually polls.

**What caused the original timeouts.** Not the roof. The tail was a stall every ~30 s,
matching `weather_plot_refresh`, and it appeared the moment a browser opened the OMS page
— before that, 206 consecutive samples came back under 21 ms. It went on at exactly that
cadence whether the roof was moving or standing still, and the *closing* phase had the
lowest maximum of any phase in the run. The single worst sample, 2932 ms, was the plot
pass that runs when a client connects.

**Roof motion, measured.** Both motions completed cleanly with the driver logging only
`Dome is unparked.` / `Dome is parked.` — no timeouts, the three-poll tolerance never
fired, `WEATHER_STATUS` green throughout. Open took ~95 s, close ~65 s, west-then-east and
east-then-west as documented.

**After the two plotting changes**, same driver, same OMS, only the browser differing:

| | n | p50 | p99 | max | over 100 ms |
|---|---|---|---|---|---|
| browser closed | 235 | 9.3 | 22.1 | 27.1 ms | 0 |
| open, Status tab | 468 | 8.7 | 94.1 | 385.7 ms | 4 |
| open, Weather tab | 464 | 9.1 | 196.6 | 610.3 ms | 8 |

Zero failed requests in every run. Worst case with the plots refreshing went from 2932 ms
to 610 ms, and the clusters of three to five consecutive slow samples became isolated
singles.

**What remains, if it is ever worth chasing.** Having the page open at all costs up to
~386 ms even with no plots drawing, so the residual is no longer figure building. It is
spread across the query decoding (`queryWeather`/`queryState` build their lists on the
loop, two `list()` copies per row, ~11,400 rows a pass), NiceGUI's outbox serialising each
payload onto the websocket, and the non-plot timers. The structural answer — serving the
API from its own event loop on its own port, so the driver cannot be affected by anything
the UI does — was raised and deliberately not taken: at 610 ms worst against a 5 s budget,
nothing is being harmed.
