/*
 * NTAG21x.c
 *
 *  Created on: 20.02.2019
 *  Author: Giovanni Cammisa (gcammisa)
 *  Still missing support for:
 *      -The management of both static and dynamic lock bytes
 *      -Bruteforce protection (AUTHLIM COUNTER)
 *  Thanks to skuser for the MifareUltralight code used as a starting point
 */

#include "NTAG21x.h"
#include "ISO14443-3A.h"
#include "../Codec/ISO14443-2A.h"
#include "../Memory/Memory.h"

//DEFINE ATQA and SAK
#define ATQA_VALUE 0x0044
#define SAK_VALUE 0x00

#define SAK_CL1_VALUE           ISO14443A_SAK_INCOMPLETE
#define SAK_CL2_VALUE           ISO14443A_SAK_COMPLETE_NOT_COMPLIANT

//ACK and NACK
#define ACK_VALUE                   0x0A
#define ACK_FRAME_SIZE          4 /* Bits */
#define NAK_INVALID_ARG             0x00
#define NAK_CRC_ERROR               0x01
#define NAK_NOT_AUTHED              0x04
#define NAK_EEPROM_ERROR            0x05
#define NAK_FRAME_SIZE          4

//DEFINING COMMANDS
/* ISO commands */
#define CMD_HALT 0x50
//NTAG COMMANDS
#define CMD_GET_VERSION 0x60
#define CMD_READ 0x30
#define CMD_FAST_READ 0x3A
#define CMD_WRITE 0xA2
#define CMD_COMPAT_WRITE 0xA0
#define CMD_READ_CNT 0x39
#define CMD_PWD_AUTH 0x1B
#define CMD_READ_SIG 0x3C


//MEMORY LAYOUT STUFF, addresses and sizes in bytes
//UID stuff
#define UID_CL1_ADDRESS         0x00
#define UID_CL1_SIZE            3
#define UID_BCC1_ADDRESS        0x03
#define UID_CL2_ADDRESS         0x04
#define UID_CL2_SIZE            4
#define UID_BCC2_ADDRESS        0x08
//LockBytes stuff
#define STATIC_LOCKBYTE_0_ADDRESS   0x0A
#define STATIC_LOCKBYTE_1_ADDRESS   0x0B
//CONFIG stuff
#define NTAG213_CONFIG_AREA_START_ADDRESS   NTAG21x_PAGE_SIZE * 0x29
#define NTAG215_CONFIG_AREA_START_ADDRESS   NTAG21x_PAGE_SIZE * 0x83
#define NTAG216_CONFIG_AREA_START_ADDRESS   NTAG21x_PAGE_SIZE * 0xE3
#define CONFIG_AREA_SIZE        8
//CONFIG offsets, relative to config start address
#define CONF_AUTH0_OFFSET       0x03
#define CONF_ACCESS_OFFSET      0x04
#define CONF_PASSWORD_OFFSET    0x08
#define CONF_PACK_OFFSET        0x0C

//WRITE STUFF
#define BYTES_PER_WRITE         4
#define PAGE_WRITE_MIN          0x02

//CONFIG masks to check individual needed bits
#define CONF_ACCESS_PROT        0x80

#define VERSION_INFO_LENGTH 8 //8 bytes info lenght + crc

#define BYTES_PER_READ NTAG21x_PAGE_SIZE * 4
//SIGNATURE Lenght
#define SIGNATURE_LENGTH        32

static enum {
    TYPE_NTAG213,
    TYPE_ULTRAMANZ,
    TYPE_NTAG215,
    TYPE_NTAG216
} Ntag_Type;

static enum {
  STATE_HALT,
  STATE_IDLE,
  STATE_READY1,
  STATE_READY2,
  STATE_ACTIVE
} State;

