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
        del cls._instance
        cls._instance = None

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
        self.__switchWestOpen = switch.Switch(pinWestOpen, init=switch.State.OFF, on_destroy=switch.State.OFF)
        self.__switchWestClose = switch.Switch(pinWestClose, init=switch.State.OFF, on_destroy=switch.State.OFF)
        self.__switchEastOpen = switch.Switch(pinEastOpen, init=switch.State.OFF, on_destroy=switch.State.OFF)
        self.__switchEastClose = switch.Switch(pinEastClose, init=switch.State.OFF, on_destroy=switch.State.OFF)
        self.connect()
        self.switch_west_open_north = ResponseState.SW3.value
        self.switch_west_open_south = ResponseState.SW4.value
        self.switch_west_closed_north = ResponseState.SW1.value
        self.switch_west_closed_south = ResponseState.SW2.value
        self.switch_east_open_north = ResponseState.SW7.value
        self.switch_east_open_south = ResponseState.SW8.value
        self.switch_east_closed_north = ResponseState.SW5.value
        self.switch_east_closed_south = ResponseState.SW6.value

    def stopMotion(self):
        self.__switchWestOpen.off()
        self.__switchWestClose.off()
        self.__switchEastOpen.off()
        self.__switchEastClose.off()

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

    def fansAreOn(self):
        status = self.getStatus()
        if status[ResponseState.FANS.value]:
            return True
        else:
            return False

    def westFullyOpen(self, status=None):
        if status is None:
            status = self.getStatus()
        return status[self.switch_west_open_north] and status[self.switch_west_open_south]

    def westFullyClosed(self, status=None):
        if status is None:
            status = self.getStatus()
        return status[self.switch_west_closed_north] and status[self.switch_west_closed_south]

    def eastFullyOpen(self, status=None):
        if status is None:
            status = self.getStatus()
        return status[self.switch_east_open_north] and status[self.switch_east_open_south]

    def eastFullyClosed(self, status=None):
        if status is None:
            status = self.getStatus()
        return status[self.switch_east_closed_north] and status[self.switch_east_closed_south]

    def isFullyOpen(self, status=None):
        return self.westFullyOpen(status=status) and self.eastFullyOpen(status=status)

    def isFullyClosed(self, status=None):
        return self.westFullyClosed(status=status) and self.eastFullyClosed(status=status)
