// Connectivity includes
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// Data management includes
#include <EEPROM.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>


/* Hello there!
 * This project was made in ~24h, from this (simple yet sweet) concept, through the 3D modeling/printing even to the programming part of it.
 * (And yes, it uses a scratch-based visual language for the Android (not iOS compatible) app... At the very least it works.)
 * Globally, it works without any apparent issues after some tests, but it's far from being perfect, and is not quite yet optimized.
 * Furthermore, I couldn't implement all functionalities in time, the cache upload to a defined URL using the APSTA mode 
 * (both hosting a wifi AP and connected to another AP) couldn't be done in time. Please note that this was my first time using the ESP-32, 
 * among doing async on a microcontroller (but actually way easier than thought).
 * I will soon make another commit with a lot of fixes and upgrades.
 */


// All "enums"
#define GOOD_AIR_QUALITY 0  // Not using enum to avoid wasting the memory
#define MEDIUM_AIR_QUALITY 1
#define BAD_AIR_QUALITY 2
#define CATASTROPHIC_AIR_QUALITY 3

#define NO_AP 0
#define UNENCRYPTED_AP 1  // Not implemented
#define ENCRYPTED_AP 2  // Not implemented

#define NO_HISTORY 0
#define STORE_AND_STOP 1
#define STORE_AND_COMPRESS 2
#define STORE_AND_UPLOAD 3  // Not implemented

#define HISTORY_NOT_USED 0
#define HISTORY_USED 1
#define HISTORY_COMPRESSING 2

// Define the EEPROM struct and variables
typedef struct eeprom_data {
  char urlToCommit[256];  // Size: 256 - Not implemented

  char pass[64];  // Size: 64 - Not implemented
  char ssid[32];  // Size: 32 - Not implemented

  uint32_t uid;  // Size: 4
  uint32_t intervalRate;  // Size: 4

  uint16_t historySize;  // Size: 2

  byte maxMergingCompression;  // Size: 1
  byte historicalDataMode;  // Size: 1
  
  byte connectToOtherAP;  // Size: 1 
  byte signalVenting;  // Size: 1
} eeprom_data;

uint32_t startupCount = 0;  // Size: 4
eeprom_data parameters;

// Define the historical data struct and list
typedef struct historical_data_node {
  uint32_t epoch;  // Size: 4
  uint16_t value;  // Size: 2
} historical_data_node;

historical_data_node * history;
uint16_t historyRealSize = 0;
uint16_t historyCompressedSize = 0;

byte historyState = HISTORY_NOT_USED;

byte compressing = false;
uint8_t currentCompressionLevel = 16;
uint32_t lastCompressed = 0;
byte resizeHistorySignal = false;

// New (async) web server on port 80
AsyncWebServer server(80);

// Current time
unsigned long currentTime = 0;
unsigned long currentTimeInSeconds = 0;
unsigned long lastCurrentTimeRegistered = 0;
boolean isLoading = true;

// Pins
const int redLed = 5;
const int yellowLed = 18;
const int greenLed = 19;

const int mainBtn = 4;
const int airSensor = 15;

// For the AP and STA, all the required variables
char STA_SSID[32];  // Not implemented yet
char STA_PASS[64];  // Not implemented yet
byte STA_SUCCESS = false;  // Not implemented yet
byte URL_UPLOAD_FAILED = false;  // Not implemented yet

char * AP_SSID;
const char * AP_PASS = "";

// Other 
byte currentState = 0;
byte ventingNudge = false;

uint16_t lastMeasure = 0;
uint32_t lastSerialPrint = 0;
uint32_t lastSavedMeasure = 0xFFFFFFFF;

byte btnPressed = false;
uint32_t btnPressedTime = 0;


