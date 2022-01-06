#include <tinyfsm.hpp>
#include <AsyncMqttClient.h>
#include <PubSubClient.h>
#include "Esp32RingBuffer.h"
#include <set>

class StateMachine
: public tinyfsm::Fsm<StateMachine>
{
public:
  virtual void react(WifiDisconnectEvent const &) {
    transit<WifiDisconnected>();
  };
  virtual void react(WifiConnectEvent const &) {
    transit<WifiConnected>();
  };
  virtual void react(MQTTConnectedEvent const &) {
    transit<MQTTConnected>();    
  };
  virtual void react(MQTTDisconnectedEvent const &) {
    transit<MQTTDisconnected>();    
  };
  virtual void react(BeginPlayAudioEvent const &) {};
  virtual void react(EndPlayAudioEvent const &) {};
  virtual void react(StreamAudioEvent const &) {
    xEventGroupClearBits(audioGroup, PLAY);
    Serial.println("Send EndPlayAudioEvent in StreamAudioEvent");
    dispatch(EndPlayAudioEvent());
    xEventGroupSetBits(audioGroup, STREAM);
  };
  virtual void react(IdleEvent const &) {};
  virtual void react(ErrorEvent const &) {};
  virtual void react(TtsEvent const &) {};  
  virtual void react(UpdateEvent const &) {};
  virtual void react(PlayBytesEvent const &) {
    xEventGroupClearBits(audioGroup, STREAM);
    Serial.println("Send BeginPlayAudioEvent in PlayBytesEvent");
    dispatch(BeginPlayAudioEvent());
    xEventGroupSetBits(audioGroup, PLAY);
  };
  virtual void react(ListeningEvent const &){};
  virtual void react(UpdateConfigurationEvent const &)
  {
    criticalSection(wbSemaphore, []() {
      device->updateBrightness(current_colors != COLORS_IDLE ? config.hotword_brightness : config.brightness);
      device->updateColors(current_colors);
    });
  };

  virtual void entry(void) {  
    xEventGroupClearBits(audioGroup, PLAY);
    xEventGroupClearBits(audioGroup, STREAM);
    criticalSection(wbSemaphore, []() {
      device->updateBrightness(current_colors != COLORS_IDLE ? config.hotword_brightness : config.brightness);
      device->updateColors(current_colors);
    });
  }; 
  virtual void run(void) {}; 
  void         exit(void) {};
};

class Tts : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter Tts");
    current_colors = COLORS_TTS;
    StateMachine::entry();
  }

  void react(IdleEvent const &) override { 
    publishDebug("IdleEvent in Tts");
    transit<Idle>();
  }

  void react(ListeningEvent const &) override { 
    publishDebug("ListeningEvent in Tts");
    transit<Listening>();
  }

  void react(BeginPlayAudioEvent const &) override { 
    publishDebug("BeginPlayAudioEvent in Tts");
    transit<TtsPlay>();
  }
};

class TtsPlay : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter TtsPlay");
  }

  void react(EndPlayAudioEvent const &) override { 
    publishDebug("EndPlayAudioEvent in TtsPlay");
    transit<Tts>();
  }
};

class Updating : public StateMachine
{
  void entry(void) override {
    Serial.println("Enter Updating");
    current_colors = COLORS_OTA;
    StateMachine::entry();
  }
};

class Listening : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter Listening");
    current_colors = COLORS_HOTWORD;
    StateMachine::entry();
    xEventGroupSetBits(audioGroup, STREAM);
  }

  void react(IdleEvent const &) override { 
    publishDebug("IdleEvent in Listening");
    transit<Idle>();
  }

  void react(TtsEvent const &) override { 
    transit<Tts>();
  }

  void react(BeginPlayAudioEvent const &) override { 
    publishDebug("BeginPlayAudioEvent in Listening");
    transit<ListeningPlay>();
  }
};

class ListeningPlay : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter ListeningPlay");
  }

  void react(EndPlayAudioEvent const &) override { 
    publishDebug("EndPlayAudioEvent in ListeningPlay");
    transit<Listening>();
  }
};

class Idle : public StateMachine
{
  bool hotwordDetected = false;

