/*

   emonPi  Discrete Sampling

   If AC-AC adapter is detected assume emonPi is also powered from adapter (jumper shorted) and take Real Power Readings and disable sleep mode to keep load on power supply constant
   If AC-AC addapter is not detected assume powering from battereis / USB 5V AC sample is not present so take Apparent Power Readings and enable sleep mode

   Transmitt values via RFM69CW radio

   ------------------------------------------
   Part of the openenergymonitor.org project

   Authors: Glyn Hudson & Trystan Lea
   Builds upon JCW JeeLabs RF12 library and Arduino

   Licence: GNU GPL V3

 */

/*Recommended node ID allocation
   ------------------------------------------------------------------------------------------------------------
   -ID-	-Node Type-
   0	- Special allocation in JeeLib RFM12 driver - reserved for OOK use
   1-4     - Control nodes
   5-10	- Energy monitoring nodes
   11-14	--Un-assigned --
   15-16	- Base Station & logging nodes
   17-30	- Environmental sensing nodes (temperature humidity etc.)
   31	- Special allocation in JeeLib RFM12 driver - Node31 can communicate with nodes on any network group
   -------------------------------------------------------b------------------------------------------------------


   Change Log:
   https://github.com/openenergymonitor/emonpi/blob/master/Atmega328/emonPi_RFM69CW_RF12Demo_DiscreteSampling/compiled/CHANGE%20LOG.md

   emonhub.conf node decoder:

   [[5]]
    nodename = emonPi
    firmware = emonPi_RFM69CW_RF12Demo_DiscreteSampling.ino
    hardware = emonpi
    [[[rx]]]
        names = power1,power2,power1_plus_power2,Vrms,T1,T2,T3,T4,T5,T6,pulseCount
        datacodes = h, h, h, h, h, h, h, h, h, h, L
        scales = 1,1,1,0.01,0.1,0.1,0.1,0.1,0.1,0.1,1
        units = W,W,W,V,C,C,C,C,C,C,p

 */

#define emonTxV3                                                      // Tell emonLib this is the emonPi V3 - don't read Vcc assume Vcc = 3.3V as is always the case on emonPi eliates bandgap error and need for calibration http://harizanov.com/2013/09/thoughts-on-avr-adc-accuracy/
#define RF69_COMPAT 1                                                 // Set to 1 if using RFM69CW or 0 is using RFM12B

#include <JeeLib.h>                                                   // https://github.com/openenergymonitor/jeelib
#include <avr/pgmspace.h>
#include <util/parity.h>
ISR(WDT_vect) {
        Sleepy::watchdogEvent();
}                                                                     // Attached JeeLib sleep function to Atmega328 watchdog -enables MCU to be put into sleep mode inbetween readings to reduce power consumption

#include "EmonLib.h"                                                  // Include EmonLib energy monitoring library https://github.com/openenergymonitor/EmonLib
EnergyMonitor ct1, ct2;

#include <OneWire.h>                                                  // http://www.pjrc.com/teensy/td_libs_OneWire.html
#include <DallasTemperature.h>                                        // http://download.milesburton.com/Arduino/MaximTemperature/DallasTemperature_LATEST.zip
#include <Wire.h>                                                     // Arduino I2C library
#include <EEPROM.h>

#include "config.h"
//----------------------------emonPi Firmware Version---------------------------------------------------------------------------------------------------------------
// Changelog: https://github.com/openenergymonitor/emonpi/blob/master/firmware/readme.md
const int firmware_version = 290; //firmware version x 100 e.g 100 = V1.00

//----------------------------emonPi Settings---------------------------------------------------------------------------------------------------------------
boolean debug =                   FALSE;
const unsigned long BAUD_RATE = 38400;

const unsigned int TIME_BETWEEN_READINGS = 5000; // Time between readings (ms)
const unsigned int RF_RESET_PERIOD = 60000;  // Time (ms) between RF resets (hack to keep RFM60CW alive)

const byte min_pulsewidth=        60;    // minimum width of interrupt pulse

const int no_of_samples=          1480;
const byte no_of_half_wavelengths = 20;
const int timeout=                2000;  // emonLib timeout
const int ACAC_DETECTION_LEVEL=   3000;

