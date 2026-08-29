"""The OMS HTTP API, version 1.

No authentication, by the operator's decision: anything that can reach the port can drive
the roof. That is a statement about where the port is, not about what these endpoints do,
and it is the reason none of them is more permissive than the UI -- every command below
goes through the same function the buttons call, so the interlocks, the refusals and the
one blocking rule are shared rather than reimplemented here.

This module cannot import oms/oms: that file is a script run as __main__ and it imports
this one. bind() is the way round it -- oms/oms hands over an explicit list of the
functions the API may use, once, at startup. Until then every endpoint answers 503, which
is the honest answer for a process that has not finished coming up.

Two rules hold throughout:

  * Commands are fire and forget, because that is what they are underneath: the roof worker
    owns the serial port and judges each command against a status it reads itself. So a
    command that is accepted returns 202, never 200 -- it has been posted, not carried out,
    and a caller that needs to know the outcome has to read the state back. The exceptions
    are the three that finish on the calling thread, and they say 200 for that reason.
  * Everything that is a reading says how old it is. A roof state or a weather payload with
    no age on it is indistinguishable from one taken an hour ago, and both of those are
    answers somebody might act on.

Every endpoint is async on purpose. FastAPI runs a plain `def` endpoint in a threadpool,
and two of the things reachable here -- setSwitchState() and the fan mode -- write
settings, which has to happen on the event loop: NiceGUI schedules its storage backup with
core.loop.create_task(), which off the loop queues the write without waking it. Nothing
here blocks, so there is no cost to staying on the loop.
"""
from nicegui import app
from http import HTTPStatus as status
from fastapi.responses import JSONResponse, Response
import requests
import logging
import math

import settings as s

logger = logging.getLogger(__name__)

__base = "/api/v1/"

# How long a scrape of the weather station may take. Named rather than inline because
# oms/oms derives the weather worker's stop budget from it: the worker sits inside this
# call, so a join that has to outlast a scrape has to be sized against this number, and
# the two must not be able to drift apart. Note requests applies it per phase -- connect
# and read get one each -- so the worst case is twice this, and DNS resolution is not
# covered by it at all.
WEATHER_HTTP_TIMEOUT = 10


def scrapeWeather():
    """Fetches the station's payload over HTTP. Blocking, and not a route.

    This is the body the GET below used to have, and the split is the point of the change.
    The weather worker calls this -- getWeather() in oms/oms is a thin wrapper over it --
    which is why an endpoint that scraped on request was doing two jobs at once: serving a
    caller and being the only scraper OMS has. Now the worker scrapes on its interval and
    the endpoint serves what it found.

    Kept here rather than moved into oms/oms so that it stays beside WEATHER_HTTP_TIMEOUT,
    which weatherStopBudget() derives the worker's join from. The worker sits inside this
    call, and those two numbers must not be able to drift apart.

    Returns a Response, because that is the shape its one caller already unpacks.
    """
    url = s.q("weather_url")
    if url in [None, ""]:
        logger.warning("Weather scraping URL is not configured")
        return JSONResponse(
                content={"Error": "No weather URL is configured"},
                status_code=status.SERVICE_UNAVAILABLE)
    try:
        logger.debug("Getting weather from {}".format(url))
        r = requests.get(url, timeout=WEATHER_HTTP_TIMEOUT)
    except Exception as e:
        logger.error("Error getting weather: {}".format(e))
        return JSONResponse(
                content={"Error": "Could not send request"},
                status_code=status.SERVICE_UNAVAILABLE)

    if r.status_code != status.OK:
        logger.error("Error getting weather: Got status {} from endpoint, message: {}".format(
            r.status_code, r.content))
        return JSONResponse(
                content={
                    "Error": "Got HTTP code {} from endpoint".format(r.status_code),
                    "Status": r.status_code,
                    "Content": r.text
                    },
                status_code=status.SERVICE_UNAVAILABLE)
    logger.debug("Response: {}".format(r.text))
    return Response(content=r.text, media_type='application/json', status_code=status.OK)


