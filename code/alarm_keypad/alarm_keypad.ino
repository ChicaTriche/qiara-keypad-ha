#include <Keypad.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ========================
// WiFi & MQTT
// ========================
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PWD";
const char* mqtt_server = "192.168.1.180"; // your HA broker IP
const char* mqtt_user = "your_mqtt_username";
const char* mqtt_pass = "your_mqtt_pwd";

WiFiClient espClient;
PubSubClient client(espClient);

// ========================
// Pins & LEDs
// ========================
#define LED_STATUS_PIN A0
#define LED_STATUS_GPIO 1
#define BUZZER_PIN D7
#define NUMPIXELS 1
Adafruit_NeoPixel strip(NUMPIXELS, LED_STATUS_GPIO, NEO_GRB + NEO_KHZ800);

// ========================
// Keypad
// ========================
const byte numRows = 4;
const byte numCols = 3;
char keys[numRows][numCols] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};
byte rowPins[numRows] = {D2,D3,D4,D5};
byte colPins[numCols] = {D10,D11,D12};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, numRows, numCols);

// ========================
// Alarm state
// ========================
bool armed = false;
bool armingInProgress = false;
unsigned long armingStart = 0;      // ← à ajouter
unsigned long lastBlinkTime = 0;    // ← à ajouter
bool blinkState = false;            // ← à ajouter
String codeBuffer = "";
unsigned long lastKeyPress = 0;
unsigned long codeTimeout = 15000; // 15 sec code timeout
unsigned long armingDelay = 10000; // 10 sec for testing
unsigned long codeErrorBlink = 50;
unsigned long hwErrorBlink = 80;
unsigned long keyBipDuration = 50;

// ========================
// MQTT Topics
// ========================
const char* topicRequest = "home/alarm/request";   // keypad → HA
const char* topicResponse = "home/alarm/response"; // HA → keypad (ok/fail)
const char* topicState = "home/alarm/state";       // HA → keypad (armed/disarmed)
const char* topicLog = "home/alarm/log";           // optional

// ========================
// Setup
// ========================
void setup() {

  Serial.begin(115200);
  WiFi.begin(ssid,password);
  Serial.print("Connecting to WiFi");
  while(WiFi.status() != WL_CONNECTED){ delay(500); Serial.print("."); }
  Serial.println(" connected");

  client.setServer(mqtt_server,1883);
  client.setCallback(mqttCallback);

  strip.begin();
  strip.setPixelColor(0, strip.Color(0,127,0)); // LED verte
  strip.show();
  pinMode(BUZZER_PIN, OUTPUT);
  //digitalWrite(BUZZER_PIN, LOW);
  noTone(BUZZER_PIN);

  Serial.println("Keypad alarm prototype ready");
}

// ========================
// MQTT Callback
// ========================
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // reconstruire le message reçu
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("MQTT reçu sur ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(msg);

  // ====== MESSAGE ETAT ALARME ======
  if(String(topic) == topicState){

    StaticJsonDocument<200> doc;

    DeserializationError error = deserializeJson(doc, msg);

    if(error){
      Serial.println("Erreur parsing JSON");
      return;
    }

    const char* state = doc["state"];

    Serial.print("Etat extrait = ");
    Serial.println(state);

    if(strcmp(state, "armed") == 0){
      if(!armed){
        armAlarm();
      }
    }
    else if(strcmp(state, "disarmed") == 0){
      if(armed){
        disarmAlarm();
      }
    }
    else if(strcmp(state, "hw_error") == 0){
      hwError();
    }

  }

  // ====== REPONSE CODE ======
  if(String(topic) == topicResponse){

    StaticJsonDocument<200> doc;

    if(deserializeJson(doc, msg)) return;

    const char* result = doc["result"];

    if(strcmp(result, "ok") == 0){
      disarmAlarm();
    } else {
      codeError();
    }
  }
}


// ========================
// MQTT Connect
// ========================
void reconnect() {
  while(!client.connected()){
    Serial.print("Connecting to MQTT...");
    // On fournit le username/password ici
    if(client.connect("ESP32Keypad", mqtt_user, mqtt_pass)){ 
      Serial.println("connected");
      client.subscribe(topicResponse);
      client.subscribe(topicState);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retry in 2s");
      delay(2000);
    }
  }
}

