from enum import Enum
import RPi.GPIO as GPIO
import logging

logger = logging.getLogger(__name__)

GPIO.setmode(GPIO.BCM)
# "This channel is already in use" fires for every switch pin on every restart, because a
# pin the previous process left as an energized output is exactly what has to be preserved
# -- nothing calls GPIO.cleanup(), so the equipment stays powered while OMS is not running.
# The warning describes the design rather than a fault, and RPi.GPIO only warns from
# setup(), so this suppresses nothing else.
GPIO.setwarnings(False)

GPIO_ON = GPIO.LOW
GPIO_OFF = GPIO.HIGH

class State(Enum):
    OFF = 0
    ON = 1
    UNKN = 2

    @classmethod
    def toString(cls, state):
        if state is cls.ON:
            return "on"
        if state is cls.OFF:
            return "off"
        if state is cls.UNKN:
            return "unkn"
        raise RuntimeError("{} is not a valid state".format(state))

    @classmethod
    def toLevel(cls, state):
        """The GPIO level that drives `state`, or None if the state has no level.

        UNKN is not something a pin can be driven to, so it maps to None: a caller asking
        for it has to leave the pin exactly as it found it.
        """
        if state is cls.ON:
            return GPIO_ON
        if state is cls.OFF:
            return GPIO_OFF
        if state is cls.UNKN:
            return None
        raise RuntimeError("{} is not a valid state".format(state))

    @classmethod
    def fromString(cls, name):
        if name == "on":
            return cls.ON
        if name == "off":
            return cls.OFF
        if name == "unkn":
            return cls.UNKN
        raise RuntimeError("{} is not a valid state name".format(name))

class Switch():
    def __init__(self, pin, init=State.OFF, on_destroy=None):
        logger.debug("Making a switch, pin {}, initial state {}".format(pin, init))
        self.__pin = pin
        self.__state = State.UNKN
        self.__onDestroy = on_destroy
        level = State.toLevel(init)
        if level is None:
            # No intended level. Leaving the pin at whatever it is already driving is the
            # only honest answer for UNKN, and it preserves what a previous run left.
            GPIO.setup(self.__pin, GPIO.OUT)
        else:
            # initial= is not cosmetic. Without it RPi.GPIO makes the pin an output at
            # whatever the level register happens to hold and writes nothing -- LOW after a
            # cold boot, and GPIO_ON is LOW, so every relay closed for the instant until
            # setState() below corrected it. With it the pin is already at its intended
            # level as it becomes an output: a relay meant to be off never closes, and one
            # meant to be on (a switch restoring its last state across an OMS restart,
            # whose pin the previous process left energized -- nothing calls
            # GPIO.cleanup(), so the level survives) is never pulsed off underneath a
            # running machine.
            GPIO.setup(self.__pin, GPIO.OUT, initial=level)
        self.setState(init)

    @property
    def pin(self):
        return self.__pin

    @property
    def state(self):
        return self.__state

    def setState(self, state):
        logger.debug("Changing state of switch at pin {} to {}".format(self.pin, state))
        level = State.toLevel(state)
        if level is None:
            return
        GPIO.output(self.pin, level)
        self.__state = state

    def on(self):
        logger.debug("Setting switch at pin {} to on".format(self.pin))
        self.setState(State.ON)

    def off(self):
        logger.debug("Setting switch at pin {} to off".format(self.pin))
        self.setState(State.OFF)

    def __del__(self):
        logger.debug("Destroying switch at pin {}".format(self.pin))
        if self.__onDestroy is not None:
            self.setState(self.__onDestroy)
