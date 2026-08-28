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
    // Defensive: normally Disconnect() has already done this, but the poll thread must
    // never outlive the OMS object it captured by pointer.
    stopPolling();
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
    // arriving in that window would see "already unparked" and silently no-op. Run
    // synchronously here, on the connection thread, rather than through the poll thread
    // below - that one may not have produced a first snapshot yet.
    pollRoofState();
    pollSwitches();
    pollEnvironment();
    startPolling();
    SetTimer(ROOF_POLL_MS);
    return true;
}

bool OMS::Disconnect() {
    stopPolling();
    return true;
}

const char *OMS::getDefaultName() {
    return "OMS";
}

bool OMS::initProperties() {
    INDI::Dome::initProperties();
    WI::initProperties(WEATHER_TAB, WEATHER_TAB);

    // Two-state roof, not an azimuth position - nothing to park at beyond open/closed.
    SetParkDataType(PARK_NONE);

    // Built here rather than in an ISGetProperties() override: IUFillText() blanks the
    // property's text, and ISGetProperties() runs once per connecting client, so building
    // it there wiped out the address/port (and any loaded config) for every client after
    // the first.
    IUFillText(&addressT[0], "ADDRESS", "Address", "");
    IUFillText(&addressT[1], "PORT", "Port", "");
    IUFillTextVector(&addressTP, addressT, 2, getDeviceName(), "DEVICE_ADDRESS", "Server", CONNECTION_TAB,
            IP_RW, 60, IPS_IDLE);
    // Register before loading: loadConfig() dispatches the saved value at whatever property
    // is registered under that name, so reading the config before defineProperty() drops the
    // saved address on the floor - and still reports "Device configuration applied". This
    // also replaces the old defineProperty() in ISGetProperties(): DefaultDevice re-sends
    // every registered property to each client that asks, so once is enough.
    defineProperty(&addressTP);
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

    IUFillLight(&fanRunningL[0], "RUNNING", "Running", IPS_IDLE);
    IUFillLightVector(&fanRunningLP, fanRunningL, 1, getDeviceName(), "SWITCH_FANS_RUNNING", "Fans Actual",
            SWITCHES_TAB, IPS_IDLE);

    for ( size_t i = 0; i < NUM_LIMIT_SWITCHES; i++ ) {
        const auto& ld = limitSwitchLights[i];
        IUFillLight(&limitSwitchL[i], ld.name.c_str(), ld.label.c_str(), IPS_IDLE);
    }
    IUFillLightVector(&limitSwitchLP, limitSwitchL, NUM_LIMIT_SWITCHES, getDeviceName(), "ROOF_LIMIT_SWITCHES",
            "Limit Switches", MAIN_CONTROL_TAB, IPS_IDLE);

    for ( size_t i = 0; i < NUM_RODS; i++ ) {
        const auto& rd = rodLights[i];
        IUFillLight(&rodL[i], rd.name.c_str(), rd.label.c_str(), IPS_IDLE);
    }
    IUFillLightVector(&rodLP, rodL, NUM_RODS, getDeviceName(), "ROOF_RODS", "Rods", MAIN_CONTROL_TAB, IPS_IDLE);

    for ( size_t i = 0; i < NUM_RELAYS; i++ ) {
        const auto& rd = relayLights[i];
        IUFillLight(&relayL[i], rd.name.c_str(), rd.label.c_str(), IPS_IDLE);
    }
    IUFillLightVector(&relayLP, relayL, NUM_RELAYS, getDeviceName(), "ROOF_RELAYS", "Relays", MAIN_CONTROL_TAB,
            IPS_IDLE);

    IUFillText(&positionT[0], "ROOF", "Roof", "");
    IUFillText(&positionT[1], "WEST", "West", "");
    IUFillText(&positionT[2], "EAST", "East", "");
    IUFillTextVector(&positionTP, positionT, 3, getDeviceName(), "ROOF_POSITION", "Position", MAIN_CONTROL_TAB,
            IP_RO, 60, IPS_IDLE);

    IUFillText(&faultReasonT[0], "REASON", "Reason", "");
    IUFillTextVector(&faultReasonTP, faultReasonT, 1, getDeviceName(), "ROOF_FAULT_REASON", "Fault",
            MAIN_CONTROL_TAB, IP_RO, 60, IPS_IDLE);

    IUFillText(&motionPhaseT[0], "PHASE", "Phase", "");
    IUFillTextVector(&motionPhaseTP, motionPhaseT, 1, getDeviceName(), "ROOF_MOTION_PHASE", "Motion Phase",
            MAIN_CONTROL_TAB, IP_RO, 60, IPS_IDLE);

    IUFillNumber(&environmentN[0], "INSIDE_TEMPERATURE", "Temperature (C)", "%.2f", -50, 80, 0, 0);
    IUFillNumber(&environmentN[1], "INSIDE_HUMIDITY", "Humidity (%)", "%.2f", 0, 100, 0, 0);
    IUFillNumberVector(&environmentNP, environmentN, 2, getDeviceName(), "INSIDE_ENVIRONMENT", "Inside",
            ENVIRONMENT_TAB, IP_RO, 60, IPS_IDLE);

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
        defineProperty(&fanRunningLP);
        defineProperty(&limitSwitchLP);
        defineProperty(&rodLP);
        defineProperty(&relayLP);
        defineProperty(&positionTP);
        defineProperty(&faultReasonTP);
        defineProperty(&motionPhaseTP);
        defineProperty(&environmentNP);
    } else {
        deleteProperty(engageSP.name);
        deleteProperty(faultClearSP.name);
        for ( size_t i = 0; i < NUM_SWITCHES; i++ ) {
            deleteProperty(switchSP[i].name);
        }
        deleteProperty(fanSP.name);
        deleteProperty(fanRunningLP.name);
        deleteProperty(limitSwitchLP.name);
        deleteProperty(rodLP.name);
        deleteProperty(relayLP.name);
        deleteProperty(positionTP.name);
        deleteProperty(faultReasonTP.name);
        deleteProperty(motionPhaseTP.name);
        deleteProperty(environmentNP.name);
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
                // The poll thread reads m_url concurrently with this write.
                std::lock_guard<std::mutex> lock(m_urlMutex);
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

// --- polling -------------------------------------------------------------------------

// TimerHit() runs on the same single-threaded event loop that reads commands off the wire
// from indiserver (see libs/eventloop/eventloop.c's select() loop) - so it must never
// block on OMS's network, or an Abort sitting in that loop's read buffer would have to
// wait out however long the poll takes to time out. All it does now is pick up whatever
// pollWorker() (below) last fetched and, if it's new, parse and publish it - no I/O of
// its own.
void OMS::TimerHit() {
    if ( ! isConnected() ) {
        return;
    }

    PollSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        snap = m_snapshot;
    }
    if ( snap.generation != m_appliedGeneration ) {
        applyRoofState(snap.roofOk, snap.roofResponse, snap.roofError);
        applySwitches(snap.switchesOk, snap.switchesResponse, snap.switchesError);
        applyEnvironment(snap.environmentResponse, snap.environmentError);
        m_appliedGeneration = snap.generation;
    }
    SetTimer(ROOF_POLL_MS);
}

