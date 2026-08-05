import logging
import time
from enum import Enum
import numpy as np
from serial import Serial, SerialException

import switch

logger = logging.getLogger(__name__)

class ResponseState(Enum):
    SW1 = 0
    SW2 = 1
    SW3 = 2
    SW4 = 3
    SW5 = 4
    SW6 = 5
    SW7 = 6
    SW8 = 7
    FANS = 8
    WEST_REQ = 9
    WEST_STATE = 10
    EAST_REQ = 11
    EAST_STATE = 12
    CMDUNKN = 13

class RoofSwitch(Enum):
    """The eight limit switches, and which status bit each one reports in.

    Listed in hardware order (SW1..SW8), which is the wiring this has to match. This
    is the only place that mapping is written down.
    """
    WEST_CLOSED_NORTH = ResponseState.SW1
    WEST_CLOSED_SOUTH = ResponseState.SW2
    WEST_OPEN_NORTH = ResponseState.SW3
    WEST_OPEN_SOUTH = ResponseState.SW4
    EAST_CLOSED_NORTH = ResponseState.SW5
    EAST_CLOSED_SOUTH = ResponseState.SW6
    EAST_OPEN_NORTH = ResponseState.SW7
    EAST_OPEN_SOUTH = ResponseState.SW8

    @property
    def bit(self):
        """Index of this switch in a decoded status."""
        return self.value.value

class RoofHalf(Enum):
    """One movable half of the roof, and the switches that report where it is.

    The switch pairs are derived from RoofSwitch by name rather than restated, so the
    wiring stays written down in exactly one place -- the same rule RoofSwitch.bit keeps
    for the status bits.
    """
    WEST = "WEST"
    EAST = "EAST"

    @property
    def openSwitches(self):
        return tuple(sw for sw in RoofSwitch if sw.name.startswith(self.value + "_OPEN"))

    @property
    def closedSwitches(self):
        return tuple(sw for sw in RoofSwitch if sw.name.startswith(self.value + "_CLOSED"))

class Direction(Enum):
    OPEN = 0
    CLOSE = 1

class CMD(Enum):
    STATUS = 0
    FANS_ON = 1
    FANS_OFF = 2
    WEST_EXTEND = 3
    WEST_RETRACT = 4
    EAST_EXTEND = 5
    EAST_RETRACT = 6

class RoofState(Enum):
    CLOSED = 0
    OPEN = 1
    CLOSING = 2
    OPENING = 3
    UNKN = 4

