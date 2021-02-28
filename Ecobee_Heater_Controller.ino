#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <WiFiUdp.h>
#include <NTPClient.h> //https://github.com/taranais/NTPClient
#include "Timer.h" //https://playground.arduino.cc/Code/Timer/

#define RELAY 16

ESP8266WebServer server(80);

const char* ssid     = "yourssid"; //Make sure you change this!
const char* password = "yourpassword"; //Make sure you change this!
const char* host = "api.ecobee.com";
const char* url_temperature = "/1/thermostat?json={\"selection\":{\"selectionType\":\"registered\",\"selectionMatch\":\"\",\"includeSensors\":\"true\"}}";

WiFiClientSecure client;
Timer t;


String appId = "yourecobeeappid"; //Make sure you change this! 

String indexHTML = "/index.html";
String logHTML = "/log.html";
String accessTokenFile = "/accessToken.txt";
String refreshTokenFile = "/refreshToken.txt";
String temperatureFile = "/temp.txt";
String timeFile = "/time.txt";
String fingerprintFile = "/fingerprint.txt";
String sensorNameFile = "/sensor.txt";
String logFile = "/log.txt";
int httpsPort = 443;
int errorCount = 0;

int UTC = -5; //EST
const long utcOffsetInSeconds = UTC * 60 * 60; //-18000 (UTC *60 *60)

bool state = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

void setup()
{
  Serial.begin(115200);
  delay(100);

  pinMode(RELAY, OUTPUT);
  SPIFFS.begin();
  ConnectWifi();
  timeClient.begin();
  delay(100);
  timeClient.update();
  SetupServer();
  t.every(300000, ContactEcobeeAPI, NULL);
  t.every(1000, CheckTime, NULL);
  ContactEcobeeAPI(NULL);
}

void loop()
{
  // Check WiFi Status
  timeClient.update();
  t.update();
  server.handleClient();
  digitalWrite(RELAY, state);
}

void SetupServer()
{
  server.on("/", handle_OnConnect);
  server.on("/temp", HTTP_POST, handle_temperature);
  server.on("/time", HTTP_POST, handle_time);
  server.on("/fingerprint", HTTP_POST, handle_fingerprint);
  server.on("/check", HTTP_POST, handle_check);
  server.on("/refreshToken", HTTP_POST, handle_refreshToken);
  server.on("/accessToken", HTTP_POST, handle_accessToken);
  server.on("/sensor", HTTP_POST, handle_sensor);
  server.on("/clearLog", HTTP_POST, handle_clearLog);
  server.on("/log", handle_log);
  server.onNotFound(handle_NotFound);
  server.begin();
}

void ConnectWifi()
{
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Netmask: ");
  Serial.println(WiFi.subnetMask());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
}

void SetState(int val, String message = "")
{
  if (state == val)
    return;
  state = val;
  PrintLog(message + String("Changing State to ") + GetStateString());
}

String GetStateString()
{
  if (state)
    return "ON";
  else
    return "OFF";
}

void CheckTime(void* context)
{
  if (!WithinTime())
    SetState(0, "Outside Timer Range. ");
}

String SendRequest(String url, int requestType, String headers = "")
{
  Serial.print("Connecting to ");
  Serial.println(host);
  String fingerprint = ReadFile(fingerprintFile);
  client.setFingerprint(fingerprint.c_str());

  if (!client.connect(host, httpsPort)) {
    PrintLog(String("ERROR: Connection to ") + host + " failed. May be an invalid fingerprint.");
    return "";
  }

  Serial.print("Requesting URL: ");
  Serial.println(String("https://") + host + url);
  String typeString = "";
  if (requestType == 0)
    typeString = "GET ";
  else
    typeString = "POST ";
  client.print(typeString + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               headers + "\r\n" +
               "Connection: close\r\n\r\n");

  Serial.println("Request sent");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r")
    {
      break;
    }
  }
  String json = client.readString();
  int startIndex = json.indexOf('{');
  int endIndex = json.lastIndexOf('}') + 1;
  return json.substring(startIndex, endIndex);
}