void setup() {
  // Serial for debugging purpose
  Serial.begin(115200);
  Serial.println("\nHello world!");  // Serial test
  
  // Init all the pins
  Serial.println("> [Interface] - Setting up...");
  pinMode(redLed, OUTPUT);
  pinMode(yellowLed, OUTPUT);
  pinMode(greenLed, OUTPUT);

  ledcAttachPin(greenLed, 0);  // Attached to esp32 pwm channel
  ledcAttachPin(yellowLed, 1);
  ledcAttachPin(redLed, 2);

  ledcSetup(0, 1000, 10);  // 10 bits precision, 1khz frequency
  ledcSetup(1, 1000, 10);
  ledcSetup(2, 1000, 10);
  
  pinMode(mainBtn, INPUT_PULLUP);  // Low when button is pressed, High when not
  pinMode(airSensor, INPUT);

  // Starts the loading animation
  xTaskCreate(ledLoadingAnimation, "ledLoadingAnimation", 4096, NULL, 1, NULL);
  Serial.println("> [Interface] - Workload done!");

  // Load the EEPROM parameters
  Serial.println("> [EEPROM] - Reading data...");
  EEPROM.begin(sizeof(parameters) + sizeof(startupCount));
  EEPROM.get(0, startupCount);
  EEPROM.get(sizeof(startupCount), parameters);

  // EEPROM limits on ESP-32
  if (startupCount >= 8000) {
    Serial.println("\n=== Warning! ===\nYou reached " + String(startupCount) + " launches with the device.\nYou are close to the 10k launches limit or already over. Starting from there, there is no guarantee that the internal memory will work correctly in the future on a classic ESP-32. These kind of memory are robust BUT not eternal, especially when used to store settings like in this case.\nTry resetting the memory if you start to see strange behavior or that the device won't start anymore.\n");
  }

  // In case of a first time launch, set up all the data
  if (startupCount == 0 || startupCount == 0xFFFFFFFF) {  // Considering first time launch as either value 0, or max value of 4 unsigned bytes (EEPROM memory always has 0xFF per default on its bytes)
    Serial.println("> [EEPROM] - First launch detected, getting memory ready...");
    firstTimeLaunch();
  } else {
    Serial.print("> [EEPROM] - Starting as ");
    Serial.print(parameters.uid, DEC);
    Serial.print(" for its ");
    Serial.print(startupCount, DEC);
    Serial.println(" launch.");
  }

  history = (historical_data_node *) malloc(sizeof(historical_data_node) * parameters.historySize);
  Serial.println("> [EEPROM] - Device memory's ready!");

  // Configure WiFi mode
  Serial.println("> [WiFi] - Starting...");
  if (parameters.connectToOtherAP) {
    // Will probably implement an upload system that connects to an AP in a future release
    WiFi.mode(WIFI_AP_STA);
    Serial.println("> [WiFi] - Acces Point and Station mode enabled");
  } else {
    WiFi.mode(WIFI_AP);
    Serial.println("> [WiFi] - Access Point only mode enabled");
  }
  delay(200);

  // Starts the soft AP
  Serial.println("> [WiFi] - Starting the soft Access Point...");
  AP_SSID = (char *) malloc(sizeof(char) * 32);
  sprintf(AP_SSID, "airFlux.135%lu", parameters.uid);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(2000);
  
  Serial.print("> [WiFi] - Soft Access Point started with station IP: ");
  Serial.println(WiFi.softAPIP());

  // If the APSTA mode is on, tries to connect to the targetted AP
  if (parameters.connectToOtherAP) {
    // Not implemented yet
  }
  Serial.println("> [WiFi] - Workload done!");

  // Set up web server
  Serial.println("> [WebServer] - Setting up the web server...");
  server.on("/", HTTP_GET, handleRoot );
  server.on("/api/", HTTP_GET, handleAPIRoot);
  server.on("/api/fetchCurrent", HTTP_GET, handleFetchCurrentData);
  server.on("/api/fetchAndCleanCache", HTTP_GET, handleFetchAndCleanCache);
  server.on("/api/updateParams", HTTP_GET, handleUpdateParams);
  server.on("/api/nudgeVenting", HTTP_GET, handleNudging);
  //server.on("/api/setAccessPoint", HTTP_POST, handleSetAccessPoint);  // Not implemented yet
  //server.on("/api/setUploadURL", HTTP_POST, handleAPIRoot);  // Not implemented yet
  server.on("/api/syn", HTTP_GET, handleAPISyn);

  server.onNotFound(handleNotFound);

  // Declares airFlux as started
  Serial.println("> [EEPROM] - Updating memory...");
  EEPROM.put(0, ++startupCount);
  EEPROM.commit();
  Serial.println("> [EEPROM] - Updated memory!");
  
  server.begin();
  Serial.println("> [WebServer] - Done!");

  isLoading = false;
  Serial.println("> Device is ready :)");
}