  void entry(void) override {
    publishDebug("Enter Idle");
    hotwordDetected = false;
    current_colors = COLORS_IDLE;
    StateMachine::entry();
    if (config.hotword_detection == HW_REMOTE)
    {
      // start streaming audio to rhasspy for remote
      // hotword detection
      xEventGroupSetBits(audioGroup, STREAM);
    }
    else 
    {
      // stop streaming audio to rhasspy if it
      // was active before
      xEventGroupClearBits(audioGroup, STREAM);
    }
  }

  void run(void) override {
    if (device->isHotwordDetected() && !hotwordDetected) {
      hotwordDetected = true;
      //start session by publishing a message to hermes/dialogueManager/startSession
      std::string message = "{\"init\":{\"type\":\"action\",\"canBeEnqueued\": false},\"siteId\":\"" + std::string(config.siteid) + "\"}";
      asyncClient.publish("hermes/dialogueManager/startSession", 0, false, message.c_str());
    }
    if (configChanged) {
      configChanged = false;
      transit<Idle>();
    }
  }

  void react(ListeningEvent const &) override { 
    transit<Listening>();
  }

  void react(BeginPlayAudioEvent const &) override { 
    publishDebug("BeginPlayAudioEvent in Idle");
    transit<IdlePlay>();
  }

  void react(TtsEvent const &) override { 
    transit<Tts>();
  }

  void react(ErrorEvent const &) override { 
    transit<Error>();
  }
  
  void react(UpdateEvent const &) override { 
    transit<Updating>();
  }
};

class IdlePlay : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter IdlePlay");
  }

  void react(EndPlayAudioEvent const &) override { 
    publishDebug("EndPlayAudioEvent in IdlePlay");
    transit<Idle>();
  }
};

class Error : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter Error");
    current_colors = COLORS_ERROR;
    StateMachine::entry();
  }

  void react(IdleEvent const &) override { 
    publishDebug("IdleEvent in Error");
    transit<Idle>();
  }

  void react(BeginPlayAudioEvent const &) override { 
    publishDebug("BeginPlayAudioEvent in Error");
    transit<ErrorPlay>();
  }
};

class ErrorPlay : public StateMachine
{
  void entry(void) override {
    publishDebug("Enter ErrorPlay");
  }

  void react(EndPlayAudioEvent const &) override { 
    publishDebug("EndPlayAudioEvent in ErrorPlay");
    transit<Error>();
  }
  
  void react(UpdateEvent const &) override { 
    transit<Updating>();
  }
};

class MQTTConnected : public StateMachine {
  void entry(void) override {
    Serial.println("Enter MQTTConnected");
    Serial.printf("Connected as %s\r\n",config.siteid.c_str());
    publishDebug("Connected to asynch MQTT!");
    asyncClient.subscribe(playBytesTopic.c_str(), 0);
    asyncClient.subscribe(hotwordTopic.c_str(), 0);
    asyncClient.subscribe(audioTopic.c_str(), 0);
    //asyncClient.subscribe(debugTopic.c_str(), 0);
    asyncClient.subscribe(ledTopic.c_str(), 0);
    asyncClient.subscribe(restartTopic.c_str(), 0);
    asyncClient.subscribe(sayTopic.c_str(), 0);
    asyncClient.subscribe(sayFinishedTopic.c_str(), 0);
    asyncClient.subscribe(errorTopic.c_str(), 0);
    transit<Idle>();
  }
};

class MQTTDisconnected : public StateMachine {

  private:
  long currentMillis, startMillis;

  void entry(void) override
  {
    Serial.println("Enter MQTTDisconnected");
    // no longer connected to MQTT, so device is no longer working nominally
    // => indicate this
    current_colors = COLORS_WIFI_CONNECTED;
    
    criticalSection(wbSemaphore, []() { device->updateColors(current_colors); });

    startMillis = millis();
    currentMillis = millis();

    criticalSection(audioServerSemaphore, []() {
      if (audioServer.connected()) {
        audioServer.disconnect();
      }

      if (asyncClient.connected()) {
        asyncClient.disconnect();
      }
      if (!mqttInitialized) {
        asyncClient.onMessage(onMqttMessage);
        mqttInitialized = true;
      }
   
      Serial.printf("Connecting MQTT: %s, %d\r\n", config.mqtt_host.c_str(), config.mqtt_port);
      asyncClient.setClientId(config.siteid.c_str());
      asyncClient.setServer(config.mqtt_host.c_str(), config.mqtt_port);
      asyncClient.setCredentials(config.mqtt_user.c_str(), config.mqtt_pass.c_str());
      asyncClient.connect();
      audioServer.setServer(config.mqtt_host.c_str(), MQTT_TLS ? 8883 : config.mqtt_port);
      audioServer.connect((config.siteid + "Audio").c_str(), config.mqtt_user.c_str(), config.mqtt_pass.c_str());
    });

    // give asyncServer some time to connect before proceeding
    vTaskDelay(100);
  }