void ContactEcobeeAPI(void* context)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    CheckTemperature(NULL);
  }
}

void CheckTemperature(void* context)
{

  if (!WithinTime())
  {
    return;
  }
  String accessToken = ReadFile(accessTokenFile);
  String json = SendRequest(url_temperature, 0, String("Authorization: Bearer ") + accessToken);
  if (json.equals(""))
  {
    PrintLog(String("ERROR: Received Empty JSON from ") + host + url_temperature);
    SetState(0);
  }
  DynamicJsonDocument doc(2048);
  deserializeJson(doc, json);

  int status_code = doc["status"]["code"]; // 0
  const char* status_message = doc["status"]["message"]; // ""
  if (status_code != 0)
  {
    String append = "";
    bool authorizationError = status_code == 1 || status_code == 2 || status_code == 14 || status_code == 16;
    if (authorizationError && errorCount < 3)
    {
      append = "Refreshing token and making another attempt. ";
    }
    PrintLog(String("ERROR: \"(") + status_code + ") - " + status_message + "\" " + append);
    if (authorizationError)
      RefreshTokens();
    if (errorCount < 3)
    {
      errorCount++;
      t.after(100, CheckTemperature, NULL);
    }
    else
    {
      errorCount = 0;
      SetState(0);
    }
  }
  else
  {
    Serial.println("SUCCESS");
    JsonObject thermostatList_0 = doc["thermostatList"][0];

    JsonArray thermostatList_0_remoteSensors = thermostatList_0["remoteSensors"];
    bool foundSensor = false;
    String sensorToCheck = ReadFile(sensorNameFile);
    sensorToCheck.trim();
    for (int i = 0; i < thermostatList_0_remoteSensors.size(); i++)
    {
      JsonObject remoteSensor = thermostatList_0_remoteSensors[i];
      const char* sensorName = remoteSensor["name"];
      String sensorNameString = String(sensorName);
      if (sensorNameString.equals(sensorToCheck))
      {
        Serial.println("Found Nursery sensor!");
        foundSensor = true;
        JsonArray capabilities = remoteSensor["capability"].as<JsonArray>();
        for (int j = 0; j < capabilities.size(); j++)
        {
          JsonObject capability = capabilities[j];
          const char* type = capability["type"];
          String typeString = String(type);
          String typeCompare = String("temperature");
          if (typeString.equals(typeCompare))
          {
            float value = ((float)capability["value"]) / 10.0f ;
            if (state == 0)
            {
              if (BelowTemperature(value))
                SetState(1, String("Temperature is ") + value + ". ");
            }
            else
            {
              if (AboveTemperature(value))
                SetState(0, String("Temperature is ") + value + ". ");
            }
          }
        }
      }
    }
    if (!foundSensor)
    {
      PrintLog(String("ERROR: JSON did not contain a sensor named \"") + sensorToCheck + ".\"");
      SetState(0);
    }
  }

  Serial.println("Closing connection");
  client.stop();

}

void RefreshTokens()
{

  String json = SendRequest(GetTokenURL(), 1);
  DynamicJsonDocument doc(1536);
  deserializeJson(doc, json);

  const char* access_token = doc["access_token"];
  const char* refresh_token = doc["refresh_token"];
  OverwriteFile(accessTokenFile, access_token);
  OverwriteFile(refreshTokenFile, refresh_token);

  Serial.println("Closing connection");
  client.stop();
}

String GetTokenURL()
{
  String refreshToken = ReadFile(refreshTokenFile);
  Serial.println(refreshToken);
  return String("/token?grant_type=refresh_token&code=" + refreshToken + "&client_id=" + appId);
}