// First time launch function (with default settings)
void firstTimeLaunch() {
  startupCount = 1;
  parameters.intervalRate = 900;  // 15 minutes between each information save
  parameters.signalVenting = true;  // Do signal venting
  parameters.historicalDataMode = STORE_AND_COMPRESS;  // Saving, with compression only mode
  parameters.connectToOtherAP = NO_AP;  // No AP configured by default
  parameters.maxMergingCompression = 64;
  parameters.historySize = 8192;
  
  parameters.uid = esp_random();  // Generate an unique ID, being the identifier of this airFlux
  
  EEPROM.put(0, startupCount);
  EEPROM.put(sizeof(startupCount), parameters);

  EEPROM.commit();

  Serial.println("EEPROM saved for first launch!");
}


// Smooth led change
void animateLEDChange(uint8_t led, byte state, uint16_t delayMs) {
  if (led > 2) return;
  
  if (state) {
    for (short i = 0; i < 1023; ++i) {
      ledcWrite(led, i);
      delayMicroseconds(delayMs ? delayMs : 250);
    }
  } else {
    for (short i = 1023; i > 0; --i) {
      ledcWrite(led, i);
      delayMicroseconds(delayMs ? delayMs : 250);
    }
  }

  return;
}


// Create a noice loading animation
void ledLoadingAnimation(void * param) {
  while (isLoading) {
    animateLEDChange(0, true, 500);
    animateLEDChange(0, false, 500);
  
    animateLEDChange(1, true, 500);
    animateLEDChange(1, false, 500);

    animateLEDChange(2, true, 500);
    animateLEDChange(2, false, 500);

    animateLEDChange(1, true, 500);
    animateLEDChange(1, false, 500);

    taskYIELD();
  }

  vTaskDelete(NULL);
  return;
}


// Do all the work to animate correctly according to the air quality or general state
void currentStateLED(void * param) {
  uint8_t actualLED = 3;
  
  while (true) {
    if (!isLoading) {
      // Blink red light extra fast if the cache is filling up even with (or without) the compression mechanism doing its best
      if (historyCompressedSize >= parameters.historySize * 0.95) {
        if (actualLED != 2) {
          animateLEDChange(actualLED, false, 500);
          actualLED = 2;
          animateLEDChange(actualLED, true, 100);
        }
        animateLEDChange(actualLED, false, 100);
        animateLEDChange(actualLED, true, 100);
        continue;
      }

      // Smooth animations through all the states
      switch (currentState) {
        case GOOD_AIR_QUALITY:
          if (actualLED != 0) {
            animateLEDChange(actualLED, false, 500);
            actualLED = 0;
            animateLEDChange(actualLED, true, 500);
          }
          break;
        case MEDIUM_AIR_QUALITY:
          if (actualLED != 1) {
            animateLEDChange(actualLED, false, 500);
            actualLED = 1;
            animateLEDChange(actualLED, true, 500);
          }
          break;
        case BAD_AIR_QUALITY:
          if (actualLED != 2) {
            animateLEDChange(actualLED, false, 500);
            actualLED = 2;
            animateLEDChange(actualLED, true, 500);
          }
          break;
        case CATASTROPHIC_AIR_QUALITY:
          if (actualLED != 2) {
            animateLEDChange(actualLED, false, 500);
            actualLED = 2;
            animateLEDChange(actualLED, true, 1000);
          } else if (!ventingNudge) {
            animateLEDChange(actualLED, false, 1000);
            animateLEDChange(actualLED, true, 1000);
          }
          break;
        default:
          break;
      }
    }
    taskYIELD();
  }
}


