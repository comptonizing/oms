#pragma once

#include "common.h"

class Motor {
    public:
        typedef enum {
            EXTEND = 0,
            RETRACT = 1,
            UNKNOWN = 2
        } STATE;

        Motor(uint8_t pinExtend, uint8_t pinRetract, mytime maxExtended = 20000);
        void extend();
        void retract();
        bool isRetracted();
        mytime lastExtended() {
            return m_lastExtended;
        }
        mytime lastRetracted() {
            return m_lastRetracted;
        }
        void update();
        STATE requested();

    private:
        uint8_t m_pinExtend;
        uint8_t m_pinRetract;
        mytime m_maxExtended = 0;
        mytime m_lastExtended = 0;
        mytime m_lastRetracted = 0;
        STATE m_state = STATE::UNKNOWN;
};
