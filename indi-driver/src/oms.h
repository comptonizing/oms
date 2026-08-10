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
#include <defaultdevice.h>
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


class OMS : public INDI::DefaultDevice, public INDI::WeatherInterface {
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

        ITextVectorProperty addressTP;
        IText addressT[2] {};
    private:
        static constexpr const char *WEATHER_TAB {"Weather"};

        bool readURL(const std::string &url, std::string &response);
        int parsePort(const char *str);
        void markUnsafe();

        std::string m_url = "";
};


