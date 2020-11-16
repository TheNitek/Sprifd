#ifdef ESP32
  #include <WiFi.h>
  #include <AsyncTCP.h>
  #include <ESPmDNS.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESPAsyncTCP.h>
  #include <ESP8266mDNS.h>
#endif
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <MFRC522.h>
#include <NfcAdapter.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoSpotify.h>
#include <ArduinoJson.h>

#include "Config.h"


char scope[] = "user-read-playback-state%20user-modify-playback-state";
char callbackURItemplate[] = "%s%s%s";
char callbackURIProtocol[] = "http%3A%2F%2F"; // "http://"
char callbackURIAddress[] = "%2Fcallback%2F"; // "/callback/"
char callbackURI[100];


// including a "spotify_server_cert" variable
// header is included as part of the ArduinoSpotify libary
#include <ArduinoSpotifyCert.h>

AsyncWebServer server(80);
DNSServer dns;
WiFiClientSecure client;
ArduinoSpotify spotify(client, clientId, clientSecret, refreshToken);

bool doUpdate = false;

CurrentlyPlaying playing;
#define MAX_DEVICES 10
SpotifyDevice devices[MAX_DEVICES];
uint8_t numDevices = 0;

#define SS_PIN 33

MFRC522 mfrc522(SS_PIN, UINT8_MAX); // Create MFRC522 instance
NfcAdapter nfc(&mfrc522);

struct UID {
  byte uidByte[10];
  uint8_t uidSize;
};

UID lastUid;

char playbackDeviceId[41] = {0};

void handleRoot(AsyncWebServerRequest *request)
{
  if(request->hasParam("device", true)) {
    AsyncWebParameter *deviceParam = request->getParam("device", true);
    String device = deviceParam->value();
    strncpy(playbackDeviceId, device.c_str(), 40);
    Serial.printf("New playback device: %s\n", playbackDeviceId);
  }

  AsyncResponseStream *response = request->beginResponseStream("text/html");

  response->print(R"(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta http-equiv="X-UA-Compatible" content="IE=edge">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
  </head>
  <body>
  )");


  const char *currentlyPlaying = "<p>Currently Playing: %s - %s</p>";
  response->printf(currentlyPlaying, playing.trackName, playing.firstArtistName);

  response->print("<form action=\"/\" method=\"post\"><label for=\"device\">Playing on: </label><select name=\"device\" id=\"device\"><option value=\"\"></option>");

  const char *deviceTpl = "<option value=\"%s\"%s>%s</option>";
  for(uint8_t i = 0; i < numDevices; i++) {
    response->printf(deviceTpl, devices[i].id, ((strcmp(playbackDeviceId, devices[i].id) == 0) ? " selected" : ""), devices[i].name);;
  }
  response->print("</select> <input type=\"submit\" value=\"set target\"></form>");

  const char *authLinkTpl = "<p><a href=\"https://accounts.spotify.com/authorize?client_id=%s&response_type=code&redirect_uri=%s&scope=%s\">spotify Auth</a></p>";
  response->printf(authLinkTpl, clientId, callbackURI, scope);

  response->print(R"(
  </body>
</html>
)")
;
  request->send(response);
  doUpdate=true;
}

void handleCallback(AsyncWebServerRequest *request)
{
  String code = "";
  const char *refreshToken = NULL;
  for (uint8_t i = 0; i < request->args(); i++)
  {
    if (request->argName(i) == "code")
    {
      code = request->arg(i);
      refreshToken = spotify.requestAccessTokens(code.c_str(), callbackURI);
    }
  }

  if (refreshToken != NULL)
  {
    request->send(200, "text/plain", refreshToken);
  }
  else
  {
    request->send(404, "text/plain", "Failed to load token, check serial monitor");
  }
}

void handleNotFound(AsyncWebServerRequest *request)
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += request->url();
  message += "\nMethod: ";
  message += (request->method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += request->args();
  message += "\n";

  for (uint8_t i = 0; i < request->args(); i++)
  {
    message += " " + request->argName(i) + ": " + request->arg(i) + "\n";
  }

  Serial.print(message);
  request->send(404, "text/plain", message);
}