class Backend:
    """The slice of oms/oms this module is allowed to touch.

    A plain attribute bag rather than an interface, but a closed one: bind() is called with
    keywords and nothing else is reachable, so the call site in oms/oms is the whole list
    of what the API depends on. Anything missing shows up as an AttributeError at the one
    endpoint that wanted it rather than as a quietly wrong answer everywhere.
    """

    def __init__(self, **parts):
        self.__dict__.update(parts)


oms = None

def bind(**parts):
    """Hands the API the functions it may call. Called once, from oms/oms, at startup."""
    global oms
    oms = Backend(**parts)
    logger.info("API v1 bound to the application")


def unbound():
    """The answer while the process is still coming up, or None once it is not."""
    if oms is None:
        return JSONResponse(
                content={"error": "OMS is still starting up"},
                status_code=status.SERVICE_UNAVAILABLE)
    return None


def jsonSafe(value):
    """`value` with every non-finite float replaced by None, recursively.

    The station sends NaN for a sensor it could not read this cycle, and json.loads()
    parses that happily -- while Starlette renders every JSONResponse with allow_nan=False
    and raises ValueError on it, which reaches the caller as a bare 500 with nothing in it
    to say which field was at fault. One unreadable field is not a server failure, so it
    goes out as null and the rest of the reading goes out with it.

    null rather than the string "NaN", because null is what the rest of OMS already means
    by an absent reading: asNumber() in oms/oms folds NaN into None for exactly this
    reason, and a consumer that tests for one absent value should not have to learn a
    second. Applied at the boundary rather than to the cache, deliberately -- the payload
    is stored as the station sent it, and the rain interlock reads it through asNumber()
    either way, so nothing that decides whether the roof may open changes shape here.
    """
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {key: jsonSafe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonSafe(item) for item in value]
    return value


def problem(message, code=status.SERVICE_UNAVAILABLE):
    return JSONResponse(content={"error": message}, status_code=code)


def accepted(message, **extra):
    """202, not 200. A command here has been posted, not carried out -- see the header."""
    return JSONResponse(content=dict({"accepted": True, "detail": message}, **extra),
                        status_code=status.ACCEPTED)


def done(message, **extra):
    """200, for the commands that have already taken effect by the time this returns."""
    return JSONResponse(content=dict({"accepted": True, "detail": message}, **extra),
                        status_code=status.OK)


@app.get(__base + "id")
async def id():
    logger.debug("id endpoint call")
    return Response(content="OMS API v1", media_type='text/plain')


# --- roof ---------------------------------------------------------------------------