// The one function that runs on m_pollThread. It must never touch an INDI property or
// call LOG*/IDSet*/def*/delete* - those write the wire indiserver reads with no locking
// of their own (see IDMessage() and friends in indidriver.c), so a call from here could
// interleave with one TimerHit() or ISNewSwitch() makes on the main thread and corrupt
// the XML both are writing. request(..., quiet=true, ...) keeps readURL() from logging
// off-thread; everything fetched here is handed to TimerHit() as inert strings instead.
void OMS::pollWorker() {
    while ( ! m_stopPoll.load() ) {
        std::string roofResp, roofErr, switchResp, switchErr, envResp, envErr;
        bool roofOk = readURL("/api/v1/roof", roofResp, true, &roofErr);
        bool switchesOk = readURL("/api/v1/switches", switchResp, true, &switchErr);
        // Return value ignored here too, same as applyEnvironment() ignores it below -
        // a handled stale-503 still has its body in envResp either way.
        readURL("/api/v1/environment", envResp, true, &envErr);

        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            m_snapshot.roofOk = roofOk;
            m_snapshot.roofResponse = roofResp;
            m_snapshot.roofError = roofErr;
            m_snapshot.switchesOk = switchesOk;
            m_snapshot.switchesResponse = switchResp;
            m_snapshot.switchesError = switchErr;
            m_snapshot.environmentResponse = envResp;
            m_snapshot.environmentError = envErr;
            m_snapshot.generation++;
        }

        // Interruptible sleep so stopPolling() doesn't have to wait out a full idle period
        // on top of whatever request was in flight when it was asked to stop.
        std::unique_lock<std::mutex> lk(m_stopMutex);
        m_stopCv.wait_for(lk, std::chrono::milliseconds(ROOF_POLL_MS), [this]{ return m_stopPoll.load(); });
    }
}

