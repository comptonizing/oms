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

using json = nlohmann::json;

static std::unique_ptr<OMS> OMSDriver(new OMS());

OMS::OMS() : WI(this) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    setVersion(1,0);
}

OMS::~OMS() {
    curl_global_cleanup();
}

bool OMS::Connect() {
    std::string response = "";
    if ( ! readURL("/api/v1/id", response) ) {
        return false;
    }
    if ( response != "OMS" ) {
        LOG_ERROR("Did not get OMS id");
        return false;
    }

    return true;
}

bool OMS::Disconnect() {
    return true;
}

void OMS::ISGetProperties(const char *dev) {
    INDI::DefaultDevice::ISGetProperties(dev);
    IUFillText(&addressT[0], "ADDRESS", "Address", "");
    IUFillText(&addressT[1], "PORT", "Port", "");
    IUFillTextVector(&addressTP, addressT, 2, getDeviceName(), "DEVICE_ADDRESS", "Server", CONNECTION_TAB,
            IP_RW, 60, IPS_IDLE);
    defineProperty(&addressTP);
    loadConfig(false, "DEVICE_ADDRESS");
}

const char *OMS::getDefaultName() {
    return "OMS";
}

bool OMS::initProperties() {
    INDI::DefaultDevice::initProperties();
    WI::initProperties(WEATHER_TAB, WEATHER_TAB);
    
    for ( const auto& v : parameters ) {
        addParameter(v.name, v.label, v.minOK, v.maxOK, v.percWarn);
    } 

    for ( const auto& v : parameters ) {
        if ( ! v.critical ) {
            continue;
        }
        setCriticalParameter(v.name);
    } 

    addDebugControl();
    addConfigurationControl();
    addPollPeriodControl();
    setDefaultPollingPeriod(500);
    setDriverInterface(AUX_INTERFACE | WEATHER_INTERFACE);

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
        if ( updateWeather() != IPS_OK ) {
            return false;
        }
    } else {
        WI::updateProperties();
    }
    return true;
}

void OMS::TimerHit() {
    if ( isConnected() ) {
    }
    SetTimer(getCurrentPollingPeriod());
}


bool OMS::ISNewSwitch(const char * dev, const char * name, ISState * states, char * names[], int n) {
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0) {
        if ( WI::processSwitch(dev, name, states, names, n) ) {
            return true;
        }
    }
    return INDI::DefaultDevice::ISNewSwitch(dev, name, states, names, n);
}

bool OMS::ISNewNumber(const char *dev, const char *name, double *values, char *names[], int n) {
    if ( dev != nullptr && strcmp(dev, getDeviceName()) == 0 ) {
        if ( WI::processNumber(dev, name, values, names, n) ) {
            return true;
        }
    }
    return INDI::DefaultDevice::ISNewNumber(dev, name, values, names, n);
}

IPState OMS::updateWeather() {
    std::string response = "";
    if ( ! readURL("/api/v1/weather", response) ) {
        return IPS_ALERT;
    }
    json data;
    try {
        data = json::parse(response);
    } catch ( json::exception &e ) {
        LOGF_ERROR("JSON parse error: %s\n%s", e.what(), response.c_str());
        return IPS_ALERT;
    } catch (...) {
        LOGF_ERROR("Unknown JSON parse error\n%s", response.c_str());
        return IPS_ALERT;
    }

    auto ret = IPS_OK;
    for ( const auto& v: parameters ) {
        double value;
        try {
            value = data[v.id].template get<double>();
        } catch ( json::exception &e ) {
            LOGF_ERROR("Error accessing %s: %s\n%s", v.id.c_str(), e.what(), response.c_str());
            ret = IPS_ALERT;
            continue;
        } catch (...) {
            LOGF_ERROR("Unknown error accessing %s\n%s", v.id.c_str(), response.c_str());
            ret = IPS_ALERT;
            continue;
        }
        setParameterValue(v.name, value);
    } 

    return ret;
}

int OMS::parsePort(const char *str) {
    auto re = std::regex("^[[:digit:]]+$");
    if ( ! std::regex_match(str, re) ) {
        return -1;
    }
    int port = atoi(str);
    if ( port < 1 || port > 65535 ) {
        return -1;
    }
    return port;
}

bool OMS::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if ( dev != nullptr && strcmp(dev, getDeviceName()) == 0 ) {
        if ( strcmp(name, addressTP.name) == 0 ) {
            auto s = IPS_OK;
            std::string host = "";
            int port = -1;
            for ( int ii = 0; ii<n; ii++ ) {
                if ( strcmp(names[ii], "ADDRESS") == 0 ) {
                    if ( strcmp(texts[ii], "") == 0 ) {
                        LOGF_ERROR("\"%s\" is not a valid host", texts[ii]);
                        s = IPS_ALERT;
                    }
                    host = texts[ii];
                }
                if ( strcmp(names[ii], "PORT") == 0 ) {
                    port = parsePort(texts[ii]);
                    if ( port == -1 ) {
                        LOGF_ERROR("\"%s\" is not a valid port", texts[ii]);
                        s = IPS_ALERT;
                    }
                }
            }
            addressTP.s = s;
            if ( s == IPS_OK ) {
                IUUpdateText(&addressTP, texts, names, n);
                char buff[256];
                snprintf(buff, 255, "http://%s:%d", host.c_str(), port);
                m_url = buff;
            }
            IDSetText(&addressTP, nullptr);
            return s == IPS_OK ? true : false;
        }
    }

    return INDI::DefaultDevice::ISNewText(dev, name, texts, names, n);
}

bool OMS::saveConfigItems(FILE * fp)
{
    IUSaveConfigText(fp, &addressTP);

    return true;
}

static size_t WriteCB(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

bool OMS::readURL(const std::string &url, std::string &response) {
    char curlErrorBuff[CURL_ERROR_SIZE] = ""; // Necessary, see curl docs
    std::string buff;
    std::string address = m_url + url;

    LOGF_DEBUG("URL: %s", url.c_str());

    if ( m_url == "" ) {
        LOG_ERROR("Connection details not provided!");
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

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_URL, address.c_str()) ) {
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
        LOGF_ERROR("Could query URL %s: %s",
                address.c_str(),
                strlen(curlErrorBuff) ? curlErrorBuff : curl_easy_strerror(res));
        return false;
    }

    LOGF_DEBUG("Response: %s", buff.c_str());

    response = buff;
    return true;
}