  void run(void) override {
    bool audioServer_connected = false;
    criticalSection(audioServerSemaphore, [&]() {
      audioServer_connected = audioServer.connected();
    });

    if (audioServer_connected && asyncClient.connected()) {
      transit<MQTTConnected>();
    } else {
      currentMillis = millis();
      if (currentMillis - startMillis > 5000) {
        Serial.println("Connect failed, retry");
        Serial.printf("Audio connected: %d, Async connected: %d\r\n", audioServer_connected, asyncClient.connected());
        transit<MQTTDisconnected>();
      }     
    }
  }
  
  void react(MQTTDisconnectedEvent const &) {
    // do nothing, no point in going to MQTTDisconnected if already in this state    
  };

};

class WifiConnected : public StateMachine
{
  void entry(void) override {
    Serial.println("Enter WifiConnected");
    #if NETWORK_TYPE == NETWORK_ETHERNET
        Serial.printf("Connected to LAN with IP: %s, \n", ETH.localIP().toString().c_str());
    #else
        Serial.printf("Connected to Wifi with IP: %s, SSID: %s, BSSID: %s, RSSI: %d\r\n", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(), WiFi.RSSI());
    #endif
    xEventGroupClearBits(audioGroup, PLAY);
    xEventGroupClearBits(audioGroup, STREAM);
    device->updateBrightness(config.brightness);
    device->updateColors(COLORS_WIFI_CONNECTED);
    ArduinoOTA.begin();
    transit<MQTTDisconnected>();
  }

  void react(MQTTDisconnectedEvent const &) {
    // do nothing, no point in going to MQTTDisconnected if we have not finished WifiConnected code    
  };

};