bool WithinTemperature(float temp)
{
  File f = SPIFFS.open(temperatureFile, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + temperatureFile);
    return false;
  }
  String tempMinString = f.readStringUntil('\n');
  String tempMaxString = f.readString();
  f.close();
  float tempMin = tempMinString.toFloat();
  float tempMax = tempMaxString.toFloat();
  if (temp > tempMin && temp < tempMax)
    return true;
  else
    return false;
}

bool AboveTemperature(float temp)
{
  File f = SPIFFS.open(temperatureFile, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + temperatureFile);
    return false;
  }
  f.readStringUntil('\n');
  String tempMaxString = f.readString();
  f.close();
  float tempMax = tempMaxString.toFloat();
  if (temp > tempMax)
    return true;
  else
    return false;
}

bool BelowTemperature(float temp)
{
  File f = SPIFFS.open(temperatureFile, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + temperatureFile);
    return false;
  }
  String tempMinString = f.readStringUntil('\n');
  f.close();
  float tempMin = tempMinString.toFloat();
  if (temp < tempMin)
    return true;
  else
    return false;
}

bool WithinTime()
{
  int hour = timeClient.getHours();
  int minute = timeClient.getMinutes();
  float currentTime = (float)hour + ((float)minute / 60.0f);
  File f = SPIFFS.open(timeFile, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + timeFile);
    return false;
  }
  String timeOnString = f.readStringUntil('\n');
  String timeOffString = f.readString();
  f.close();
  int colonInd = timeOnString.indexOf(':');
  float timeOnHours = timeOnString.substring(0, colonInd).toFloat();
  float timeOnMinutes = timeOnString.substring(colonInd + 1).toFloat();
  colonInd = timeOffString.indexOf(':');
  float timeOffHours = timeOffString.substring(0, colonInd).toFloat();
  float timeOffMinutes = timeOffString.substring(colonInd + 1).toFloat();
  float timeOn = timeOnHours + (timeOnMinutes / 60.0f);
  float timeOff = timeOffHours + (timeOffMinutes / 60.0f);
  if (timeOff < timeOn)
  {
    if (currentTime > timeOff && currentTime < timeOn)
      return false;
    else
      return true;
  }
  else
  {
    if (currentTime > timeOn && currentTime < timeOff)
      return true;
    else
      return false;
  }
}

String ReadFile(String filename)
{
  File f = SPIFFS.open(filename, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + filename);
    f.close();
    return "FAILURE";
  }
  String contents = f.readString();
  f.close();
  return contents;
}


void OverwriteFile(String filename, String contents)
{
  File f = SPIFFS.open(filename, "w");
  if (!f)
  {
    PrintLog(String("Failed to Overwrite File: ") + filename);
    f.close();
    return;
  }
  f.print(contents);
  f.close();
}

void AppendFile(String filename, String contents)
{
  File f = SPIFFS.open(filename, "a");
  if (!f)
  {
    PrintLog(String("Failed to Append File: ") + filename);
    f.close();
    return;
  }
  f.print(contents);
  f.close();
}

void handle_NotFound()
{
  server.send(404, "text/plain", "Not found");
}

void handle_OnConnect()
{
  Serial.println("CONNECTED!");
  String html = ReadFile(indexHTML);
  html.replace("%STATE", GetStateString());
  File f = SPIFFS.open(temperatureFile, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + temperatureFile);
  }
  String tempMinString = f.readStringUntil('\n');
  String tempMaxString = f.readString();
  tempMinString.trim();
  tempMaxString.trim();
  f.close();
  html.replace("%TEMPMIN", tempMinString);
  html.replace("%TEMPMAX", tempMaxString);
  f = SPIFFS.open(timeFile, "r");
  if (!f)
  {
    PrintLog(String("Failed to Read File: ") + timeFile);
  }
  String timeOnString = f.readStringUntil('\n');
  String timeOffString = f.readString();
  timeOnString.trim();
  timeOffString.trim();
  f.close();
  html.replace("%TIMEON", timeOnString);
  html.replace("%TIMEOFF", timeOffString);
  String sensor = ReadFile(sensorNameFile);
  sensor.trim();
  html.replace("%SENSOR", sensor);
  String fingerprint = ReadFile(fingerprintFile);
  fingerprint.trim();
  html.replace("%FINGERPRINT", fingerprint);
  server.send(200, "text/html", html);
}

