/**************************************************************************
This is an example for our Monochrome OLEDs based on SSD1306 drivers

Pick one up today in the adafruit shop!
------> http://www.adafruit.com/category/63_98

This example is for a 128x32 pixel display using I2C to communicate
3 pins are required to interface (two I2C and one reset).

Adafruit invests time and resources providing this open
source code, please support Adafruit and open-source
hardware by purchasing products from Adafruit!

Written by Limor Fried/Ladyada for Adafruit Industries,
with contributions from the open source community.
BSD license, check license.txt for more information
All text above, and the splash screen below must be
included in any redistribution.
**************************************************************************/
// HW I2C
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

#include <Encoder.h>

#include "bitmaps.h"
#include "serialprotocol.h"

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
#define MA_SCREEN_WIDTH 128 // OLED display width, in pixels
#define MA_SCREEN_HEIGHT 64 // OLED display height, in pixels
#define CH_SCREEN_WIDTH 128 // OLED display width, in pixels
#define CH_SCREEN_HEIGHT 32 // OLED display height, in pixels
Adafruit_SSD1306 mdisplay(MA_SCREEN_WIDTH, MA_SCREEN_HEIGHT, &Wire, -1, 800000, 800000);
Adafruit_SSD1306 display(CH_SCREEN_WIDTH, CH_SCREEN_HEIGHT, &Wire, -1, 800000, 800000);

#define MAP_MAX       100
#define ENC_SCALE_MIN 0
#define ENC_SCALE_MAX 200
#define NUM_CHANNELS	4
#define NUM_BUSES	    5
enum BUS_NUMBER
{
    CHANNEL_0 = 0,
    CHANNEL_1,
    CHANNEL_2,
    CHANNEL_3,
    MASTER,
};

#define BUSY_WAIT_1MS   16L
#define BUSY_WAIT_1S    (BUSY_WAIT_1MS * 1000L)
#define SLEEP_TIMEOUT   (BUSY_WAIT_1S * 5L)

//Set up the serial protocol functions
#define serialSendBuffer(_dPtr, _dCount)	Serial.write(_dPtr, _dCount)

//This is the volume structure, used to control a specific volume
typedef struct
{
    uint8_t update;
    uint8_t volVal;
    char name[32];
    Adafruit_SSD1306 *display;
    uint8_t *iconPtr;
}volume_t;

Encoder encoder(2, 3);
Encoder encoder1(4, 5);
const Encoder *encs[NUM_CHANNELS] =
{
    &encoder,
    &encoder,
    &encoder,
    &encoder
};
const Encoder *mencoder = &encoder;

long prevPos[NUM_CHANNELS+1]    = { 0 };    // Previous encoder position
long encData[NUM_CHANNELS+1]    = { 0 };    // Current value in the allowed range
volume_t chData[NUM_CHANNELS] = { 0 };    // Scaled volume units
volume_t masterData = { 0 };              // Scaled volume units

unsigned int ledval = 0;


void getCmds(uint8_t *, uint16_t);
void readVols(void);
int drawScreen(void);
void drawText(volume_t *, const char *);
void drawBar(volume_t *);
void drawVolIcon(volume_t *);
void drawAppIcon(volume_t *);
void decodeProtocol(void);
void selectBus(int8_t);
void screenSaver(void);
void checkAndSetVolVal(long, long *, volume_t *);
void trimLabel(char *, uint8_t);