static uint32_t ConfigStartAddr = NTAG213_CONFIG_AREA_START_ADDRESS;
static bool FromHalt = false;
static uint8_t PageCount;
static bool ArmedForCompatWrite;
static uint8_t CompatWritePageAddress;
static bool Authenticated;
static uint8_t FirstAuthenticatedPage;
static bool ReadAccessProtected;
static uint8_t Access;


//Writes a page
static uint8_t AppWritePage(uint8_t PageAddress, uint8_t *const Buffer) {
    if (!ActiveConfiguration.ReadOnly) {
        AppCardMemoryWrite(Buffer, PageAddress * NTAG21x_PAGE_SIZE, NTAG21x_PAGE_SIZE);
    } else {
        /* If the chameleon is in read only mode, it silently
        * ignores any attempt to write data. */
    }
    return 0;
}

//HELPER FUNCTIONS
void NTAG21xGetUid(ConfigurationUidType Uid) {
    /* Read UID from memory */
    AppCardMemoryRead(&Uid[0], UID_CL1_ADDRESS, UID_CL1_SIZE);
    AppCardMemoryRead(&Uid[UID_CL1_SIZE], UID_CL2_ADDRESS, UID_CL2_SIZE);
}

void NTAG21xSetUid(ConfigurationUidType Uid) {
    /* Calculate check bytes and write everything into memory */
    uint8_t BCC1 = ISO14443A_UID0_CT ^ Uid[0] ^ Uid[1] ^ Uid[2];
    uint8_t BCC2 = Uid[3] ^ Uid[4] ^ Uid[5] ^ Uid[6];

    AppCardMemoryWrite(&Uid[0], UID_CL1_ADDRESS, UID_CL1_SIZE);
    AppCardMemoryWrite(&BCC1, UID_BCC1_ADDRESS, ISO14443A_CL_BCC_SIZE);
    AppCardMemoryWrite(&Uid[UID_CL1_SIZE], UID_CL2_ADDRESS, UID_CL2_SIZE);
    AppCardMemoryWrite(&BCC2, UID_BCC2_ADDRESS, ISO14443A_CL_BCC_SIZE);
}

static void NTAG21xAppInit(void) {
    State = STATE_IDLE;
    
    FromHalt = false;
    ArmedForCompatWrite = false;
    Authenticated = false;

    switch (Ntag_Type) {
    case TYPE_NTAG213:
    case TYPE_ULTRAMANZ: {
        PageCount = NTAG213_PAGES;
        ConfigStartAddr = NTAG213_CONFIG_AREA_START_ADDRESS;
        break;
    }
    case TYPE_NTAG215: {
        PageCount = NTAG215_PAGES;
        ConfigStartAddr = NTAG215_CONFIG_AREA_START_ADDRESS;
        break;
    }
    case TYPE_NTAG216: {
        PageCount = NTAG216_PAGES;
        ConfigStartAddr = NTAG216_CONFIG_AREA_START_ADDRESS;
        break;
    }
    default:
        break;
    }
    
    /* Fetch some of the configuration into RAM */
    AppCardMemoryRead(&FirstAuthenticatedPage, ConfigStartAddr + CONF_AUTH0_OFFSET, 1);
    AppCardMemoryRead(&Access, ConfigStartAddr + CONF_ACCESS_OFFSET, 1);
    ReadAccessProtected = !!(Access & CONF_ACCESS_PROT);
}

void NTAG21xAppReset(void) {
    State = STATE_IDLE;
}

#if defined(CONFIG_NTAG213_SUPPORT)

void NTAG213AppInit(void) {
    Ntag_Type = TYPE_NTAG213;
    NTAG21xAppInit();
}

#endif // CONIFG_NTAG213_SUPPORT

#ifdef CONFIG_ULTRAMANZ_SUPPORT

void UltramanAppInit(void) {
    Ntag_Type = TYPE_ULTRAMANZ;
    PageCount = NTAG213_PAGES;
    ConfigStartAddr = NTAG213_CONFIG_AREA_START_ADDRESS;
    NTAG21xAppInit();
}

