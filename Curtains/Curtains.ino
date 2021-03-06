 #define MQTT_DEBUG 1
/*
LazyRolls
(C) 2019 ACE, a_c_e@mail.ru
http://imlazy.ru/rolls/

21.12.2018 v0.04 beta
21.02.2019 v0.05 beta
16.03.2019 v0.06
10.04.2019 v0.07

*/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <WiFiUdp.h>
#include "settings.h"

#define MQTT 1

#ifdef MQTT
 // For MQTT support: Sketch - Include Library - Manage Libraries - Adafruit MQTT - Install
 #include <Adafruit_MQTT.h>
 #include <Adafruit_MQTT_Client.h>
#endif

// copy "wifi_settings.example.h" to "wifi_settings.h" and modify it, if you wish
// Or comment next string and change defaults in this file
//#include "wifi_settings.h"

#ifndef SSID_AND_PASS
// if "wifi_settings.h" not included
const char* def_ssid = "lazyrolls";
const char* def_password = "";
const char* def_ntpserver = "time.nist.gov";
const char* def_hostname = "lazyroll";
const char* def_mqtt_server = "mqtt.lan";
const char* def_mqtt_login = "";
const char* def_mqtt_password = "";
const uint16_t def_mqtt_port=1883;
const char* def_mqtt_topic_state = "/lazyroll/%HOSTNAME%/state";
const char* def_mqtt_topic_command = "/lazyroll/%HOSTNAME%/command";
#endif

#define VERSION "0.07"
#define SPIFFS_AUTO_INIT

#ifdef SPIFFS_AUTO_INIT
#include "spiff_files.h"
#endif

//char *temp_host;
const char* update_path = "/update";
const char* update_username = "admin";
const char* update_password = "admin";
uint16_t def_step_delay_mks = 1500;
#define FAV_FILE "/favicon.ico"
#define CLASS_FILE "/styles.css"
#define PIN_SWITCH 14
#define PIN_A 5
#define PIN_B 4
#define PIN_C 13
#define PIN_D 12
#define DIR_UP (-1)
#define DIR_DN (1)
#define UP_SAFE_LIMIT 300 // make extra rotations up if not hit switch
#define DEFAULT_SWITCH_IGNORE_STEPS 100 // ignore endstop for first step on moving down
#define MIN_STEP_DELAY 100 // minimal motor step time in mks
#define ALARMS 10
#define DAY (24*60*60) // day length in seconds

const int Languages=2;
const char *Language[Languages]={"English", "Русский"};
const char *onoff[][Languages]={{"off", "on"}, {"выкл", "вкл"}};

const uint8_t steps[3][4]={
  {PIN_A, PIN_C, PIN_B, PIN_D},
  {PIN_A, PIN_B, PIN_D, PIN_C},
  {PIN_A, PIN_B, PIN_C, PIN_D}
};
volatile int position; // current motor steps position. 0 - fully open, -1 - unknown.
volatile int roll_to; // position to go to
uint16_t step_c[8], step_s[8]; // bitmasks for every step

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

typedef struct {
	uint32_t time;
	uint8_t percent_open; // 0 - open, 100 - close
	uint8_t day_of_week; // LSB - monday
	uint16_t flags; 
	uint32_t reserved;
} alarm_type;
#define ALARM_FLAG_ENABLED 0x0001

struct {
  char hostname[16+1];
  char ssid[32+1];
  char password[32+1];
  char ntpserver[32+1];
  int lang;
  uint8_t pinout; // index in "steps" array, according to motor wiring
  bool reversed; // up-down reverse
  uint16_t step_delay_mks; // delay (mks) for each step of motor
  int timezone; // timezone in minutes relative to UTC
  int full_length; // steps from open to close
	uint32_t spiffs_time; // time of files in spiffs for version checking
	alarm_type alarms[ALARMS];
	uint8_t switch_reversed; // reverse switch logic. 0 - NC, 1 - NO
	bool mqtt_enabled;
	char mqtt_server[64+1]; // MQTT server, ip or hostname
	uint32_t mqtt_port;
	char mqtt_login[64+1]; // MQTT server login
	char mqtt_password[64+1]; // MQTT server password
	uint16_t mqtt_ping_interval; // I'm alive ping interval, seconds
	char mqtt_topic_state[127+1]; // publish current state topic
	char mqtt_topic_command[127+1]; // sbscribe to commands topic
	uint8_t mqtt_state_type; // percents, ON/OFF, etc
	uint16_t switch_ignore_steps; // ignore endstop for first step on moving down
} ini;

// language functions
const char * L(const char *s1, const char *s2)
{
  return (ini.lang==0) ? s1 : s2;
}
String SL(const char *s1, const char *s2)
{
  return (ini.lang==0) ? String(s1) : String(s2);
}
String SL(String s1, String s2)
{
  return (ini.lang==0) ? s1 : s2;
}

//===================== NTP ============================================

const unsigned long NTP_WAIT=5000;
const unsigned long NTP_SYNC=24*60*60*1000;  // sync clock once a day
unsigned long lastRequest=0; // timestamp of last request
unsigned long lastSync=0; // timestamp of last successful synchronization
IPAddress timeServerIP;
const int NTP_PACKET_SIZE = 48;  // NTP time stamp is in the first 48 bytes of the message
byte NTPBuffer[NTP_PACKET_SIZE]; // buffer to hold incoming and outgoing packets
// Unix time starts on Jan 1 1970. That's 2208988800 seconds in NTP time:
const uint32_t seventyYears = 2208988800UL;
uint32_t UNIXTime;
WiFiUDP UDP;

void setup_NTP()
{
  UDP.begin(123); // Start listening for UDP messages on port 123
}

