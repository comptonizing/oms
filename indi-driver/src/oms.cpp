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
    pollStatus();
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

    // Registered with minOK == maxOK == 0, which is not a degenerate range but a documented
    // second mode of addParameter(): it creates no WEATHER_ROOF_HELD range property at all,
    // and checkParameterState() then reports a parameter with no range as danger whenever
    // its value is non-zero (see indiweatherinterface.cpp). That is exactly the semantics
    // wanted - the value is a flag, not a measurement, so there is no threshold to pick -
    // and it has the property none of the readings above have: there is nothing on the
    // Weather tab for an operator to widen, and nothing saved to the driver config that
    // could come back wrong. OMS's interlock cannot be talked out of by INDI configuration.
    addParameter(ROOF_HELD_PARAM, "Roof held shut", 0, 0, 0);
    setCriticalParameter(ROOF_HELD_PARAM);

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

        // Re-send the roof's park state now that ParkSP has been defined to the client.
        //
        // Connect() primes the roof state before this runs, and has to: Park()/UnPark()
        // read m_DomeState to decide whether there is anything to do, so a command arriving
        // before the first TimerHit() would see a roof that is still DOME_UNKNOWN (see
        // there). But updateProperties() is what defines ParkSP to the client, so the
        // apply() that priming triggers goes out as a setSwitchVector for a property this
        // client has not been sent a defSwitchVector for yet - and a client drops a set for
        // a property it does not know about.
        //
        // The def that follows a moment later carries the right value, so the INDI Control
        // Panel shows the roof correctly and nothing looked wrong. Ekos is where it hurt:
        // ISD::Dome tracks park status in processSwitch(), which runs for set-events only,
        // while registerProperty() takes nothing from a def but the fact that the dome can
        // park. With no set to act on, Ekos held the roof at PARK_UNKNOWN for the whole
        // session - and SchedulerProcess::isDomeParked() answers PARK_UNKNOWN with false,
        // so a closed roof read as not parked, and unParkDome() would ask an already open
        // one to open again.
        //
        // One apply() after the def fixes it, and re-applying a value the def already
        // carried costs a client nothing. The other polled properties need no equivalent:
        // their defs carry the primed values too, and nothing derives state from their sets
        // the way ISD::Dome does from this one.
        //
        // The Observatory tab is where this is visible. Observatory::setDome() fills its
        // position field with the placeholder "N/A" for a roll-off roof, then ends by
        // calling setDomeParkStatus(m_Dome->parkStatus()) to replace it with Open or
        // Closed. PARK_UNKNOWN falls through that switch's default case, so the
        // placeholder stayed on screen for the whole session - which is what a roof
        // reported as "N/A" on a freshly launched driver meant.
        ParkSP.apply();
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
    // Reads what the poll thread last brought back rather than fetching. WeatherInterface
    // calls this from its own timer on the INDI event loop - the single thread that also
    // has to read commands off the wire from indiserver - so the blocking HTTP request
    // that used to be the first line of this function stalled that loop for as long as OMS
    // took to answer, which on a busy OMS is exactly the wait this driver was seeing time
    // out. The reading is at most one poll old, which is far fresher than the once-a-minute
    // cadence this is called on.
    //
    // weatherUsable() is false for the three cases that mean the same thing here: no
    // reading has arrived, OMS called the last one stale, or the last one has since aged
    // past the limit OMS gave for it. None is a reading to publish, so none is
    // distinguished.
    // Recorded as it is read, so publishWeatherIfChanged() has something to compare a
    // later poll against: this is the only place that knows what actually went out.
    m_weatherReported = weatherUsable();
    if ( ! m_weatherReported ) {
        markUnsafe();
        return IPS_ALERT;
    }
    const json &data = m_weatherData;

    auto ret = IPS_OK;
    for ( const auto& v: parameters ) {
        double value;
        try {
            value = data.at(v.id).template get<double>();
        } catch ( json::exception &e ) {
            LOGF_ERROR("Error accessing %s: %s", v.id.c_str(), e.what());
            ret = IPS_ALERT;
            continue;
        } catch (...) {
            LOGF_ERROR("Unknown error accessing %s", v.id.c_str());
            ret = IPS_ALERT;
            continue;
        }
        setParameterValue(v.name, value);
    }

    // Folded in here, as one more critical parameter, rather than forced onto the light
    // after the fact. syncCriticalParameters() recomputes the whole of WEATHER_STATUS from
    // these values every time the weather updates, so anything applied on top of it would
    // be undone on the next tick - and a WEATHER_STATUS that goes green for even one tick
    // is not a cosmetic problem here. Ekos reads a green tick during a weather shutdown as
    // "Safety has improved", wakes the scheduler, and runs the startup procedure, whose
    // first act is to unpark the dome: the roof-open attempt this parameter exists to
    // prevent. Reading the flag rather than fetching it keeps this off the roof endpoint;
    // applyRoofState() has it fresh from the poll thread within ROOF_POLL_MS.
    setParameterValue(ROOF_HELD_PARAM, m_roofHeldShut ? 1.0 : 0.0);

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
        applyStatus(snap.ok, snap.response, snap.error);
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
        std::string response, error;
        // One request for the roof, the switches, the environment and the weather, where
        // this used to make three and updateWeather() a fourth. OMS serves its API from
        // the same event loop as its own web UI, so every request has to wait its turn
        // behind whatever the UI is doing - and the UI is busiest while the roof is
        // moving and somebody is watching it move. Four chances a cycle to be caught
        // behind that is four times the exposure of one, and /api/v1/status exists to be
        // asked exactly this ("Everything in one request, for a dashboard that would
        // otherwise poll four", see apiv1.py).
        bool ok = readURL("/api/v1/status", response, true, &error);

        {
            std::lock_guard<std::mutex> lock(m_snapshotMutex);
            m_snapshot.ok = ok;
            m_snapshot.response = response;
            m_snapshot.error = error;
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
    // Worst case this waits out one in-flight request per endpoint (TRANSFER_TIMEOUT
    // each) - paid
    // once, on disconnect, not on every tick the way it used to be paid on the main thread.
    m_pollThread.join();
}