void OMS::startPolling() {
    m_stopPoll.store(false);
    m_pollThread = std::thread(&OMS::pollWorker, this);
}

void OMS::stopPolling() {
    if ( ! m_pollThread.joinable() ) {
        return;
    }
    m_stopPoll.store(true);
    m_stopCv.notify_all();
    // Worst case this waits out one in-flight request per endpoint (TIMEOUT each) - paid
    // once, on disconnect, not on every tick the way it used to be paid on the main thread.
    m_pollThread.join();
}

// Fetches used only for the synchronous priming read in Connect() (see its comment) -
// pollWorker() above does the three GETs itself from then on. Kept as their own functions,
// rather than folded into Connect(), so the fetch-then-apply shape stays in one place.
void OMS::pollRoofState() {
    std::string response;
    bool ok = readURL("/api/v1/roof", response);
    applyRoofState(ok, response, "");
}

void OMS::pollSwitches() {
    std::string response;
    bool ok = readURL("/api/v1/switches", response);
    applySwitches(ok, response, "");
}

void OMS::pollEnvironment() {
    std::string response;
    // Return value ignored, same as applyEnvironment() ignores it: a handled stale-503
    // still has its body in response either way (see request()'s comment).
    readURL("/api/v1/environment", response);
    applyEnvironment(response, "");
}

// IUFillText() leaves .text as NULL rather than "" when the initial value is empty (see
// indidriver.c), and positionT/faultReasonT/motionPhaseT all start out empty - so read
// them through this, or std::string's operator!= runs strlen() on a null pointer the
// first time a poll compares an incoming value against one.
static const char *textOrEmpty(const IText &t) {
    return t.text ? t.text : "";
}

