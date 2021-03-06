/*

    LutronBridge

    Creates a simple pass through bridge into the Lutron RadioRA2 gateway

    This app will connect through TELNET to your Lutron bridge (set the IP
    address under lutronIP[])

    The app stays connected and passes any string sent through "sendCommand"
    the luton Telnet session.

    For examnple, #OUTPUT,5,1,100.00 will set deviceID 5 to ON

    Created this as a hack to allow for IFTTT connectivity.  Use the Particle
    connector on IFTTT / DO Button / etc. and you can control any LUTRON device

    IMPORTANT NOTE: Requires Particle PHOTON firmware 4.7 which has fixes in TCPClient

    Meant as a temporary workaround until we get the Lutron Smart Brige
    (or whatever they call it)...

    /rowantrollope
*/
// this thread watches for the specific events we care about -
// dimmers changing levels, and stores the new level

#include "LutronBridge.h"
#include <mutex>

std::mutex tcpClient_mutex;

LUTRON_DEVICE::LUTRON_DEVICE(int nID, float fCurLevel, float fOnLevel)
{
    id=nID;
    currentLevel=fCurLevel;
    onLevel=fOnLevel;
}

LutronBridge::LutronBridge()
{
    changeCB = NULL;
    bPublishAll = true;
    telnetListenerThread = NULL;
    m_bMonitor = false;
}

os_thread_return_t listener(void* param)
{
    return ((LutronBridge *)param)->telnetListener(param);
}

bool LutronBridge::connect(byte lutronIP[])
{
    Serial.println("lutronConnect - Connecting...");
    tcpClient_mutex.lock();
    client.connect(lutronIP, TELNET_PORT);
    tcpClient_mutex.unlock();

    if (client.connected())
    {
        Serial.println("lutronConnect - Connected");

        tcpClient_mutex.lock();
        client.println("lutron");
        client.println("integration");
        tcpClient_mutex.unlock();

        // wait a couple seconds for the telnet server to catch up
        delay(1000);

        // Setup listener thread to respond to light change events
        telnetListenerThread = new Thread("telnetListener", listener, this);

        delay(1000);

        // Get the current states of the lights we care about
        // the enums all device ID's from 0 to 90.
        if(m_bMonitor)
          initDimmerLevels(90);

        return true;
    }
    else
    {
        Serial.println("lutronConnect - Connection failed");

        tcpClient_mutex.lock();
        client.stop();
        tcpClient_mutex.unlock();

        delay(1000);
        return false;
    }
}
void LutronBridge::disconnect()
{
    tcpClient_mutex.lock();
    client.stop();
    tcpClient_mutex.unlock();

    if(telnetListenerThread)
    {
        delete telnetListenerThread;
        telnetListenerThread = NULL;
    }
    return;
}

os_thread_return_t LutronBridge::telnetListener(void* param)
{

    // Extract the current level and save it - format ~OUTPUT,DEVICEID,LEVEL (FLOAT)
    String sVal = LUTRON_RETURN;

    while(true)
    {
        String sResult;
        bool bInput=false;

        tcpClient_mutex.lock();
        while (client.available())
        {
            char c = client.read();
            sResult += c;
            bInput = true;
        }
        tcpClient_mutex.unlock();

        if(bInput) // Work to do?
        {
            Serial.println("LutronBridge::telnetListener - RECEIVED: ");
            Serial.println(sResult);

            // HANDLE INPUT
            int start=0;
            int end=0;
            do
            {
                // LOOK For ~OUTPUT,NN,NN,NN
                start = sResult.indexOf(sVal, start);

                if(start == -1)
                    break;

                // FOUND ~OUTPUT // ~OUTPUT,NN,NN,NN
                // first TOKEN is device ID
                start += sVal.length();

                end = sResult.indexOf(',', start);
                int nDevice = sResult.substring(start, end).toInt();

                // next TOKEN is the command, 1, advance past it
                start = end + 1;
                end = sResult.indexOf(',', start);

                int nCommand = sResult.substring(start, end).toInt();

                // next TOKEN is the level
                start = end + 1;
                end = sResult.indexOf('\r\n', start);

                float fLevel = sResult.substring(start, end).toFloat();

                // only command we care about for lights is "OUTPUT CHANGED"
                if(nCommand == 1)
                {
                    String sDebug = String::format("LutronBridge::telnetListener() - PROCESSING - DEVICE=%i, CMD=%i, LEVEL=%.2f", nDevice, nCommand, fLevel);
                    Serial.println(sDebug);

                    // Publish light changed events
                    if(bPublishAll)
                    {
                        String sEventData = String::format("device=%i&level=%.0f", nDevice, fLevel);
                        Particle.publish("lutron/device/changed", sEventData);
                    }
                    // we now STORE all light events in our map...
                    LUTRON_DEVICE device(nDevice, fLevel, DEFAULT_ON_LEVEL);

                    deviceMap[nDevice] = device;

                    if(changeCB)
                        changeCB(nDevice);

                }
                else
                {
                    String sDebug = String::format("LutronBridge::telnetListener() - IGNORING - DEVICE=%i, CMD=%i", nDevice, nCommand);
                    Serial.println(sDebug);
                }

            } while (start != -1);
        }
        // throttle read as we seem to be overwhelming something here
        delay(250);
    }

}

