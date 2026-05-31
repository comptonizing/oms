#include "common.h"
#include "motor.h"
#include "fans.h"

#define PIN_SW1 12
#define PIN_SW2 13
#define PIN_SW3 A0
#define PIN_SW4 A1
#define PIN_SW5 A2
#define PIN_SW6 A3
#define PIN_SW7 A4
#define PIN_SW8 A5

size_t switches[] = {PIN_SW1, PIN_SW2, PIN_SW3, PIN_SW4, PIN_SW5, PIN_SW6, PIN_SW7, PIN_SW8};
#define NSW 8

#define PIN_FANS 2

#define PIN_M1_1 5
#define PIN_M1_2 4
#define PIN_M2_1 7
#define PIN_M2_2 8
#define MAX_EXTENDED 20000

typedef enum {
  STATUS = 0,
  FANS_ON = 1,
  FANS_OFF = 2,
  WEST_EXTEND = 3,
  WEST_RETRACT = 4,
  EAST_EXTEND = 5,
  EAST_RETRACT = 6,
} CMD;

Motor motorWest(PIN_M1_1, PIN_M1_2, MAX_EXTENDED);
Motor motorEast(PIN_M2_1, PIN_M2_2, MAX_EXTENDED);
Fans fans(PIN_FANS);

void sendBits(uint16_t status) {
  Serial.write((char *)&status, 2);
}

typedef enum {
  SW1 = 0,
  SW2 = 1,
  SW3 = 2,
  SW4 = 3,
  SW5 = 4,
  SW6 = 5,
  SW7 = 6,
  SW8 = 7,
  FANS = 8,
  WEST_REQ = 9,
  WEST_STATE = 10,
  EAST_REQ = 11,
  EAST_STATE = 12,
  CMDUNKN = 13
} BITS;

void cmdStatus() {
    uint16_t status = 0;
    if ( digitalRead(PIN_SW1) ) {
        status |= (1 << BITS::SW1);
    }
    if ( digitalRead(PIN_SW2) ) {
        status |= (1 << BITS::SW2);
    }
    if ( digitalRead(PIN_SW3) ) {
        status |= (1 << BITS::SW3);
    }
    if ( digitalRead(PIN_SW4) ) {
        status |= (1 << BITS::SW4);
    }
    if ( digitalRead(PIN_SW5) ) {
        status |= (1 << BITS::SW5);
    }
    if ( digitalRead(PIN_SW6) ) {
        status |= (1 << BITS::SW6);
    }
    if ( digitalRead(PIN_SW7) ) {
        status |= (1 << BITS::SW7);
    }
    if ( digitalRead(PIN_SW8) ) {
        status |= (1 << BITS::SW8);
    }
    if ( fans.isOn() ) {
        status |= (1 << BITS::FANS);
    }
    if ( motorWest.requested() == Motor::STATE::EXTEND ) {
        status |= (1 << BITS::WEST_REQ );
    }
    if ( ! motorWest.isRetracted() ) {
        status |= (1 << BITS::WEST_STATE);
    }
    if ( motorEast.requested() == Motor::STATE::EXTEND ) {
        status |= (1 << BITS::EAST_REQ );
    }
    if ( ! motorEast.isRetracted() ) {
        status |= (1 << BITS::EAST_STATE);
    }
    sendBits(status);
}

void cmdUnkn() {
  uint16_t status = 0;
  status |= (1 << BITS::CMDUNKN);
  sendBits(status);
}

void cmdFansOn() {
  fans.on();
  cmdStatus();
}

void cmdFansOff() {
  fans.off();
  cmdStatus();
}

void cmdWestExtend() {
  motorWest.extend();
  cmdStatus();
}

void cmdWestRetract() {
  motorWest.retract();
  cmdStatus();
}

void cmdEastExtend() {
  motorEast.extend();
  cmdStatus();
}

void cmdEastRetract() {
  motorEast.retract();
  cmdStatus();
}

bool runcmd(uint16_t cmd) {
    if ( cmd & (1 << CMD::STATUS) ) {
      cmdStatus();
    } else if ( cmd & (1 << CMD::FANS_ON) ) {
      cmdFansOn();
    } else if ( cmd & (1 << CMD::FANS_OFF) ) {
      cmdFansOff();
    } else if ( cmd & (1 << CMD::EAST_EXTEND) ) {
      cmdEastExtend();
    } else if ( cmd & (1 << CMD::EAST_RETRACT) ) {
      cmdEastRetract();
    } else if ( cmd & (1 << CMD::WEST_EXTEND) ) {
      cmdWestExtend();
    } else if ( cmd & (1 << CMD::WEST_RETRACT) ) {
      cmdWestRetract();
    } else {
      cmdUnkn();
    }
}

void setup() {
  for (size_t ii=0; ii<NSW; ii++) {
    pinMode(switches[ii], INPUT);
  }
  Serial.begin(9600);
}

void loop() {
  motorWest.update();
  motorEast.update();
  while ( Serial.available() ) {
    uint16_t cmd = Serial.read(); // Reads a uint8_t into a uint16_t. I don't know why ...
    runcmd(cmd);
  }
  delay(10);
}