void OMS::applyRoofState(bool ok, const std::string &response, const std::string &error) {
    if ( ! ok ) {
        // readURL() already logged this itself when it wasn't quiet (the Connect() priming
        // read); when it was quiet (pollWorker(), off the main thread), error carries what
        // it would have logged, for us to say here instead.
        if ( ! error.empty() ) {
            LOGF_ERROR("%s", error.c_str());
        }
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

    // engageSP is also a command the operator sends, but it must still mirror the server
    // like every other polled property here: the web UI has its own Engage/Disengage
    // button on the same roofEngaged flag, so another client (or the roof disengaging
    // itself) can move it out from under whatever this client last clicked.
    if ( data.contains("engaged") && ! data["engaged"].is_null() ) {
        try {
            bool engaged = data.at("engaged").template get<bool>();
            bool curEngaged = engageS[0].s == ISS_ON;
            if ( curEngaged != engaged || engageSP.s != IPS_OK ) {
                engageS[0].s = engaged ? ISS_ON : ISS_OFF;
                engageS[1].s = engaged ? ISS_OFF : ISS_ON;
                engageSP.s = IPS_OK;
                IDSetSwitch(&engageSP, nullptr);
            }
        } catch ( json::exception &e ) {
        }
    }

    // "motion" is always present, unlike the detail below - roofDetail() sets it before
    // the "no reading yet" early return, since it's about the sequence driving the roof
    // rather than what the board last said.
    if ( data.contains("motion") && ! data["motion"].is_null() ) {
        const auto &motion = data["motion"];
        std::string phase;
        if ( motion.contains("phase") && ! motion["phase"].is_null() ) {
            try {
                phase = motion.at("phase").template get<std::string>();
            } catch ( json::exception &e ) {
                phase = "";
            }
        }
        bool running = false;
        try {
            running = motion.at("running").template get<bool>();
        } catch ( json::exception &e ) {
        }
        IPState want = running ? IPS_BUSY : IPS_IDLE;
        if ( motionPhaseTP.s != want || phase != textOrEmpty(motionPhaseT[0]) ) {
            IUSaveText(&motionPhaseT[0], phase.c_str());
            motionPhaseTP.s = want;
            IDSetText(&motionPhaseTP, nullptr);
        }
    }

    // Limit switches, rods and position are null together until the roof board has
    // answered once (see roofDetail()'s docstring); relays are not, since those come from
    // Pi GPIO the worker always knows. Each block below only pushes an update when
    // something actually changed, same as applySwitches() - a light vector updating every
    // 2s regardless would just be client chatter over what is, most of the time, a switch
    // sitting exactly where it was last tick.
    if ( data.contains("limitSwitches") && ! data["limitSwitches"].is_null() ) {
        const auto &sw = data["limitSwitches"];
        bool changed = limitSwitchLP.s != IPS_OK;
        for ( size_t i = 0; i < NUM_LIMIT_SWITCHES; i++ ) {
            const auto &ld = limitSwitchLights[i];
            bool engaged;
            try {
                engaged = sw.at(ld.id).template get<bool>();
            } catch ( json::exception &e ) {
                continue;
            }
            IPState want = engaged ? IPS_OK : IPS_IDLE;
            if ( limitSwitchL[i].s != want ) {
                limitSwitchL[i].s = want;
                changed = true;
            }
        }
        if ( changed ) {
            limitSwitchLP.s = IPS_OK;
            IDSetLight(&limitSwitchLP, nullptr);
        }
    }

    if ( data.contains("rods") && ! data["rods"].is_null() ) {
        const auto &rods = data["rods"];
        bool changed = rodLP.s != IPS_OK;
        for ( size_t i = 0; i < NUM_RODS; i++ ) {
            const auto &rd = rodLights[i];
            bool value;
            try {
                value = rods.at(rd.half).at(rd.field).template get<bool>();
            } catch ( json::exception &e ) {
                continue;
            }
            IPState want = value ? IPS_OK : IPS_IDLE;
            if ( rodL[i].s != want ) {
                rodL[i].s = want;
                changed = true;
            }
        }
        if ( changed ) {
            rodLP.s = IPS_OK;
            IDSetLight(&rodLP, nullptr);
        }
    }

    if ( data.contains("relays") && ! data["relays"].is_null() ) {
        const auto &rel = data["relays"];
        bool changed = relayLP.s != IPS_OK;
        for ( size_t i = 0; i < NUM_RELAYS; i++ ) {
            const auto &rd = relayLights[i];
            bool value;
            try {
                value = rel.at(rd.id).template get<bool>();
            } catch ( json::exception &e ) {
                continue;
            }
            IPState want = value ? IPS_OK : IPS_IDLE;
            if ( relayL[i].s != want ) {
                relayL[i].s = want;
                changed = true;
            }
        }
        if ( changed ) {
            relayLP.s = IPS_OK;
            IDSetLight(&relayLP, nullptr);
        }
    }

    if ( data.contains("position") && ! data["position"].is_null() ) {
        const auto &pos = data["position"];
        static const std::pair<const char *, size_t> positionFields[] = {
            {"roof", 0}, {"west", 1}, {"east", 2},
        };
        bool changed = positionTP.s != IPS_OK;
        for ( const auto &f : positionFields ) {
            std::string value;
            try {
                value = pos.at(f.first).template get<std::string>();
            } catch ( json::exception &e ) {
                continue;
            }
            if ( value != textOrEmpty(positionT[f.second]) ) {
                IUSaveText(&positionT[f.second], value.c_str());
                changed = true;
            }
        }
        if ( changed ) {
            positionTP.s = IPS_OK;
            IDSetText(&positionTP, nullptr);
        }
    }

    if ( data.contains("fault") && ! data["fault"].is_null() ) {
        const auto &fault = data["fault"];
        bool latched = false;
        bool known = fault.contains("latched") && ! fault["latched"].is_null();
        if ( known ) {
            try {
                latched = fault.at("latched").template get<bool>();
            } catch ( json::exception &e ) {
                known = false;
            }
        }
        std::string faultReason;
        if ( known && latched && fault.contains("reason") && ! fault["reason"].is_null() ) {
            try {
                faultReason = fault.at("reason").template get<std::string>();
            } catch ( json::exception &e ) {
                faultReason = "";
            }
        }
        IPState want = ! known ? IPS_IDLE : latched ? IPS_ALERT : IPS_OK;
        if ( faultReasonTP.s != want || faultReason != textOrEmpty(faultReasonT[0]) ) {
            IUSaveText(&faultReasonT[0], faultReason.c_str());
            faultReasonTP.s = want;
            IDSetText(&faultReasonTP, nullptr);
        }
    }
}

void OMS::applySwitches(bool ok, const std::string &response, const std::string &error) {
    if ( ! ok ) {
        // Transport failure already gets a log line out of readURL() itself when it isn't
        // quiet; the background poller passes its captured message through here instead
        // (see applyRoofState()'s comment) - either way, nothing more useful to say here
        // than leaving the last-known switch states in place.
        if ( ! error.empty() ) {
            LOGF_ERROR("%s", error.c_str());
        }
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

    if ( data.contains(FAN_ID) && data[FAN_ID].contains("state") && ! data[FAN_ID]["state"].is_null() ) {
        std::string state;
        try {
            state = data[FAN_ID]["state"].template get<std::string>();
        } catch ( json::exception &e ) {
            return;
        }
        // "unkn" (no roof reading yet) and "off" both read as not-running; the fan mode
        // switch above is what tells them apart, since it comes from settings rather than
        // the board.
        IPState want = state == "on" ? IPS_OK : IPS_IDLE;
        if ( fanRunningLP.s != IPS_OK || fanRunningL[0].s != want ) {
            fanRunningL[0].s = want;
            fanRunningLP.s = IPS_OK;
            IDSetLight(&fanRunningLP, nullptr);
        }
    }
}

void OMS::applyEnvironment(const std::string &response, const std::string &error) {
    // response.empty(), not ok/error, is what says nothing came back: /api/v1/environment
    // answers 503-with-a-body when the reading is merely stale (see its docstring), and
    // request() leaves that body in response either way.
    if ( response.empty() ) {
        // Genuinely nothing came back (host unreachable, etc.), as opposed to the handled
        // stale-503-with-a-body case above - surface it once, here, since the background
        // poller that fetched it may not log directly (see applyRoofState()'s comment).
        if ( ! error.empty() ) {
            LOGF_ERROR("%s", error.c_str());
        }
        if ( environmentNP.s != IPS_ALERT ) {
            environmentNP.s = IPS_ALERT;
            IDSetNumber(&environmentNP, nullptr);
        }
        return;
    }

    json data;
    try {
        data = json::parse(response);
    } catch ( json::exception &e ) {
        LOGF_ERROR("Environment JSON parse error: %s\n%s", e.what(), response.c_str());
        return;
    } catch (...) {
        LOGF_ERROR("Unknown environment JSON parse error\n%s", response.c_str());
        return;
    }

    double temperature, humidity;
    bool stale;
    try {
        temperature = data["inside_temperature"].template get<double>();
        humidity = data["inside_humidity"].template get<double>();
        stale = data.value("stale", false);
    } catch ( json::exception &e ) {
        // Also the "no reading yet" case: problem() answers {"error": ...}, with neither
        // field present.
        LOGF_ERROR("Error accessing environment fields: %s\n%s", e.what(), response.c_str());
        if ( environmentNP.s != IPS_ALERT ) {
            environmentNP.s = IPS_ALERT;
            IDSetNumber(&environmentNP, nullptr);
        }
        return;
    }

    environmentN[0].value = temperature;
    environmentN[1].value = humidity;
    environmentNP.s = stale ? IPS_ALERT : IPS_OK;
    IDSetNumber(&environmentNP, nullptr);
}

// --- HTTP --------------------------------------------------------------------------------

static size_t WriteCB(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string *)userp)->append((char *)contents, size * nmemb);
    return size * nmemb;
}

