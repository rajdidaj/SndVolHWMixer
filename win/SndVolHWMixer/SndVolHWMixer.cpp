
#include "pch.h"
#include "rs232.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <windows.h>
#include <mmdeviceapi.h>
#include <tchar.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <Psapi.h>
#include <WinUser.h>

//Constants
#define MAX_GROUPS 128
#define MAX_STREAMS 128

//Types
typedef struct
{
	int streamIndex;
	IAudioSessionControl	*pSessionControl;
	IAudioSessionControl2	*pSessionControl2;
	ISimpleAudioVolume		*pVolumeControl;
	GUID					guid;

	PWSTR					displayName;
	WCHAR					prettyName[MAX_PATH];

}groupData_t;

//Globals
groupData_t groups[MAX_GROUPS];

int cport_nr = 3;        /* /dev/ttyS0 (COM1 on windows) */
int bdrate = 9600;       /* 9600 baud */

#define serialSendBuffer(_dPtr, _dCount)	RS232_SendBuf(cport_nr, _dPtr, _dCount)

// --------------------- Serial Protocol start ----------------------
#define STX				2
#define ETX				3
#define DLE				0x10

typedef struct
{
	uint8_t startToken;		//Always STX
	uint16_t dataLength;	//Always before stuffing
}protocolHdr_t;

typedef struct
{
	uint16_t checksum;		//Always calculated before stuffing
	uint8_t endToken;		//Always ETX
}protocolFtr_t;

const int MAX_MSG_LENGTH = 256;	//Any old number, deemed enough, would do
const int MAX_RXTX_BUFFER_LENGTH = MAX_MSG_LENGTH*2 + 2; //Needs to facilitate the start and stop tokens and the worst case stuffing situation

uint8_t msgBuffer[MAX_MSG_LENGTH] = { 0 };
uint8_t txBuffer[MAX_RXTX_BUFFER_LENGTH] = { 0 };

#ifdef serialSendBuffer
void protocolTxData(void *, int);	//Use this to send a known number of data bytes, set up the send macro to use
#endif

// ------------------------- Data layer ---------------------------
typedef uint8_t msgtype_t;
const msgtype_t MSGTYPE_SET_MASTER_VOL_PREC = 0;
const msgtype_t MSGTYPE_SET_MASTER_LABEL = 1;
const msgtype_t MSGTYPE_SET_CHANNEL_VOL_PREC = 2;
const msgtype_t MSGTYPE_SET_CHANNEL_LABEL = 3;

typedef struct
{
	msgtype_t msgType;
	uint8_t volVal;
}serialProtocol_set_master_vol_prec_t;

typedef struct
{
	msgtype_t msgType;
	uint8_t strLen;
	uint8_t str[32];
}serialProtocol_set_master_label_t;

typedef struct
{
	msgtype_t msgType;
	uint8_t volVal;
}serialProtocol_set_channel_vol_prec_t;

typedef struct
{
	msgtype_t msgType;
	uint8_t strLen;
	uint8_t str[32];
}serialProtocol_set_channel_label_t;

typedef union 
{
	msgtype_t msgType;
	serialProtocol_set_master_vol_prec_t	msg_set_master_vol_prec;
	serialProtocol_set_master_label_t		msg_set_master_label;
	serialProtocol_set_channel_vol_prec_t	msg_set_channel_vol_prec;
	serialProtocol_set_channel_label_t		msg_set_channel_label;
}serialProtocol_t;

serialProtocol_t * allocProtocolBuf(msgtype_t, size_t);
void freeProtocolBuf(serialProtocol_t *);


// --------------------- Serial Protocol end ----------------------






//Functions 
int getGroups(IAudioSessionEnumerator *, groupData_t *);
void getLabels(IAudioSessionEnumerator *, groupData_t *, int);
void sendChannelInfo(int);
void sendMasterInfo(float);

HWND g_HWND = NULL;
BOOL CALLBACK EnumWindowsProcMy(HWND hwnd, LPARAM lParam)
{
	DWORD lpdwProcessId;
	GetWindowThreadProcessId(hwnd, &lpdwProcessId);
	if (lpdwProcessId == lParam)
	{
		g_HWND = hwnd;
		return FALSE;
	}
	return TRUE;
}

void Usage()
{
	printf("Usage: \n");
	printf(" SetVolume [Reports the current volume]\n");
	printf(" SetVolume -d <new volume in decibels> [Sets the current default render device volume to the new volume]\n");
	printf(" SetVolume -f <new volume as an amplitude scalar> [Sets the current default render device volume to the new volume]\n");

}