class Roof():
    _instance = None
    # writeCmd()'s default retry count, named so oms/oms's roofStopBudget() can derive the
    # worst-case exchange time from the same number rather than a second, unrelated guess.
    WRITE_RETRIES = 1
    # How long a half must sit with both its relays de-energized before the opposite
    # direction may be energized. Break-before-make with a real gap, not just an ordering:
    # a belt drive reversed while the motor is still turning fights its own inertia, and
    # both relays are switching mains-side contactors.
    REVERSE_DEAD_TIME = 2.0
    # Consecutive polls that must agree before "both halves adrift" is believed. Checks 1
    # and 2 below need a switch to spuriously *engage*, which does not happen; this one
    # fires on a switch spuriously *disengaging*, which a dirty contact or vibration on the
    # parked half can do for a single sample. Counted in polls rather than seconds because
    # it is tied to oms/oms's roofPollPeriod, not to the site.
    ABSENCE_STRIKES = 3

    def __new__(cls, *args, **kwargs):
        if cls._instance is not None:
            raise RuntimeError("Roof is already initialized")
        cls._instance = super().__new__(cls)
        return cls._instance

    @classmethod
    def isInitialized(cls):
        return cls._instance != None

    @classmethod
    def reset(cls):
        """Drops the singleton, de-energizing the motion relays and closing the port.

        Neither may be left to garbage collection. The port is opened exclusive=True,
        so it has to be closed before another Roof can open it, and makeRoof() calls
        this from an except block where the traceback of the failing exchange still
        references the old instance and keeps it alive -- exactly when the port is most
        needed back. The relays are worse: RPi.GPIO leaves pin state untouched at
        process exit, so a relay nobody switched off stays energized with no software
        left to switch it.

        stopMotion() de-energizes the relays and releaseMotionRelays() then cancels the
        write they would otherwise make when they are collected. Both are needed: the
        first is the one that reports failures, the second is the one that makes the
        timing deterministic, and this instance can outlive the call by minutes.

        _instance is cleared before anything is torn down, because writeCmd()
        reconnects by itself when __serial is None: a caller still holding the instance
        would otherwise reopen the port being closed here. Afterwards it fails fast
        with "No roof instance initialized", which every call site already handles.
        """
        instance = cls._instance
        cls._instance = None
        if instance is None:
            return
        # Teardown must not raise. makeRoof() calls this while handling another
        # failure, and __init__ can leave a half-built instance behind whose attributes
        # are missing; an exception here would replace the error being handled.
        try:
            instance.stopMotion()
        except Exception as e:
            logger.error("Error stopping roof motion while resetting: {}".format(e))
        try:
            instance.releaseMotionRelays()
        except Exception as e:
            logger.error("Error releasing roof motion relays while resetting: {}".format(e))
        try:
            instance.disconnect()
        except Exception as e:
            logger.warning("Error closing roof serial port while resetting: {}".format(e))

    @classmethod
    def get(cls):
        if cls._instance is None:
            raise ValueError("No roof instance initialized")
        return cls._instance

    def __init__(self, pinWestOpen, pinWestClose, pinEastOpen, pinEastClose, port, baud=9600, timeout=1):
        self.__port = port
        self.__baud = baud
        self.__timeout = timeout
        self.__serial = None
        # Registered as they are created, so that a construction that fails partway
        # through still leaves stopMotion() able to de-energize the relays that exist.
        self.__motionRelays = []
        # Motion state, set up before the relays exist: __makeMotionRelay() drives a pin,
        # and anything that touches a pin has to be able to reach these.
        self.__motionFaultReason = None
        self.__driving = {half: None for half in RoofHalf}
        # Monotonic stamp of the last de-energize per half, which is what the dead time is
        # measured from. Starts at construction, so the first drive() on a freshly built
        # Roof also waits it out rather than energizing a relay the instant OMS starts.
        now = time.monotonic()
        self.__lastStopped = {half: now for half in RoofHalf}
        self.__absenceStrikes = 0
        self.__switchWestOpen = self.__makeMotionRelay(pinWestOpen)
        self.__switchWestClose = self.__makeMotionRelay(pinWestClose)
        self.__switchEastOpen = self.__makeMotionRelay(pinEastOpen)
        self.__switchEastClose = self.__makeMotionRelay(pinEastClose)
        # The only mapping from (half, direction) to a relay. drive() and stopHalf() go
        # through this; nothing else is allowed to touch these four objects.
        self.__relays = {
                (RoofHalf.WEST, Direction.OPEN): self.__switchWestOpen,
                (RoofHalf.WEST, Direction.CLOSE): self.__switchWestClose,
                (RoofHalf.EAST, Direction.OPEN): self.__switchEastOpen,
                (RoofHalf.EAST, Direction.CLOSE): self.__switchEastClose,
                }
        # Stated outright at startup rather than left to be inferred. Every one of these
        # pins was driven to its off level by Switch's constructor before this line, so
        # this reports what has already happened rather than an intention.
        logger.info(
                "Roof motion relays created de-energized: west open pin {}, west close pin "
                "{}, east open pin {}, east close pin {}".format(
                    pinWestOpen, pinWestClose, pinEastOpen, pinEastClose))
        self.connect()
        # Superseded by RoofSwitch and the per-switch accessors below; derived from the
        # enum rather than restating the mapping. Nothing outside this class reads them
        # any more, so they can go once you are sure of that.
        self.switch_west_open_north = RoofSwitch.WEST_OPEN_NORTH.bit
        self.switch_west_open_south = RoofSwitch.WEST_OPEN_SOUTH.bit
        self.switch_west_closed_north = RoofSwitch.WEST_CLOSED_NORTH.bit
        self.switch_west_closed_south = RoofSwitch.WEST_CLOSED_SOUTH.bit
        self.switch_east_open_north = RoofSwitch.EAST_OPEN_NORTH.bit
        self.switch_east_open_south = RoofSwitch.EAST_OPEN_SOUTH.bit
        self.switch_east_closed_north = RoofSwitch.EAST_CLOSED_NORTH.bit
        self.switch_east_closed_south = RoofSwitch.EAST_CLOSED_SOUTH.bit

    def __makeMotionRelay(self, pin):
        sw = switch.Switch(pin, init=switch.State.OFF, on_destroy=switch.State.OFF)
        self.__motionRelays.append(sw)
        return sw

    @property
    def motionReady(self):
        """Whether motion is permitted. False from the moment anything faults."""
        return self.__motionFaultReason is None

    @property
    def motionFaultReason(self):
        return self.__motionFaultReason

    def faultMotion(self, reason):
        """Latches a motion fault and de-energizes everything. Never raises.

        Idempotent, and deliberately unable to fail: it is called from getStatus(), which
        the fan poll and the UI both sit on, and a fault that raised would take out paths
        that have nothing to do with motion. The de-energize failing is itself logged, but
        the latch is set either way -- refusing further motion is the one thing that must
        happen no matter what else went wrong.
        """
        first = self.__motionFaultReason is None
        if first:
            self.__motionFaultReason = reason
        try:
            self.stopMotion()
        except Exception as e:
            logger.error("Could not de-energize the roof while faulting motion: {}".format(e))
        if first:
            logger.error("Roof motion faulted, no further motion until cleared: {}".format(reason))

    def clearMotionFault(self):
        """Clears the latch unconditionally, for an operator who has looked at the roof.

        It does not refuse while the offending condition is still live, on purpose. The
        next getStatus() re-latches it, and serviceRoofOnce() polls *before* it takes
        commands from the mailbox, so a clear followed straight away by a motion command
        cannot get a motion past a live fault. Refusing instead would leave an operator
        whose roof is genuinely mispositioned -- both halves adrift, check 3 below --
        unable to move it by software at all, with no way out but the hardware. Letting
        them try and watch it come straight back is a far better answer than a flat no.

        The strike counter goes back to zero with it, so a condition that is still there
        takes the full ABSENCE_STRIKES polls to re-latch rather than snapping back on the
        first one.
        """
        if self.__motionFaultReason is None:
            return
        logger.warning("Clearing roof motion fault: {}".format(self.__motionFaultReason))
        self.__motionFaultReason = None
        self.__absenceStrikes = 0

    def driving(self, half):
        """The direction `half` is currently being driven, or None."""
        return self.__driving[half]

    def drive(self, half, direction):
        """Energizes one relay of one half, if that is safe to do this instant.

        Returns True when the relay is energized, False when it is not yet -- never an
        error, never a block. False means "ask again"; this class does not sleep and does
        not retry, because it runs under roofLock on the roof worker thread and a hold
        that outlasts roofStopBudget() is what makes a settings save abandon a worker
        mid-travel.

        The two relays of a half are a direction pair driving one motor, and energizing
        both is the failure this method exists to make impossible. Every path below is
        off-before-on, and after a reversal the half sits with both relays de-energized
        for REVERSE_DEAD_TIME before the other direction is allowed.
        """
        if not self.motionReady:
            raise RuntimeError("Roof motion is faulted: {}".format(self.__motionFaultReason))
        current = self.__driving[half]
        if current is direction:
            # Already going the right way. No write at all -- re-driving an energized pin
            # every poll would be harmless but it would also bury the real transitions in
            # the GPIO log, which is where a reversal has to be readable.
            return True
        if current is not None:
            # Reversal. De-energize now and start the clock; nothing is energized on this
            # call, whatever the answer would have been.
            self.stopHalf(half)
            return False
        waited = time.monotonic() - self.__lastStopped[half]
        if waited < self.REVERSE_DEAD_TIME:
            return False
        self.__relays[(half, direction)].on()
        self.__driving[half] = direction
        return True

    def stopHalf(self, half):
        """De-energizes both relays of one half and starts its dead time.

        Both are attempted even if one raises, for the same reason stopMotion() does it:
        leaving a contactor energized because the other pin write failed is not an
        acceptable outcome.
        """
        errors = []
        for direction in Direction:
            try:
                self.__relays[(half, direction)].off()
            except Exception as e:
                errors.append("pin {}: {}".format(self.__relays[(half, direction)].pin, e))
        # Recorded as stopped regardless of whether the writes landed. If one failed the
        # relay may well still be energized, but the intent is stopped and the dead time
        # has to be observed before anything reverses -- and the caller faults on the
        # exception below, which is what actually stops further motion.
        self.__driving[half] = None
        self.__lastStopped[half] = time.monotonic()
        if errors:
            raise RuntimeError("Could not stop roof {} half: {}".format(
                half.value.lower(), "; ".join(errors)))

    def stopMotion(self):
        """De-energizes every motion relay.

        All of them are attempted even if one fails: leaving a contactor energized
        because some other pin write raised is not an acceptable outcome. Failures are
        collected and reported together, once everything has been tried.

        Iterates __motionRelays rather than calling stopHalf() twice, so that a
        half-constructed instance -- __relays is built after the four relays exist, and
        __init__ can fail in between -- still de-energizes whatever was made. The
        per-half bookkeeping is updated separately below for the same reason.
        """
        errors = []
        for sw in self.__motionRelays:
            try:
                sw.off()
            except Exception as e:
                errors.append("pin {}: {}".format(sw.pin, e))
        now = time.monotonic()
        for half in RoofHalf:
            self.__driving[half] = None
            self.__lastStopped[half] = now
        if errors:
            raise RuntimeError("Could not stop roof motion: " + "; ".join(errors))

    def releaseMotionRelays(self):
        """Settles the relays' pins now, so their collection can never move them later.

        Switch.__del__ drives its pin to the switch's on-destroy state whenever the
        instance is actually collected, and a Roof can outlive being dropped by a long
        way: roofLoop() is started with the instance as an argument, so a worker that
        stopRoof() could not join holds it until its serial read returns. Should the roof
        have been unconfigured in the meantime, pinUsedForRoof() no longer protects these
        four pins and a switch can claim one -- and the collection that follows would then
        de-energize that switch's equipment, unasked, logged only at debug.

        Called from reset() rather than folded into stopMotion(), which stopRoof() also
        calls on a roof that is meant to keep working. Dropping the on-destroy state there
        would take away the backstop for every teardown that is not a reset.

        Every relay is attempted even if one fails, and the failures are reported
        together, exactly as stopMotion() does.
        """
        errors = []
        for sw in self.__motionRelays:
            try:
                sw.release()
            except Exception as e:
                errors.append("pin {}: {}".format(sw.pin, e))
        if errors:
            raise RuntimeError("Could not release roof motion relays: " + "; ".join(errors))

    def disconnect(self):
        if self.__serial is None:
            return
        try:
            self.__serial.close()
        except (SerialException, OSError):
            pass
        self.__serial = None

    def connect(self):
        try:
            self.__serial = Serial(self.__port, self.__baud, timeout=self.__timeout, write_timeout=self.__timeout, exclusive=True)
            self.__serial.reset_input_buffer()
            self.__serial.reset_output_buffer()
            if not self.__serial.is_open:
                raise RuntimeError("Unknown error")
        except SerialException as e:
            raise RuntimeError("Can not open serial port {}: {}".format(self.__port, e))

    def reconnect(self):
        self.disconnect()
        self.connect()

    RESPONSE_SIZE = 2

    def __transact(self, payload, size):
        # Flush before writing rather than after a bad read: a stale byte left over
        # from an earlier timed-out exchange shifts every later response by one, and
        # since any two bytes decode without error that desync would be silent.
        self.__serial.reset_input_buffer()
        self.__serial.write(payload)
        return self.__serial.read(size)

    def writeCmd(self, cmd, retries=WRITE_RETRIES):
        payload = (1 << cmd.value).to_bytes(1, "big")
        lastError = None
        for attempt in range(1, retries + 2):
            try:
                if self.__serial is None:
                    self.connect()
                data = self.__transact(payload, self.RESPONSE_SIZE)
                if len(data) != self.RESPONSE_SIZE:
                    # The board only answers when spoken to, so a short read means this
                    # exchange is over. Recovering requires re-sending the command;
                    # reading again without one can only ever time out.
                    lastError = "incomplete response ({} of {} bytes)".format(len(data), self.RESPONSE_SIZE)
                elif self.__rejected(data):
                    # Retry rather than give up: the command byte is built here from CMD
                    # and is always one the firmware knows, so a rejection means it
                    # arrived corrupted, and re-sending is exactly what fixes that.
                    lastError = "board did not recognise {}".format(cmd.name)
                else:
                    return data
            except (SerialException, OSError, RuntimeError) as e:
                lastError = str(e)
                # Drop the connection so the next attempt reopens it. Never raises.
                self.disconnect()
            # Debug, not warning: this runs at poll rate, and logging is a blocking
            # write. A board that is down would otherwise emit warnings faster than
            # the handler can drain them and stall the caller. The detail survives in
            # the exception below, which the caller logs once.
            logger.debug("Roof board exchange failed (attempt {}/{}): {}".format(
                attempt, retries + 1, lastError))
        # Every attempt failed. A port that times out without ever raising (a wedged
        # USB bridge) would otherwise never recover, since a timeout alone doesn't
        # drop the connection. Reopening is cheap and harmless here -- DTR reset is
        # blocked in hardware, so it does not disturb the board -- so give the next
        # call a fresh port.
        self.disconnect()
        raise SerialException("No valid response from roof board on {} after {} attempts: {}".format(
            self.__port, retries + 1, lastError))

    def __rejected(self, response):
        """Whether this reply is the firmware's "I did not understand that".

        cmdUnkn() answers an unrecognised command with CMDUNKN set and every other bit
        clear, which is why this cannot be left to the decoder: a rejection decodes as a
        perfectly plausible status -- no switch engaged, neither rod out, fans off. Read
        as one it is actively dangerous. OPEN_SETTLE takes a clear REQ bit for a finished
        rod stroke and would run the half open on gravity having never pushed it;
        CLOSE_RODCLEAR takes a clear STATE bit for a stowed rod and would close a half
        onto one still out; and the absence check would blame the roof for what is a
        link fault. So it is treated as a failed exchange, which is what it is.
        """
        return bool(int.from_bytes(response, "little") & (1 << ResponseState.CMDUNKN.value))

    def decodeResponse(self, response):
        buff = np.frombuffer(response, dtype=np.uint16)[0]
        n = max(c.value for c in ResponseState) + 1
        ret = [False] * n
        for thing in ResponseState:
            if buff & ( 1 << thing.value ):
                ret[thing.value] = True
        return ret

    def __decodeAndCheck(self, response):
        """Decodes a board reply and runs the consistency checks over it.

        Every exchange that returns a status word goes through here, not just getStatus():
        it must be impossible to observe an inconsistent roof without acting on it,
        whichever call happened to do the reading.
        """
        status = self.decodeResponse(response)
        self.__checkConsistency(status)
        return status

    def __checkConsistency(self, status):
        """Faults motion if the switches report something that cannot physically be.

        Three checks, and the rules they use differ on purpose. A *contradiction* needs a
        switch to spuriously engage, which a false contact closure would have to cause and
        which essentially does not happen -- so one sample is believed, and one switch is
        enough. An *absence* is a switch spuriously disengaging, which a dirty contact or
        vibration on the parked half does manage from time to time -- so that one wants
        both switches of a pair and several polls in a row before it halts a moving roof.

        Never raises: it is reached from the fan poll and from the UI's status read, and
        those must not start failing because the roof developed a wiring fault.
        """
        for half in RoofHalf:
            openSeen = any(status[sw.bit] for sw in half.openSwitches)
            closedSeen = any(status[sw.bit] for sw in half.closedSwitches)
            if openSeen and closedSeen:
                # 1. The two ends of one half's travel, both reporting engaged. There is no
                # position that does this, so it is broken wiring or a broken switch.
                self.faultMotion(
                        "{} half reports open and closed at the same time".format(
                            half.value.lower()))
                return
        if (any(status[sw.bit] for sw in RoofHalf.WEST.closedSwitches)
                and any(status[sw.bit] for sw in RoofHalf.EAST.openSwitches)):
            # 2. West shut with east open. The sequences never produce it -- opening runs
            # west then east, closing runs east then west -- so whenever east reports open,
            # west is fully open and its closed switches are at the far end of travel.
            self.faultMotion("west half reports closed while the east half reports open")
            return
        # 3. Neither half at an end position. Only one half ever moves at a time and the
        # other is parked, so both being adrift means something moved uncommanded or a
        # switch has failed. Subsumes the all-eight-disengaged case. Debounced, per above.
        if (self.halfPosition(RoofHalf.WEST, status=status) is RoofState.UNKN
                and self.halfPosition(RoofHalf.EAST, status=status) is RoofState.UNKN):
            self.__absenceStrikes += 1
            if self.__absenceStrikes >= self.ABSENCE_STRIKES:
                self.faultMotion(
                        "neither half is fully open or fully closed ({} polls in a row)".format(
                            self.__absenceStrikes))
            return
        self.__absenceStrikes = 0

    def getStatus(self):
        return self.__decodeAndCheck(self.writeCmd(CMD.STATUS))

    def fansOn(self):
        return self.__decodeAndCheck(self.writeCmd(CMD.FANS_ON))

    def fansOff(self):
        return self.__decodeAndCheck(self.writeCmd(CMD.FANS_OFF))

    # The rods are the Arduino's two linear actuators, one per half. Extending one shoves
    # a half that has stuck on its seat far enough for gravity to take over; the firmware
    # retracts it again by itself after Motor::MAX_EXTENDED (20 s).
    __rodCommands = {
            (RoofHalf.WEST, True): CMD.WEST_EXTEND,
            (RoofHalf.WEST, False): CMD.WEST_RETRACT,
            (RoofHalf.EAST, True): CMD.EAST_EXTEND,
            (RoofHalf.EAST, False): CMD.EAST_RETRACT,
            }
    __rodStateBits = {
            RoofHalf.WEST: ResponseState.WEST_STATE,
            RoofHalf.EAST: ResponseState.EAST_STATE,
            }
    __rodRequestBits = {
            RoofHalf.WEST: ResponseState.WEST_REQ,
            RoofHalf.EAST: ResponseState.EAST_REQ,
            }

    def extendRod(self, half):
        return self.__decodeAndCheck(self.writeCmd(self.__rodCommands[(half, True)]))

    def retractRod(self, half):
        return self.__decodeAndCheck(self.writeCmd(self.__rodCommands[(half, False)]))

    def rodRetracted(self, half, status=None):
        """Whether the firmware confirms this half's rod is back and settled.

        Note what the sketch means by it: Motor::isRetracted() only answers true once
        MAX_EXTENDED (20 s) has passed since the *last retract command*, so this stays
        False for 20 s after a retract and for up to 40 s after an extend, which
        auto-retracts at 20 s. Waiting on it is therefore never instant.
        """
        if status is None:
            status = self.getStatus()
        return not status[self.__rodStateBits[half].value]

    def rodExtendRequested(self, half, status=None):
        if status is None:
            status = self.getStatus()
        return status[self.__rodRequestBits[half].value]

    def halfPosition(self, half, status=None):
        """Where one half is: OPEN, CLOSED, or UNKN for anything in between.

        Both switches of a pair are required, unlike the contradiction checks above. A
        half with one open switch engaged is not open, it is somewhere near open, and
        nothing may advance on that.
        """
        if status is None:
            status = self.getStatus()
        if all(status[sw.bit] for sw in half.openSwitches):
            return RoofState.OPEN
        if all(status[sw.bit] for sw in half.closedSwitches):
            return RoofState.CLOSED
        return RoofState.UNKN

    def position(self, status=None):
        """The roof as a whole: OPEN or CLOSED only when both halves agree."""
        if status is None:
            status = self.getStatus()
        west = self.halfPosition(RoofHalf.WEST, status=status)
        if west is not self.halfPosition(RoofHalf.EAST, status=status):
            return RoofState.UNKN
        return west

    def fansAreOn(self, status=None):
        if status is None:
            status = self.getStatus()
        return status[ResponseState.FANS.value]

    def switchState(self, sw, status=None):
        """Whether one limit switch reports engaged.

        As everywhere else here, pass a status to read it out of a snapshot the caller
        already holds instead of asking the board again.
        """
        if status is None:
            status = self.getStatus()
        return status[sw.bit]

    def switchStates(self, status=None):
        """All eight limit switches from a single status, as {RoofSwitch: bool}."""
        if status is None:
            status = self.getStatus()
        return {sw: status[sw.bit] for sw in RoofSwitch}

    def westClosedNorth(self, status=None):
        return self.switchState(RoofSwitch.WEST_CLOSED_NORTH, status=status)

    def westClosedSouth(self, status=None):
        return self.switchState(RoofSwitch.WEST_CLOSED_SOUTH, status=status)

    def westOpenNorth(self, status=None):
        return self.switchState(RoofSwitch.WEST_OPEN_NORTH, status=status)

    def westOpenSouth(self, status=None):
        return self.switchState(RoofSwitch.WEST_OPEN_SOUTH, status=status)

    def eastClosedNorth(self, status=None):
        return self.switchState(RoofSwitch.EAST_CLOSED_NORTH, status=status)

    def eastClosedSouth(self, status=None):
        return self.switchState(RoofSwitch.EAST_CLOSED_SOUTH, status=status)

    def eastOpenNorth(self, status=None):
        return self.switchState(RoofSwitch.EAST_OPEN_NORTH, status=status)

    def eastOpenSouth(self, status=None):
        return self.switchState(RoofSwitch.EAST_OPEN_SOUTH, status=status)

    # The composites below resolve the status once and then hand it down, so that a
    # call without one costs a single exchange rather than one per switch pair.
    def westFullyOpen(self, status=None):
        if status is None:
            status = self.getStatus()
        return self.westOpenNorth(status=status) and self.westOpenSouth(status=status)

    def westFullyClosed(self, status=None):
        if status is None:
            status = self.getStatus()
        return self.westClosedNorth(status=status) and self.westClosedSouth(status=status)

    def eastFullyOpen(self, status=None):
        if status is None:
            status = self.getStatus()
        return self.eastOpenNorth(status=status) and self.eastOpenSouth(status=status)

    def eastFullyClosed(self, status=None):
        if status is None:
            status = self.getStatus()
        return self.eastClosedNorth(status=status) and self.eastClosedSouth(status=status)

    def isFullyOpen(self, status=None):
        if status is None:
            status = self.getStatus()
        return self.westFullyOpen(status=status) and self.eastFullyOpen(status=status)

    def isFullyClosed(self, status=None):
        if status is None:
            status = self.getStatus()
        return self.westFullyClosed(status=status) and self.eastFullyClosed(status=status)
