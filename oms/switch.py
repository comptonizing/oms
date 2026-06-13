from enum import Enum
import RPi.GPIO as GPIO
import logging

logger = logging.getLogger(__name__)

GPIO.setmode(GPIO.BCM)

GPIO_ON = GPIO.LOW
GPIO_OFF = GPIO.HIGH

class State(Enum):
    OFF = 0
    ON = 1
    UNKN = 2

class Switch():
    def __init__(self, pin, init=State.OFF):
        logger.debug("Making a switch, pin {}, initial state {}".format(pin, init))
        self.__pin = pin
        self.__state = State.UNKN
        GPIO.setup(self.__pin, GPIO.OUT)
        self.setState(init)

    @property
    def pin(self):
        return self.__pin

    @property
    def state(self):
        return self.__state

    def setState(self, state):
        logger.debug("Changing state of switch at pin {} to {}".format(self.pin, state))
        if state is State.ON:
            GPIO.output(self.pin, GPIO_ON)
            self.__state = State.ON
        elif state is State.OFF:
            GPIO.output(self.pin, GPIO_OFF)
            self.__state = State.OFF

    def on(self):
        logger.debug("Setting switch at pin {} to on".format(self.pin))
        self.setState(State.ON)

    def off(self):
        logger.debug("Setting switch at pin {} to off".format(self.pin))
        self.setState(State.OFF)
