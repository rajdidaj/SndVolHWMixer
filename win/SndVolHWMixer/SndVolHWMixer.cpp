/*
**------------------------------------------------------------------------------
** SndVolHWMixer:
**
** Finds all streams of the current audio endpoint and sends this information to
** a hardware receiver.
**------------------------------------------------------------------------------
*/
/*
**------------------------------------------------------------------------------
** Includes
**------------------------------------------------------------------------------
*/
#include "pch.h"
#include "rs232.h"
#include <mmdeviceapi.h>
#include <tchar.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <Psapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <conio.h>
#include <list>
#include <thread>

#include "../../common/serialprotocol.h"

/*
**------------------------------------------------------------------------------
** ToDo
**------------------------------------------------------------------------------
*/
//Detect changes in the list of groups and send info accordingly
//Refactor master info enquiry to a single function. Use the Group class?
//hr = endpointVolume->SetMasterVolumeLevelScalar((float)newVolume, NULL); //set master volume

/*
**------------------------------------------------------------------------------
** Constants
**------------------------------------------------------------------------------
*/
const uint8_t corsair[] =
    {
    0x00, 0x00,
    0x00, 0x20,
    0x04, 0x30,
    0x02, 0x18,
    0x22, 0x40,
    0x10, 0x60,
    0x06, 0x70,
    0x03, 0x78,
    0x33, 0x7c,
    0x33, 0x7e,
    0x37, 0xfe,
    0x3f, 0xce,
    0x3c, 0x00,
    0x60, 0x00,
    0x00, 0x00,
    0x00, 0x00
    };


/*
**------------------------------------------------------------------------------
** Type/class definitions
**------------------------------------------------------------------------------
*/
typedef struct
    {    
    IAudioEndpointVolume    *pEndpointVolume;
    IAudioSessionEnumerator *pSessionEnumerator;
    GUID					guid;                   //guid for this device
    WCHAR					deviceName[MAX_PATH * 2]; //Pretty name, the final name sent to the receiver

    float                   prevVolume;             //previous volume value
    BOOL                    prevMute;               //previous mute status
    int                     update;                 //update flag
    }deviceData_t;

typedef struct
    {
    IAudioSessionControl	*pSessionControl;       //SessionControl for this stream
    IAudioSessionControl2	*pSessionControl2;      //SessionControl2 for this stream
    ISimpleAudioVolume		*pVolumeControl;        //AudioVolume for this stream
    GUID					guid;                   //guid for this stream

    PWSTR					displayName;            //Obtained by IAudioSessionControl.GetDisplayName, don't forget to release after use
    WCHAR					prettyName[MAX_PATH * 2]; //Pretty name, the final name sent to the receiver

    float                   prevVolume;             //previous volume value
    BOOL                    prevMute;               //previous mute status
    int                     update;                 //update flag

    }groupData_t;


class Group
    {
    public:
        groupData_t g;
        int duplicate;

        /*
        **----------------------------------------------------------------------
        ** Group constructor:
        **
        ** Initiates everything
        **----------------------------------------------------------------------
        */
        Group(void)
            {
            duplicate = -1;

            g.pSessionControl = NULL;
            g.pSessionControl2 = NULL;
            g.pVolumeControl = NULL;
            g.guid = GUID_NULL;

            g.displayName = NULL;
            g.prettyName[0] = '\0';
            g.prevVolume = NAN;
            g.prevMute = -1;
            g.update = true;
            }

        /*
        **----------------------------------------------------------------------
        ** freeAll method:
        **
        ** Frees all objects associated with this group object (acquired by
        ** getSessionData)
        **----------------------------------------------------------------------
        */
        void freeAll(void)
            {
            if (g.pSessionControl)
                {
                g.pSessionControl->Release();
                }

            if (g.pSessionControl2)
                {
                g.pSessionControl2->Release();
                }

            if (g.pVolumeControl)
                {
                g.pVolumeControl->Release();
                }

            if (g.displayName)
                {
                CoTaskMemFree(g.displayName);
                }
            }

        /*
        **----------------------------------------------------------------------
        ** getSessionData method:
        **
        ** Gets all avaliable objects needed for the information to be displayed
        **----------------------------------------------------------------------
        */
        void getSessionData(IAudioSessionEnumerator *pEnumerator, int streamIndex)
            {
            //Get sessioncontrol
            pEnumerator->GetSession(streamIndex, &g.pSessionControl);

            //Get guid
            g.pSessionControl->GetGroupingParam(&g.guid);

            //Get sessioncontrol2
            g.pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&g.pSessionControl2);

            //Get volume control
            g.pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&g.pVolumeControl);

            //Get displayname
            g.pSessionControl->GetDisplayName(&g.displayName);

            OLECHAR* guidString;
            StringFromCLSID(g.guid, &guidString);
            printf("Stream: %S, displayName: \"%S\"\n",
                guidString,
                g.displayName);
            CoTaskMemFree(guidString);

            }
    };

