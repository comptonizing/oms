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

    // OMS's own DEVICE_ADDRESS text property is the only connection surface this driver
    // has - Dome would otherwise register its own Serial/TCP connection plugins we never use.
    setDomeConnection(CONNECTION_NONE);
    // No azimuth, no shutter distinct from the roof itself: open or closed is the whole
    // state, exactly the model roll_off.cpp uses for a roll-off roof.
    SetDomeCapability(DOME_CAN_ABORT | DOME_CAN_PARK);

    setVersion(1,0);
}

OMS::~OMS() {
    curl_global_cleanup();
}

bool OMS::Connect() {
    std::string response = "";
    std::string expected = "OMS API v1";
    if ( ! readURL("/api/v1/id", response) ) {
        return false;
    }
    if ( response != expected ) {
        LOGF_ERROR("Did not get OMS id, expected \"%s\", got \"%s\"", expected.c_str(), response.c_str());
        return false;
    }

    // Without this, m_DomeState stays DOME_UNKNOWN until the first TimerHit() tick fires
    // (SetTimer() schedules a future call, not an immediate one) - and Dome::Park()/UnPark()
    // read m_DomeState to decide whether there is anything to do, so a park/unpark request
    // arriving in that window would see "already unparked" and silently no-op.
    pollRoofState();
    pollSwitches();
    SetTimer(ROOF_POLL_MS);
    return true;
}

bool OMS::Disconnect() {
    return true;
}

void OMS::ISGetProperties(const char *dev) {
    INDI::Dome::ISGetProperties(dev);
    defineProperty(&addressTP);
}

const char *OMS::getDefaultName() {
    return "OMS";
}