/*
**------------------------------------------------------------------------------
** setup:
**
** Sets everything up
**------------------------------------------------------------------------------
*/
void setup()
{
    int i;

    pinMode(13, OUTPUT);

    Serial.begin(19200);

    Wire.begin();

    for ( i = CHANNEL_0 ; i < NUM_CHANNELS ; i++)
    {
        selectBus(i);
        // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
        if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3c))  // Address 0x3D for 128x64, 0x3c for 128x32
        {
            for (;;)
            Serial.println(F("Channel display allocation failed")); // Don't proceed, loop forever
        }

        // Clear the displays at start
        chData[i].update = 1;
        chData[i].display = &display;
        chData[i].display->clearDisplay();
        //chData[i].display->display();

        chData[i].display->setTextSize(1);      // Normal 1:1 pixel scale
        chData[i].display->setTextColor(WHITE); // Draw white text
        chData[i].display->setCursor(0, 0);     // Start at top-left corner
        chData[i].display->cp437(true);         // Use full 256 char 'Code Page 437' font
        chData[i].display->display();
        chData[i].iconPtr = NULL;
    }

    selectBus(MASTER);
    if (!mdisplay.begin(SSD1306_SWITCHCAPVCC, 0x3d))  // Address 0x3D for 128x64, 0x3c for 128x32
    {
        for (;;)
        Serial.println(F("Master display allocation failed")); // Don't proceed, loop forever
    }
    masterData.update = 1;
    masterData.display = &mdisplay;
    masterData.display->clearDisplay();
    //masterData.display->.display();

    masterData.display->setTextSize(1);      // Normal 1:1 pixel scale
    masterData.display->setTextColor(WHITE); // Draw white text
    masterData.display->setCursor(0, 0);     // Start at top-left corner
    masterData.display->cp437(true);         // Use full 256 char 'Code Page 437' font
    masterData.display->display();
    masterData.iconPtr = NULL;
}


/*
**------------------------------------------------------------------------------
** loop:
**
** The standard main loop
**------------------------------------------------------------------------------
*/
void loop()
{
    static int loops = 0;
    static unsigned long idletimer = 0;

    decodeProtocol();

    readVols();

    //Avoid display updates while receiving data
    if(!Serial.available())
    {
        if(loops-- <= 0)
        {
            loops = 1;
            if(drawScreen())
            {
                idletimer = 0;
            }
        }
    }
    else
    {
        loops = (BUSY_WAIT_1MS * 125);
    }

    digitalWrite(13, 0);

    //Clear the displays when not in use
    if(idletimer++ >= SLEEP_TIMEOUT)
    {
        idletimer = 0;
        screenSaver();
    }
}

/*
**------------------------------------------------------------------------------
** readVols:
**
** Reads all volumes
**------------------------------------------------------------------------------
*/
void readVols(void)
{
    int i;
    long newPosition;

    for(i = CHANNEL_0 ; i < NUM_CHANNELS ; i++)
    {
        newPosition = ((Encoder)*encs[i]).read(); //Crazy cast to avoid warnings
        checkAndSetVolVal(newPosition, &prevPos[i], &chData[i]);
    }

    newPosition = ((Encoder)*mencoder).read(); //Crazy cast to avoid warnings
    checkAndSetVolVal(newPosition, &prevPos[MASTER], &masterData);
}

/*
**------------------------------------------------------------------------------
** drawScreen:
**
** Redraws the whole screen
**------------------------------------------------------------------------------
*/
int drawScreen(void)
{
    int i;
    int update = 0;
    for (i = 0; i < NUM_CHANNELS; i++)
    {
        if(chData[i].update)
        {
            update++;
        }
    }
    if(masterData.update)
    {
        update++;
    }

    if(!update)
    {
        return 0;
    }

    digitalWrite(13, 1);

    //Draw the master image
    if(masterData.name[0] != 0)
    {
        drawText(&masterData, masterData.name);
    }
    else
    {
        drawText(&masterData, "Master");
    }
    drawBar(&masterData);
    drawVolIcon(&masterData);
    drawAppIcon(&masterData);

    //Update
    selectBus(MASTER);
    masterData.display->display();
    masterData.update = 0;

    //channel data
    for (i = CHANNEL_0 ; i < NUM_CHANNELS; i++)
    {
        if(chData[i].name[0] != 0)
        {
            drawText(&chData[i], (char*)chData[i].name);
        }
        else
        {
            drawText(&chData[i], "Untitled channel");
        }

        drawBar(&chData[i]);
        drawVolIcon(&chData[i]);
        drawAppIcon(&chData[i]);

        //Update
        selectBus(i);
        chData[i].display->display();
        chData[i].update = 0;
    }
    return 1;
}