/*
**------------------------------------------------------------------------------
** Variables
**------------------------------------------------------------------------------
*/
using namespace std;
list <Group> groupList;                     //Group data
deviceData_t deviceData;

int cport_nr = 5;                           //Serial port index
int bdrate = 19200;                         //Baud rate
int cPortOpen = 0;
int cPortRxActive = 0;

/*
**------------------------------------------------------------------------------
** Function prototypes
**------------------------------------------------------------------------------
*/
HRESULT initDevice(deviceData_t *);
int getGroups(IAudioSessionEnumerator *);
void getLabels(void);
void sendChannelInfo(int, float);
void sendMasterInfo(void);

serialProtocol_t * allocProtocolBuf(msgtype_t, size_t);
void freeProtocolBuf(serialProtocol_t **);
void serialRxCb(void);
void getCmds(uint8_t *, uint16_t);
void setGroupVolume(int, int, int);
void setMasterVolume(int, int);

/*
**------------------------------------------------------------------------------
** Macros
**------------------------------------------------------------------------------
*/
#define serialSendBuffer(_dPtr, _dCount)	RS232_SendBuf(cport_nr, _dPtr, _dCount)

/*
**------------------------------------------------------------------------------
** Callback functions
**------------------------------------------------------------------------------
*/

/*
**------------------------------------------------------------------------------
** EnumWindowsProcMy:
**
** Enumerates windows by PID
**------------------------------------------------------------------------------
*/
HWND g_HWND = NULL;
BOOL CALLBACK EnumWindowsProcMy(HWND hwnd, LPARAM lParam)
    {
    DWORD lpdwProcessId;
    g_HWND = NULL;
    GetWindowThreadProcessId(hwnd, &lpdwProcessId);
    if (lpdwProcessId == lParam)
        {
        //Find the top level
        while (1)
            {
            if (GetParent(hwnd))
                {
                hwnd = GetParent(hwnd);
                }
            else
                {
                break;
                }
            }
        g_HWND = hwnd;
        return FALSE;
        }
    return TRUE;
    }

/*
**------------------------------------------------------------------------------
** serialRxCb:
**
** Polls the serial port for data
**------------------------------------------------------------------------------
*/
thread serialRxThread(serialRxCb);
void serialRxCb(void)
    {
    RS232_flushRXTX(cport_nr);

    uint8_t ch;
    static int msgLen = 0;
    static msgState_t msgState = MSGSTATE_IDLE;
    static uint8_t *msgPtr;

    while (true)
        {
        if (cPortOpen && cPortRxActive)
            {
            if (RS232_PollComport(cport_nr, &ch, 1))
                {             
                
                if (msgLen >= MAX_RXTX_BUFFER_LENGTH)
                    {
                    //Not enough room, stop
                    msgState = MSGSTATE_IDLE;
                    msgLen = 0;
                    }

                switch (ch)
                    {
                    case STX:
                        //Message starting
                        msgState = MSGSTATE_ACTIVE;
                        msgLen = 0;
                        msgPtr = rxBuffer;
                        break;

                    case ETX:
                        //Message ending
                        if (msgState == MSGSTATE_ACTIVE)
                            {
                            if (ctrlChecksum(rxBuffer))
                                {
                                getCmds(rxBuffer + 2, msgLen - 2);
                                }
                            msgState = MSGSTATE_IDLE;
                            }
                        break;

                    case DLE:
                        //Stuff byte
                        if (MSGSTATE_ACTIVE)
                            {
                            msgState = MSGSTATE_DLE;
                            }
                        break;

                    default:
                        //Copy data
                        if (msgState == MSGSTATE_DLE)
                            {
                            *msgPtr++ = ch ^ DLE;
                            msgState = MSGSTATE_ACTIVE;
                            msgLen++;
                            }
                        else if (msgState == MSGSTATE_ACTIVE)
                            {
                            *msgPtr++ = ch;
                            msgLen++;
                            }
                        break;
                    }
                }
            }
        std::this_thread::yield();
        }
    }

