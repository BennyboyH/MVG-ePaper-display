/*
   
   mvg.ino - Based on kvv from harbaum: https://github.com/harbaum/kvv

   Sketch for ESP8266 driving a 2.9" quad color eink display.

   Displaying the departure information for KVV tram lines in Munich.
   Specify your preferred station id below and setup your WIFI credentials
   and the device will update the eink display whenever powered up and go
   to deep sleep / power save for a specified amount of time, until it 
   turns itself on again. Press the reset button for a manuel refresh
*/

// stop IDs can be found in the JSON reply for e.g. Rosenheim using this request in a regular browser:
// https://www.mvg.de/api/bgw-pt/v3/locations?query=Rosenheim
#define STOP_ID  "de:09163:9"  // Rosenheim Bahnhof
//define STOP_ID "de:09163:70" // Endorfer Au, Krones
#define stopName "Rosenheim Bf" // station name, used for display, OPTIONAL
#define TRANSPORT_TYPES "BAHN,BUS,REGIONAL_BUS,TRAM,UBAHN,SBAHN" //possible types: BAHN, BUS, REGIONAL_BUS, TRAM, UBAHN, SBAHN


// number of departures to be requested
#define LIMIT "6"   // the epaper display can display six text lines
#define OFFSET "0"  //adds an offset (e.g. walking distance to the station) in minutes

// make sure there's a wifi.h with the following contents:
// #define WIFI_SSID "<your ssid>"
// #define WIFI_PASSWORD "<your password>"
#include "wifi.h"


#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include <GxEPD2_4C.h>
#include <epd4c/GxEPD2_290c_GDEY029F51.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>

#include <ESP8266HTTPClient.h>

#include <WiFiClientSecureBearSSL.h>


// Eink pins: DC = D6, CS=D3, BUSY=D2, SRAM_CS=-1, RESET=D1
GxEPD2_4C<GxEPD2_290c_GDEY029F51, GxEPD2_290c_GDEY029F51::HEIGHT> display(GxEPD2_290c_GDEY029F51(/*CS=5*/ D3, /*DC=*/ D6, /*RST=*/ D1, /*BUSY=*/ D2));


ESP8266WiFiMulti WiFiMulti;


#define TOP 15
#define SKIP 18
#define COL0_WIDTH 36
#include "FreeSansBold9pt8b.h"

// ****** UTF8-Decoder: convert UTF8-string to extended ASCII *******

// Convert a single Character from UTF8 to Extended ASCII
// Return "0" if a byte has to be ignored
byte utf8ascii(byte ascii) {
  static byte c1;  // Last character buffer
  
  if ( ascii<128 ) {  // Standard ASCII-set 0..0x7F handling  
    c1=0;
    return( ascii );
  }

  // get previous input
  byte last = c1;   // get last char
  c1=ascii;         // remember actual character

  switch (last) {  // conversion depending on first UTF8-character
    case 0xC2: return  (ascii);  break;
    case 0xC3: return  (ascii | 0xC0);  break;
    case 0x82: if(ascii==0xAC) return(0x80);       // special case Euro-symbol
  }

  return  (0);                                     // otherwise: return zero, if character has to be ignored
}

// convert String object from UTF8 String to Extended ASCII
String utf8ascii(String s) {      
  String r="";
  char c;
  for (int i=0; i<s.length(); i++) {
    c = utf8ascii(s.charAt(i));
    if (c!=0) r+=c;
  }
  return r;
}

uint16_t get_dsp_length(String str) {
  int16_t  x, y;
  uint16_t w, h;

  display.getTextBounds(utf8ascii(str), 0, 0, &x, &y, &w, &h);
  return w;
}

//converts unix epoch time into human-readable format
void parse_time(struct tm *timeinfo, long long millis) {
  time_t seconds = millis / 1000; 
  localtime_r(&seconds, timeinfo); 
}