// Convert the current state into a string
String currentStateInString() {
  if (currentState == GOOD_AIR_QUALITY) {
    return "good";
  } else if (currentState == MEDIUM_AIR_QUALITY) {
    return "mediocre";
  } else {
    return "bad";
  }
}


// Simply compress data in a lossfull way, by merging close values together (calculating the mean out of it)
void compressHistory(void * param) {
  compressing = true;
  
  Serial.println("> [Cache] - Compression routine started (size before: " +  String(historyCompressedSize) +  ")...");
  Serial.print("> [Cache] - Initial list: ");
  for (uint16_t h = 0; h < historyCompressedSize; h++) {
    Serial.print(String(history[h].value) + ", ");
  }
  Serial.println();
  
  uint64_t mean = 0;
  uint16_t count = 0;
  uint16_t i = 0;
  uint16_t j = 0;

  // Thread safe
  while (historyState != HISTORY_NOT_USED) {
    taskYIELD();  // Yield as the compression task must be run async
  }
  historyState = HISTORY_COMPRESSING;

  // Loop through all the elemnts
  while (i < historyCompressedSize) {
    mean = (uint64_t) history[i].value;
    count = 0;

    // And loop again for elements following the actual i element, and stop when the distance between element i and element j is above the current compression level
    for (j = i + 1; j < historyCompressedSize && abs((int32_t) history[j].value - (int32_t) history[i].value) < currentCompressionLevel; j++) {
      count++;
      mean += (uint64_t) history[j].value;
      taskYIELD();
    }

    // Avoid division by 0
    if (count) {
      history[i].value = (uint16_t) (mean / ((uint64_t) count));
    }

    // If compressable
    if (count > 1) {
      // No elements are after the last compressable element
      if (j == historyCompressedSize) {
        // Resize the whole history
        historyCompressedSize = i + 1;
  
        if (currentCompressionLevel < parameters.maxMergingCompression) {
          currentCompressionLevel++;
        }
        
        lastCompressed = currentTimeInSeconds;
        compressing = false;
  
        historyState = HISTORY_NOT_USED;
  
        Serial.println("> [Cache] - Compression routine done! (size now:" + String(historyCompressedSize) + ")");
  
        Serial.print("Final: ");
        for (short h = 0; h < historyCompressedSize; h++) {
          Serial.print(String(history[h].value) + ", ");
        }
        Serial.println();
        
        vTaskDelete(NULL);
        return;
      // Needs to shift the following history
      } else {
        // Shift the whole history
        for (j = i + 1; j < historyCompressedSize; ++j) {
          history[j] = history[j + count];
          taskYIELD();
        }
        historyCompressedSize -= count;
      }
    }
    
    i++;
    taskYIELD();
  }

  // For the next compression run, up the max acceptable merging compression level
  if (currentCompressionLevel < parameters.maxMergingCompression) {
    currentCompressionLevel++;
  }
  lastCompressed = currentTimeInSeconds;
  compressing = false;

  historyState = HISTORY_NOT_USED;

  Serial.println("> [Cache] - Compression routine done! (size now:" + String(historyCompressedSize) + ")");
  Serial.print("> [Cache] - Final list: ");
  for (uint16_t h = 0; h < historyCompressedSize; h++) {
    Serial.print(String(history[h].value) + ", ");
  }
  Serial.println();
  
  vTaskDelete(NULL);
  return;
}


// Resets the memory (bad for the EEPROM, even if it's a put and not a set operation (overwrites only if different))
void EEPROMRewrite(void * param) {
  char debug[200];
  sprintf((char *) &debug, "> [EEPROM] - Writing parameters...\n> [EEPROM] - Parameters:\n> signalVenting: %s\n> historyMode: %s\n> intervalRate: %u\n> maxMergingCompression: %u\n> historySize: %u", parameters.signalVenting ? "true" : "false", parameters.historicalDataMode ? (parameters.historicalDataMode == STORE_AND_STOP ? "StoreOnly" : "CompressionAllowed") : "off", parameters.intervalRate, parameters.maxMergingCompression, parameters.historySize);
  Serial.println(debug);
  
  EEPROM.put(0, startupCount);
  EEPROM.put(startupCount, parameters);
  EEPROM.commit();
  
  Serial.println("> [EEPROM] - Data wrote!");

  vTaskDelete(NULL);
  return;
}