const byte TEMPERATURE_PRECISION = 12;  // 9 (93.8ms),10 (187.5ms) ,11 (375ms) or 12 (750ms) bits equal to resplution of 0.5C, 0.25C, 0.125C and 0.0625C
const byte MaxOnewire=             6;  // maximum number of DS18B20 one wire sensors

//-------------------------------------------------------------------------------------------------------------------------------------------

//Setup DS128B20
OneWire oneWire(oneWire_pin);
DallasTemperature sensors(&oneWire);
byte allAddress [MaxOnewire][8];  // 8 bytes per address
byte numSensors = 0;
//-------------------------------------------------------------------------------------------------------------------------------------------

//-----------------------RFM12B / RFM69CW SETTINGS----------------------------------------------------------------------------------------------------
byte RF_freq=RF12_433MHZ;  // Frequency of RF69CW module can be RF12_433MHZ, RF12_868MHZ or RF12_915MHZ. You should use the one matching the module you have.
byte nodeID = 5;           // emonpi node ID
int networkGroup = 210;

typedef struct {
        int power1;
        int power2;
        int power1_plus_2;
        int Vrms;
        int temp[MaxOnewire];
        unsigned long pulseCount;
} PayloadTX;                                                    // create JeeLabs RF packet structure - a neat way of packaging data for RF comms
PayloadTX emonPi;
//-------------------------------------------------------------------------------------------------------------------------------------------

//Global Variables Energy Monitoring
boolean ACAC;
byte CT_count;
unsigned long last_sample=0;                                     // Record millis time of last discrete sample
byte flag;                                                       // flag to record shutdown push button press
volatile byte pulseCount = 0;
unsigned long now =0;
unsigned long pulsetime=0;                                      // Record time of interrupt pulse
unsigned long last_rf_rest=0;                                  // Record time of last RF reset

// RF Global Variables
static byte stack[RF12_MAXDATA+4], top, sendLen, dest;           // RF variables
static char cmd;
static word value;                                               // Used to store serial input
long unsigned int start_press=0;                                 // Record time emonPi shutdown push switch is pressed
boolean quiet_mode = 1;

const char helpText1[] PROGMEM =                                 // Available Serial Commands
                                 "\n"
                                 "Available commands:\n"
                                 "  <nn> i     - set node IDs (standard node ids are 1..30)\n"
                                 "  <n> b      - set MHz band (4 = 433, 8 = 868, 9 = 915)\n"
                                 "  <nnn> g    - set network group (RFM12 only allows 212, 0 = any)\n"
                                 "  <n> c      - set collect mode (advanced, normally 0)\n"
                                 "  ...,<nn> a - send data packet to node <nn>, request ack\n"
                                 "  ...,<nn> s - send data packet to node <nn>, no ack\n"
                                 "  ...,<n> p  - Set AC Adapter Vcal 1p = UK, 2p = USA\n"
                                 "  v          - Show firmware version\n"
                                 "  <n> q      - set quiet mode (1 = don't report bad packets)\n"
;

//-------------------------------------------------------------------------------------------------------------------------------------------
// SETUP ********************************************************************************************
//-------------------------------------------------------------------------------------------------------------------------------------------

// Default configuration.
struct Config config = { DEFAULT_CONFIG };

void config_eeprom_read()
{
        struct Config eeconfig;

        EEPROM.get(0, eeconfig);
        if (eeconfig.version == config.version) {
                config = eeconfig;
        } else {
                config_eeprom_update();
        }

}

void config_eeprom_update()
{
        EEPROM.put(0, config);
}

void setup()
{
        delay(100);
        config_eeprom_read();
        emonPi_startup(); // emonPi startup procedure, check for AC waveform and print out debug
        serial_print_config(&config);

        if (config.rf_enable)
                RF_Setup();
        numSensors = check_for_DS18B20();

        emonPi_LCD_Startup();

        delay(2000);
        CT_Detect();
        serial_print_startup();
        lcd_print_startup();

        attachInterrupt(digitalPinToInterrupt(emonpi_pulse_pin), onPulse, FALLING); // Attach pulse counting interrupt on RJ45 (Dig 3 / INT 1)
        emonPi.pulseCount = 0;                                            // Reset Pulse Count

        ct1.current(1, config.Ical1); // CT ADC channel 1, calibration.
        ct2.current(2, config.Ical2); // CT ADC channel 2, calibration.

        if (ACAC) {                                          //If AC wavefrom has been detected
                ct1.voltage(0, config.Vcal, config.phase_shift); // ADC pin, Calibration, phase_shift
                ct2.voltage(0, config.Vcal, config.phase_shift); // ADC pin, Calibration, phase_shift
        }
} //end setup


