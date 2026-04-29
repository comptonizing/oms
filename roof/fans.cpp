#include "fans.h"

Fans::Fans(uint8_t pin) : m_pin(pin) {
    pinMode(pin, OUTPUT);
    off();
}

void Fans::off() {
    digitalWrite(m_pin, LOW);
    m_on = false;
}

void Fans::on() {
    digitalWrite(m_pin, HIGH);
    m_on = true;
}

bool Fans::isOn() {
    return m_on;
}
