/*
 * tasks.c
 * FreeRTOS tasks running in parallel (`threads`)

 * This file mostly contains Functions for the communicating
 * with the host PC, which includes parsing and executing commands.
 *
 *  Created on: Dec 22, 2015
 *      Author: michael
 */

#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "inc/hw_ssi.h"

// FreeRTOS includes
#include "FreeRTOSConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// TivaWare includes
#include "driverlib/i2c.h"
#include "driverlib/ssi.h"
#include "driverlib/pin_map.h"
#include "driverlib/gpio.h"
#include "driverlib/debug.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"
#include "driverlib/sysctl.h"
#include "utils/i2cm_drv.h"
#include "utils/ustdlib.h"
#include "utils/uartstdio.h"
#include "utils/cmdline.h"

// USB stuff
#include "usblib/usblib.h"
#include "usblib/usbcdc.h"
#include "usblib/device/usbdevice.h"
#include "usblib/device/usbdcdc.h"
#include "usb_serial_structs.h"
#include "myTasks.h"
#include "mySpi.h"
#include "i2cHandlerTask.h"

//*****************************************************************************
// Global vars.
//*****************************************************************************
// Stuff for synchronizing TI I2C driver with freeRtos `do custom I2C transaction` task
TaskHandle_t g_customI2cTask = NULL;
SemaphoreHandle_t g_SemaCustomI2C = NULL;
uint16_t g_customI2CnBytesRx;
uint8_t g_customI2CrxBuffer[CUSTOM_I2C_BUF_LEN];
uint8_t g_customI2Cstate;
uint8_t g_reportSwitchEvents = 0;

//*****************************************************************************
// Command parser
//*****************************************************************************
// This is the table that holds the command names, implementing functions, and brief description.
tCmdLineEntry g_psCmdTable[] = {
        { "?",     Cmd_help, ": Display list of commands" },
        { "*IDN?", Cmd_IDN,  ": Display ID and version info" },
        { "DISC",  Cmd_DISC, ": Discover PCF8574 GPIO expanders on I2C busses"},
        { "SWE",   Cmd_SWE,  ": <OnOff> En./Dis. reporting of switch events" },
        { "DEB",   Cmd_DEB,  ": <hwIndex> <OnOff> En./Dis. 12 ms debouncing" },
        { "SW?",   Cmd_SW,   ": Return the state of ALL switches (40 bytes)" },
        //{ "PCFIN", Cmd_PCFIN,": <hwIndex> Config. PCF8574 pin as input (HIGH)" },
        { "OUT",   Cmd_OUT,  ": <hwIndex> <PWMlow> [tPulse] [PWMhigh]" },
        { "RUL",   Cmd_RUL,  ": <ID> <IDin> <IDout> <trHoldOff>\n        <tPulse> <pwmOn> <pwmOff> <bPosEdge>" },
        { "RULE",  Cmd_RULE, ": En./Dis a prev. def. rule: RULE <ID> <OnOff>" },
        { "LEC",   Cmd_LEC,  ": <channel> <spiSpeed [Hz]> [frameFmt]" },
        { "LED",   Cmd_LED,  ": <channel> <nBytes>\\n<binary blob of nBytes>" },
        { "I2C",   Cmd_I2C,  ": <channel> <I2Caddr> <sendData> <nBytesRx>" },
        { 0, 0, 0 } };


//*****************************************************************************
// Functions
//*****************************************************************************

uint16_t strMyStrip(uint8_t *cmdString, uint16_t cmdLen) {
    //Remove \n, \r and make sure there is a \0 at the end
    uint8_t *pos = cmdString;
    uint16_t i;
    for (i = 0; i < cmdLen; i++) {
        if (*pos == '\n' || *pos == '\r' || *pos == '\0') {
            *pos = '\0';
            return (i);
        }
        pos++;
    }
    *pos = '\0';
    return (i);
}

