#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>  
#include <SD.h>
#include <EEPROM.h>
#include <PubSubClient.h>                        /* MQTT library from https://github.com/knolleary/pubsubclient */
#include <TimeLib.h>                             /* Time library from https://github.com/PaulStoffregen/Time    */

extern "C" {
#include "gpio.h"
#include "user_interface.h"
}

#define DEBUG true                                /* Send debug messages if true          */
#define NOT_READ_EEPROM false                     /* Dont't read from EEPROM if true      */

#define SD_PIN D4                                 /* microSD use D4 for Wemos D1 Mini     */
#define HOT_PIN D1                                /* Pin of hot water                     */
#define COLD_PIN D2                               /* Pin of cold water                    */

#define TIME_BOUNCE 250                           /* Timeout for debounce in msec         */

/* Name and Version */
#define PLATFORM "Wemos D1 mini & Micro SD"
#define MODULE_VERSION "v2.2.3"
#define MODULE_NAME "WaterMeter " MODULE_VERSION
#define WEB_WATERMETER_FIRST_NAME "Water"
#define WEB_WATERMETER_LAST_NAME "Meter"

/* Directory and name of config files for SD card */
#define DELIM "/"
#define WM_DIR DELIM "wmeter"                    /* Drirectory for config file        - "/wmeter"             */
#define WM_CONFIG_FILE WM_DIR DELIM "wmeter.dat" /* Full path and name of config file - "/wmeter/wmeter.dat"  */

/* For config (default settings) */
#define WEB_ADMIN_LOGIN "Admin"                  /* Login for web Auth                */
#define WEB_ADMIN_PASSWORD "1111"                /* Password for web Auth             */
#define AP_SSID "WaterMeter"                     /* Name WiFi AP Mode                 */
#define AP_PASSWORD "12345678"                   /* Password WiFi AP Mode             */
#define MQTT_USER "test"                         /* mqtt user                         */
#define MQTT_PASSWORD "1111"                     /* mqtt password                     */
#define MQTT_BROKER "192.168.1.1"                /* URL mqtt broker                   */
#define MQTT_TOPIC "/WaterMeter"                 /* Primary mqtt topic name           */
#define LITERS_PER_PULSE 10                      /* How many liters per one pulse     */
#define TIME_ZONE 3                              /* Default Time Zone                 */

/* For TimeLib (NTP and clock) */
#define SYNC_TIME 21600                          /* Interval sync to NTP Server in sec           */
#define NTP_SERVER_NAME "ru.pool.ntp.org"        /* URL of NTP server                            */ 
#define NTP_LOCAL_PORT 8888                      /* NTP local port                               */
unsigned int localPort = NTP_LOCAL_PORT;
const int NTP_PACKET_SIZE = 48;                  /* NTP time is in the first 48 bytes of message */
byte packetBuffer[NTP_PACKET_SIZE];              /* buffer to hold incoming & outgoing packets   */
WiFiUDP Udp;


/* For SD */
bool sdOk = false;                              
String watermeterDirName = WM_DIR;
String configFileName = WM_CONFIG_FILE;

/* For EEPROM */
#define EEPROM_START 0
bool setEEPROM = false;
uint32_t memcrc; uint8_t *p_memcrc = (uint8_t*)&memcrc;

PROGMEM const uint32_t crc_table[16] = {
  0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
  0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
};

/* For WiFi */
String macAddress;
WiFiClient wifiClient;

/* For MQTT */
#define MQTT_PORT 1883
#define END_TOPIC_HOT_OUT "HotWater"
#define END_TOPIC_COLD_OUT "ColdWater"
#define END_TOPIC_RSSI "RSSI"
PubSubClient mqttClient(wifiClient);
String mqttClientId;                      /* "MODULE_NAME-MacAddress"                                    */
String mqttTopicHotOut, mqttTopicColdOut, mqttTopicRSSI;
                                          /* Full name Topic -                                           *
                                           *  mqttTopicHotOut  - "MQTT_TOPIC/MacAddress/HotWater"    *
                                           *  mqttTopicColdOut - "MQTT_TOPIC/MacAddress/ColdWater"   *
                                           *  mqttTopicRSSI    - "MQTT_TOPIC/MacAddress/RSSI"        *
                                           *  see mqtt.ino                                               */


