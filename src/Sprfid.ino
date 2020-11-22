#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <AsyncElegantOTA.h>
#include <MFRC522.h>
#include <NfcAdapter.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoSpotify.h>
#include <ArduinoJson.h>

#include "Config.h"


const char scope[] = "user-read-playback-state%20user-modify-playback-state";
const char callbackURItemplate[] = "%s%s%s";
const char callbackURIProtocol[] = "http%3A%2F%2F"; // "http://"
const char callbackURIAddress[] = "%2Fcallback"; // "/callback/"
char callbackURI[100];

const char PROPERTY_DEVICE[] = "device";


// including a "spotify_server_cert" variable
// header is included as part of the ArduinoSpotify libary
#include <ArduinoSpotifyCert.h>

AsyncWebServer server(80);
DNSServer dns;
WiFiClientSecure client;
ArduinoSpotify spotify(client, clientId, clientSecret, refreshToken);

Preferences preferences;

CurrentlyPlaying playing;
#define MAX_DEVICES 10
SpotifyDevice devices[MAX_DEVICES];
uint8_t numDevices = 0;

#define SS_PIN 33
#define RST_PIN 25

MFRC522 mfrc522(SS_PIN, RST_PIN); // Create MFRC522 instance
NfcAdapter nfc(&mfrc522);

const char NDEF_DOMAIN[] = "com.github:nitek/sprfid";

struct UID {
  byte uidByte[10];
  uint8_t uidSize=sizeof(uidByte);
};

UID lastUid;
int lastRfidCheck = 0;

struct PlaybackOptions {
  bool shuffle: 1;
  bool repeat: 1;
};

char playbackDeviceId[41] = {0};

struct {
  char uri[61] = "";
  PlaybackOptions options {
    .shuffle = false,
    .repeat = true
  };
} writeCache;

bool updateSpotify = false;
bool updateDevice = false;
bool updateRefreshToken = false;
bool playbackStarted = false;

void handleRoot(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("text/html");

  response->print(R"(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta http-equiv="X-UA-Compatible" content="IE=edge">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
    <style>
      body {
        font-family: -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,"Noto Sans",sans-serif,"Apple Color Emoji","Segoe UI Emoji","Segoe UI Symbol","Noto Color Emoji";
      }
    </style>
  </head>
  <body>
  )");

  if(strlen(writeCache.uri) > 0) {
    const char pairing[] = "<p><strong>Pairing mode active!</strong> \"%s\" is going to be written onto the next tag placed on the device</p>";
    response->printf(pairing, writeCache.uri);
  }
  

  const char currentlyPlaying[] = "<p>Currently Playing: %s - %s</p>";
  response->printf(currentlyPlaying, playing.trackName, playing.firstArtistName);

  response->print("<p><form action=\"/save\" method=\"post\"><label for=\"device\">Playing on: </label><select name=\"device\" id=\"device\"><option value=\"\"></option>");

  const char deviceTpl[] = "<option value=\"%s\"%s>%s</option>";
  for(uint8_t i = 0; i < numDevices; i++) {
    response->printf(deviceTpl, devices[i].id, ((strcmp(playbackDeviceId, devices[i].id) == 0) ? " selected" : ""), devices[i].name);
  }
  response->print("</select> <input type=\"submit\" value=\"set target\"></form></p>");

  response->printf(
    "<p><form action=\"/save\" method=\"post\">"
      "<label for=\"album\">Spotify URI:</label> <input name=\"uri\" id=\"uri\" type=\"text\" size=\"60\" value=\"%s\"><br>"
      "<label for=\"shuffle\">Shuffle:</label> <input name=\"shuffle\" id=\"shuffle\" type=\"checkbox\" value=\"1\" %s><br>"
      "<label for=\"repeat\">Repeat:</label> <input name=\"repeat\" id=\"repeat\" type=\"checkbox\" value=\"1\" %s><br>"
      "<input type=\"submit\" value=\"save to tag\">"
    "</form></p>", 
    (writeCache.uri[0] == '\0') ? playing.albumUri : writeCache.uri,
    (writeCache.options.shuffle) ? "checked" : "",
    (writeCache.options.repeat) ? "checked" : ""
  );

  const char authLinkTpl[] = "<p><a href=\"https://accounts.spotify.com/authorize?client_id=%s&response_type=code&redirect_uri=%s&scope=%s\">spotify Auth</a></p>";
  response->printf(authLinkTpl, clientId, callbackURI, scope);

  response->print("</body></html>");
  request->send(response);
  updateSpotify=true;
}