static void UltramanZ_ButtonFunc(uint8_t ultraman_type) {
    Ultraman_Medal ultraman_code;
    uint8_t magic_code[4];
    static uint8_t index = 1;
    static uint8_t max_index;
    ConfigurationUidType Uid;    

    AppCardMemoryRead(&ultraman_code, ULTRAMANZ_CODE_PAGE * NTAG21x_PAGE_SIZE, NTAG21x_PAGE_SIZE);

    switch (ultraman_type) {
    case ULTRAMANZ_ACC_POS_TYPE:
    case ULTRAMANZ_ACC_NEG_TYPE: {
        max_index = 20;
        break;
    }
    case ULTRAMANZ_CHAR_POS_TYPE:
    case ULTRAMANZ_CHAR_NEG_TYPE: {
        max_index = 128;
        break;
    }
    default:
        max_index = 20;
        break;
    }
    if (ultraman_code.type != ultraman_type) {
        ultraman_code.type = ultraman_type;
        if (ultraman_type == ULTRAMANZ_ACC_POS_TYPE
            || ultraman_type == ULTRAMANZ_ACC_NEG_TYPE) {
            ultraman_code.type2 = 0x01;
        }
        magic_code[0] = 0xba;
        magic_code[1] = 0xbd;
        magic_code[2] = 0x10;
        magic_code[3] = 0x20;
        AppWritePage(29, magic_code);
        magic_code[0] = 0xbe;
        magic_code[1] = 0xef;
        magic_code[2] = 0x00;
        magic_code[3] = 0x00;
        AppCardMemoryWrite(magic_code, ConfigStartAddr + CONF_PACK_OFFSET, 4);
    }
    ultraman_code.index = index;

    if (++index > max_index) {
        index = 1;
    }

    ultraman_code.sum = ultraman_code.type + ultraman_code.index + ultraman_code.type2;

    AppWritePage(ULTRAMANZ_CODE_PAGE, (uint8_t *)&ultraman_code);

    NTAG21xGetUid(Uid);

    memcpy(&Uid[4], &ultraman_code, 3);

    NTAG21xSetUid(Uid);
}

void Z_ACC_POS_ButtonFunc(void) {
    UltramanZ_ButtonFunc(ULTRAMANZ_ACC_POS_TYPE);
}

void Z_ACC_NEG_ButtonFunc(void) {
    UltramanZ_ButtonFunc(ULTRAMANZ_ACC_NEG_TYPE);
}

void Z_CHAR_POS_ButtonFunc(void) {
    UltramanZ_ButtonFunc(ULTRAMANZ_CHAR_POS_TYPE);
}

void Z_CHAR_NEG_ButtonFunc(void) {
    UltramanZ_ButtonFunc(ULTRAMANZ_CHAR_NEG_TYPE);
}

#endif // CONFIG_ULTRAMANZ_SUPPORT

#ifdef CONFIG_NTAG215_SUPPORT

void NTAG215AppInit(void) {
    Ntag_Type = TYPE_NTAG215;
    NTAG21xAppInit();
}

#endif // CONFIG_NTAG215_SUPPORT

#ifdef CONFIG_NTAG216_SUPPORT

void NTAG216AppInit(void) {
    Ntag_Type = TYPE_NTAG216;
    NTAG21xAppInit();
}

#endif // CONFIG_NTAG216_SUPPORT

//Verify authentication
static bool VerifyAuthentication(uint8_t PageAddress) {
    /* If authenticated, no verification needed */
    if (Authenticated) {
        return true;
    }
    /* Otherwise, verify the accessed page is below the limit */
    return PageAddress < FirstAuthenticatedPage;
}