/* For Web */
String webServerIndex;
String httpRoot = DELIM;
ESP8266WebServer webServer(80);

/* Flags */
bool rebootNow = false;
bool restartWiFi = false;
bool apModeNow = true;
bool staModeNow = false;
bool staConfigure = false;
bool defaultConfig = false;
bool saveNewConfig = false;
bool firstStart = false;
bool responseNTP = false;
bool mqttRestart = false;
bool mqttFirstStart = true;
bool subsHotWater = false;
bool subsColdWater = false;
bool firstNTP = true;

/* Counter of water for interrupts */
volatile unsigned long counterHotWater, counterColdWater;

/* Timers for debounce */
os_timer_t hotTimer;
os_timer_t coldTimer;

/* struct of config file */
typedef struct config {
  char webAdminLogin[16];      /* Login for web Auth                    */
  char webAdminPassword[16];   /* Password for web Auth                 */
  bool fullSecurity;           /* true - all web Auth, false - free     */
  bool configSecurity;         /* true - only config and update Auth    */
  char staSsid[16];            /* STA SSID WiFi                         */
  char staPassword[16];        /* STA Password WiFi                     */
  bool apMode;                 /* true - AP Mode, false - STA Mode      */
  char apSsid[16];             /* WiFi Name in AP mode                  */
  char apPassword[16];         /* WiFi Password in AP mode              */
  char mqttBroker[32];         /* URL or IP-address of mqtt-broker      */
  char mqttUser[16];           /* mqtt user                             */
  char mqttPassword[16];       /* mqtt password                         */
  char mqttTopic[64];          /* mqtt topic                            */
  char ntpServerName[32];      /* URL or IP-address of NTP server       */
  int timeZone;                /* Time Zone                             */
  byte litersPerPulse;         /* liters per pulse                      */
  time_t hotTime;              /* Last update time of hot water         */
  unsigned long hotWater;      /* Last number of liters hot water       */
  time_t coldTime;             /* Last update time of cold water        */
  unsigned long coldWater;     /* Last number of litres cold water      */
} _config;

_config wmConfig;

unsigned long mqttReconnectLastTime = 0;
unsigned long mqttPublishLastTime = 0;
unsigned long staReconnectLastTime = 0;
unsigned long ntpReconnectLastTime = 0;
volatile unsigned long hotTimeBounce, coldTimeBounce;
time_t timeStart;

volatile int hotInt, coldInt;
volatile int hotState, coldState;


ADC_MODE (ADC_VCC);


