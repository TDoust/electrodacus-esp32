//travis version handling
#ifndef VERSION
    #define VERSION "undefined version"
#endif

#define STRINGIFY(x) #x
#define TOSTR(x) STRINGIFY(x)
#define VERSION_STR TOSTR(VERSION)


#include <Arduino.h>

#include <WiFi.h>

//JSON includes
#include <ArduinoJson.h>

//web server includes
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

//MQTT includes
#include <WiFiClient.h>
#include <PubSubClient.h>

//file system
#include <SPIFFS.h>

//local libraries
#include "jsvarStore.hpp"
#include "sbmsData.hpp"

// Set LED_BUILTIN if it is not defined by Arduino framework
// #define LED_BUILTIN 2

//instances
AsyncWebServer server(80);

WiFiClient mqttWifiClient;
PubSubClient mqtt(mqttWifiClient);

JsvarStore varStore(Serial);

//------------------------- GLOBALS ---------------------

//WIFI
bool ap_fallback = false;
unsigned long lastWiFiTime = 0;
bool wifiSettingsChanged = false;

//MQTT
unsigned long mqLastConnectionAttempt = 0;

//------------------------- SETTINGS --------------------

bool s_sta_enabled = false;
String s_hostname = "SBMS";
String s_ssid = "SBMS";
String s_password = "";

void readWifiSettings()
{
  auto sWifi = SPIFFS.open("/sWifi.txt"); //default mode is read

  s_sta_enabled = sWifi.readStringUntil('\r') == "1"; if(sWifi.peek() == '\n') sWifi.read();
  s_hostname = sWifi.readStringUntil('\r'); if(sWifi.peek() == '\n') sWifi.read();
  s_ssid = sWifi.readStringUntil('\r'); if(sWifi.peek() == '\n') sWifi.read();
  s_password = sWifi.readStringUntil('\r');
  sWifi.close();
}

void writeWifiSettings(const char *hostname, const char *ssid, const char *pw)
{

  auto sWifi = SPIFFS.open("/sWifi.txt", "w");

  sWifi.printf("1\r\n");
  sWifi.printf("%s\r\n", hostname);
  sWifi.printf("%s\r\n", ssid);
  sWifi.printf("%s\r\n", pw);

  sWifi.flush();
  sWifi.close();
  
}

bool s_mq_enabled = false;
String s_mq_host;
uint32_t s_mq_port = 1883;
String s_mq_prefix = "/";
String s_mq_user;
String s_mq_password;

void readMqttSettings()
{
  auto sMqtt = SPIFFS.open("/sMqtt.txt"); //default mode is read

  s_mq_enabled = sMqtt.readStringUntil('\r') == "1"; if(sMqtt.peek() == '\n') sMqtt.read();
  s_mq_host = sMqtt.readStringUntil('\r'); if(sMqtt.peek() == '\n') sMqtt.read();
  s_mq_port = sMqtt.readStringUntil('\r').toInt(); if(sMqtt.peek() == '\n') sMqtt.read();
  s_mq_prefix = sMqtt.readStringUntil('\r'); if(sMqtt.peek() == '\n') sMqtt.read();
  s_mq_user = sMqtt.readStringUntil('\r'); if(sMqtt.peek() == '\n') sMqtt.read();
  s_mq_password = sMqtt.readStringUntil('\r'); if(sMqtt.peek() == '\n') sMqtt.read();

  sMqtt.close();
}


//------------------------- TEMPLATES --------------------

String templateVersion(const String& var)
{
  if(var == "VERSION")
    return F(VERSION_STR);
  return String();
}

String templateSettings(const String& var)
{
  if(var == "wifi_hostname")
    return s_hostname;
  if(var == "wifi_ssid")
    return s_ssid;
  if(var == "wifi_pw")
    return s_password;
  return String();
}


//------------------------- MQTT --------------------

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  //we won't receive anything for now
}

void mqttSetup()
{

  //mqttWifiClient.setCACert(ca_cert);
  mqtt.setServer(s_mq_host.c_str(), s_mq_port);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60); //default is 15 seconds
  //mqtt.setBufferSize(265); //override max package size (default 256)
  
}

bool mqttConnect()
{
  if(s_mq_user.isEmpty())
  {
    return mqtt.connect(s_hostname.c_str());
  }
  return mqtt.connect(s_hostname.c_str(), s_mq_user.c_str(), s_mq_password.c_str());
}

void mqttUpdate()
{
  if(!s_mq_enabled)
  {
    if(mqtt.connected())
    {
      mqtt.disconnect();
    }
  }
  else
  {
    if(!mqtt.connected() && millis() - mqLastConnectionAttempt > 1000)
    {
      printf("Connecting to %s : %d as %s", s_mq_host.c_str(), s_mq_port, s_hostname.c_str());
      mqttSetup();
      if (mqttConnect())
      {
        //client.subscribe(TOPIC);
      }
      else
      {
        mqLastConnectionAttempt = millis();
      }
    }
    mqtt.loop();
  }
}