void SyncNTPTime()
{
  if (UDP.parsePacket() == 0)
  { // If there's no response (yet)
    if (lastSync!=0 && (millis()-lastSync < NTP_SYNC)) return; // time ok
    if (millis()-lastRequest < NTP_WAIT) return; // waiting for answer
    lastRequest=millis();
    memset(NTPBuffer, 0, NTP_PACKET_SIZE);  // set all bytes in the buffer to 0
    // Initialize values needed to form NTP request
    NTPBuffer[0] = 0b11100011;   // LI, Version, Mode
    if(!WiFi.hostByName(ini.ntpserver, timeServerIP))
    { // Get the IP address of the NTP server failed
      return;
    }
    // send a packet requesting a timestamp:
    UDP.beginPacket(timeServerIP, 123); // NTP requests are to port 123
    UDP.write(NTPBuffer, NTP_PACKET_SIZE);
    UDP.endPacket();
    return;
  }
  UDP.read(NTPBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
  // Combine the 4 timestamp bytes into one 32-bit number
  uint32_t NTPTime = (NTPBuffer[40] << 24) | (NTPBuffer[41] << 16) | (NTPBuffer[42] << 8) | NTPBuffer[43];
  // Convert NTP time to a UNIX timestamp: subtract seventy years:
  UNIXTime = NTPTime - seventyYears;
  lastSync=millis();
}

uint32_t getTime()
{
  if (lastSync == 0) return 0;
  return UNIXTime+ini.timezone*60 + (millis() - lastSync)/1000;
}

String TimeStr()
{
  char buf[9];
  uint32_t t=getTime();
  sprintf(buf, "%02d:%02d:%02d", t/60/60%24, t/60%60, t%60);
  return String(buf);
}

String UptimeStr()
{
  char buf[9];
  uint32_t t=millis()/1000;
  sprintf(buf, "%dd %02d:%02d", t/60/60/24, t/60/60%24, t/60%60);
  return String(buf);
}

String DoWName(int d)
{
	switch (d)
	{
		case 0: return SL("Mo", "Пн"); break;
		case 1: return SL("Tu", "Вт"); break;
		case 2: return SL("We", "Ср"); break;
		case 3: return SL("Th", "Чт"); break;
		case 4: return SL("Fr", "Пт"); break;
		case 5: return SL("Sa", "Сб"); break;
		case 6: return SL("Su", "Вс"); break;
	}
}

int DayOfWeek(uint32_t time)
{
	return ((time / DAY) + 3) % 7;
}

//----------------------- Motor ----------------------------------------
const uint8_t microstep[8][4]={
	{1, 0, 0, 0},
	{1, 1, 0, 0},
	{0, 1, 0, 0},
	{0, 1, 1, 0},
	{0, 0, 1, 0},
	{0, 0, 1, 1},
	{0, 0, 0, 1},
	{1, 0, 0, 1}};
void FillStepsTable()
{
	uint8_t i, j, n;
	for (i=0; i<8; i++)
	{
		n=i;
		if (ini.reversed) n=7-i;
		step_s[i]=0;
		step_c[i]=0;
		for (j=0; j<4; j++)
		{
			if (microstep[n][j]) 
				step_s[i] |= (1 << steps[ini.pinout][j]);
			else
				step_c[i] |= (1 << steps[ini.pinout][j]);
		}
	}		
}

void inline ICACHE_RAM_ATTR MotorOff()
{
  // digitalWrite(PIN_A, LOW);
  // digitalWrite(PIN_B, LOW);
  // digitalWrite(PIN_C, LOW);
  // digitalWrite(PIN_D, LOW);
	GPOC = (1 << PIN_A) | (1 << PIN_B) | (1 << PIN_C) | (1 << PIN_D);
}

bool ICACHE_RAM_ATTR IsSwitchPressed()
{
	//  return GPIP(PIN_SWITCH) ^^ ini.switch_reversed;
	return (((GPI >> ((PIN_SWITCH) & 0xF)) & 1) != ini.switch_reversed);
}

void Open()
{
  roll_to=0-UP_SAFE_LIMIT; // up to 0 and beyond (a little)
}

void Close()
{
  if (position<0) position=0;
  roll_to=ini.full_length;
}

void ToPercent(uint8_t pos)
{
	if ((pos<0) || (pos>100)) return;
	roll_to=ini.full_length * pos / 100;
}

void Stop()
{
  roll_to=position;
  MotorOff();
}

//===================== MQTT ===========================================

#ifdef MQTT

WiFiClient client;
Adafruit_MQTT_Client *mqtt = NULL;
Adafruit_MQTT_Subscribe *mqtt_sub = NULL;
String mqtt_topic_sub, mqtt_topic_pub;

void mqtt_callback(char *str, uint16_t len) 
{
	int x;
	if (len==0 || len>4) return;
  for(int i = 0; i<len; i++) str[i] = tolower(str[i]); // make it lowercase
	
	x=strtol(str, NULL, 10);
	if (x>0 && x<=100) ToPercent(x);
	else if (strcmp(str, "0") == 0) Open();
	else if (strcmp(str, "on") == 0) Open();
	else if (strcmp(str, "off") == 0) Close();
	else if (strcmp(str, "stop") == 0) Stop();
}

void setup_MQTT()
{
	if (mqtt) 
	{
		mqtt->disconnect();
		delete mqtt;
	}
	if (mqtt_sub)
	{
		delete mqtt_sub;
		mqtt_sub=NULL;
	}
	mqtt_topic_sub = String(ini.mqtt_topic_command);
	mqtt_topic_sub.replace("%HOSTNAME%", String(ini.hostname));
	mqtt_topic_pub = String(ini.mqtt_topic_state);
	mqtt_topic_pub.replace("%HOSTNAME%", String(ini.hostname));

	mqtt = new Adafruit_MQTT_Client(&client, ini.mqtt_server, ini.mqtt_port, ini.mqtt_login, ini.mqtt_password);
	if (mqtt_topic_sub != "")
	{
		mqtt_sub = new Adafruit_MQTT_Subscribe(mqtt, mqtt_topic_sub.c_str());
		mqtt_sub->setCallback(mqtt_callback);
		mqtt->subscribe(mqtt_sub);
	}
}

void MQTT_connect() 
{
  int8_t ret;
	static uint32_t last_reconnect=0;

	if (!ini.mqtt_enabled) return;
	
  // Stop if already connected.
  if (mqtt->connected()) return;

	if ((last_reconnect != 0) && (millis() - last_reconnect < 10000)) return;
	
  Serial.print("Connecting to MQTT... ");

  if ((ret = mqtt->connect()) != 0) 
	{ // connect will return 0 for connected
		Serial.println(mqtt->connectErrorString(ret));
		Serial.println("Retrying MQTT connection in 10 seconds...");
		mqtt->disconnect();
		last_reconnect=millis();
  } else
	{
		Serial.println("MQTT Connected!");
		last_reconnect=0;
	}
}

void ProcessMQTT()
{
	static uint32_t last_ping=0;
	static int last_val;
	
	if (!ini.mqtt_enabled) return;
	
  // Ensure the connection to the MQTT server is alive (this will make the first
  // connection and automatically reconnect when disconnected).
  MQTT_connect();

  if (!mqtt->connected()) return;
	mqtt->processPackets(1);

	// keep the connection alive
	if (millis()-last_ping > (uint32_t)ini.mqtt_ping_interval * 1000)
	{
		last_ping=millis();
		mqtt->ping();
	}
	
	// Publishing
	if (mqtt_topic_pub != "")
	{
		int val;
		switch (ini.mqtt_state_type)
		{
			case 1: 
			case 2: 
				val=(roll_to > (ini.full_length/2) ? 0 : 1); 
				break;
			default: 
				val=(position*100 + (ini.full_length/2)) / ini.full_length; 
				if (val > 100) val=100;
				if (val < 0) val=0;
			break;
		}
		if (val != last_val)
		{
			char buf[32];
			last_val=val;
			switch (ini.mqtt_state_type)
			{
				case 0: 
					sprintf(buf, "%i", val);
					break;
				case 1:
					if (val == 0) strcpy(buf, "OFF"); else strcpy(buf, "ON"); 
					break;
				case 2:
					if (val == 0) strcpy(buf, "0"); else strcpy(buf, "1"); 
					break;
				case 3:
					sprintf(buf, "{\"state\":\"%s\", \"position\":\"%d\"}", (val == 0 ? "OFF" : "ON"), val);
					break;
				default:
					buf[0]=0;
					break;
			}
			mqtt->publish(mqtt_topic_pub.c_str(), buf);
			last_ping=millis();
		}
	}
}	

const char *MQTTstatus()
{
	if (ini.mqtt_enabled)
	{
		if (mqtt->connected())
  		return L("connected", "подключен");
		else
  		return L("disconnected", "отключен");
	}
	else
		return L("disabled", "выключен");
}

#else
void setup_MQTT() {}
void ProcessMQTT() {}
char *MQTTstatus() { return "off"; }
#endif

// ==================== timer & interrupt ===============================

volatile uint8_t switch_ignore_steps;

void ICACHE_RAM_ATTR timer1Isr()
{
  static uint8_t step=0;

  bool dir_up;

  if (position==roll_to)
	{
		MotorOff();
		return; // stopped, do nothing
	}
	
  dir_up=(roll_to < position); // up - true

	if (position==0 && !dir_up) switch_ignore_steps=ini.switch_ignore_steps;
	
	if (IsSwitchPressed())
	{
		if (switch_ignore_steps == 0)
		{
			if (roll_to <= 0)
			{ // full open, done
				roll_to=0;
				position=0; // zero point found
				MotorOff();
				return;
			} else
			{ // destination - [partialy] closed
				if (dir_up)
				{
					// Zero found, now can go down, ignoring switch for some steps
					position=0; // zero point found
				  switch_ignore_steps=ini.switch_ignore_steps;
				} else
				{
					// endswitch hit on going down. Something wrong
					roll_to=0;
					MotorOff();
					return;
				}
			}
			return;
		}
	}
	if (switch_ignore_steps>0) switch_ignore_steps--;

  if (dir_up)
	{
		if (step > 0) step--;
		else
		{
			step=7;
			position--;
		}
	} else
	{
		step++;
		if (step==8)
		{
			step=0;
			position++;
		}
	}

	GPOS = step_s[step % 8];
	GPOC = step_c[step % 8];
}

void AdjustTimerInterval()
{
	timer1_write((uint32_t)ini.step_delay_mks*ESP.getCpuFreqMHz()/256);
}

void SetupTimer()
{
	timer1_isr_init();
	timer1_attachInterrupt(timer1Isr);
	timer1_enable(TIM_DIV256, TIM_EDGE, TIM_LOOP);
	AdjustTimerInterval();
}

void StopTimer()
{
	timer1_disable();
}

// ==================== initialization ===============================

#ifdef SPIFFS_AUTO_INIT
void CreateFile(const char *filename, const uint8_t *data, int len)
{
	uint8_t buf[256];
	int blk;
	unsigned int bytes;

  // update file if not exist or if version changed
  if ((ini.spiffs_time!=0 && ini.spiffs_time != spiffs_time) ||
	  (!SPIFFS.exists(filename)))
	{
		File f = SPIFFS.open(filename, "w");
		if (!f) {
			Serial.println("file creation failed");
		} else
		{
			bytes=len;
			while (bytes>0)
			{
				blk=min(bytes, sizeof(buf));
				memcpy_P(buf, data, blk);
				data+=blk;
				bytes-=blk;
				f.write(buf, blk);
			}
			f.close();
			Serial.print("file written: ");
			Serial.println(filename);
		}
	}
}
#endif

void init_SPIFFS()
{
#ifdef SPIFFS_AUTO_INIT
	CreateFile(FAV_FILE, fav_icon_data, sizeof(fav_icon_data));
	CreateFile(CLASS_FILE, css_data, sizeof(css_data));
	if (ini.spiffs_time != spiffs_time)
	{
		ini.spiffs_time=spiffs_time;
		SaveSettings(&ini, sizeof(ini));
	}
#endif
}

void ValidateSettings()
{
  if (ini.lang<0 || ini.lang>=Languages) ini.lang=0;
  if (ini.step_delay_mks < MIN_STEP_DELAY) ini.step_delay_mks=MIN_STEP_DELAY;
	if (ini.lang<0 || ini.lang>=Languages) ini.lang=0;
	if (ini.pinout<0 || ini.pinout>=3) ini.pinout=0;
	if (ini.step_delay_mks<MIN_STEP_DELAY || ini.step_delay_mks>=65000) ini.step_delay_mks=def_step_delay_mks;
	if (ini.timezone<-11*60 || ini.timezone>=14*60) ini.timezone=0;
	if (ini.full_length<300 || ini.full_length>=99999) ini.full_length=10000;
	if (ini.switch_reversed>1) ini.switch_reversed=1;
	if (ini.switch_ignore_steps<5) ini.switch_ignore_steps=DEFAULT_SWITCH_IGNORE_STEPS;
	if (ini.switch_ignore_steps>5000) ini.switch_ignore_steps=5000;
	if (!ini.mqtt_server[0]) strcpy(ini.mqtt_server, def_mqtt_server);
	if (ini.mqtt_port == 0) ini.mqtt_port=def_mqtt_port;
	if (ini.mqtt_ping_interval < 5) ini.mqtt_ping_interval=60;
	if (!ini.mqtt_topic_state[0]) strcpy(ini.mqtt_topic_state, def_mqtt_topic_state);
	if (!ini.mqtt_topic_command[0]) strcpy(ini.mqtt_topic_command, def_mqtt_topic_command);
	if (ini.mqtt_state_type>3) ini.mqtt_state_type=0;
}	

void setup_Settings(void)
{
  memset(&ini, sizeof(ini), 0);
  if (LoadSettings(&ini, sizeof(ini)))
	{
		Serial.println("Settings loaded");
	} else
	{
		strcpy(ini.hostname  , def_hostname);
		strcpy(ini.ssid      , def_ssid);
		strcpy(ini.password  , def_password);
		strcpy(ini.ntpserver , def_ntpserver);
		ini.lang=0;
		ini.pinout=2;
		ini.reversed=false;
		ini.step_delay_mks=def_step_delay_mks;
		ini.timezone=3*60; // Default City time zone by default :)
		ini.full_length=11300;
		ini.spiffs_time=0;
		ini.mqtt_enabled=false;
		strcpy(ini.mqtt_server, def_mqtt_server);
		strcpy(ini.mqtt_login, def_mqtt_login);
		strcpy(ini.mqtt_password, def_mqtt_password);
		ini.mqtt_port=def_mqtt_port;
		ini.mqtt_ping_interval=60;
		strcpy(ini.mqtt_topic_state, def_mqtt_topic_state);
		strcpy(ini.mqtt_topic_command, def_mqtt_topic_command);
		ini.mqtt_state_type=0;
		ini.switch_ignore_steps=DEFAULT_SWITCH_IGNORE_STEPS;
		Serial.println("Settings set to default");
	}
	ValidateSettings();
}

void print_SPIFFS_info()
{
	FSInfo fs_info;
	if (SPIFFS.info(fs_info))
	{
		Serial.printf("SPIFFS totalBytes: %i\n", fs_info.totalBytes);
		Serial.printf("SPIFFS usedBytes: %i\n", fs_info.usedBytes);
		Serial.printf("SPIFFS blockSize: %i\n", fs_info.blockSize);
		Serial.printf("SPIFFS pageSize: %i\n", fs_info.pageSize);
		Serial.printf("SPIFFS maxOpenFiles: %i\n", fs_info.maxOpenFiles);
		Serial.printf("SPIFFS maxPathLength: %i\n", fs_info.maxPathLength);
		Dir dir = SPIFFS.openDir("/");
		while (dir.next()) {
				Serial.print(dir.fileName());
				Serial.print(" ");
				File f = dir.openFile("r");
				Serial.println(f.size());
				f.close();
		}			
	} else
		Serial.println("SPIFFS.info() failed");
}

void setup_SPIFFS()
{
  if (SPIFFS.begin()) 
	{
    Serial.println("SPIFFS Active");
		// print_SPIFFS_info();
  } else {
    Serial.println("Unable to activate SPIFFS");
  }
}

void setup_OTA()
{
  ArduinoOTA.onStart([]() {
		StopTimer();
    Serial.println("Updating");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.print(".");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.setHostname(ini.hostname);
  ArduinoOTA.begin();
}

void HTTP_handleRoot(void);
void HTTP_handleOpen(void);
void HTTP_handleClose(void);
void HTTP_handleStop(void);
void HTTP_handleTest(void);
void HTTP_handleSettings(void);
void HTTP_handleAlarms(void);
void HTTP_handleReboot(void);
void HTTP_handleFormat(void);
void HTTP_handleXML(void);
void HTTP_handleSet(void);
void HTTP_redirect(String link);

void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Booting...");

  setup_SPIFFS();
  setup_Settings(); // setup and load settings.
	init_SPIFFS(); // create static HTML files, if needed

  Serial.print("Hostname: ");
  Serial.println(ini.hostname);

  WiFi.hostname(ini.hostname);
  WiFi.mode(WIFI_STA);

  int attempts=3;
  while (attempts>0)
  {
    WiFi.begin(ini.ssid, ini.password);
    if (WiFi.waitForConnectResult() == WL_CONNECTED) break;
    Serial.println("WiFi failed, retrying.");
    attempts--;
    if (attempts>0) delay(1000);
    else
    { // Cannot connect to WiFi. Lets make our own Access Point with blackjack and hookers
      Serial.print("Starting access point. SSID: ");
      Serial.println(ini.hostname);
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ini.hostname); // ... but without password
    }
  }

  if (!MDNS.begin(ini.hostname)) Serial.println("Error setting up MDNS responder!");
	else Serial.println("mDNS responder started");

  httpUpdater.setup(&httpServer, update_path, update_username, update_password);
  httpServer.on("/",         HTTP_handleRoot);
  httpServer.on("/open",     HTTP_handleOpen);
  httpServer.on("/close",    HTTP_handleClose);
  httpServer.on("/stop",     HTTP_handleStop);
  httpServer.on("/test",     HTTP_handleTest);
  httpServer.on("/settings", HTTP_handleSettings);
  httpServer.on("/alarms", 	 HTTP_handleAlarms);
  httpServer.on("/reboot",   HTTP_handleReboot);
  httpServer.on("/format",   HTTP_handleFormat);
	httpServer.on("/xml",      HTTP_handleXML);
	httpServer.on("/set",      HTTP_handleSet);
  httpServer.serveStatic(FAV_FILE, SPIFFS, FAV_FILE, "max-age=86400");
  httpServer.serveStatic(CLASS_FILE, SPIFFS, CLASS_FILE, "max-age=86400");
  httpServer.begin();

  Serial.println(WiFi.localIP());

  MDNS.addService("http", "tcp", 80);

  pinMode(PIN_SWITCH, INPUT_PULLUP);
  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(PIN_C, OUTPUT);
  pinMode(PIN_D, OUTPUT);
  MotorOff();

  setup_OTA();
  setup_NTP();
	setup_MQTT();

  // start position is twice as fully open. So on first close we will go up till home position
  position=ini.full_length*2+UP_SAFE_LIMIT;
  if (IsSwitchPressed()) position=0; // Fully open, if switch is pressed
  roll_to=position;
	
	FillStepsTable();
	
	SetupTimer();
}

// ============================= HTTP ===================================
String HTML_header()
{
  String ret ="<!doctype html>\n" \
	"<html>\n" \
	"<head>\n" \
	"  <meta http-equiv=\"Content-type\" content=\"text/html; charset=utf-8\">" \
	"  <meta http-equiv=\"X-UA-Compatible\" content=\"IE=Edge\">\n" \
	"  <meta name = \"viewport\" content = \"width=device-width, initial-scale=1\">\n" \
	"  <title>"+SL("Lazy rolls", "Ленивые шторы")+"</title>\n" \
	"  <link rel=\"stylesheet\" href=\"styles.css\" type=\"text/css\">\n" \
// ToDo: move to styles.css
"<style type=\"text/css\">" \
".settings input[type=text] {" \
"	width: 255px;" \
"}	" \
"</style>	" \
	
	"  <script>\n" \
	"    var timerId;\n" \
	"    var timeout;\n" \
	"    var active;\n" \
	"    window.onfocus = function() { active = true; clearTimeout(timerId); GetStatus(); };\n" \
	"    window.onblur = function() { active = false; clearTimeout(timerId); };\n" \
	"    function GetStatus()\n" \
	"    { if (active) {\n"\
	"      nocache = \"&nocache=\" + Math.random() * 1000000;\n" \
	"      var request = new XMLHttpRequest();\n" \
	"      request.onreadystatechange = function()\n" \
	"        {\n" \
	"          if (this.readyState == 4) {\n" \
	"            if (this.status == 200) {\n" \
	"              if (this.responseXML != null) {\n" \
	"                var element; \n" \
	"                document.getElementById(\"time\").innerHTML = this.responseXML.getElementsByTagName('Time')[0].childNodes[0].nodeValue;\n" \
	"                document.getElementById(\"uptime\").innerHTML = this.responseXML.getElementsByTagName('UpTime')[0].childNodes[0].nodeValue;\n" \
	"                document.getElementById(\"RSSI\").innerHTML = this.responseXML.getElementsByTagName('RSSI')[0].childNodes[0].nodeValue;\n" \
	"                document.getElementById(\"pos\").innerHTML = this.responseXML.getElementsByTagName('Now')[0].childNodes[0].nodeValue;\n" \
	"                document.getElementById(\"dest\").innerHTML = this.responseXML.getElementsByTagName('Dest')[0].childNodes[0].nodeValue;\n" \
	"                document.getElementById(\"switch\").innerHTML = this.responseXML.getElementsByTagName('End1')[0].childNodes[0].nodeValue;\n" \
	"                document.getElementById(\"mqtt\").innerHTML = this.responseXML.getElementsByTagName('MQTT')[0].childNodes[0].nodeValue;\n" \

	"                if (document.getElementById(\"pos\").innerHTML != document.getElementById(\"dest\").innerHTML)\n" \
	"                  timeout=500;\n" \
	"                else\n" \
	"                  timeout=5000;\n" \
	"              }\n" \
	"            }\n" \
	"          }\n" \
	"        }\n" \
	"      // send HTTP GET request   \n" \
	"      request.open(\"GET\", \"xml\");\n" \
	"      request.send(null);\n" \
	"    } timerId = setTimeout('GetStatus()', timeout);\n" \
	"    }\n" \
	"    function Call(url)\n" \
	"    {\n" \
	"      clearTimeout(timerId);\n" \
	"      var xhttp = new XMLHttpRequest();\n" \
	"      xhttp.open(\"GET\", url);\n" \
	"      xhttp.send();\n" \
	"      timeout=200;\n" \
	"      GetStatus();\n" \
	"    }\n" \
	"    function Open() { Call(\"set?pos=0\"); return false; }\n" \
	"    function Close() { Call(\"set?pos=100\"); return false; }\n" \
	"    function Stop() { Call(\"stop\"); return false; }\n" \
	"  </script>\n" \
	"</head>\n" \
	"<body onload=\"{ active=true; GetStatus(); };\">\n" \
	" <div id=\"wrapper2\">\n" \
	"  <div id=\"wrapper\">\n" \
	"    <header>"+SL("Lazy rolls", "Ленивые шторы")+"</header>\n" \
	"    <nav></nav>\n" \
	"    <div id=\"heading\"></div>\n";
  return ret;
}

String HTML_footer()
{
  String ret = "  </div></div>\n" \
  "  <footer></footer>\n" \
  "</body>\n" \
  "</html>";
  return ret;
}

String HTML_openCloseStop()
{
	String out;
  out = "<a href=\"open\" onclick=\"return Open();\">["+SL("Open", "Открыть")+"]</a> \n";
  out += "<a href=\"close\" onclick=\"return Close();\">["+SL("Close", "Закрыть")+"]</a> \n";
//  if (position != roll_to)
  out += "<a href=\"stop\" onclick=\"return Stop();\">["+SL("Stop", "Стоп")+"]</a> \n";
	return out;
}	

String HTML_tableLine(const char *name, String val, char *id=NULL)
{
  String ret="<tr><td>";
  ret+=name;
  if (id==NULL)
    ret+="</td><td>";
  else
    ret+="</td><td id=\""+String(id)+"\">";
  ret+=val;
  ret+="</td></tr>\n";
  return ret;
}

String HTML_editString(const char *header, const char *id, const char *inistr, int len)
{
  String out;

  out="<tr><td class=\"idname\">";
  out+=header;
  out+="</td><td class=\"val\"><input type=\"text\" name=\"";
  out+=String(id);
  out+="\" id=\"";
  out+=String(id);
  out+="\" value=\"";
  out+=String(inistr);
  out+="\" maxlength=\"";
  out+=String(len-1);
  out+="\"\></td></tr>\n";

  return out;
}

String HTML_addOption(int value, int selected, const char *text)
{
	return "<option value=\""+String(value)+"\""+(selected==value ? " selected=\"selected\"" : "")+">"+text+"</option>\n";
}

String MemSize2Str(uint32_t mem)
{
	if (mem%(1024*1024) == 0) return String(mem/1024/1024)+ SL(" MB", " МБ");
	if (mem%1024 == 0) return String(mem/1024)+SL(" KB"," КБ");
	return String(mem)+SL(" B", " Б");
}

String HTML_status()
{
  uint32_t realSize = ESP.getFlashChipRealSize();
  uint32_t ideSize = ESP.getFlashChipSize();
  FlashMode_t ideMode = ESP.getFlashChipMode();
  String out;

  out = HTML_header();
  out.reserve(4096);

  out += "    <section class=\"info\"><table>\n";
  out += "<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Status", "Статус")+"</td></tr>\n";
  out += HTML_tableLine(L("Version", "Версия"), VERSION);
  out += HTML_tableLine(L("IP", "IP"), WiFi.localIP().toString());
  if (lastSync==0)
    out += HTML_tableLine(L("Time", "Время"), SL("unknown", "хз"), "time");
  else
    out += HTML_tableLine(L("Time", "Время"), TimeStr() + " ["+ DoWName(DayOfWeek(getTime())) +"]", "time");
	
  out += HTML_tableLine(L("Uptime", "Аптайм"), UptimeStr(), "uptime");
  out += HTML_tableLine(L("RSSI", "RSSI"), String(WiFi.RSSI())+SL(" dBm", " дБм"), "RSSI");

  out += "<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Position", "Положение")+"</td></tr>\n";
  out += HTML_tableLine(L("Now", "Сейчас"), String(position), "pos");
  out += HTML_tableLine(L("Roll to", "Цель"), String(roll_to), "dest");
  out += HTML_tableLine(L("Switch", "Концевик"), onoff[ini.lang][IsSwitchPressed()], "switch");
    
  out += "<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Memory", "Память")+"</td></tr>\n";
  out += HTML_tableLine(L("Flash id", "ID чипа"), String(ESP.getFlashChipId(), HEX));
  out += HTML_tableLine(L("Real size", "Реально"), MemSize2Str(realSize));
  out += HTML_tableLine(L("IDE size", "Прошивка"), MemSize2Str(ideSize));
  if(ideSize != realSize) {
    out += HTML_tableLine(L("Config", "Конфиг"), L("error!", "ошибка!"));
  } else {
    out += HTML_tableLine(L("Config", "Конфиг"), "OK!");
  }
  out += HTML_tableLine(L("Speed", "Частота"), String(ESP.getFlashChipSpeed()/1000000)+SL("MHz", "МГц"));
  out += HTML_tableLine(L("Mode", "Режим"), (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN"));
  //out += HTML_tableLine("Host", String(ini.hostname));
	FSInfo fs_info;
	out += "<tr class=\"sect_name\"><td colspan=\"2\">"+SL("SPIFFS", "SPIFFS")+"</td></tr>\n";
	if (SPIFFS.info(fs_info))
	{
		out += HTML_tableLine(L("Size", "Выделено"), MemSize2Str(fs_info.totalBytes));
		out += HTML_tableLine(L("Used", "Занято"), MemSize2Str(fs_info.usedBytes));
	} else
		out += HTML_tableLine(L("Error", "Ошибка"), "<a href=\"/format\">"+SL("Format", "Формат-ть")+"</a>");
#ifdef MQTT	
	out += "<tr class=\"sect_name\"><td colspan=\"2\">"+SL("MQTT", "MQTT")+"</td></tr>\n";
	out += HTML_tableLine(L("MQTT", "MQTT"), MQTTstatus(), "mqtt");
	if (ini.mqtt_enabled)
	{
	}
#endif

  out +="</table></section>\n";

  return out;
}

String HTML_mainmenu(void)
{
  String out;

  out.reserve(128);

  out += "<section class=\"status\">\n";
	out += HTML_openCloseStop();
  out += "<a href=\"/\">["+SL("Main", "Главная")+"]</a> \n";
  out += "<a href=\"settings\">["+SL("Settings", "Настройки")+"]</a> \n";
  out += "<a href=\"alarms\">["+SL("Schedule", "Расписание")+"]</a> \n";
  out += "</section>\n";
	return out;
}

void HTTP_handleRoot(void)
{
  String out;

  out=HTML_status();
  
	out += HTML_mainmenu();

  out += HTML_footer();
  httpServer.send(200, "text/html", out);
}

void SaveString(const char *id, char *inistr, int len)
{
	String s;
  if (!httpServer.hasArg(id)) return;

	s=httpServer.arg(id);
	s.trim(); // remove leading and trailing whitespace
  strncpy(inistr, s.c_str(), len-1);
  inistr[len-1]='\0';
}

void SaveInt(const char *id, uint8_t *iniint)
{
  if (!httpServer.hasArg(id)) return;
  *iniint=atoi(httpServer.arg(id).c_str());
}
void SaveInt(const char *id, uint16_t *iniint)
{
  if (!httpServer.hasArg(id)) return;
  *iniint=atoi(httpServer.arg(id).c_str());
}
void SaveInt(const char *id, uint32_t *iniint)
{
  if (!httpServer.hasArg(id)) return;
  *iniint=atoi(httpServer.arg(id).c_str());
}
void SaveInt(const char *id, int *iniint)
{
  if (!httpServer.hasArg(id)) return;
  *iniint=atoi(httpServer.arg(id).c_str());
}
void SaveInt(const char *id, bool *iniint)
{
  if (!httpServer.hasArg(id)) return;
  *iniint=atoi(httpServer.arg(id).c_str());
}

void HTTP_handleSettings(void)
{
  String out;
	char pass[sizeof(ini.password)];

  if (httpServer.hasArg("save"))
  {
		pass[0]='*';
		pass[1]='\0';
    SaveString("hostname", ini.hostname, sizeof(ini.hostname));
    SaveString("ssid",     ini.ssid,     sizeof(ini.ssid));
    SaveString("password", pass,         sizeof(ini.password));
    SaveString("ntp",      ini.ntpserver,sizeof(ini.ntpserver));
		if (strcmp(pass, "*")!=0) memcpy(ini.password, pass, sizeof(ini.password));

		SaveInt("lang", &ini.lang);
		SaveInt("pinout", &ini.pinout);
		SaveInt("reversed", &ini.reversed);
		SaveInt("delay", &ini.step_delay_mks);
		SaveInt("timezone", &ini.timezone);
		SaveInt("length", &ini.full_length);
		SaveInt("switch", &ini.switch_reversed);
		SaveInt("switch_ignore", &ini.switch_ignore_steps);

		pass[0]='*';
		pass[1]='\0';
    ini.mqtt_enabled=httpServer.hasArg("mqtt_enabled");
    SaveString("mqtt_server", ini.mqtt_server, sizeof(ini.mqtt_server));
		SaveInt("mqtt_port", &ini.mqtt_port);
    SaveString("mqtt_login", ini.mqtt_login, sizeof(ini.mqtt_login));
    SaveString("mqtt_password", pass, sizeof(ini.mqtt_password));
    SaveString("ntp", ini.ntpserver,sizeof(ini.ntpserver));
		if (strcmp(pass, "*")!=0) memcpy(ini.mqtt_password, pass, sizeof(ini.mqtt_password));
		SaveInt("mqtt_ping_interval", &ini.mqtt_ping_interval);
    SaveString("mqtt_topic_state", ini.mqtt_topic_state, sizeof(ini.mqtt_topic_state));
    SaveString("mqtt_topic_command", ini.mqtt_topic_command, sizeof(ini.mqtt_topic_command));
		SaveInt("mqtt_state_type", &ini.mqtt_state_type);

    // if (httpServer.hasArg("lang")) ini.lang=atoi(httpServer.arg("lang").c_str());
    // if (httpServer.hasArg("pinout")) ini.pinout=atoi(httpServer.arg("pinout").c_str());
    // if (httpServer.hasArg("reversed")) ini.reversed=atoi(httpServer.arg("reversed").c_str());
    // if (httpServer.hasArg("delay")) ini.step_delay_mks=atoi(httpServer.arg("delay").c_str());
    // if (httpServer.hasArg("timezone")) ini.timezone=atoi(httpServer.arg("timezone").c_str());
    // if (httpServer.hasArg("length")) ini.full_length=atoi(httpServer.arg("length").c_str());
    // if (httpServer.hasArg("switch")) ini.switch_reversed=atoi(httpServer.arg("switch").c_str());
		ValidateSettings();

    SaveSettings(&ini, sizeof(ini));
		
		setup_MQTT();

		FillStepsTable();
		AdjustTimerInterval();
		
    HTTP_redirect("/settings?ok=1");
    return;
  }

  out.reserve(10240);
    
  out=HTML_status();

	out += HTML_mainmenu();

  out += "<section class=\"settings\">\n";

  if (httpServer.hasArg("ok"))
    out+=SL("<p>Saved!<br/>Network settings will be applied after reboot.<br/><a href=\"reboot\">[Reboot]</a></p>\n",
      "<p>Сохранено!<br/>Настройки сети будут применены после перезагрузки.<br/><a href=\"reboot\">[Перезагрузить]</a></p>\n");

  out +="<script>\n" \
	"function Test(dir)\n" \
	"{\n" \
	"document.getElementById(\"btn_up\").disabled=true;\n" \
	"document.getElementById(\"btn_dn\").disabled=true;\n" \
	"pinout=document.getElementById(\"pinout\").value;\n" \
	"reversed=document.getElementById(\"reversed\").value;\n" \
	"delay=document.getElementById(\"delay\").value;\n" \
	"var xhttp = new XMLHttpRequest();\n" \
	"xhttp.onreadystatechange = function() {\n" \
	"if (this.readyState == 4 && this.status == 200) {\n" \
	"document.getElementById(\"btn_up\").disabled=false;\n" \
	"document.getElementById(\"btn_dn\").disabled=false;\n" \
	"document.getElementById(\"pos\").innerHTML=this.responseText;\n" \
	"document.getElementById(\"dest\").innerHTML=this.responseText;\n" \
	"  }};\n" \
	"url=\"test?pinout=\"+pinout+\"&reversed=\"+reversed+\"&delay=\"+delay;\n" \
	"if (dir==1) url=url+\"&up=1\"; else url=url+\"&down=1\";\n" \
	"xhttp.open(\"GET\", url, true);\n" \
	"xhttp.send();\n" \
	"}\n" \
	"function TestUp() { Test(1); }\n" \
	"function TestDown() { Test(0); }\n" \
	"</script>\n" \
	"<form method=\"post\" action=\"/settings\">\n";

  out+="<table>\n";
  out+="<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Network", "Сеть")+"</td></tr>\n";
  out+="<tr><td>"+SL("Language: ", "Язык: ")+"</td><td><select id=\"lang\" name=\"lang\">\n";
  for (int i=0; i<Languages; i++)
  {
    out+="<option value=\""+String(i)+"\"";
    if (i==ini.lang) out+=" selected=\"selected\"";
    out+=+">"+String(Language[i])+"</option>\n";
  }
  out+="</select></td></tr>\n";
  out+=HTML_editString(L("Hostname:", "Имя в сети:"), "hostname", ini.hostname, sizeof(ini.hostname));
  out+=HTML_editString(L("SSID:", "Wi-Fi сеть:"),     "ssid",     ini.ssid,     sizeof(ini.ssid));
  out+=HTML_editString(L("Password:", "Пароль:"),     "password", "*",          sizeof(ini.password));

  out+="<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Time", "Время")+"</td></tr>\n";
  out+=HTML_editString(L("NTP-server:", "NTP-сервер:"),"ntp",     ini.ntpserver,sizeof(ini.ntpserver));
  out+="<tr><td>"+SL("Timezone: ", "Пояс: ")+"</td><td><select id=\"timezone\" name=\"timezone\">\n";
  for (int i=-11*60; i<=14*60; i+=15) // timezones from -11:00 to +14:00 every 15 min
  {
    char b[7];
    sprintf(b, "%+d:%02d", i/60, abs(i%60));
    if (i<0) b[0]='-';
    out+="<option value=\""+String(i)+"\"";
    if (i==ini.timezone) out+=" selected=\"selected\"";
    out+=+">UTC"+String(b)+"</option>\n";
  }
  out+="</select></td></tr>\n";

  out+="<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Motor", "Мотор")+"</td></tr>\n";
	out+="<tr><td>"+SL("Pinout:", "Подключение:")+"</td><td><select id=\"pinout\" name=\"pinout\">\n";
	out+=HTML_addOption(2, ini.pinout, "A-B-C-D");
	out+=HTML_addOption(0, ini.pinout, "A-C-B-D");
	out+=HTML_addOption(1, ini.pinout, "A-B-D-C");
	out+="</select></td></tr>\n" \
	"<tr><td>"+SL("Direction:", "Направление:")+"</td><td><select id=\"reversed\" name=\"reversed\">\n" \
	"<option value=\"1\""+(ini.reversed ? " selected=\"selected\"" : "")+">"+SL("Normal", "Прямое")+"</option>\n" \
	"<option value=\"0\""+(ini.reversed ? "" : " selected=\"selected\"")+">"+SL("Reversed", "Обратное")+"</option>\n" \
	"</select></td></tr>\n";
  out+=HTML_editString(L("Step delay:", "Время шага:"),"delay", String(ini.step_delay_mks).c_str(), 6);
	out+="<tr><td colspan=\"2\">"+SL("(microsecs, "+String(MIN_STEP_DELAY)+"-65000, default 1500)", "(в мкс, "+String(MIN_STEP_DELAY)+"-65000, обычно 1500)")+"</td></tr>\n" \
	"<tr><td colspan=\"2\">\n" \
	"<input id=\"btn_up\" type=\"button\" name=\"up\" value=\""+SL("Test up", "Тест вверх")+"\" onclick=\"TestUp()\">\n" \
	"<input id=\"btn_dn\" type=\"button\" name=\"down\" value=\""+SL("Test down", "Тест вниз")+"\" onclick=\"TestDown()\">\n" \
	"</td></tr>\n";

  out+="<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Endstop", "Концевик")+"</td></tr>\n" \
	"<tr><td>"+SL("Type:", "Тип:")+"</td><td><select id=\"switch\" name=\"switch\">\n" \
	"<option value=\"0\""+(ini.switch_reversed ? "" : " selected=\"selected\"")+">"+SL("Normal closed", "Нормально замкнут")+"</option>\n" \
	"<option value=\"1\""+(ini.switch_reversed ? " selected=\"selected\"" : "")+">"+SL("Normal open", "Нормально разомкнут")+"</option>\n" \
	"</select></td></tr>\n";
  out+=HTML_editString(L("Length:", "Длина:"), "switch_ignore", String(ini.switch_ignore_steps).c_str(), 6);
  out+="<tr><td colspan=\"2\">"+SL("(switch ignore zone, steps, default 100)", "(игнорировать концевик первые шаги, обычно 100)")+"</td></tr>\n";

  out+="<tr class=\"sect_name\"><td colspan=\"2\">"+SL("Curtain", "Штора")+"</td></tr>\n";
  out+=HTML_editString(L("Length:", "Длина:"), "length", String(ini.full_length).c_str(), 6);
  out+="<tr><td colspan=\"2\">"+SL("(closed position, steps)", "(шагов до полного закрытия)")+"</td></tr>\n";

  out+="<tr class=\"sect_name\"><td colspan=\"2\">"+SL("MQTT", "MQTT")+"</td></tr>\n";
	out+="<tr><td colspan=\"2\"><label for=\"mqtt_enabled\">\n";
	out+="<input type=\"checkbox\" id=\"mqtt_enabled\" name=\"mqtt_enabled\"" + String((ini.mqtt_enabled) ? " checked" : "") + "/>\n";
	out+=SL("MQTT enabled Help:", "MQTT включен Помощь:")+" <a href=\"http://imlazy.ru/rolls/mqtt.html\">imlazy.ru/rolls/mqtt.html</a></label></td></tr>\n";
  out+=HTML_editString(L("Server:", "Сервер:"), "mqtt_server", ini.mqtt_server, sizeof(ini.mqtt_server));
  out+=HTML_editString(L("Port:", "Порт:"), "mqtt_port", String(ini.mqtt_port).c_str(), 5);
  out+=HTML_editString(L("Login:", "Логин:"), "mqtt_login", ini.mqtt_login, sizeof(ini.mqtt_login));
  out+=HTML_editString(L("Password:", "Пароль:"), "mqtt_password", "*", sizeof(ini.mqtt_password));
  out+=HTML_editString(L("Keep-alive:", "Keep-alive:"), "mqtt_ping_interval", String(ini.mqtt_ping_interval).c_str(), 5);
  out+=HTML_editString(L("Commands:", "Команды:"), "mqtt_topic_command", ini.mqtt_topic_command, sizeof(ini.mqtt_topic_command));
  out+=HTML_editString(L("State:", "Статус:"), "mqtt_topic_state", ini.mqtt_topic_state, sizeof(ini.mqtt_topic_state));
	out+="<tr><td>"+SL("Type:", "Формат:")+"</td><td><select id=\"mqtt_state_type\" name=\"mqtt_state_type\">\n";
	out+=HTML_addOption(0, ini.mqtt_state_type, "0-100 (%)");
	out+=HTML_addOption(1, ini.mqtt_state_type, "ON/OFF");
	out+=HTML_addOption(2, ini.mqtt_state_type, "0/1");
	out+=HTML_addOption(3, ini.mqtt_state_type, "JSON");
	out+="</select></td></tr>\n";
  
  out+="<tr class=\"sect_name\"><td colspan=\"2\"><input id=\"save\" type=\"submit\" name=\"save\" value=\""+SL("Save", "Сохранить")+"\"></td></tr>\n";
  out+="</table>\n";
  out+="</form>\n";
  out+="</section>\n";

  out += HTML_footer();

  httpServer.send(200, "text/html", out);
}

