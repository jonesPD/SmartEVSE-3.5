#include <unordered_map>
#if MODEM
int8_t InitialSoC = -1;                                                     // State of charge of car
int8_t FullSoC = -1;                                                        // SoC car considers itself fully charged
int8_t ComputedSoC = -1;                                                    // Estimated SoC, based on charged kWh
int8_t RemainingSoC = -1;                                                   // Remaining SoC, based on ComputedSoC
int32_t TimeUntilFull = -1;                                                 // Remaining time until car reaches FullSoC, in seconds
int32_t EnergyCapacity = -1;                                                // Car's total battery capacity
int32_t EnergyRequest = -1;                                                 // Requested amount of energy by car
char EVCCID[32];                                                            // Car's EVCCID (EV Communication Controller Identifer)
char RequiredEVCCID[32] = "";                                               // Required EVCCID before allowing charging
#endif

#ifdef SMARTEVSE_VERSION //ESP32

#include <ArduinoJson.h>
#include <SPI.h>
#include <Preferences.h>

#include <FS.h>

#include <WiFi.h>
#include "network.h"
#include "esp_ota_ops.h"
#include "mbedtls/md_internal.h"

#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <glcd.h>

#include <Logging.h>
#include <ModbusServerRTU.h>        // Slave/node
#include <ModbusClientRTU.h>        // Master
#include <time.h>

#include <soc/sens_reg.h>
#include <soc/sens_struct.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <soc/rtc_io_struct.h>

#include "esp32.h"
#include "glcd.h"
#include "utils.h"
#include "OneWire.h"
#include "modbus.h"
#include "meter.h"

//OCPP includes
#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
#include <MicroOcpp.h>
#include <MicroOcppMongooseClient.h>
#include <MicroOcpp/Core/Configuration.h>
#include <MicroOcpp/Core/Context.h>
#endif //ENABLE_OCPP

#if SMARTEVSE_VERSION >= 40
#include <esp_sleep.h>
#include <driver/uart.h>

#include "wchisp.h"
#include "qca.h"

SPIClass QCA_SPI1(FSPI);  // The ESP32-S3 has two usable SPI busses FSPI and HSPI
SPIClass LCD_SPI2(HSPI);

/*    Commands send from ESP32 to CH32V203 over Uart
/    cmd        Name           Answer/data        Comments
/---------------------------------------------------------------------------------------------------------------------------------
/    Ver?    Version           0001              Version of CH32 software
/    Stat?   Status                              State, Amperage, PP pin, SSR outputs, ACT outputs, VCC enable, Lock input, RCM, Temperature, Error
/    Amp:    Set AMP           160               Set Chargecurrent A (*10)
/    Con:    Set Contactors    0-3               0= Both Off, 1= SSR1 ON, 2= SSR2 ON, 3= Both ON
/    Vcc:    Set VCC           0-1               0= VCC Off, 1= VCC ON
/    Sol:    Set Solenoid      0-3               0= Both Off, 1= LOCK_R ON, 2= LOCK_W ON, 3= Both ON (or only lock/unlock?)
/    Led:    Set Led color                       RGB, Fade speed, Blink
/    485:    Modbus data
/
/    Bij wegvallen ZC -> Solenoid unlock (indien locked)

*/

uint8_t CommState = COMM_OFF;