void taskDemoLED(void *pvParameters) {
// Flash the LEDs on the launchpad
//    static char debugBuffer[256];
//    uint16_t i;
    // Set up the UART which is connected to the virtual debugging COM port
    UARTStdioConfig(0, 115200, SYSTEM_CLOCK);
    UARTprintf("\n\n\n\n"
            "**************************************************\n"
            " Hi, here's the brain of Fan-Tas-Tic Pinball \n"
            "**************************************************\n");
    UARTprintf("%22s: %s", "taskDemoLED()", "Started!\n");
    while (1) {
        // Turn on LED 1
        ledOut( 1 );
        vTaskDelay(2);
        ledOut( 0 );
        vTaskDelay(1000);
        // Turn on LED 2
        ledOut( 2 );
        vTaskDelay(1);
        ledOut( 0 );
        vTaskDelay(1000);
        // Turn on LED 1+2
        ledOut( 3 );
        vTaskDelay(1);
        ledOut( 0 );
        vTaskDelay(1000);
//        vTaskGetRunTimeStats( debugBuffer );
//        UARTprintf( "\033[2J%s", debugBuffer );
//        if( xSemaphoreTake( g_spiState[0].semaToReleaseWhenFinished, 30 ) ){
//            for( i=0 ; i<50*3; i++ ){
//                g_spiBuffer[0][i] = i;
//            }
//            spiSend( 0, 50*3 );
//        }
    }
}

int8_t cmdParse( uint8_t *charBuffer, uint16_t nCharsRead ){
    if (nCharsRead == 0){   //This must be a \n, ignore silently
        return( 0 );
    }
    int8_t retVal = CmdLineProcess( (char*)charBuffer, nCharsRead );
    switch (retVal) {
     case CMDLINE_BAD_CMD:
         UARTprintf("[CMDLINE_BAD_CMD] %s\n", charBuffer);
         break;
     case CMDLINE_INVALID_ARG:
         UARTprintf("[CMDLINE_INVALID_ARG] %s\n", charBuffer);
         break;
     case CMDLINE_TOO_FEW_ARGS:
         UARTprintf("[CMDLINE_TOO_FEW_ARGS] %s\n", charBuffer);
         break;
     case CMDLINE_TOO_MANY_ARGS:
         UARTprintf("[CMDLINE_TOO_MANY_ARGS] %s\n", charBuffer);
         break;
    }
    return retVal;
}

int16_t eolSearch( uint8_t *charPointer, uint16_t nChars ){
//  Return -1 if no eol has been found, otherwise return offset to eol
    uint16_t i;
    for( i=0; i<nChars; i++ ){          // Find the end of line in received chars
        if( *charPointer=='\n' || *charPointer=='\r' || *charPointer=='\0' ){
            *charPointer = '\0';        //Replace EOL with \0
            return i;
        }
        charPointer++;
    }
    return -1;
}

uint32_t min( uint32_t a, uint32_t b ){
    if( b < a ){
        return b;
    }
    return a;
}

typedef enum{
    PARS_MODE_ASCII, PARS_MODE_BIN_LED
}t_usbParserMode;

uint32_t g_LEDnBytesToCopy;
int8_t g_LEDChannel;