void loop () {
  String s;
  
  /* If counter of hot water was added */
  if (counterHotWater > 0) {
    wmConfig.hotTime = localTimeT();
    s = "";
    wmConfig.hotWater += counterHotWater * wmConfig.litersPerPulse;
    s += wmConfig.hotWater;
    if (DEBUG) {
      Serial.print(mqttTopicHotOut + " <== "); Serial.println(s);
    }
    if (mqttClient.connected()) mqttClient.publish(mqttTopicHotOut.c_str(),s.c_str());
    saveConfig();
    counterHotWater = 0;
  }

  /* If counter of cold water was added */
  if (counterColdWater > 0) {
    wmConfig.coldTime = localTimeT();
    s = "";
    wmConfig.coldWater += counterColdWater * wmConfig.litersPerPulse;
    s += wmConfig.coldWater;
    if (DEBUG) {
      Serial.print(mqttTopicColdOut + " <== "); Serial.println(s);
    }
    if (mqttClient.connected()) mqttClient.publish(mqttTopicColdOut.c_str(),s.c_str());
    saveConfig();
    counterColdWater = 0;
  }

  webServer.handleClient();

  /* Restart connecting to NTP server once in 60 sec */
  if (!responseNTP && !apModeNow && millis() - ntpReconnectLastTime > 60000 && WiFi.status() == WL_CONNECTED)  {
    ntpReconnectLastTime = millis();
    startNTP();
  }

  /* If reconfigured counter of hot water */
  if (subsHotWater) {
    subsHotWater = false;
    s = "";
    s += wmConfig.hotWater;
      
    if (DEBUG) Serial.printf("%s <== %s\n", mqttTopicHotOut.c_str(), s.c_str());
    
    if (mqttClient.connected())  mqttClient.publish(mqttTopicHotOut.c_str(),s.c_str());
  }

  /* If reconfigured counter of cold water */
  if (subsColdWater) {
    subsColdWater = false;
    s = "";
    s += wmConfig.coldWater;
    
    if (DEBUG) Serial.printf("%s <== %s\n", mqttTopicColdOut.c_str(), s.c_str());

    if (mqttClient.connected()) mqttClient.publish(mqttTopicColdOut.c_str(),s.c_str());
  }

  /* publish to mqtt server on start and once in 300 sec */
  if (mqttPublishLastTime == 0 || millis() - mqttPublishLastTime > 300000) {
    if (mqttClient.connected()) {
      s = "";
      s += wmConfig.coldWater;
      mqttClient.publish(mqttTopicColdOut.c_str(),s.c_str());
      s = "";
      s += wmConfig.hotWater;
      mqttClient.publish(mqttTopicHotOut.c_str(),s.c_str());
      s = "";
      s += WiFi.RSSI();
      mqttClient.publish(mqttTopicRSSI.c_str(),s.c_str());
      mqttPublishLastTime = millis();
    }
  }
  
  if (mqttClient.connected()) mqttClient.loop();
  else mqttRestart = true;

  /* reconnect to mqqt broker once in 10 sec. */
  if (mqttRestart && millis() - mqttReconnectLastTime > 10000) {
    if (staModeNow && WiFi.status() == WL_CONNECTED) {
      mqttClient.disconnect();
      mqttReconnect();
      printMqttState();
    }
    mqttReconnectLastTime = millis();
  }

  if (wmConfig.apMode && !apModeNow && !staModeNow) {
    startWiFiAP();
  }

  /* checking connect to WiFi network once in 30 sec */
  if (!wmConfig.apMode && staConfigure && millis() - staReconnectLastTime > 30000 && 
         ((apModeNow || (staModeNow && WiFi.status() != WL_CONNECTED)) || (!apModeNow && !staModeNow))) {
    staReconnectLastTime = millis();
    if (DEBUG) Serial.printf("Check WiFi network: %s\n", wmConfig.staSsid);
    int n = WiFi.scanNetworks(); 
    if (n != 0) {
      for (int i = 0; i < n; i++) {
        if (strcmp(wmConfig.staSsid, WiFi.SSID(i).c_str()) == 0) {
          if (startWiFiSTA()) {
            apModeNow = false;
            staModeNow = true;
            wmConfig.apMode = false;
            responseNTP = false;
            ntpReconnectLastTime = 0;
          }
          break;
        }
      }
      if (!staModeNow && !apModeNow) {
         if (DEBUG) Serial.println("No WiFi connect STA mode. Start AP mode");
         startWiFiAP();
      }
    } else {
      if (!apModeNow && !apModeNow) {
        if (DEBUG) Serial.println("No WiFi networks. Start AP mode");
        startWiFiAP();
      }
    }
  }

   if (firstNTP && responseNTP) {
      timeStart = timeTwTZ();
      firstNTP = false;
   }
  

  /* reset soft watchdog */
  ESP.wdtFeed();

  yield();
}
