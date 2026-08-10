#include "motor.h"

Motor::Motor(uint8_t pinExtend, uint8_t pinRetract, mytime maxExtended) :
    m_pinExtend(pinExtend), m_pinRetract(pinRetract), m_maxExtended(maxExtended) {
    pinMode(m_pinExtend, OUTPUT);
    pinMode(m_pinRetract, OUTPUT);
    retract();
}

void Motor::extend() {
    digitalWrite(m_pinRetract, LOW);
    digitalWrite(m_pinExtend, HIGH);
    m_lastExtended = millis();
    m_state = STATE::EXTEND;
}

void Motor::retract() {
    digitalWrite(m_pinExtend, LOW);
    digitalWrite(m_pinRetract, HIGH);
    m_lastRetracted = millis();
    m_state = STATE::RETRACT;
}

void Motor::update() {
    if ( m_state == STATE::EXTEND ) {
        if ( (unsigned long)(millis() - m_lastExtended) >= m_maxExtended ) {
            retract();
        }
    }
}

bool Motor::isRetracted() {
    if ( m_state == STATE::RETRACT ) {
        if ( (unsigned long)(millis() - m_lastRetracted) >= m_maxExtended ) {
            return true;
        }
    }
    return false;
}

Motor::STATE Motor::requested() {
    return m_state;
}
