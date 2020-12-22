/*
Raingauge

    * Author :             F. Guiet 
    * Creation           : 20201004
    * Last modification  : 20201222      
    * History            : 1.0 - First version
                           1.1 - Change resistor values to operate with built-in reference (INTERNAL ADC) of 1.1v , chane the way interrupt is handled ... sometime it was sending messages 2 times  
                          
                          Sample of message sent two times...

                          wakeUpByFlipFlop was reset to true when while message was sent...dunno why because I put a debounce of 1 s...
                          I think the pb is because micros() is used by LMIC library ... so debounce must not work properly...

                          2020-12-21T08:37:08.913Z 1        3.309999942779541
                          2020-12-21T08:37:19.968Z 1        4.210000038146973

                          2020-12-21T08:55:20.273Z 1        4.199999809265137
                          2020-12-21T08:55:31.336Z 1        4.199999809265137

                    
                  
    * Tips ! :
      - Change baudrate in PlatformIO : Ctrl+T b 115200

    * Extra libraries used (see /lib folder of this project):

      - https://github.com/matthijskooijman/arduino-lmic/releases/tag/1.5.1

    * References:
      - https://tum-gis-sensor-nodes.readthedocs.io/en/latest/adafruit_32u4_with_display/README.html
      - https://github.com/CongducPham/LMIC_low_power/blob/master/Arduino_LoRa_LMIC_ABP_temp/Arduino_LoRa_LMIC_ABP_temp.ino
      - https://github.com/henri98/LoRaWAN-Weather-Station

    * Power consumption :
      - according to my Current ranger : 1542uA when sleeping (with power led on), 4,8mA when sending LoRa message

*/

#include <Arduino.h>
#include <LowPower.h>
#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>

////////////////////////// LoraWan Setup ////////////////////////////

// LoRaWAN NwkSKey, network session key
// This is the default Semtech key, which is used by the early prototype TTN
// network.
static const PROGMEM u1_t NWKSKEY[16] = { 0x44, 0x4D, 0x47, 0x85, 0xF1, 0xEB, 0x22, 0x4A, 0xE0, 0x25, 0x86, 0xAB, 0x17, 0x56, 0x9C, 0x59 };

// LoRaWAN AppSKey, application session key
// This is the default Semtech key, which is used by the early prototype TTN
// network.
static const u1_t PROGMEM APPSKEY[16] = { 0x23, 0x71, 0x3C, 0x3C, 0x05, 0x5A, 0x92, 0xD2, 0xBE, 0x23, 0xBE, 0x0E, 0x60, 0x95, 0x80, 0xF1 };

// LoRaWAN end-device address (DevAddr)
static const u4_t DEVADDR = 0x26013B84;

// These callbacks are only used in over-the-air activation, so they are
// left empty here (we cannot leave them out completely unless
// DISABLE_JOIN is set in config.h, otherwise the linker will complain).
void os_getArtEui (u1_t* buf) { }
void os_getDevEui (u1_t* buf) { }
void os_getDevKey (u1_t* buf) { }

static osjob_t sendjob;

// Pin mapping
const lmic_pinmap lmic_pins = {
    .nss = 6,
    .rxtx = LMIC_UNUSED_PIN,
    .rst = 5,    
    .dio = {3, 4, LMIC_UNUSED_PIN}
};

///////////////////////////////////////////////////////////////////

#define DEBUG 0

const String FIRMWARE_VERSION= "1.1";
const String SENSOR_ID= "17";

const int REED_SWITCH_PIN = 2;
const int BATTERY_ANALOG_PIN = A0;
volatile bool wakeUpByFlipFlop = false;
const long DEBOUNCING_TIME = 1000; //Debouncing Time in Milliseconds 
volatile unsigned long last_micros;
const unsigned TX_INTERVAL = 30*60; //Transnmit at least every 1/2 hour
bool messageSent = true;
char buff[30];

void OnRainfall() {
  // Interrupt service routine or ISR  
  // Do not use debounce anymore...not working properly...because of LMIC library??
  //if((long)(micros() - last_micros) >= DEBOUNCING_TIME * 1000) {
  wakeUpByFlipFlop = true;
  //  last_micros = micros();
  //}  
}