struct MqttJsonWriter {
  // Writes one byte, returns the number of bytes written (0 or 1)
  size_t write(uint8_t c)
  { 
    buf[bufi++] = c;
    if(bufi == MQTT_MAX_PACKET_SIZE) flush();
    return 1;
  }
  // Writes several bytes, returns the number of bytes written
  size_t write(const uint8_t *buffer, size_t length)
  { 
    size_t i = 0;
    while(i < length && bufi < MQTT_MAX_PACKET_SIZE)
    {
      buf[bufi++] = buffer[i++];
    }

    if(bufi == MQTT_MAX_PACKET_SIZE) flush();
    
    return i; 
  }

  uint8_t buf[MQTT_MAX_PACKET_SIZE];
  size_t bufi;

  MqttJsonWriter()
  {
    bufi = 0;
  }

  void flush()
  {
    size_t written = 0;
    while(written < bufi)
    {

      size_t thisWrite = mqtt.write(buf + written, bufi - written);
      if(thisWrite == 0) return; //error, couldn't even write a single byte. Prevent infinite loop.
      written += thisWrite;
      printf("flushed %d of %d bytes\n", written, bufi);
    }

    bufi = 0;
  }
};

void mqttPublishJson(const JsonDocument *doc, const String topic)
{

  mqtt.beginPublish((s_mq_prefix + topic).c_str(), measureJson(*doc), false);

  MqttJsonWriter writer;
  serializeJson(*doc, writer);

  writer.flush();

  mqtt.endPublish();
}

void mqttPublishSBMS(const SbmsData &sbms)
{
  //size calculated by https://arduinojson.org/v6/assistant/
  StaticJsonDocument<JSON_ARRAY_SIZE(8) + JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(6) + JSON_OBJECT_SIZE(12) + JSON_OBJECT_SIZE(15)> doc;

  doc["time"]["year"] = sbms.year;
  doc["time"]["month"] = sbms.month;
  doc["time"]["day"] = sbms.day;
  doc["time"]["hour"] = sbms.hour;
  doc["time"]["minute"] = sbms.minute;
  doc["time"]["second"] = sbms.second;
  
  doc["soc"] = sbms.stateOfChargePercent;

  JsonArray volt = doc.createNestedArray("cellsMV");
  for(uint8_t i=0; i<8; i++)
  {
    volt.add(sbms.cellVoltageMV[i]);
  }

  doc["tempInt"] = sbms.temperatureInternalTenthC / 10.0;
  doc["tempExt"] = sbms.temperatureExternalTenthC / 10.0;

  JsonObject curr = doc.createNestedObject("currentMA");

  curr["battery"] = sbms.batteryCurrentMA;
  curr["pv1"] = sbms.pv1CurrentMA;
  curr["pv2"] = sbms.pv2CurrentMA;
  curr["extLoad"] = sbms.extLoadCurrentMA;

  doc["ad2"] = sbms.ad2;
  doc["ad3"] = sbms.ad3;
  doc["ad4"] = sbms.ad4;

  doc["heat1"] = sbms.heat1;
  doc["heat2"] = sbms.heat2;

  JsonObject flags = doc.createNestedObject("flags");

  flags["OV"] = sbms.getFlag(SbmsData::FlagBit::OV);
  flags["OVLK"] = sbms.getFlag(SbmsData::FlagBit::OVLK);
  flags["UV"] = sbms.getFlag(SbmsData::FlagBit::UV);
  flags["UVLK"] = sbms.getFlag(SbmsData::FlagBit::UVLK);
  flags["IOT"] = sbms.getFlag(SbmsData::FlagBit::IOT);
  flags["COC"] = sbms.getFlag(SbmsData::FlagBit::COC);
  flags["DOC"] = sbms.getFlag(SbmsData::FlagBit::DOC);
  flags["DSC"] = sbms.getFlag(SbmsData::FlagBit::DSC);
  flags["CELF"] = sbms.getFlag(SbmsData::FlagBit::CELF);
  flags["OPEN"] = sbms.getFlag(SbmsData::FlagBit::OPEN);
  flags["LVC"] = sbms.getFlag(SbmsData::FlagBit::LVC);
  flags["ECCF"] = sbms.getFlag(SbmsData::FlagBit::ECCF);
  flags["CFET"] = sbms.getFlag(SbmsData::FlagBit::CFET);
  flags["EOC"] = sbms.getFlag(SbmsData::FlagBit::EOC);
  flags["DFET"] = sbms.getFlag(SbmsData::FlagBit::DFET);

  mqttPublishJson( &doc, "sbms");

}


//------------------------- WIFI --------------------