void startWifi() {
  Serial.println("Connecting Wifi");

  AsyncWiFiManager wifiManager(&server,&dns);

  wifiManager.setConfigPortalTimeout(30);
  uint8_t i = 0;
  while(!wifiManager.autoConnect("sprfid", "rfidrfid", 5) && i++ < 3) {
    Serial.println("Retry autoConnect");
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  }
  if(!WiFi.isConnected()) {
    wifiManager.autoConnect("sprfid", "rfidrfid");
  }

  Serial.print("WiFi connected with IP: "); Serial.println(WiFi.localIP());
  
  if (MDNS.begin("sprfid"))
  {
    Serial.println("MDNS responder started");
  }
}


void handleRfid() {
  if(!nfc.tagPresent()) {
    return;
  }

  NfcTag tag = nfc.read();
  if(!tag.hasNdefMessage()) {
    Serial.println("Ignoring non Ndef tag");
    nfc.haltTag();
    return;
  }

  /*UID tagUid;
  tag.getUid(tagUid.uidByte, tagUid.uidSize);
  if((lastUid.uidSize == tagUid.uidSize) && memcmp(tagUid.uidByte, lastUid.uidByte, lastUid.uidSize)) {
    Serial.print(".");
    nfc.haltTag();
    return;
  }*/

  size_t count = tag.getNdefMessage().getRecordCount();
  Serial.printf("Found %d records\n", count);
  for(uint8_t i=0; i < count; i++) {
    NdefRecord record = tag.getNdefMessage().getRecord(i);
    if(!((record.getTnf() == NdefRecord::TNF_WELL_KNOWN) && (record.getTypeLength() == 1) && (record.getType()[0] == NdefRecord::RTD_URI))) {
      Serial.println("Ignoring non-URI record");
      continue;
    }

    unsigned int payloadLength = record.getPayloadLength();
    const byte *payload = record.getPayload();
    if(payloadLength < 10) {
      Serial.println("Too short for spotify URI - ignoring");
      continue;
    }

    // Maxlength is 40, but first byte is RTD and will be cut off)
    if(payloadLength > 41) {
      Serial.println("Too long for spotify URI - ignoring");
      continue;
    }

    // One more for \0
    char uri[41] = {0};
    memcpy(uri, payload+1, payloadLength-1);

    if(strncmp(uri, "spotify", 7) != 0) {
      Serial.println("URI not does not start with \"spotify\"");
      continue;
    }

    char body[100];
    sprintf(body, "{\"context_uri\" : \"%s\"}", uri);
    yield();
    if (spotify.playAdvanced(body, playbackDeviceId)) {
        tag.getUid(lastUid.uidByte, lastUid.uidSize);
        nfc.haltTag();
        Serial.printf("Started playback for %s\n", uri);
        return;
    }
  }

  // No spotify tags
  nfc.haltTag();
}

void setup()
{

  Serial.begin(115200);

  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522

  startWifi();

  // If you want to enable some extra debugging
  // uncomment the "#define SPOTIFY_DEBUG" in ArduinoSpotify.h
#ifdef ESP32
  client.setCACert(spotify_server_cert);
#else
  client.setFingerprint(SPOTIFY_FINGERPRINT);
#endif

  // Building up callback URL using IP address.
  sprintf(callbackURI, callbackURItemplate, callbackURIProtocol, "sprfid", callbackURIAddress);

  server.on("/", handleRoot);
  server.on("/callback/", handleCallback);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");

  playing = spotify.getCurrentlyPlaying("DE");

  numDevices = spotify.getDevices(devices, sizeof(devices)/sizeof(devices[0]));

  Serial.println("Setup done");
}
void loop()
{
  handleRfid();

  if(doUpdate) {
    playing = spotify.getCurrentlyPlaying("DE");

    numDevices = spotify.getDevices(devices, sizeof(devices)/sizeof(devices[0]));
  }
}