void blink(unsigned long ms) {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(ms);
  digitalWrite(LED_BUILTIN, LOW);
  delay(ms);
}

void burn8Readings(int pin)
{
  for (int i = 0; i < 8; i++)
  {
    analogRead(pin);
    delay(2);
  }
}

float ReadVoltage() {

  analogReference(INTERNAL);    // set the ADC reference to 1.1V
  burn8Readings(BATTERY_ANALOG_PIN);            // make 8 readings but don't use them
  delay(10);                    // idle some time
  unsigned int sensorValue = analogRead(BATTERY_ANALOG_PIN);    // read actual value

  //with R1 = 43kOhm, R2 = 15kOhm
  //Max voltage 4.2v of fully charged battery produces = 1.0862068965517242v on A0
  //According to voltage divider formula : Vout = Vin * (R2 / (R1 + R2))

  //So 4.2v is roughly represented by 1023 value on A0

  //This live experience shows 1013 for full charged lithium battery (4.2v)
  //So 4.2v is roughly represented by 1013 value on A0 (we are closed to the theoric 1023 :))

  float voltage = (sensorValue * 4.2) / 1013;

  if (DEBUG) {
    Serial.println(sensorValue);
    Serial.println("Battery voltage is : " + String(voltage,2));
  }

  return voltage;
}

void debug_message(String message, bool doReturnLine) {

  if (DEBUG) {
    if (doReturnLine)
      Serial.println(message);
    else
      Serial.print(message);
    
    Serial.flush();      
  }  
}

void Hibernate()         // here arduino is put to sleep/hibernation
{  
  //Attach Interrupt
  //attachInterrupt(digitalPinToInterrupt(REED_SWITCH_PIN),OnRainfall, FALLING);

  extern volatile unsigned long timer0_overflow_count;

  int sleepcycles = TX_INTERVAL / 8;  // calculate the number of sleepcycles (8s) given the TX_INTERVAL
  
  //Reset boolean  
  wakeUpByFlipFlop = false;

  for (int i=1; i<=sleepcycles;i++) {
    
      LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);      

      //
      // 2020/12/22 : Don't delete that!!! it makes LMIC works a lot better !!! message are sent quickly!!!
      // without that message are sent but it is very slow!!!
      //
      ///See https://github.com/henri98/LoRaWAN-Weather-Station/blob/master/src/main.cpp
      //Not what if does...but...
      // LMIC uses micros() to keep track of the duty cycle, so
      // hack timer0_overflow for a rude adjustment:      
      cli();
      timer0_overflow_count+= 8 * 64 * clockCyclesPerMicrosecond();
      sei();
    
      if (wakeUpByFlipFlop) { 
        //detachInterrupt(digitalPinToInterrupt(REED_SWITCH_PIN));
        debug_message("wakeUpByFlipFlop : TRUE", true);        
        break;
      }
  }  
}