def roofDetail():
    """Everything the board and the relays can say about the roof, from one reading.

    One status word for the whole answer, deliberately. Every accessor on Roof takes a
    status so it can be decoded from a snapshot the caller already holds, and letting each
    of them fetch its own would be a dozen serial exchanges for one request -- on a port
    only the worker is allowed to use. So this reads what the worker last published, and
    says how old it is rather than pretending it is now.
    """
    state, reason = oms.roofDecisiveState()
    detail = {
            "state": state.value,
            "reason": reason,
            "engaged": state is not oms.RoofStatus.DISENGAGED,
            "moving": state in (oms.RoofStatus.OPENING, oms.RoofStatus.CLOSING),
            "commandsBlocked": oms.roofCommandsBlocked(),
            "available": oms.roofUnavailableReason() is None,
            "unavailableReason": oms.roofUnavailableReason(),
            }

    # The rain interlock, reported in full rather than as one boolean. A caller refused an
    # open needs to be able to tell three cases apart that a single flag would collapse: it
    # is raining and the roof is held shut, it is raining but somebody has overridden the
    # interlock, and the station is not reporting a usable rain figure at all -- which is
    # *not* treated as rain, and a client deciding whether to start an observing run has
    # every reason to want to know that rather than to read `false` as "dry". `detected`
    # ignores the override on purpose, for the same reason the UI shows both.
    interlock = oms.rainInterlockReason()
    detected = oms.rainReason()
    detail["rain"] = {
            "reading": oms.rainReading(),
            "threshold": oms.numericSetting("roof_rain_threshold", oms.rainThresholdDefault),
            "detected": detected is not None,
            "detectedReason": detected,
            "override": oms.rainOverrideActive(),
            "holdingClosed": interlock is not None,
            "reason": interlock,
            }
    motion = oms.roofMotion
    detail["motion"] = {
            "running": motion.running,
            "action": motion.action,
            "phase": motion.phase.name if motion.phase is not None else None,
            }

    # The relays are Pi GPIO and are invisible to the board, so they come from what the
    # worker published rather than from the status word. drive() only ever energizes one
    # relay of a pair -- that is the interlock the whole class is built around -- so the
    # direction a half is being driven says exactly which of its two contactors is closed.
    drive = oms.getRoofDrive()
    relays = {}
    for half in oms.roof.RoofHalf:
        direction = drive.get(half)
        name = half.value.lower()
        relays[name + "_open"] = direction is oms.roof.Direction.OPEN
        relays[name + "_close"] = direction is oms.roof.Direction.CLOSE
    detail["relays"] = relays
    detail["driving"] = {half.value.lower(): (drive.get(half).name
                                              if drive.get(half) is not None else None)
                         for half in oms.roof.RoofHalf}

    word, age = oms.getRoofState()
    detail["reading"] = {
            "age": None if word is None else round(age, 2),
            "stale": word is None or age > oms.roofStateMaxAge,
            "maxAge": oms.roofStateMaxAge,
            }
    if word is None or not oms.roof.Roof.isInitialized():
        # No reading to decode. Reported as nulls rather than left out, so a caller can tell
        # "the roof has not answered" from "this version does not report limit switches".
        detail["limitSwitches"] = None
        detail["rods"] = None
        detail["position"] = None
        detail["fans"] = None
        detail["fault"] = {"latched": None, "reason": None}
        return detail

    instance = oms.roof.Roof.get()
    detail["fault"] = {
            "latched": not instance.motionReady,
            "reason": instance.motionFaultReason,
            }
    detail["limitSwitches"] = {sw.name.lower(): bool(value) for sw, value
                               in instance.switchStates(status=word).items()}
    detail["rods"] = {
            half.value.lower(): {
                # What the firmware was last told, and what it says about the stroke. Both,
                # because they answer different questions: REQ is "OMS asked for it out",
                # retracted is "the board considers it back and settled", and there are 20 s
                # after a retract in which neither is true.
                "extendRequested": bool(instance.rodExtendRequested(half, status=word)),
                "retracted": bool(instance.rodRetracted(half, status=word)),
                }
            for half in oms.roof.RoofHalf
            }
    detail["position"] = {
            "roof": instance.position(status=word).name,
            **{half.value.lower(): instance.halfPosition(half, status=word).name
               for half in oms.roof.RoofHalf},
            }
    detail["fans"] = bool(instance.fansAreOn(status=word))
    return detail


@app.get(__base + "roof")
async def roofState():
    logger.debug("roof state endpoint call")
    down = unbound()
    if down is not None:
        return down
    return JSONResponse(content=roofDetail(), status_code=status.OK)


# The commands the API offers. The stop is the one the block does not apply to, for the
# same reason it is the one button never disabled: it is what somebody reaches for when
# things are wrong, and a roof that is moving is the case it exists for rather than a case
# to refuse it in.
ROOF_COMMANDS = ("open", "close", "stop", "reset", "engage", "disengage")