/*
**------------------------------------------------------------------------------
** drawText:
**
** Draws the text on the screen
**------------------------------------------------------------------------------
*/
void drawText(volume_t *vP, const char *str)
{
    vP->display->setCursor(0, 0);
    vP->display->clearDisplay();
    vP->display->println(str);
    vP->display->print(masterData.volVal, DEC);
    vP->display->println(F(" %"));
}

/*
**------------------------------------------------------------------------------
** drawBar:
**
** Draws the volume bar graph on the screen
**------------------------------------------------------------------------------
*/
void drawBar(volume_t *vP)
{
    vP->display->drawRoundRect(2, 17, 100, 10, 3, WHITE);
    vP->display->fillRoundRect(4, 19, map(masterData.volVal, 0, MAP_MAX, 0, 96),6, 2, WHITE);
}

/*
**------------------------------------------------------------------------------
** drawVolIcon:
**
** Draws the volume icon
**------------------------------------------------------------------------------
*/
void drawVolIcon(volume_t *vP)
{
    if(!vP->volVal)
    {
        vP->display->drawBitmap(108, 14, speaker_mute, SPEAKERICON_WIDTH, SPEAKERICON_HEIGHT, WHITE);
        return;
    }

    if (vP->volVal >= 90)
    {
        vP->display->drawBitmap(108, 14, speaker_100, SPEAKERICON_WIDTH, SPEAKERICON_HEIGHT, WHITE);
    }
    else if (vP->volVal >= 60)
    {
        vP->display->drawBitmap(108, 14, speaker_66, SPEAKERICON_WIDTH, SPEAKERICON_HEIGHT, WHITE);
    }
    else if (vP->volVal >= 30)
    {
        vP->display->drawBitmap(108, 14, speaker_33, SPEAKERICON_WIDTH, SPEAKERICON_HEIGHT, WHITE);
    }
    else
    {
        vP->display->drawBitmap(108, 14, speaker_0, SPEAKERICON_WIDTH, SPEAKERICON_HEIGHT, WHITE);
    }
}

/*
**------------------------------------------------------------------------------
** drawAppIcon:
**
** Draws application icon if available
**------------------------------------------------------------------------------
*/
void drawAppIcon(volume_t *vP)
{
    if(vP->iconPtr)
    {
        vP->display->drawBitmap(108, 0, vP->iconPtr, SPEAKERICON_WIDTH, SPEAKERICON_HEIGHT, WHITE);
    }
}