class WifiDisconnected : public StateMachine
{
  void entry(void) override {
    if (!audioGroup) {
      audioGroup = xEventGroupCreate();
    }
    //Mute initial output
    device->muteOutput(true);
    xEventGroupClearBits(audioGroup, STREAM);
    xEventGroupClearBits(audioGroup, PLAY);
    if (i2sHandle == NULL) {
      Serial.println("Creating I2Stask");
      xTaskCreatePinnedToCore(I2Stask, "I2Stask", 8192, NULL, 3, &i2sHandle, 1);
    } else {  
      Serial.println("We already have a I2Stask");
    }
    Serial.println("Enter WifiDisconnected");

    #if NETWORK_TYPE == NETWORK_ETHERNET
      WiFi.onEvent(WiFiEvent);
      ETH.begin();
    #else
      device->updateBrightness(config.brightness);
      device->updateColors(COLORS_WIFI_DISCONNECTED);
      
      // Set static ip address
      #if defined(HOST_IP) && defined(HOST_GATEWAY)  && defined(HOST_SUBNET)  && defined(HOST_DNS1)
        IPAddress ip;
        IPAddress gateway;
        IPAddress subnet;
        IPAddress dns1;
        IPAddress dns2;

        ip.fromString(HOST_IP);
        gateway.fromString(HOST_GATEWAY);
        subnet.fromString(HOST_SUBNET);
        dns1.fromString(HOST_DNS1);

        #ifdef HOST_DNS2
          dns2.fromString(HOST_DNS2);
        #endif

        Serial.printf("Set static ip: %s, gateway: %s, subnet: %s, dns1: %s, dns2: %s\r\n", ip.toString().c_str(), gateway.toString().c_str(), subnet.toString().c_str(), dns1.toString().c_str(), dns2.toString().c_str());
        WiFi.config(ip, gateway, subnet, dns1, dns2);
      #endif

      WiFi.onEvent(WiFiEvent);
      WiFi.mode(WIFI_STA);

      // find best AP (BSSID) if there are several AP for a given SSID
      // https://github.com/arendst/Tasmota/blob/db615c5b0ba0053c3991cf40dd47b0d484ac77ae/tasmota/support_wifi.ino#L261
      // https://esp32.com/viewtopic.php?t=18979
      #if defined(SCAN_STRONGEST_AP)
        Serial.println("WiFi scan start");
        int n = WiFi.scanNetworks(); // WiFi.scanNetworks will return the number of networks found
        // or WIFI_SCAN_RUNNING   (-1), WIFI_SCAN_FAILED    (-2)

        Serial.printf("WiFi scan done, result %d\r\n", n);
        if (n <= 0) {
          Serial.println("error or no networks found");
        } else {
          for (int i = 0; i < n; ++i) {
            // Print metrics for each network found
            Serial.printf("%d: BSSID: %s  %ddBm, %d%% %s, %s (%d)\r\n", i + 1, WiFi.BSSIDstr(i).c_str(), WiFi.RSSI(i), constrain(2 * (WiFi.RSSI(i) + 100), 0, 100),
              (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open     " : "encrypted", WiFi.SSID(i).c_str(), WiFi.channel(i));
          }
        }
        Serial.println();

        // find first that matches SSID. Expect results to be sorted by signal strength.
        int i = 0;
        while ( String(WIFI_SSID) != String(WiFi.SSID(i)) && (i < n)) {
          i++;
        }

        if (i == n || n < 0) {
          Serial.println("No network with SSID " WIFI_SSID " found!");
          WiFi.begin(WIFI_SSID, WIFI_PASS); // try basic method anyway
        } else {
          Serial.printf("SSID match found at index: %d\r\n", i + 1);
          WiFi.begin(WIFI_SSID, WIFI_PASS, 0, WiFi.BSSID(i)); // pass selected BSSID
        }
      #else
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      #endif

      while (WiFi.waitForConnectResult() != WL_CONNECTED) {
          retryCount++;
          if (retryCount > 2) {
              Serial.println("Connection Failed! Rebooting...");
              ESP.restart();
          } else {
              Serial.println("Connection Failed! Retry...");
          }
      }
    #endif
  }
  
  void react(MQTTDisconnectedEvent const &) {
    // do nothing, no point in going to MQTTDisconnected if we have no Wifi    
  };

};

FSM_INITIAL_STATE(StateMachine, WifiDisconnected)

using fsm = tinyfsm::Fsm<StateMachine>;

void send_event(events::Events event)
{
  if (xSemaphoreTake(eventListSemaphore, portMAX_DELAY)) {
    if (std::find(fsm_event.begin(), fsm_event.end(), event) == fsm_event.end()) {
      fsm_event.push_back(event);
    }
    xSemaphoreGive(eventListSemaphore);
  } else {
    // TODO: Decide what happens if the look cannot be acquirred...
  }
}

template<typename E>
bool handle_event(E const & event)
{
  fsm::template dispatch<E>(event);
  return true;
}

std::vector<std::string> explode( const std::string &delimiter, const std::string &str)
{
    std::vector<std::string> arr;
 
    int strleng = str.length();
    int delleng = delimiter.length();
    if (delleng==0)
        return arr;//no change
 
    int i=0;
    int k=0;
    while( i<strleng )
    {
        int j=0;
        while (i+j<strleng && j<delleng && str[i+j]==delimiter[j])
            j++;
        if (j==delleng)//found delimiter
        {
            arr.push_back(  str.substr(k, i-k) );
            i+=delleng;
            k=i;
        }
        else
        {
            i++;
        }
    }
    arr.push_back(  str.substr(k, i-k) );
    return arr;
}

void push_i2s_data(const uint8_t *const payload, size_t len)
{
  while (audioData.push((uint8_t *)payload, len) == false)
  {
    // getting in here indicates a completely filled buffer, as this is the only 
    // reason why push can fail unless len is larger than max buffer size
    // this case is not handled and must be avoided, hence we put an assert in
    assert(len < audioData.maxSize());
    do
    {
      if ((xEventGroupGetBits(audioGroup) & PLAY) != PLAY)
      {
        publishDebug("Send PlayBytesEvent");
        send_event(events::PlayBytesEvent);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    } while (audioData.isFull());
  }
}

void handle_playBytes(const std::string& topicstr, uint8_t *payload, size_t len, size_t index, size_t total)
{
  size_t offset = 0;

  // start of message
  if (index == 0)
  {
    message_size = total;
    audioData.clear();
    XT_Wav_Class Message((const uint8_t *)payload);
    
    sampleRate = Message.SampleRate;
    numChannels = Message.NumChannels;
    bitDepth = Message.BitsPerSample;
    offset = Message.DataStart;

    char message[100];
    snprintf(message, 100, "Samplerate: %d, Channels: %d, Format: %d, Bits per Sample: %d, Start: %d", sampleRate, numChannels, (int)Message.Format, bitDepth, offset);
    publishDebug(message);
    queueDelay = (sampleRate * numChannels * bitDepth) / 1000;
  }

  push_i2s_data((uint8_t *)&payload[offset], len - offset);

  // enf of message 
  if (len + index == total)
  {    
    //At the end, make sure to start play in case the buffer is not full yet
    if (!audioData.isEmpty() && (xEventGroupGetBits(audioGroup) & PLAY) != PLAY)
    {
      publishDebug("Send PlayBytesEvent");
      send_event(events::PlayBytesEvent);
    }

    std::vector<std::string> topicparts = explode("/", topicstr);
    finishedMsg = "{\"id\":\"" + topicparts[4] + "\",\"siteId\":\"" + config.siteid + "\",\"sessionId\":null}";
  }
}



struct OnMqttMessageTopicAction
{
  std::string& topicId; // which topic shall be handled, use   .topicId = *(new std::string("errorTopic")) to initialize with string constant
  void (*action)(JsonObject& root, const std::string& topic); // pointer to function / lambda function to handle topics message
};

OnMqttMessageTopicAction onTopic[] =
{
  {
  .topicId = errorTopic,
  .action = [](JsonObject& root, const std::string& topic) {
      if (root["siteId"] == config.siteid.c_str()) {
        Serial.println("Send ErrorEvent from errorTopic");
        send_event(events::ErrorEvent);
      }
    }
  },
  {
  .topicId = sayFinishedTopic,
  .action = [](JsonObject& root, const std::string& topic) {
      if (root["siteId"] == config.siteid.c_str()) {
        Serial.println("Send ListeningEvent or IdleEvent from sayFinishedTopic");
        send_event((xEventGroupGetBits(audioGroup) & HOTWORD) == ASR ? events::ListeningEvent : events::IdleEvent);
      }
    }
  },
  {
  .topicId = sayTopic,
  .action = [](JsonObject& root, const std::string& topic) {
      if (root["siteId"] == config.siteid.c_str()) {
        Serial.println("Send TtsEvent from sayTopic");
        send_event(events::TtsEvent);
      }
    }
  },
  {
    .topicId = *(new std::string("toggleOff")),
    .action = [](JsonObject& root, const std::string& topic) {
      if (root["siteId"] == config.siteid.c_str() && root.containsKey("reason")) {
        xEventGroupClearBits(audioGroup, topic == "hermes/hotword/toggleOff" ? HOTWORD : ASR);
        if (root["reason"] == "dialogueSession") {
            Serial.println("Send ListeningEvent from toggleOff (dialogueSession)");
            send_event(events::ListeningEvent);
        }
        if (root["reason"] == "ttsSay") {
            Serial.println("Send TtsEvent from toggleOff (ttsSay)");
            send_event(events::TtsEvent);
        }
        if (root["reason"] == "playAudio") {
            Serial.println("Send ListeningEvent from toggleOff (playAudio)");
            send_event(events::ListeningEvent);
        }
      }
    }
  },
  {
    .topicId = *(new std::string("toggleOn")),
    .action = [](JsonObject& root, const std::string& topic) {
      #if 1
        static const std::set<std::string> reasons { "dialogueSession", "ttsSay", "playAudio" };
        
        auto reason = root.containsKey("reason") ? reasons.find(root["reason"].as<const char *>()) : reasons.end();
        if (root["siteId"] == config.siteid.c_str() && reason != reasons.end())
        {
          xEventGroupSetBits(audioGroup, topic == "hermes/hotword/toggleOn" ? HOTWORD : ASR);
          Serial.printf("Send IdleEvent from toggleOn (%s)\n", reason->c_str());
          send_event(events::IdleEvent);
        }
        #endif
#if 0
        if (root["siteId"] == config.siteid.c_str() && root.containsKey("reason")) {
          if (root["reason"] == "dialogueSession") {
              Serial.println("Send IdleEvent from toggleOn (dialogueSession)");
              send_event(events::IdleEvent);
          }
          if (root["reason"] == "ttsSay") {
              Serial.println("Send IdleEvent from toggleOn (ttsSay)");
              send_event(events::IdleEvent);
          }
          if (root["reason"] == "playAudio") {
              Serial.println("Send IdleEvent from toggleOn (playAudio)");
              send_event(events::IdleEvent);
          }
        }
#endif      
      }
  },
  {
    .topicId = ledTopic,
    .action = [](JsonObject& root, const std::string& topic) {
        bool saveNeeded = false;

        if (root.containsKey("animation")) {
          config.animation = (uint16_t)(root["animation"]);
          saveNeeded = true;
        }
        if (root.containsKey("brightness")) {
          if (config.brightness != (int)root["brightness"]) {
            config.brightness = (int)(root["brightness"]);
            saveNeeded = true;
          }
        }
        if (root.containsKey("hotword_brightness")) {
          config.hotword_brightness = (int)(root["hotword_brightness"]);
        }
        if (root.containsKey("hotword")) {
          ColorMap[COLORS_HOTWORD][0] = root["hotword"][0];
          ColorMap[COLORS_HOTWORD][1] = root["hotword"][1];
          ColorMap[COLORS_HOTWORD][2] = root["hotword"][2];
          ColorMap[COLORS_HOTWORD][3] = root["hotword"][3];
        }
        if (root.containsKey("tts")) {
          ColorMap[COLORS_TTS][0] = root["tts"][0];
          ColorMap[COLORS_TTS][1] = root["tts"][1];
          ColorMap[COLORS_TTS][2] = root["tts"][2];
          ColorMap[COLORS_TTS][3] = root["tts"][3];
        }
        if (root.containsKey("idle")) {
          ColorMap[COLORS_IDLE][0] = root["idle"][0];
          ColorMap[COLORS_IDLE][1] = root["idle"][1];
          ColorMap[COLORS_IDLE][2] = root["idle"][2];
          ColorMap[COLORS_IDLE][3] = root["idle"][3];
        }
        if (root.containsKey("wifi_disconnect")) {
          ColorMap[COLORS_WIFI_DISCONNECTED][0] = root["wifi_disconnect"][0];
          ColorMap[COLORS_WIFI_DISCONNECTED][1] = root["wifi_disconnect"][1];
          ColorMap[COLORS_WIFI_DISCONNECTED][2] = root["wifi_disconnect"][2];
          ColorMap[COLORS_WIFI_DISCONNECTED][3] = root["wifi_disconnect"][3];
        }
        if (root.containsKey("wifi_connect")) {
          ColorMap[COLORS_WIFI_CONNECTED][0] = root["wifi_connect"][0];
          ColorMap[COLORS_WIFI_CONNECTED][1] = root["wifi_connect"][1];
          ColorMap[COLORS_WIFI_CONNECTED][2] = root["wifi_connect"][2];
          ColorMap[COLORS_WIFI_CONNECTED][3] = root["wifi_connect"][3];
        }
        if (root.containsKey("update")) {
          ColorMap[COLORS_OTA][0] = root["update"][0];
          ColorMap[COLORS_OTA][1] = root["update"][1];
          ColorMap[COLORS_OTA][2] = root["update"][2];
          ColorMap[COLORS_OTA][3] = root["update"][3];
        }
        if (root.containsKey("error")) {
          ColorMap[COLORS_ERROR][0] = root["error"][0];
          ColorMap[COLORS_ERROR][1] = root["error"][1];
          ColorMap[COLORS_ERROR][2] = root["error"][2];
          ColorMap[COLORS_ERROR][3] = root["error"][3];
        }
        if (saveNeeded) {
          saveConfiguration(configfile, config);
        }
        send_event(events::UpdateConfigurationEvent);
    }
  },
  {
    .topicId = audioTopic,
    .action = [](JsonObject& root, const std::string& topic) {
        if (root.containsKey("mute_input")) {
          config.mute_input = (root["mute_input"] == "true") ? true : false;
        }
        if (root.containsKey("mute_output")) {
          config.mute_output = (root["mute_output"] == "true") ? true : false;
        }
        if (root.containsKey("amp_output")) {
            config.amp_output =  (root["amp_output"] == "0") ? AMP_OUT_SPEAKERS : AMP_OUT_HEADPHONE;
        }
        if (root.containsKey("gain")) {
          config.gain = (int)root["gain"];
        }
        if (root.containsKey("volume")) {
          config.volume = (uint16_t)root["volume"];
        }
        if (root.containsKey("hotword")) {
          config.hotword_detection = (root["hotword"] == "local") ? HW_LOCAL : HW_REMOTE;
        }
        saveConfiguration(configfile, config);
    }
  },
  {
    .topicId = restartTopic,
    .action = [](JsonObject& root, const std::string& topic) {
      if (root.containsKey("passwordhash")) {
        if (root["passwordhash"] == OTA_PASS_HASH) {
          ESP.restart();
        }
      }
    }
  },
  {
    .topicId = debugTopic,
    .action = [](JsonObject& root, const std::string& topic) {
        if (root.containsKey("debug")) {
          DEBUG = (root["debug"] == "true") ? true : false;
        }
    }
  },
};

void onMqttMessage(char *topic, char *payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
  const std::string topicstr(topic);

  // complete or end of message has been received
  if (len + index == total) {
    bool isHandled = false;

    // handle JSON messages
    for(auto topicHandler: onTopic) {
      isHandled = topicstr.find(topicHandler.topicId) != std::string::npos; 
      if (isHandled) {
        std::string payloadstr(payload);
        StaticJsonDocument<300> doc;
        DeserializationError err = deserializeJson(doc, payloadstr.c_str());
        if (!err) {
          JsonObject root = doc.as<JsonObject>();
          topicHandler.action(root, topicstr);
        } else {
          publishDebug(err.c_str());
        }
        // we are done with this loop after first match, leave it
        break;
      }
    }

    if (isHandled == false) {
      if (topicstr.find("playBytes") != std::string::npos) {
        handle_playBytes(topicstr, (uint8_t*)payload, len, index, total);
      } else {
        char message[100];
        snprintf(message, 100, "Unhandled message received, topic '%s'", topic);
        publishDebug(message);
      }
    }
  } else {
    // len + index < total ==> partial message
    if (topicstr.find("playBytes") != std::string::npos) {
      handle_playBytes(topicstr, (uint8_t*)payload, len, index, total);
    } else {
      char message[100];
      snprintf(message, 100, "Unhandled partial message received, topic '%s'", topic);
      publishDebug(message);
    }
  }
}

void I2Stask(void *p)
{
  while (1) {
    if ((xEventGroupGetBits(audioGroup) & PLAY) == PLAY) {
      size_t bytes_written;
      boolean timeout = false;
      int played = 44;

      xSemaphoreTake(wbSemaphore, portMAX_DELAY);
      device->setWriteMode(sampleRate, bitDepth, numChannels);
      xSemaphoreGive(wbSemaphore);

      while (played < message_size && timeout == false) {
        xSemaphoreTake(wbSemaphore, portMAX_DELAY);
        device->animate(current_colors, config.animation);
        xSemaphoreGive(wbSemaphore);
        int bytes_to_write = device->writeSize;
        if (message_size - played < device->writeSize) {
          bytes_to_write = message_size - played;
        }
        uint16_t data[bytes_to_write / 2];
        if (!timeout) {
          for (int i = 0; i < bytes_to_write / 2; i++) {
            if (!audioData.pop(data[i])) {
              char message[100];
              snprintf(message, 100, "Buffer underflow %d %ld", played + i, message_size);
              publishDebug(message);
              vTaskDelay(60);
              bytes_to_write = (i)*2;
            }
          }
          played = played + bytes_to_write;
          if (!config.mute_output) {
            xSemaphoreTake(wbSemaphore, portMAX_DELAY);
            device->muteOutput(false);
            device->writeAudio((uint8_t *)data, bytes_to_write, &bytes_written);
            xSemaphoreGive(wbSemaphore);
          } else {
            bytes_written = bytes_to_write;
          }
          if (bytes_written != bytes_to_write) {
            char message[100];
            snprintf(message, 100, "Bytes to write %d, but bytes written %d", bytes_to_write, bytes_written);
            publishDebug(message);
          }
        }
      }
      asyncClient.publish(playFinishedTopic.c_str(), 0, false, finishedMsg.c_str());
      xSemaphoreTake(wbSemaphore, portMAX_DELAY);
      device->muteOutput(true);
      xSemaphoreGive(wbSemaphore);
      audioData.clear();

      publishDebug("Done");
      publishDebug("Send StreamAudioEvent");
      send_event(events::StreamAudioEvent);
    }

    if ((xEventGroupGetBits(audioGroup) & STREAM) == STREAM && !config.mute_input) {
      xSemaphoreTake(wbSemaphore, portMAX_DELAY);
      device->setReadMode();
      xSemaphoreGive(wbSemaphore);

      uint8_t data[device->readSize * device->width];

      if (xSemaphoreTake(audioServerSemaphore, portMAX_DELAY) && audioServer.connected()) {
        xSemaphoreTake(wbSemaphore, portMAX_DELAY);
        if (device->readAudio(data, device->readSize * device->width)) {
          // Rhasspy needs an audiofeed of 512 bytes+header per message
          // Some devices, like the Matrix Voice do 512 16 bit read in one mic read
          // This is 1024 bytes, so two message are needed in that case
          const int messageBytes = 512;
          uint8_t payload[sizeof(header) + messageBytes];
          const int message_count = sizeof(data) / messageBytes;
          for (int i = 0; i < message_count; i++) {
            memcpy(payload, &header, sizeof(header));
            memcpy(&payload[sizeof(header)], &data[messageBytes * i], messageBytes);
            audioServer.publish(audioFrameTopic.c_str(), payload, sizeof(payload));
          }
        }
        xSemaphoreGive(wbSemaphore);
        xSemaphoreGive(audioServerSemaphore);
      }
    }
    // keep the audioServer connection alive, also used to
    // monitor the MQTT Server, in order to detect
    // disconnection events
    if (fsm::is_in_state<WifiConnected>() == false && fsm::is_in_state<WifiDisconnected>() == false &&
        fsm::is_in_state<MQTTDisconnected>() == false && xSemaphoreTake(audioServerSemaphore, portMAX_DELAY)) {
      if (audioServer.connected()) {
        // Loop, because otherwise this causes timeouts
        audioServer.loop();
      } else {
        // if we are not already in MQTTDisconnected state, try to get there
        // this does not affect WifiDisconnected / WifiConnected, as these
        // ignore our requests in order to establish Wifi connection before
        // MQTT connection can be established
        xEventGroupClearBits(audioGroup, STREAM | PLAY);
        send_event(events::MQTTDisconnectedEvent);
      }
      xSemaphoreGive(audioServerSemaphore);
    }
    // Added for stability when neither PLAY or STREAM is set.
    vTaskDelay(10);
  }
  vTaskDelete(NULL);
}

void WiFiEvent(WiFiEvent_t event) {
    switch (event) {

      #if NETWORK_TYPE == NETWORK_ETHERNET
        case SYSTEM_EVENT_ETH_START:
          Serial.println("ETH Started");
          //set eth hostname here
          ETH.setHostname(HOSTNAME);
          break;
        case SYSTEM_EVENT_ETH_CONNECTED:
          Serial.println("ETH Connected");
          break;
        case SYSTEM_EVENT_ETH_GOT_IP:
          Serial.print("ETH MAC: ");
          Serial.print(ETH.macAddress());
          Serial.print(", IPv4: ");
          Serial.print(ETH.localIP());
          if (ETH.fullDuplex()) {
            Serial.print(", FULL_DUPLEX");
          }
          Serial.print(", ");
          Serial.print(ETH.linkSpeed());
          Serial.println("Mbps");
            send_event(WifiConnectEvent());
          break;
        case SYSTEM_EVENT_ETH_DISCONNECTED:
          Serial.println("ETH Disconnected");
           send_event(WifiDisconnectEvent());
          break;
        case SYSTEM_EVENT_ETH_STOP:
          Serial.println("ETH Stopped");
          send_event(WifiDisconnectEvent());
          break;
        default:
          Serial.println("ETH Event");
          break;
      #else
        case SYSTEM_EVENT_STA_START:
            WiFi.setHostname(HOSTNAME);
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            send_event(events::WifiConnectEvent);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            send_event(events::WifiDisconnectEvent);
            break;
        default:
            break;
      #endif
    }
}