/*
**------------------------------------------------------------------------------
** Main function
**------------------------------------------------------------------------------
*/
int _tmain(int argc, _TCHAR* argv[])
    {
    HRESULT hr;

    double newVolume;
    int groupCount = 0;

    int i = 0;
    char mode[] = { '8','N','1',0 };
    char str[2][512];

    if (RS232_OpenComport(cport_nr, bdrate, mode))
        {
        printf("Can not open serial port\n");
        }
    else
        {
        printf("Serial port %d opened\n", cport_nr + 1);
        cPortOpen = 1;

        Sleep(2000); //Wait for the arduino to reset
        }

    hr = initDevice(&deviceData);
      

    //Enter main loop
    while (!_kbhit())
        {
        // Master volume
        sendMasterInfo();
                
        //Get data and list info about streams		
        groupCount = getGroups(deviceData.pSessionEnumerator);

        getLabels();


        int channelIx = 0;
        for (int i = 0; i < groupCount; i++)
            {
            sendChannelInfo(i, -1);
            }

        Sleep(2000);

        if (cPortOpen)
            {
            cPortRxActive = 1;
            }
        }

    //Let the controls remain active for another keypress
    _getch();
    printf("Still running! Press any key to exit...\n");
    Sleep(1000);    
    while (!_kbhit())
        {
        // Master volume
        sendMasterInfo();
        Sleep(2000);
        }

    //Clean up
    deviceData.pEndpointVolume->Release();
    deviceData.pSessionEnumerator->Release();        

    RS232_CloseComport(cport_nr);

    CoUninitialize();
    return 0;
    }


/*
**------------------------------------------------------------------------------
** initDevice:
**
** Initializes the audio endpoint
**------------------------------------------------------------------------------
*/
HRESULT initDevice(deviceData_t *dev)
    {
    HRESULT hr;

    /*
    **--------------------------------------------------------------------------
    ** Get the device instance
    **--------------------------------------------------------------------------
    */
    CoInitialize(NULL);
    IMMDeviceEnumerator *deviceEnumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (LPVOID *)&deviceEnumerator);
    IMMDevice *defaultDevice = NULL;
    hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    deviceEnumerator->Release(); //Enumerator is no longer needed
    deviceEnumerator = NULL;

    /*
    **--------------------------------------------------------------------------
    ** Get the volume control
    **--------------------------------------------------------------------------
    */
    dev->pEndpointVolume = NULL;
    hr = defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (LPVOID *)&dev->pEndpointVolume);

    /*
    **--------------------------------------------------------------------------
    ** Get the device name
    **--------------------------------------------------------------------------
    */
    LPWSTR devID;
    IPropertyStore *pProps = NULL;
    hr = defaultDevice->GetId(&devID);

    hr = defaultDevice->OpenPropertyStore(
        STGM_READ, &pProps);

    PROPVARIANT varName;
    // Initialize container for property value.
    PropVariantInit(&varName);

    // Get the endpoint's friendly-name property.
    hr = pProps->GetValue(
        PKEY_Device_FriendlyName, &varName);
    wcscpy_s(dev->deviceName, varName.pwszVal);

    /*
    **--------------------------------------------------------------------------
    ** Get the session manager, to get streams later
    **--------------------------------------------------------------------------
    */
    IAudioSessionManager2 *pSessionManager;
    pSessionManager = NULL;
    hr = defaultDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, NULL, (void**)&pSessionManager);

    dev->guid = GUID_NULL;

    //Release the device, we have what we need
    defaultDevice->Release();
    defaultDevice = NULL;

    //get the sessionenumerator
    dev->pSessionEnumerator = NULL;
    hr = pSessionManager->GetSessionEnumerator(&dev->pSessionEnumerator);

    pSessionManager->Release();

    dev->prevVolume = NAN;
    dev->prevMute = -1;

    return S_OK;
    }