uint32_t StrToTime(String s)
{
	uint32_t t=0;
	
	if (s.length()<5) return 0;
	if (s[0]>='0' && s[0]<='9') t+=(s[0]-'0')*10*60; else return 0;
	if (s[1]>='0' && s[1]<='9') t+=(s[1]-'0')* 1*60; else return 0;
	if (s[3]>='0' && s[3]<='9') t+=(s[3]-'0')*   10; else return 0;
	if (s[4]>='0' && s[4]<='9') t+=(s[4]-'0')*    1; else return 0;
	return t;
}

String TimeToStr(uint32_t t)
{
	char buf[6];
  sprintf(buf, "%02d:%02d", t/60%24, t%60);
	return String(buf);
}	

void HTTP_handleAlarms(void)
{
  String out;

  if (httpServer.hasArg("save"))
  {
		for (int a=0; a<ALARMS; a++)
		{
		  String n=String(a);
      if (httpServer.hasArg("en"+n)) 
				ini.alarms[a].flags |= ALARM_FLAG_ENABLED;
		  else
				ini.alarms[a].flags &= ~ALARM_FLAG_ENABLED;

      if (httpServer.hasArg("time"+n))
			{
				ini.alarms[a].time=StrToTime(httpServer.arg("time"+n));
			}

      if (httpServer.hasArg("dest"+n))
			{
				ini.alarms[a].percent_open=atoi(httpServer.arg("dest"+n).c_str());
				if (ini.alarms[a].percent_open>100) ini.alarms[a].percent_open=100;
			}

			uint8_t b=0;
			for (int d=6; d>=0; d--)
			{
				b=b<<1;
        if (httpServer.hasArg("d"+n+"_"+String(d))) b|=1;
			}
			ini.alarms[a].day_of_week=b;
		}	

    SaveSettings(&ini, sizeof(ini));
  }

  out.reserve(16384);
    
  out=HTML_status();

	out += HTML_mainmenu();

  out += "<section class=\"alarms\">\n";
	out += "<form method=\"post\" action=\"/alarms\">\n";
	out += "<table width=\"100%\">\n";

	for (int a=0; a<ALARMS; a++)
	{
		String n=String(a);
		out += "<tr><td colspan=\"2\">\n";
		if (a>0) out += "<hr/>\n";
		out += "<label for=\"en"+n+"\">\n";
		out += "<input type=\"checkbox\" id=\"en"+n+"\" name=\"en"+n+"\"" + 
		  ((ini.alarms[a].flags & ALARM_FLAG_ENABLED) ? " checked" : "") + "/>\n";
		out += SL("Enabled", "Включено")+"</label></td></tr>\n";
		
		out += "<tr><td style=\"width:1px\"><label for=\"time"+n+"\">"+
		  SL("Time:", "Время:")+"</label></td><td><input type=\"time\" id=\"time"+n+
			"\" name=\"time"+n+"\" value=\""+TimeToStr(ini.alarms[a].time)+"\" required></td></tr>\n";
		
		out += "<tr><td><label for=\"dest"+n+"\">"+
		  SL("Position:", "Положение:")+"</label></td><td><select id=\"dest"+n+"\" name=\"dest"+n+"\">\n";
		for (int p=0; p<=100; p+=20)
		{
			String s = String(p)+"%";
			if (p==0)   s=SL("Open", "Открыть");
			if (p==100) s=SL("Close", "Закрыть");
			out += "<option value=\""+String(p)+"\""+
			  (ini.alarms[a].percent_open==p ? " selected" : "")+">"+s+"</option>\n";
		}
		out += "</select>\n";
		out += "</td></tr><tr><td>";

		out += SL("Repeat:", "Повтор:")+"</td><td class=\"days\">\n";
		for (int d=0; d<7; d++)
		{
			String id="\"d"+n+"_"+String(d)+"\"";
			out += "<label for="+id+">";
			out += "<input type=\"checkbox\" id="+id+" name="+id+
					((ini.alarms[a].day_of_week & (1 << d)) ? " checked" : "")+">";
			out+=DoWName(d);
			out += "</label> \n";
		}
		out += "</td></tr>\n";
	}

  out+="<tr class=\"sect_name\"><td colspan=\"2\"><input id=\"save\" type=\"submit\" name=\"save\" value=\""+SL("Save", "Сохранить")+"\"></td></tr>\n";
	
	out += "</table>\n";
  out+="</form>\n";
  out+="</section>\n";
  out += HTML_footer();

  httpServer.send(200, "text/html", out);
}

