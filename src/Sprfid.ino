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
#include <Arduinospotify.h>
#include <ArduinoJson.h>

#include "Templates.h"

#define DEBUG 1

char clientId[33] = "";     // Your client ID of your spotify APP
char clientSecret[33] = ""; // Your client Secret of your spotify APP (Do Not share this!)
char refreshToken[132] = "";
const char scope[] = "user-read-playback-state%20user-modify-playback-state";
const char callbackURItemplate[] = "%s%s%s";
const char callbackURIProtocol[] = "http%3A%2F%2F"; // "http://"
const char callbackURIAddress[] = "%2Fcallback"; // "/callback/"

const char PROPERTY_DEVICE[] = "device";
const char PROPERTY_CLIENT_ID[] = "clientId";
const char PROPERTY_CLIENT_SECRET[] = "clientSecret";
const char PROPERTY_REFRESH_TOKEN[] = "refreshToken";


// including a "spotify_server_cert" variable
// header is included as part of the ArduinoSpotify libary
#include <ArduinoSpotifyCert.h>

AsyncWebServer server(80);
DNSServer dns;
WiFiClientSecure client;
ArduinoSpotify *spotify = NULL;

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
} tagWriteCache;

bool updateSpotify = false;
bool updateDevice = false;
bool clearNvs = false;
bool playbackStarted = false;

struct {
  char clientId[33] = {0};
  char clientSecret[33] = {0};
} nvsWriteCache;

char oauthCode[250] = {0};

void handleRoot(AsyncWebServerRequest *request)
{
  if((clientId[0] == '\0') || (clientSecret[0] == '\0')) {
    request->redirect("/setup");
    return;
  }

  AsyncResponseStream *response = request->beginResponseStream("text/html");

  response->print(HTML_HEADER);

  if(tagWriteCache.uri[0] != '\0') {
    const char pairing[] = "<p><strong>Pairing mode active!</strong> \"%s\" is going to be written onto the next tag placed on the device</p>";
    response->printf(pairing, tagWriteCache.uri);
  }
  
  if(refreshToken[0] != '\0') {
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
      (tagWriteCache.uri[0] == '\0') ? playing.albumUri : tagWriteCache.uri,
      (tagWriteCache.options.shuffle) ? "checked" : "",
      (tagWriteCache.options.repeat) ? "checked" : ""
    );
  }

  const char authLinkTpl[] = "<p><a href=\"https://accounts.spotify.com/authorize?client_id=%s&response_type=code&redirect_uri=%s&scope=%s\">spotify login</a></p>";
  char callbackURI[100];
  sprintf(callbackURI, callbackURItemplate, callbackURIProtocol, "sprfid.local", callbackURIAddress);
  response->printf(authLinkTpl, clientId, callbackURI, scope);

  response->print("</body></html>");
  request->send(response);
  updateSpotify=true;
}

void handleCallback(AsyncWebServerRequest *request) {
  if(request->hasParam("code")) {
    AsyncWebParameter *codeParam = request->getParam("code");
    String code = codeParam->value();
    strncpy(oauthCode, code.c_str(), sizeof(oauthCode)-1);
    request->redirect("/waitForRefresh");
  } else {
    request->send(404, "text/plain", "Failed to load token, check serial monitor");
  }
}

void handleWaitForRefreshToken(AsyncWebServerRequest *request) {
  if(oauthCode[0] == '\0') {
    request->redirect("/");
    return;
  }

  AsyncResponseStream *response = request->beginResponseStream("text/html");
  response->print(HTML_HEADER);
  response->print("<p>Please wait until credentials are fetched.</p><script type=\"text/javascript\">setTimeout('location.reload(true)',1000);</script>");
  request->send(response);
}

void handleSave(AsyncWebServerRequest *request) {
  if(request->hasParam("device", true)) {
    AsyncWebParameter *deviceParam = request->getParam("device", true);
    String device = deviceParam->value();
    strncpy(playbackDeviceId, device.c_str(), 40);
    Serial.printf("New playback device: %s\n", playbackDeviceId);
    updateDevice = true;
  } if(request->hasParam("uri", true)) {
    AsyncWebParameter *uriParam = request->getParam("uri", true);
    String uri = uriParam->value();
    strncpy(tagWriteCache.uri, uri.c_str(), 60);
    Serial.printf("New URI to store: %s\n", tagWriteCache.uri);
    tagWriteCache.options.shuffle = request->hasParam("shuffle", true);
    Serial.printf("New shuffle: %d\n", tagWriteCache.options.shuffle);
    tagWriteCache.options.repeat = request->hasParam("repeat", true);
    Serial.printf("New repeat: %d\n", tagWriteCache.options.repeat);
  } if(request->hasParam("clientId", true)) {
    AsyncWebParameter *clientIdParam = request->getParam("clientId", true);
    String clientId = clientIdParam->value();
    strncpy(nvsWriteCache.clientId, clientId.c_str(), 32);
    Serial.printf("New ClientId to store: %s\n", nvsWriteCache.clientId);
  } if(request->hasParam("clientSecret", true)) {
    AsyncWebParameter *clientSecretParam = request->getParam("clientSecret", true);
    String clientSecret = clientSecretParam->value();
    strncpy(nvsWriteCache.clientSecret, clientSecret.c_str(), 32);
    Serial.println("New ClientSecret to store");
  }
  request->redirect("/");
}