bool OMS::initProperties() {
    INDI::Dome::initProperties();
    WI::initProperties(WEATHER_TAB, WEATHER_TAB);

    // Two-state roof, not an azimuth position - nothing to park at beyond open/closed.
    SetParkDataType(PARK_NONE);

    // Built once here, not in ISGetProperties(): IUFillText() blanks the
    // property's text, and ISGetProperties() runs once per connecting
    // client, so building it there wiped out the address/port (and any
    // loaded config) for every client after the first.
    IUFillText(&addressT[0], "ADDRESS", "Address", "");
    IUFillText(&addressT[1], "PORT", "Port", "");
    IUFillTextVector(&addressTP, addressT, 2, getDeviceName(), "DEVICE_ADDRESS", "Server", CONNECTION_TAB,
            IP_RW, 60, IPS_IDLE);
    loadConfig(false, "DEVICE_ADDRESS");

    IUFillSwitch(&engageS[0], "ENGAGE", "Engage", ISS_ON);
    IUFillSwitch(&engageS[1], "DISENGAGE", "Disengage", ISS_OFF);
    IUFillSwitchVector(&engageSP, engageS, 2, getDeviceName(), "ROOF_ENGAGE", "Safety Checks", MAIN_CONTROL_TAB,
            IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillSwitch(&faultClearS[0], "CLEAR", "Clear Fault", ISS_OFF);
    IUFillSwitchVector(&faultClearSP, faultClearS, 1, getDeviceName(), "ROOF_FAULT_CLEAR", "Fault", MAIN_CONTROL_TAB,
            IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    for ( size_t i = 0; i < NUM_SWITCHES; i++ ) {
        const auto& sw = switchDevices[i];
        IUFillSwitch(&switchS[i][0], "ON", "On", ISS_OFF);
        IUFillSwitch(&switchS[i][1], "OFF", "Off", ISS_ON);
        IUFillSwitchVector(&switchSP[i], switchS[i], 2, getDeviceName(), sw.name.c_str(), sw.label.c_str(),
                SWITCHES_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);
    }

    IUFillSwitch(&fanS[0], "ON", "On", ISS_OFF);
    IUFillSwitch(&fanS[1], "OFF", "Off", ISS_OFF);
    IUFillSwitch(&fanS[2], "AUTO", "Auto", ISS_ON);
    IUFillSwitchVector(&fanSP, fanS, 3, getDeviceName(), "SWITCH_FANS", "Fans", SWITCHES_TAB,
            IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    for ( const auto& v : parameters ) {
        addParameter(v.name, v.label, v.minOK, v.maxOK, v.percWarn);
    }

    for ( const auto& v : parameters ) {
        if ( ! v.critical ) {
            continue;
        }
        setCriticalParameter(v.name);
    }

    // Dome::initProperties() already calls addDebugControl() and sets DOME_INTERFACE;
    // add Configuration ourselves and fold WEATHER_INTERFACE into what Dome just set.
    addConfigurationControl();
    setDriverInterface(getDriverInterface() | WEATHER_INTERFACE);

    return true;
}

bool OMS::updateProperties() {
    INDI::Dome::updateProperties();
    // WI::updateProperties() already calls checkWeatherUpdate() -> updateWeather()
    // when connecting; don't fetch a second time here.
    WI::updateProperties();

    if ( isConnected() ) {
        defineProperty(&engageSP);
        defineProperty(&faultClearSP);
        for ( size_t i = 0; i < NUM_SWITCHES; i++ ) {
            defineProperty(&switchSP[i]);
        }
        defineProperty(&fanSP);
    } else {
        deleteProperty(engageSP.name);
        deleteProperty(faultClearSP.name);
        for ( size_t i = 0; i < NUM_SWITCHES; i++ ) {
            deleteProperty(switchSP[i].name);
        }
        deleteProperty(fanSP.name);
    }

    return true;
}

bool OMS::ISNewSwitch(const char * dev, const char * name, ISState * states, char * names[], int n) {
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0) {
        if ( WI::processSwitch(dev, name, states, names, n) ) {
            return true;
        }

        if ( strcmp(name, engageSP.name) == 0 ) {
            IUUpdateSwitch(&engageSP, states, names, n);
            bool engage = engageS[0].s == ISS_ON;
            std::string response;
            if ( ! sendRoofCommand(engage ? "engage" : "disengage", response) ) {
                engageSP.s = IPS_ALERT;
                LOGF_ERROR("Could not %s the roof", engage ? "engage" : "disengage");
            } else {
                engageSP.s = IPS_OK;
                LOGF_INFO("Roof %s", engage ? "engaged" : "disengaged");
            }
            IDSetSwitch(&engageSP, nullptr);
            return true;
        }

        if ( strcmp(name, faultClearSP.name) == 0 ) {
            IUUpdateSwitch(&faultClearSP, states, names, n);
            faultClearS[0].s = ISS_OFF;
            std::string response;
            if ( ! sendRoofCommand("reset", response) ) {
                faultClearSP.s = IPS_ALERT;
                LOG_ERROR("Could not clear the roof fault");
            } else {
                faultClearSP.s = IPS_OK;
                LOG_INFO("Roof fault clear requested");
            }
            IDSetSwitch(&faultClearSP, nullptr);
            return true;
        }

        for ( size_t i = 0; i < NUM_SWITCHES; i++ ) {
            if ( strcmp(name, switchSP[i].name) != 0 ) {
                continue;
            }
            IUUpdateSwitch(&switchSP[i], states, names, n);
            bool on = switchS[i][0].s == ISS_ON;
            std::string response;
            if ( ! sendSwitchCommand(switchDevices[i].id, on ? "on" : "off", response) ) {
                switchSP[i].s = IPS_ALERT;
                LOGF_ERROR("Could not switch %s %s", switchDevices[i].label.c_str(), on ? "on" : "off");
            } else {
                switchSP[i].s = IPS_OK;
                LOGF_INFO("%s switched %s", switchDevices[i].label.c_str(), on ? "on" : "off");
            }
            IDSetSwitch(&switchSP[i], nullptr);
            return true;
        }

        if ( strcmp(name, fanSP.name) == 0 ) {
            IUUpdateSwitch(&fanSP, states, names, n);
            std::string state = fanS[0].s == ISS_ON ? "on" : fanS[1].s == ISS_ON ? "off" : "auto";
            std::string response;
            if ( ! sendSwitchCommand(FAN_ID, state, response) ) {
                fanSP.s = IPS_ALERT;
                LOGF_ERROR("Could not set fans to %s", state.c_str());
            } else {
                fanSP.s = IPS_OK;
                LOGF_INFO("Fans set to %s", state.c_str());
            }
            IDSetSwitch(&fanSP, nullptr);
            return true;
        }
    }
    return INDI::Dome::ISNewSwitch(dev, name, states, names, n);
}

bool OMS::ISNewNumber(const char *dev, const char *name, double *values, char *names[], int n) {
    if ( dev != nullptr && strcmp(dev, getDeviceName()) == 0 ) {
        if ( WI::processNumber(dev, name, values, names, n) ) {
            return true;
        }
    }
    return INDI::Dome::ISNewNumber(dev, name, values, names, n);
}

void OMS::markUnsafe() {
    critialParametersLP.setState(IPS_ALERT);
    critialParametersLP.apply();
    SafetyStatusLP.setState(IPS_ALERT);
    SafetyStatusLP.apply();
}

IPState OMS::updateWeather() {
    std::string response = "";
    if ( ! readURL("/api/v1/weather", response) ) {
        markUnsafe();
        return IPS_ALERT;
    }
    json data;
    try {
        data = json::parse(response);
    } catch ( json::exception &e ) {
        LOGF_ERROR("JSON parse error: %s\n%s", e.what(), response.c_str());
        markUnsafe();
        return IPS_ALERT;
    } catch (...) {
        LOGF_ERROR("Unknown JSON parse error\n%s", response.c_str());
        markUnsafe();
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

    if ( ret == IPS_ALERT ) {
        markUnsafe();
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

    return INDI::Dome::ISNewText(dev, name, texts, names, n);
}

bool OMS::saveConfigItems(FILE * fp)
{
    INDI::Dome::saveConfigItems(fp);
    WI::saveConfigItems(fp);
    IUSaveConfigText(fp, &addressTP);

    return true;
}

// --- roof motion (INDI::Dome) -----------------------------------------------------------

IPState OMS::Move(DomeDirection dir, DomeMotionCommand operation) {
    if ( operation == MOTION_STOP ) {
        // Goes through the same wrapper Dome::ISNewSwitch's Abort switch does, which
        // handles the ParkSP/DomeMotionSP bookkeeping before it calls our Abort() virtually.
        return Dome::Abort() ? IPS_OK : IPS_ALERT;
    }

    std::string command = dir == DOME_CW ? "open" : "close";
    std::string response;
    if ( ! sendRoofCommand(command, response) ) {
        LOGF_ERROR("Could not request roof %s", command.c_str());
        return IPS_ALERT;
    }
    LOGF_INFO("Roof %s requested.", command.c_str());
    return IPS_BUSY;
}

IPState OMS::Park() {
    // Explicitly qualified so the base wrapper's busy/parked bookkeeping runs before it
    // calls our Move() override virtually - the same reason roll_off.cpp does this.
    IPState rc = INDI::Dome::Move(DOME_CCW, MOTION_START);
    if ( rc == IPS_BUSY ) {
        LOG_INFO("Roof is closing...");
        return IPS_BUSY;
    }
    return IPS_ALERT;
}

IPState OMS::UnPark() {
    IPState rc = INDI::Dome::Move(DOME_CW, MOTION_START);
    if ( rc == IPS_BUSY ) {
        LOG_INFO("Roof is opening...");
        return IPS_BUSY;
    }
    return IPS_ALERT;
}

bool OMS::Abort() {
    std::string response;
    if ( ! sendRoofCommand("stop", response) ) {
        LOG_ERROR("Could not request roof stop");
        return false;
    }
    LOG_INFO("Roof stop requested.");
    return true;
}

void OMS::TimerHit() {
    if ( ! isConnected() ) {
        return;
    }
    pollRoofState();
    pollSwitches();
    SetTimer(ROOF_POLL_MS);
}

void OMS::pollRoofState() {
    std::string response;
    if ( ! readURL("/api/v1/roof", response) ) {
        setDomeState(DOME_ERROR);
        return;
    }

    json data;
    try {
        data = json::parse(response);
    } catch ( json::exception &e ) {
        LOGF_ERROR("Roof state JSON parse error: %s\n%s", e.what(), response.c_str());
        setDomeState(DOME_ERROR);
        return;
    } catch (...) {
        LOGF_ERROR("Unknown roof state JSON parse error\n%s", response.c_str());
        setDomeState(DOME_ERROR);
        return;
    }

    std::string state;
    try {
        state = data["state"].template get<std::string>();
    } catch ( json::exception &e ) {
        LOGF_ERROR("Roof state missing \"state\": %s\n%s", e.what(), response.c_str());
        setDomeState(DOME_ERROR);
        return;
    }

    std::string reason;
    if ( data.contains("reason") && ! data["reason"].is_null() ) {
        try {
            reason = data["reason"].template get<std::string>();
        } catch ( json::exception &e ) {
            reason = "";
        }
    }

    // roofDecisiveState() on the OMS side already reduces everything the board and the
    // relays can say to one of six words (see RoofStatus in oms/oms); mirror that directly
    // rather than re-deriving it from limit switches, which the driver never sees.
    if ( state == "open" ) {
        if ( isParked() ) {
            SetParked(false);
        } else if ( getDomeState() != DOME_UNPARKED ) {
            setDomeState(DOME_UNPARKED);
        }
    } else if ( state == "closed" ) {
        if ( ! isParked() ) {
            SetParked(true);
        } else if ( getDomeState() != DOME_PARKED ) {
            setDomeState(DOME_PARKED);
        }
    } else if ( state == "opening" ) {
        setDomeState(DOME_UNPARKING);
    } else if ( state == "closing" ) {
        setDomeState(DOME_PARKING);
    } else if ( state == "disengaged" ) {
        if ( getDomeState() != DOME_UNKNOWN ) {
            LOGF_WARN("Roof is disengaged: %s", reason.empty() ? "safety checks are off" : reason.c_str());
        }
        setDomeState(DOME_UNKNOWN);
    } else {
        // "fault", or anything this driver doesn't recognize yet.
        if ( getDomeState() != DOME_ERROR ) {
            LOGF_ERROR("Roof fault: %s", reason.empty() ? "unknown" : reason.c_str());
        }
        setDomeState(DOME_ERROR);
    }
}

void OMS::pollSwitches() {
    std::string response;
    if ( ! readURL("/api/v1/switches", response) ) {
        // Transport failure already gets a log line out of readURL() itself; nothing more
        // useful to say here than leaving the last-known switch states in place.
        return;
    }

    json data;
    try {
        data = json::parse(response);
    } catch ( json::exception &e ) {
        LOGF_ERROR("Switches JSON parse error: %s\n%s", e.what(), response.c_str());
        return;
    } catch (...) {
        LOGF_ERROR("Unknown switches JSON parse error\n%s", response.c_str());
        return;
    }

    for ( size_t i = 0; i < NUM_SWITCHES; i++ ) {
        const auto& sw = switchDevices[i];
        if ( ! data.contains(sw.id) ) {
            continue;
        }
        std::string apiState;
        try {
            apiState = data[sw.id]["state"].template get<std::string>();
        } catch ( json::exception &e ) {
            continue;
        }
        bool wantOn = apiState == "on";
        IPState wantIPS = apiState == "unkn" ? IPS_IDLE : IPS_OK;
        bool curOn = switchS[i][0].s == ISS_ON;
        if ( curOn == wantOn && switchSP[i].s == wantIPS ) {
            continue;
        }
        switchS[i][0].s = wantOn ? ISS_ON : ISS_OFF;
        switchS[i][1].s = wantOn ? ISS_OFF : ISS_ON;
        switchSP[i].s = wantIPS;
        IDSetSwitch(&switchSP[i], nullptr);
    }

    if ( data.contains(FAN_ID) && data[FAN_ID].contains("mode") && ! data[FAN_ID]["mode"].is_null() ) {
        std::string mode;
        try {
            mode = data[FAN_ID]["mode"].template get<std::string>();
        } catch ( json::exception &e ) {
            return;
        }
        ISState wantOn = mode == "on" ? ISS_ON : ISS_OFF;
        ISState wantOff = mode == "off" ? ISS_ON : ISS_OFF;
        ISState wantAuto = mode == "auto" ? ISS_ON : ISS_OFF;
        if ( fanS[0].s != wantOn || fanS[1].s != wantOff || fanS[2].s != wantAuto ) {
            fanS[0].s = wantOn;
            fanS[1].s = wantOff;
            fanS[2].s = wantAuto;
            fanSP.s = IPS_OK;
            IDSetSwitch(&fanSP, nullptr);
        }
    }
}

// --- HTTP --------------------------------------------------------------------------------

static size_t WriteCB(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

bool OMS::readURL(const std::string &url, std::string &response) {
    return request(false, url, response);
}

bool OMS::sendRoofCommand(const std::string &command, std::string &response) {
    return request(true, "/api/v1/roof/" + command, response);
}

bool OMS::sendSwitchCommand(const std::string &id, const std::string &state, std::string &response) {
    return request(true, "/api/v1/switches/" + id + "/" + state, response);
}

bool OMS::request(bool isPost, const std::string &url, std::string &response) {
    char curlErrorBuff[CURL_ERROR_SIZE] = ""; // Necessary, see curl docs
    std::string buff;
    std::string address = m_url + url;

    LOGF_DEBUG("%s %s", isPost ? "POST" : "GET", url.c_str());

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

    if ( isPost ) {
        // The roof/weather commands take no body - POSTFIELDS("") is enough to make curl
        // send POST rather than GET, and CURLOPT_POST makes that explicit either way.
        if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_POST, 1L) ) {
            LOGF_ERROR("Could not set curl POST method: %s",
                    strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
            curl_easy_cleanup(curl);
            return false;
        }

        if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "") ) {
            LOGF_ERROR("Could not set curl POST body: %s",
                    strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
            curl_easy_cleanup(curl);
            return false;
        }
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

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT) ) {
        LOGF_ERROR("Could not set curl connect timeout: %s",
                strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
        curl_easy_cleanup(curl);
        return false;
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT) ) {
        LOGF_ERROR("Could not set curl timeout: %s",
                strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error");
        curl_easy_cleanup(curl);
        return false;
    }

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if ( CURLE_OK != res ) {
        LOGF_ERROR("Could query URL %s: %s",
                address.c_str(),
                strlen(curlErrorBuff) ? curlErrorBuff : curl_easy_strerror(res));
        return false;
    }

    if ( httpCode < 200 || httpCode >= 300 ) {
        LOGF_ERROR("URL %s returned HTTP %ld: %s", address.c_str(), httpCode, buff.c_str());
        return false;
    }

    LOGF_DEBUG("Response: %s", buff.c_str());

    response = buff;
    return true;
}
