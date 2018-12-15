Serial protocol

    Transport layer         Start                                                 End 
                                |                                                 |
    Protocol layer              |--Length                                Checksum-|   
                                        |                                |
    Data layer                          |--Data--------------------------|

    Reserved symbols:
        Start token:    STX
        
        End token:      ETX
        
        Stuff byte:     DLE

Length:             uint16_t, small endian. Length of the data layer, before stuffing.

Checksum:           uint16_t, XORed sum of all raw (unstuffed) bytes in the protocol layer.

Stuff pattern:      DLE, (DataByte XOR DLE)

                        E.g: ETX in data is stuffed to: DLE, 0x13

TX example:
    Get buffer, data size + 4 bytes (reserve room for length and checksum).
    
    Send STX
    
    Send data byte by byte, if a reserved symbol is encontered, send a DLE, followed by the DLE-XORed symbol
    
    Send ETX


RX example:
    Wait for STX, if triggered, set up a receive buffer.
    
    Add each byte to the receive buffer, if a DLE is encountered, throw it away and XOR the next byte with DLE.
    
    If ETX is encontered, the protocol layer message is received.


Data layer:
    MSGTYPE | D00 D01 .. Dnn 

Message types:

    MSGTYPE 0: Set master volume %
        PC <-> MCU
        uint8_t    volVal

    MSGTYPE 1: Set (custom) master label
        PC -> MCU
        uint8_t    strLen
        char[]     str, null terminated, MAX 32

    MSGTYPE 2: Set channel volume %
        PC <-> MCU
        uint8_t     channel
        uint8_t     volVal

    MSGTYPE 3: Set (custom) channel label
        PC -> MCU
        uint8_t     channel
        uint8_t     strLen
        char[]      str, null terminated, MAX 32
        
    MSGTYPE 4: Set master icon
        PC->MCU
        uint8_t     icon