void setup_lorawan_system() {
    //Serial.begin(115200);
    //Serial.println(F("Starting"));    

    /*#ifdef VCC_ENABLE
    // For Pinoccio Scout boards
    pinMode(VCC_ENABLE, OUTPUT);
    digitalWrite(VCC_ENABLE, HIGH);
    delay(1000);
    #endif*/

    // LMIC init
    os_init();
    // Reset the MAC state. Session and pending data transfers will be discarded.
    LMIC_reset();

    // Use with Arduino Pro Mini ATmega328P 3.3V 8 MHz
    // Let LMIC compensate for +/- 10% clock error    
    LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100); 
    
    // Set static session parameters. Instead of dynamically establishing a session
    // by joining the network, precomputed session parameters are be provided.
    #ifdef PROGMEM
    // On AVR, these values are stored in flash and only copied to RAM
    // once. Copy them to a temporary buffer here, LMIC_setSession will
    // copy them into a buffer of its own again.
    uint8_t appskey[sizeof(APPSKEY)];
    uint8_t nwkskey[sizeof(NWKSKEY)];
    memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
    memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
    LMIC_setSession (0x1, DEVADDR, nwkskey, appskey);
    #else
    // If not running an AVR with PROGMEM, just use the arrays directly
    LMIC_setSession (0x1, DEVADDR, NWKSKEY, APPSKEY);
    #endif

    #if defined(CFG_eu868)
    // Set up the channels used by the Things Network, which corresponds
    // to the defaults of most gateways. Without this, only three base
    // channels from the LoRaWAN specification are used, which certainly
    // works, so it is good for debugging, but can overload those
    // frequencies, so be sure to configure the full frequency range of
    // your network here (unless your network autoconfigures them).
    // Setting up channels should happen after LMIC_setSession, as that
    // configures the minimal channel set.
    // NA-US channels 0-71 are configured automatically
    LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);      // g-band
    LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);      // g-band
    LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);      // g2-band
    // TTN defines an additional channel at 869.525Mhz using SF9 for class B
    // devices' ping slots. LMIC does not have an easy way to define set this
    // frequency and support for class B is spotty and untested, so this
    // frequency is not configured here.
    #elif defined(CFG_us915)
    // NA-US channels 0-71 are configured automatically
    // but only one group of 8 should (a subband) should be active
    // TTN recommends the second sub band, 1 in a zero based count.
    // https://github.com/TheThingsNetwork/gateway-conf/blob/master/US-global_conf.json
    LMIC_selectSubBand(1);
    #endif

    // Disable link check validation
    LMIC_setLinkCheckMode(0);

    // TTN uses SF9 for its RX2 window.
    LMIC.dn2Dr = DR_SF9;

    // Set data rate and transmit power for uplink (note: txpow seems to be ignored by the library)
    LMIC_setDrTxpow(DR_SF7,14);

    // Start job
    //do_send(&sendjob);
}

void onEvent (ev_t ev) {
    debug_message(String(os_getTime()), false);
    debug_message(": ", false);
    //Serial.print(os_getTime());
    //Serial.print(": ");
    switch(ev) {
        case EV_SCAN_TIMEOUT:
            debug_message(F("EV_SCAN_TIMEOUT"),true);
            //Serial.println(F("EV_SCAN_TIMEOUT"));
            break;
        case EV_BEACON_FOUND:
            debug_message(F("EV_BEACON_FOUND"),true);
            //Serial.println(F("EV_BEACON_FOUND"));
            break;
        case EV_BEACON_MISSED:
            debug_message(F("EV_BEACON_MISSED"), true);
            //Serial.println(F("EV_BEACON_MISSED"));
            break;
        case EV_BEACON_TRACKED:
            debug_message(F("EV_BEACON_TRACKED"), true);
            //Serial.println(F("EV_BEACON_TRACKED"));
            break;
        case EV_JOINING:
            debug_message(F("EV_JOINING"), true);
            //Serial.println(F("EV_JOINING"));
            break;
        case EV_JOINED:
            debug_message(F("EV_JOINED"), true);
            //Serial.println(F("EV_JOINED"));
            break;
        case EV_RFU1:
            debug_message(F("EV_RFU1"), true);
            //Serial.println(F("EV_RFU1"));
            break;
        case EV_JOIN_FAILED:
            debug_message(F("EV_JOIN_FAILED"), true);
            //Serial.println(F("EV_JOIN_FAILED"));
            break;
        case EV_REJOIN_FAILED:
            debug_message(F("EV_REJOIN_FAILED"), true);
            //Serial.println(F("EV_REJOIN_FAILED"));
            break;
        case EV_TXCOMPLETE:
            debug_message(F("EV_TXCOMPLETE (includes waiting for RX windows)"), true);
            debug_message("Time is : ", false);   
            Serial.println(millis());
            //Serial.println(F("EV_TXCOMPLETE (includes waiting for RX windows)"));
            if (LMIC.txrxFlags & TXRX_ACK)
              debug_message(F("Received ack"), true);
              //Serial.println(F("Received ack"));
            if (LMIC.dataLen) {
              debug_message(F("Received"), false);
              debug_message(String(LMIC.dataLen), false);
              debug_message(F("bytes of payload"), true);
              //Serial.println(F("Received "));
              //Serial.println(LMIC.dataLen);
              //Serial.println(F(" bytes of payload"));
            }
            // Schedule next transmission
            //os_setTimedCallback(&sendjob, os_getTime()+sec2osticks(TX_INTERVAL), do_send);
            messageSent = true;
            break;
        case EV_LOST_TSYNC:
            debug_message(F("EV_LOST_TSYNC"), true);
            //Serial.println(F("EV_LOST_TSYNC"));
            break;
        case EV_RESET:
            debug_message(F("EV_RESET"), true);
            //Serial.println(F("EV_RESET"));
            break;
        case EV_RXCOMPLETE:
            // data received in ping slot
            debug_message(F("EV_RXCOMPLETE"), true);            
            //Serial.println(F("EV_RXCOMPLETE"));
            break;
        case EV_LINK_DEAD:
            debug_message("EV_LINK_DEAD", true);
            //Serial.println(F("EV_LINK_DEAD"));
            break;
        case EV_LINK_ALIVE:
            debug_message(F("EV_LINK_ALIVE"), true);
            //Serial.println(F("EV_LINK_ALIVE"));
            break;
         default:
            debug_message(F("Unknown event"), true);
            //Serial.println(F("Unknown event"));
            break;
    }
}

