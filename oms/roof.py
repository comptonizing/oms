import logging
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
        self.__switchWestOpen = self.__makeMotionRelay(pinWestOpen)
        self.__switchWestClose = self.__makeMotionRelay(pinWestClose)
        self.__switchEastOpen = self.__makeMotionRelay(pinEastOpen)
        self.__switchEastClose = self.__makeMotionRelay(pinEastClose)
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

    def stopMotion(self):
        """De-energizes every motion relay.

        All of them are attempted even if one fails: leaving a contactor energized
        because some other pin write raised is not an acceptable outcome. Failures are
        collected and reported together, once everything has been tried.
        """
        errors = []
        for sw in self.__motionRelays:
            try:
                sw.off()
            except Exception as e:
                errors.append("pin {}: {}".format(sw.pin, e))
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

    def writeCmd(self, cmd, retries=1):
        payload = (1 << cmd.value).to_bytes(1, "big")
        lastError = None
        for attempt in range(1, retries + 2):
            try:
                if self.__serial is None:
                    self.connect()
                data = self.__transact(payload, self.RESPONSE_SIZE)
                if len(data) == self.RESPONSE_SIZE:
                    return data
                # The board only answers when spoken to, so a short read means this
                # exchange is over. Recovering requires re-sending the command;
                # reading again without one can only ever time out.
                lastError = "incomplete response ({} of {} bytes)".format(len(data), self.RESPONSE_SIZE)
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

    def decodeResponse(self, response):
        buff = np.frombuffer(response, dtype=np.uint16)[0]
        n = max(c.value for c in ResponseState) + 1
        ret = [False] * n
        for thing in ResponseState:
            if buff & ( 1 << thing.value ):
                ret[thing.value] = True
        return ret

    def getStatus(self):
        return self.decodeResponse(self.writeCmd(CMD.STATUS))

    def fansOn(self):
        return self.decodeResponse(self.writeCmd(CMD.FANS_ON))

    def fansOff(self):
        return self.decodeResponse(self.writeCmd(CMD.FANS_OFF))

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