int _tmain(int argc, _TCHAR* argv[])
{
	HRESULT hr;
	bool decibels = false;
	bool scalar = false;
	double newVolume;
	int groupCount = 0;


	if (argc != 3 && argc != 1)
	{
		Usage();
		return -1;
	}
	if (argc == 3)
	{
		if (argv[1][0] == '-')
		{
			if (argv[1][1] == 'f')
			{
				scalar = true;
			}
			else if (argv[1][1] == 'd')
			{
				decibels = true;
			}
		}
		else
		{
			Usage();
			return -1;
		}

		newVolume = _tstof(argv[2]);
	}


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
		char testBuf[64];
		sprintf_s(testBuf, "%c", 0x03);	//Clear display
		RS232_SendBuf(cport_nr, (unsigned char*)testBuf, strlen(testBuf));
	}

	// -------------------------
	CoInitialize(NULL);
	IMMDeviceEnumerator *deviceEnumerator = NULL;
	hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (LPVOID *)&deviceEnumerator);
	IMMDevice *defaultDevice = NULL;

	hr = deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
	deviceEnumerator->Release();
	deviceEnumerator = NULL;

	IAudioEndpointVolume *endpointVolume = NULL;
	hr = defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (LPVOID *)&endpointVolume);

	// ---------------------------
	// Get the session managers for the endpoint device.	
	IAudioSessionManager2 *pManager2 = NULL;
	hr = defaultDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, NULL, (void**)&pManager2);

	//Release the device, we have what we need
	defaultDevice->Release();
	defaultDevice = NULL;

	// Master volume
	float currentVolume = 0;
	
	endpointVolume->GetMasterVolumeLevel(&currentVolume);
	printf("Current volume in dB is: %f\n", currentVolume);
	sendMasterInfo(currentVolume);
	Sleep(100);
	hr = endpointVolume->GetMasterVolumeLevelScalar(&currentVolume);
	printf("Current volume as a scalar is: %f\n", currentVolume);

	//get the sessionenumerator
	IAudioSessionEnumerator *pEnumerator = NULL;
	hr = pManager2->GetSessionEnumerator(&pEnumerator);

	//Get data and list info about streams		
	groupCount = getGroups(pEnumerator, groups);

	getLabels(pEnumerator, groups, groupCount);



	if (decibels)
	{
		hr = endpointVolume->SetMasterVolumeLevel((float)newVolume, NULL);
	}
	else if (scalar)
	{
		hr = endpointVolume->SetMasterVolumeLevelScalar((float)newVolume, NULL);
	}
	sendChannelInfo(8);
		

	//Clean up
	endpointVolume->Release();
	pEnumerator->Release();
	pManager2->Release();

	RS232_CloseComport(cport_nr);

	CoUninitialize();
	return 0;
}



int getGroups(IAudioSessionEnumerator *pEnumerator, groupData_t *groupData)
{

	typedef struct
	{
		IAudioSessionControl *pSessionControl;
		GUID guid;
	}streamList_t;

	streamList_t streams[MAX_STREAMS];
	groupData_t wgroups[MAX_GROUPS];

	HRESULT hr;
	int currentStreamCount = 0;
	int groupCount0 = 0;
	int groupCount = 0;
	int groupCountG = 0;

	hr = pEnumerator->GetCount(&currentStreamCount);

	printf("Current number of streams %d\n", currentStreamCount);

	//Get guids
	for (int i = 0; i < currentStreamCount; i++)
	{
		//Get session control
		hr = pEnumerator->GetSession(i, &streams[i].pSessionControl);

		//Get guid
		hr = streams[i].pSessionControl->GetGroupingParam(&streams[i].guid);
		if ((hr == S_OK))
		{
			/*			OLECHAR* guidString;
						StringFromCLSID(streams[i].guid, &guidString);
						printf("Stream no %3d, group: %S\n",
							i,
							guidString);
						CoTaskMemFree(guidString);			*/
		}
	}

	//Get null guids, directly to the output
	for (int i = 0; i < currentStreamCount; i++)
	{
		if (streams[i].guid == GUID_NULL)
		{
			//Copy the current stream to a group
			groupData[groupCount0++].guid = streams[i].guid;
			groupData[groupCount0++].pSessionControl = streams[i].pSessionControl;

			groupCount0++;
		}
	}

	//Get the remaining guids
	for (int i = 0; i < currentStreamCount; i++)
	{
		if (streams[i].guid != GUID_NULL)
		{
			//Copy the current stream to a group
			wgroups[groupCount].guid = streams[i].guid;
			wgroups[groupCount].pSessionControl = streams[i].pSessionControl;

			groupCount++;
		}
	}

	groupCountG = groupCount0;
	//Compare guids, throw away duplicates
	for (int i = 0; i < groupCount; i++)
	{
		for (int j = 0; j < groupCount; j++)
		{
			if ((wgroups[j].guid != GUID_NULL) && (j != i) && IsEqualGUID(wgroups[i].guid, wgroups[j].guid))
			{
				//OLECHAR* guidString;
				//StringFromCLSID(groups[i].guid, &guidString);
				//printf("Clearing group no %d, %d == %d, group: %S\n",
				//	j,
				//	i,
				//	j,
				//	guidString);
				memset(&wgroups[j], 0, sizeof(groupData_t));
			}
		}
	}

	//Collect the remaining unique guids
	for (int i = 0; i < groupCount; i++)
	{
		if (wgroups[i].guid != GUID_NULL)
		{
			groupData[groupCountG].guid = wgroups[i].guid;
			groupData[groupCountG].pSessionControl = wgroups[i].pSessionControl;

			groupCountG++;
		}
	}

	//Total number of groups, get the rest of the data
	for (int i = 0; i < groupCountG; i++)
	{
		//Sessioncontrol2
		groupData[i].pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&groupData[i].pSessionControl2);
		groupData[i].pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&groupData[i].pVolumeControl);
		groupData[i].pSessionControl->GetDisplayName(&groupData[i].displayName);


		OLECHAR* guidString;
		StringFromCLSID(groupData[i].guid, &guidString);
		printf("Group no %3d, group: %S, name: \"%S\"\n",
			i,
			guidString,
			groupData[i].displayName);
	}

	printf("Current number of groups %d\n", groupCountG);

	return groupCountG;
}

