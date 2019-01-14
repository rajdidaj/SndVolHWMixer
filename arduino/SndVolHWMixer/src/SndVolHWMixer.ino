/*
**------------------------------------------------------------------------------
** This is the Arduino Mega 2560 receiver of the SndVolHWMixer software.
** It is used to drive a bunch of Adafruit_SSD1306 OLEDs with information
** from the Windows Volume Mixer (AKA sndvol32).
** It also decodes a number of encoders and returns this information to the
** Windows application, allowing for separate hardware volume controls.
**
** Use responsibly!
**
**------------------------------------------------------------------------------
*/
// HW I2C
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

#include "bitmaps.h"
#include "../../../common/serialprotocol.h"

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
#define MA_SCREEN_WIDTH         128     // OLED display width, in pixels
#define MA_SCREEN_HEIGHT        64      // OLED display height, in pixels
#define CH_SCREEN_WIDTH         128     // OLED display width, in pixels
#define CH_SCREEN_HEIGHT        32      // OLED display height, in pixels
Adafruit_SSD1306 mdisplay(MA_SCREEN_WIDTH, MA_SCREEN_HEIGHT, &Wire, -1, 600000, 400000);
Adafruit_SSD1306 display(CH_SCREEN_WIDTH, CH_SCREEN_HEIGHT, &Wire, -1, 600000, 400000);

#define MINVOLVAL               0
#define MAXVOLVAL               100
#define MAX_TEXT_LEN            80
#define MAX_TEXT_ONSCREEN       21

enum BUS_NUMBER
{
    BUS_0 = 0,
    BUS_1,
    BUS_2,
    BUS_3,
    BUS_4,
    BUS_5,
    BUS_6,
    BUS_MASTER,
    NUM_BUSES
};

enum CHANNEL_NUMBER
{
    CHANNEL_MASTER = 0,
    CHANNEL_0,
    CHANNEL_1,
    CHANNEL_2,
    CHANNEL_3,
    NUM_CHANNELS
};

const int irqPins[] =
{
    2, //CHANNEL_MASTER
    3, //CHANNEL_0
    4, //CHANNEL_1
    5, //CHANNEL_2
    6, //CHANNEL_3
};

#define BUSY_WAIT_1MS   16L
#define BUSY_WAIT_1S    (BUSY_WAIT_1MS * 1000L)
#define SLEEP_TIMEOUT   (BUSY_WAIT_1S * 5L)

//Set up the serial protocol functions
#define serialSendBuffer(_dPtr, _dCount)	Serial.write(_dPtr, _dCount)

//This is the volume structure, used to control a specific volume
typedef struct
{
    uint8_t active;
    uint8_t encPin;

    uint8_t encWriteUpdate;
    uint8_t encReadUpdate;

    uint8_t update;
    uint8_t scrolling;
    uint8_t volVal;
    uint8_t muteStatus;
    char name[MAX_TEXT_LEN + 1];
    char scrname[MAX_TEXT_ONSCREEN + 1];
    unsigned char curCh;
    Adafruit_SSD1306 *display;
    uint8_t *iconPtr;
}volume_t;

typedef union
{
    uint8_t bval[4];
    uint32_t lval;
}conv_t;

volume_t chData[NUM_CHANNELS]   = { 0 };
unsigned int ledval = 0;