// Power Panic handler
// Shut down ESP to conserve the power we have left. RTC will automatically store powerdown timestamp
// We can store some important data in flash storage or the RTC chip (2 bytes)
//
void PowerPanicESP() {

    _LOG_D("Power Panic!\n");
    ledcWrite(LCD_CHANNEL, 0);                 // LCD Backlight off

    // Stop SPI bus, and set all QCA data lines low
    // TODO: store important information.

    gpio_wakeup_enable(GPIO_NUM_8, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    esp_light_sleep_start();
    // ESP32 is now in light sleep mode

    // It will re-enable everything as soon it has woken up again.
    // When using USB, you will have to unplug, and replug to re-establish the connection

    _LOG_D("Power Back up!\n");

    ledcWrite(LCD_CHANNEL, 50);                 // LCD Backlight on
}

extern void SendConfigToCH32(void);
#endif //SMARTEVSE_VERSION

#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
// Create a ModbusRTU server, client and bridge instance on Serial1
ModbusServerRTU MBserver(2000, PIN_RS485_DIR);     // TCP timeout set to 2000 ms
ModbusClientRTU MBclient(PIN_RS485_DIR);
static esp_adc_cal_characteristics_t * adc_chars_PP;
static esp_adc_cal_characteristics_t * adc_chars_Temperature;
extern ModbusMessage MBEVMeterResponse(ModbusMessage request);
#endif //SMARTEVSE_VERSION

hw_timer_t * timerA = NULL;
Preferences preferences;

uint16_t LCDPin = 0;                                                        // PIN to operate LCD keys from web-interface

extern esp_adc_cal_characteristics_t * adc_chars_CP;
extern void setStatePowerUnavailable(void);
extern char IsCurrentAvailable(void);
extern unsigned char RFID[8];
extern uint8_t pilot;

extern const char StrStateName[15][13];
const char StrStateNameWeb[15][17] = {"Ready to Charge", "Connected to EV", "Charging", "D", "Request State B", "State B OK", "Request State C", "State C OK", "Activate", "Charging Stopped", "Stop Charging", "Modem Setup", "Modem Request", "Modem Done", "Modem Denied"};
const char StrErrorNameWeb[9][20] = {"None", "No Power Available", "Communication Error", "Temperature High", "EV Meter Comm Error", "RCM Tripped", "Waiting for Solar", "Test IO", "Flash Error"};
const char StrMode[3][8] = {"Normal", "Smart", "Solar"};
const char StrRFIDStatusWeb[8][20] = {"Ready to read card","Present", "Card Stored", "Card Deleted", "Card already stored", "Card not in storage", "Card Storage full", "Invalid" };

// Global data


// The following data will be updated by eeprom/storage data at powerup:
extern uint16_t MaxMains;
extern uint16_t MaxSumMains;
                                                                            // see https://github.com/serkri/SmartEVSE-3/issues/215
                                                                            // 0 means disabled, allowed value 10 - 600 A
extern uint8_t MaxSumMainsTime;
extern uint16_t MaxSumMainsTimer;
extern uint16_t GridRelayMaxSumMains;
                                                                            // Meant to obey par 14a of Energy Industry Act, where the provider can switch a device
                                                                            // down to 4.2kW by a relay connected to the "switch" connectors.
                                                                            // you will have to set the "Switch" setting to "GridRelay",
                                                                            // and connect the relay to the switch terminals
                                                                            // When the relay opens its contacts, power will be reduced to 4.2kW
                                                                            // The relay is only allowed on the Master
extern bool CustomButton;
extern uint16_t MaxCurrent;
extern uint16_t MinCurrent;
extern uint8_t Mode;
extern uint32_t CurrentPWM;
extern void SetCurrent(uint16_t current);

extern bool CPDutyOverride;
extern uint8_t Lock;
extern uint16_t MaxCircuit;
extern uint8_t Config;
extern uint8_t Switch;
                                                                            // 3:Smart-Solar B / 4:Smart-Solar S / 5: Grid Relay
                                                                            // 6:Custom B / 7:Custom S)
                                                                            // B=momentary push <B>utton, S=toggle <S>witch
extern uint8_t RCmon;
extern uint8_t AutoUpdate;
extern uint16_t StartCurrent;
extern uint16_t StopTime;
extern uint16_t ImportCurrent;
extern struct DelayedTimeStruct DelayedStopTime;
extern uint8_t DelayedRepeat;
extern uint8_t LCDlock;
extern uint8_t Lock;
extern uint8_t CableLock;
extern EnableC2_t EnableC2;
extern uint8_t RFIDReader;

extern uint16_t maxTemp;

extern uint16_t MaxCapacity;                                                       // Cable limit (A) (limited by the wire in the charge cable, set automatically, or manually if Config=Fixed Cable)
extern uint16_t ChargeCurrent;                                                     // Calculated Charge Current (Amps *10)
extern uint16_t OverrideCurrent;

// Load Balance variables
extern int16_t IsetBalanced;
extern uint16_t Balanced[NR_EVSES];
#if SMARTEVSE_VERSION < 40 //v3
extern uint16_t BalancedMax[NR_EVSES];
extern uint8_t BalancedState[NR_EVSES];
extern uint16_t BalancedError[NR_EVSES];
#endif

extern Node_t Node[NR_EVSES];
extern uint16_t BacklightTimer;
extern uint8_t BacklightSet;
extern int8_t TempEVSE;
SemaphoreHandle_t buttonMutex = xSemaphoreCreateMutex();
uint8_t ButtonStateOverride = 0x07;                                         // Possibility to override the buttons via API
uint32_t LastBtnOverrideTime = 0;                                           // Avoid UI buttons getting stuck
extern uint8_t ChargeDelay;
extern uint8_t NoCurrent;
extern uint8_t ModbusRequest;
extern uint16_t CardOffset;

extern uint8_t ConfigChanged;

extern uint16_t SolarStopTimer;

extern uint8_t ActivationMode, ActivationTimer;
extern volatile uint16_t adcsample;
extern volatile uint16_t ADCsamples[25];                                           // declared volatile, as they are used in a ISR
extern volatile uint8_t sampleidx;
extern char str[20];

extern int phasesLastUpdate;
extern bool phasesLastUpdateFlag;
extern int16_t IrmsOriginal[3];
extern int homeBatteryCurrent;
extern int homeBatteryLastUpdate;
// set by EXTERNAL logic through MQTT/REST to indicate cheap tariffs ahead until unix time indicated
extern uint8_t ColorOff[3] ;
extern uint8_t ColorNormal[3] ;
extern uint8_t ColorSmart[3] ;
extern uint8_t ColorSolar[3] ;
extern uint8_t ColorCustom[3];

#define FW_UPDATE_DELAY 3600                                                    // time between detection of new version and actual update in seconds
extern uint16_t firmwareUpdateTimer;
                                                                                // 0 means timer inactive
                                                                                // 0 < timer < FW_UPDATE_DELAY means we are in countdown for an actual update
                                                                                // FW_UPDATE_DELAY <= timer <= 0xffff means we are in countdown for checking
                                                                                //                                              whether an update is necessary

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
extern unsigned char OcppRfidUuid [7];
extern size_t OcppRfidUuidLen;
extern unsigned long OcppLastRfidUpdate;
extern unsigned long OcppTrackLastRfidUpdate;

extern bool OcppForcesLock;
extern std::shared_ptr<MicroOcpp::Configuration> OcppUnlockConnectorOnEVSideDisconnect; // OCPP Config for RFID-based transactions: if false, demand same RFID card again to unlock connector
extern std::shared_ptr<MicroOcpp::Transaction> OcppLockingTx; // Transaction which locks connector until same RFID card is presented again

extern bool OcppTrackPermitsCharge;
extern bool OcppTrackAccessBit;
extern uint8_t OcppTrackCPvoltage;
extern MicroOcpp::MOcppMongooseClient *OcppWsClient;

extern float OcppCurrentLimit;

extern unsigned long OcppStopReadingSyncTime; // Stop value synchronization: delay StopTransaction by a few seconds so it reports an accurate energy reading

extern bool OcppDefinedTxNotification;
extern MicroOcpp::TxNotification OcppTrackTxNotification;
extern unsigned long OcppLastTxNotification;
#endif //ENABLE_OCPP


#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
// Some low level stuff here to setup the ADC, and perform the conversion.
//
//
uint16_t IRAM_ATTR local_adc1_read(int channel) {
    uint16_t adc_value;

    SENS.sar_read_ctrl.sar1_dig_force = 0;                      // switch SARADC into RTC channel 
    SENS.sar_meas_wait2.force_xpd_sar = SENS_FORCE_XPD_SAR_PU;  // adc_power_on
    RTCIO.hall_sens.xpd_hall = false;                           // disable other peripherals
    
    //adc_ll_amp_disable()  // Close ADC AMP module if don't use it for power save.
    SENS.sar_meas_wait2.force_xpd_amp = SENS_FORCE_XPD_AMP_PD;  // channel is set in the convert function
    // disable FSM, it's only used by the LNA.
    SENS.sar_meas_ctrl.amp_rst_fb_fsm = 0; 
    SENS.sar_meas_ctrl.amp_short_ref_fsm = 0;
    SENS.sar_meas_ctrl.amp_short_ref_gnd_fsm = 0;
    SENS.sar_meas_wait1.sar_amp_wait1 = 1;
    SENS.sar_meas_wait1.sar_amp_wait2 = 1;
    SENS.sar_meas_wait2.sar_amp_wait3 = 1; 

    // adc_hal_set_controller(ADC_NUM_1, ADC_CTRL_RTC);         //Set controller
    // see esp-idf/components/hal/esp32/include/hal/adc_ll.h
    SENS.sar_read_ctrl.sar1_dig_force       = 0;                // 1: Select digital control;       0: Select RTC control.
    SENS.sar_meas_start1.meas1_start_force  = 1;                // 1: SW control RTC ADC start;     0: ULP control RTC ADC start.
    SENS.sar_meas_start1.sar1_en_pad_force  = 1;                // 1: SW control RTC ADC bit map;   0: ULP control RTC ADC bit map;
    SENS.sar_touch_ctrl1.xpd_hall_force     = 1;                // 1: SW control HALL power;        0: ULP FSM control HALL power.
    SENS.sar_touch_ctrl1.hall_phase_force   = 1;                // 1: SW control HALL phase;        0: ULP FSM control HALL phase.

    // adc_hal_convert(ADC_NUM_1, channel, &adc_value);
    // see esp-idf/components/hal/esp32/include/hal/adc_ll.h
    SENS.sar_meas_start1.sar1_en_pad = (1 << channel);          // select ADC channel to sample on
    while (SENS.sar_slave_addr1.meas_status != 0);              // wait for conversion to be idle (blocking)
    SENS.sar_meas_start1.meas1_start_sar = 0;         
    SENS.sar_meas_start1.meas1_start_sar = 1;                   // start ADC conversion
    while (SENS.sar_meas_start1.meas1_done_sar == 0);           // wait (blocking) for conversion to finish
    adc_value = SENS.sar_meas_start1.meas1_data_sar;            // read ADC value from register

    return adc_value;
}



// CP pin low to high transition ISR
//
//
void IRAM_ATTR onCPpulse() {

  // reset timer, these functions are in IRAM !
  timerWrite(timerA, 0);                                        
  timerAlarmEnable(timerA);
}



// Timer interrupt handler
// in STATE A this is called every 1ms (autoreload)
// in STATE B/C there is a PWM signal, and the Alarm is set to 5% after the low-> high transition of the PWM signal
void IRAM_ATTR onTimerA() {

  RTC_ENTER_CRITICAL();
  adcsample = local_adc1_read(ADC1_CHANNEL_3);

  RTC_EXIT_CRITICAL();

  ADCsamples[sampleidx++] = adcsample;
  if (sampleidx == 25) sampleidx = 0;
}

#endif //SMARTEVSE_VERSION

// --------------------------- END of ISR's -----------------------------------------------------

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
// Inverse function of SetCurrent (for monitoring and debugging purposes)
uint16_t GetCurrent() {
    uint32_t DutyCycle = CurrentPWM;

    if (DutyCycle < 102) {
        return 0; //PWM off or ISO15118 modem enabled
    } else if (DutyCycle < 870) {
        return (DutyCycle * 1000 / 1024) * 0.6 + 1; // invert duty cycle formula + fixed rounding error correction
    } else if (DutyCycle <= 983) {
        return ((DutyCycle * 1000 / 1024)- 640) * 2.5 + 3; // invert duty cycle formula + fixed rounding error correction
    } else {
        return 0; //constant +12V
    }
}
#endif //ENABLE_OCPP


#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
// Sample the Temperature sensor.
//
int8_t TemperatureSensor() {
    uint32_t sample, voltage;
    signed char Temperature;

    RTC_ENTER_CRITICAL();
    // Sample Temperature Sensor
    sample = local_adc1_read(ADC1_CHANNEL_0);
    RTC_EXIT_CRITICAL();

    // voltage range is from 0-2200mV 
    voltage = esp_adc_cal_raw_to_voltage(sample, adc_chars_Temperature);

    // The MCP9700A temperature sensor outputs 500mV at 0C, and has a 10mV/C change in output voltage.
    // so 750mV is 25C, 400mV = -10C
    Temperature = (signed int)(voltage - 500)/10;
    //_LOG_A("\nTemp: %i C (%u mV) ", Temperature , voltage);
    
    return Temperature;
}

// Sample the Proximity Pin, and determine the maximum current the cable can handle.
//
uint8_t ProximityPin() {
    uint32_t sample, voltage;
    uint8_t MaxCap = 13;                                               // No resistor, Max cable current = 13A

    RTC_ENTER_CRITICAL();
    // Sample Proximity Pilot (PP)
    sample = local_adc1_read(ADC1_CHANNEL_6);
    RTC_EXIT_CRITICAL();

    voltage = esp_adc_cal_raw_to_voltage(sample, adc_chars_PP);

    if (!Config) {                                                          // Configuration (0:Socket / 1:Fixed Cable)
        //socket
        _LOG_A("PP pin: %u (%u mV)\n", sample, voltage);
    } else {
        //fixed cable
        _LOG_A("PP pin: %u (%u mV) (warning: fixed cable configured so PP probably disconnected, making this reading void)\n", sample, voltage);
    }

    if ((voltage > 1200) && (voltage < 1400)) MaxCap = 16;             // Max cable current = 16A	680R -> should be around 1.3V
    if ((voltage > 500) && (voltage < 700)) MaxCap = 32;               // Max cable current = 32A	220R -> should be around 0.6V
    if ((voltage > 200) && (voltage < 400)) MaxCap = 63;               // Max cable current = 63A	100R -> should be around 0.3V

    if (Config) MaxCap = MaxCurrent;                                   // Override with MaxCurrent when Fixed Cable is used.
    return MaxCap;
}
#endif


/**
 * Get name of a state
 *
 * @param uint8_t State
 * @return uint8_t[] Name
 */
const char * getStateName(uint8_t StateCode) {
    if(StateCode < 15) return StrStateName[StateCode];
    else return "NOSTATE";
}


const char * getStateNameWeb(uint8_t StateCode) {
    if(StateCode < 15) return StrStateNameWeb[StateCode];
    else return "NOSTATE";    
}


uint8_t getErrorId(uint8_t ErrorCode) {
    uint8_t count = 0;
    //find the error bit that is set
    while (ErrorCode) {
        count++;
        ErrorCode = ErrorCode >> 1;
    }    
    return count;
}


const char * getErrorNameWeb(uint8_t ErrorCode) {
    uint8_t count = 0;
    count = getErrorId(ErrorCode);
    if(count < 9) return StrErrorNameWeb[count];
    else return "Multiple Errors";
}


void getButtonState() {
    // Sample the three < o > buttons.
    // As the buttons are shared with the SPI lines going to the LCD,
    // we have to make sure that this does not interfere by write actions to the LCD.
    // Therefore updating the LCD is also done in this task.
    xSemaphoreTake(buttonMutex, portMAX_DELAY);
    if (ButtonStateOverride != 7 && millis() - LastBtnOverrideTime < 4000)
        ButtonState = ButtonStateOverride;
    else {
#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
        pinMatrixOutDetach(PIN_LCD_SDO_B3, false, false);       // disconnect MOSI pin
        pinMode(PIN_LCD_SDO_B3, INPUT);
        pinMode(PIN_LCD_A0_B2, INPUT);

        // sample buttons                                                         < o >
        ButtonState = (digitalRead(PIN_LCD_SDO_B3) ? 4 : 0) |  // > (right)
                      (digitalRead(PIN_LCD_A0_B2)  ? 2 : 0) |  // o (middle)
                      (digitalRead(PIN_IO0_B1)     ? 1 : 0);   // < (left)

        pinMode(PIN_LCD_SDO_B3, OUTPUT);
        pinMatrixOutAttach(PIN_LCD_SDO_B3, VSPID_IN_IDX, false, false); // re-attach MOSI pin
#else
        pinMode(PIN_LCD_A0_B2, INPUT_PULLUP);                  // Switch the shared pin for the middle button to input
        ButtonState = (digitalRead(BUTTON3)        ? 4 : 0) |  // > (right)
                      (digitalRead(PIN_LCD_A0_B2)  ? 2 : 0) |  // o (middle)
                      (digitalRead(BUTTON1)        ? 1 : 0);   // < (left)
#endif
        pinMode(PIN_LCD_A0_B2, OUTPUT);                        // switch pin back to output
    }
    xSemaphoreGive(buttonMutex);
}


#if MQTT && defined(SMARTEVSE_VERSION) // ESP32 only
void mqtt_receive_callback(const String topic, const String payload) {
    if (topic == MQTTprefix + "/Set/Mode") {
        if (payload == "Off") {
            Serial1.printf("@ResetModemTimers\n");
            setAccess(OFF);
        } else if (payload == "Normal") {
            setMode(MODE_NORMAL);
        } else if (payload == "Solar") {
            setOverrideCurrent(0);
            setMode(MODE_SOLAR);
        } else if (payload == "Smart") {
            setOverrideCurrent(0);
            setMode(MODE_SMART);
        } else if (payload == "Pause") {
            setAccess(PAUSE);
        }
    } else if (topic == MQTTprefix + "/Set/CustomButton") {
        if (payload == "On") {
            CustomButton = true;
        } else {
            CustomButton = false;
        }
    } else if (topic == MQTTprefix + "/Set/CurrentOverride") {
        uint16_t RequestedCurrent = payload.toInt();
        if (RequestedCurrent == 0) {
            setOverrideCurrent(0);
        } else if (LoadBl < 2 && (Mode == MODE_NORMAL || Mode == MODE_SMART)) { // OverrideCurrent not possible on Slave
            if (RequestedCurrent >= (MinCurrent * 10) && RequestedCurrent <= (MaxCurrent * 10)) {
                setOverrideCurrent(RequestedCurrent);
            }
        }
    } else if (topic == MQTTprefix + "/Set/CurrentMaxSumMains" && LoadBl < 2) {
        uint16_t RequestedCurrent = payload.toInt();
        if (RequestedCurrent == 0) {
            MaxSumMains = 0;
        } else if (RequestedCurrent == 0 || (RequestedCurrent >= 10 && RequestedCurrent <= 600)) {
                MaxSumMains = RequestedCurrent;
        }
    } else if (topic == MQTTprefix + "/Set/CPPWMOverride") {
        int pwm = payload.toInt();
        if (pwm == -1) {
            SetCPDuty(1024);
            PILOT_CONNECTED;
            CPDutyOverride = false;
        } else if (pwm == 0) {
            SetCPDuty(0);
            PILOT_DISCONNECTED;
            CPDutyOverride = true;
        } else if (pwm <= 1024) {
            SetCPDuty(pwm);
            PILOT_CONNECTED;
            CPDutyOverride = true;
        }
    } else if (topic == MQTTprefix + "/Set/MainsMeter") {
        if (MainsMeter.Type != EM_API || LoadBl >= 2)
            return;

        int32_t L1, L2, L3;
        int n = sscanf(payload.c_str(), "%d:%d:%d", &L1, &L2, &L3);

        // MainsMeter can measure -200A to +200A per phase
        if (n == 3 && (L1 > -2000 && L1 < 2000) && (L2 > -2000 && L2 < 2000) && (L3 > -2000 && L3 < 2000)) {
#if SMARTEVSE_VERSION < 40 //v3
            if (LoadBl < 2) {
                MainsMeter.setTimeout(COMM_TIMEOUT);
                MainsMeter.Irms[0] = L1;
                MainsMeter.Irms[1] = L2;
                MainsMeter.Irms[2] = L3;
                CalcIsum();
            }
#else //v4
            Serial1.printf("@Irms:%03u,%d,%d,%d\n", MainsMeter.Address, L1, L2, L3); //Irms:011,312,123,124 means: the meter on address 11(dec) has Irms[0] 312 dA, Irms[1] of 123 dA, Irms[2] of 124 dA
#endif
        }
    } else if (topic == MQTTprefix + "/Set/EVMeter") {
        if (EVMeter.Type != EM_API)
            return;

        int32_t L1, L2, L3, W, WH;
        int n = sscanf(payload.c_str(), "%d:%d:%d:%d:%d", &L1, &L2, &L3, &W, &WH);

        // We expect 5 values (and accept -1 for unknown values)
        if (n == 5) {
            if ((L1 > -1 && L1 < 1000) && (L2 > -1 && L2 < 1000) && (L3 > -1 && L3 < 1000)) {
#if SMARTEVSE_VERSION < 40 //v3
                // RMS currents
                EVMeter.Irms[0] = L1;
                EVMeter.Irms[1] = L2;
                EVMeter.Irms[2] = L3;
                EVMeter.CalcImeasured();
                EVMeter.Timeout = COMM_EVTIMEOUT;
#else //v4
                Serial1.printf("@Irms:%03u,%d,%d,%d\n", EVMeter.Address, L1, L2, L3); //Irms:011,312,123,124 means: the meter on address 11(dec) has Irms[0] 312 dA, Irms[1] of 123 dA, Irms[2] of 124 dA
#endif
            }

            if (W > -1) {
                // Power measurement
#if SMARTEVSE_VERSION < 40 //v3
                EVMeter.PowerMeasured = W;
#else //v4
                Serial1.printf("@PowerMeasured:%03u,%d\n", EVMeter.Address, W);
#endif
            }

            if (WH > -1) {
                // Energy measurement;  //we dont send the energies to CH32 because they are not used there
                EVMeter.Import_active_energy = WH;
                EVMeter.Export_active_energy = 0;
                EVMeter.UpdateEnergies();
            }
        }
    } else if (topic == MQTTprefix + "/Set/HomeBatteryCurrent") {
        if (LoadBl >= 2)
            return;
        homeBatteryCurrent = payload.toInt();
        homeBatteryLastUpdate = time(NULL);
#if SMARTEVSE_VERSION >= 40
        SEND_TO_CH32(homeBatteryCurrent); //we set homeBatteryLastUpdate on CH32 on receipt
#endif
#if MODEM
    } else if (topic == MQTTprefix + "/Set/RequiredEVCCID") {
        strncpy(RequiredEVCCID, payload.c_str(), sizeof(RequiredEVCCID));
        Serial1.printf("@RequiredEVCCID:%s\n", RequiredEVCCID);
        write_settings();
#endif
    } else if (topic == MQTTprefix + "/Set/ColorOff") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorOff[0] = R;
            ColorOff[1] = G;
            ColorOff[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorNormal") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorNormal[0] = R;
            ColorNormal[1] = G;
            ColorNormal[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorSmart") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorSmart[0] = R;
            ColorSmart[1] = G;
            ColorSmart[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorSolar") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorSolar[0] = R;
            ColorSolar[1] = G;
            ColorSolar[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorCustom") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorCustom[0] = R;
            ColorCustom[1] = G;
            ColorCustom[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/CableLock") {
        if (payload == "1") {
            CableLock = 1;
        } else {
            CableLock = 0;
        }
        write_settings();
    }

    // Make sure MQTT updates directly to prevent debounces
    lastMqttUpdate = 10;
}


//print RFID in hex format
void printRFID(char *buf) {
    if (RFID[0] == 0x01) {  // old reader 6 byte UID starts at RFID[1]
        sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
    } else {
        sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X", RFID[0], RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
    }
}


//jsn(device_class, current) expands to:
// "device_class" : "current"
String jsn(const String& key, const String& value) {
    return "\"" + key + "\" : \"" + value + "\"";
}
template<typename T>
String jsn(const String& key, T value) {
    return "\"" + key + "\" : \"" + String(value) + "\"";
}


//jsna(device_class, current) expands to:
// , "device_class" : "current"
String jsna(const String& key, const String& value) {
    return ", " + jsn(key, value);
}
template<typename T>
String jsna(const String& key, T value) {
    return ", " + jsn(key, value);
}


void announce(const String& entity_name, const String& domain, const String& optional_payload) {
    String entity_suffix = entity_name;
    entity_suffix.replace(" ", "");
    String topic = "homeassistant/" + domain + "/" + MQTTprefix + "-" + entity_suffix + "/config";

    const String config_url = "http://" + WiFi.localIP().toString();
    const String device_payload = String(R"("device": {)") + jsn("model","SmartEVSE v3") + jsna("identifiers", MQTTprefix) + jsna("name", MQTTprefix) + jsna("manufacturer","Stegen") + jsna("configuration_url", config_url) + jsna("sw_version", String(VERSION)) + "}";

    String payload = "{"
        + jsn("name", entity_name)
        + jsna("object_id", String(MQTTprefix + "-" + entity_suffix))
        + jsna("unique_id", String(MQTTprefix + "-" + entity_suffix))
        + jsna("state_topic", String(MQTTprefix + "/" + entity_suffix))
        + jsna("availability_topic", String(MQTTprefix + "/connected"))
        + ", " + device_payload + optional_payload
        + "}";

    MQTTclient.publish(topic.c_str(), payload.c_str(), true, 0);  // Retain + QoS 0
}

void SetupMQTTClient() {
    // Set up subscriptions
    MQTTclient.subscribe(MQTTprefix + "/Set/#",1);
    MQTTclient.publish(MQTTprefix+"/connected", "online", true, 0);

    //set the parameters for and announce sensors with device class 'current':
    String optional_payload = jsna("device_class","current") + jsna("unit_of_measurement","A") + jsna("value_template", R"({{ value | int / 10 }})");
    announce("Charge Current", "sensor", optional_payload);
    announce("Max Current", "sensor", optional_payload);
    if (MainsMeter.Type) {
        announce("Mains Current L1", "sensor", optional_payload);
        announce("Mains Current L2", "sensor", optional_payload);
        announce("Mains Current L3", "sensor", optional_payload);
    }
    if (EVMeter.Type) {
        announce("EV Current L1", "sensor", optional_payload);
        announce("EV Current L2", "sensor", optional_payload);
        announce("EV Current L3", "sensor", optional_payload);
    }
    if (homeBatteryLastUpdate) {
        announce("Home Battery Current", "sensor", optional_payload);
    }

#if MODEM
        //set the parameters for modem/SoC sensor entities:
        optional_payload = jsna("unit_of_measurement","%") + jsna("value_template", R"({{ none if (value | int == -1) else (value | int) }})");
        announce("EV Initial SoC", "sensor", optional_payload);
        announce("EV Full SoC", "sensor", optional_payload);
        announce("EV Computed SoC", "sensor", optional_payload);
        announce("EV Remaining SoC", "sensor", optional_payload);

        optional_payload = jsna("device_class","duration") + jsna("unit_of_measurement","m") + jsna("value_template", R"({{ none if (value | int == -1) else (value | int / 60) | round }})");
        announce("EV Time Until Full", "sensor", optional_payload);

        optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh") + jsna("value_template", R"({{ none if (value | int == -1) else (value | int) }})");
        announce("EV Energy Capacity", "sensor", optional_payload);
        announce("EV Energy Request", "sensor", optional_payload);

        optional_payload = jsna("value_template", R"({{ none if (value == '') else value }})");
        announce("EVCCID", "sensor", optional_payload);
        optional_payload = jsna("state_topic", String(MQTTprefix + "/RequiredEVCCID")) + jsna("command_topic", String(MQTTprefix + "/Set/RequiredEVCCID"));
        announce("Required EVCCID", "text", optional_payload);
#endif

    optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh") + jsna("state_class","total_increasing");
    if (MainsMeter.Type) {
        announce("Mains Import Active Energy", "sensor", optional_payload);
        announce("Mains Export Active Energy", "sensor", optional_payload);
    }

    if (EVMeter.Type) {
        announce("EV Import Active Energy", "sensor", optional_payload);
        announce("EV Export Active Energy", "sensor", optional_payload);
        //set the parameters for and announce other sensor entities:
        optional_payload = jsna("device_class","power") + jsna("unit_of_measurement","W");
        announce("EV Charge Power", "sensor", optional_payload);
        optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh");
        announce("EV Energy Charged", "sensor", optional_payload);
        optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh") + jsna("state_class","total_increasing");
        announce("EV Total Energy Charged", "sensor", optional_payload);
    }

    //set the parameters for and announce sensor entities without device_class or unit_of_measurement:
    optional_payload = "";
    announce("EV Plug State", "sensor", optional_payload);
    announce("Access", "sensor", optional_payload);
    announce("State", "sensor", optional_payload);
    announce("RFID", "sensor", optional_payload);
    announce("RFIDLastRead", "sensor", optional_payload);

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
    announce("OCPP", "sensor", optional_payload);
    announce("OCPPConnection", "sensor", optional_payload);
#endif //ENABLE_OCPP

    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorOff")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorOff"));
    announce("LED Color Off", "text", optional_payload);
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorNormal")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorNormal"));
    announce("LED Color Normal", "text", optional_payload);
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorSmart")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorSmart"));
    announce("LED Color Smart", "text", optional_payload);
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorSolar")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorSolar"));
    announce("LED Color Solar", "text", optional_payload);
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorCustom")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorCustom"));
    announce("LED Color Custom", "text", optional_payload);
    
    optional_payload = jsna("state_topic", String(MQTTprefix + "/CustomButton")) + jsna("command_topic", String(MQTTprefix + "/Set/CustomButton"));
    optional_payload += String(R"(, "options" : ["On", "Off"])");
    announce("Custom Button", "select", optional_payload);

    optional_payload = jsna("device_class","duration") + jsna("unit_of_measurement","s");
    announce("SolarStopTimer", "sensor", optional_payload);
    //set the parameters for and announce diagnostic sensor entities:
    optional_payload = jsna("entity_category","diagnostic");
    announce("Error", "sensor", optional_payload);
    announce("WiFi SSID", "sensor", optional_payload);
    announce("WiFi BSSID", "sensor", optional_payload);
    optional_payload = jsna("entity_category","diagnostic") + jsna("device_class","signal_strength") + jsna("unit_of_measurement","dBm");
    announce("WiFi RSSI", "sensor", optional_payload);
    optional_payload = jsna("entity_category","diagnostic") + jsna("device_class","temperature") + jsna("unit_of_measurement","°C");
    announce("ESP Temp", "sensor", optional_payload);
    optional_payload = jsna("entity_category","diagnostic") + jsna("device_class","duration") + jsna("unit_of_measurement","s") + jsna("entity_registry_enabled_default","False");
    announce("ESP Uptime", "sensor", optional_payload);