// Try to connect to an Access Point
void connectToAP() {
  return;  // Not implemented yet
}


// Main function
void loop(){
  Serial.println("\n> Main thread is now in loop function");
  xTaskCreate(currentStateLED, "currentStateLED", 4096, NULL, 1, NULL);
  
  while (true) {
    // Register current time at start of the loop and the last sensor measure
    currentTime = millis();
    lastMeasure = analogRead(airSensor);

    // Thread safe history resize
    if (resizeHistorySignal && historyState == HISTORY_NOT_USED) {
        historyState = HISTORY_USED;
        
        if (historyRealSize > 0) {
          history = (historical_data_node *) realloc(history, sizeof(historical_data_node) * parameters.historySize);
        } else {
          history = (historical_data_node *) malloc(sizeof(historical_data_node) * parameters.historySize);
        }
        
        if (historyRealSize > parameters.historySize) {
          // For the sake of simplicity, just lose the current data
          historyRealSize = 0;
          historyCompressedSize = 0;
        }

        resizeHistorySignal = false;
        historyState = HISTORY_NOT_USED;
    }

    // Compression mode is on, and data is using 90% or more of allocated memory (Can compress every 120 seconds)
    if (parameters.historicalDataMode >= STORE_AND_COMPRESS && currentTimeInSeconds - lastCompressed > 120 && !compressing) {
      if (historyCompressedSize >= parameters.historySize * 0.9) {
        // Needs to compress
        compressing = true;
        xTaskCreate(compressHistory, "compressHistory", 10240, NULL, 1, NULL);
      }
    }

    if ((int64_t) lastCurrentTimeRegistered - (int64_t) currentTime > 15000) {
      // Consider that the millis() has overflowed (or the loop function hasn't run for 15 seconds but that's another story)
      lastCurrentTimeRegistered = 0;
    }

    if (lastCurrentTimeRegistered - currentTime > 1000) {
      // Precisely counts the seconds without resetting after ~49.71 days (4 bytes max value in milliseconds) like the millis() function will do
      currentTimeInSeconds++;
      lastCurrentTimeRegistered += 1000;
    }

    if (((currentTimeInSeconds - lastSavedMeasure >= parameters.intervalRate) || lastSavedMeasure == 0xFFFFFFFF) && historyState == HISTORY_NOT_USED && parameters.historicalDataMode > NO_HISTORY && historyCompressedSize < parameters.historySize) {
      // Thread safe history saving (doesn't save while compression is happening, which might delay a bit the saving)
      historyState = HISTORY_USED;
      lastSavedMeasure = currentTimeInSeconds;

      history[historyCompressedSize].epoch = currentTimeInSeconds;
      history[historyCompressedSize].value = lastMeasure;
      
      historyRealSize++;
      historyCompressedSize++;
      historyState = HISTORY_NOT_USED;
      Serial.println("> [Cache] - Saved at time " + String(currentTimeInSeconds) + "s, with value: " + String(lastMeasure));
    }

    // Debug print the sensor value every five seconds
    if (currentTimeInSeconds - lastSerialPrint > 5) {
      Serial.print("> [DEBUG] - Current value: ");
      Serial.println(lastMeasure, DEC);

      lastSerialPrint = currentTimeInSeconds;
    }

    // Calculate state
    switch (currentState) {
      case GOOD_AIR_QUALITY:
        if (lastMeasure > 300) {
          currentState =  MEDIUM_AIR_QUALITY;
        }
        break;
      case MEDIUM_AIR_QUALITY:
        if (lastMeasure < 280) {
          currentState =  GOOD_AIR_QUALITY;
        } else if (lastMeasure > 500) {
          currentState =  BAD_AIR_QUALITY;
        }
        break;
      case BAD_AIR_QUALITY:
        if (lastMeasure < 480) {
          currentState =  MEDIUM_AIR_QUALITY;
        } else if (lastMeasure > 660) {
          currentState =  CATASTROPHIC_AIR_QUALITY;
        }

        ventingNudge = false;
        break;
      case CATASTROPHIC_AIR_QUALITY:
        if (lastMeasure < 620) {
          currentState = BAD_AIR_QUALITY;
        }
        break;
      default:
        break;
    }

    // Button actions
    btnPressed = digitalRead(mainBtn);
    
    if (btnPressed == LOW) {
      // Count how long the button was pressed
      if (btnPressedTime == 0) {
        btnPressedTime = currentTime;
      }
    } else if (btnPressed == HIGH && btnPressedTime > 0) {  // If the button was pressed for less than 5s, nudge the venting alert if it's on
      if (currentTime - btnPressedTime < 5000) {
        if (currentState == CATASTROPHIC_AIR_QUALITY && parameters.signalVenting) {
          ventingNudge = true;
        }
      } else if (currentTime - btnPressedTime < 15000) {  // If the button was pressed between 5s and 15s, restart the esp32
        Serial.println("> Restarting from user input...\n\n\n");
        isLoading = true;
        ledcWrite(0, 0);
        ledcWrite(1, 0);
        ledcWrite(2, 0);
        ESP.restart();
      } else {  // If the button was pressed longer than 15 seconds, reset the EEPROM memory
        Serial.println("> [EEPROM] - Resetting memory...");
        firstTimeLaunch();
        Serial.println("> [EEPROM] - Memory wiped.");

        Serial.println("> Restarting from user input...\n\n\n");
        isLoading = true;
        ledcWrite(0, 0);
        ledcWrite(1, 0);
        ledcWrite(2, 0);
        ESP.restart();
      }

      btnPressedTime = 0;
    }

    taskYIELD();
  }
}