void HTTP_redirect(String link)
{
  httpServer.sendHeader("Location", link, true);
  httpServer.send(302, "text/plain", "");
}

void HTTP_handleReboot(void)
{
  HTTP_redirect(String("/"));
	delay(500);
  ESP.reset();
}

void HTTP_handleFormat(void)
{
  HTTP_redirect(String("/"));
	delay(500);
	SPIFFS.end();
	SPIFFS.format();
  setup_SPIFFS();
	SaveSettings(&ini, sizeof(ini));
	Serial.println("SPIFFS formatted. rebooting");
	delay(500);
  ESP.reset();
}

void HTTP_handleOpen(void)
{
  Open();
  HTTP_redirect(String("/"));
}

void HTTP_handleClose(void)
{
  Close();
  HTTP_redirect(String("/"));
}

void HTTP_handleStop(void)
{
	Stop();
  HTTP_redirect(String("/"));
}

void HTTP_handleSet(void)
{
	if (httpServer.hasArg("pos"))
	{
		int pos=atoi(httpServer.arg("pos").c_str());
		if (pos==0) Open();
		else if (pos==100) Close();
		else if (pos>0 && pos<100) ToPercent(pos);
	}
  httpServer.send(200, "text/XML", "");
}

void HTTP_handleTest(void)
{
  bool dir=false;
  uint8_t bak_pinout;
  bool bak_reversed;
  uint16_t bak_step_delay_mks;
  int steps=300;

  bak_pinout=ini.pinout;
  bak_reversed=ini.reversed;
  bak_step_delay_mks=ini.step_delay_mks;

  if (httpServer.hasArg("up")) dir=true;
  if (httpServer.hasArg("reversed")) ini.reversed=atoi(httpServer.arg("reversed").c_str());
  if (httpServer.hasArg("pinout")) ini.pinout=atoi(httpServer.arg("pinout").c_str());
  if (ini.pinout>=3) ini.pinout=0;
  if (httpServer.hasArg("delay")) ini.step_delay_mks=atoi(httpServer.arg("delay").c_str());
  if (ini.step_delay_mks<MIN_STEP_DELAY) ini.step_delay_mks=MIN_STEP_DELAY;
  if (ini.step_delay_mks>65000) ini.step_delay_mks=65000;
  if (httpServer.hasArg("steps")) steps=atoi(httpServer.arg("steps").c_str());
  if (steps<0) steps=0;
	
	FillStepsTable();
	AdjustTimerInterval();

	switch_ignore_steps=ini.switch_ignore_steps;
	if (dir)
		roll_to=position-steps;
  else
		roll_to=position+steps;
	
	uint32_t start_time=millis();
	while(roll_to != position && (millis()-start_time < 10000)) 
	{
		delay(10);
		yield();
	}
	roll_to=position;
	
  ini.pinout=bak_pinout;
  ini.reversed=bak_reversed;
  ini.step_delay_mks=bak_step_delay_mks;
	
	FillStepsTable();
	AdjustTimerInterval();

  httpServer.send(200, "text/html", String(position));
}