void updateWifiState()
{

  if(ap_fallback || !s_sta_enabled)
  {
    uint64_t uid = ESP.getEfuseMac();
    char ssid[22];
    sprintf(ssid, "SBMS-%04X%08X", (uint32_t)((uid>>32)%0xFFFF), (uint32_t)uid);


    WiFi.mode(WIFI_MODE_APSTA);

    
    

    WiFi.softAP(ssid, "electrodacus");
    delay(100);
    WiFi.softAPConfig(IPAddress (192, 168, 4, 1), IPAddress (192, 168, 4, 1), IPAddress (255,255,255,0));
    WiFi.softAPsetHostname("SBMS");
  }
  else if(s_sta_enabled) {
    WiFi.mode(WIFI_MODE_STA);

    WiFi.setHostname(s_hostname.c_str());

    WiFi.begin(s_ssid.c_str(), s_password.c_str());
    WiFi.setAutoConnect(true);
    WiFi.setAutoReconnect(true);
  }


}







void setup()
{
  
  //setup peripherals
  Serial.begin(921600);

  pinMode(LED_BUILTIN, OUTPUT);

  //load settings
  SPIFFS.begin();

  readWifiSettings();

  readMqttSettings();
  


  //setup libraries
  updateWifiState();
  
  //mqttSetup(); //will be set up automatically when enabled
  
  


  //setup webserver

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect("/index.html");
    });

  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/web/index.html", String(), false, templateVersion);
    });

  server.on("/sbms.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/web/sbms.html");
    });

  server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/web/settings.html", String(), false, templateSettings);
    });

  server.on("/sWifi", HTTP_POST, [](AsyncWebServerRequest *request){

    //Check if POST (but not File) parameter exists
    if(request->hasParam("ssid", true) && request->hasParam("pw", true) && request->hasParam("hostname", true))
    {
      AsyncWebParameter* p_name = request->getParam("hostname", true);
      AsyncWebParameter* p_ssid = request->getParam("ssid", true);
      AsyncWebParameter* p_pw = request->getParam("pw", true);

      
      const String name = p_name->value();
      const String ssid = p_ssid->value();
      const String pw = p_pw->value();

      writeWifiSettings(name.c_str(), ssid.c_str(), pw.c_str());
      readWifiSettings();

      if(name == s_hostname && ssid == s_ssid && pw == s_password) { //success
        request->send(SPIFFS, "/web/set_response.html", String(), false, [](const String &var){
          if(var == "message") return String(F("WiFi Settings stored successfully. Connect to your WiFi to access this device again."));
          return String();
        });
        wifiSettingsChanged = true;
      }
      else {
        request->send(SPIFFS, "/web/set_response.html", String(), false, [](const String &var){
          if(var == "message") return String(F("Error storing parameters. Read values do not match what should have been written."));
          return String();
        });
        wifiSettingsChanged = false;
      }
      
      
    }
    else
    {
      request->send(200, "text/plain", "Error. Missing Parameters.");
    }
    
    
  });

    


  server.on("/rawData", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/javascript", varStore.dumpVars());
    });
  
  server.on("/dummyData", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/testdata");
    });


  server.serveStatic("/static/", SPIFFS, "/web/static/").setCacheControl("max-age=600"); // Cache static responses for 10 minutes (600 seconds)

  server.onNotFound([](AsyncWebServerRequest *request){
        request->send(404, "text/plain", "Not found");
    });

  server.begin();
  
}

void updateLed()
{
  if(s_sta_enabled && WiFi.status() == WL_CONNECTED)
  {
    digitalWrite(BUILTIN_LED, HIGH);
  }
  else if(s_sta_enabled) {
    if(ap_fallback){
      digitalWrite(BUILTIN_LED, millis()%500 < 100);
    }
    else {
      digitalWrite(BUILTIN_LED, millis()%1500 < 100);
    }
  }
  else {
    digitalWrite(BUILTIN_LED, millis()%1500 < 750);
  }

}

bool handleWiFi()
{
  auto t = millis();

  if(wifiSettingsChanged)
  {
    wifiSettingsChanged = false;
    lastWiFiTime = t + 5000; //give it a little extra time
    ap_fallback = false;
    updateWifiState();
  }

  if(s_sta_enabled && WiFi.status() == WL_CONNECTED)
  {
    lastWiFiTime = t;
    if(ap_fallback) {
      ap_fallback = false;
      updateWifiState();
    }
    
  }
  else if(s_sta_enabled && !ap_fallback && WiFi.status() != WL_CONNECTED && t-lastWiFiTime > 10000)
  {
    ap_fallback = true;
    updateWifiState();
  }

  return WiFi.status() == WL_CONNECTED;
}

void loop()
{
  bool connected = handleWiFi();

  updateLed();

  if(connected)
  {
    mqttUpdate();
  }


  String parsed = varStore.update();
  if(parsed.length() > 0)
  {
    if(parsed == "sbms") //this guarantees the variable is stored in the varStore so we can get it
    {
      auto sbms = SbmsData(varStore.getVar("sbms"));

      mqttPublishSBMS(sbms);
    }
  }
  

}