void handleSetup(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response = request->beginResponseStream("text/html");

  response->print(HTML_HEADER);
  response->printf(HTML_SETUP_FORM, clientId);
  request->send(response);
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
    if(spotify->pause()) {
      playbackStarted = false;
    } else {
      playing = spotify->getCurrentlyPlaying("DE");
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
    if(spotify->play()) {
      Serial.println("Resumed playback");
      playbackStarted = true;
    }
    return;
  }

  NfcTag tag = nfc.read();

  if(!tag.isFormatted()) {
    if(tagWriteCache.uri[0] == '\0') {
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

  if(tagWriteCache.uri[0] != '\0') {
      NdefMessage message = NdefMessage();
      message.addUriRecord(tagWriteCache.uri);
      message.addExternalRecord(NDEF_DOMAIN, (byte*)&(tagWriteCache.options), sizeof(tagWriteCache.options));
      if(nfc.write(message)) {
        Serial.println("Wrote data to tag");
        tagWriteCache.uri[0] = '\0';
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
        spotify->toggleShuffle(options->shuffle);
        spotify->setRepeatMode(options->repeat ? repeat_context : repeat_off);
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
      playbackStarted = spotify->playAdvanced(body, playbackDeviceId);
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

  server.on("/", handleRoot);
  server.on("/callback", handleCallback);
#ifdef DEBUG
  server.on("/reset", [](AsyncWebServerRequest *request){
    clearNvs = true;
    request->redirect("/");
  });
#endif
  server.on("/save", handleSave);
  server.on("/setup", handleSetup);
  server.on("/waitForRefresh", handleWaitForRefreshToken);
  server.begin();
  Serial.println("HTTP server started");

  AsyncElegantOTA.begin(&server);
  Serial.println("OTA setup");

  preferences.begin("sprfid", true);
  preferences.getString(PROPERTY_DEVICE, playbackDeviceId, sizeof(playbackDeviceId));
  preferences.getString(PROPERTY_CLIENT_ID, clientId, sizeof(clientId));
  preferences.getString(PROPERTY_CLIENT_SECRET, clientSecret, sizeof(clientSecret));
  preferences.getString(PROPERTY_REFRESH_TOKEN, refreshToken, sizeof(refreshToken));
  preferences.end();
  Serial.println();

  if((clientId[0] != '\0') && (clientSecret[0] != '\0')) {
    if(refreshToken[0] == '\0') {
      Serial.println("Creating spotify client without refreshToken");
      spotify = new ArduinoSpotify(client, clientId, clientSecret, NULL);
    } else {
      spotify = new ArduinoSpotify(client, clientId, clientSecret, refreshToken);

      Serial.print("Playback device: "); Serial.println(playbackDeviceId);

      playing = spotify->getCurrentlyPlaying("DE");
      numDevices = spotify->getDevices(devices, sizeof(devices)/sizeof(devices[0]));
    }
  } else {
    Serial.println("Spotify credentials missing");
  }

  Serial.println("Setup done");
}

void loop()
{
  AsyncElegantOTA.loop();

  if(updateDevice) {
    preferences.begin("sprfid", false);
    preferences.putString(PROPERTY_DEVICE, playbackDeviceId);
    preferences.end();
    updateDevice = false;
  }

  if(nvsWriteCache.clientId[0] != '\0' || nvsWriteCache.clientSecret[0] != '\0') {
    Serial.println("Storing new credentials");
    preferences.begin("sprfid", false);
    if(nvsWriteCache.clientId[0] != '\0') {
      preferences.putString(PROPERTY_CLIENT_ID, nvsWriteCache.clientId);
    }
    if(nvsWriteCache.clientSecret[0] != '\0') {
      preferences.putString(PROPERTY_CLIENT_SECRET, nvsWriteCache.clientSecret);
    }
    preferences.end();
    ESP.restart();
  }

  if(oauthCode[0] != '\0') {
    const char *newRefreshToken = NULL;

    char callbackURI[100];
    sprintf(callbackURI, callbackURItemplate, callbackURIProtocol, "sprfid.local", callbackURIAddress);

    newRefreshToken = spotify->requestAccessTokens(oauthCode, callbackURI);

    if (newRefreshToken != NULL)
    {
      Serial.println("Storing refresh token");
      preferences.begin("sprfid", false);
      preferences.putString(PROPERTY_REFRESH_TOKEN, newRefreshToken);
      preferences.end();
      strncpy(refreshToken, newRefreshToken, sizeof(refreshToken)-1);
    }

    oauthCode[0] = '\0';
  }

  if(clearNvs) {
    preferences.begin("sprfid", false);
    preferences.clear();
    preferences.end();
    ESP.restart();
  }

  if((spotify != NULL) && (refreshToken[0] != '\0')) {
    handleRfid();

    if(updateSpotify) {
      playing = spotify->getCurrentlyPlaying("DE");

      numDevices = spotify->getDevices(devices, sizeof(devices)/sizeof(devices[0]));
      updateSpotify = false;
    }
  }
}