// Used only for the synchronous priming read in Connect() (see its comment) - pollWorker()
// above does the GET itself from then on. Kept as its own function, rather than folded into
// Connect(), so the fetch-then-apply shape stays in one place.
void OMS::pollStatus() {
    std::string response;
    bool ok = readURL("/api/v1/status", response);
    applyStatus(ok, response, "");
}

// IUFillText() leaves .text as NULL rather than "" when the initial value is empty (see
// indidriver.c), and positionT/faultReasonT/motionPhaseT all start out empty - so read
// them through this, or std::string's operator!= runs strlen() on a null pointer the
// first time a poll compares an incoming value against one.
static const char *textOrEmpty(const IText &t) {
    return t.text ? t.text : "";
}

void OMS::applyStatus(bool ok, const std::string &response, const std::string &error) {
    if ( ! ok ) {
        // A poll that did not come back is not a roof that has faulted. It used to be
        // treated as one, and that had teeth: DOME_ERROR puts ParkSP into IPS_ALERT, Ekos
        // reads that as PARK_ERROR, and SchedulerProcess::checkDomeParkingStatus() answers
        // a PARK_ERROR during a park or an unpark by restarting the operation - so one
        // timed-out poll in the middle of a perfectly good motion had Ekos re-issuing the
        // park it was already carrying out, at a roof that answers a command mid-motion
        // with "The roof is moving; stop it first", up to five times before giving up on
        // the whole procedure. Which is to say the driver turned a slow reply into a
        // failed shutdown, and the roof was moving correctly throughout.
        //
        // So a failure now costs the state nothing until there have been several in a row.
        // Between them the roof keeps the state it last had, which is the best answer
        // available: OMS was told to open or close, and nothing has said it stopped.
        // Silence is still reported, just as silence - and only once, since the poll runs
        // every two seconds and a server that is down is not news 30 times a minute.
        //
        // The switches and the inside readings keep their last state for the same reason,
        // and for the same span. The weather is the one thing judged on its own terms
        // rather than on this counter: OMS says how long its reading stays good for, so
        // weatherUsable() ages it out on that schedule whether or not the polls are
        // getting through, and the publish at the bottom reports it the moment it does.
        m_statusPollFailures++;
        if ( m_statusPollFailures == 1 && ! error.empty() ) {
            // readURL() already logged this itself when it wasn't quiet (the Connect()
            // priming read); when it was quiet (pollWorker(), off the main thread), error
            // carries what it would have logged, for us to say here instead.
            LOGF_WARN("%s", error.c_str());
        }
        if ( m_statusPollFailures < STATUS_POLL_FAILURES_BEFORE_ERROR ) {
            publishWeatherIfChanged();
            return;
        }
        if ( environmentNP.s != IPS_ALERT ) {
            environmentNP.s = IPS_ALERT;
            IDSetNumber(&environmentNP, nullptr);
        }
        if ( m_statusPollFailures == STATUS_POLL_FAILURES_BEFORE_ERROR ) {
            LOGF_ERROR("No answer from OMS for %u status polls; the roof state is unknown.",
                    m_statusPollFailures);
        }
        // Guarded because Dome::setDomeState() applies ParkSP unconditionally on
        // DOME_ERROR, and an outage that lasts is one failed poll every two seconds -
        // the same "only push when something changed" the light and text blocks below
        // are written to.
        if ( getDomeState() != DOME_ERROR ) {
            setDomeState(DOME_ERROR);
        }
        publishWeatherIfChanged();
        return;
    }

    json data;
    try {
        data = json::parse(response);
    } catch ( json::exception &e ) {
        LOGF_ERROR("Status JSON parse error: %s\n%s", e.what(), response.c_str());
        setDomeState(DOME_ERROR);
        publishWeatherIfChanged();
        return;
    } catch (...) {
        LOGF_ERROR("Unknown status JSON parse error\n%s", response.c_str());
        setDomeState(DOME_ERROR);
        publishWeatherIfChanged();
        return;
    }

    if ( m_statusPollFailures > 0 ) {
        if ( m_statusPollFailures >= STATUS_POLL_FAILURES_BEFORE_ERROR ) {
            LOGF_INFO("OMS is answering again after %u missed status polls.",
                    m_statusPollFailures);
        }
        m_statusPollFailures = 0;
    }

    // Each of the four is missing rather than malformed when OMS has nothing to say about
    // it, so each applier is handed what there is and decides for itself - the roof is the
    // only one whose absence is a fault, since /api/v1/status cannot answer at all without
    // it (roofDetail() is not optional in there).
    if ( ! data.contains("roof") || data["roof"].is_null() ) {
        LOGF_ERROR("Status reply carries no roof state\n%s", response.c_str());
        setDomeState(DOME_ERROR);
        publishWeatherIfChanged();
        return;
    }
    // Weather first, though it publishes nothing: applyRoofState() re-runs the weather
    // update itself when the rain interlock changes, and it should judge that on this
    // poll's reading rather than on the one before it.
    applyWeather(data.contains("weather") ? data["weather"] : json());
    applyRoofState(data["roof"]);
    applySwitches(data.contains("switches") ? data["switches"] : json::object());
    applyEnvironment(data.contains("environment") ? data["environment"] : json::object());
    publishWeatherIfChanged();
}