/*
**------------------------------------------------------------------------------
** getGroups:
**
** Gets all streams and groups them up per guid
**------------------------------------------------------------------------------
*/
int getGroups(IAudioSessionEnumerator *pEnumerator)
    {
    HRESULT hr;
    int currentStreamCount = 0;

    hr = pEnumerator->GetCount(&currentStreamCount);

    printf("Current number of streams %d\n", currentStreamCount);

    //Get guids of all streams    
    for (int i = 0; i < currentStreamCount; i++)
        {
        Group group;

        //Get guid, sessioncontrol, sessioncontrol2, etc...
        group.getSessionData(pEnumerator, i);

        //Add to the list
        groupList.push_back(group);
        }

    //Find stream groups
    list <Group>::iterator grp;
    list <Group>::iterator cmp;
    int dupe = 0;

    //Iterate through the list of streams
    for (grp = groupList.begin(); grp != groupList.end(); grp++, dupe++)
        {
        //Compare grp to all other list items, except null guids
        for (cmp = groupList.begin(); cmp != groupList.end(); cmp++)
            {
            if (((*cmp).g.guid != GUID_NULL) && (cmp != grp) && IsEqualGUID((*grp).g.guid, (*cmp).g.guid))
                {
                //Mark the duplicate
                (*cmp).duplicate = dupe;
                }
            }
        }

    //Get rid of the dupes
    for (grp = groupList.begin(); grp != groupList.end(); grp++)
        {
        dupe = (*grp).duplicate;
        if (dupe >= 0)
            {
            cmp = grp;

            for (cmp++; cmp != groupList.end(); cmp++)
                {
                if ((*cmp).duplicate == dupe)
                    {
                    //Free memory occupied by the duplicate list item
                    (*cmp).freeAll();

                    //Remove the duplicate list item
                    groupList.erase(cmp);

                    //cmp is now invalid, so restart at grp+1
                    cmp = grp;
                    cmp++;
                    }
                }
            }
        }

    printf("Current number of groups %d\n", groupList.size());

    return groupList.size();
    }