void handle_log()
{
  Serial.println("CONNECTED TO LOG!");
  String html = ReadFile(logHTML);
  String log = ReadFile(logFile);
  html.replace("%CONTENT", log);
  server.send(200, "text/html", html);
}

void handle_fingerprint()
{
  if (server.hasArg("fingerprint"))
  {
    String fingerprint = server.arg("fingerprint");
    PrintLog(String("Updating Fingerprint to: ") + fingerprint);
    OverwriteFile(fingerprintFile, fingerprint);
  }
  RefreshPage();
}

void handle_sensor()
{
  if (server.hasArg("sensor"))
  {
    String sensor = server.arg("sensor");
    PrintLog(String("Updating Sensor Name to: ") + sensor);
    OverwriteFile(sensorNameFile, sensor);
  }
  RefreshPage();
}

void handle_refreshToken()
{
  if (server.hasArg("refreshToken"))
  {
    String refreshToken = server.arg("refreshToken");
    PrintLog(String("Updating Refresh Token to: ") + refreshToken);
    OverwriteFile(refreshTokenFile, refreshToken);
  }
  RefreshPage();
}

void handle_accessToken()
{
  if (server.hasArg("accessToken"))
  {
    String accessToken = server.arg("accessToken");
    PrintLog(String("Updating Access Token to: ") + accessToken);
    OverwriteFile(accessTokenFile, accessToken);
  }
  RefreshPage();
}

void handle_temperature()
{
  if (server.hasArg("tempMin") && server.hasArg("tempMax"))
  {
    String tempMin = server.arg("tempMin");
    String tempMax = server.arg("tempMax");
    PrintLog(String("Updating Temperature Range to: (") + tempMin + ", " + tempMax + ").");
    OverwriteFile(temperatureFile, tempMin + "\n");
    AppendFile(temperatureFile, tempMax);
  }
  else
    PrintLog(String("Temperature Request did not contain required parameters"));

  RefreshPage();

}

void handle_time()
{
  if (server.hasArg("timeOff") && server.hasArg("timeOn"))
  {
    String timeOff = server.arg("timeOff");
    String timeOn = server.arg("timeOn");
    PrintLog(String("Updating Time Range to: (") + timeOn + ", " + timeOff + ").");
    OverwriteFile(timeFile, timeOn + "\n");
    AppendFile(timeFile, timeOff);
  }
  else
    PrintLog(String("Temperature Request did not contain required parameters"));
  RefreshPage();

}

void handle_check()
{
  RefreshPage();
  CheckTemperature(NULL);
}

void handle_clearLog()
{
  ClearLog();
  RefreshPage();
}

void RefreshPage()
{
  server.sendHeader("Location", "/");  // Add a header to respond with a new location for the browser to go to the home page again
  server.send(303);
}

void PrintLog(String message)
{
  //Count current number of logged lines. If greater than 100, erase the first one. Then append a new one.
  String dateTime = timeClient.getFormattedDate();
  dateTime.replace("T", " ");
  dateTime.replace("Z", "");
  message = String("(") + dateTime + ") " + message;
  Serial.println(message);
  File f = SPIFFS.open(logFile, "r");
  int lineCount = 0;
  while (f.available())
  {
    lineCount++;
    f.readStringUntil('\n');
  }
  f.close();
  if (lineCount > 100)
  {
    f = SPIFFS.open(logFile, "r");
    f.readStringUntil('\n');
    String s = f.readString();
    f.close();
    OverwriteFile(logFile, s);
  }
  f = SPIFFS.open(logFile, "a");
  f.println(message);
  f.close();
}

void ClearLog()
{
  OverwriteFile(logFile, "");
  PrintLog("Log Cleared Manually");
}