void getCmds(uint8_t *, uint16_t);
void readVols(void);
int drawScreen(void);
void updateScrolls(void);
void drawText(volume_t *, int);
void drawBar(volume_t *);
void drawVolIcon(volume_t *);
void drawAppIcon(volume_t *);
void decodeProtocol(void);
void selectBus(int8_t);
void screenSaver(void);
void pollEncs(void);
void trimLabel(char *, uint8_t);
void sendChannelUpdate(int8_t);
void encoderSetup(int8_t, uint8_t);
void encoderRead(int8_t, volume_t *);
void encoderSet(int8_t ch, uint8_t);
void sleepDisplay(Adafruit_SSD1306*);
void wakeDisplay(Adafruit_SSD1306*);
void initChannel(Adafruit_SSD1306 *, int);

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
        initChannel(&display, i);
    }

    //Set up the master display
    i = CHANNEL_MASTER;
    selectBus(i);
    if (!mdisplay.begin(SSD1306_SWITCHCAPVCC, 0x3d))  // Address 0x3D for 128x64, 0x3c for 128x32
    {
        for (;;)
        Serial.println(F("Master display allocation failed")); // Don't proceed, loop forever
    }
    initChannel(&mdisplay, i);
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
    static uint32_t idletimer = SLEEP_TIMEOUT;

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
                idletimer = SLEEP_TIMEOUT;
            }
            else if(idletimer && !(idletimer % (BUSY_WAIT_1MS * 100)))
            {
                updateScrolls();
            }
        }
    }
    else
    {
        loops = (BUSY_WAIT_1MS * 125);
    }

    digitalWrite(13, 0);

    //Clear the displays when not in use
    if(idletimer)
    {
        idletimer--;
    }
    else
    {
        screenSaver();
    }

    pollEncs();
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

    for(i = CHANNEL_0 ; i < NUM_CHANNELS ; i++)
    {
        if(chData[i].encReadUpdate)
        {
            chData[i].encReadUpdate = 0;

            //Read the encoder
            encoderRead(i, &chData[i]);
            //Update channel in the PC
            sendChannelUpdate(i);
        }
    }

    i = CHANNEL_MASTER;
    if(chData[i].encReadUpdate)
    {
        chData[i].encReadUpdate = 0;

        //Read the encoder
        encoderRead(i, &chData[i]);
        //Update channel in the PC
        sendChannelUpdate(i);
    }
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

    if(chData[CHANNEL_MASTER].update && chData[CHANNEL_MASTER].active)
    {
        update++;

        //Draw the master image
        chData[CHANNEL_MASTER].update = 0;

        if(chData[CHANNEL_MASTER].name[0] == 0)
        {
            sprintf(chData[CHANNEL_MASTER].name, "%s", "Master");
        }
        drawText(&chData[CHANNEL_MASTER], 0);

        drawBar(&chData[CHANNEL_MASTER]);
        drawVolIcon(&chData[CHANNEL_MASTER]);
        drawAppIcon(&chData[CHANNEL_MASTER]);

        //Update
        selectBus(CHANNEL_MASTER);
        wakeDisplay(chData[CHANNEL_MASTER].display);
        chData[CHANNEL_MASTER].display->display();
    }

    for (i = CHANNEL_0; i < NUM_CHANNELS; i++)
    {
        if(chData[i].update && chData[i].active)
        {
            update++;

            chData[i].update = 0;

            if(chData[i].name[0] == 0)
            {
                sprintf(chData[i].name, "%s %d", "Untitled channel", i-CHANNEL_0);
            }
            drawText(&chData[i], 0);

            drawBar(&chData[i]);
            drawVolIcon(&chData[i]);
            drawAppIcon(&chData[i]);

            //Update
            selectBus(i);
            wakeDisplay(chData[i].display);
            chData[i].display->display();
        }
    }

    if(!update)
    {
        return 0;
    }

    digitalWrite(13, 1);

    return 1;
}

/*
**------------------------------------------------------------------------------
** updateScrolls:
**
** Updates scrolling texts
**------------------------------------------------------------------------------
*/
void updateScrolls(void)
{
    int i;

    //Draw the master image
    i = CHANNEL_MASTER;
    if(chData[i].scrolling)
    {
        drawText(&chData[i], 1);

        drawBar(&chData[i]);
        drawVolIcon(&chData[i]);
        drawAppIcon(&chData[i]);

        //Update
        selectBus(i);
        chData[i].display->display();
    }

    //channel data
    for (i = CHANNEL_0 ; i < NUM_CHANNELS; i++)
    {
        if(chData[i].scrolling)
        {
            drawText(&chData[i], 1);

            drawBar(&chData[i]);
            drawVolIcon(&chData[i]);
            drawAppIcon(&chData[i]);

            //Update
            selectBus(i);
            chData[i].display->display();
        }
    }
}