/*
**------------------------------------------------------------------------------
** getLabels:
**
** Gets the best available label for the all groups
**------------------------------------------------------------------------------
*/
void getLabels(void)
    {
    HRESULT hr;
    int label;
    list <Group>::iterator grp;

    //Find the groups that fo not have a good label already
    for (grp = groupList.begin(); grp != groupList.end(); grp++)
        {
        label = 0;
        if (!wcscmp((*grp).g.displayName, L""))
            {
            OLECHAR* guidString;
            StringFromCLSID((*grp).g.guid, &guidString);

            printf("Group %S has no label", guidString);

            CoTaskMemFree(guidString);
            }
        else
            {

            OLECHAR* guidString;
            StringFromCLSID((*grp).g.guid, &guidString);

            printf("Group %S has a label: \"%S\"", guidString, (*grp).g.displayName);

            CoTaskMemFree(guidString);

            wcscpy_s((*grp).g.prettyName, _countof((*grp).g.prettyName), (*grp).g.displayName);
            label = 1;

            if (wcsstr((*grp).g.prettyName, L"AudioSrv.Dll") != NULL)
                {
                wsprintfW((*grp).g.prettyName, L"System Sounds");
                }
            }

        //Get process id
        DWORD pid;
        hr = (*grp).g.pSessionControl2->GetProcessId(&pid);

        //Get imagename
        HANDLE Handle = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            FALSE,
            pid
        );
        if (Handle)
            {
            WCHAR Buffer[MAX_PATH];
            if (GetProcessImageFileNameW(Handle, Buffer, _countof(Buffer)))
                {

                //WCHAR label[MAX_PATH];
                //hr = GetWindowText((HWND)Handle, label, _countof(label));
                // At this point, buffer contains the full path to the executable
                //printf("%S, ", Buffer);

                wchar_t *res_p;
                TCHAR fullpath[MAX_PATH];
                res_p = _wfullpath(fullpath, Buffer, _countof(fullpath));

                //printf("%S, ", fullpath);

                TCHAR drive[3];
                TCHAR dir[256];
                TCHAR fname[256];
                TCHAR ext[256];
                _tsplitpath_s(
                    fullpath,
                    drive,
                    _countof(drive),
                    dir,
                    _countof(dir),
                    fname,
                    _countof(fname),
                    ext,
                    _countof(ext));
                printf(", executable name: \"%S\"", fname);

                if (wcslen(fname) && !label)
                    {
                    wcscpy_s((*grp).g.prettyName, _countof((*grp).g.prettyName), fname);
                    memset(fname, 0, sizeof(fname));
                    }

                if (!EnumWindows(EnumWindowsProcMy, pid))
                    {
                    if (GetWindowText(g_HWND, Buffer, _countof(Buffer)))
                        {
                        printf(", window text: \"%S\"", Buffer);
                        if (wcslen(Buffer))
                            {
                            wcscpy_s((*grp).g.prettyName, _countof((*grp).g.prettyName), Buffer);
                            memset(Buffer, 0, sizeof(Buffer));
                            }
                        }
                    }
                }
            else
                {
                // You better call GetLastError() here
                }
            CloseHandle(Handle);
            }

        (*grp).g.update = true;

        printf(", prettyName: \"%S\"", (*grp).g.prettyName);
        printf("\n");
        }
    }

/*
**------------------------------------------------------------------------------
** sendChannelInfo:
**
** Sends the specified channel information to the receiver
**------------------------------------------------------------------------------
*/
void sendChannelInfo(int ch, float masterVolume)
    {
    size_t numconv;
    char charName[64+1];
    uint8_t vol;
    BOOL mute;
    float fvol;
    serialProtocol_t *msg;
    int chnum = ch;

    list <Group>::iterator i;
    for (i = groupList.begin(); ch; i++, ch--)
        {
        //do nothing
        }

    (*i).g.pVolumeControl->GetMasterVolume(&fvol);
    if (fvol != (*i).g.prevVolume)
        {
        (*i).g.update = true;
        (*i).g.prevVolume = fvol;
        }

    (*i).g.pVolumeControl->GetMute(&mute);
    if (mute != (*i).g.prevMute)
        {
        (*i).g.update = true;
        (*i).g.prevMute = mute;
        }

    if ((*i).g.update)
        {
        (*i).g.update = false;

        vol = fvol * 100;
        if (masterVolume >= 0.0)
            {
            vol *= masterVolume;
            }

        msg = allocProtocolBuf(MSGTYPE_SET_CHANNEL_VOL_PREC, sizeof(struct msg_set_channel_vol_prec));
        msg->msg_set_channel_vol_prec.channel = chnum;
        msg->msg_set_channel_vol_prec.volVal = vol;
        msg->msg_set_channel_vol_prec.muteStatus = mute;
        protocolTxData(msg, sizeof(struct msg_set_channel_vol_prec));
        freeProtocolBuf(&msg);

        wcstombs_s(&numconv, charName, (*i).g.prettyName, _countof(charName)-1);
        charName[sizeof(charName) - 1] = 0;

        int len = sizeof(struct msg_set_channel_label) + strlen(charName) + 1;
        msg = allocProtocolBuf(MSGTYPE_SET_CHANNEL_LABEL, len);
        msg->msg_set_channel_label.channel = chnum;
        memcpy_s(msg->msg_set_channel_label.str, strlen(charName) + 1, charName, strlen(charName) + 1);
        msg->msg_set_channel_label.strLen = strlen(charName);

        protocolTxData(msg, len);
        freeProtocolBuf(&msg);
        }
    }

