import logging
import asyncio
from enum import Enum
import numpy as np
from serial import Serial, SerialException
from nicegui import ui
import threading


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
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            return cls._instance

    @classmethod
    def reset(cls):
        #if cls._instance is not None:
        #    cls._instance.stopLoop()
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
        self.__thread: threading.Thread | None = None
        self.__stop_event = threading.Event()

    def __del__(self):
        #self.stopLoop()
        return

    def stopMotion(self):
        self.__switchWestOpen.off()
        self.__switchWestClose.off()
        self.__switchEastOpen.off()
        self.__switchEastClose.off()

    def isRunning(self):
        return self.__thread is not None and self.__thread.is_alive()

    def disconnect(self):
        try:
            self.__serial.close()
        except (serial.SerialException, OSError):
            pass
        self.__serial = None

    def connect(self):
        try:
            self.__serial = Serial(self.__port, self.__baud, timeout=self.__timeout, write_timeout=self.__timeout)
            self.__serial.reset_input_buffer()
            self.__serial.reset_output_buffer()
            if not self.__serial.is_open:
                raise RuntimeError("Unknown error")
        except SerialException as e:
            raise RuntimeError("Can not open serial port {}: {}".format(self.__port, e))

    def reconnect(self):
        self.disconnect()
        self.connect()

    def isConnected(self):
        return self.__serial.is_open

    def read(self, size):
        try:
            return self.__serial.read(size)
        except (serial.SerialException, OSError):
            self.reconnect()
            return self.__serial.read(size)

    def write(self, data):
        try:
            return self.__serial.write(data)
        except (serial.SerialException, OSError):
            self.reconnect()
            return self.__serial.write(data)

    def writeCmd(self, cmd):
        self.write((1 << cmd.value).to_bytes(1, "big"))
        return self.read(2)

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

    def startLoop(self):
        logger.info("Starting roof loop")
        if self.__thread is None or not self.__thread.is_alive():
            self.__stop_event.clear()
            self.__thread = threading.Thread(target=self.loop, daemon=True)
            self.__thread.start()



    def stopLoop(self):
        logger.info("Stopping roof loop")
        self.__stop_event.set()

    def loop(self):
        try:
            while not self.__stop_event.is_set():
                # Do stuff
                self.__stop_event.wait(timeout=0.1)
        finally:
            # cleanup
            pass