void taskUsbCommandParser( void *pvParameters ) {
    // Read data from USB serial and parse it
    UARTprintf("%22s: %s", "taskUsbCommandParser()", "Started!\n");
    static uint8_t charBuffer[CMD_PARSER_BUF_LEN];
//    uint32_t minStackSpace;
    uint32_t nCharsRead=0, tempCharsRead=0, remainderSize=0;
    int16_t retVal;
    uint8_t *writePointer = charBuffer;                         // Points to first free place in charBuffer
    uint8_t *readPointer = charBuffer;                          // Points to the next unprocessed character
    uint8_t *spiWritePointer;                                   // Points to first free place in spiBuffer
    t_usbParserMode currentMode = PARS_MODE_ASCII;
    while (1) {
        switch( currentMode ){
        case PARS_MODE_ASCII:
            // -----------------------------------------------------------
            //  Check if there is any data in the buffer to process
            // -----------------------------------------------------------
            if( remainderSize == 0 ){                           // There's no unprocessed data in the buffer
                while( !USBBufferDataAvailable(&g_sRxBuffer) ){
                    ulTaskNotifyTake( pdTRUE, portMAX_DELAY);   // Wait for receiving new serial data over USB
                }                                               // Here we must have new data in any case
                tempCharsRead = USBBufferRead(&g_sRxBuffer, writePointer, CMD_PARSER_BUF_LEN-nCharsRead-1 );
//                UARTwrite( writePointer, tempCharsRead );
                writePointer += tempCharsRead;
                remainderSize += tempCharsRead;                 // How many chars to process
            }
            // -----------------------------------------------------------
            //  Wait for a EOL character, then parse the substring
            // -----------------------------------------------------------
            retVal = eolSearch( readPointer, remainderSize );
            if( retVal > -1 ){                                              //EOL was found in new chars
                readPointer += retVal + 1;                                  //Points to the first remainder char after the found EOL char
                remainderSize -= retVal + 1;                                //Length of the remainder after the CMD
                //Parse cmd from beginning of charBuffer
                if( cmdParse( charBuffer, nCharsRead + retVal ) == PARS_MODE_BIN_LED ){
                    // cmdParse might enable PARS_MODE_BIN_LED
                    // cmdParse calls CMD_LED, which will wait until a previous send on this channel is finished
                    // -----------------------------------------------------------
                    //  Switch to binary input mode
                    // -----------------------------------------------------------
                    currentMode = PARS_MODE_BIN_LED;
                    //Okay we need to copy the remainder from the charBuffer into the SPI buffer
                    nCharsRead = min(remainderSize,g_LEDnBytesToCopy);      //How many chars to copy? remainder might contain more commands!
                    memcpy( g_spiBuffer[ g_LEDChannel ], readPointer, nCharsRead );
                    spiWritePointer= &g_spiBuffer[g_LEDChannel][nCharsRead];//Points to next free char in spiBuffer
                    readPointer += nCharsRead;
                    remainderSize -= nCharsRead;                            //This is the remainder after taking the LED data out
                                                                            //Might not be zero if multiple commands are in charBuffer!
                } else {
                    nCharsRead = 0;                                         //We've processed one command, start again with the next!
                }
                // -----------------------------------------------------------
                //  Prepare parsing of the next Ascii cmd
                // -----------------------------------------------------------
                //Okay we need to copy the remainder from the charBuffer into the beginning of the charBuffer
                memcpy( charBuffer, readPointer, remainderSize );       //Remainder is now in the beginning
                // At this point, either all chars are processed (reminderSize=0)
                // Or the remaining chars have been copied to the beginning of the charBuffer
                readPointer = charBuffer;
                writePointer = &charBuffer[remainderSize];              //Point to first free char

            } else {
                // -----------------------------------------------------------
                //  Here we have processed all chars in charBuffer and haven't
                //  Found any EOL. Keep appending new stuff to it.
                //  Also Check how many chars we can still append
                // -----------------------------------------------------------
                nCharsRead += remainderSize;
                readPointer += remainderSize;
                remainderSize = 0;
                if( nCharsRead >= CMD_PARSER_BUF_LEN - 1 ){
                    UARTprintf("%22s: %s", "taskUsbCommandParser()", "Command buffer overflow, try a shorter command!\n");
                    ASSERT(0);
//                    USBBufferFlush( &g_sRxBuffer );
//                    writePointer = charBuffer;
//                    nCharsRead = 0;
//                    remainderSize = 0;
                }                                       //We still have space!
//                UARTprintf("%22s: nCharsRead = %d\n", "taskUsbCommandParser()", nCharsRead);
            }
            break;

        case PARS_MODE_BIN_LED:
            // -----------------------------------------------------------
            //  simply take g_LEDnBytesToCopy bytes from USB and put them
            //  in the spiBuffer
            // -----------------------------------------------------------
            if( nCharsRead < g_LEDnBytesToCopy ){       //If there was not enough data in the remainder, get more over USB
                ASSERT( remainderSize == 0 );           //Remainder must be empty before we take data from USB
                tempCharsRead = USBBufferRead(&g_sRxBuffer, spiWritePointer, g_LEDnBytesToCopy-nCharsRead );
                spiWritePointer += tempCharsRead;
                nCharsRead += tempCharsRead;
            }
            if( nCharsRead >= g_LEDnBytesToCopy ){      //Are we good already?
                spiSend( g_LEDChannel, g_LEDnBytesToCopy );
//                xSemaphoreGive( g_spiState[g_LEDChannel].semaToReleaseWhenFinished );
                nCharsRead = 0;
                currentMode = PARS_MODE_ASCII;
            }
            break;

        default:
            UARTprintf("%22s: Unknown currentMode!\n", "taskUsbCommandParser()");
        }
    }
}