void handleSave(AsyncWebServerRequest *request)
{
  if(request->hasParam("device", true)) {
    AsyncWebParameter *deviceParam = request->getParam("device", true);
    String device = deviceParam->value();
    strncpy(playbackDeviceId, device.c_str(), 40);
    Serial.printf("New playback device: %s\n", playbackDeviceId);
    updateDevice = true;
  }
  if(request->hasParam("uri", true)) {
    AsyncWebParameter *uriParam = request->getParam("uri", true);
    String uri = uriParam->value();
    strncpy(writeCache.uri, uri.c_str(), 60);
    Serial.printf("New URI to store: %s\n", writeCache.uri);
    writeCache.options.shuffle = request->hasParam("shuffle", true);
    Serial.printf("New shuffle: %d\n", writeCache.options.shuffle);
    writeCache.options.repeat = request->hasParam("repeat", true);
    Serial.printf("New repeat: %d\n", writeCache.options.repeat);
  }
  request->redirect("/");
}

void handleCallback(AsyncWebServerRequest *request)
{
  const char *refreshToken = NULL;

  if(request->hasParam("code")) {
    AsyncWebParameter *codeParam = request->getParam("code");
    String code = codeParam->value();
    refreshToken = spotify.requestAccessTokens(code.c_str(), callbackURI);
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

void startWifi() {
  Serial.println("Connecting Wifi");

  AsyncWiFiManager wifiManager(&server,&dns);
  wifiManager.autoConnect("sprfid", "rfidrfid");

  Serial.print("WiFi connected with IP: "); Serial.println(WiFi.localIP());
  
  if (MDNS.begin("sprfid"))
  {
    Serial.println("MDNS responder started");
  }
}


void handleRfid() {
  if(millis() - lastRfidCheck < 500) {
    return;
  }
  lastRfidCheck = millis();

  if(playbackStarted) {
    for (uint8_t i = 0; i < 3; i++) {
      byte bufferATQA[2];
      byte bufferSize = sizeof(bufferATQA);

      MFRC522::StatusCode result = mfrc522.PICC_WakeupA(bufferATQA, &bufferSize);

      if (result == mfrc522.STATUS_OK &&
          mfrc522.PICC_ReadCardSerial() &&
          mfrc522.uid.size == lastUid.uidSize &&
          memcmp(mfrc522.uid.uidByte, lastUid.uidByte, lastUid.uidSize) == 0) {
        mfrc522.PICC_HaltA();
        return;
      }
    }
    Serial.println("Pausing playback");
    if(spotify.pause()) {
      playbackStarted = false;
    } else {
      playing = spotify.getCurrentlyPlaying("DE");
      if(!playing.error && !playing.isPlaying) {
        Serial.println("Pausing request failed, but no playback running");
        playbackStarted = false;
      } else {
        Serial.println("Pausing failed");
      }
    }
    return;
  }

  if(!nfc.tagPresent()) {
    return;
  }
  
  if(mfrc522.uid.size == lastUid.uidSize &&
      memcmp(mfrc522.uid.uidByte, lastUid.uidByte, lastUid.uidSize) == 0) {
    if(spotify.play()) {
      Serial.println("Resumed playback");
      playbackStarted = true;
    }
    return;
  }

  NfcTag tag = nfc.read();

  if(!tag.isFormatted()) {
    if(writeCache.uri[0] == '\0') {
      Serial.println("Ignoring unformatted tag");
      nfc.haltTag();
      return;
    }

    Serial.print("Not formatted. Wakeing tag up again");
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);

    if((mfrc522.PICC_WakeupA(bufferATQA, &bufferSize) == mfrc522.STATUS_OK) && mfrc522.PICC_ReadCardSerial()){
      Serial.println("Formatting tag");
      if(!nfc.format()) {
        Serial.println("Formatting failed");
        return;
      }
    }
  }

  if(writeCache.uri[0] != '\0') {
      NdefMessage message = NdefMessage();
      message.addUriRecord(writeCache.uri);
      message.addExternalRecord(NDEF_DOMAIN, (byte*)&(writeCache.options), sizeof(writeCache.options));
      if(nfc.write(message)) {
        Serial.println("Wrote data to tag");
        writeCache.uri[0] = '\0';
      } else {
        Serial.println("Failed to write tag");
      }
      tag = nfc.read();
  }

  if(!tag.hasNdefMessage()) {
      Serial.println("Ignoring non Ndef tag");
      nfc.haltTag();
      return;
  }

  NdefMessage msg = tag.getNdefMessage();

  size_t count = msg.getRecordCount();
  Serial.printf("Found %d records\n", count);
  for(uint8_t i=0; i < count; i++) {
    NdefRecord record = msg.getRecord(i);

    if((record.getTnf() == NdefRecord::TNF_EXTERNAL_TYPE) && 
      (record.getTypeLength() == strlen(NDEF_DOMAIN)) &&
      (memcmp(record.getType(), (byte *)NDEF_DOMAIN, record.getTypeLength()) == 0) &&
      record.getPayloadLength() == 1) {
        const PlaybackOptions *options = (PlaybackOptions *)record.getPayload();
        Serial.printf("Shuffle %d\n", options->shuffle);
        Serial.printf("Repeat %d\n", options->repeat);
        yield();
        spotify.toggleShuffle(options->shuffle);
        spotify.setRepeatMode(options->repeat ? repeat_context : repeat_off);
    } else if((record.getTnf() == NdefRecord::TNF_WELL_KNOWN) && (record.getTypeLength() == 1) && (record.getType()[0] == NdefRecord::RTD_URI)) {
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
      playbackStarted = spotify.playAdvanced(body, playbackDeviceId);
      if(playbackStarted) {
        lastUid.uidSize=sizeof(lastUid.uidByte);
        tag.getUid(lastUid.uidByte, &(lastUid.uidSize));
        Serial.printf("Started playback for %s\n", uri);
      }
    } else {
      Serial.println("Ignoring record");
      continue;
    }
}

  // No spotify tags
  nfc.haltTag();
  Serial.println("Done with tag");
}

void setup()
{

  Serial.begin(115200);

  SPI.begin();        // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522

  startWifi();

  client.setCACert(spotify_server_cert);
  client.setHandshakeTimeout(5);

  // Building up callback URL using IP address.
  sprintf(callbackURI, callbackURItemplate, callbackURIProtocol, "sprfid", callbackURIAddress);

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/callback", handleCallback);
  server.begin();
  Serial.println("HTTP server started");

  AsyncElegantOTA.begin(&server);
  Serial.println("OTA setup");

  playing = spotify.getCurrentlyPlaying("DE");
  numDevices = spotify.getDevices(devices, sizeof(devices)/sizeof(devices[0]));

  preferences.begin("sprfid", true);
  preferences.getString(PROPERTY_DEVICE, playbackDeviceId, sizeof(playbackDeviceId));
  preferences.end();
  Serial.print("Playback device: "); Serial.println(playbackDeviceId);

  Serial.println("Setup done");
}

void loop()
{
  AsyncElegantOTA.loop();

  handleRfid();

  if(updateDevice) {
    preferences.begin("sprfid", false);
    preferences.putString(PROPERTY_DEVICE, playbackDeviceId);
    preferences.end();
    updateDevice = false;
  }

  if(updateSpotify) {
    playing = spotify.getCurrentlyPlaying("DE");

    numDevices = spotify.getDevices(devices, sizeof(devices)/sizeof(devices[0]));
    updateSpotify = false;
  }
}