/*
**------------------------------------------------------------------------------
** drawText:
**
** Draws the text on the screen
**------------------------------------------------------------------------------
*/
void drawText(volume_t *vP, int scroll)
{
    vP->display->setCursor(0, 0);
    vP->display->clearDisplay();

    if(strlen(vP->name) > MAX_TEXT_ONSCREEN)
    {
        vP->scrolling = 1;
        strncpy(vP->scrname, &vP->name[(scroll ? vP->curCh++ : vP->curCh)], MAX_TEXT_ONSCREEN);
        if (vP->curCh >= MAX_TEXT_ONSCREEN)
        {
            vP->curCh = 0;
        }
    }
    else
    {
        strncpy(vP->scrname, vP->name, MAX_TEXT_ONSCREEN);
    }
    vP->display->println(vP->scrname);
    vP->display->print(vP->volVal, DEC);
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
    vP->display->fillRoundRect(4, 19, map(vP->volVal, MINVOLVAL, MAXVOLVAL, 0, 96),6, 2, WHITE);
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
    if(vP->muteStatus)
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
        if( (msgPtr->msg_set_master_vol_prec.volVal >= MINVOLVAL) && (msgPtr->msg_set_master_vol_prec.volVal <= MAXVOLVAL) )
        {
            channel = CHANNEL_MASTER;
            chData[channel].volVal = msgPtr->msg_set_master_vol_prec.volVal;
            chData[channel].muteStatus = msgPtr->msg_set_master_vol_prec.muteStatus;

            chData[channel].update = 1;
            encoderSet(channel, chData[channel].volVal);
            chData[channel].active = 1;
        }
        break;

        case MSGTYPE_SET_MASTER_LABEL:
        channel = CHANNEL_MASTER;
        memset(chData[channel].name, 0, sizeof(chData[channel].name));
        strncpy(chData[channel].name, (char*)msgPtr->msg_set_master_label.str, MAX_TEXT_LEN);
        chData[channel].update = 1;
        break;

        case MSGTYPE_SET_CHANNEL_VOL_PREC:
        if(msgPtr->msg_set_channel_vol_prec.channel < NUM_CHANNELS)
        {
            channel = msgPtr->msg_set_channel_vol_prec.channel + CHANNEL_0;
            if( (msgPtr->msg_set_channel_vol_prec.volVal >= MINVOLVAL) && (msgPtr->msg_set_channel_vol_prec.volVal <= MAXVOLVAL) )
            {
                chData[channel].volVal = msgPtr->msg_set_channel_vol_prec.volVal;
                chData[channel].muteStatus = msgPtr->msg_set_channel_vol_prec.muteStatus;

                chData[channel].update = 1;
                encoderSet(channel, chData[channel].volVal);
                chData[channel].active = 1;
            }
        }
        break;

        case MSGTYPE_SET_CHANNEL_LABEL:
        if(msgPtr->msg_set_channel_label.channel < NUM_CHANNELS)
        {
            channel = msgPtr->msg_set_channel_label.channel + CHANNEL_0;
            memset(chData[channel].name, 0, sizeof(chData[channel].name));
            strncpy(chData[channel].name, (char*)msgPtr->msg_set_channel_label.str, MAX_TEXT_LEN);
            chData[channel].update = 1;
        }
        break;

        case MSGTYPE_SET_MASTER_ICON:
        channel = CHANNEL_MASTER;
        if(chData[channel].iconPtr)
        {
            free(chData[channel].iconPtr);
            chData[channel].iconPtr = NULL;
        }

        if(!chData[channel].iconPtr)
        {
            chData[channel].iconPtr = (uint8_t*)malloc(dataLen);
        }

        if(chData[channel].iconPtr)
        {
            memcpy(chData[channel].iconPtr, msgPtr->msg_set_master_icon.icon, dataLen);
            chData[channel].update = 1;
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
** Changes the active I2C bus by channel numbers
**------------------------------------------------------------------------------
*/
void selectBus(int8_t ch)
{
    const int channel2bus[] =
    {
        1 << BUS_MASTER,
        1 << BUS_0,
        1 << BUS_1,
        1 << BUS_2,
        1 << BUS_3,
        1 << BUS_4,
        1 << BUS_5,
        1 << BUS_6
    };

    static int8_t lastch = -1;

    if(ch != lastch)
    {
        Wire.beginTransmission(0x70);
        Wire.write(channel2bus[ch]);
        Wire.endTransmission();

        lastch = ch;
    }
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
        sleepDisplay(chData[i].display);
    }

    i = CHANNEL_MASTER;
    selectBus(i);
    sleepDisplay(chData[i].display);
}

/*
**------------------------------------------------------------------------------
** pollEncs:
**
** Checks the encoder interrupt pins for updates.
**------------------------------------------------------------------------------
*/
void pollEncs(void)
{
    int i;

    for(i = CHANNEL_0 ; i < NUM_CHANNELS ; i++)
    {
        if(!digitalRead(chData[i].encPin) && !chData[i].encWriteUpdate)
        {
            chData[i].encReadUpdate = 1;
        }
        else
        {
            chData[i].encWriteUpdate = 0;
        }
    }

    if(!digitalRead(chData[CHANNEL_MASTER].encPin) && !chData[i].encWriteUpdate)
    {
        chData[CHANNEL_MASTER].encReadUpdate = 1;
    }
    else
    {
        chData[i].encWriteUpdate = 0;
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

    if (len >= MAX_TEXT_LEN)
    {
        i = MAX_TEXT_LEN;
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

/*
**------------------------------------------------------------------------------
** protocolTxData:
**
** Stuffs and checksums the data to be sent
**------------------------------------------------------------------------------
*/
#ifdef serialSendBuffer
void protocolTxData(void *dataPtr, int dataLength)
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
    memcpy(workBufPtr, dataPtr, dataLength);
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
** sendChannelUpdate:
**
** Sends a channel volume change update.
**------------------------------------------------------------------------------
*/
void sendChannelUpdate(int8_t ch)
{
    uint8_t msg[3] = {0};
    uint8_t len = 0;

    if(ch == CHANNEL_MASTER)
    {
        msg[len++] = MSGTYPE_SET_MASTER_VOL_PREC;
        msg[len++] = chData[CHANNEL_MASTER].volVal;
        msg[len++] = chData[CHANNEL_MASTER].muteStatus;
    }
    else
    {
        msg[len++] = MSGTYPE_SET_CHANNEL_VOL_PREC;
        msg[len++] = ch - CHANNEL_0;
        msg[len++] = chData[ch].volVal;
        msg[len++] = chData[ch].muteStatus;
    }

    protocolTxData(msg, len);
}

/*
**------------------------------------------------------------------------------
** encoderSetup:
**
** Sets up the selected encoder
**------------------------------------------------------------------------------
*/
void encoderSetup(int8_t ch, uint8_t volVal)
{
    conv_t tempval;

    selectBus(ch);

    //Set up general config and interrupts
    Wire.beginTransmission(0); //address the encoder
    Wire.write(0x00); //Select general configuration register
    Wire.write(0x08);
    Wire.endTransmission();

    Wire.beginTransmission(0); //address the encoder
    Wire.write(0x04); //Select general configuration register
    Wire.write(0x19); //inc/dec interrupt
    Wire.endTransmission();

    //Set up min/max values
    Wire.beginTransmission(0); //address the encoder
    Wire.write(0x08); //Select counter value register

    tempval.lval = volVal;
    Wire.write(tempval.bval[3]); //Write the counter value
    Wire.write(tempval.bval[2]); //Write the counter value
    Wire.write(tempval.bval[1]); //Write the counter value
    Wire.write(tempval.bval[0]); //Write the counter value

    tempval.lval = MAXVOLVAL;
    Wire.write(tempval.bval[3]); //Write the counter max value
    Wire.write(tempval.bval[2]); //Write the counter max value
    Wire.write(tempval.bval[1]); //Write the counter max value
    Wire.write(tempval.bval[0]); //Write the counter max value

    tempval.lval = MINVOLVAL;
    Wire.write(tempval.bval[3]); //Write the counter min value
    Wire.write(tempval.bval[2]); //Write the counter min value
    Wire.write(tempval.bval[1]); //Write the counter min value
    Wire.write(tempval.bval[0]); //Write the counter min value

    Wire.write(0); //Write the increment step value
    Wire.write(0); //Write the increment step value
    Wire.write(0); //Write the increment step value
    Wire.write(1); //Write the increment step value

    Wire.endTransmission();

}

/*
**------------------------------------------------------------------------------
** encoderRead:
**
** Reads the selected encoder
**------------------------------------------------------------------------------
*/
void encoderRead(int8_t ch, volume_t *vP)
{
    int32_t pos;
    uint8_t irq;

    selectBus(ch);

    //Clear any interrupts
    Wire.beginTransmission(0); //address the encoder
    Wire.write(0x05); //Select register
    Wire.endTransmission();

    irq = 0;
    Wire.requestFrom(0, 1);
    if (Wire.available())
    {
        irq = Wire.read();
    }

    //Value changed
    if(irq & 0x18)
    {
        Wire.beginTransmission(0); //address the encoder
        Wire.write(0x08); //Select register
        Wire.endTransmission();

        pos = 0;
        Wire.requestFrom(0, 4);    // request 4 bytes
        if (Wire.available())
        {
            pos = Wire.read();
            pos *= 256;
            pos += Wire.read();
            pos *= 256;
            pos += Wire.read();
            pos *= 256;
            pos += Wire.read();
        }
        Wire.endTransmission();

        vP->update = 1;
        vP->volVal = pos;
    }

    //Button released
    if(irq & 1)
    {
        if(vP->muteStatus)
        {
            vP->muteStatus = 0;
        }
        else
        {
            vP->muteStatus = 1;
        }
        vP->update = 1;
    }
}


/*
**------------------------------------------------------------------------------
** encoderSet:
**
** Sets the current encoder value
**------------------------------------------------------------------------------
*/
void encoderSet(int8_t ch, uint8_t val)
{
    conv_t tempval;

    selectBus(ch);
    Wire.beginTransmission(0); //address the encoder
    Wire.write(0x08); //Select register

    tempval.lval = val;
    Wire.write(tempval.bval[3]); //Write the counter value
    Wire.write(tempval.bval[2]); //Write the counter value
    Wire.write(tempval.bval[1]); //Write the counter value
    Wire.write(tempval.bval[0]); //Write the counter value

    Wire.endTransmission();
}

/*
**------------------------------------------------------------------------------
** sleepDisplay:
**
** Sets teh display into sleep mode
**------------------------------------------------------------------------------
*/
void sleepDisplay(Adafruit_SSD1306* display)
{
  display->ssd1306_command(SSD1306_DISPLAYOFF);
}

/*
**------------------------------------------------------------------------------
** wakeDisplay:
**
** Wakes the display from sleep mode
**------------------------------------------------------------------------------
*/
void wakeDisplay(Adafruit_SSD1306* display)
{
  display->ssd1306_command(SSD1306_DISPLAYON);
}

/*
**------------------------------------------------------------------------------
** initChannel:
**
** Sets up a channel
**------------------------------------------------------------------------------
*/
void initChannel(Adafruit_SSD1306 *dP, int ix)
{
    // Clear the displays at start
    chData[ix].update = 1;
    chData[ix].display = dP;

    wakeDisplay(chData[ix].display);

    chData[ix].display->clearDisplay();
    chData[ix].display->display();

    chData[ix].display->setTextSize(1);      // Normal 1:1 pixel scale
    chData[ix].display->setTextColor(WHITE); // Draw white text
    chData[ix].display->setCursor(0, 0);     // Start at top-left corner
    chData[ix].display->cp437(true);         // Use full 256 char 'Code Page 437' font
    chData[ix].display->display();
    chData[ix].iconPtr = NULL;

    chData[ix].encPin = irqPins[ix];
    chData[ix].encWriteUpdate = 1;
    encoderSetup(ix, chData[ix].volVal);
    pinMode(irqPins[ix], INPUT_PULLUP);

    chData[ix].muteStatus = 0;
    chData[ix].active = 0;
}