void ts_usbSend(uint8_t *data, uint16_t len) {
//    Do a thread safe USB TX transfer in background (add data to USB send buffer)
    uint32_t freeSpace;
    taskENTER_CRITICAL();
    UARTwrite( (const char*) data, len );   //Echo to debug connection
    freeSpace = USBBufferSpaceAvailable(&g_sTxBuffer);
//        UARTprintf( "ts_usbSend(): TX buffer %d bytes free.\n", freeSpace );
    if (freeSpace >= len) {
        USBBufferWrite(&g_sTxBuffer, data, len);
    } else {
//        UARTprintf("%22s: Not enough space in USB TX buffer! Need %d have %d. <FLUSH>\n",
//                "ts_usbSend()", len, freeSpace);
        USBBufferFlush( &g_sTxBuffer );
    }
    taskEXIT_CRITICAL();
}

// This function implements the "help" command.  It prints a simple list of the available commands with a brief description.
int Cmd_help(int argc, char *argv[]) {
    tCmdLineEntry *pEntry;
    UARTprintf("\n**************************************************\n");
    UARTprintf(  " Available commands   <required>  [optional]\n");
    UARTprintf(  "**************************************************\n");
    pEntry = &g_psCmdTable[0];    // Point at the beginning of the command table.
    // Enter a loop to read each entry from the command table.  The end of the
    // table has been reached when the command name is NULL.
    while (pEntry->pcCmd) {
        UARTprintf("%6s%s\n", pEntry->pcCmd, pEntry->pcHelp);// Print the command name and the brief description.
        pEntry++;                    // Advance to the next entry in the table.
#ifdef UART_BUFFERED
        while( UARTTxBytesFree() < 150 ){
            vTaskDelay( 1 );
        }
#endif
    }
    return (0);                                                // Return success.
}

int Cmd_IDN(int argc, char *argv[]) {
    const uint8_t buff[] = VERSION_IDN;
    ts_usbSend( (uint8_t*)buff, VERSION_IDN_LEN );
    UARTprintf( VERSION_INFO );
    return (0);
}

int Cmd_DISC(int argc, char *argv[]){
    g_reDiscover = 1;                                      // Flag Rescann all I2C inputs
    return(0);
}

int Cmd_SW(int argc, char *argv[]) {
    // Report state of all switches
    static char outBuffer[REPORT_SWITCH_BUF_SIZE];
    uint16_t charsWritten = 3;
    uint8_t i;
    ustrncpy(outBuffer, "SW:", REPORT_SWITCH_BUF_SIZE); //SW = Hex coded switch state
    for (i = 0; i < N_LONGS; i++) {
        charsWritten += usnprintf( &outBuffer[charsWritten], REPORT_SWITCH_BUF_SIZE - charsWritten, "%08x", g_SwitchStateDebounced.longValues[i] );
        if (charsWritten >= REPORT_SWITCH_BUF_SIZE - 1) {
            UARTprintf("Cmd_SW(): string buffer overflow!\n");
            return (0);
        }
    }
    outBuffer[ charsWritten ] = '\n';
    ts_usbSend((uint8_t*) outBuffer, charsWritten+1 );
    return (0);
}