// void parse_reply(String payload) {
void parse_reply(Stream& jsonStream) {
  String debugDump = "";
  while (jsonStream.available()) {
    debugDump += (char)jsonStream.read();
  }
  //Serial.println("----- Raw JSON content -----");
  //Serial.println(debugDump); //show raw values for debugging
  //Serial.println("----------------------------");
  int16_t  x, y;
  uint16_t w, h;

  //We need to search for the start of the array, because for whatever fucking reason, MVG sends a 3-hexadigit code at the start of the response
  int jsonStart = debugDump.indexOf('[');
  if (jsonStart > 0) {
    Serial.println(debugDump.substring(0, jsonStart));
    debugDump = debugDump.substring(jsonStart);
  }

  DynamicJsonDocument doc(4096);

  // install filters to extract only the required information from the stream
  StaticJsonDocument<512> filter;
  filter[0]["label"] = true;                // line number
  filter[0]["destination"] = true;
  filter[0]["plannedDepartureTime"] = true;
  filter[0]["realtime"] = true;             //real-time information available?
  filter[0]["realtimeDepartureTime"] = true;
  filter[0]["delayInMinutes"] = true;

  DeserializationError error = deserializeJson(doc, debugDump);  
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  JsonArray list = doc.as<JsonArray>();
  if (list.isNull() || list.size() == 0) {
    Serial.println("Empty or invalid JSON list!");
    return;
  }
  //parse current time into timeinfo
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // one would usually use strftime, but that adds leading 0's to the
  // month and hour which we don't want to save space
  // max length of timestring is "DD.MM.YY HH:mm" -> 15 Bytes incl \0-term 
  char timeStamp[15];
  sprintf(timeStamp, "%d.%d.%02d %d:%02d", 
    timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year - 100,
    timeinfo.tm_hour, timeinfo.tm_min);

  
  Serial.printf("Name: %s\n", stopName);
  Serial.printf("Timestamp: %s\n", timeStamp);

  display.init();
  display.setFont(&FreeSansBold9pt8b);
  display.firstPage();
  display.setTextSize(1);
  display.setRotation(3);

  // top row has a black background
  display.fillRect(0, 0, 296, SKIP, GxEPD_BLACK);

  // station name in white
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(COL0_WIDTH+2, TOP);
  display.print(utf8ascii(stopName));

  // time stamp is right aligned
  display.getTextBounds(timeStamp, 0, 0, &x, &y, &w, &h);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(295-w-2, TOP);
  display.print(timeStamp);

  //No sorting needed for MVG

    for (size_t i = 0; i < list.size(); i++) {
    JsonObject nobj = list[i];

    const char *route = nobj["label"];
    const char *direction = nobj["destination"];

    long long departureMillis = nobj.containsKey("realtimeDepartureTime") ? //contains realtimeDeparture?
                                nobj["realtimeDepartureTime"] :             //if yes, use realtimeDepartureTime
                                nobj["plannedDepartureTime"];               //if not, use plannedDepartureTime

    int delayMinutes = nobj.containsKey("delayInMinutes") ? //contains delayInMinutes?
                       nobj["delayInMinutes"].as<int>() :   //if yes, use delayInMinutes
                       0;                                   //if not, delayMinutes = 0

    struct tm deptime;
    parse_time(&deptime, departureMillis);

    //calculate countdown if real-time data is available
    int countdown = -1;
    if (nobj["realtime"].as<bool>()) {
      time_t now = time(nullptr);
      countdown = (departureMillis / 1000 - now) / 60;
    }

    // Create nice time string. Countdown is actually sometimes < 0 if the vehicle
    // was expected to arrive already
    char time[8];
    if (countdown <= 0 && countdown != -1)       strcpy(time, "sofort");
    else if (countdown > 0 && countdown < 10)    sprintf(time, "%d min", countdown);
    else                                         sprintf(time, "%d:%02d", deptime.tm_hour, deptime.tm_min);

    Serial.printf("[%s] %s %s\n", route, direction, time);

    // up to 6  entries fit onto the screen
    if(i < 6) {
      uint16_t w, dw;
      // route column has red background
      display.fillRect(0, TOP + SKIP * i + 5, COL0_WIDTH, SKIP - 2, GxEPD_RED);

      // center route in first column
      display.setTextColor(GxEPD_WHITE);
      w = get_dsp_length(route);
      display.setCursor((COL0_WIDTH - w) / 2 - 1, TOP + SKIP + SKIP * i);
      display.print(route);

      // left align destination
      display.setTextColor(GxEPD_BLACK, GxEPD_WHITE);
      String destStr = String(direction);
      dw = get_dsp_length(destStr);
      w = get_dsp_length(time);

      // check if destination fits left of time or if it
      // needs to be truncated
      if (COL0_WIDTH + 2 + dw >= 295 - w - 2) {
        // destination needs to be truncated.

        // append elipsis (...) and recalculate display width
        destStr.concat("...");
        dw = get_dsp_length(destStr);
        // truncate until destination fits
        while (COL0_WIDTH + 2 + dw >= 295 - w - 2) {
          destStr = destStr.substring(0, destStr.length() - 4);
          destStr.concat("...");
          dw = get_dsp_length(destStr);
        }
      }

      display.setCursor(COL0_WIDTH + 2, TOP + SKIP + SKIP * i);
      display.print(utf8ascii(destStr));

      // display time right aligned
      if (delayMinutes > 0) {
        display.setTextColor(GxEPD_RED, GxEPD_WHITE); // delayed = red
      } else {
        display.setTextColor(GxEPD_BLACK, GxEPD_WHITE); // punctual = black
      }
      display.setCursor(295 - w - 2, TOP + SKIP + SKIP * i);
      display.print(time);
    }
  }

  display.display();  
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(10000);

  Serial.println();
  Serial.println();
  Serial.println();

  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  // wait for WiFi link up
  while((WiFiMulti.run() != WL_CONNECTED)) {
    Serial.print('.');
    digitalWrite(LED_BUILTIN, HIGH);  // led off
    delay(50);
    digitalWrite(LED_BUILTIN, LOW);   // led on
    delay(50);
  }

  std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);

  client->setInsecure();

  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov"); //sets automatic timezone for Germany
  Serial.println("waiting for ntp...");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(100);
    now = time(nullptr);
  }
  Serial.println("time received.");


  HTTPClient https;

  Serial.print("[HTTPS] begin...\n");
  if (https.begin(*client, "https://www.mvg.de/api/bgw-pt/v3/departures?"
        "globalId=" STOP_ID "&transportTypes=" TRANSPORT_TYPES "&offsetInMinutes=" OFFSET "&limit=" LIMIT)) {

    Serial.print("[HTTPS] GET...");
    // start connection and send HTTP header
    int httpCode = https.GET();

    // httpCode will be negative on error
    if (httpCode > 0) {
      // HTTP header has been send and Server response header has been handled
      Serial.printf(" code: %d\n", httpCode);

      // file found at server
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY){
        parse_reply(https.getStream());
      }

    } else{
      Serial.printf(" failed, error: %s\n", https.errorToString(httpCode).c_str());
    }

    https.end();
  } else
    Serial.printf("[HTTPS] Unable to connect\n");
    
  Serial.println("Going to sleep ...");
  display.hibernate();
  delay(200); //give the little thing time to process what he's done :)
  ESP.deepSleep(120e6); //sleeps for 120*10^6 microseconds ≙ 120 seconds
}

void loop() {
  Serial.printf("loop() should never be reached!\n");
  delay(1000);
}