/*
**------------------------------------------------------------------------------
** sendMasterInfo:
**
** Sends the audio endpoint master volume to the receiver
**------------------------------------------------------------------------------
*/
void sendMasterInfo(void)
    {
    char charName[64+1];
    size_t numconv;
    
    BOOL mute;
    float fvol;    

    deviceData.pEndpointVolume->GetMasterVolumeLevelScalar(&fvol);    
    deviceData.pEndpointVolume->GetMute(&mute);
    if ((fvol != deviceData.prevVolume) || (mute != deviceData.prevMute))
        {
        printf("Current volume as a scalar is: %f\n", fvol);
        deviceData.update = true;
        deviceData.prevVolume = fvol;
        deviceData.prevMute = mute;
        }

    if (deviceData.update)
        {
        deviceData.update = false;

        serialProtocol_t *msg = allocProtocolBuf(MSGTYPE_SET_MASTER_VOL_PREC, sizeof(struct msg_set_master_vol_prec));
        msg->msg_set_master_vol_prec.volVal = fvol * 100;
        msg->msg_set_master_vol_prec.muteStatus = mute;
        protocolTxData(msg, sizeof(struct msg_set_master_vol_prec));
        freeProtocolBuf(&msg);

        int size = sizeof(struct msg_set_master_icon) + sizeof(corsair);
        msg = allocProtocolBuf(MSGTYPE_SET_MASTER_ICON, size);
        memcpy(msg->msg_set_master_icon.icon, corsair, sizeof(corsair));
        protocolTxData(msg, size);
        freeProtocolBuf(&msg);

        wcstombs_s(&numconv, charName, deviceData.deviceName, _countof(charName) - 1);
        charName[sizeof(charName) - 1] = 0;

        int len = sizeof(struct msg_set_master_label) + strlen(charName) + 1;
        msg = allocProtocolBuf(MSGTYPE_SET_MASTER_LABEL, len);
        memcpy_s(msg->msg_set_master_label.str, strlen(charName) + 1, charName, strlen(charName) + 1);
        msg->msg_set_master_label.strLen = strlen(charName);

        protocolTxData(msg, len);
        freeProtocolBuf(&msg);
        }
    }

/*
**------------------------------------------------------------------------------
** allocProtocolBuf:
**
** Allocates memory for the specified message
**------------------------------------------------------------------------------
*/
serialProtocol_t * allocProtocolBuf(msgtype_t msgType, size_t bytes)
    {
    serialProtocol_t *blockPtr = 0;

    blockPtr = (serialProtocol_t*)malloc(bytes);

    blockPtr->msgType = msgType;

    return blockPtr;
    }

/*
**------------------------------------------------------------------------------
** freeProtocolBuf:
**
** Frees up a previously allocated buffer
**------------------------------------------------------------------------------
*/
void freeProtocolBuf(serialProtocol_t **blockBtr)
    {
    free(*blockBtr);
    *blockBtr = NULL;
    }

/*
**------------------------------------------------------------------------------
** protocolTxData:
**
** Pads, checksums and transmits a message to the receiver
**------------------------------------------------------------------------------
*/
#ifdef serialSendBuffer
static void protocolTxData(void *dataPtr, int dataLength)
    {
    int numData = 0;
    int i;
    int totalData = 0;
    uint16_t checksum;
    uint8_t *txBufPtr;
    uint8_t *workBufPtr;

    //Fill the work buffer
    workBufPtr = msgBuffer;

    //Start the message by adding the length
    *((uint16_t*)workBufPtr) = dataLength;
    numData += 2;
    workBufPtr += 2;	//Jump to the first data byte

    //Copy the data
    memcpy_s(workBufPtr, sizeof(msgBuffer) - numData, dataPtr, dataLength);
    numData += dataLength;
    workBufPtr += dataLength;	//Jump to the checksum

    //calculate the checksum
    checksum = 0;
    for (i = 0; i < numData; i++)
        {
        checksum ^= msgBuffer[i];
        }

    //Add the checksum
    *((uint16_t*)workBufPtr) = checksum;
    numData += 2;


    //Start the TX buffer with STX
    txBufPtr = txBuffer;
    *txBufPtr++ = STX;
    totalData++;

    //Copy data and check for reserved symbols and stuff if needed
    for (i = 0; i < numData; i++)
        {
        switch (msgBuffer[i])
            {
            case STX:
            case ETX:
            case DLE:
                //Reserved data, add a stuff byte and stuff the data
                *txBufPtr++ = DLE;
                totalData++;
                *txBufPtr++ = msgBuffer[i] ^ 0x10;
                break;

            default:
                *txBufPtr++ = msgBuffer[i];
                break;
            }
        totalData++;
        }

    //Finish off by adding the ETX
    *txBufPtr++ = ETX;
    totalData++;

    serialSendBuffer(txBuffer, totalData);
    }
