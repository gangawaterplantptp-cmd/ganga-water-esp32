// Ganga Water Machine ESP32 v1.2
// Based on your original sketch with stability improvements.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#define RELAY_PIN 22

const char* WIFI_NAME = "chaitra 4g";
const char* WIFI_PASSWORD = "chaitra123";

const char* WORKER_URL =
"https://small-mud-0ce7.gagawaterplantptp.workers.dev";

const unsigned long POLL_INTERVAL = 3000;
const unsigned long HEARTBEAT_INTERVAL = 30000;

unsigned long lastPollTime=0;
unsigned long lastHeartbeatTime=0;

bool machineBusy=false;

void relayOn(){ pinMode(RELAY_PIN,OUTPUT); digitalWrite(RELAY_PIN,LOW); }
void relayOff(){ pinMode(RELAY_PIN,INPUT); }

void sendOnePulse(){ relayOn(); delay(80); relayOff(); delay(120); }

void dispenseAmount(int amount){
  Serial.printf("Dispensing %d pulses\n",amount);
  for(int i=0;i<amount;i++) sendOnePulse();
  Serial.println("All pulses completed");
}

void connectWiFi(){
  if(WiFi.status()==WL_CONNECTED) return;
  Serial.println("Connecting WiFi...");
  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_NAME,WIFI_PASSWORD);

  int c=0;
  while(WiFi.status()!=WL_CONNECTED && c<40){
    delay(500);
    Serial.print(".");
    c++;
  }
  Serial.println();

  if(WiFi.status()==WL_CONNECTED){
    Serial.print("Connected IP: ");
    Serial.println(WiFi.localIP());
  }else{
    Serial.println("WiFi connection failed");
  }
}

bool postJson(String endpoint,String body){
  if(WiFi.status()!=WL_CONNECTED){
    connectWiFi();
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(10000);

  if(!https.begin(client,String(WORKER_URL)+endpoint)){
    Serial.println("HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type","application/json");

  int code=https.POST(body);

  if(code==200){
    if(endpoint=="/heartbeat") Serial.println("Heartbeat sent");
    https.end();
    return true;
  }

  Serial.printf("%s HTTP error: %d\n",endpoint.c_str(),code);
  https.end();

  if(code==-1 || code==-11){
    Serial.println("Recovering WiFi...");
    WiFi.disconnect(true);
    delay(1000);
    connectWiFi();
  }

  return false;
}

void sendHeartbeat(){
  postJson("/heartbeat","{\"machine_id\":\"ganga01\"}");
}

bool commandComplete(){
  return postJson("/command-complete","{}");
}

void checkForPayment(){

  if(WiFi.status()!=WL_CONNECTED || machineBusy) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(10000);

  if(!https.begin(client,String(WORKER_URL)+"/check-command")){
    Serial.println("Command connection failed");
    return;
  }

  int code=https.GET();

  if(code!=200){
    Serial.printf("Command HTTP error: %d\n",code);
    https.end();
    if(code==-1 || code==-11){
      WiFi.disconnect(true);
      delay(1000);
      connectWiFi();
    }
    return;
  }

  String response=https.getString();
  https.end();

  if(response.indexOf("\"command\":\"NONE\"")>=0){
    Serial.println("No pending payment");
    return;
  }

  int amount=0;

  if(response.indexOf("PULSE_5")>=0) amount=5;
  else if(response.indexOf("PULSE_10")>=0) amount=10;
  else if(response.indexOf("PULSE_15")>=0) amount=15;
  else{
    Serial.println(response);
    return;
  }

  machineBusy=true;

  dispenseAmount(amount);

  for(int i=0;i<3;i++){
    if(commandComplete()){
      Serial.println("Payment command completed");
      break;
    }
    Serial.println("Completion retry...");
    delay(2000);
  }

  machineBusy=false;
}

void setup(){

  Serial.begin(115200);
  delay(1000);

  relayOff();

  Serial.println("Ganga Water Machine Starting");

  connectWiFi();

  delay(1000);

  sendHeartbeat();

  lastHeartbeatTime=millis();
  lastPollTime=millis();
}

void loop(){

  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("WiFi Lost");
    connectWiFi();
    delay(3000);
    return;
  }

  unsigned long now=millis();

  if(now-lastPollTime>=POLL_INTERVAL){
    lastPollTime=now;
    checkForPayment();
  }

  if(now-lastHeartbeatTime>=HEARTBEAT_INTERVAL){
    lastHeartbeatTime=now;
    sendHeartbeat();
  }
}