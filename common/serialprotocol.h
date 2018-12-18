
#ifndef _SERIALPROTOCOL_H_
#define _SERIALPROTOCOL_H_

// --------------------- Serial Protocol start ----------------------
#define STX				2
#define ETX				3
#define DLE				0x10

typedef enum
{
    MSGSTATE_IDLE = 0,
    MSGSTATE_ACTIVE,
    MSGSTATE_DLE
}msgState_t;

const int MAX_MSG_LENGTH = 120;	//Any old number, deemed enough, would do
const int MAX_RXTX_BUFFER_LENGTH = MAX_MSG_LENGTH * 2 + 4; //Needs to facilitate the start and stop tokens and the worst case stuffing situation

uint8_t msgBuffer[MAX_MSG_LENGTH] = { 0 };
uint8_t txBuffer[MAX_RXTX_BUFFER_LENGTH] = { 0 };
uint8_t rxBuffer[MAX_RXTX_BUFFER_LENGTH] = { 0 };

#ifdef serialSendBuffer
void protocolTxData(void *, int);	//Use this to send a known number of data bytes, set up the send macro to use
#endif

// ------------------------- Data layer ---------------------------
typedef uint8_t msgtype_t;
const msgtype_t MSGTYPE_SET_MASTER_VOL_PREC = 0;
const msgtype_t MSGTYPE_SET_MASTER_LABEL = 1;
const msgtype_t MSGTYPE_SET_CHANNEL_VOL_PREC = 2;
const msgtype_t MSGTYPE_SET_CHANNEL_LABEL = 3;
const msgtype_t MSGTYPE_SET_MASTER_ICON = 4;

struct msg_set_master_vol_prec
{
    msgtype_t msgType;
    uint8_t volVal;
};

struct msg_set_master_label
{
    msgtype_t msgType;
    uint8_t strLen;
    uint8_t str[];
};

struct msg_set_channel_vol_prec
{
    msgtype_t msgType;
    uint8_t channel;
    uint8_t volVal;
};

struct msg_set_channel_label
{
    msgtype_t msgType;
    uint8_t channel;
    uint8_t strLen;
    uint8_t str[];
};

struct msg_set_master_icon
{
    msgtype_t msgType;
    uint8_t icon[];
};

typedef union
{
    msgtype_t msgType;
    struct msg_set_master_vol_prec		msg_set_master_vol_prec;
    struct msg_set_master_label			msg_set_master_label;
    struct msg_set_channel_vol_prec		msg_set_channel_vol_prec;
    struct msg_set_channel_label		msg_set_channel_label;
    struct msg_set_master_icon          msg_set_master_icon;
}serialProtocol_t;

void protocolTxData(void *, int);	//Use this to send a known number of data bytes, set up a send macro to use

serialProtocol_t * allocProtocolBuf(msgtype_t, size_t);
void freeProtocolBuf(serialProtocol_t *);

// --------------------- Serial Protocol end ----------------------

#endif