// this function requests the level for every deviceID between 0 and nMax,
// the telnetListener will catch the response and initialize the object
// in our object map
// This is less than ideal, and really we should only call getDimmer for the devices
// we know exist, but there doesn't seem to be a way to request the list of
// valid devices...
int LutronBridge::initDimmerLevels(int nMax)
{
    /*
    for(DEVICE_MAP::iterator it = deviceMap.begin();
        it != deviceMap.end();
        it++)
    {
        _getDimmer(it->first);
        delay(250);
    }
    */

    for (int i=0;i<nMax;i++)
    {
        _getDimmer(i);
        delay(50);
    }
    return 1;
}
void LutronBridge::addDevice(int nDeviceID, LUTRON_DEVICE device)
{
    deviceMap[nDeviceID] = device;
}
void LutronBridge::updateDevice(int nDeviceID, LUTRON_DEVICE device)
{
    deviceMap[nDeviceID] = device;
}
LUTRON_DEVICE LutronBridge::getDevice(int nDeviceID)
{
    return deviceMap[nDeviceID];
}
bool LutronBridge::deviceExists(int nDeviceID)
{
    return deviceMap.find(nDeviceID) != deviceMap.end();
}

// Internet Published Function ... REQUIRES FORMAT: NN,MM where NN == DeviceID, MM == DimLevel 0-100
int LutronBridge::setDimmer(String sDimmer)
{
    Serial.println("LutronBridge::setDimmer - Received Command: " + sDimmer);

    int nPos = sDimmer.indexOf(',');

    if(nPos == -1)
        return -1;

    int nDimmerID = sDimmer.substring(0,nPos).toInt();
    float fLevel = sDimmer.substring(nPos+1).toFloat();

    return _setDimmer(nDimmerID, fLevel);
}

int LutronBridge::_setDimmer(int nDimmer, float fLevel)
{
    String sCommand = String::format(LUTRON_WRITE, nDimmer, fLevel);
    return sendCommand(sCommand);
}

// Get level of dimmer, sDimmer is the Device ID (number), returns the dim level (0-100)
int LutronBridge::getDimmer(String sDimmer)
{
    return _getDimmer(sDimmer.toInt());
}
int LutronBridge::_getDimmer(int nDimmer)
{
    String sCommand = String::format(LUTRON_READ, nDimmer);

    // Ask lutron the dimmer level
    return sendCommand(sCommand);
}

// returns a string with all dimmer states
String LutronBridge::getAllDimmers()
{
    String states;

    for(DEVICE_MAP::iterator it = deviceMap.begin();
        it != deviceMap.end();
        it++)
    {
        states += String::format("D=%i&L=%.0f\r\n", it->first, it->second.currentLevel);
    }

    Particle.publish("lutron/alldevices/state", states);
    Serial.println("LutronBridge::getAllDimmers()");
    Serial.println(states);

    return states;
}

int LutronBridge::setAllDimmers(String sCommand)
{
    String  sDeviceToken="D=";
    String  sLevelToken="L=";
    String  sResult = sCommand;

    // HANDLE INPUT
    int start=0;
    int end=0;
    do
    {
        // LOOK For D= (NN&L=NN)
        start = sResult.indexOf(sDeviceToken, start);

        if(start == -1)
            break;

        // FOUND D=
        // first TOKEN is device ID
        start += sDeviceToken.length();

        end = sResult.indexOf('&', start);
        String sDevice = sResult.substring(start, end);
        int nDevice = sDevice.toInt();

        // LOOK for L=
        start = sResult.indexOf(sLevelToken, start);

        if(start == -1)
            break;

        start += sLevelToken.length();
        end = sResult.indexOf('\r\n', start);

        String sLevel = sResult.substring(start, end);
        float fLevel = sLevel.toFloat();

        start = end + 2;

        _setDimmer(nDevice, fLevel);

    } while (start != -1);

    return 1;
}
// Internet Published function
int LutronBridge::sendCommand(String sCommand)
{
    // send command to lutron
    if(client.connected())
    {
        Serial.println("LutronBridge::sendCommand - " + sCommand);
        tcpClient_mutex.lock();
        client.println(sCommand);
        tcpClient_mutex.unlock();
        //Serial.println("LutronBridge::sendCommand RETURNED");
        return 1;
    }
    else
    {
        Serial.println("LutronBridge::sendCommand - CLIENT DISCONNECTED");
        return -1;
    }
}