bool OMS::readURL(const std::string &url, std::string &response, bool quiet, std::string *errorOut) {
    return request(false, url, response, quiet, errorOut);
}

bool OMS::sendRoofCommand(const std::string &command, std::string &response) {
    return request(true, "/api/v1/roof/" + command, response);
}

bool OMS::sendSwitchCommand(const std::string &id, const std::string &state, std::string &response) {
    return request(true, "/api/v1/switches/" + id + "/" + state, response);
}

bool OMS::request(bool isPost, const std::string &url, std::string &response, bool quiet,
        std::string *errorOut) {
    char curlErrorBuff[CURL_ERROR_SIZE] = ""; // Necessary, see curl docs
    std::string buff;
    std::string urlBase;
    {
        // Read under lock: ISNewText() writes m_url from the main thread while this may
        // run on the poll thread (see the PollSnapshot comment in oms.h).
        std::lock_guard<std::mutex> lock(m_urlMutex);
        urlBase = m_url;
    }
    std::string address = urlBase + url;

    // quiet is true only for the poll thread's GETs. It must never call LOG*/IDSet*/etc -
    // those write the wire indiserver reads with no locking of their own - so on that path
    // this hands the message that would have been logged back through errorOut instead,
    // for the caller to log later, on the main thread.
    auto fail = [&](const std::string &msg) -> bool {
        if ( quiet ) {
            if ( errorOut ) {
                *errorOut = msg;
            }
        } else {
            LOGF_ERROR("%s", msg.c_str());
        }
        return false;
    };

    if ( ! quiet ) {
        LOGF_DEBUG("%s %s", isPost ? "POST" : "GET", url.c_str());
    }

    if ( urlBase.empty() ) {
        return fail("Connection details not provided!");
    }

    CURL *curl = curl_easy_init();
    if ( curl == NULL ) {
        curl_easy_cleanup(curl);
        return fail("Could not initialize curl!");
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlErrorBuff) ) {
        curl_easy_cleanup(curl);
        return fail("Could not set curl error buffer!");
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_URL, address.c_str()) ) {
        curl_easy_cleanup(curl);
        return fail(std::string("Could not use specified URL: ") +
                (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
    }

    if ( isPost ) {
        // The roof/weather commands take no body - POSTFIELDS("") is enough to make curl
        // send POST rather than GET, and CURLOPT_POST makes that explicit either way.
        if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_POST, 1L) ) {
            curl_easy_cleanup(curl);
            return fail(std::string("Could not set curl POST method: ") +
                    (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
        }

        if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "") ) {
            curl_easy_cleanup(curl);
            return fail(std::string("Could not set curl POST body: ") +
                    (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
        }
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCB) ) {
        curl_easy_cleanup(curl);
        return fail(std::string("Could not set curl write callback: ") +
                (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buff) ) {
        curl_easy_cleanup(curl);
        return fail(std::string("Could not set curl data buffer: ") +
                (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, TIMEOUT) ) {
        curl_easy_cleanup(curl);
        return fail(std::string("Could not set curl connect timeout: ") +
                (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT) ) {
        curl_easy_cleanup(curl);
        return fail(std::string("Could not set curl timeout: ") +
                (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
    }

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if ( CURLE_OK != res ) {
        return fail("Could query URL " + address + ": " +
                (strlen(curlErrorBuff) ? curlErrorBuff : curl_easy_strerror(res)));
    }

    // Set regardless of what httpCode turns out to be below: /api/v1/environment answers a
    // handled 503 with the reading still in the body when it's merely stale (see its
    // docstring), and a caller reading such a body on a false return should not have to
    // duplicate the curl call to get it.
    response = buff;

    if ( httpCode < 200 || httpCode >= 300 ) {
        return fail("URL " + address + " returned HTTP " + std::to_string(httpCode) + ": " + buff);
    }

    if ( ! quiet ) {
        LOGF_DEBUG("Response: %s", buff.c_str());
    }
    return true;
}