void do_send(osjob_t* j){

    // Check if there is not a current TX/RX job running
    if (LMIC.opmode & OP_TXRXPEND) {
        debug_message(F("OP_TXRXPEND, not sending"), true);        
    } else {        
        // Prepare upstream data transmission at the next possible time.        
        //strlen() searches for that NULL character and counts the number of memory address passed, So it actually counts the number of elements present in the string before the NULL character, here which is 8.
        LMIC_setTxData2(1, buff, strlen(buff), 0);
        debug_message(F("Packet queued"), true);                
    }
    // Next TX is scheduled after TX_COMPLETE event.
}

void setup() {
  // Initialize Serial Port
  if (DEBUG)
    Serial.begin(9600);

  setup_lorawan_system();

  //analogReference(INTERNAL);

  pinMode(REED_SWITCH_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(REED_SWITCH_PIN),OnRainfall, FALLING);
   
  debug_message(F("Setup completed successfully...Starting main loop now !"), true);
}

void sendMessage(bool wakeUpByFlipFlop) {

  float voltage = ReadVoltage();   

  //Reset buffer
  memset(buff, '\0', sizeof(buff));

  //First sensor id
  strcpy(buff, SENSOR_ID.c_str());
  
  //Firmware version
  strcat(buff, " ");
  strcat(buff, FIRMWARE_VERSION.c_str());
  //dtostrf(FIRMWARE_VERSION, 3, 1, buff + strlen(buff));

  char temp[33] = {};

  //Reset Array
  memset(temp, '\0', sizeof(temp));
  
  //Voltage
  strcat(buff, " ");
  dtostrf(voltage, 4, 2, temp);
  strcat(buff, temp);  
  
  //Liter
  //Reset Array
  memset(temp, '\0', sizeof(temp));
  
  strcat(buff, " ");
  
  if (wakeUpByFlipFlop) {
    strcat(buff, "1");
  }
  else {
    strcat(buff, "0");
  }

  messageSent = false;

  do_send(&sendjob);  
}

void loop() {  
  
  /*float toto = ReadVoltage();
  Serial.println("Battery voltage is : " + String(toto,2));
  delay(1000);
  return;*/

  //First time consider message is sent
  if (!messageSent) { 

    //Waiting for EV_TXCOMPLETE event
    os_runloop_once();      
    return;  
  }

  if (wakeUpByFlipFlop) {
    debug_message("wakeUpByFlipFlop : TRUE", true);
  }
  else
  {
    debug_message("wakeUpByFlipFlop : FALSE", true);
  }
  
  //Needed something is expected on serial port..
  if (DEBUG)
    delay(1000);

  debug_message("Hibernating...", true);

  Hibernate();

  debug_message("Alive !", true);
  
  if (wakeUpByFlipFlop) {
    //Send LoRA message
    sendMessage(true);

    //Signal
    //blink(200);

    //Reset boolean    
    //wakeUpByFlipFlop = false;    
    
  } else {
    //Woke up because one hour has passed....
    //Just send battery status info
    sendMessage(false);
  }
}





