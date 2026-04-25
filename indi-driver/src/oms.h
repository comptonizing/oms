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

#include <termios.h>

#include <curl/curl.h>

#include <indibase.h>
#include <indiweatherinterface.h>
#include <defaultdevice.h>

#include "json.hpp"

#define TIMEOUT 2

using json = nlohmann::json;

class OMS : public INDI::DefaultDevice, public INDI::WeatherInterface {
    public:
        OMS();
        ~OMS();
    protected:
        virtual const char *getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual bool ISNewSwitch(const char * dev, const char * name, ISState * states, char * names[], int n) override;
        virtual bool ISNewNumber(const char *dev, const char *name, double *values, char *names[], int n) override;
        virtual bool ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n) override;
        virtual void ISGetProperties(const char *dev) override;
        virtual void TimerHit() override;
        virtual bool saveConfigItems(FILE *fp) override;
        virtual bool loadConfig(bool silent = false, const char *property = nullptr) override;
        virtual IPState updateWeather() override;

        ITextVectorProperty AddressTP;
        IText AddressT[2] {};
    private:
        static constexpr const char *WEATHER_TAB {"Weather"};

        virtual bool Handshake();
        bool update();
        std::string readURL(const std::string &url);
};


