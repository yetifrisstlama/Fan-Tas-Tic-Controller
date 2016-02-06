# Fan-Tas-Tic-Controller
Controller for Pinball machines based on an TM4C123G LaunchPad™ Evaluation Kit, compatible with the [Mission Pinball API](https://missionpinball.com/).

## Hardware features
 * 8x8 Switch matrix inputs
 * 12 onboard drivers for solenoids, 4 of them can do hardware PWM (> 100 kHz)
 * 4 x I2C channels for extension boards (solenoid drivers, switch inputs, LED drivers, servo drivers, ...)
 * 3 x SPI channels for running [WS2811 / WS2812 LED strings](http://www.ebay.com/sch/i.html?_from=R40&_trksid=p2050601.m570.l1313.TR0.TRC0.H0.Xled+strand+ws2811.TRS5&_nkw=led+strand+ws2811&_sacat=0) with up to 1024 LEDs per channel
 * In- / Outputs can be easily and cheaply added with [PCF8574](http://www.ti.com/product/pcf8574) I2C GPIO extenders (check eBay for [cheap I/O modules](http://www.ebay.com/sch/i.html?_sacat=0&_nkw=i2c+expander&_frs=1))
 * Super fast USB virtual serial connection to host PC
 * KiCad PCB files available, no tiny SMD components, can be assembled by hand

## Software features 
 * Software can handle up to 320 channels which can be used as In- or Outputs
 * All In- / Outputs are identified by a 16 bit unique ID. No configuration necessary.
 * All Outputs support 4 bit PWM with > 125 Hz (using [binary code modulation](http://www.batsocks.co.uk/readme/art_bcm_1.htm))
 * All inputs are debounced and read 333 times per second. A switch toggles after keeping its state for 4 ticks (12 ms).
 * The timing for the WS2811 LEDs is strictly within spec by using the hardware SPI module and DMA transfers.
 * Software easily extendable by running [FreeRTOS](http://www.freertos.org/)

# Digital inputs / outputs

There are 4 I2C channels which can handle up to 8 x PCF8574 each. The addresses of the chips needs to be configured from 0x20 - 0x27.

To use an PCF8574 as an input,  a 0xFF needs to be _written_ to it (to prevent the open drain outputs pulling the pin low). 
Then the pin state can be acquired with a 1 byte read transaction over I2C.

So all I2C extenders (no matter if used as in- or output) can be read in bulk. 
I2C is fast enough to read all 32 of them (on 4 channels in parallel) with 625 Hz repetition rate. 
Reading an empty address will give a NO_ACK read error, which is silently ignored.



## Debouncing

The taskDebouncer() is running at 333 Hz, reading all switches (matrix and I2C) into an array
 * Switch Matrix (SM) is read in foreground while I2C transactions happen in background (TI I2Cm driver)
 * When both reads are finished, the Debouncing routine is called
 * If a changed input bit was detected, it increments a 2 bit [vertical counter](http://www.compuphase.com/electronics/debouncing.htm).
   The ARM CPU can process 32 input bits in a single instruction, which speeds things up!
 * If the change persists for 4 read cycles, the vertical counter of that bit will overflow and trigger a definite change.
 * The current debounced state and any bits which toggled during the current iteration are stored in global variables.
 * The serial reporter function looks for changed bits, encodes them and reports them as `Switch Events` on the serial port
 * The quick-fire rule function checks a list of rules, which can be triggered by changed bits, which can lead to immediate actions, like coils firing.

## `hwIndex` identifies an input / output pin
It is a 16 bit number which uniquely identifies each input / output pin. Note that depending on the number of IO expanders usend in a setup,
not all addresses will be valid. 

         hwIndex for inputs
        ---------------------------------
         0 - 63  --> Switch matrix inputs on mainboard
       [ 64 - 71 --> I2C Solenoid driver on mainboard  NOT AN INPUT! ]
         72 - 79 --> I2Cch. 0, I2Cadr. 0x21, bit 0-7  (First  external PCL GPIO extender on channel 0)
         80 - 87 --> I2Cch. 0, I2Cadr. 0x22, bit 0-7  (Second external PCL GPIO extender on channel 0)
         312-319 --> I2Cch. 3, I2Cadr. 0x27, bit 0-7  (7th    external PCL GPIO extender on channel 3)

### Calculating the hwIndex for the Switch matrix
        hwIndex = SMrow * 8 + SMcol
where SMrow is the row wire number (from 0-7) and SMcol is the column wire number (from 0-7).

### Calculating the hwIndex for I2C inputs
        hwIndex = 64 + I2Cchannel * 64 + ( I2Cadr - 32 ) * 8 + PinIndex
 where I2Cchannel is the output channel on the mainboard (from 0-3), I2Cadr is the configured I2C address
 set by dip switches on the port extender (0x20 - 0x27) and PinIndex is the output pin (0-7).
 
### What about I2C outputs 
Output extension boards (solenoid driver / digital outputs / etc.) also use the PCF8574 I2C chip and hence the same
addresses. The chip automatically switches its pins to output mode once an I2C `write` operation has been carried out. 
So outputs do not need to be specially configured, just fire the OUT command with the known output address.
 
The PC8574 has open drain output drivers and can only sink current to ground. If the output driver is disabled 
(by writing the bit to 1), the pin can be used as an input. If the output driver is enabled 
(by writing the bit to 0) the pin cannot be used as an input as it is constantly pulled low.
  
__!!! To avoid this, the user must take care to not send `OUT` commands on an `hwIndex` which is used as an input !!!__

The mainboard features a built-in PCF8574 to drive 8 solenoids. use hwIndex 64 - 71 to address these channels.

### What about the HW. PWM outputs
The mainboard features 4 high resolution PWM solenoid drivers running at 50 kHz. 
For these, the pwm hold power and pwm pulse power can be specified from 0 - 1500.
The hwIndexes 60 - 63 are mapped in any `OUT` command to the HW. PWM channels.
For any `IN` command, these addresses are mapped to the highest 4 Switch Matrix inputs.

# Serial command API

The Tiva board has two physical USB connectors. The `DEBUG` port is used to load and debug the firmware.
It also provides a virtual serial port, which can be opened in a terminal to enter commands manually and
to see status and debug messages from the Fan-Tas-Tic firmware. 

The `DEVICE` port provides a virtual serial port
which is meant to communicate with the (Python) host application. This port listens to the same commands but does not echo any
input characters, status messages or errors, which makes it easier to talk to programatically.

        **************************************************
         Available commands   <required>  [optional]
        **************************************************
        ?     : Display list of commands
        *IDN? : Display ID and version info
        SW?   : Return the state of ALL switches (40 bytes)
        OUT   : <hwIndex> <PWMlow> [tPulse] [PWMhigh]
        RUL   : <ID> <IDin> <IDout> <trHoldOff> <tPulse>
                <pwmOn> <pwmOff> <bPosEdge> <bAutoOff> <bLevelTr>
        RULE  : Enable  a previously disabled rule: RULE <ID>
        RULD  : Disable a previously defined rule:  RULD <ID>
        LEC   : <channel> <spiSpeed [Hz]> [frameFmt]
        LED   : <channel> <nBytes>\n<binary blob of nBytes>
        I2C   : <channel> <I2Caddr> <sendData> <nBytesRx>


## Switch events
When a switch input flips its state, its hwIndex and new state is immediately reported on the USB serial port

__Example__

The input with hwIndex 0x0F8 changed to 1, 0x0FC changed to 0 and 0x0FE changed to 1
 
Received:

        SE:0f8=1 0fa=1 0fc=0 0fe=1\n

## `SW?` returns the state of all Switch inputs
Returns 40 bytes as 8 digit hex numbers. This encodes all 320 bits which can be addressed by a hwIndex.

__Example__

Sent:

        SW?\n
Received:

        SW:00000000123456789ABCDEF0AFFE0000DEAD0000BEEF0000C0FFEE00000000000000000000000000\n

## `OUT` set a solenoid driver output
 * hwIndex 
 * pwm hold power  (0-15)
 * pulse time in ms  (0 - 32767), optional
 * pwm pulse power (0-15), optional 
 
__Example__

Set output pin with hwIndex 0x100 to a pwm power level of 10 and keep it there.

Sent:

        OUT 0x100 10\n

Pulse output with hwIndex 0x110 for 300 ms with a pwm power level of 10 and then keep it at a power level of 2.

Sent:

        OUT 0x110 2 300 10\n

 
## `RUL` setup and enable a quick-fire rule
 * quickRuleId (0-64)
 * input switch ID number
 * driver output ID number
 * post trigger hold-off time [ms]
 * pulse duration [ms]
 * pulse pwm (0-15)
 * hold pwm  (0-15)
 * Enable trigger on pos edge?
 * Enable auto. output off once input releases
 * Enable level Trigger (no edge check)


### Logic for each rule

        If a rule is enabled:
            If it is currently triggered:
                If holdOff time expired:
                    If OFF_ON_RELEASE flag is set:
                        If input is released:
                            switch output Off
                            set Rule to untriggered state
                    Else:
                        set Rule to untriggered state
                Else:
                    decrement holdOff time
            else:
                Check if the input matches the trigger condition:
                    Set Rule to triggered state
                    switch output ON


### Notes
When enabling level trigger, the edge detecion is disabled and the rule will stay in triggered state
as long as the input is high (or low)
 
When auto. output off is enabled, the rule stays in triggered state as long as the level is high. 
When the level is low again, it disables the outputs and arms the trigger again.

Warning: When auto. output off is disabled and level trigger is enabled it leads to a periodic trigger condition.
Sending trigger events every `triggerHoldOffTime` as long as the level is there (not so good)

__Example__

Sent:

        RUL 0 0x23 0x100 4 1 15 3 1 1 0\n
    
Setup ruleId 0. Input hwIndex is 0x23, output hwIndex is 0x100. After triggering, at least 4 ms need to ellapse
 before the trigger becomes armed again. Once triggered it pulses the output for 1 ms with pwmPower 15, then it
  holds the output with pwmPower 3. The trigger happens on a positive edge. Once the input is released (and at
   least 4 ms ellapsed), the output is switched off again.

## `LED` dump data to WS2811 RGB LED strings
There are 3 channels which can address up to 1024 LEDs each. 
First argument is the channel address (0-2).
Second argument is the number of bytes which will be sent (nBytes) and must be an integer multiple of 3.
Each LED needs to be set with 1 byte per color. For the WS2811 LED chip, the order of the bytes is RGB. 
For performance reasons, data must be sent as raw binary values (not ascii encoded!).

__Example__

Sent:

        LED 1 6\n
        \xFF\xFF\xFF\x7F\x00\x00

Set the first two LEDs on channel 1. The first LED will glow white at full power, the second red at half power.

### Troubleshooting glitches
If you get glitches and artifacts on your LEDs, you can try the following:

    * If using the TI compiler, optimization level must be set to 3 `Interprocedure Optimization`
      and speed vs size tradeoff to `optimize for max. speed`.
      Anything below gives glitches as the DMA buffer underflows. Anything above
      gives deadlocks due to skipped bytes on the USB serial port.
    * Play with the SPI speed setting (`LEC` command). Some LED strings, especially cheap ones from eBay, 
      may significantly deviate from specifications
    * Try shorter cables to the first LED
    * If nothing else helps, have a look at the data stream on a scope

## `LEC` configure the WS2811 data rate
Set the output data-rate of the SPI module in bits / s.
Note that 4 bits are needed to transmit 1 WS2811 `baud`.
The WS2811 chip supports low speed (400 kBaud) and high speed (800 kBaud) mode. This setting is applied by
the voltage level on a physical pin on the chip. The WS2812 LED only supports 800 kBaud mode.
    
    -------------------------
     kBaud     spiSpeed [Hz]
       400   =       1600000
       800   =       3200000
    -------------------------
      
This command allows the timing to be fine-tuned, to remove glitches.

The second argument `frameFmt` is optional and allows to experiment with the SPI frame format. 
Refer to the [TM4C1294 datasheet](http://www.ti.com/lit/gpn/tm4c123gh6pm) for details.

    ---------------------------------------------
     frameFmt    Description
         0x00    Moto fmt, polarity 0, phase 0
         0x02    Moto fmt, polarity 0, phase 1
         0x01    Moto fmt, polarity 1, phase 0
         0x03    Moto fmt, polarity 1, phase 1
         0x10    TI frame format
         0x20    National MicroWire frame format
    ---------------------------------------------

__Example__

Sent:

        LEC 0 1700000\n
        
Set the SPI speed of the first LED channel to 1.7 Mbit/s. 
As 4 SPI bits encode 1 WS2811 clock period, the effective speed is 425 kBaud.         


## `I2C` do a custom I2C transaction
This command does a send / receive transaction on one of the I2C busses. Use this to communicate with custom extension boards from python.

__Example__

Sent:

         I2C 3 0x20 ABCDEF 2\n

Received:

         I2:E3B4\n

Do an I2C transaction on channel 3. The right shifted device address (without R/W bit) is 0x20. Send the 3 bytes of data 0xAB, 0xCD, 0xEF. Then read 2 bytes of data from the device, which are 0xE3 and 0xB4.