#if MODEM
        optional_payload = jsna("unit_of_measurement","%") + jsna("value_template", R"({{ (value | int / 1024 * 100) | round(0) }})");
        announce("CP PWM", "sensor", optional_payload);

        optional_payload = jsna("value_template", R"({{ none if (value | int == -1) else (value | int / 1024 * 100) | round }})");
        optional_payload += jsna("command_topic", String(MQTTprefix + "/Set/CPPWMOverride")) + jsna("min", "-1") + jsna("max", "100") + jsna("mode","slider");
        optional_payload += jsna("command_template", R"({{ (value | int * 1024 / 100) | round }})");
        announce("CP PWM Override", "number", optional_payload);
#endif
    //set the parameters for and announce select entities, overriding automatic state_topic:
    optional_payload = jsna("state_topic", String(MQTTprefix + "/Mode")) + jsna("command_topic", String(MQTTprefix + "/Set/Mode"));
    optional_payload += String(R"(, "options" : ["Off", "Normal", "Smart", "Solar", "Pause"])");
    announce("Mode", "select", optional_payload);

    //set the parameters for and announce number entities:
    optional_payload = jsna("command_topic", String(MQTTprefix + "/Set/CurrentOverride")) + jsna("min", "0") + jsna("max", MaxCurrent ) + jsna("mode","slider");
    optional_payload += jsna("value_template", R"({{ value | int / 10 if value | is_number else none }})") + jsna("command_template", R"({{ value | int * 10 }})");
    announce("Charge Current Override", "number", optional_payload);

    //set the parameters for and announce Cable Lock:
    optional_payload = jsna("cablelock_topic", String(MQTTprefix + "/CableLock")) + jsna("command_topic", String(MQTTprefix + "/Set/CableLock"));
    optional_payload += String(R"(, "options" : ["0", "1"])");
    announce("Cable Lock", "select", optional_payload);
}