#endif


/*
**------------------------------------------------------------------------------
** getCmds:
**
** Acts on the commands received on serial
**------------------------------------------------------------------------------
*/
void getCmds(uint8_t *pMsgBuf, uint16_t dataLen)
    {
    serialProtocol_t *msgPtr = (serialProtocol_t*)pMsgBuf;
    int channel;

    switch (msgPtr->msgType)
        {
        case MSGTYPE_SET_MASTER_VOL_PREC:
            /*if ((msgPtr->msg_set_master_vol_prec.volVal >= MINVOLVAL) && (msgPtr->msg_set_master_vol_prec.volVal <= MAXVOLVAL))
                {

                }*/

            setMasterVolume(
                msgPtr->msg_set_master_vol_prec.volVal,
                msgPtr->msg_set_master_vol_prec.muteStatus);
            break;
                  
        case MSGTYPE_SET_CHANNEL_VOL_PREC:
            /*if (msgPtr->msg_set_channel_vol_prec.channel < NUM_CHANNELS)
                {
                channel = msgPtr->msg_set_channel_vol_prec.channel - 1;
                if ((msgPtr->msg_set_channel_vol_prec.volVal >= MINVOLVAL) && (msgPtr->msg_set_channel_vol_prec.volVal <= MAXVOLVAL))
                    {

                    }
                }*/
            
            setGroupVolume(
                msgPtr->msg_set_channel_vol_prec.channel,
                msgPtr->msg_set_channel_vol_prec.volVal,
                msgPtr->msg_set_channel_vol_prec.muteStatus);
            break;                
        
        default:
            break;
        }
    }

/*
**------------------------------------------------------------------------------
** ctrlChecksum:
**
** Checks the received checksum
**------------------------------------------------------------------------------
*/
bool ctrlChecksum(uint8_t *pMsgBuf)
    {
    uint16_t len = *(uint16_t*)pMsgBuf;
    uint16_t check = *(uint16_t*)&pMsgBuf[len + 2];
    uint16_t sum = 0;
    unsigned int i;

    for (i = 0; i < len + 2; i++)
        {
        sum ^= pMsgBuf[i];
        }

    if (check == sum)
        {
        return 1;
        }
    else
        {
        return 0;
        }
    }

/*
**------------------------------------------------------------------------------
** setGroupVolume:
**
** Sets the referenced groups volume
**------------------------------------------------------------------------------
*/
void setGroupVolume(int ch, int percent, int mute)
    {
    float fvol = percent / 100.0;
    printf("New group %d vol: %d\n", ch, percent);

    list <Group>::iterator i;
    for (i = groupList.begin(); ch; i++, ch--)
        {
        //do nothing
        }

    (*i).g.pVolumeControl->SetMasterVolume(fvol, &(*i).g.guid);
    (*i).g.prevVolume = fvol;

    (*i).g.pVolumeControl->SetMute(mute, &(*i).g.guid);
    (*i).g.prevMute = mute;

    }


/*
**------------------------------------------------------------------------------
** setMasterVolume:
**
** Sets master volume
**------------------------------------------------------------------------------
*/
void setMasterVolume(int percent, int mute)
    {
    float fvol = percent / 100.0;
    printf("New master vol: %d\n", percent, mute);

    deviceData.pEndpointVolume->SetMasterVolumeLevelScalar(fvol, &deviceData.guid);
    deviceData.prevVolume = fvol;

    deviceData.pEndpointVolume->SetMute(mute, &deviceData.guid);
    deviceData.prevMute = mute;

    }