@app.post(__base + "roof/{command}")
async def roofCommand(command: str):
    logger.info("roof command endpoint call: {}".format(command))
    down = unbound()
    if down is not None:
        return down
    if command not in ROOF_COMMANDS:
        return problem("Unknown roof command {!r}, expected one of {}".format(
            command, ", ".join(ROOF_COMMANDS)), status.NOT_FOUND)
    if command != "stop" and oms.roofCommandsBlocked():
        # 409 rather than 403: nothing about the request is wrong, it is the state of the
        # roof that makes it impossible, and it will succeed once the motion ends.
        return problem("The roof is moving; stop it first", status.CONFLICT)
    if command == "open":
        held = oms.rainInterlockReason()
        if held is not None:
            # Answered here as well as refused in requestRoofMotion(), which is the guard.
            # This exists so the caller gets 409 and the reason rather than the flat 503 the
            # generic refusal below would give -- same distinction as the block above: the
            # request is fine, the weather is not, and it will be accepted once that changes
            # or once somebody overrides the interlock.
            return problem("Cannot open the roof: {}".format(held), status.CONFLICT)

    if command in ("open", "close", "stop"):
        if not oms.requestRoofMotion(command):
            _, reason = oms.roofDecisiveState()
            return problem("Roof {} was refused: {}".format(
                command, reason or "see the OMS log"))
        if command == "stop":
            # The one motion command carried out on this thread rather than posted: it is
            # four local GPIO writes, and the relays are open by the time this returns.
            return done("Roof stopped, motion relays de-energized")
        return accepted("Roof {} requested".format(command))

    if command == "reset":
        if not oms.requestClearRoofFault():
            return problem("The fault could not be cleared: {}".format(
                oms.roofUnavailableReason() or "the roof is disengaged"))
        return accepted("Roof fault clear requested")

    engaged = command == "engage"
    if not oms.requestRoofEngaged(engaged):
        return problem("Roof {} failed; see the OMS log".format(command))
    # Not 202. Both of these take effect on this thread -- disengage() de-energizes and
    # latches the refusal before it returns, and engage() is a flag -- so by the time a
    # caller reads this the roof is already in the state they asked for.
    return done("Roof {}".format("engaged" if engaged else "disengaged"))


# A path of its own rather than another word in ROOF_COMMANDS, because it is not a roof
# command: nothing is driven, nothing is posted, and it is deliberately *not* subject to
# the moving-roof block those are all held to -- overriding while a rain close runs stops
# nothing, so there is nothing there to refuse. Two segments, so it cannot collide with the
# single-segment {command} route above whichever order they are registered in.
@app.post(__base + "roof/rain-override/{state}")
async def roofRainOverride(state: str):
    logger.info("roof rain override endpoint call: {}".format(state))
    down = unbound()
    if down is not None:
        return down
    if state not in ("on", "off"):
        return problem("Unknown rain override state {!r}, expected on or off".format(state),
                       status.NOT_FOUND)
    override = state == "on"
    changed = oms.setRainOverride(override)
    # 200, not 202: this is a flag and it is set by the time this returns. `changed` says
    # whether it was already in that state rather than whether anything failed -- there is
    # nothing here that can fail -- so a repeated call is a success, not a conflict.
    return done("Rain interlock {}".format("overridden" if override else "armed"),
                changed=changed,
                holdingClosed=oms.rainInterlockReason() is not None)


# --- switches -----------------------------------------------------------------------

# The fans are a switch to an operator and nothing like one underneath: the other four are
# Pi GPIO pins driven directly, while the fans live on the roof board behind the serial
# port and are commanded through the worker's mailbox. They are offered here under the same
# noun anyway, because that distinction is OMS's problem and not the caller's.
FAN_NAME = "fans"
# "auto" is a fan-only state, and it is not a level: it hands the decision to the fan logic
# rather than setting anything. Refused for the other four, which have no such mode.
FAN_STATES = ("on", "off", "auto")
SWITCH_STATES = ("on", "off")


def switchNames():
    return list(oms.switchNames) + [FAN_NAME]