void HTTP_handleXML(void)
{
	String XML;
  uint32_t realSize = ESP.getFlashChipRealSize();
  uint32_t ideSize = ESP.getFlashChipSize();
  FlashMode_t ideMode = ESP.getFlashChipMode();

  XML.reserve(1024);
  XML="<?xml version='1.0'?>";
  XML+="<Curtain>";
  XML+="<Info>";
  XML+="<Version>";
  XML+=VERSION;
  XML+="</Version>";
  XML+="<IP>";
  XML+=WiFi.localIP().toString();
  XML+="</IP>";
  XML+="<Time>";
  if (lastSync == 0)
    XML+=SL("unknown", "хз");
  else
    XML+=TimeStr() + " [" + DoWName(DayOfWeek(getTime())) + "]";
  XML+="</Time>";
  XML+="<UpTime>";
  XML+=UptimeStr();
  XML+="</UpTime>";
  XML+="<RSSI>";
  XML+=String(WiFi.RSSI()) + SL(" dBm", " дБм");
  XML+="</RSSI>";
  XML+="<MQTT>";
  XML+=MQTTstatus();
  XML+="</MQTT>";
  XML+="</Info>";

  XML+="<ChipInfo>";
  XML+="<ID>";
  XML+=String(ESP.getChipId(), HEX);
  XML+="</ID>";
  XML+="<FlashID>";
  XML+=String(ESP.getFlashChipId(), HEX);
  XML+="</FlashID>";
  XML+="<RealSize>";
  XML+=MemSize2Str(realSize);
  XML+="</RealSize>";
  XML+="<IdeSize>";
  XML+=MemSize2Str(ideSize);
  XML+="</IdeSize>";

  XML+="<Speed>";
  XML+=String(ESP.getFlashChipSpeed() / 1000000) + SL("MHz", "МГц");
  XML+="</Speed>";
  XML+="<IdeMode>";
  XML+=(ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN");
  XML+="</IdeMode>";
  XML+="</ChipInfo>";

  XML+="<Position>";
  XML+="<Now>";
  XML+=String(position);
  XML+="</Now>";
  XML+="<Dest>";
  XML+=String(roll_to);
  XML+="</Dest>";
  XML+="<Max>";
  XML+=String(ini.full_length);
  XML+="</Max>";
  XML+="<End1>";
  XML+=onoff[ini.lang][IsSwitchPressed()];
  XML+="</End1>";
  XML+="</Position>";
  XML+="</Curtain>";

  httpServer.send(200, "text/XML", XML);
}

void Scheduler()
{
  uint32_t t;
	static uint32_t last_t;
	int dayofweek;

  t=getTime();
  if (t == 0) return;
	dayofweek = DayOfWeek(t); // 0 - monday
  t=t % DAY; // time from day start
	t=t/60; // in minutes
	
	if (t == last_t) return; // this minute already handled
	last_t=t;

	for (int a=0; a<ALARMS; a++)
	{
		if (!(ini.alarms[a].flags & ALARM_FLAG_ENABLED)) continue;
		if ( (ini.alarms[a].time != t)) continue;
		if (!(ini.alarms[a].day_of_week & (1<<dayofweek)) && (ini.alarms[a].day_of_week != 0)) continue;
		
		if (ini.alarms[a].day_of_week == 0) // if no repeat
		{
			ini.alarms[a].flags &= ~ALARM_FLAG_ENABLED; // disabling
			SaveSettings(&ini, sizeof(ini));
		}

		if (ini.alarms[a].percent_open == 0)
			Open();
		else
		{
			if (position<0) position=0;
			roll_to=ini.full_length * ini.alarms[a].percent_open / 100;
		}
	}
}

unsigned long last_reconnect=0;
void loop(void) 
{
  if(WiFi.getMode() == WIFI_AP_STA || WiFi.getMode() == WIFI_AP)
  { // in soft AP mode, trying to connect to network
		if (last_reconnect==0) last_reconnect=millis();
    if (millis()-last_reconnect > 60*1000) // every 60 sec
    {
      Serial.println("Trying to reconnect");
      last_reconnect=millis();
      WiFi.begin(ini.ssid, ini.password);
      if (WiFi.waitForConnectResult() == WL_CONNECTED)
      {
        Serial.println("Reconnected to network in STA mode. Closing AP");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
      } else
				WiFi.mode(WIFI_AP);
    }
  }
  
  httpServer.handleClient();
  ArduinoOTA.handle();
  SyncNTPTime();
  Scheduler();
	ProcessMQTT();
	delay(1);
}