void mqttPublishData() {
    lastMqttUpdate = 0;

        if (MainsMeter.Type) {
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL1", MainsMeter.Irms[0], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL2", MainsMeter.Irms[1], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL3", MainsMeter.Irms[2], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsImportActiveEnergy", MainsMeter.Import_active_energy, false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsExportActiveEnergy", MainsMeter.Export_active_energy, false, 0);
        }
        if (EVMeter.Type) {
            MQTTclient.publish(MQTTprefix + "/EVCurrentL1", EVMeter.Irms[0], false, 0);
            MQTTclient.publish(MQTTprefix + "/EVCurrentL2", EVMeter.Irms[1], false, 0);
            MQTTclient.publish(MQTTprefix + "/EVCurrentL3", EVMeter.Irms[2], false, 0);
            MQTTclient.publish(MQTTprefix + "/EVImportActiveEnergy", EVMeter.Import_active_energy, false, 0);
            MQTTclient.publish(MQTTprefix + "/EVExportActiveEnergy", EVMeter.Export_active_energy, false, 0);
        }
        MQTTclient.publish(MQTTprefix + "/ESPTemp", TempEVSE, false, 0);
        MQTTclient.publish(MQTTprefix + "/Mode", AccessStatus == OFF ? "Off" : AccessStatus == PAUSE ? "Pause" : Mode > 3 ? "N/A" : StrMode[Mode], true, 0);
        MQTTclient.publish(MQTTprefix + "/MaxCurrent", MaxCurrent * 10, true, 0);
        MQTTclient.publish(MQTTprefix + "/CustomButton", CustomButton ? "On" : "Off", false, 0);
        MQTTclient.publish(MQTTprefix + "/ChargeCurrent", Balanced[0], true, 0);
        MQTTclient.publish(MQTTprefix + "/ChargeCurrentOverride", OverrideCurrent, true, 0);
        MQTTclient.publish(MQTTprefix + "/NrOfPhases", Nr_Of_Phases_Charging, false, 0);
        MQTTclient.publish(MQTTprefix + "/Access", AccessStatus == OFF ? "Deny" : AccessStatus == ON ? "Allow" : AccessStatus == PAUSE ? "Pause" : "N/A", true, 0);
        MQTTclient.publish(MQTTprefix + "/RFID", !RFIDReader ? "Not Installed" : RFIDstatus >= 8 ? "NOSTATUS" : StrRFIDStatusWeb[RFIDstatus], true, 0);
        if (RFIDReader) {
            char buf[15];
            printRFID(buf);
            MQTTclient.publish(MQTTprefix + "/RFIDLastRead", buf, true, 0);
        }
        MQTTclient.publish(MQTTprefix + "/State", getStateNameWeb(State), true, 0);
        MQTTclient.publish(MQTTprefix + "/Error", getErrorNameWeb(ErrorFlags), true, 0);
        MQTTclient.publish(MQTTprefix + "/EVPlugState", (pilot != PILOT_12V) ? "Connected" : "Disconnected", true, 0);
        MQTTclient.publish(MQTTprefix + "/WiFiSSID", String(WiFi.SSID()), true, 0);
        MQTTclient.publish(MQTTprefix + "/WiFiBSSID", String(WiFi.BSSIDstr()), true, 0);
#if MODEM
        MQTTclient.publish(MQTTprefix + "/CPPWM", CurrentPWM, false, 0);
        MQTTclient.publish(MQTTprefix + "/CPPWMOverride", CPDutyOverride ? String(CurrentPWM) : "-1", true, 0);
        MQTTclient.publish(MQTTprefix + "/EVInitialSoC", InitialSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVFullSoC", FullSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVComputedSoC", ComputedSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVRemainingSoC", RemainingSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVTimeUntilFull", TimeUntilFull, false, 0);
        MQTTclient.publish(MQTTprefix + "/EVEnergyCapacity", EnergyCapacity, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVEnergyRequest", EnergyRequest, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVCCID", EVCCID, true, 0);
        MQTTclient.publish(MQTTprefix + "/RequiredEVCCID", RequiredEVCCID, true, 0);
#endif
        if (EVMeter.Type) {
            MQTTclient.publish(MQTTprefix + "/EVChargePower", EVMeter.PowerMeasured, false, 0);
            MQTTclient.publish(MQTTprefix + "/EVEnergyCharged", EVMeter.EnergyCharged, true, 0);
            MQTTclient.publish(MQTTprefix + "/EVTotalEnergyCharged", EVMeter.Energy, false, 0);
        }
        if (homeBatteryLastUpdate)
            MQTTclient.publish(MQTTprefix + "/HomeBatteryCurrent", homeBatteryCurrent, false, 0);
#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
        MQTTclient.publish(MQTTprefix + "/OCPP", OcppMode ? "Enabled" : "Disabled", true, 0);
        MQTTclient.publish(MQTTprefix + "/OCPPConnection", (OcppWsClient && OcppWsClient->isConnected()) ? "Connected" : "Disconnected", false, 0);
#endif //ENABLE_OCPP
        MQTTclient.publish(MQTTprefix + "/LEDColorOff", String(ColorOff[0])+","+String(ColorOff[1])+","+String(ColorOff[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorNormal", String(ColorNormal[0])+","+String(ColorNormal[1])+","+String(ColorNormal[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorSmart", String(ColorSmart[0])+","+String(ColorSmart[1])+","+String(ColorSmart[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorSolar", String(ColorSolar[0])+","+String(ColorSolar[1])+","+String(ColorSolar[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorCustom", String(ColorCustom[0])+","+String(ColorCustom[1])+","+String(ColorCustom[2]), true, 0);
        if (Lock != 0) {
            MQTTclient.publish(MQTTprefix + "/CableLock", CableLock ? "Enabled" : "Disabled", true, 0);
        }
}
#endif


/**
 * Validate setting ranges and dependencies
 */
void validate_settings(void) {
    uint8_t i;
    uint16_t value;

    // If value is out of range, reset it to default value
    for (i = MENU_ENTER + 1;i < MENU_EXIT; i++){
        value = getItemValue(i);
    //    _LOG_A("value %s set to %i\n",MenuStr[i].LCD, value );
        if (value > MenuStr[i].Max || value < MenuStr[i].Min) {
            value = MenuStr[i].Default;
    //        _LOG_A("set default value for %s to %i\n",MenuStr[i].LCD, value );
            setItemValue(i, value);
        }
    }

    // Sensorbox v2 has always address 0x0A
    if (MainsMeter.Type == EM_SENSORBOX) MainsMeter.Address = 0x0A;
    // set Lock variables for Solenoid or Motor
    //if (Lock == 1) { lock1 = LOW; lock2 = HIGH; }                               // Solenoid
    //else if (Lock == 2) { lock1 = HIGH; lock2 = LOW; }                          // Motor
    // Erase all RFID cards from ram + eeprom if set to EraseAll
    if (RFIDReader == 5) {
        DeleteAllRFID();
        setItemValue(MENU_RFIDREADER, 0);                                       // RFID Reader Disabled
    }
#if SMARTEVSE_VERSION < 40 //v3
    // Update master node config; for v4 this is taken care of when receiving the EVMeterType/Address
    if (LoadBl < 2) {
        Node[0].EVMeter = EVMeter.Type;
        Node[0].EVAddress = EVMeter.Address;
    }
#endif
    // Default to modbus input registers
    if (EMConfig[EM_CUSTOM].Function != 3) EMConfig[EM_CUSTOM].Function = 4;

    // Backward compatibility < 2.20
    if (EMConfig[EM_CUSTOM].IRegister == 8 || EMConfig[EM_CUSTOM].URegister == 8 || EMConfig[EM_CUSTOM].PRegister == 8 || EMConfig[EM_CUSTOM].ERegister == 8) {
        EMConfig[EM_CUSTOM].DataType = MB_DATATYPE_FLOAT32;
        EMConfig[EM_CUSTOM].IRegister = 0;
        EMConfig[EM_CUSTOM].URegister = 0;
        EMConfig[EM_CUSTOM].PRegister = 0;
        EMConfig[EM_CUSTOM].ERegister = 0;
    }

#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
    // If the address of the MainsMeter or EVmeter on a Node has changed, we must re-register the Modbus workers.
    if (LoadBl > 1) {
        if (EVMeter.Type && EVMeter.Type != EM_API) MBserver.registerWorker(EVMeter.Address, ANY_FUNCTION_CODE, &MBEVMeterResponse);
    }
#endif
    MainsMeter.setTimeout(COMM_TIMEOUT);
    EVMeter.setTimeout(COMM_TIMEOUT);                                             // Short Delay, to clear the error message for ~10 seconds.

}

void read_settings() {
    
    // Open preferences. true = read only,  false = read/write
    // If "settings" does not exist, it will be created, and initialized with the default values
    if (preferences.begin("settings", false) ) {                                
        bool Initialized = preferences.isKey("Config");
        Config = preferences.getUChar("Config", CONFIG); 
        Lock = preferences.getUChar("Lock", LOCK); 
        Mode = preferences.getUChar("Mode", MODE); 
        AccessStatus = (AccessStatus_t) preferences.getUChar("Access", ON);
        if (preferences.isKey("CardOffset")) {
            CardOffset = preferences.getUChar("CardOffset", CARD_OFFSET);
            //write the old 8 bits value to the new 16 bits value
            preferences.putUShort("CardOffs16", CardOffset);
            preferences.remove("CardOffset");
        }
        else
            CardOffset = preferences.getUShort("CardOffs16", CARD_OFFSET);
        LoadBl = preferences.getUChar("LoadBl", LOADBL); 
        MaxMains = preferences.getUShort("MaxMains", MAX_MAINS); 
        MaxSumMains = preferences.getUShort("MaxSumMains", MAX_SUMMAINS);
        MaxSumMainsTime = preferences.getUShort("MaxSumMainsTime", MAX_SUMMAINSTIME);
        MaxCurrent = preferences.getUShort("MaxCurrent", MAX_CURRENT); 
        MinCurrent = preferences.getUShort("MinCurrent", MIN_CURRENT); 
        MaxCircuit = preferences.getUShort("MaxCircuit", MAX_CIRCUIT); 
        Switch = preferences.getUChar("Switch", SWITCH); 
        RCmon = preferences.getUChar("RCmon", RC_MON); 
        StartCurrent = preferences.getUShort("StartCurrent", START_CURRENT); 
        StopTime = preferences.getUShort("StopTime", STOP_TIME); 
        ImportCurrent = preferences.getUShort("ImportCurrent",IMPORT_CURRENT);
        Grid = preferences.getUChar("Grid",GRID);
        SB2_WIFImode = preferences.getUChar("SB2WIFImode",SB2_WIFI_MODE);
        RFIDReader = preferences.getUChar("RFIDReader",RFID_READER);

        MainsMeter.Type = preferences.getUChar("MainsMeter", MAINS_METER);
        MainsMeter.Address = preferences.getUChar("MainsMAddress",MAINS_METER_ADDRESS);
        EVMeter.Type = preferences.getUChar("EVMeter",EV_METER);
        EVMeter.Address = preferences.getUChar("EVMeterAddress",EV_METER_ADDRESS);
        EMConfig[EM_CUSTOM].Endianness = preferences.getUChar("EMEndianness",EMCUSTOM_ENDIANESS);
        EMConfig[EM_CUSTOM].IRegister = preferences.getUShort("EMIRegister",EMCUSTOM_IREGISTER);
        EMConfig[EM_CUSTOM].IDivisor = preferences.getUChar("EMIDivisor",EMCUSTOM_IDIVISOR);
        EMConfig[EM_CUSTOM].URegister = preferences.getUShort("EMURegister",EMCUSTOM_UREGISTER);
        EMConfig[EM_CUSTOM].UDivisor = preferences.getUChar("EMUDivisor",EMCUSTOM_UDIVISOR);
        EMConfig[EM_CUSTOM].PRegister = preferences.getUShort("EMPRegister",EMCUSTOM_PREGISTER);
        EMConfig[EM_CUSTOM].PDivisor = preferences.getUChar("EMPDivisor",EMCUSTOM_PDIVISOR);
        EMConfig[EM_CUSTOM].ERegister = preferences.getUShort("EMERegister",EMCUSTOM_EREGISTER);
        EMConfig[EM_CUSTOM].EDivisor = preferences.getUChar("EMEDivisor",EMCUSTOM_EDIVISOR);
        EMConfig[EM_CUSTOM].DataType = (mb_datatype)preferences.getUChar("EMDataType",EMCUSTOM_DATATYPE);
        EMConfig[EM_CUSTOM].Function = preferences.getUChar("EMFunction",EMCUSTOM_FUNCTION);
        WIFImode = preferences.getUChar("WIFImode",WIFI_MODE);
        DelayedStartTime.epoch2 = preferences.getULong("DelayedStartTim", DELAYEDSTARTTIME); //epoch2 is 4 bytes long on arduino; NVS key has reached max size
        DelayedStopTime.epoch2 = preferences.getULong("DelayedStopTime", DELAYEDSTOPTIME);    //epoch2 is 4 bytes long on arduino
        DelayedRepeat = preferences.getUShort("DelayedRepeat", 0);
        LCDlock = preferences.getUChar("LCDlock", LCD_LOCK);
        CableLock = preferences.getUChar("CableLock", CABLE_LOCK);
        LCDPin = preferences.getUShort("LCDPin", 0);
        AutoUpdate = preferences.getUChar("AutoUpdate", AUTOUPDATE);


        EnableC2 = (EnableC2_t) preferences.getUShort("EnableC2", ENABLE_C2);
#if MODEM
        strncpy(RequiredEVCCID, preferences.getString("RequiredEVCCID", "").c_str(), sizeof(RequiredEVCCID));
#endif
        maxTemp = preferences.getUShort("maxTemp", MAX_TEMPERATURE);

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
        OcppMode = preferences.getUChar("OcppMode", OCPP_MODE);
#endif //ENABLE_OCPP

        preferences.end();                                  

        // Store settings when not initialized
        if (!Initialized) write_settings();

    } else {
        _LOG_A("Can not open preferences!\n");
    }
}

void write_settings(void) {

    validate_settings();

 if (preferences.begin("settings", false) ) {

    preferences.putUChar("Config", Config); 
    preferences.putUChar("Lock", Lock); 
    preferences.putUChar("Mode", Mode); 
    preferences.putUChar("Access", AccessStatus);
    preferences.putUShort("CardOffs16", CardOffset);
    preferences.putUChar("LoadBl", LoadBl); 
    preferences.putUShort("MaxMains", MaxMains); 
    preferences.putUShort("MaxSumMains", MaxSumMains);
    preferences.putUShort("MaxSumMainsTime", MaxSumMainsTime);
    preferences.putUShort("MaxCurrent", MaxCurrent); 
    preferences.putUShort("MinCurrent", MinCurrent); 
    preferences.putUShort("MaxCircuit", MaxCircuit); 
    preferences.putUChar("Switch", Switch); 
    preferences.putUChar("RCmon", RCmon); 
    preferences.putUShort("StartCurrent", StartCurrent); 
    preferences.putUShort("StopTime", StopTime); 
    preferences.putUShort("ImportCurrent", ImportCurrent);
    preferences.putUChar("Grid", Grid);
    preferences.putUChar("SB2WIFImode", SB2_WIFImode);
    preferences.putUChar("RFIDReader", RFIDReader);

    preferences.putUChar("MainsMeter", MainsMeter.Type);
    preferences.putUChar("MainsMAddress", MainsMeter.Address);
    preferences.putUChar("EVMeter", EVMeter.Type);
    preferences.putUChar("EVMeterAddress", EVMeter.Address);
    preferences.putUChar("EMEndianness", EMConfig[EM_CUSTOM].Endianness);
    preferences.putUShort("EMIRegister", EMConfig[EM_CUSTOM].IRegister);
    preferences.putUChar("EMIDivisor", EMConfig[EM_CUSTOM].IDivisor);
    preferences.putUShort("EMURegister", EMConfig[EM_CUSTOM].URegister);
    preferences.putUChar("EMUDivisor", EMConfig[EM_CUSTOM].UDivisor);
    preferences.putUShort("EMPRegister", EMConfig[EM_CUSTOM].PRegister);
    preferences.putUChar("EMPDivisor", EMConfig[EM_CUSTOM].PDivisor);
    preferences.putUShort("EMERegister", EMConfig[EM_CUSTOM].ERegister);
    preferences.putUChar("EMEDivisor", EMConfig[EM_CUSTOM].EDivisor);
    preferences.putUChar("EMDataType", EMConfig[EM_CUSTOM].DataType);
    preferences.putUChar("EMFunction", EMConfig[EM_CUSTOM].Function);
    preferences.putUChar("WIFImode", WIFImode);
    preferences.putUShort("EnableC2", EnableC2);
#if MODEM
    preferences.putString("RequiredEVCCID", String(RequiredEVCCID));
#endif
    preferences.putUShort("maxTemp", maxTemp);
    preferences.putUChar("AutoUpdate", AutoUpdate);
    preferences.putUChar("LCDlock", LCDlock);
    preferences.putUChar("CableLock", CableLock);
    preferences.putUShort("LCDPin", LCDPin);

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
    preferences.putUChar("OcppMode", OcppMode);
#endif //ENABLE_OCPP

    preferences.end();

    _LOG_I("settings saved\n");
#if SMARTEVSE_VERSION >= 40
    SendConfigToCH32();
#endif

 } else {
     _LOG_A("Can not open preferences!\n");
 }


    if (LoadBl == 1) {                                                          // Master mode
        // Broadcast settings to other controllers
        BroadcastSettings();
    }

    ConfigChanged = 1;                                                          // FIXME this variable never reset to 0?
    SEND_TO_CH32(ConfigChanged);
}


/* Takes TimeString in format
 * String = "2023-04-14T11:31"
 * and store it in the DelayedTimeStruct
 * returns 0 on success, 1 on failure
*/
int StoreTimeString(String DelayedTimeStr, DelayedTimeStruct *DelayedTime) {
    // Parse the time string
    tm delayedtime_tm = {};
    if (strptime(DelayedTimeStr.c_str(), "%Y-%m-%dT%H:%M", &delayedtime_tm)) {
        delayedtime_tm.tm_isdst = -1;                 //so mktime is going to figure out whether DST is there or not
        DelayedTime->epoch2 = mktime(&delayedtime_tm) - EPOCH2_OFFSET;
        // Compare the times
        time_t now = time(nullptr);             //get current local time
        DelayedTime->diff = DelayedTime->epoch2 - (mktime(localtime(&now)) - EPOCH2_OFFSET);
        return 0;
    }
    //error TODO not sure whether we keep the old time or reset it to zero?
    //DelayedTime.epoch2 = 0;
    //DelayedTime.diff = 0;
    return 1;
}


#if MODEM
// Recompute State of Charge, in case we have a known initial state of charge
// This function is called by kWh logic and after an EV state update through API, Serial or MQTT
void RecomputeSoC(void) {
    if (InitialSoC > 0 && FullSoC > 0 && EnergyCapacity > 0) {
        if (InitialSoC == FullSoC) {
            // We're already at full SoC
            ComputedSoC = FullSoC;
            RemainingSoC = 0;
            TimeUntilFull = -1;
        } else {
            int EnergyRemaining = -1;
            int TargetEnergyCapacity = (FullSoC / 100.f) * EnergyCapacity;

            if (EnergyRequest > 0) {
                // Attempt to use EnergyRequest to determine SoC with greater accuracy
                EnergyRemaining = EVMeter.EnergyCharged > 0 ? (EnergyRequest - EVMeter.EnergyCharged) : EnergyRequest;
            } else {
                // We use a rough estimation based on FullSoC and EnergyCapacity
                EnergyRemaining = TargetEnergyCapacity - (EVMeter.EnergyCharged + (InitialSoC / 100.f) * EnergyCapacity);
            }

            RemainingSoC = ((FullSoC * EnergyRemaining) / TargetEnergyCapacity);
            ComputedSoC = RemainingSoC > 1 ? (FullSoC - RemainingSoC) : FullSoC;

            // Only attempt to compute the SoC and TimeUntilFull if we have a EnergyRemaining and PowerMeasured
            if (EnergyRemaining > -1) {
                int TimeToGo = -1;
                // Do a very simple estimation in seconds until car would reach FullSoC according to current charging power
                if (EVMeter.PowerMeasured > 0) {
                    // Use real-time PowerMeasured data if available
                    TimeToGo = (3600 * EnergyRemaining) / EVMeter.PowerMeasured;
                } else if (Mode != MODE_SOLAR) {
                    // Else, fall back on the theoretical maximum of the cable + nr of phases
                    TimeToGo = (3600 * EnergyRemaining) / (MaxCapacity * (Nr_Of_Phases_Charging * 230));
                }

                // Wait until we have a somewhat sensible estimation while still respecting granny chargers
                if (TimeToGo < 100000) {
                    TimeUntilFull = TimeToGo;
                }
            }

            // We can't possibly charge to over 100% SoC
            if (ComputedSoC > FullSoC) {
                ComputedSoC = FullSoC;
                RemainingSoC = 0;
                TimeUntilFull = -1;
            }

            _LOG_I("SoC: EnergyRemaining %i RemaningSoC %i EnergyRequest %i EnergyCharged %i EnergyCapacity %i ComputedSoC %i FullSoC %i TimeUntilFull %i TargetEnergyCapacity %i\n", EnergyRemaining, RemainingSoC, EnergyRequest, EVMeter.EnergyCharged, EnergyCapacity, ComputedSoC, FullSoC, TimeUntilFull, TargetEnergyCapacity);
        }
    } else {
        if (TimeUntilFull != -1) TimeUntilFull = -1;
    }
    // There's also the possibility an external API/app is used for SoC info. In such case, we allow setting ComputedSoC directly.
}


// EV disconnected from charger. Triggered after 60 seconds of disconnect
// This is done so we can "re-plug" the car in the Modem process without triggering disconnect events
void DisconnectEvent(void){
    _LOG_A("EV disconnected for a while. Resetting SoC states");
    uint8_t ModemStage = 0; // Enable Modem states again
    SEND_TO_CH32(ModemStage)
    InitialSoC = -1;
    FullSoC = -1;
    RemainingSoC = -1;
    ComputedSoC = -1;
    EnergyCapacity = -1;
    EnergyRequest = -1;
    TimeUntilFull = -1;
    strncpy(EVCCID, "", sizeof(EVCCID));
}
#endif //MODEM


//make mongoose 7.14 compatible with 7.13
#define mg_http_match_uri(X,Y) mg_match(X->uri, mg_str(Y), NULL)

// handles URI, returns true if handled, false if not
bool handle_URI(struct mg_connection *c, struct mg_http_message *hm,  webServerRequest* request) {
    static bool LCDPasswordOK = false;
//    if (mg_match(hm->uri, mg_str("/settings"), NULL)) {               // REST API call?
    if (mg_http_match_uri(hm, "/settings")) {                            // REST API call?
      if (!memcmp("GET", hm->method.buf, hm->method.len)) {                     // if GET
        String mode = "N/A";
        int modeId = -1;
        if(AccessStatus == OFF)  {
            mode = "OFF";
            modeId=0;
        } else if(AccessStatus == PAUSE)  {
            mode = "PAUSE";
            modeId=4;
        } else {
            switch(Mode) {
                case MODE_NORMAL: mode = "NORMAL"; modeId=1; break;
                case MODE_SOLAR: mode = "SOLAR"; modeId=2; break;
                case MODE_SMART: mode = "SMART"; modeId=3; break;
            }
        }
        if (mode == "N/A") //this should never happen, but it does
            _LOG_A("ERROR: mode=%s, Mode=%u, modeId=%d, AccessStatus=%u.\n", mode.c_str(), Mode, modeId, AccessStatus);

        String backlight = "N/A";
        switch(BacklightSet) {
            case 0: backlight = "OFF"; break;
            case 1: backlight = "ON"; break;
            case 2: backlight = "DIMMED"; break;
        }
        String evstate = StrStateNameWeb[State];
        String error = getErrorNameWeb(ErrorFlags);
        int errorId = getErrorId(ErrorFlags);

        if (ErrorFlags & LESS_6A) {
            evstate += " - " + error;
            error = "None";
            errorId = 0;
        }

        boolean evConnected = pilot != PILOT_12V;                    //when access bit = 1, p.ex. in OFF mode, the STATEs are no longer updated

        DynamicJsonDocument doc(3072); // https://arduinojson.org/v6/assistant/
        doc["version"] = String(VERSION);
        doc["serialnr"] = serialnr;
        doc["mode"] = mode;
        doc["mode_id"] = modeId;
        doc["car_connected"] = evConnected;

        if(WiFi.isConnected()) {
            switch(WiFi.status()) {
                case WL_NO_SHIELD:          doc["wifi"]["status"] = "WL_NO_SHIELD"; break;
                case WL_IDLE_STATUS:        doc["wifi"]["status"] = "WL_IDLE_STATUS"; break;
                case WL_NO_SSID_AVAIL:      doc["wifi"]["status"] = "WL_NO_SSID_AVAIL"; break;
                case WL_SCAN_COMPLETED:     doc["wifi"]["status"] = "WL_SCAN_COMPLETED"; break;
                case WL_CONNECTED:          doc["wifi"]["status"] = "WL_CONNECTED"; break;
                case WL_CONNECT_FAILED:     doc["wifi"]["status"] = "WL_CONNECT_FAILED"; break;
                case WL_CONNECTION_LOST:    doc["wifi"]["status"] = "WL_CONNECTION_LOST"; break;
                case WL_DISCONNECTED:       doc["wifi"]["status"] = "WL_DISCONNECTED"; break;
                default:                    doc["wifi"]["status"] = "UNKNOWN"; break;
            }

            doc["wifi"]["ssid"] = WiFi.SSID();    
            doc["wifi"]["rssi"] = WiFi.RSSI();    
            doc["wifi"]["bssid"] = WiFi.BSSIDstr();  
        }
        
        doc["evse"]["temp"] = TempEVSE;
        doc["evse"]["temp_max"] = maxTemp;
        doc["evse"]["connected"] = evConnected;
        doc["evse"]["access"] = AccessStatus;
        doc["evse"]["mode"] = Mode;
        doc["evse"]["loadbl"] = LoadBl;
        doc["evse"]["pwm"] = CurrentPWM;
        doc["evse"]["custombutton"] = CustomButton;
        doc["evse"]["solar_stop_timer"] = SolarStopTimer;
        doc["evse"]["state"] = evstate;
        doc["evse"]["state_id"] = State;
        doc["evse"]["error"] = error;
        doc["evse"]["error_id"] = errorId;
        doc["evse"]["rfid"] = !RFIDReader ? "Not Installed" : RFIDstatus >= 8 ? "NOSTATUS" : StrRFIDStatusWeb[RFIDstatus];
        if (RFIDReader) {
            char buf[15];
            printRFID(buf);
            doc["evse"]["rfid_lastread"] = buf;
        }

        doc["settings"]["charge_current"] = Balanced[0];
        doc["settings"]["override_current"] = OverrideCurrent;
        doc["settings"]["current_min"] = MinCurrent;
        doc["settings"]["current_max"] = MaxCurrent;
        doc["settings"]["current_main"] = MaxMains;
        doc["settings"]["current_max_circuit"] = MaxCircuit;
        doc["settings"]["current_max_sum_mains"] = MaxSumMains;
        doc["settings"]["max_sum_mains_time"] = MaxSumMainsTime;
        doc["settings"]["solar_max_import"] = ImportCurrent;
        doc["settings"]["solar_start_current"] = StartCurrent;
        doc["settings"]["solar_stop_time"] = StopTime;
        doc["settings"]["enable_C2"] = StrEnableC2[EnableC2];
        doc["settings"]["mains_meter"] = EMConfig[MainsMeter.Type].Desc;
        doc["settings"]["starttime"] = (DelayedStartTime.epoch2 ? DelayedStartTime.epoch2 + EPOCH2_OFFSET : 0);
        doc["settings"]["stoptime"] = (DelayedStopTime.epoch2 ? DelayedStopTime.epoch2 + EPOCH2_OFFSET : 0);
        doc["settings"]["repeat"] = DelayedRepeat;
        doc["settings"]["lcdlock"] = LCDlock;
        doc["settings"]["lock"] = Lock;
        doc["settings"]["cablelock"] = CableLock;
#if MODEM
            doc["settings"]["required_evccid"] = RequiredEVCCID;
#if SMARTEVSE_VERSION < 40
            doc["settings"]["modem"] = "Experiment";
#else
            doc["settings"]["modem"] = "QCA7000";
#endif
            doc["ev_state"]["initial_soc"] = InitialSoC;
            doc["ev_state"]["remaining_soc"] = RemainingSoC;
            doc["ev_state"]["full_soc"] = FullSoC;
            doc["ev_state"]["energy_capacity"] = EnergyCapacity > 0 ? round((float)EnergyCapacity / 100)/10 : -1; //in kWh, precision 1 decimal;
            doc["ev_state"]["energy_request"] = EnergyRequest > 0 ? round((float)EnergyRequest / 100)/10 : -1; //in kWh, precision 1 decimal
            doc["ev_state"]["computed_soc"] = ComputedSoC;
            doc["ev_state"]["evccid"] = EVCCID;
            doc["ev_state"]["time_until_full"] = TimeUntilFull;
#endif

#if MQTT && defined(SMARTEVSE_VERSION) // ESP32 only
        doc["mqtt"]["host"] = MQTTHost;
        doc["mqtt"]["port"] = MQTTPort;
        doc["mqtt"]["topic_prefix"] = MQTTprefix;
        doc["mqtt"]["username"] = MQTTuser;
        doc["mqtt"]["password_set"] = MQTTpassword != "";

        if (MQTTclient.connected) {
            doc["mqtt"]["status"] = "Connected";
        } else {
            doc["mqtt"]["status"] = "Disconnected";
        }
#endif

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
        doc["ocpp"]["mode"] = OcppMode ? "Enabled" : "Disabled";
        doc["ocpp"]["backend_url"] = OcppWsClient ? OcppWsClient->getBackendUrl() : "";
        doc["ocpp"]["cb_id"] = OcppWsClient ? OcppWsClient->getChargeBoxId() : "";
        doc["ocpp"]["auth_key"] = OcppWsClient ? OcppWsClient->getAuthKey() : "";

        {
            auto freevendMode = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendActive");
            doc["ocpp"]["auto_auth"] = freevendMode && freevendMode->getBool() ? "Enabled" : "Disabled";
            auto freevendIdTag = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendIdTag");
            doc["ocpp"]["auto_auth_idtag"] = freevendIdTag ? freevendIdTag->getString() : "";
        }

        if (OcppWsClient && OcppWsClient->isConnected()) {
            doc["ocpp"]["status"] = "Connected";
        } else {
            doc["ocpp"]["status"] = "Disconnected";
        }
#endif //ENABLE_OCPP

        doc["home_battery"]["current"] = homeBatteryCurrent;
        doc["home_battery"]["last_update"] = homeBatteryLastUpdate;

        //[rob040 20240819] Fixed: the net effect of "round(float/100)/10" is a Json value like 235.6999969 or 1.600000024; i.e. result in many decimals, i.s.o. just one.
        // When using FP constants, like "round(float/100.0)/10.0", no such rounding errors do occurr.
        doc["ev_meter"]["description"] = EMConfig[EVMeter.Type].Desc;
        doc["ev_meter"]["address"] = EVMeter.Address;
        doc["ev_meter"]["import_active_power"] = round((float)EVMeter.PowerMeasured / 100.0)/10.0; //in kW, precision 1 decimal
        doc["ev_meter"]["total_kwh"] = round((float)EVMeter.Energy / 100.0)/10.0; //in kWh, precision 1 decimal
        doc["ev_meter"]["charged_kwh"] = round((float)EVMeter.EnergyCharged / 100.0)/10.0; //in kWh, precision 1 decimal
        doc["ev_meter"]["currents"]["TOTAL"] = EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2];
        doc["ev_meter"]["currents"]["L1"] = EVMeter.Irms[0];
        doc["ev_meter"]["currents"]["L2"] = EVMeter.Irms[1];
        doc["ev_meter"]["currents"]["L3"] = EVMeter.Irms[2];
        doc["ev_meter"]["import_active_energy"] = round((float)EVMeter.Import_active_energy / 100.0)/10.0; //in kWh, precision 1 decimal
        doc["ev_meter"]["export_active_energy"] = round((float)EVMeter.Export_active_energy / 100.0)/10.0; //in kWh, precision 1 decimal

        doc["mains_meter"]["import_active_energy"] = round((float)MainsMeter.Import_active_energy / 100.0)/10.0; //in kWh, precision 1 decimal
        doc["mains_meter"]["export_active_energy"] = round((float)MainsMeter.Export_active_energy / 100.0)/10.0; //in kWh, precision 1 decimal
        if (MainsMeter.Type == EM_HOMEWIZARD_P1) {
            doc["mains_meter"]["host"] = !homeWizardHost.isEmpty() ? homeWizardHost : "HomeWizard P1 Not Found";
        }
          
        doc["phase_currents"]["TOTAL"] = MainsMeter.Irms[0] + MainsMeter.Irms[1] + MainsMeter.Irms[2];
        doc["phase_currents"]["L1"] = MainsMeter.Irms[0];
        doc["phase_currents"]["L2"] = MainsMeter.Irms[1];
        doc["phase_currents"]["L3"] = MainsMeter.Irms[2];
        doc["phase_currents"]["last_data_update"] = phasesLastUpdate;
        doc["phase_currents"]["original_data"]["TOTAL"] = IrmsOriginal[0] + IrmsOriginal[1] + IrmsOriginal[2];
        doc["phase_currents"]["original_data"]["L1"] = IrmsOriginal[0];
        doc["phase_currents"]["original_data"]["L2"] = IrmsOriginal[1];
        doc["phase_currents"]["original_data"]["L3"] = IrmsOriginal[2];
        
        doc["backlight"]["timer"] = BacklightTimer;
        doc["backlight"]["status"] = backlight;

        doc["color"]["off"]["R"] = ColorOff[0];
        doc["color"]["off"]["G"] = ColorOff[1];
        doc["color"]["off"]["B"] = ColorOff[2];
        doc["color"]["normal"]["R"] = ColorNormal[0];
        doc["color"]["normal"]["G"] = ColorNormal[1];
        doc["color"]["normal"]["B"] = ColorNormal[2];
        doc["color"]["smart"]["R"] = ColorSmart[0];
        doc["color"]["smart"]["G"] = ColorSmart[1];
        doc["color"]["smart"]["B"] = ColorSmart[2];
        doc["color"]["solar"]["R"] = ColorSolar[0];
        doc["color"]["solar"]["G"] = ColorSolar[1];
        doc["color"]["solar"]["B"] = ColorSolar[2];
        doc["color"]["custom"]["R"] = ColorCustom[0];
        doc["color"]["custom"]["G"] = ColorCustom[1];
        doc["color"]["custom"]["B"] = ColorCustom[2];

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", json.c_str());    // Yes. Respond JSON
        return true;
      } else if (!memcmp("POST", hm->method.buf, hm->method.len)) {                     // if POST
        if(request->hasParam("mqtt_update")) {
            return false;                                                       // handled in network.cpp
        }
        DynamicJsonDocument doc(512); // https://arduinojson.org/v6/assistant/

        if(request->hasParam("backlight")) {
            int backlight = request->getParam("backlight")->value().toInt();
            BacklightTimer = backlight * BACKLIGHT;
            doc["Backlight"] = backlight;
        }

        if(request->hasParam("current_min")) {
            int current = request->getParam("current_min")->value().toInt();
            if(current >= MIN_CURRENT && current <= 16 && LoadBl < 2) {
                MinCurrent = current;
                doc["current_min"] = MinCurrent;
            } else {
                doc["current_min"] = "Value not allowed!";
            }
        }

        if(request->hasParam("current_max_sum_mains")) {
            int current = request->getParam("current_max_sum_mains")->value().toInt();
            if((current == 0 || (current >= 10 && current <= 600)) && LoadBl < 2) {
                MaxSumMains = current;
                doc["current_max_sum_mains"] = MaxSumMains;
            } else {
                doc["current_max_sum_mains"] = "Value not allowed!";
            }
        }

        if(request->hasParam("max_sum_mains_timer")) {
            int time = request->getParam("max_sum_mains_timer")->value().toInt();
            if(time >= 0 && time <= 60 && LoadBl < 2) {
                MaxSumMainsTime = time;
                doc["max_sum_mains_time"] = MaxSumMainsTime;
            } else {
                doc["max_sum_mains_time"] = "Value not allowed!";
            }
        }

        if(request->hasParam("disable_override_current")) {
            setOverrideCurrent(0);
            doc["disable_override_current"] = "OK";
        }

        if(request->hasParam("custombutton")) {
            CustomButton = request->getParam("custombutton")->value().toInt() > 0;
            doc["custombutton"] = CustomButton;
        }

        if(request->hasParam("mode")) {
            String mode = request->getParam("mode")->value();

            //first check if we have a delayed mode switch
            if(request->hasParam("starttime")) {
                String DelayedStartTimeStr = request->getParam("starttime")->value();
                //string time_str = "2023-04-14T11:31";
                if (!StoreTimeString(DelayedStartTimeStr, &DelayedStartTime)) {
                    //parse OK
                    if (DelayedStartTime.diff > 0)
                        setAccess(OFF);                         //switch to OFF, we are Delayed Charging
                    else {//we are in the past so no delayed charging
                        DelayedStartTime.epoch2 = DELAYEDSTARTTIME;
                        DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                        DelayedRepeat = 0;
                    }
                }
                else {
                    //we couldn't parse the string, so we are NOT Delayed Charging
                    DelayedStartTime.epoch2 = DELAYEDSTARTTIME;
                    DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                    DelayedRepeat = 0;
                }

                // so now we might have a starttime and we might be Delayed Charging
                if (DelayedStartTime.epoch2) {
                    //we only accept a DelayedStopTime if we have a valid DelayedStartTime
                    if(request->hasParam("stoptime")) {
                        String DelayedStopTimeStr = request->getParam("stoptime")->value();
                        //string time_str = "2023-04-14T11:31";
                        if (!StoreTimeString(DelayedStopTimeStr, &DelayedStopTime)) {
                            //parse OK
                            if (DelayedStopTime.diff <= 0 || DelayedStopTime.epoch2 <= DelayedStartTime.epoch2)
                                //we are in the past or DelayedStopTime before DelayedStartTime so no DelayedStopTime
                                DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                        }
                        else
                            //we couldn't parse the string, so no DelayedStopTime
                            DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                        doc["stoptime"] = (DelayedStopTime.epoch2 ? DelayedStopTime.epoch2 + EPOCH2_OFFSET : 0);
                        if(request->hasParam("repeat")) {
                            int Repeat = request->getParam("repeat")->value().toInt();
                            if (Repeat >= 0 && Repeat <= 1) {                                   //boundary check
                                DelayedRepeat = Repeat;
                                doc["repeat"] = Repeat;
                            }
                        }
                    }

                }
                doc["starttime"] = (DelayedStartTime.epoch2 ? DelayedStartTime.epoch2 + EPOCH2_OFFSET : 0);
            } else
                DelayedStartTime.epoch2 = DELAYEDSTARTTIME;


            switch(mode.toInt()) {
                case 0: // OFF
                    Serial1.printf("@ResetModemTimers\n");
                    setAccess(OFF);
                    break;
                case 1:
                    setMode(MODE_NORMAL);
                    break;
                case 2:
                    setMode(MODE_SOLAR);
                    break;
                case 3:
                    setMode(MODE_SMART);
                    break;
                case 4: // PAUSE
                    setAccess(PAUSE);
                    break;
                default:
                    mode = "Value not allowed!";
            }
            doc["mode"] = mode;
        }

        if(request->hasParam("enable_C2")) {
            EnableC2 = (EnableC2_t) request->getParam("enable_C2")->value().toInt();
            doc["settings"]["enable_C2"] = StrEnableC2[EnableC2];
        }

        if(request->hasParam("stop_timer")) {
            int stop_timer = request->getParam("stop_timer")->value().toInt();

            if(stop_timer >= 0 && stop_timer <= 60) {
                StopTime = stop_timer;
                doc["stop_timer"] = true;
            } else {
                doc["stop_timer"] = false;
            }

        }

        if(Mode == MODE_NORMAL || Mode == MODE_SMART) {
            if(request->hasParam("override_current")) {
                int current = request->getParam("override_current")->value().toInt();
                if (LoadBl < 2 && (current == 0 || (current >= ( MinCurrent * 10 ) && current <= ( MaxCurrent * 10 )))) { //OverrideCurrent not possible on Slave
                    setOverrideCurrent(current);
                    doc["override_current"] = OverrideCurrent;
                } else {
                    doc["override_current"] = "Value not allowed!";
                }
            }
        }

        if(request->hasParam("solar_start_current")) {
            int current = request->getParam("solar_start_current")->value().toInt();
            if(current >= 0 && current <= 48) {
                StartCurrent = current;
                doc["solar_start_current"] = StartCurrent;
            } else {
                doc["solar_start_current"] = "Value not allowed!";
            }
        }

        if(request->hasParam("solar_max_import")) {
            int current = request->getParam("solar_max_import")->value().toInt();
            if(current >= 0 && current <= 48) {
                ImportCurrent = current;
                doc["solar_max_import"] = ImportCurrent;
            } else {
                doc["solar_max_import"] = "Value not allowed!";
            }
        }

        //special section to post stuff for experimenting with an ISO15118 modem
        if(request->hasParam("override_pwm")) {
            int pwm = request->getParam("override_pwm")->value().toInt();
            if (pwm == 0){
                PILOT_DISCONNECTED;
                CPDutyOverride = true;
            } else if (pwm < 0){
                PILOT_CONNECTED;
                CPDutyOverride = false;
                pwm = 100; // 10% until next loop, to be safe, corresponds to 6A
            } else{
                PILOT_CONNECTED;
                CPDutyOverride = true;
            }

            SetCPDuty(pwm);
            doc["override_pwm"] = pwm;
        }
#if MODEM
        //allow basic plug 'n charge based on evccid
        //if required_evccid is set to a value, SmartEVSE will only allow charging requests from said EVCCID
        if(request->hasParam("required_evccid")) {
            if (request->getParam("required_evccid")->value().length() <= 32) {
                strncpy(RequiredEVCCID, request->getParam("required_evccid")->value().c_str(), sizeof(RequiredEVCCID));
                doc["required_evccid"] = RequiredEVCCID;
                Serial1.printf("@RequiredEVCCID:%s\n", RequiredEVCCID);
            } else {
                doc["required_evccid"] = "EVCCID too long (max 32 char)";
            }
        }
#endif
        if(request->hasParam("lcdlock")) {
            int lock = request->getParam("lcdlock")->value().toInt();
            if (lock >= 0 && lock <= 1) {                                   //boundary check
                LCDlock = lock;
                doc["lcdlock"] = lock;
            }
        }

        if(request->hasParam("cablelock")) {
            int c_lock = request->getParam("cablelock")->value().toInt();
            if (c_lock >= 0 && c_lock <= 1) {                               //boundary check
                CableLock = c_lock;
                doc["cablelock"] = c_lock;
            }
        }

#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
        if(request->hasParam("ocpp_update")) {
            if (request->getParam("ocpp_update")->value().toInt() == 1) {

                if(request->hasParam("ocpp_mode")) {
                    OcppMode = request->getParam("ocpp_mode")->value().toInt();
                    doc["ocpp_mode"] = OcppMode;
                }

                if(request->hasParam("ocpp_backend_url")) {
                    if (OcppWsClient) {
                        OcppWsClient->setBackendUrl(request->getParam("ocpp_backend_url")->value().c_str());
                        doc["ocpp_backend_url"] = OcppWsClient->getBackendUrl();
                    } else {
                        doc["ocpp_backend_url"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_cb_id")) {
                    if (OcppWsClient) {
                        OcppWsClient->setChargeBoxId(request->getParam("ocpp_cb_id")->value().c_str());
                        doc["ocpp_cb_id"] = OcppWsClient->getChargeBoxId();
                    } else {
                        doc["ocpp_cb_id"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_auth_key")) {
                    if (OcppWsClient) {
                        OcppWsClient->setAuthKey(request->getParam("ocpp_auth_key")->value().c_str());
                        doc["ocpp_auth_key"] = OcppWsClient->getAuthKey();
                    } else {
                        doc["ocpp_auth_key"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_auto_auth")) {
                    auto freevendMode = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendActive");
                    if (freevendMode) {
                        freevendMode->setBool(request->getParam("ocpp_auto_auth")->value().toInt());
                        doc["ocpp_auto_auth"] = freevendMode->getBool() ? 1 : 0;
                    } else {
                        doc["ocpp_auto_auth"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_auto_auth_idtag")) {
                    auto freevendIdTag = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendIdTag");
                    if (freevendIdTag) {
                        freevendIdTag->setString(request->getParam("ocpp_auto_auth_idtag")->value().c_str());
                        doc["ocpp_auto_auth_idtag"] = freevendIdTag->getString();
                    } else {
                        doc["ocpp_auto_auth_idtag"] = "Can only update when OCPP enabled";
                    }
                }

                // Apply changes in OcppWsClient
                if (OcppWsClient) {
                    OcppWsClient->reloadConfigs();
                }
                MicroOcpp::configuration_save();
            }
        }
#endif //ENABLE_OCPP

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", json.c_str());    // Yes. Respond JSON
        write_settings();
        return true;
      }
    } else if (mg_http_match_uri(hm, "/color_off") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorOff[0] = R;
                ColorOff[1] = G;
                ColorOff[2] = B;
                doc["color"]["off"]["R"] = ColorOff[0];
                doc["color"]["off"]["G"] = ColorOff[1];
                doc["color"]["off"]["B"] = ColorOff[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_normal") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorNormal[0] = R;
                ColorNormal[1] = G;
                ColorNormal[2] = B;
                doc["color"]["normal"]["R"] = ColorNormal[0];
                doc["color"]["normal"]["G"] = ColorNormal[1];
                doc["color"]["normal"]["B"] = ColorNormal[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_smart") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorSmart[0] = R;
                ColorSmart[1] = G;
                ColorSmart[2] = B;
                doc["color"]["smart"]["R"] = ColorSmart[0];
                doc["color"]["smart"]["G"] = ColorSmart[1];
                doc["color"]["smart"]["B"] = ColorSmart[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_solar") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorSolar[0] = R;
                ColorSolar[1] = G;
                ColorSolar[2] = B;
                doc["color"]["solar"]["R"] = ColorSolar[0];
                doc["color"]["solar"]["G"] = ColorSolar[1];
                doc["color"]["solar"]["B"] = ColorSolar[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_custom") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorCustom[0] = R;
                ColorCustom[1] = G;
                ColorCustom[2] = B;
                doc["color"]["custom"]["R"] = ColorCustom[0];
                doc["color"]["custom"]["G"] = ColorCustom[1];
                doc["color"]["custom"]["B"] = ColorCustom[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/currents") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        if(request->hasParam("battery_current")) {
            if (LoadBl < 2) {
                homeBatteryCurrent = request->getParam("battery_current")->value().toInt();
                homeBatteryLastUpdate = time(NULL);
                doc["battery_current"] = homeBatteryCurrent;
            } else
                doc["battery_current"] = "not allowed on slave";
        }

        if(MainsMeter.Type == EM_API) {
            if(request->hasParam("L1") && request->hasParam("L2") && request->hasParam("L3")) {
                if (LoadBl < 2) {
#if SMARTEVSE_VERSION < 40 //v3
                    MainsMeter.Irms[0] = request->getParam("L1")->value().toInt();
                    MainsMeter.Irms[1] = request->getParam("L2")->value().toInt();
                    MainsMeter.Irms[2] = request->getParam("L3")->value().toInt();

                    CalcIsum();
                    MainsMeter.setTimeout(COMM_TIMEOUT);
#else  //v4
                    Serial1.printf("@Irms:%03u,%d,%d,%d\n", MainsMeter.Address, (int16_t) request->getParam("L1")->value().toInt(), (int16_t) request->getParam("L2")->value().toInt(), (int16_t) request->getParam("L3")->value().toInt()); //Irms:011,312,123,124 means: the meter on address 11(dec) has Irms[0] 312 dA, Irms[1] of 123 dA, Irms[2] of 124 dA
#endif
                    for (int x = 0; x < 3; x++) {
                        doc["original"]["L" + x] = IrmsOriginal[x];
                        doc["L" + x] = MainsMeter.Irms[x];
                    }
                    doc["TOTAL"] = Isum;

                } else
                    doc["TOTAL"] = "not allowed on slave";
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/ev_meter") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        if(EVMeter.Type == EM_API) {
            if(request->hasParam("L1") && request->hasParam("L2") && request->hasParam("L3")) {
#if SMARTEVSE_VERSION < 40 //v3
                EVMeter.Irms[0] = request->getParam("L1")->value().toInt();
                EVMeter.Irms[1] = request->getParam("L2")->value().toInt();
                EVMeter.Irms[2] = request->getParam("L3")->value().toInt();
                EVMeter.CalcImeasured();
                EVMeter.Timeout = COMM_EVTIMEOUT;
#else //v4
                Serial1.printf("@Irms:%03u,%d,%d,%d\n", EVMeter.Address, (int16_t) request->getParam("L1")->value().toInt(), (int16_t) request->getParam("L2")->value().toInt(), (int16_t) request->getParam("L3")->value().toInt()); //Irms:011,312,123,124 means: the meter on address 11(dec) has Irms[0] 312 dA, Irms[1] of 123 dA, Irms[2] of 124 dA
#endif
                for (int x = 0; x < 3; x++)
                    doc["ev_meter"]["currents"]["L" + x] = EVMeter.Irms[x];
                doc["ev_meter"]["currents"]["TOTAL"] = EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2];
            }

            if(request->hasParam("import_active_energy") && request->hasParam("export_active_energy") && request->hasParam("import_active_power")) {

                EVMeter.Import_active_energy = request->getParam("import_active_energy")->value().toInt();
                EVMeter.Export_active_energy = request->getParam("export_active_energy")->value().toInt();
#if SMARTEVSE_VERSION < 40 //v3
                EVMeter.PowerMeasured = request->getParam("import_active_power")->value().toInt();
#else //v4
                Serial1.printf("@PowerMeasured:%03u,%d\n", EVMeter.Address, (int16_t) request->getParam("import_active_power")->value().toInt());
#endif
                EVMeter.UpdateEnergies(); //we dont send the energies to CH32 because they are not used there
                doc["ev_meter"]["import_active_power"] = EVMeter.PowerMeasured;
                doc["ev_meter"]["import_active_energy"] = EVMeter.Import_active_energy;
                doc["ev_meter"]["export_active_energy"] = EVMeter.Export_active_energy;
                doc["ev_meter"]["total_kwh"] = EVMeter.Energy;
                doc["ev_meter"]["charged_kwh"] = EVMeter.EnergyCharged;
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;

    } else if (mg_http_match_uri(hm, "/lcd")) {
        if (strncmp("POST", hm->method.buf, hm->method.len) == 0) {
            DynamicJsonDocument doc(100);
            if (LCDPasswordOK) {
                const String btnName = request->getParam("button")->value();
                const bool btnDown = request->getParam("state")->value() == "1";

                // Button state bitmasks.
                static constexpr uint8_t RIGHT_MASK = 0b100;
                static constexpr uint8_t MIDDLE_MASK = 0b010;
                static constexpr uint8_t LEFT_MASK = 0b001;
                static constexpr uint8_t ALL_BUTTONS_UP = 0b111;
                static const std::unordered_map<std::string, uint8_t> btnMasks = {
                    {"right", RIGHT_MASK},
                    {"middle", MIDDLE_MASK},
                    {"left", LEFT_MASK}
                };

                xSemaphoreTake(buttonMutex, portMAX_DELAY);
                auto it = btnMasks.find(btnName.c_str());
                if (it != btnMasks.end()) {
                    // Clear bits if button is pressed, set bits if up.
                    const uint8_t mask = it->second;
                    if (btnDown) {
                        ButtonStateOverride = ALL_BUTTONS_UP & ~mask;
                    } else {
                        ButtonStateOverride = ALL_BUTTONS_UP | mask;
                    }
                    // Prevent stuck button in case we forget to reset to a 'down' button state.
                    LastBtnOverrideTime = millis();
                }
                xSemaphoreGive(buttonMutex);

                // Create JSON response
                doc["button"]["right"] = ButtonStateOverride & 4 ? "up" : "down";
                doc["button"]["middle"] = ButtonStateOverride & 2 ? "up" : "down";
                doc["button"]["left"] = ButtonStateOverride & 1 ? "up" : "down";
            } else { //LCDPasswordOK is false
                // Create JSON response; buttons are not pressed if we don't have the right password!
                doc["button"]["right"] = "down";
                doc["button"]["middle"] = "down";
                doc["button"]["left"] = "down";
            }
            // Serialize and send response
            String json;
            serializeJson(doc, json);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());
        } else {
            // Generate BMP image from LCD buffer.
    		const std::vector<uint8_t> bmpImage = createImageFromGLCDBuffer();
		    const size_t bmpImageSize = bmpImage.size();

            // Start the HTTP response with chunked encoding
            mg_printf(c,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/bmp\r\n"
                      "Connection: keep-alive\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n");

            // Using chunked transfer encoding to get rid of content-len + keep-alive problems.
            mg_http_write_chunk(c, reinterpret_cast<const char *>(bmpImage.data()), bmpImageSize);

            // Send an empty chunk to signal the end of the response.
            mg_http_write_chunk(c, "", 0);
        }
        return true;

    } else if (mg_http_match_uri(hm, "/lcd-verify-password") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        char password[32];
        mg_http_get_var(&hm->body, "password", password, sizeof(password));
        DynamicJsonDocument doc(256);

        LCDPasswordOK = (atoi(password) == LCDPin);
        if (LCDPasswordOK) {
            doc["success"] = true;
        } else {
            doc["success"] = false;
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());
        return true;


    } else if (mg_http_match_uri(hm, "/cablelock") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        if(request->hasParam("1")) {
            CableLock = 1;
            doc["cablelock"] = CableLock;
        } else {
            CableLock = 0;
            doc["cablelock"] = CableLock;
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;

#if MODEM && SMARTEVSE_VERSION < 40 
    } else if (mg_http_match_uri(hm, "/ev_state") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        //State of charge posting
        int current_soc = request->getParam("current_soc")->value().toInt();
        int full_soc = request->getParam("full_soc")->value().toInt();

        // Energy requested by car
        int energy_request = request->getParam("energy_request")->value().toInt();

        // Total energy capacity of car's battery
        int energy_capacity = request->getParam("energy_capacity")->value().toInt();

        // Update EVCCID of car
        if (request->hasParam("evccid")) {
            if (request->getParam("evccid")->value().length() <= 32) {
                strncpy(EVCCID, request->getParam("evccid")->value().c_str(), sizeof(EVCCID));
                doc["evccid"] = EVCCID;
            }
        }

        if (full_soc >= FullSoC) // Only update if we received it, since sometimes it's there, sometimes it's not
            FullSoC = full_soc;

        if (energy_capacity >= EnergyCapacity) // Only update if we received it, since sometimes it's there, sometimes it's not
            EnergyCapacity = energy_capacity;

        if (energy_request >= EnergyRequest) // Only update if we received it, since sometimes it's there, sometimes it's not
            EnergyRequest = energy_request;

        if (current_soc >= 0 && current_soc <= 100) {
            // We set the InitialSoC for our own calculations
            InitialSoC = current_soc;

            // We also set the ComputedSoC to allow for app integrations
            ComputedSoC = current_soc;

            // Skip waiting, charge since we have what we've got
            if (State == STATE_MODEM_REQUEST || State == STATE_MODEM_WAIT || State == STATE_MODEM_DONE){
                _LOG_A("Received SoC via REST. Shortcut to State Modem Done\n");
                setState(STATE_MODEM_DONE); // Go to State B, which means in this case setting PWM
            }
        }

        RecomputeSoC();

        doc["current_soc"] = current_soc;
        doc["full_soc"] = full_soc;
        doc["energy_capacity"] = energy_capacity;
        doc["energy_request"] = energy_request;

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
#endif
#if MODEM && SMARTEVSE_VERSION >= 40
    } else if (mg_http_match_uri(hm, "/ev_state") && !memcmp("GET", hm->method.buf, hm->method.len)) {
        //this can be activated by: curl -X GET "http://smartevse-xxxx.lan/ev_state?update_ev_state=1" -d ''
        uint8_t GetState = 0;
        if(request->hasParam("update_ev_state")) {
            GetState = strtol(request->getParam("update_ev_state")->value().c_str(),NULL,0);
            if (GetState)
                setState(STATE_MODEM_REQUEST);
        }
        _LOG_A("DEBUG: GetState=%u.\n", GetState);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", ""); //json request needs json response
        return true;
#endif

#if FAKE_RFID
    //this can be activated by: http://smartevse-xxx.lan/debug?showrfid=1
    } else if (mg_http_match_uri(hm, "/debug") && !memcmp("GET", hm->method.buf, hm->method.len)) {
        if(request->hasParam("showrfid")) {
            Show_RFID = strtol(request->getParam("showrfid")->value().c_str(),NULL,0);
        }
        _LOG_A("DEBUG: Show_RFID=%u.\n",Show_RFID);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", ""); //json request needs json response
        return true;
#endif

#if AUTOMATED_TESTING
    //this can be activated by: http://smartevse-xxx.lan/automated_testing?current_max=100
    //WARNING: because of automated testing, no limitations here!
    //THAT IS DANGEROUS WHEN USED IN PRODUCTION ENVIRONMENT
    //FOR SMARTEVSE's IN A TESTING BENCH ONLY!!!!
    } else if (mg_http_match_uri(hm, "/automated_testing") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        if(request->hasParam("current_max")) {
            MaxCurrent = strtol(request->getParam("current_max")->value().c_str(),NULL,0);
            SEND_TO_CH32(MaxCurrent)
        }
        if(request->hasParam("current_main")) {
            MaxMains = strtol(request->getParam("current_main")->value().c_str(),NULL,0);
            SEND_TO_CH32(MaxMains)
        }
        if(request->hasParam("current_max_circuit")) {
            MaxCircuit = strtol(request->getParam("current_max_circuit")->value().c_str(),NULL,0);
            SEND_TO_CH32(MaxCircuit)
        }
        if(request->hasParam("mainsmeter")) {
            MainsMeter.Type = strtol(request->getParam("mainsmeter")->value().c_str(),NULL,0);
            Serial1.printf("@MainsMeterType:%u\n", MainsMeter.Type);
        }
        if(request->hasParam("evmeter")) {
            EVMeter.Type = strtol(request->getParam("evmeter")->value().c_str(),NULL,0);
            Serial1.printf("@EVMeterType:%u\n", EVMeter.Type);
        }
        if(request->hasParam("config")) {
            Config = strtol(request->getParam("config")->value().c_str(),NULL,0);
            SEND_TO_CH32(Config)
            setState(STATE_A);                                                  // so the new value will actually be read
        }
        if(request->hasParam("loadbl")) {
            int LBL = strtol(request->getParam("loadbl")->value().c_str(),NULL,0);
#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
            ConfigureModbusMode(LBL);
#endif
            LoadBl = LBL;
            SEND_TO_CH32(LoadBl)
        }
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", ""); //json request needs json response
        return true;
#endif
  }
  return false;
}


/*
 * OCPP-related function definitions
 */
#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32

void ocppUpdateRfidReading(const unsigned char *uuid, size_t uuidLen) {
    if (!uuid || uuidLen > sizeof(OcppRfidUuid)) {
        _LOG_W("OCPP: invalid UUID\n");
        return;
    }
    memcpy(OcppRfidUuid, uuid, uuidLen);
    OcppRfidUuidLen = uuidLen;
    OcppLastRfidUpdate = millis();
}

bool ocppIsConnectorPlugged() {
    return OcppTrackCPvoltage >= PILOT_3V && OcppTrackCPvoltage <= PILOT_9V;
}

bool ocppHasTxNotification() {
    return OcppDefinedTxNotification && millis() - OcppLastTxNotification <= 3000;
}

MicroOcpp::TxNotification ocppGetTxNotification() {
    return OcppTrackTxNotification;
}

bool ocppLockingTxDefined() {
    return OcppLockingTx != nullptr;
}

void ocppInit() {

    //load OCPP library modules: Mongoose WS adapter and Core OCPP library

    auto filesystem = MicroOcpp::makeDefaultFilesystemAdapter(
            MicroOcpp::FilesystemOpt::Use_Mount_FormatOnFail // Enable FS access, mount LittleFS here, format data partition if necessary
            );

    OcppWsClient = new MicroOcpp::MOcppMongooseClient(
            &mgr,
            nullptr,    // OCPP backend URL (factory default)
            nullptr,    // ChargeBoxId (factory default)
            nullptr,    // WebSocket Basic Auth token (factory default)
            nullptr,    // CA cert (cert string must outlive WS client)
            filesystem);

    mocpp_initialize(
            *OcppWsClient, //WebSocket adapter for MicroOcpp
            ChargerCredentials("SmartEVSE", "Stegen Electronics", VERSION, String(serialnr).c_str(), NULL, (char *) EMConfig[EVMeter.Type].Desc),
            filesystem);

    //setup OCPP hardware bindings

    setEnergyMeterInput([] () { //Input of the electricity meter register in Wh
        return EVMeter.Energy;
    });

    setPowerMeterInput([] () { //Input of the power meter reading in W
        return EVMeter.PowerMeasured;
    });

    setConnectorPluggedInput([] () { //Input about if an EV is plugged to this EVSE
        return ocppIsConnectorPlugged();
    });

    setEvReadyInput([] () { //Input if EV is ready to charge (= J1772 State C)
        return OcppTrackCPvoltage >= PILOT_3V && OcppTrackCPvoltage <= PILOT_6V;
    });

    setEvseReadyInput([] () { //Input if EVSE allows charge (= PWM signal on)
        return GetCurrent() > 0; //PWM is enabled
    });

    addMeterValueInput([] () {
            return (float) (EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2])/10;
        },
        "Current.Import",
        "A");

    addMeterValueInput([] () {
            return (float) EVMeter.Irms[0]/10;
        },
        "Current.Import",
        "A",
        nullptr, // Location defaults to "Outlet"
        "L1");

    addMeterValueInput([] () {
            return (float) EVMeter.Irms[1]/10;
        },
        "Current.Import",
        "A",
        nullptr, // Location defaults to "Outlet"
        "L2");

    addMeterValueInput([] () {
            return (float) EVMeter.Irms[2]/10;
        },
        "Current.Import",
        "A",
        nullptr, // Location defaults to "Outlet"
        "L3");

    addMeterValueInput([] () {
            return (float)GetCurrent() * 0.1f;
        },
        "Current.Offered",
        "A");

    addMeterValueInput([] () {
            return (float)TempEVSE;
        },
        "Temperature",
        "Celsius");

#if MODEM
        addMeterValueInput([] () {
                return (float)ComputedSoC;
            },
            "SoC",
            "Percent");
#endif

    addErrorCodeInput([] () {
        return (ErrorFlags & TEMP_HIGH) ? "HighTemperature" : (const char*)nullptr;
    });

    addErrorCodeInput([] () {
        return (ErrorFlags & RCM_TRIPPED) ? "GroundFailure" : (const char*)nullptr;
    });

    addErrorDataInput([] () -> MicroOcpp::ErrorData {
        if (ErrorFlags & CT_NOCOMM) {
            MicroOcpp::ErrorData error = "PowerMeterFailure";
            error.info = "Communication with mains meter lost";
            return error;
        }
        return nullptr;
    });

    addErrorDataInput([] () -> MicroOcpp::ErrorData {
        if (ErrorFlags & EV_NOCOMM) {
            MicroOcpp::ErrorData error = "PowerMeterFailure";
            error.info = "Communication with EV meter lost";
            return error;
        }
        return nullptr;
    });

    // If SmartEVSE load balancer is turned off, then enable OCPP Smart Charging
    // This means after toggling LB, OCPP must be disabled and enabled for changes to become effective
    if (!LoadBl) {
        setSmartChargingCurrentOutput([] (float currentLimit) {
            OcppCurrentLimit = currentLimit; // Can be negative which means that no limit is defined

            // Re-evaluate charge rate and apply
            if (!LoadBl) { // Execute only if LB is still disabled

                CalcBalancedCurrent(0);
                if (IsCurrentAvailable()) {
                    // OCPP is the exclusive LB, clear LESS_6A error if set
                    clearErrorFlags(LESS_6A);
                    setChargeDelay(0);
                }
                if ((State == STATE_B || State == STATE_C) && !CPDutyOverride) {
                    if (IsCurrentAvailable()) {
                        SetCurrent(ChargeCurrent);
                    } else {
                        setStatePowerUnavailable();
                    }
                }
            }
        });
    }

    setOnUnlockConnectorInOut([] () -> UnlockConnectorResult {
        // MO also stops transaction which should toggle OcppForcesLock false
        OcppLockingTx.reset();
        if (Lock == 0 || digitalRead(PIN_LOCK_IN) == (Lock == 2 ? 0:1 )) {
            // Success
            return UnlockConnectorResult_Unlocked;
        }

        // No result yet, wait (MO eventually times out)
        return UnlockConnectorResult_Pending;
    });

    setOccupiedInput([] () -> bool {
        // Keep Finishing state while LockingTx effectively blocks new transactions
        return OcppLockingTx != nullptr;
    });

    setStopTxReadyInput([] () {
        // Stop value synchronization: block StopTransaction for 5 seconds to give the Modbus readings some time to come through
        return millis() - OcppStopReadingSyncTime >= 5000;
    });

    setTxNotificationOutput([] (MicroOcpp::Transaction*, MicroOcpp::TxNotification event) {
        OcppDefinedTxNotification = true;
        OcppTrackTxNotification = event;
        OcppLastTxNotification = millis();
    });

    OcppUnlockConnectorOnEVSideDisconnect = MicroOcpp::declareConfiguration<bool>("UnlockConnectorOnEVSideDisconnect", true);

    endTransaction(nullptr, "PowerLoss"); // If a transaction from previous power cycle is still running, abort it here
}

void ocppDeinit() {

    // Record stop value for transaction manually (normally MO would wait until `mocpp_loop()`, but that's too late here)
    if (auto& tx = getTransaction()) {
        if (tx->getMeterStop() < 0) {
            // Stop value not defined yet
            tx->setMeterStop(EVMeter.Import_active_energy); // Use same reading as in `setEnergyMeterInput()`
            tx->setStopTimestamp(getOcppContext()->getModel().getClock().now());
        }
    }

    endTransaction(nullptr, "Other"); // If a transaction is running, shut it down forcefully. The StopTx request will be sent when OCPP runs again.

    OcppUnlockConnectorOnEVSideDisconnect.reset();
    OcppLockingTx.reset();
    OcppForcesLock = false;

    if (OcppTrackPermitsCharge) {
        _LOG_A("OCPP unset Access_bit\n");
        setAccess(OFF);
    }

    OcppTrackPermitsCharge = false;
    OcppTrackAccessBit = false;
    OcppTrackCPvoltage = PILOT_NOK;
    OcppCurrentLimit = -1.f;

    mocpp_deinitialize();

    delete OcppWsClient;
    OcppWsClient = nullptr;
}

void ocppLoop() {

    if (pilot >= PILOT_3V && pilot <= PILOT_12V) {
        OcppTrackCPvoltage = pilot;
    }

    mocpp_loop();

    //handle RFID input

    if (OcppTrackLastRfidUpdate != OcppLastRfidUpdate) {
        // New RFID card swiped

        char uuidHex [2 * sizeof(OcppRfidUuid) + 1];
        uuidHex[0] = '\0';
        for (size_t i = 0; i < OcppRfidUuidLen; i++) {
            snprintf(uuidHex + 2*i, 3, "%02X", OcppRfidUuid[i]);
        }

        if (OcppLockingTx) {
            // Connector is still locked by earlier transaction

            if (!strcmp(uuidHex, OcppLockingTx->getIdTag())) {
                // Connector can be unlocked again
                OcppLockingTx.reset();
                endTransaction(uuidHex, "Local");
            } // else: Connector remains blocked for now
        } else if (getTransaction()) {
            //OCPP lib still has transaction (i.e. transaction running or authorization pending) --> swiping card again invalidates idTag
            endTransaction(uuidHex, "Local");
        } else {
            //OCPP lib has no idTag --> swiped card is used for new transaction
            OcppLockingTx = beginTransaction(uuidHex);
        }
    }
    OcppTrackLastRfidUpdate = OcppLastRfidUpdate;

    // Set / unset Access_bit
    // Allow to set Access_bit only once per OCPP transaction because other modules may override the Access_bit
    // Doesn't apply if SmartEVSE built-in RFID store is enabled
    if (RFIDReader == 6 || RFIDReader == 0) {
        // RFID reader in OCPP mode or RFID fully disabled - OCPP controls Access_bit
        if (!OcppTrackPermitsCharge && ocppPermitsCharge()) {
            _LOG_A("OCPP set Access_bit\n");
            setAccess(ON);
        } else if (AccessStatus == ON && !ocppPermitsCharge()) {
            _LOG_A("OCPP unset Access_bit\n");
            setAccess(OFF);
        }
        OcppTrackPermitsCharge = ocppPermitsCharge();

        // Check if OCPP charge permission has been revoked by other module
        if (OcppTrackPermitsCharge && // OCPP has set Acess_bit and still allows charge
                AccessStatus == OFF) { // Access_bit is not active anymore
            endTransaction(nullptr, "Other");
        }
    } else {
        // Built-in RFID store enabled - OCPP does not control Access_bit, but starts transactions when Access_bit is set
        if ((AccessStatus == ON || AccessStatus == PAUSE ) && !OcppTrackAccessBit && !getTransaction() && isOperative()) {
            // Access_bit has been set
            OcppTrackAccessBit = true;
            _LOG_A("OCPP detected Access_bit set\n");
            char buf[15];
            if (RFID[0] == 0x01) {  // old reader 6 byte UID starts at RFID[1]
                sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            } else {
                sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X", RFID[0], RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            }
            beginTransaction_authorized(buf);
        } else if (AccessStatus == OFF && (OcppTrackAccessBit || (getTransaction() && getTransaction()->isActive()))) {
            OcppTrackAccessBit = false;
            _LOG_A("OCPP detected Access_bit unset\n");
            char buf[15];
            if (RFID[0] == 0x01) {  // old reader 6 byte UID starts at RFID[1]
                sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            } else {
                sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X", RFID[0], RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            }
            endTransaction_authorized(buf);
        }
    }

    // Stop value synchronization: block StopTransaction for a short period as long as charging is permitted
    if (ocppPermitsCharge()) {
        OcppStopReadingSyncTime = millis();
    }

    auto& transaction = getTransaction(); // Common tx which OCPP is currently processing (or nullptr if no tx is ongoing)

    // Check if Locking Tx has been invalidated by something other than RFID swipe
    if (OcppLockingTx) {
        if (OcppUnlockConnectorOnEVSideDisconnect->getBool() && !OcppLockingTx->isActive()) {
            // No LockingTx mode configured (still, keep LockingTx until end of transaction because the config could be changed in the middle of tx)
            OcppLockingTx.reset();
        } else if (OcppLockingTx->isAborted()) {
            // LockingTx hasn't successfully started
            OcppLockingTx.reset();
        } else if (transaction && transaction != OcppLockingTx) {
            // Another Tx has already started
            OcppLockingTx.reset();
        } else if (digitalRead(PIN_LOCK_IN) == (Lock == 2 ? 0:1 ) && !OcppLockingTx->isActive()) {
            // Connector is has been unlocked and LockingTx has already run
            OcppLockingTx.reset();
        } // There may be further edge cases
    }

    OcppForcesLock = false;

    if (transaction && transaction->isAuthorized() && (transaction->isActive() || transaction->isRunning()) && // Common tx ongoing
            (OcppTrackCPvoltage >= PILOT_3V && OcppTrackCPvoltage <= PILOT_9V)) { // Connector plugged
        OcppForcesLock = true;
    }

    if (OcppLockingTx && OcppLockingTx->getStartSync().isRequested()) { // LockingTx goes beyond tx completion
        OcppForcesLock = true;
    }

}
#endif //ENABLE_OCPP


#if SMARTEVSE_VERSION >=40
void WCHUPDATE(unsigned long RunningVersion) {
        // we reset before flashing because when the WCH chip is sending messages (by printf) the programming can fail
        _LOG_D("reset WCH ic\n");
        WchReset();
        if (WchFirmwareUpdate(RunningVersion)) {
            _LOG_A("Firmware update failed.\n");
        } else { 
            _LOG_D("WCH programming done\n");
        }    
        // should not be needed to reset the WCH ic at powerup/reset on the production version.
        _LOG_D("reset WCH ic\n");
        WchReset();
}
#endif


void setup() {
#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40

    pinMode(PIN_CP_OUT, OUTPUT);            // CP output
    //pinMode(PIN_SW_IN, INPUT);            // SW Switch input, handled by OneWire32 class
    pinMode(PIN_SSR, OUTPUT);               // SSR1 output
    pinMode(PIN_SSR2, OUTPUT);              // SSR2 output
    pinMode(PIN_RCM_FAULT, INPUT_PULLUP);   

    pinMode(PIN_LCD_LED, OUTPUT);           // LCD backlight
    pinMode(PIN_LCD_RST, OUTPUT);           // LCD reset
    pinMode(PIN_IO0_B1, INPUT);             // < button
    pinMode(PIN_LCD_A0_B2, OUTPUT);         // o Select button + A0 LCD
    pinMode(PIN_LCD_SDO_B3, OUTPUT);        // > button + SDA/MOSI pin

    pinMode(PIN_LOCK_IN, INPUT);            // Locking Solenoid input
    pinMode(PIN_LEDR, OUTPUT);              // Red LED output
    pinMode(PIN_LEDG, OUTPUT);              // Green LED output
    pinMode(PIN_LEDB, OUTPUT);              // Blue LED output
    pinMode(PIN_ACTA, OUTPUT);              // Actuator Driver output R
    pinMode(PIN_ACTB, OUTPUT);              // Actuator Driver output W
    pinMode(PIN_CPOFF, OUTPUT);             // Disable CP output (active high)
    pinMode(PIN_RS485_RX, INPUT);
    pinMode(PIN_RS485_TX, OUTPUT);
    pinMode(PIN_RS485_DIR, OUTPUT);

    digitalWrite(PIN_LEDR, LOW);
    digitalWrite(PIN_LEDG, LOW);
    digitalWrite(PIN_LEDB, LOW);
    digitalWrite(PIN_ACTA, LOW);
    digitalWrite(PIN_ACTB, LOW);        
    digitalWrite(PIN_SSR, LOW);             // SSR1 OFF
    digitalWrite(PIN_SSR2, LOW);            // SSR2 OFF
    digitalWrite(PIN_LCD_LED, HIGH);        // LCD Backlight ON
    PILOT_DISCONNECTED;                     // CP signal OFF

 
    // Uart 0 debug/program port
    Serial.begin(115200);
    while (!Serial);
    _LOG_A("SmartEVSE v3 powerup\n");

    // configure SPI connection to LCD
    // only the SPI_SCK and SPI_MOSI pins are used
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_SS);
    // the ST7567's max SPI Clock frequency is 20Mhz at 3.3V/25C
    // We choose 10Mhz here, to reserve some room for error.
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge)
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    

    // The CP (control pilot) output is a fixed 1khz square-wave (+6..9v / -12v).
    // It's pulse width varies between 10% and 96% indicating 6A-80A charging current.
    // to detect state changes we should measure the CP signal while it's at ~5% (so 50uS after the positive pulse started)
    // we use an i/o interrupt at the CP pin output, and a one shot timer interrupt to start the ADC conversion.
    // would be nice if there was an easier way...

    // setup timer, and one shot timer interrupt to 50us
    timerA = timerBegin(0, 80, true);
    timerAttachInterrupt(timerA, &onTimerA, false);
    // we start in STATE A, with a static +12V CP signal
    // set alarm to trigger every 1mS, and let it reload every 1ms
    timerAlarmWrite(timerA, PWM_100, true);
    // when PWM is active, we sample the CP pin after 5% 
    timerAlarmEnable(timerA);


    // Setup ADC on CP, PP and Temperature pin
    adc1_config_width(ADC_WIDTH_BIT_10);                                    // 10 bits ADC resolution is enough
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_12);             // setup the CP pin input attenuation to 11db
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_6);              // setup the PP pin input attenuation to 6db
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_6);              // setup the Temperature input attenuation to 6db

    //Characterize the ADC at particular attentuation for each channel
    adc_chars_CP = (esp_adc_cal_characteristics_t *) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    adc_chars_PP = (esp_adc_cal_characteristics_t *) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    adc_chars_Temperature = (esp_adc_cal_characteristics_t *) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_10, 1100, adc_chars_CP);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_10, 1100, adc_chars_PP);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_10, 1100, adc_chars_Temperature);
          
    
    // Setup PWM on channel 0, 1000Hz, 10 bits resolution
    ledcSetup(CP_CHANNEL, 1000, 10);            // channel 0  => Group: 0, Channel: 0, Timer: 0
    // setup the RGB led PWM channels
    // as PWM channel 1 is used by the same timer as the CP timer (channel 0), we start with channel 2
    ledcSetup(RED_CHANNEL, 5000, 8);            // R channel 2, 5kHz, 8 bit
    ledcSetup(GREEN_CHANNEL, 5000, 8);          // G channel 3, 5kHz, 8 bit
    ledcSetup(BLUE_CHANNEL, 5000, 8);           // B channel 4, 5kHz, 8 bit
    ledcSetup(LCD_CHANNEL, 5000, 8);            // LCD channel 5, 5kHz, 8 bit

    // attach the channels to the GPIO to be controlled
    ledcAttachPin(PIN_CP_OUT, CP_CHANNEL);      
    //pinMode(PIN_CP_OUT, OUTPUT);                // Re-init the pin to output, required in order for attachInterrupt to work (2.0.2)
                                                // not required/working on master branch..
                                                // see https://github.com/espressif/arduino-esp32/issues/6140
    ledcAttachPin(PIN_LEDR, RED_CHANNEL);
    ledcAttachPin(PIN_LEDG, GREEN_CHANNEL);
    ledcAttachPin(PIN_LEDB, BLUE_CHANNEL);
    ledcAttachPin(PIN_LCD_LED, LCD_CHANNEL);

    SetCPDuty(1024);                            // channel 0, duty cycle 100%
    ledcWrite(RED_CHANNEL, 255);
    ledcWrite(GREEN_CHANNEL, 0);
    ledcWrite(BLUE_CHANNEL, 255);
    ledcWrite(LCD_CHANNEL, 0);

    // Setup PIN interrupt on rising edge
    // the timer interrupt will be reset in the ISR.
    attachInterrupt(PIN_CP_OUT, onCPpulse, RISING);   
   
    // Uart 1 is used for Modbus @ 9600 8N1
    RTUutils::prepareHardwareSerial(Serial1);
    Serial1.begin(MODBUS_BAUDRATE, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);

   
    //Check type of calibration value used to characterize ADC
    _LOG_A("Checking eFuse Vref settings: ");
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        _LOG_A_NO_FUNC("OK\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        _LOG_A_NO_FUNC("Two Point\n");
    } else {
        _LOG_A_NO_FUNC("not programmed!!!\n");
    }
    

#else //SMARTEVSE_VERSION v4

    //lower the CPU frequency to 160, 80, 40 MHz
    setCpuFrequencyMhz(160);

    pinMode(PIN_QCA700X_CS, OUTPUT);           // SPI_CS QCA7005
    pinMode(PIN_QCA700X_INT, INPUT);           // SPI_INT QCA7005
    pinMode(SPI_SCK, OUTPUT);
    pinMode(SPI_MISO, INPUT);
    pinMode(SPI_MOSI, OUTPUT);
    pinMode(PIN_QCA700X_RESETN, OUTPUT);

    pinMode(BUTTON1, INPUT_PULLUP);
    pinMode(BUTTON3, INPUT_PULLUP);

    pinMode(LCD_LED, OUTPUT);               // LCD backlight
    pinMode(PIN_LCD_RST, OUTPUT);           // LCD reset, active high
    pinMode(LCD_SDA, OUTPUT);               // LCD Data
    pinMode(LCD_SCK, OUTPUT);               // LCD Clock
    pinMode(PIN_LCD_A0_B2, OUTPUT);             // Select button + A0 LCD
    pinMode(LCD_CS, OUTPUT);

    pinMode(WCH_SWDIO, INPUT);              // WCH-Link (unused/unconnected)
    pinMode(WCH_SWCLK, INPUT);              // WCH-Link (unused) / BOOT0 select
    pinMode(WCH_NRST, INPUT);               // WCH NRST


    // shutdown QCA is done by the WCH32V, we set all IO pins low, so no current is flowing into the powered down chip.
    digitalWrite(PIN_QCA700X_CS, LOW);
    digitalWrite(PIN_QCA700X_RESETN, LOW);
    digitalWrite(SPI_SCK, LOW);
    digitalWrite(SPI_MOSI, LOW);

    // configure SPI connection to QCA modem
    QCA_SPI1.begin(SPI_SCK, SPI_MISO, SPI_MOSI, PIN_QCA700X_CS);
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge), we use a 10Mhz SPI clock
    QCA_SPI1.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    //attachInterrupt(digitalPinToInterrupt(PIN_QCA700X_INT), SPI_InterruptHandler, RISING);

    // Setup SWDIO pin as Power Panic interrupt received from the WCH uC. (unused, we use serial comm)
    //attachInterrupt(WCH_SWDIO, PowerPanicESP, FALLING);

    Serial.setTxBufferSize(2048);                                       // prevent error message: [HWCDC.cpp:467] write(): write failed due to waiting USB Host - timeout
    Serial.begin();                                                     // Debug output on USB
    Serial.setTxTimeoutMs(1);                                           // Workaround for Serial.print while unplugged USB.
                                                                        // log_d does not have this issue?
    Serial1.setRxBufferSize(2048);                                      // increase RX/TX buffers, prevent buffer overruns
    Serial1.setTxBufferSize(2048);
    Serial1.begin(FUNCONF_UART_PRINTF_BAUD, SERIAL_8N1, USART_RX, USART_TX, false);       // Serial connection to main board microcontroller
    //Serial2.begin(115200, SERIAL_8N1, USART_TX, -1, false);
    Serial.printf("\nSmartEVSE v4 powerup\n");

    _LOG_D("Total heap: %u.\n", ESP.getHeapSize());
    _LOG_D("Free heap: %u.\n", ESP.getFreeHeap());
    _LOG_D("Flash Size: %u.\n", ESP.getFlashChipSize());
    _LOG_D("Total PSRAM: %u.\n", ESP.getPsramSize());
    _LOG_D("Free PSRAM: %u.\n", ESP.getFreePsram());


    // configure SPI connection to LCD
    // SPI_SCK, SPI_MOSI and LCD_CS pins are used.
    LCD_SPI2.begin(LCD_SCK, -1, LCD_SDA, LCD_CS);
    // the ST7567's max SPI Clock frequency is 20Mhz at 3.3V/25C
    // We choose 10Mhz here, to reserve some room for error.
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge)
    LCD_SPI2.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    // Dummy transaction, to make sure SCLK idles high (IDF bug?)
    LCD_SPI2.transfer(0);
    _LOG_D("SPI for LCD configured.\n");

    //GLCD_init();                                // Initialize LCD


    ledcSetup(LCD_CHANNEL, 5000, 8);            // LCD channel 5, 5kHz, 8 bit
    ledcAttachPin(LCD_LED, LCD_CHANNEL);
    ledcWrite(LCD_CHANNEL, 255);                // Set LCD backlight brightness 0-255

    digitalWrite(PIN_QCA700X_RESETN, HIGH);         // get modem out of reset
    esp_read_mac(myMac, ESP_MAC_ETH); // select the Ethernet MAC
extern void setSeccIp();
    setSeccIp();  // use myMac to create link-local IPv6 address.
extern uint8_t modem_state;
    modem_state = MODEM_POWERUP;
    // Create Task 20ms Timer
extern void Timer20ms(void * parameter);
    xTaskCreate(
        Timer20ms,      // Function that should be called
        "Timer20ms",    // Name of the task (for debugging)
        3072,           // Stack size (bytes)
        NULL,           // Parameter to pass
        1,              // Task priority
        NULL            // Task handle
    );
#endif //SMARTEVSE_VERSION

    // Read all settings from non volatile memory; MQTTprefix will be overwritten if stored in NVS
    read_settings();                                                            // initialize with default data when starting for the first time
    validate_settings();
    ReadRFIDlist();                                                             // Read all stored RFID's from storage

    getButtonState();
/*     * @param Buttons: < o >
 *          Value: 1 2 4
 *            Bit: 0:Pressed / 1:Released         */
    // Sample middle+right button, and lock/unlock LCD buttons.
    if (ButtonState == 1) {
        LCDlock = !LCDlock;
        write_settings();
    }

    BacklightTimer = BACKLIGHT;
    GLCD_init();

#if SMARTEVSE_VERSION >=40 //v4

    // After powerup request WCH version (version?)
    // then send Configuration to WCH
    unsigned long FlashTimeout = millis();
    uint16_t RXbyte, idx = 0;
    char *ret;
    char RxBuf[512];
    bool gotVersion = false;
    do {
        Serial1.print("@version?\n");            // send command to WCH ic
        _LOG_V("[->] version?\n");

        vTaskDelay(100 / portTICK_PERIOD_MS);

        // ESP32 requests version info from CH32; we need to do this outside of the ESP32 10ms routines because
        // we can not communicate with the CH32 and simultaneously reprogram it.
        if (Serial1.available()) {
            while (Serial1.available() && idx<sizeof(RxBuf)) {      // make sure buffer does not overflow
                RXbyte = Serial1.read();
                RxBuf[idx] = RXbyte;
                idx++;
            }
            _LOG_D("[(%u)<-] %.*s.\n", idx, idx, RxBuf);
        }

        // process data from mainboard
        if (idx > 5) {
            char token[64];
            strncpy(token, "version:", sizeof(token));
            ret = strstr(RxBuf, token);
            if (ret != NULL) {
                unsigned long WCHRunningVersion = atoi(ret+strlen(token));
                _LOG_V("version %lu received\n", WCHRunningVersion);
                WCHUPDATE(WCHRunningVersion);
                gotVersion = true;
            }
            memset(RxBuf,0,idx);                                    // Clear buffer
            idx = 0;
        }

    } while (!gotVersion && millis() - FlashTimeout < 10000);       // only try for 10s, then release so ESP32 can boot and OTA updates are possible
    memset(RxBuf, 0, sizeof(RxBuf));                                // clear SerialBuffer

    if (!gotVersion) {                                              // we timed out
        WCHUPDATE(0);
    }
#endif

    // Create Task EVSEStates, that handles changes in the CP signal
    xTaskCreate(
        Timer10ms,      // Function that should be called
        "Timer10ms",    // Name of the task (for debugging)
        4096,           // Stack size (bytes)                              // printf needs atleast 1kb
        NULL,           // Parameter to pass
        5,              // Task priority - high
        NULL            // Task handle
    );


#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
    // Create Task 100ms Timer
    xTaskCreate(
        Timer100ms,     // Function that should be called
        "Timer100ms",   // Name of the task (for debugging)
        4608,           // Stack size (bytes)
        NULL,           // Parameter to pass
        3,              // Task priority - medium
        NULL            // Task handle
    );
#endif //SMARTEVSE_VERSION

    // Create Task Second Timer (1000ms)
    xTaskCreate(
        Timer1S,        // Function that should be called
        "Timer1S",      // Name of the task (for debugging)
        4096,           // Stack size (bytes)                              
        NULL,           // Parameter to pass
        3,              // Task priority - medium
        NULL            // Task handle
    );

    WiFiSetup();


#if SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40
    // Set eModbus LogLevel to 1, to suppress possible E5 errors
    MBUlogLvl = LOG_LEVEL_CRITICAL;
    ConfigureModbusMode(255);
    PILOT_CONNECTED;           // CP signal ACTIVE
#endif

    firmwareUpdateTimer = random(FW_UPDATE_DELAY, 0xffff);
}


// returns true if current and latest version can be detected correctly and if the latest version is newer then current
// this means that ANY home compiled version, which has version format "11:20:03@Jun 17 2024", will NEVER be automatically updated!!
// same goes for current version with an -RC extension: this will NEVER be automatically updated!
// same goes for latest version with an -RC extension: this will NEVER be automatically updated! This situation should never occur since
// we only update from the "stable" repo !!
bool fwNeedsUpdate(char * version) {
    // version NEEDS to be in the format: vx.y.z[-RCa] where x, y, z, a are digits, multiple digits are allowed.
    // valid versions are v3.6.10   v3.17.0-RC13
    int latest_major, latest_minor, latest_patch, latest_rc, cur_major, cur_minor, cur_patch, cur_rc;
    int hit = sscanf(version, "v%i.%i.%i-RC%i", &latest_major, &latest_minor, &latest_patch, &latest_rc);
    _LOG_A("Firmware version detection hit=%i, LATEST version detected=v%i.%i.%i-RC%i.\n", hit, latest_major, latest_minor, latest_patch, latest_rc);
    int hit2 = sscanf(VERSION, "v%i.%i.%i-RC%i", &cur_major, &cur_minor, &cur_patch, &cur_rc);
    _LOG_A("Firmware version detection hit=%i, CURRENT version detected=v%i.%i.%i-RC%i.\n", hit2, cur_major, cur_minor, cur_patch, cur_rc);
    if (hit != 3 || hit2 != 3)                                                  // we couldnt detect simple vx.y.z version nrs, either current or latest
        return false;
    if (cur_major > latest_major)
        return false;
    if (cur_major < latest_major)
        return true;
    if (cur_major == latest_major) {
        if (cur_minor > latest_minor)
            return false;
        if (cur_minor < latest_minor)
            return true;
        if (cur_minor == latest_minor)
            return (cur_patch < latest_patch);
    }
    return false;
}

/**
  * Periodically retrieves current measurements from the HomeWizard P1 energy meter
  * and updates the main meter's currents.
  *
  * This function ensures a delay of at least 1.95 seconds between consecutive data retrieval attempts.
  */
 void homewizard_loop() {
    static unsigned long lastCheck_homewizard = 0;

    constexpr unsigned long interval = 1950; // 1.95 seconds - With this setting there can be 5 attempts for updating the data before the 10 second Mains Meter timeout.
    const unsigned long currentTime = millis();

    if (currentTime - lastCheck_homewizard < interval) {
        return;
    }

    _LOG_A("homewizard_loop(): start HomeWizrd P1 reading.");
    lastCheck_homewizard = currentTime;

    const auto currents = getMainsFromHomeWizardP1();
#if SMARTEVSE_VERSION < 40 //v3
    for (int i = 0; i < currents.first; i++)
        MainsMeter.Irms[i] = currents.second[i];
    if (currents.first) {
        CalcIsum();
        MainsMeter.setTimeout(COMM_TIMEOUT);
    }
#else
    Serial1.printf("@Irms:%03u,%d,%d,%d\n", MainsMeter.Address, currents.second[0], currents.second[1], currents.second[2]); //Irms:011,312,123,124 means: the meter on address 11(dec) has Irms[0] 312 dA, Irms[1] of 123 dA, Irms[2] of 124 dA
#endif
}

void loop() {

    network_loop();
    if (MainsMeter.Type == EM_HOMEWIZARD_P1 && LoadBl < 2) {
        homewizard_loop();
    }
    
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck >= 1000) {
        lastCheck = millis();
        //this block is for non-time critical stuff that needs to run approx 1 / second
#if !defined(SMARTEVSE_VERSION) || SMARTEVSE_VERSION >=30 && SMARTEVSE_VERSION < 40 //not on ESP32 v4
        //printStatus:
        _LOG_I ("STATE: %s Error: %u StartCurrent: -%i ChargeDelay: %u SolarStopTimer: %u NoCurrent: %u Imeasured: %.1f A IsetBalanced: %.1f A, MainsMeter.Timeout=%u, EVMeter.Timeout=%u.\n", getStateName(State), ErrorFlags, StartCurrent, ChargeDelay, SolarStopTimer,  NoCurrent, (float)MainsMeter.Imeasured/10, (float)IsetBalanced/10, MainsMeter.Timeout, EVMeter.Timeout);
#else
        _LOG_I ("STATE: %s Error: %u StartCurrent: -%i ChargeDelay: %u SolarStopTimer: %u NoCurrent: %u Imeasured: %.1f A IsetBalanced: %.1f A.\n", getStateName(State), ErrorFlags, StartCurrent, ChargeDelay, SolarStopTimer,  NoCurrent, (float)MainsMeter.Imeasured/10, (float)IsetBalanced/10);
#endif
        _LOG_I("L1: %.1f A L2: %.1f A L3: %.1f A Isum: %.1f A\n", (float)MainsMeter.Irms[0]/10, (float)MainsMeter.Irms[1]/10, (float)MainsMeter.Irms[2]/10, (float)Isum/10);

#if AUTOMATED_TESTING
        if (shouldReboot) {
#else
        // a reboot is requested, but we kindly wait until EV is not charging
        if (shouldReboot && State != STATE_C) {                                 //slaves in STATE_C continue charging when Master reboots
            delay(5000);                                                        //give user some time to read any message on the webserver
#endif
            ESP.restart();
        }

        // TODO move this to a once a minute loop?
        if (DelayedStartTime.epoch2 && LocalTimeSet) {
            // Compare the times
            time_t now = time(nullptr);             //get current local time
            DelayedStartTime.diff = DelayedStartTime.epoch2 - (mktime(localtime(&now)) - EPOCH2_OFFSET);
            if (DelayedStartTime.diff > 0) {
                if (AccessStatus != OFF && (DelayedStopTime.epoch2 == 0 || DelayedStopTime.epoch2 > DelayedStartTime.epoch2))
                    setAccess(OFF);                         //switch to OFF, we are Delayed Charging
            }
            else {
                //starttime is in the past so we are NOT Delayed Charging, or we are Delayed Charging but the starttime has passed!
                if (DelayedRepeat == 1)
                    DelayedStartTime.epoch2 += 24 * 3600;                           //add 24 hours so we now have a new starttime
                else
                    DelayedStartTime.epoch2 = DELAYEDSTARTTIME;
                setAccess(ON);
            }
        }
        //only update StopTime.diff if starttime has already passed
        if (DelayedStopTime.epoch2 && LocalTimeSet) {
            // Compare the times
            time_t now = time(nullptr);             //get current local time
            DelayedStopTime.diff = DelayedStopTime.epoch2 - (mktime(localtime(&now)) - EPOCH2_OFFSET);
            if (DelayedStopTime.diff <= 0) {
                //DelayedStopTime has passed
                if (DelayedRepeat == 1)                                         //we are on a daily repetition schedule
                    DelayedStopTime.epoch2 += 24 * 3600;                        //add 24 hours so we now have a new starttime
                else
                    DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                setAccess(OFF);                         //switch to OFF
            }
        }
        //_LOG_A("DINGO: firmwareUpdateTimer just before decrement=%i.\n", firmwareUpdateTimer);
        if (AutoUpdate && !shouldReboot) {                                      // we don't want to autoupdate if we are on the verge of rebooting
            firmwareUpdateTimer--;
            char version[32];
            if (firmwareUpdateTimer == FW_UPDATE_DELAY) {                       // we now have to check for a new version
                //timer is not reset, proceeds to 65535 which is approx 18h from now
                if (getLatestVersion(String(String(OWNER_FACT) + "/" + String(REPO_FACT)), "", version)) {
                    if (fwNeedsUpdate(version)) {
                        _LOG_A("Firmware reports it needs updating, will update in %i seconds\n", FW_UPDATE_DELAY);
                        asprintf(&downloadUrl, "%s/fact_firmware.signed.bin", FW_DOWNLOAD_PATH); //will be freed in FirmwareUpdate() ; format: http://s3.com/fact_firmware.debug.signed.bin
                    } else {
                        _LOG_A("Firmware reports it needs NO update!\n");
                        firmwareUpdateTimer = random(FW_UPDATE_DELAY + 36000, 0xffff);  // at least 10 hours in between checks
                    }
                }
            } else if (firmwareUpdateTimer == 0) {                              // time to download & flash!
                if (getLatestVersion(String(String(OWNER_FACT) + "/" + String(REPO_FACT)), "", version)) { // recheck version info
                    if (fwNeedsUpdate(version)) {
                        _LOG_A("Firmware reports it needs updating, starting update NOW!\n");
                        asprintf(&downloadUrl, "%s/fact_firmware.signed.bin", FW_DOWNLOAD_PATH); //will be freed in FirmwareUpdate() ; format: http://s3.com/fact_firmware.debug.signed.bin
                        RunFirmwareUpdate();
                    } else {
                        _LOG_A("Firmware changed its mind, NOW it reports it needs NO update!\n");
                    }
                    //note: the firmwareUpdateTimer will decrement to 65535s so next check will be in 18hours or so....
                }
            }
        } // AutoUpdate
        /////end of non-time critical stuff
    }

    //OCPP lifecycle management
#if ENABLE_OCPP && defined(SMARTEVSE_VERSION) //run OCPP only on ESP32
    if (OcppMode && !getOcppContext()) {
        ocppInit();
    } else if (!OcppMode && getOcppContext()) {
        ocppDeinit();
    }

    if (OcppMode) {
        ocppLoop();
    }
#endif //ENABLE_OCPP

}
#endif //ESP32