def fanDetail():
    """What the fans are doing, and what mode decided it."""
    mode = oms.settings.q("fan_mode", fallback=None)
    detail = {"name": FAN_NAME, "mode": mode, "auto": mode == "auto"}
    word, age = oms.getRoofState()
    if word is None or not oms.roof.Roof.isInitialized():
        # The fans are on the board, so with no reading there is nothing to report. The
        # mode above is still worth having: it says what OMS intends, which survives the
        # board going away.
        detail["state"] = "unkn"
        detail["age"] = None
        return detail
    detail["state"] = "on" if oms.roof.Roof.get().fansAreOn(status=word) else "off"
    detail["age"] = round(age, 2)
    return detail


def switchDetail(name):
    if name == FAN_NAME:
        return fanDetail()
    return {
            "name": name,
            "state": oms.switch.State.toString(oms.getSwitchState(name)),
            # Whether the switch can be commanded, which is not the same as whether the
            # device on the other end can be used -- a switch that is simply off is
            # commandable and its device is not. This is the former, because it is the one
            # that says whether a POST to this name would do anything. A switch with no pin
            # reads "unkn", and a caller looking only at `state` would take that for a
            # device it could switch on.
            "configured": oms.switchConfigured(name),
            }


@app.get(__base + "switches")
async def switches():
    logger.debug("switches endpoint call")
    down = unbound()
    if down is not None:
        return down
    return JSONResponse(
            content={name: switchDetail(name) for name in switchNames()},
            status_code=status.OK)


@app.get(__base + "switches/{name}")
async def switchState(name: str):
    logger.debug("switch endpoint call: {}".format(name))
    down = unbound()
    if down is not None:
        return down
    if name not in switchNames():
        return problem("Unknown switch {!r}, expected one of {}".format(
            name, ", ".join(switchNames())), status.NOT_FOUND)
    return JSONResponse(content=switchDetail(name), status_code=status.OK)


@app.post(__base + "switches/{name}/{state}")
async def setSwitch(name: str, state: str):
    logger.info("switch command endpoint call: {} -> {}".format(name, state))
    down = unbound()
    if down is not None:
        return down
    if name not in switchNames():
        return problem("Unknown switch {!r}, expected one of {}".format(
            name, ", ".join(switchNames())), status.NOT_FOUND)

    if name == FAN_NAME:
        if state not in FAN_STATES:
            return problem("Unknown fan state {!r}, expected one of {}".format(
                state, ", ".join(FAN_STATES)), status.BAD_REQUEST)
        # Persisted before it is applied, and on this thread, which is the event loop --
        # applyFanMode() deliberately does not write the setting itself, because it is also
        # called from a worker thread where a NiceGUI storage write does not land.
        oms.settings.p("fan_mode", state)
        oms.applyFanMode(state)
        return accepted("Fan mode set to {}".format(state), mode=state)

    if state not in SWITCH_STATES:
        return problem("Unknown switch state {!r}, expected one of {}".format(
            state, ", ".join(SWITCH_STATES)), status.BAD_REQUEST)
    if not oms.switchConfigured(name):
        # Refused rather than attempted. setSwitchState() would raise KeyError for a switch
        # with no pin configured, and the caller would get a 500 for what is a configuration
        # answer rather than a failure.
        return problem("Cannot switch {}: it has no pin configured".format(name))
    oms.setSwitchState(name, oms.switch.State.ON if state == "on" else oms.switch.State.OFF)
    return done("{} switched {}".format(name, state), name=name, state=state)


# --- weather and environment --------------------------------------------------------

def weatherMaxAge():
    """How old a weather reading may be before it counts as stale.

    Three refresh periods, so a station that misses a scrape or two is not called stale for
    it. Written once and read by both the endpoints that judge the age -- /weather, which
    answers 503 past it, and /status, which reports it -- because two copies of a rule this
    small is exactly how they come to disagree, and a client that polls one and trusts the
    other would then be told the reading is current and stale at the same time.
    """
    return oms.numericSetting("weather_refresh", 5) * 3