// ========================
// Main Loop
// ========================
void loop() {
  if(!client.connected()) reconnect();
  client.loop();

  if (armingInProgress) {
    unsigned long now = millis();

    // Annulé depuis MQTT pendant le décompte
    //if (!armingInProgress) return; // sera géré au prochain tick

    // Décompte terminé → alarme armée
    if (now - armingStart >= armingDelay) {
      armingInProgress = false;
      armed = true;
      strip.setPixelColor(0, strip.Color(127, 0, 0)); // rouge fixe
      strip.show();
      //digitalWrite(BUZZER_PIN, LOW);
      noTone(BUZZER_PIN);
      Serial.println("Alarme armée.");
    }
    // Clignotement pendant le décompte
    else if (now - lastBlinkTime >= 500) {
      lastBlinkTime = now;
      blinkState = !blinkState;
      strip.setPixelColor(0, blinkState ? strip.Color(127, 0, 0) : strip.Color(0, 0, 0));
      strip.show();
      //digitalWrite(BUZZER_PIN, blinkState ? HIGH : LOW);
      //blinkState ? tone(BUZZER_PIN, 4100) : noTone(BUZZER_PIN);
      if (blinkState) {
        tone(BUZZER_PIN, 4100);
      } else {
        noTone(BUZZER_PIN);
      }
    }
  }

  // === Gestion clavier ===
  char key = keypad.getKey();
  if(key) Serial.println(key);
  unsigned long now = millis();

  // Code timeout
  if(codeBuffer.length() >0 && (now - lastKeyPress > codeTimeout)){
    Serial.println("Code timeout - buffer reset");
    codeBuffer = "";
  }

  if(key){
    lastKeyPress = now;

    // beep at each keypress
    //digitalWrite(BUZZER_PIN,HIGH);
    tone(BUZZER_PIN, 4100); 
    delay(keyBipDuration);
    //digitalWrite(BUZZER_PIN,LOW);
    noTone(BUZZER_PIN); 

    if(key=='*'){ // Arm
      Serial.println("*");
      
      if(!armed){
        client.publish(topicRequest,"{\"action\":\"arm\"}");
      }
      codeBuffer="";
    }
    else if(key=='#'){ // Disarm attempt
      if(armingInProgress){
        // Annuler l'armement
        armingInProgress = false;
        strip.setPixelColor(0, strip.Color(0, 127, 0));
        strip.show();
        noTone(BUZZER_PIN);
        Serial.println("Armement annulé");
        codeBuffer="";
      }else if(codeBuffer.length()>0){
        Serial.println("#");
        if(armed){
          String payload = "{\"action\":\"disarm\",\"code\":\""+codeBuffer+"\"}";
          client.publish(topicRequest,payload.c_str());
        }
        codeBuffer="";
      }
    } else { // Digit pressed
      codeBuffer += key;
      Serial.print("Code buffer: "); Serial.println(codeBuffer);
    }
  }
}

// ========================
// Alarm functions
// ========================
void armAlarm() {
  Serial.println("Début armement...");
  armingInProgress = true;
  armingStart = millis();
  lastBlinkTime = millis();
  blinkState = false;
}

void disarmAlarm(){
  Serial.println("Disarming...");
  if(armingInProgress) armingInProgress = false;
  strip.setPixelColor(0, strip.Color(0,127,0)); // green fixed
  strip.show();
  //digitalWrite(BUZZER_PIN,HIGH); delay(200); digitalWrite(BUZZER_PIN,LOW);
  tone(BUZZER_PIN, 4100); delay(400); noTone(BUZZER_PIN);
  armed = false;
}

void codeError(){
  Serial.println("Code incorrect!");
  for(int i=0;i<4;i++){
    strip.setPixelColor(0, strip.Color(0,0,127)); // bleu
    strip.show();
    //digitalWrite(BUZZER_PIN,HIGH);
    tone(BUZZER_PIN, 4100); 
    delay(codeErrorBlink);
    strip.setPixelColor(0, strip.Color(0,0,0)); // off
    strip.show();
    //digitalWrite(BUZZER_PIN,LOW);
    noTone(BUZZER_PIN);
    delay(codeErrorBlink);
  }
  // Restore previous state
  strip.setPixelColor(0, armed ? strip.Color(127,0,0) : strip.Color(0,127,0));
  strip.show();
}

void hwError(){
  Serial.println("Problème hw : sirène non connectée!");
  for(int i = 0; i < 5; i++) {
    // 4 bips courts
    for(int b = 0; b < 4; b++) {
      strip.setPixelColor(0, strip.Color(127, 50, 0));
      strip.show();
      tone(BUZZER_PIN, 4100);
      delay(hwErrorBlink);
      strip.setPixelColor(0, strip.Color(0, 0, 0));
      strip.show();
      noTone(BUZZER_PIN);
      delay(80);
    }
    // pause
    delay(400);
  }
  // Restore previous state
  strip.setPixelColor(0, armed ? strip.Color(127,0,0) : strip.Color(0,127,0));
  strip.show();
}