// All the HTTP request handler, names speaks by themselves
void handleRoot(AsyncWebServerRequest *req) {
  Serial.println("> [WebServer] - Client tried to directly access the server root");
  req->send(200, "text/plain", String("Server is up and running. Please find the airFlux API under http://") + WiFi.softAPIP().toString() + String(":80/api/."));
}


void handleAPIRoot(AsyncWebServerRequest *req) {
  Serial.println("> [WebServer] - Client discovered API");
  req->send(200, "application/json", "{\"message\":\"AirFlux API is on and working.\", \"commands\":[\"GET: api/syn\", \"GET: api/fetchCurrentData\", \"GET: api/fetchAndCleanCache\", \"GET: updateParams\", \"GET: api/nudgeVenting\"]}");
}


void handleAPISyn(AsyncWebServerRequest *req) {
  if (req->hasParam("syn")) {
    String syn = req->getParam("syn")->value();
    uint32_t synValue = (uint32_t) atoi(syn.c_str());

    Serial.println("> [WebServer] - Client tried to see if the ESP32 still knew how to increment a number. It apparently worked.");
    req->send(200, "application/json", "{\"syn\":" + String(++synValue) + "}");
  }
  req->send(400, "text/plain", "Can't find syn parameter.");
}


void handleFetchCurrentData(AsyncWebServerRequest *req) {
  StaticJsonDocument<666> answer;

  answer["sensorValue"] = lastMeasure;
  answer["currentState"] = currentStateInString();
  answer["trigger"] = (currentState == CATASTROPHIC_AIR_QUALITY && parameters.signalVenting && !ventingNudge) ? 1 : 0;

  answer["timestampWorthOfStoredData"] = historyRealSize * parameters.intervalRate;
  answer["isCompressed"] = (historyRealSize > historyCompressedSize) ? 1 : 0;
  // Determine whether the user should be warned that the cache needs to be fetched so it doesn't overload.
  answer["needsCacheFlush"] = (historyRealSize > historyCompressedSize || historyRealSize > parameters.historySize * 0.9) ? 1 : 0;

  String answerStringified;
  serializeJson(answer, answerStringified);

  Serial.println("> [WebServer] - Successfully gave current data to client");
  req->send(200, "application/json", answerStringified);
}