@app.get(__base + "weather")
async def weather():
    """The weather station's own last reading, served from OMS's cache.

    Changed from proxying the scrape on every call, which had two problems worth naming.
    It scraped the station again per request, going round the worker whose entire job is to
    do that on an interval -- so a caller polling this hit the station harder than OMS does.
    And it answered 200 for data of any age, including data fetched while the allsky camera
    the station hangs off was still powered down.

    The body is the station's payload with its own field names, exactly as before, so a
    consumer reading fields off this does not have to change. What is new is that a 200 now
    means the reading is current: anything older than the same limit the UI uses is a 503
    with the age in it, which is what the status code is for.
    """
    logger.debug("weather endpoint call")
    down = unbound()
    if down is not None:
        return down
    payload, age = oms.getWeatherCache()
    if payload is None or age is None:
        return problem("No weather data has been fetched yet")
    maxAge = weatherMaxAge()
    if age > maxAge:
        return problem("The weather data is {:.0f}s old (limit {:.0f}s)".format(age, maxAge))
    return JSONResponse(content=jsonSafe(payload), status_code=status.OK)


@app.get(__base + "environment")
async def environment():
    """The inside temperature and humidity, from the board rather than the station.

    Its own endpoint rather than folded into the weather above, for the same reason it has
    its own table in the UI: different sensor, different port, different staleness rule, and
    one of them being unavailable says nothing about the other.
    """
    logger.debug("environment endpoint call")
    down = unbound()
    if down is not None:
        return down
    data, age = oms.getEnvironment()
    if age is None:
        return problem("No environment data has been read yet")
    body = dict(data)
    body["age"] = round(age, 2)
    body["stale"] = age > oms.environmentStateMaxAge
    body["maxAge"] = oms.environmentStateMaxAge
    if body["stale"]:
        # 503 with the reading still in the body: the values are real, they are simply too
        # old to act on, and somebody deciding whether to open a roof should have to reach
        # past a 503 for them rather than be handed them as current.
        return JSONResponse(content=jsonSafe(body), status_code=status.SERVICE_UNAVAILABLE)
    return JSONResponse(content=jsonSafe(body), status_code=status.OK)


@app.get(__base + "status")
async def overallStatus():
    """Everything in one request, for a dashboard that would otherwise poll four.

    Always 200 if OMS is up, unlike the endpoints it aggregates: a caller asking for
    everything is not asking about any one thing, so an absent weather reading belongs in
    the body as a null rather than as the status of the whole response.
    """
    logger.debug("status endpoint call")
    down = unbound()
    if down is not None:
        return down
    payload, weatherAge = oms.getWeatherCache()
    data, environmentAge = oms.getEnvironment()
    # stale and maxAge alongside the age, for both readings and on the same terms roof
    # already reports them. Without them this endpoint is not a substitute for the ones it
    # aggregates: /weather and /environment answer 503 once a reading is too old to act on,
    # and that verdict is the server's to make -- it is the one that knows weather_refresh
    # and environmentStateMaxAge. A client given only the age would have to re-derive both,
    # which is a second copy of a rule that lives here, in a process that cannot see the
    # settings it is derived from.
    weatherLimit = weatherMaxAge()
    return JSONResponse(content=jsonSafe({
            "roof": roofDetail(),
            "switches": {name: switchDetail(name) for name in switchNames()},
            "weather": None if payload is None else {
                "age": None if weatherAge is None else round(weatherAge, 2),
                "stale": weatherAge is None or weatherAge > weatherLimit,
                "maxAge": weatherLimit,
                "data": payload,
                },
            "environment": {
                "age": None if environmentAge is None else round(environmentAge, 2),
                "stale": environmentAge is None or environmentAge > oms.environmentStateMaxAge,
                "maxAge": oms.environmentStateMaxAge,
                "data": data,
                },
            }), status_code=status.OK)
