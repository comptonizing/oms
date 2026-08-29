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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

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

// minOK/maxOK are the range outside which WeatherInterface calls a value dangerous, and
// percWarn is the width of the warning band at *each* end of that range, as a percentage
// of (maxOK - minOK) - so widening a range widens its warning bands with it unless
// percWarn comes down to match (see checkParameterState() in indiweatherinterface.cpp).
// The band at the minOK end is only applied when minOK is non-zero, which is why the
// one-sided readings below can sit at 0 all night without being called a warning.

static const weatherData parameters[] = {
    weatherData {"env_temperature", "WEATHER_TEMPERATURE", "Temperature (C)", -20, 40, 15, false},
    weatherData {"env_pressure", "WEATHER_PRESSURE", "Pressure (mbar)", 900, 1100, 15, false},
    weatherData {"env_humidity", "WEATHER_HUMIDITY", "Humidity (%)", 0, 100, 15, false},
    weatherData {"env_dewpoint", "WEATHER_DEWPOINT", "Dewpoint (C)", -20, 40, 15, false},
    weatherData {"rain_percentage", "WEATHER_RAIN_PERCENTAGE", "Rain (%)", 0, 5, 15, true},
    // The ceiling here is a bound on the *possible*, not on the acceptable: more mpsas is
    // a darker sky, which is the condition this observatory is for, and the old 23 called
    // anything better than 21.8 a warning and anything better than 23 an alert. The
    // darkest natural night sky is around 22, so 30 cannot be reached; what is left is the
    // floor, where a bright sky really does mean daylight or moon.
    weatherData {"sqm_mpsas", "WEATHER_SQM_MPSAS", "SQM (mpsas)", 12, 30, 5, true},
    weatherData {"wind_speed", "WEATHER_WIND_SPEED", "Wind (km/h)", 0, 20, 15, true},
    weatherData {"wind_gust", "WEATHER_WIND_GUST", "Gust (km/h)", 0, 40, 15, true},
    weatherData {"ir_sky", "WEATHER_IR_SKY", "Sky IR (C)", -30, -15, 15, false},
    weatherData {"ir_ambient", "WEATHER_IR_AMBIENT", "Sky ambient (C)", -20, 40, 15, false},
    // Same shape of mistake as SQM, and the one that mattered most: a bigger sky-to-ambient
    // difference is a clearer sky, and a cold clear winter night reaches 40 easily - which
    // the old ceiling reported as danger, on exactly the nights worth observing. 100 is
    // past anything the sensor can produce. percWarn drops to 5 along with the widening,
    // or the warning band would reach up to 27 and cover ordinary clear-sky readings.
    weatherData {"ir_diff", "WEATHER_IR_DIFF", "Sky difference (C)", 15, 100, 5, true},
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

// The board and relay detail roofDetail() (see apiv1.py) reports each tick, beyond the
// six-word summary pollRoofState() already reduces "state" to. All read-only: nothing here
// is a command, it is what the state machine used to decide the six-word summary.
struct lightData {
    std::string id;    // key within the JSON dict this light is read from
    std::string name;  // INDI light name
    std::string label;
};

static const lightData limitSwitchLights[] = {
    lightData {"west_closed_north", "WEST_CLOSED_NORTH", "West Closed North"},
    lightData {"west_closed_south", "WEST_CLOSED_SOUTH", "West Closed South"},
    lightData {"west_open_north", "WEST_OPEN_NORTH", "West Open North"},
    lightData {"west_open_south", "WEST_OPEN_SOUTH", "West Open South"},
    lightData {"east_closed_north", "EAST_CLOSED_NORTH", "East Closed North"},
    lightData {"east_closed_south", "EAST_CLOSED_SOUTH", "East Closed South"},
    lightData {"east_open_north", "EAST_OPEN_NORTH", "East Open North"},
    lightData {"east_open_south", "EAST_OPEN_SOUTH", "East Open South"},
};
static constexpr size_t NUM_LIMIT_SWITCHES = sizeof(limitSwitchLights) / sizeof(limitSwitchLights[0]);

static const lightData relayLights[] = {
    lightData {"west_open", "WEST_OPEN", "West Open"},
    lightData {"west_close", "WEST_CLOSE", "West Close"},
    lightData {"east_open", "EAST_OPEN", "East Open"},
    lightData {"east_close", "EAST_CLOSE", "East Close"},
};
static constexpr size_t NUM_RELAYS = sizeof(relayLights) / sizeof(relayLights[0]);

// Rods live under two keys deep ("rods"."west"."retracted", not a flat dict), so they get
// their own two-key lookup rather than reusing lightData's single id.
struct rodLightData {
    std::string half;   // "west" / "east"
    std::string field;  // "extendRequested" / "retracted"
    std::string name;
    std::string label;
};

static const rodLightData rodLights[] = {
    rodLightData {"west", "extendRequested", "WEST_EXTEND_REQUESTED", "West Extend Requested"},
    rodLightData {"west", "retracted", "WEST_RETRACTED", "West Retracted"},
    rodLightData {"east", "extendRequested", "EAST_EXTEND_REQUESTED", "East Extend Requested"},
    rodLightData {"east", "retracted", "EAST_RETRACTED", "East Retracted"},
};
static constexpr size_t NUM_RODS = sizeof(rodLights) / sizeof(rodLights[0]);


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
        // fanSP above is the commanded mode; this is switches()[fans]["state"], the board's
        // live answer to whether they're actually spinning - the two can disagree in "auto".
        ILight fanRunningL[1];
        ILightVectorProperty fanRunningLP;

        // Detailed roof status - limit switches, rod state, half position, relay drive and
        // a latched fault reason - on the main tab because that's where the roof controls
        // already are. All read-only and all sourced from the same /api/v1/roof poll that
        // already drives the six-word summary above.
        ILight limitSwitchL[NUM_LIMIT_SWITCHES];
        ILightVectorProperty limitSwitchLP;
        ILight rodL[NUM_RODS];
        ILightVectorProperty rodLP;
        ILight relayL[NUM_RELAYS];
        ILightVectorProperty relayLP;
        IText positionT[3] {};
        ITextVectorProperty positionTP;
        IText faultReasonT[1] {};
        ITextVectorProperty faultReasonTP;
        // "state" (opening/closing/...) is the six-word summary; this is the actual step
        // within it - OPEN_KICK, OPEN_RUN, OPEN_SETTLE, CLOSE_RODCLEAR, ... (see roofMotion
        // in oms/oms) - which is what a motion stuck mid-sequence needs to diagnose.
        IText motionPhaseT[1] {};
        ITextVectorProperty motionPhaseTP;

        // Inside temperature/humidity, from the board behind the environment sensor port -
        // a different sensor, port and staleness rule than the outdoor station above (see
        // /api/v1/environment's docstring), so its own tab rather than folded into Weather.
        INumber environmentN[2];
        INumberVectorProperty environmentNP;
    private:
        static constexpr const char *WEATHER_TAB {"Weather"};
        static constexpr const char *SWITCHES_TAB {"Switches"};
        static constexpr const char *ENVIRONMENT_TAB {"Environment"};
        // Cadence both TimerHit() re-arms itself at and the poll thread (below) sleeps
        // between fetches. The GETs this drives just read what the worker/settings last
        // published (see roofDetail()'s docstring in apiv1.py, and fanDetail()'s use of the
        // same cached roof status word) rather than triggering a serial exchange, so
        // polling this often costs OMS nothing - it only trades off how quickly INDI
        // notices a state change.
        static constexpr uint32_t ROOF_POLL_MS {2000};

        bool request(bool isPost, const std::string &url, std::string &response,
                bool quiet = false, std::string *errorOut = nullptr);
        bool readURL(const std::string &url, std::string &response, bool quiet = false,
                std::string *errorOut = nullptr);
        bool sendRoofCommand(const std::string &command, std::string &response);
        bool sendSwitchCommand(const std::string &id, const std::string &state, std::string &response);
        void pollRoofState();
        void pollSwitches();
        void pollEnvironment();
        void applyRoofState(bool ok, const std::string &response, const std::string &error);
        void applySwitches(bool ok, const std::string &response, const std::string &error);
        void applyEnvironment(const std::string &response, const std::string &error);
        int parsePort(const char *str);
        void markUnsafe();

        std::string m_url = "";
        // Guards m_url: ISNewText() writes it from the main thread, request() reads it
        // from both the main thread (commands) and the poll thread below (GETs).
        std::mutex m_urlMutex;

        // The three GETs TimerHit() used to run inline now run on this thread instead, so
        // the main event loop - the same one that reads commands off the wire from
        // indiserver - is never blocked on OMS's network. libindi's IDSet*/LOGF_* calls
        // write that wire with no locking of their own (see IDMessage() and friends in
        // indidriver.c), so this thread must never call them: request(..., quiet=true, ...)
        // keeps it from logging off-thread, and it only fetches raw JSON, stashing it here
        // for TimerHit() - main thread only - to parse, diff and publish.
        struct PollSnapshot {
            uint64_t generation = 0;
            bool roofOk = false;
            std::string roofResponse;
            std::string roofError;
            bool switchesOk = false;
            std::string switchesResponse;
            std::string switchesError;
            std::string environmentResponse;
            std::string environmentError;
        };
        std::mutex m_snapshotMutex;
        PollSnapshot m_snapshot;
        // TimerHit()-only, so it needs no lock: the generation last applied, to skip
        // re-parsing a snapshot the poll thread hasn't refreshed since.
        uint64_t m_appliedGeneration = 0;

        std::thread m_pollThread;
        std::atomic<bool> m_stopPoll {false};
        std::mutex m_stopMutex;
        std::condition_variable m_stopCv;

        void pollWorker();
        void startPolling();
        void stopPolling();
};
