/*
 *  This file is part of the Pollux Astro OMS software
 *
 *  Created by Philipp Weber
 *  Copyright (c) 2023 Philipp Weber
 *  All rights reserved.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <regex>
#include <map>

#include <curl/curl.h>

#include <indibase.h>
#include <indiweatherinterface.h>
#include <indidome.h>
#include <unordered_map>

#include "json.hpp"

#define TIMEOUT 2

using json = nlohmann::json;

/*
{
    "env_temperature": 8.56,
    "env_pressure": 967.52,
    "env_humidity": 46.0,
    "env_dewpoint": -2.42,
    "ir_sky": -18.69,
    "ir_ambient": 7.07,
    "ir_diff": 27.25,
    "rain_capacitance": 188.09,
    "rain_percentage": 0.0,
    "rain_temperature": 19.17,
    "rain_dutycycle": 17.02,
    "rain_heating": 0.58,
    "sqm_ir": 0.0,
    "sqm_full": 137.0,
    "sqm_vis": 137.0,
    "sqm_mpsas": 19.12,
    "sqm_dmpsas": 0.09,
    "sqm_integration": 600.0,
    "sqm_gain": 9876.0,
    "wind_speed": 0.86,
    "wind_gust": 2.43,
    "startup": 1.0
}
*/

struct weatherData {
    std::string id;
    std::string name;
    std::string label;
    double minOK;
    double maxOK;
    double percWarn;
    bool critical;
};

static const weatherData parameters[] = {
    weatherData {"env_temperature", "WEATHER_TEMPERATURE", "Temperature (C)", -20, 40, 15, false},
    weatherData {"env_pressure", "WEATHER_PRESSURE", "Pressure (mbar)", 900, 1100, 15, false},
    weatherData {"env_humidity", "WEATHER_HUMIDITY", "Humidity (%)", 0, 100, 15, false},
    weatherData {"env_dewpoint", "WEATHER_DEWPOINT", "Dewpoint (C)", -20, 40, 15, false},
    weatherData {"rain_percentage", "WEATHER_RAIN_PERCENTAGE", "Rain (%)", 0, 5, 15, true},
    weatherData {"sqm_mpsas", "WEATHER_SQM_MPSAS", "SQM (mpsas)", 15, 23, 15, true},
    weatherData {"wind_speed", "WEATHER_WIND_SPEED", "Wind (km/h)", 0, 20, 15, true},
    weatherData {"wind_gust", "WEATHER_WIND_GUST", "Gust (km/h)", 0, 40, 15, true},
    weatherData {"ir_sky", "WEATHER_IR_SKY", "Sky IR (C)", -30, -15, 15, false},
    weatherData {"ir_ambient", "WEATHER_IR_AMBIENT", "Sky ambient (C)", -20, 40, 15, false},
    weatherData {"ir_diff", "WEATHER_IR_DIFF", "Sky difference (C)", 15, 40, 15, true},
};

// The four plain on/off switches OMS drives as Pi GPIO pins. Fans are a fifth switch to an
// operator (and to /api/v1/switches) but are not one of these: they live on the roof board
// behind the serial port and take a third state ("auto"), so they get their own property.
struct switchData {
    std::string id;    // OMS's /api/v1/switches/{id} name
    std::string name;  // INDI switch vector name
    std::string label;
};

static const switchData switchDevices[] = {
    switchData {"pier", "SWITCH_PIER", "Pier"},
    switchData {"cctv", "SWITCH_CCTV", "CCTV"},
    switchData {"pc", "SWITCH_PC", "PC"},
    switchData {"allsky", "SWITCH_ALLSKY", "Allsky"},
};

static constexpr size_t NUM_SWITCHES = sizeof(switchDevices) / sizeof(switchDevices[0]);
static constexpr const char *FAN_ID = "fans";


// OMS is one physical box, so it is one INDI device with two roles: INDI::Dome drives the
// roof (open/close/stop map onto UnPark/Park/Abort - there is no azimuth, just the two-state
// roll-off roof OMS's own API already models), and the INDI::WeatherInterface mixin still
// reports the same station readings as before. Dome is itself a DefaultDevice subclass, so
// this replaces the old "public INDI::DefaultDevice" base rather than adding to it.
class OMS : public INDI::Dome, public INDI::WeatherInterface {
    public:
        OMS();
        ~OMS();
    protected:
        virtual const char *getDefaultName() override;
        virtual bool Connect() override;
        virtual bool Disconnect() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual bool ISNewSwitch(const char * dev, const char * name, ISState * states, char * names[], int n) override;
        virtual bool ISNewNumber(const char *dev, const char *name, double *values, char *names[], int n) override;
        virtual bool ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n) override;
        virtual void ISGetProperties(const char *dev) override;
        virtual bool saveConfigItems(FILE *fp) override;
        virtual IPState updateWeather() override;

        // Roof (INDI::Dome). DOME_CW/UnPark open the roof, DOME_CCW/Park close it - OMS has
        // no azimuth, so this is the same two-state model roll_off.cpp uses for a roll-off
        // roof, just backed by OMS's REST API instead of local limit-switch GPIO.
        virtual IPState Move(DomeDirection dir, DomeMotionCommand operation) override;
        virtual IPState Park() override;
        virtual IPState UnPark() override;
        virtual bool Abort() override;
        virtual void TimerHit() override;

        ITextVectorProperty addressTP;
        IText addressT[2] {};

        // OMS's roof has two controls beyond open/close/stop that don't map onto anything
        // INDI::Dome already models: handing the roof to/from an operator, and clearing a
        // latched motion fault. Exposed as their own switches rather than folded into Park/
        // UnPark, which client UIs assume only ever mean "move the roof".
        ISwitch engageS[2];
        ISwitchVectorProperty engageSP;
        ISwitch faultClearS[1];
        ISwitchVectorProperty faultClearSP;

        // pier/cctv/pc/allsky - plain on/off GPIO switches, one property each, under their
        // own tab rather than folded into Main Control or Options.
        ISwitch switchS[NUM_SWITCHES][2];
        ISwitchVectorProperty switchSP[NUM_SWITCHES];
        // Fans: a third state ("auto") the other four don't have, so its own property rather
        // than a fifth entry in switchDevices[].
        ISwitch fanS[3];
        ISwitchVectorProperty fanSP;
    private:
        static constexpr const char *WEATHER_TAB {"Weather"};
        static constexpr const char *SWITCHES_TAB {"Switches"};
        // Cadence TimerHit() re-arms itself at while connected. The GETs this drives just
        // read what the worker/settings last published (see roofDetail()'s docstring in
        // apiv1.py, and fanDetail()'s use of the same cached roof status word) rather than
        // triggering a serial exchange, so polling this often costs OMS nothing - it only
        // trades off how quickly INDI notices a state change.
        static constexpr uint32_t ROOF_POLL_MS {2000};

        bool request(bool isPost, const std::string &url, std::string &response);
        bool readURL(const std::string &url, std::string &response);
        bool sendRoofCommand(const std::string &command, std::string &response);
        bool sendSwitchCommand(const std::string &id, const std::string &state, std::string &response);
        void pollRoofState();
        void pollSwitches();
        int parsePort(const char *str);
        void markUnsafe();

        std::string m_url = "";
};