int Cmd_DEB(int argc, char *argv[]) {
    //Enable / Disable the 12 ms debouncing timer for an input
    uint16_t hwIndex;
    uint8_t onOff;
    t_outputBit inputSwitchId;
    if (argc == 3) {
        hwIndex = ustrtoul(argv[1], NULL, 0);
        onOff = ustrtoul(argv[2], NULL, 0);     // 1: Debouncing ON
        inputSwitchId = decodeHwIndex( hwIndex, 1 );
        if ( inputSwitchId.hwIndexType==HW_INDEX_INVALID ) {
            UARTprintf( "%22s: inputSwitchId = %s invalid\n", "Cmd_DEB()", argv[1] );
            return 0;
        }
        if( onOff ){
            // Reset noDebounce Flag
            HWREGBITB( &g_SwitchStateNoDebounce.charValues[inputSwitchId.byteIndex], inputSwitchId.pinIndex ) = 0;
        } else {
            // Set noDebounce Flag
            HWREGBITB( &g_SwitchStateNoDebounce.charValues[inputSwitchId.byteIndex], inputSwitchId.pinIndex ) = 1;
        }
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}


int Cmd_OUT(int argc, char *argv[]) {
//    OUT <hwIndex> <PWMlow> <tPulse> <PWMhigh>   or OUT <hwIndex> <PWMvalue>
//    OUT 0x0FE 1 1500 15
//    OUT 0x0FE 2
    int32_t hwIndex;
    t_outputBit outLocation;
    uint16_t tPulse, pwmHigh, pwmLow;
    if (argc == 5) {
        tPulse  = ustrtoul(argv[3], NULL, 0);
        pwmHigh = ustrtoul(argv[4], NULL, 0);
        pwmLow  = ustrtoul(argv[2], NULL, 0);
    } else if (argc == 3) {
        tPulse  = 0;
        pwmLow  = ustrtoul(argv[2], NULL, 0);
        pwmHigh = pwmLow;
    } else {
        return( CMDLINE_TOO_FEW_ARGS );
    }
    hwIndex = ustrtoul(argv[1], NULL, 0);
    outLocation = decodeHwIndex( hwIndex, 0 );
    switch( outLocation.hwIndexType ){
    case HW_INDEX_I2C:
        if (pwmHigh >= (1 << N_BIT_PWM) || pwmLow >= (1 << N_BIT_PWM)) {
            UARTprintf("Cmd_OUT(): I2C PWMvalue must be < %d\n", (1 << N_BIT_PWM));
            return 0;
        }
        UARTprintf("Cmd_OUT(): i2cCh %d, i2cAdr 0x%02x, bit %d = tp %d, pH %d, pL %d\n", outLocation.i2cChannel, outLocation.i2cAddress, outLocation.pinIndex, tPulse, pwmHigh, pwmLow);
        setPCFOutput(outLocation, tPulse, pwmHigh, pwmLow);
        return(0);

    case HW_INDEX_HWPWM:
        if ( pwmHigh > MAX_PWM || pwmLow > MAX_PWM ) {
            UARTprintf("Cmd_OUT(): HW PWMvalue must be <= %d\n", MAX_PWM);
            return(0);
        }
        UARTprintf("Cmd_OUT(): HW_PWM_CH %d, tp %d, pH %d, pL %d\n", outLocation.pinIndex, tPulse, pwmHigh, pwmLow);
        setPCFOutput(outLocation, tPulse, pwmHigh, pwmLow);
        return(0);

    case HW_INDEX_SWM:
        UARTprintf("Cmd_OUT(): HW_INDEX_INVALID: %s\n", argv[1]);
        return (0);

    case HW_INDEX_INVALID:
        UARTprintf("Cmd_OUT(): HW_INDEX_INVALID: %s\n", argv[1]);
        return (0);

    default:
        ASSERT(0);
    }
    return (0);
}

int Cmd_SWE(int argc, char *argv[]) {
    //Enable / Disable the reporting of Switch Events
    uint8_t onOff;
    if (argc == 2) {
        onOff = ustrtoul(argv[1], NULL, 0);
        if( onOff ){
            g_reportSwitchEvents = 1;
        } else {
            g_reportSwitchEvents = 0;
        }
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}

int Cmd_RULE(int argc, char *argv[]) {
    //Enable / Disable a quickfire rule
    uint8_t id, onOff;
    if (argc == 3) {
        id = ustrtoul(argv[1], NULL, 0);
        onOff = ustrtoul(argv[2], NULL, 0);
        if (id >= MAX_QUICK_RULES) {
            UARTprintf("%22s: quickRuleId must be < %d\n", "Cmd_RULE()",
                    MAX_QUICK_RULES);
            return 0;
        }
        if( onOff ){
            enableQuickRule(id);
        } else {
            disableQuickRule(id);
        }
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}

int Cmd_RUL(int argc, char *argv[]) {
// Configure and activate a Quick-fire rule:
//  * quickRuleId (0-64)
//  * input switch ID number
//  * driver output ID number
//  * post trigger hold-off time [ms]
//  * pulse duration [ms]
//  * pulse pwm [only for output ID 0-3 which are the pwm channels]
//  * hold pwm  [only for output ID 0-3 which are the pwm channels]
//  * Enable trigger on pos edge?
//  ------------------
//   Example command:
//  ------------------
//  RUL ID IDin IDout trHoldOff tPulse pwmOn pwmOff bPosEdge
//  RUL 0 0x23 0x100 4 1 15 3 1
    uint8_t id;
    int16_t pwmHigh, pwmLow;
    uint16_t triggerHoldOffTime, tPulse, hwIndex;
    t_outputBit inputSwitchId, outputDriverId;
    bool trigPosEdge;
    if (argc == 9) {
        id = ustrtoul(argv[1], NULL, 0);
        triggerHoldOffTime = ustrtoul(argv[4], NULL, 0);
        tPulse = ustrtoul(argv[5], NULL, 0);
        pwmHigh = ustrtoul(argv[6], NULL, 0);
        pwmLow = ustrtoul(argv[7], NULL, 0);
        trigPosEdge = ustrtoul(argv[8], NULL, 0) == 1;
        if (id >= MAX_QUICK_RULES) {
            UARTprintf("%22s: quickRuleId must be < %d\n", "Cmd_RUL()",
                    MAX_QUICK_RULES);
            return 0;
        }
        inputSwitchId = decodeHwIndex( ustrtoul(argv[2], NULL, 0), 1 );
        if ( inputSwitchId.hwIndexType==HW_INDEX_INVALID ) {
            UARTprintf( "%22s: inputSwitchId = %s invalid\n", "Cmd_RUL()", argv[2] );
            return 0;
        }
        hwIndex = ustrtoul(argv[3], NULL, 0);
        outputDriverId = decodeHwIndex( hwIndex, 0 );
        switch( outputDriverId.hwIndexType ){
        case HW_INDEX_I2C:
            if ( pwmHigh >= (1 << N_BIT_PWM) || pwmLow >= (1 << N_BIT_PWM) ) {
                UARTprintf( "%22s: pwmValues must be < %d\n", "Cmd_RUL()", (1 << N_BIT_PWM) );
                return 0;
            }
            break;
        case HW_INDEX_HWPWM:
            if ( pwmHigh > MAX_PWM || pwmLow > MAX_PWM ) {
                UARTprintf( "%22s: HW pwmValues must be < %d\n", "Cmd_RUL()", MAX_PWM );
                return(0);
            }
            break;
        case HW_INDEX_INVALID:
        case HW_INDEX_SWM:
            UARTprintf( "%22s: outputDriverId = %s invalid\n", "Cmd_RUL()", argv[3] );
            return 0;
        }
        UARTprintf("%22s: Setting up autofiring rule %d\n", "Cmd_RUL()", id);
        setupQuickRule( id, inputSwitchId, outputDriverId, triggerHoldOffTime,
                tPulse, pwmHigh, pwmLow, trigPosEdge );
        return (0);
    }
    return CMDLINE_TOO_FEW_ARGS;
}

int Cmd_LEC(int argc, char *argv[]) {   //Here we need re - initialize the SPI module
    //LEC <channel> <spiSpeed [Hz]> <SSI_FRF (opt)>"
    uint8_t channel;
    uint32_t baseAddr, spiSpeed, frameFmt=SSI_FRF_MOTO_MODE_1;
    if( argc==3 || argc==4 ){
        channel = ustrtoul(argv[1], NULL, 0);
        if( channel<=2 ){
            baseAddr = g_spiState[channel].baseAdr;
        } else {
            UARTprintf("%22s: Invalid LED channel (%d)\n", "Cmd_LEC()", channel);
            return(0);
        }
        spiSpeed = ustrtoul(argv[2], NULL, 0);
        if( argc == 4 ){
            frameFmt = ustrtoul(argv[3], NULL, 0);
        }
        while( HWREG( baseAddr + SSI_O_SR ) & SSI_SR_BSY ){  //Wait for SPI to finish
            vTaskDelay( 10 );
        }
        UARTprintf("%22s: Setting SPI to %d Hz, frameFmt = 0x%02x\n", "Cmd_LEC()", spiSpeed, frameFmt);
        ROM_SSIDisable( baseAddr );
        // SPI at 3.2 MHz, 16 bit SPI words (encoding 4 bit data each)
        ROM_SSIConfigSetExpClk( baseAddr, SYSTEM_CLOCK, frameFmt, SSI_MODE_MASTER, spiSpeed, 16 );
        ROM_SSIEnable( baseAddr );
    } else {
        return( CMDLINE_TOO_FEW_ARGS );
    }

    return(0);
}


int Cmd_LED(int argc, char *argv[]) {   //Here we need to switch the serialCommandParser to Binary mode
    //LED 0 128\nxxxxxx
    uint8_t channel;
    uint32_t blobSize;
    if( argc==3 ){
        channel = ustrtoul(argv[1], NULL, 0);
        if( channel <= 2 ){
            blobSize = ustrtoul(argv[2], NULL, 0);
            if( blobSize%3 || blobSize>N_LEDS_MAX*3 ){
                UARTprintf("%22s: Invalid number of bytes (%d)\n", "Cmd_LED()", blobSize);
                return 0;
            }
//            UARTprintf("Blasting %d bytes of data to LED string on channel %d\n", blobSize, channel);
            //-------------------------
            // Lock the SPI channel
            //-------------------------
            t_spiTransferState *state = &g_spiState[channel];
            if( xSemaphoreTake( state->semaToReleaseWhenFinished, 1000 ) ){
                g_LEDnBytesToCopy = blobSize;   //We got the Lock, start copy process
                g_LEDChannel = channel;
                return PARS_MODE_BIN_LED;
            } else {
                UARTprintf("%22s: Timeout, could not access sendBuffer %d\n", "spiSend()", channel);
            }
        } else {
            UARTprintf("%22s: Invalid LED channel (%d)\n", "Cmd_LED()", channel);
        }
    } else {
        UARTprintf("%22s: Invalid number of arguments (%d)\n", "Cmd_LED()", argc);
    }
    return( 0 );
}

uint8_t hexDigitToNibble( uint8_t hexChar ){
    if( hexChar >= '0' && hexChar <= '9' ){
        return( hexChar - '0' );
    } else if ( hexChar >= 'a' && hexChar <= 'f' ){
        return( hexChar - 'a' + 10 );
    } else if ( hexChar >= 'A' && hexChar <= 'F' ){
        return( hexChar - 'A' + 10 );
    }
    return( 0 );
}

void taskI2CCustomReporter(void *pvParameters) {
    // When a custom I2C transaction is finished, report the result to commandline
    static uint8_t outBuffer[CUSTOM_I2C_BUF_LEN*2+5];
    uint8_t *writePointer, *readPointer;
    uint16_t i;
    g_customI2cTask = xTaskGetCurrentTaskHandle();
    g_SemaCustomI2C = xSemaphoreCreateBinary();
    xSemaphoreGive( g_SemaCustomI2C );
    UARTprintf("%22s: %s", "taskI2CustomReporter()", "Started!\n");
    while( 1 ){
        if( ulTaskNotifyTake( pdTRUE, portMAX_DELAY) ) {    // Wait for notification of i2c transaction finished
            writePointer = outBuffer;
            readPointer = g_customI2CrxBuffer;
            // Report result
            writePointer += usprintf( (char*)writePointer, "I2C:" );
            switch( g_customI2Cstate ){
            case I2CM_STATUS_SUCCESS:
                for( i=0; i<g_customI2CnBytesRx; i++ ){
                    writePointer += usprintf( (char*)writePointer, "%02X", *readPointer++ );
                }
            break;
            case I2CM_STATUS_ADDR_NACK:
                writePointer += usprintf( (char*)writePointer, "I2CM_STATUS_ADDR_NACK" );
            break;
            case I2CM_STATUS_DATA_NACK:
                writePointer += usprintf( (char*)writePointer, "I2CM_STATUS_DATA_NACK" );
            break;
            case I2CM_STATUS_ARB_LOST:
                writePointer += usprintf( (char*)writePointer, "I2CM_STATUS_ARB_LOST" );
            break;
            case I2CM_STATUS_ERROR:
                writePointer += usprintf( (char*)writePointer, "I2CM_STATUS_ERROR" );
            break;
            }
            writePointer += usprintf( (char*)writePointer, "\n" );
            ts_usbSend( outBuffer, ustrlen((char*)outBuffer) );
            xSemaphoreGive( g_SemaCustomI2C ); // release the Binary Smeaphore to allow another custom I2C command
        }
    }
}

void cmdI2CCallback(void* pvCallbackData, uint_fast8_t ui8Status){
//   This is called from ISR context, so dont do anything ambitious here
//   Notify taskI2CCustomReporter task to report results
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    g_customI2Cstate = ui8Status;
    vTaskNotifyGiveFromISR( g_customI2cTask, &xHigherPriorityTaskWoken );
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

int Cmd_I2C(int argc, char *argv[]) {
    //I2C <channel> <I2Caddr> <sendData> <nBytesRx>
    static uint8_t txBuffer[CUSTOM_I2C_BUF_LEN];
    uint8_t channel, i2cAddr, *readPointer, *writePointer;
    uint16_t nBytesTx, temp;
    if ( argc == 5 ){
        channel  = ustrtoul(argv[1], NULL, 0);
        if ( channel > 3 ){
            UARTprintf("%22s: I2C Channel must be <= 4\n", "Cmd_I2C()");
            return 0;
        }
        i2cAddr  = ustrtoul(argv[2], NULL, 0);
        g_customI2CnBytesRx = ustrtoul(argv[4], NULL, 0);
        nBytesTx = ustrlen( argv[3] )/2;      //argv[3] string contains hex characters [0FFEDEADBEEF]
        writePointer = txBuffer;
        readPointer = (uint8_t*)argv[3];
        if( g_customI2CnBytesRx > CUSTOM_I2C_BUF_LEN ){
            UARTprintf("%22s: Too many bytes to receive: %d, max. %d\n", "Cmd_I2C()", g_customI2CnBytesRx, CUSTOM_I2C_BUF_LEN);
            return 0;
        }
        if( nBytesTx > CUSTOM_I2C_BUF_LEN ){
            UARTprintf("%22s: Too many bytes to send: %d, max. %d\n", "Cmd_I2C()", nBytesTx, CUSTOM_I2C_BUF_LEN);
            return 0;
        }
        for( temp=0; temp<nBytesTx; temp++ ){
            *writePointer  = hexDigitToNibble( *readPointer++ )<<4;
            *writePointer |= hexDigitToNibble( *readPointer++ );
            writePointer++;
        }
        // Take the binary semaphore g_semaCustomI2C (released after reporting result on USB)
        if( xSemaphoreTake( g_SemaCustomI2C, 3000 ) ){
            ts_i2cTransfer( channel, i2cAddr, txBuffer, nBytesTx, g_customI2CrxBuffer, g_customI2CnBytesRx, cmdI2CCallback, NULL );
        } else {
            UARTprintf("%22s: Timeout, could not acquire custom I2C Semaphore\n", "Cmd_I2C()");
        }
        return 0;
    }
    return CMDLINE_TOO_FEW_ARGS;
}