void handleFetchAndCleanCache(AsyncWebServerRequest *req) {
  // Needs to make sure about the answer's size limit. Shouldn't be a problem under 10000 chars or around 1000 elements. But it should be able to support up to around 175kB of chars, maybe using a stream ?
  // Will release a working solution in a fix. Could also send it step by step, requiring calling the api a lot of time.
  if (historyState != HISTORY_NOT_USED ) {
    req->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  } else if (parameters.historicalDataMode == NO_HISTORY) {
    req->send(404, "application/json", "{\"error\":\"can't found history as it's disabled\"}");
    return;
  }
  historyState = HISTORY_USED;
  
  String answer;
  char * tempFormat = (char *) malloc(sizeof(char) * 45);

  answer += "{\"currentLocalTime\":" + String(currentTimeInSeconds) + ",\"data\":\"";

  for (uint16_t i = 0; i < historyCompressedSize; ++i) {
    sprintf(tempFormat, "%lu:%u,", history[i].epoch, history[i].value);
    Serial.print("there: ");
    Serial.println(String(tempFormat));
    
    answer += String(tempFormat);
  }

  answer += "\"}";
  
  historyRealSize = 0;
  historyCompressedSize = 0;
  currentCompressionLevel = 0;

  historyState = HISTORY_NOT_USED;

  Serial.println("> [WebServer] - Delivered cache to client and cleaned it");
  req->send(200, "application/json", answer);
}


void handleUpdateParams(AsyncWebServerRequest *req) {
  if (req->hasParam("historicalDataMode")) {
    uint16_t newHistoricalDataMode = (uint16_t) strtol(req->getParam("historicalDataMode")->value().c_str(), NULL, 0);
    if (newHistoricalDataMode >= 0 && newHistoricalDataMode <= 2) {
      parameters.historicalDataMode = newHistoricalDataMode;
    }

    if (parameters.historicalDataMode == NO_HISTORY) {
      historyCompressedSize = 0;
      historyRealSize = 0;
      free(history);
    }
  }
  
  if (req->hasParam("historySize")) {
    uint16_t newHistorySize = (uint16_t) strtol(req->getParam("historySize")->value().c_str(), NULL, 0);
    if (newHistorySize >= 256 && newHistorySize <= 8192) {
      parameters.historySize = newHistorySize;

      // If history is actually enabled
      if (parameters.historicalDataMode > NO_HISTORY) {
        // Resize history signal (Can't do it here, different heap as each thread has it own heap)
        resizeHistorySignal = true;
      }
    }
  }

  if (req->hasParam("maxMergingCompression")) {
    byte newCompressionLevel = (byte) strtol(req->getParam("maxMergingCompression")->value().c_str(), NULL, 0);
    if (newCompressionLevel >= 0 && newCompressionLevel <= 255) {
      parameters.maxMergingCompression = newCompressionLevel;
    }
  }

  if (req->hasParam("intervalRate")) {
    uint16_t newIntervalRate = (uint16_t) strtol(req->getParam("intervalRate")->value().c_str(), NULL, 0);
    if (newIntervalRate >= 1) {
      parameters.intervalRate = newIntervalRate;
    }
  }

  if (req->hasParam("signalVenting")) {
    parameters.signalVenting = (byte) ((req->getParam("signalVenting")->value() == "true") ? 1 : 0);
  }

  xTaskCreate(EEPROMRewrite, "EEPROMRewrite", 3192, NULL, 1, NULL);
  
  Serial.println("> [WebServer] - Client updated parameters");
  req->send(200, "application/json", "{\"success\":\"true\"}");
}


void handleNudging(AsyncWebServerRequest *req) {
  if (currentState == CATASTROPHIC_AIR_QUALITY) {
    ventingNudge = true;
  }

  Serial.println("> [WebServer] - Venting alert was nudged by client");
  req->send(200, "application/json", "{\"success\":\"true\"}");
}


void handleNotFound(AsyncWebServerRequest *req) {
  Serial.println("> [WebServer] - Client is apparently struggling to use the API x)");
  req->send(404, "text/plain", "Couldn't find ressource.");
}
