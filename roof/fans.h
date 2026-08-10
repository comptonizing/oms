#pragma once

#include "common.h"

class Fans {
    public:
        Fans(uint8_t pin);
        void on();
        void off();
        bool isOn();
    private:
        uint8_t m_pin;
        bool m_on = false;
};