void OMS::applyRoofState(const json &data) {
    std::string state;
    try {
        state = data.at("state").template get<std::string>();
    } catch ( json::exception &e ) {
        LOGF_ERROR("Roof state missing \"state\": %s", e.what());
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

    // OMS's rain interlock. rainInterlockReason() in oms/oms is what actually decides
    // whether the roof may open - the buttons, the API and the automatic close all arrive
    // at it - so mirroring it here is what keeps WEATHER_STATUS reporting OMS's own
    // decision instead of a second opinion. The two used to be able to disagree: the
    // thresholds on the Weather tab are configured separately from roof_rain_threshold,
    // and when they said "clear" while OMS was holding the roof shut, nothing told Ekos to
    // stop - no client watches a dome for parking itself, so a running job would carry on
    // exposing at a closed roof, abort on a failed solve, and come back round to the
    // startup procedure to ask for the roof again.
    //
    // A reply with no usable "rain" block leaves this false rather than latching the alert
    // on. An OMS that does not report the interlock is one this driver can only judge by
    // WEATHER_RAIN_PERCENTAGE, which is what it did before this existed; a reply that does
    // not arrive at all returned above, keeping the last known answer, and takes the
    // weather endpoint on the same server down with it, which markUnsafe()s anyway.
    bool held = false;
    std::string heldReason;
    if ( data.contains("rain") && ! data["rain"].is_null() ) {
        const auto &rain = data["rain"];
        try {
            held = rain.at("holdingClosed").template get<bool>();
        } catch ( json::exception &e ) {
            held = false;
        }
        if ( held && rain.contains("reason") && ! rain["reason"].is_null() ) {
            try {
                heldReason = rain.at("reason").template get<std::string>();
            } catch ( json::exception &e ) {
                heldReason = "";
            }
        }
    }
    if ( held != m_roofHeldShut ) {
        m_roofHeldShut = held;
        if ( held ) {
            LOGF_WARN("OMS is holding the roof shut: %s. Reporting the observatory unsafe.",
                    heldReason.empty() ? "rain interlock" : heldReason.c_str());
        } else {
            LOG_INFO("OMS has released the roof: the rain interlock is no longer holding it shut.");
        }
        // WeatherInterface recomputes the light on its own update timer, which is up to
        // WEATHER_UPDATE seconds away - too late to be worth much to a client deciding
        // whether to keep observing, and far behind the two seconds it took this poll to
        // notice. Re-run the update here, on the transition only, so the light follows the
        // interlock at roof-poll speed rather than weather-poll speed. A no-op on the
        // priming call from Connect(), where isConnected() is still false: WI's own
        // updateProperties() runs the first update a moment later regardless.
        checkWeatherUpdate();
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

void OMS::applySwitches(const json &data) {
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

void OMS::applyEnvironment(const json &data) {
    // "stale" comes from OMS rather than being worked out here, and that is the point of
    // reading it from /api/v1/status: the endpoints this replaced answered 503 once a
    // reading was too old to act on, and the rule behind that (environmentStateMaxAge, and
    // weather_refresh for the weather below) is the server's - it is the one that can see
    // the settings it is derived from. Re-deriving it here would be a second copy of a
    // rule that lives there, free to disagree with it.
    double temperature, humidity;
    bool stale;
    try {
        const auto &reading = data.at("data");
        temperature = reading.at("inside_temperature").template get<double>();
        humidity = reading.at("inside_humidity").template get<double>();
        stale = data.value("stale", false);
    } catch ( json::exception &e ) {
        // Also the "no reading yet" case: getEnvironment() has nothing to report until the
        // board has answered once, and neither field is there.
        LOGF_ERROR("Error accessing environment fields: %s", e.what());
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

bool OMS::weatherUsable() const {
    if ( ! m_weatherHave ) {
        return false;
    }
    if ( m_weatherMaxAge <= 0.0 ) {
        // An OMS that does not report the limit is one this driver cannot age a reading
        // against, so it trusts the "stale" flag alone - which is what it did before the
        // limit was reported at all.
        return true;
    }
    const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_weatherStamp).count();
    return m_weatherAgeAtPoll + elapsed <= m_weatherMaxAge;
}

void OMS::applyWeather(const json &data) {
    // Stored, not published. WeatherInterface owns the publishing and does it on its own
    // timer by calling updateWeather(), which reads what this leaves here.
    //
    // "weather" is null until the station has been scraped once, and carries OMS's own
    // verdict on the age once it has. Taking that verdict rather than working it out here
    // is the point of reading this from /api/v1/status: the rule behind it is
    // weather_refresh, a setting only the server can see, and a second copy of it in this
    // driver would be free to disagree with the one that decides whether /api/v1/weather
    // answers 200 or 503.
    m_weatherHave = false;
    if ( data.is_null() ) {
        return;
    }
    try {
        if ( data.value("stale", true) ) {
            return;
        }
        m_weatherData = data.at("data");
        m_weatherAgeAtPoll = data.value("age", 0.0);
        m_weatherMaxAge = data.value("maxAge", 0.0);
    } catch ( json::exception &e ) {
        LOGF_ERROR("Error accessing weather fields: %s", e.what());
        return;
    }
    m_weatherStamp = std::chrono::steady_clock::now();
    m_weatherHave = true;
}

void OMS::publishWeatherIfChanged() {
    const bool usable = weatherUsable();
    if ( usable == m_weatherReported ) {
        return;
    }
    if ( usable ) {
        LOG_INFO("The weather station is reporting again.");
    } else {
        LOG_WARN("No usable weather reading from OMS; reporting the observatory unsafe.");
    }
    // Same reasoning as the rain interlock's re-run in applyRoofState(): this driver knows
    // within one poll that the reading has aged out, and WeatherInterface would not say so
    // until its own timer came round, up to WEATHER_UPDATE seconds later. A client deciding
    // whether to keep observing should not have to wait out that gap for an answer this
    // already has.
    checkWeatherUpdate();
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

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT) ) {
        curl_easy_cleanup(curl);
        return fail(std::string("Could not set curl connect timeout: ") +
                (strlen(curlErrorBuff) ? curlErrorBuff : "Unknown error"));
    }

    if ( CURLE_OK != curl_easy_setopt(curl, CURLOPT_TIMEOUT, TRANSFER_TIMEOUT) ) {
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