/*
**------------------------------------------------------------------------------
** getCmds:
**
** Acts on the commands received on serial
**------------------------------------------------------------------------------
*/
void getCmds(uint8_t *pMsgBuf,  uint16_t dataLen)
{
    serialProtocol_t *msgPtr = (serialProtocol_t*)pMsgBuf;
    int channel;

    switch(msgPtr->msgType)
    {
        case MSGTYPE_SET_MASTER_VOL_PREC:
        masterData.volVal = msgPtr->msg_set_master_vol_prec.volVal;
        masterData.update = 1;
        break;

        case MSGTYPE_SET_MASTER_LABEL:
        memset(masterData.name, 0, 32);
        memcpy(masterData.name, msgPtr->msg_set_master_label.str, msgPtr->msg_set_master_label.strLen);
        trimLabel(masterData.name, msgPtr->msg_set_master_label.strLen);
        masterData.update = 1;
        break;

        case MSGTYPE_SET_CHANNEL_VOL_PREC:
        if(msgPtr->msg_set_channel_vol_prec.channel < NUM_CHANNELS)
        {
            channel = msgPtr->msg_set_channel_vol_prec.channel;
            chData[channel].volVal = msgPtr->msg_set_channel_vol_prec.volVal;
            chData[channel].update = 1;
        }
        break;

        case MSGTYPE_SET_CHANNEL_LABEL:
        if(msgPtr->msg_set_channel_label.channel < NUM_CHANNELS)
        {
            channel = msgPtr->msg_set_channel_label.channel;
            memset(chData[channel].name, 0, 32);
            memcpy(chData[channel].name, msgPtr->msg_set_channel_label.str, msgPtr->msg_set_channel_label.strLen);
            trimLabel(chData[channel].name,  msgPtr->msg_set_channel_label.strLen);
            chData[channel].update = 1;
        }
        break;

        case MSGTYPE_SET_MASTER_ICON:
        if(masterData.iconPtr)
        {
            free(masterData.iconPtr);
            masterData.iconPtr = NULL;
        }

        if(!masterData.iconPtr)
        {
            masterData.iconPtr = (uint8_t*)malloc(dataLen);
        }

        if(masterData.iconPtr)
        {
            memcpy(masterData.iconPtr, msgPtr->msg_set_master_icon.icon, dataLen);
            masterData.update = 1;
        }
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

    for(i = 0 ; i < len + 2; i++)
    {
        sum ^= pMsgBuf[i];
    }

    if(check == sum)
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
** decodeProtocol:
**
** Receives serial data and decodes it
**------------------------------------------------------------------------------
*/
void decodeProtocol(void)
{
    uint8_t ch;
    static int msgLen = 0;
    static msgState_t msgState = MSGSTATE_IDLE;
    static uint8_t *msgPtr;

    if(Serial.available())
    {
        ch = (uint8_t)Serial.read();

        if(msgLen >= MAX_RXTX_BUFFER_LENGTH)
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
            if(msgState == MSGSTATE_ACTIVE)
            {
                if(ctrlChecksum(rxBuffer))
                {
                    getCmds(rxBuffer+2, msgLen-2);
                }
                msgState = MSGSTATE_IDLE;
            }
            break;

            case DLE:
            //Stuff byte
            if(MSGSTATE_ACTIVE)
            {
                msgState = MSGSTATE_DLE;
            }
            break;

            default:
            //Copy data
            if(msgState == MSGSTATE_DLE)
            {
                *msgPtr++ = ch ^ DLE;
                msgState = MSGSTATE_ACTIVE;
                msgLen++;
            }
            else if(msgState == MSGSTATE_ACTIVE)
            {
                *msgPtr++ = ch;
                msgLen++;
            }
            break;
        }
    }
}

/*
**------------------------------------------------------------------------------
** selectBus:
**
** Changes the active I2C bus
**------------------------------------------------------------------------------
*/
void selectBus(int8_t busno)
{
    Wire.beginTransmission(0x70);
    Wire.write(1 << busno);
    Wire.endTransmission();
}

/*
**------------------------------------------------------------------------------
** screenSaver:
**
** Clears all displays
**------------------------------------------------------------------------------
*/
void screenSaver(void)
{
    int i;

    for ( i = CHANNEL_0 ; i < NUM_CHANNELS ; i++)
    {
    selectBus(i);
    chData[i].display->clearDisplay();
    chData[i].display->display();
    }
    selectBus(MASTER);
    masterData.display->clearDisplay();
    masterData.display->display();
}

/*
**------------------------------------------------------------------------------
** checkAndSetVolVal:
**
** Checks the provided encoder position for differences amd updates the volume
** accordingly.
**------------------------------------------------------------------------------
*/
void checkAndSetVolVal(long newPosition, long *prevPosition, volume_t *volume)
{
    long diff;

    if (newPosition != *prevPosition)
    {
        diff = *prevPosition - newPosition;

        if(volume->volVal + diff > 100)
        {
            volume->volVal = 100;
        }
        else if(volume->volVal + diff <= 0)
        {
            volume->volVal = 0;
        }
        else
        {
            volume->volVal += diff;
        }
        volume->update = 1;

        *prevPosition = newPosition;
    }
}

/*
**------------------------------------------------------------------------------
** trimLabel:
**
** Searches the provided label for the first non-alphanumeric character and
** terminates the string there.
**------------------------------------------------------------------------------
*/
void trimLabel(char * label, uint8_t len)
{
    int i;

    if (len >= 21)
    {
        i = 21;
    }
    else
    {
        i = len;
    }

    for(; i > 0 ; i--)
    {
        if(!isalnum(label[i]))
        {
            label[i] = 0;
            break;
        }
    }
}