//Basic sketch of the command handling stuff
static uint16_t AppProcess(uint8_t *const Buffer, uint16_t ByteCount) {
    uint8_t Cmd = Buffer[0];

    /* Handle the compatibility write command */
    if (ArmedForCompatWrite) {
        ArmedForCompatWrite = false;

        AppWritePage(CompatWritePageAddress, &Buffer[2]);
        Buffer[0] = ACK_VALUE;
        return ACK_FRAME_SIZE;
    }

    switch (Cmd) {
        case CMD_GET_VERSION: {
            /* Provide hardcoded version response */ //VERSION RESPONSE FOR NTAG 21x
            Buffer[0] = 0x00;
            Buffer[1] = 0x04;
            Buffer[2] = 0x04;
            Buffer[3] = 0x02;
            Buffer[4] = 0x01;
            Buffer[5] = 0x00;
            switch (Ntag_Type) {
            case TYPE_NTAG213:
            case TYPE_ULTRAMANZ: {
                Buffer[6] = 0x0F;
                break;
            }
            case TYPE_NTAG215: {
                Buffer[6] = 0x11;
                break;
            }
            case TYPE_NTAG216: {
                Buffer[6] = 0x13;
                break;
            }
            default:
                break;
            }
            Buffer[7] = 0x03;
            ISO14443AAppendCRCA(Buffer, VERSION_INFO_LENGTH);
            return (VERSION_INFO_LENGTH + ISO14443A_CRCA_SIZE) * 8;
        }

        case CMD_READ: {
            uint8_t PageAddress = Buffer[1];
            uint8_t PageLimit;
            uint8_t Offset;

            PageLimit = PageCount;

            /* if protected and not autenticated, ensure the wraparound is at the first protected page */
            if (ReadAccessProtected && !Authenticated) {
                PageLimit = FirstAuthenticatedPage;
            } else {
                PageLimit = PageCount;
            }

            /* Validation */
            if (PageAddress >= PageLimit) {
                Buffer[0] = NAK_INVALID_ARG;
                return NAK_FRAME_SIZE;
            }
            /* Read out, emulating the wraparound */
            for (Offset = 0; Offset < BYTES_PER_READ; Offset += 4) {
                AppCardMemoryRead(&Buffer[Offset], PageAddress * NTAG21x_PAGE_SIZE, NTAG21x_PAGE_SIZE);
                PageAddress++;
                if (PageAddress == PageLimit) { // if arrived ad the last page, start reading from page 0
                    PageAddress = 0;
                }
            }
            ISO14443AAppendCRCA(Buffer, BYTES_PER_READ);
            return (BYTES_PER_READ + ISO14443A_CRCA_SIZE) * 8;
        }

        case CMD_FAST_READ: {
            uint8_t StartPageAddress = Buffer[1];
            uint8_t EndPageAddress = Buffer[2];
            /* Validation */
            if ((StartPageAddress > EndPageAddress) || (StartPageAddress >= PageCount) || (EndPageAddress >= PageCount)) {
                Buffer[0] = NAK_INVALID_ARG;
                return NAK_FRAME_SIZE;
            }

            /* Check authentication only if protection is read&write (instead of only write protection) */
            if (ReadAccessProtected) {
                if (!VerifyAuthentication(StartPageAddress) || !VerifyAuthentication(EndPageAddress)) {
                    Buffer[0] = NAK_NOT_AUTHED;
                    return NAK_FRAME_SIZE;
                }
            }

            ByteCount = (EndPageAddress - StartPageAddress + 1) * NTAG21x_PAGE_SIZE;
            AppCardMemoryRead(Buffer, StartPageAddress * NTAG21x_PAGE_SIZE, ByteCount);
            ISO14443AAppendCRCA(Buffer, ByteCount);
            return (ByteCount + ISO14443A_CRCA_SIZE) * 8;
        }

        case CMD_PWD_AUTH: {
            uint8_t Password[4];

            /* For now I don't care about bruteforce protection, so: */
            /* TODO: IMPLEMENT COUNTER AUTHLIM */

            /* Read and compare the password */
            switch (Ntag_Type) {

            /* Disable PWD AUTH for Ultraman Z */
            case TYPE_ULTRAMANZ: {
                AppCardMemoryWrite(&Buffer[1], ConfigStartAddr + CONF_PASSWORD_OFFSET, 4);
                break;
            }
            default:
                AppCardMemoryRead(Password, ConfigStartAddr + CONF_PASSWORD_OFFSET, 4);
                if (Password[0] != Buffer[1] || Password[1] != Buffer[2] || Password[2] != Buffer[3] || Password[3] != Buffer[4]) {
                    Buffer[0] = NAK_NOT_AUTHED;
                    return NAK_FRAME_SIZE;
                }
                break;
            }
            /* Authenticate the user */
            //RESET AUTHLIM COUNTER, CURRENTLY NOT IMPLEMENTED
            Authenticated = 1;
            /* Send the PACK value back */
            AppCardMemoryRead(Buffer, ConfigStartAddr + CONF_PACK_OFFSET, 2);
            ISO14443AAppendCRCA(Buffer, 2);
            return (2 + ISO14443A_CRCA_SIZE) * 8;
        }

        case CMD_WRITE: {
            /* This is a write command containing 4 bytes of data that
            * should be written to the given page address. */
            uint8_t PageAddress = Buffer[1];
            /* Validation */
            if ((PageAddress < PAGE_WRITE_MIN) || (PageAddress >= PageCount)) {
                Buffer[0] = NAK_INVALID_ARG;
                return NAK_FRAME_SIZE;
            }
            if (!VerifyAuthentication(PageAddress)) {
                Buffer[0] = NAK_NOT_AUTHED;
                return NAK_FRAME_SIZE;
            }
            AppWritePage(PageAddress, &Buffer[2]);
            Buffer[0] = ACK_VALUE;
            return ACK_FRAME_SIZE;
        }

        case CMD_COMPAT_WRITE: {
            uint8_t PageAddress = Buffer[1];
            /* Validation */
            if ((PageAddress < PAGE_WRITE_MIN) || (PageAddress >= PageCount)) {
                Buffer[0] = NAK_INVALID_ARG;
                return NAK_FRAME_SIZE;
            }
            if (!VerifyAuthentication(PageAddress)) {
                Buffer[0] = NAK_NOT_AUTHED;
                return NAK_FRAME_SIZE;
            }
            /* CRC check passed and page-address is within bounds.
            * Store address and proceed to receiving the data. */
            CompatWritePageAddress = PageAddress;
            ArmedForCompatWrite = true; //TODO:IMPLEMENT ARMED COMPAT WRITE
            Buffer[0] = ACK_VALUE;
            return ACK_FRAME_SIZE;
        }


        case CMD_READ_SIG: {
            /* Hardcoded response */
            memset(Buffer, 0xCA, SIGNATURE_LENGTH);
            ISO14443AAppendCRCA(Buffer, SIGNATURE_LENGTH);
            return (SIGNATURE_LENGTH + ISO14443A_CRCA_SIZE) * 8;
        }

        //PART OF ISO STANDARD, NOT OF NTAG DATASHEET
        case CMD_HALT: {
            /* Halts the tag. According to the ISO14443, the second
            * byte is supposed to be 0. */
            if (Buffer[1] == 0) {
                /* According to ISO14443, we must not send anything
                * in order to acknowledge the HALT command. */
                State = STATE_HALT;
                return ISO14443A_APP_NO_RESPONSE;
            } else {
                Buffer[0] = NAK_INVALID_ARG;
                return NAK_FRAME_SIZE;
            }
        }


        default: {
            break;
        }

    }
    /* Command not handled. Switch to idle. */

    State = STATE_IDLE;
    return ISO14443A_APP_NO_RESPONSE;

}