void getLabels(IAudioSessionEnumerator *pEnumerator, groupData_t *groupData, int groupCount)
{
	HRESULT hr;

	//Find the groups that fo not have a good label already
	for (int i = 0; i < groupCount; i++)
	{
		if (!wcscmp(groupData[i].displayName, L""))
		{
			printf("Group %d has no label", i);
		}
		else
		{
			printf("Group %d has a label: \"%S\"", i, groupData[i].displayName);
			wcscpy_s(groups[i].prettyName, _countof(groups[i].prettyName), groupData[i].displayName);
		}

		//Get process id

		//Get imagename
		DWORD pid;
		hr = groupData[i].pSessionControl2->GetProcessId(&pid);
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

				if (wcslen(fname))
				{
					wcscpy_s(groups[i].prettyName, _countof(groups[i].prettyName), fname);
					memset(fname, 0, sizeof(fname));
				}

				if (!EnumWindows(EnumWindowsProcMy, pid))
				{
					if (GetWindowText((HWND)g_HWND, Buffer, _countof(Buffer)))
					{
						printf(", window text: \"%S\"", Buffer);
						if (wcslen(Buffer))
						{
							wcscpy_s(groups[i].prettyName, _countof(groups[i].prettyName), Buffer);
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

		printf(", prettyName: \"%S\"", groups[i].prettyName);
		printf("\n");
	}
}


void sendChannelInfo(int ch)
{
	//size_t numconv;
	//char charName[CHNAMELEN * 2];
	//uint8_t vol;
	//float fvol;

	//groups[ch].pVolumeControl->GetMasterVolume(&fvol);
	////scale [-64.0 ; 0.0] to [0 ; 100]

	//vol = (fvol + 64.0) / 0.64;

	//wcstombs_s(&numconv, charName, groups[ch].prettyName, sizeof(charName));
	//
	////Build message
	//pMsg = msgBuf;
	//msg_t *msg;
	//*pMsg++ = STX;

	//// msg structure start
	//msg = (msg_t*)pMsg;
	//msg->startCookie = STARTCOOKIE;
	//msg->chIndex = ch;		

	////volume info
	//msg->chData.update = 1;
	//msg->chData.volVal = vol;
	//memset(msg->chData.name, 0, CHNAMELEN);
	//memcpy_s(msg->chData.name, CHNAMELEN, charName, numconv);

	////Finish message
	//pMsg += sizeof(msg_t);
	//msg->endCookie = ENDCOOKIE;
	//*pMsg++ = ETX;

	//RS232_SendBuf(cport_nr, msgBuf, sizeof(msg_t) + 2);
}

void sendMasterInfo(float fvol)
{
	static serialProtocol_t *msg = allocProtocolBuf(MSGTYPE_SET_MASTER_VOL_PREC, sizeof(serialProtocol_set_master_vol_prec_t));
	

	msg->msg_set_master_vol_prec.volVal = 50;

	protocolTxData(msg, sizeof(serialProtocol_set_master_vol_prec_t));

	freeProtocolBuf(msg);
}

serialProtocol_t * allocProtocolBuf(msgtype_t msgType, size_t bytes)
{
	serialProtocol_t *blockPtr = 0;

	blockPtr = (serialProtocol_t*)malloc(bytes);

	blockPtr->msgType = msgType;

	return blockPtr;
}

void freeProtocolBuf(serialProtocol_t *blockBtr)
{
	free(blockBtr);
}

#ifdef serialSendBuffer
static void protocolTxData(void *dataPtr, int dataLength)
{
	int numData = 0;
	int i;
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
			*txBufPtr++ = msgBuffer[i] ^ 0x10;
			break;

		default:
			*txBufPtr++ = msgBuffer[i];
			break;
		}
	}

	//Finish off by adding the ETX
	*txBufPtr++ = ETX;
	
	   
	serialSendBuffer(txBuffer, txBufPtr - &txBuffer[0]);
}
#endif
