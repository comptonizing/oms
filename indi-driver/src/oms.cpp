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

#include "oms.h"
#include <defaultdevice.h>

using json = nlohmann::json;

static std::unique_ptr<OMS> OMSDriver(new OMS());

OMS::OMS() : WI(this) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    setDefaultPollingPeriod(1000);
    setVersion(1,0);

}

OMS::~OMS() {
    curl_global_cleanup();
}

void OMS::ISGetProperties(const char *dev) {
    INDI::DefaultDevice::ISGetProperties(dev);
    IUFillText(&AddressT[0], "ADDRESS", "Address", "");
    IUFillText(&AddressT[1], "PORT", "Port", "");
    IUFillTextVector(&AddressTP, AddressT, 2, getDeviceName(), "DEVICE_ADDRESS", "Server", COMMUNICATION_TAB,
            IP_RW, 60, IPS_IDLE);
    defineProperty(&AddressTP);
    loadConfig(false, "DEVICE_ADDRESS");
}

const char *OMS::getDefaultName() {
    return "OMS";
}


bool OMS::Handshake() {
    return true;
}

bool OMS::initProperties() {
    INDI::DefaultDevice::initProperties();
    WI::initProperties(WEATHER_TAB, WEATHER_TAB);
    setDriverInterface(AUX_INTERFACE | WEATHER_INTERFACE);
    addDebugControl();
    addConfigurationControl();
    setDefaultPollingPeriod(500);
    addPollPeriodControl();


    // addParameter("WEATHER_TEMPERATURE", "Temperature (C)", -30, 60, 15);
    // setCriticalParameter("WEATHER_TEMPERATURE");

    return true;
}

bool OMS::loadConfig(bool silent, const char *property) {
    DefaultDevice::loadConfig(silent, property);
    if ( property == nullptr ) {
    }
    return true;
}

bool OMS::updateProperties() {
    INDI::DefaultDevice::updateProperties();
    if ( isConnected() ) {
        WI::updateProperties();
        if ( ! update() ) {
            LOG_ERROR("Device communication failed!");
            return false;
        }
    } else {
        WI::updateProperties();
    }
    return true;
}

bool OMS::update() {
    return true;
}

void OMS::TimerHit() {
    if ( isConnected() ) {
        update(); // can ignore return code here
    }
    SetTimer(getCurrentPollingPeriod());
}


bool OMS::ISNewSwitch(const char * dev, const char * name, ISState * states, char * names[], int n) {
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0) {
    }
    return INDI::DefaultDevice::ISNewSwitch(dev, name, states, names, n);
}

bool OMS::ISNewNumber(const char *dev, const char *name, double *values, char *names[], int n) {
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0) {
        if (strstr(name, "WEATHER_")) {
            return WI::processNumber(dev, name, values, names, n);
        }
    }
    return INDI::DefaultDevice::ISNewNumber(dev, name, values, names, n);
}

IPState OMS::updateWeather() {
    if ( ! update() ) {
        return IPS_ALERT;
    }
    // setParameterValue already called in setEnvironment()
    return IPS_OK;
}

bool OMS::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if ( dev != nullptr && strcmp(dev, getDeviceName()) == 0 )
    {
        if (!strcmp(name, AddressTP.name))
        {
            IUUpdateText(&AddressTP, texts, names, n);
            AddressTP.s = IPS_OK;
            IDSetText(&AddressTP, nullptr);
            return true;
        }
    }

    return false;
}

bool OMS::saveConfigItems(FILE * fp)
{
    IUSaveConfigText(fp, &AddressTP);

    return true;
}


std::string readURL(const std::string &url) {
    /*
    char curlErrorBuff[CURL_ERROR_SIZE] = ""; // Necessary, see curl docs
    std::string buff;

    if ( addressTP[0].getText() == nullptr ) {
        LOG_ERROR("Address not defined!");
        return false;
    }

    CURL *curl = curl_easy_init();
    if ( curl == NULL ) {
        LOG_ERROR("Could not initialize curl!");
        curl_easy_cleanup(curl);
        return false;
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuff) ) {
        LOG_ERROR("Could not set curl error buffer!");
        curl_easy_cleanup(curl);
        return false;
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_URL, addressTP[0].getText()) ) {
        LOGF_ERROR("Could not use specified URL: %s",
                strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
        curl_easy_cleanup(curl);
        return false;
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCB) ) {
        LOGF_ERROR("Could not set curl write callback: %s",
                strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
        curl_easy_cleanup(curl);
        return false;
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buff) ) {
        LOGF_ERROR("Could not set curl data buffer: %s",
                strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
        curl_easy_cleanup(curl);
        return false;
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if ( CURLE_OK != res ) {
        LOGF_ERROR("Could not read data from Cloudwatcher: %s",
                strlen(curlErrorBuff) ? curlErrorBuff : curl_easy_strerror(res));
        return false;
    }

    try {
        m_lastData = std::make_unique<CloudwatcherData>(buff);
    } catch (const std::exception& e) {
        LOGF_ERROR("Could not decode values from device: %s", e.what());
    }

    */
    return "";
}