//FINITE STATE MACHINE STUFF, SHOULD BE THE VERY SIMILAR TO Mifare Ultralight
uint16_t NTAG21xAppProcess(uint8_t *Buffer, uint16_t BitCount) {
    uint8_t Cmd = Buffer[0];
    uint16_t ByteCount;

    switch (State) {
        case STATE_IDLE:
        case STATE_HALT:
            FromHalt = State == STATE_HALT;
            if (ISO14443AWakeUp(Buffer, &BitCount, ATQA_VALUE, FromHalt)) {
                /* We received a REQA or WUPA command, so wake up. */
                State = STATE_READY1;
                return BitCount;
            }
            break;

        case STATE_READY1:
            if (ISO14443AWakeUp(Buffer, &BitCount, ATQA_VALUE, FromHalt)) {
                State = FromHalt ? STATE_HALT : STATE_IDLE;
                return ISO14443A_APP_NO_RESPONSE;
            } else if (Cmd == ISO14443A_CMD_SELECT_CL1) {
                /* Load UID CL1 and perform anticollision. Since
                * MF Ultralight use a double-sized UID, the first byte
                * of CL1 has to be the cascade-tag byte. */
                uint8_t UidCL1[ISO14443A_CL_UID_SIZE] = { [0] = ISO14443A_UID0_CT };

                AppCardMemoryRead(&UidCL1[1], UID_CL1_ADDRESS, UID_CL1_SIZE);

                if (ISO14443ASelect(Buffer, &BitCount, UidCL1, SAK_CL1_VALUE)) {
                    /* CL1 stage has ended successfully */
                    State = STATE_READY2;
                }

                return BitCount;
            } else {
                /* Unknown command. Enter halt state */
                State = STATE_IDLE;
            }
            break;

        case STATE_READY2:
            if (ISO14443AWakeUp(Buffer, &BitCount, ATQA_VALUE, FromHalt)) {
                State = FromHalt ? STATE_HALT : STATE_IDLE;
                return ISO14443A_APP_NO_RESPONSE;
            } else if (Cmd == ISO14443A_CMD_SELECT_CL2) {
                /* Load UID CL2 and perform anticollision */
                uint8_t UidCL2[ISO14443A_CL_UID_SIZE];

                AppCardMemoryRead(UidCL2, UID_CL2_ADDRESS, UID_CL2_SIZE);

                if (ISO14443ASelect(Buffer, &BitCount, UidCL2, SAK_CL2_VALUE)) {
                    /* CL2 stage has ended successfully. This means
                    * our complete UID has been sent to the reader. */
                    State = STATE_ACTIVE;
                }

                return BitCount;
            } else {
                /* Unknown command. Enter halt state */
                State = STATE_IDLE;
            }
            break;

        //Only ACTIVE state, no AUTHENTICATED state, PWD_AUTH is handled in commands.
        case STATE_ACTIVE:
            /* Preserve incoming data length */
            ByteCount = (BitCount + 7) >> 3;
            if (ISO14443AWakeUp(Buffer, &BitCount, ATQA_VALUE, FromHalt)) {
                State = FromHalt ? STATE_HALT : STATE_IDLE;
                return ISO14443A_APP_NO_RESPONSE;
            }
            /* At the very least, there should be 3 bytes in the buffer. */
            if (ByteCount < (1 + ISO14443A_CRCA_SIZE)) {
                State = STATE_IDLE;
                return ISO14443A_APP_NO_RESPONSE;
            }
            /* All commands here have CRCA appended; verify it right away */
            ByteCount -= 2;
            if (!ISO14443ACheckCRCA(Buffer, ByteCount)) {
                Buffer[0] = NAK_CRC_ERROR;
                return NAK_FRAME_SIZE;
            }
            return AppProcess(Buffer, ByteCount);

        default:
            /* Unknown state? Should never happen. */
            break;
    }

    /* No response has been sent, when we reach here */
    return ISO14443A_APP_NO_RESPONSE;
}