//-------------------------------------------------------------------------------------------------------------------------------------------
// LOOP ********************************************************************************************
//-------------------------------------------------------------------------------------------------------------------------------------------
void loop()
{
        now = millis();

        // Update Vcal
        ct1.voltage(0, config.Vcal, config.phase_shift); // ADC pin, Calibration, phase_shift
        ct2.voltage(0, config.Vcal, config.phase_shift); // ADC pin, Calibration, phase_shift

        if (digitalRead(shutdown_switch_pin) == 0 )
                digitalWrite(emonpi_GPIO_pin, HIGH);                              // if emonPi shutdown butten pressed then send signal to the Pi on GPIO 11
        else
                digitalWrite(emonpi_GPIO_pin, LOW);

        if (Serial.available()) {
                handleInput(Serial.read());                                       // If serial input is received
                double_LED_flash();
        }

        if (config.rf_enable) {    // IF RF module is present and enabled then perform RF tasks
                if (RF_Rx_Handle() == 1) { // Returns true if RF packet is received
                        double_LED_flash();
                }

                send_RF();  // Transmitt data packets if needed

                // Periodically reset RFM69CW to keep it alive :-(
                if ((now - last_rf_rest) > RF_RESET_PERIOD) {
                        rf12_initialize(nodeID, RF_freq, networkGroup);
                }
        }

        if ((now - last_sample) > TIME_BETWEEN_READINGS) {
                single_LED_flash();

                // CT1 -------------------------------------------------------
                emonPi.power1 = 0;
                if (analogRead(1) > 0) {  // If CT is plugged in then sample
                        if (ACAC) {
                                ct1.calcVI(no_of_half_wavelengths,timeout);
                                emonPi.power1=ct1.realPower;
                                emonPi.Vrms=ct1.Vrms*100;
                        } else {
                                emonPi.power1 = ct1.calcIrms(no_of_samples) * config.Vrms; // Calculate Apparent Power if no AC-AC
                        }
                }
// CT2 ------------------------------------------------------------------------
                emonPi.power2 = 0;
                if (analogRead(2) > 0) {  // If CT is plugged in then sample
                        if (ACAC) {       // Read from CT 2
                                ct2.calcVI(no_of_half_wavelengths, timeout);
                                emonPi.power2 = ct2.realPower;
                                emonPi.Vrms = ct2.Vrms * 100;
                        } else {
                                emonPi.power2 = ct2.calcIrms(no_of_samples) * config.Vrms;
                        }
                }

                emonPi.power1_plus_2 = emonPi.power1 + emonPi.power2;                         //Calculate power 1 plus power 2 variable for US and solar PV installs

                if ((ACAC == 0) && (CT_count > 0))
                        emonPi.Vrms = config.Vrms*100;  // If no AC wave detected set VRMS constant

                if ((ACAC == 1) && (CT_count == 0)) {  // If only AC-AC is connected then return just VRMS calculation
                        ct1.calcVI(no_of_half_wavelengths, timeout);
                        emonPi.Vrms = ct1.Vrms * 100;
                }

                if (numSensors) {
                        sensors.requestTemperatures();                      // Send the command to get temperatures
                        for(byte j = 0; j < numSensors; j++)
                                emonPi.temp[j]=get_temperature(j);
                }

                if (pulseCount) {                                           // if the ISR has counted some pulses, update the total count
                        cli();                                            // Disable interrupt just in case pulse comes in while we are updating the count
                        emonPi.pulseCount += pulseCount;
                        pulseCount = 0;
                        sei();                                            // Re-enable interrupts
                }

                send_emonpi_serial(); //Send emonPi data to Pi serial using struct packet structure
                if (debug)
                        serial_print_emonpi();

                last_sample = now;                               //Record time of sample
        } // end sample
} // end loop---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void single_LED_flash()
{
    digitalWrite(LEDpin, HIGH);  delay(50); digitalWrite(LEDpin, LOW);
}

void double_LED_flash()
{
    digitalWrite(LEDpin, HIGH);  delay(25); digitalWrite(LEDpin, LOW);
    digitalWrite(LEDpin, HIGH);  delay(25); digitalWrite(LEDpin, LOW);
}
