/*该代码为ESP32模块上云，使用arduino开发。云平台为阿里云*/
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <mbedtls/md.h>
#include <time.h>

/* 网络配置 */
#define WIFI_SSID         "替换为你的Wi-Fi"
#define WIFI_PASSWD       "替换为你的密码"

/* 设备三元组信息 以阿里云平台为例 */
#define PRODUCT_KEY       "Your ProductKey"
#define DEVICE_NAME       "Your DeviceName"
#define DEVICE_SECRET     "Your DeviceSecret"

/* MQTT服务器配置 */
#define MQTT_SERVER       "Your ProductKey.iot-as-mqtt.cn-shanghai.aliyuncs.com"
#define MQTT_PORT         1883
#define MQTT_KEEPALIVE    60

/* 数据上传间隔（毫秒） */
#define UPLOAD_INTERVAL   5000

// 引脚定义
#define CC2530_RX_PIN 16  // ESP32-S3 UART2 RX ← CC2530 TX
#define CC2530_TX_PIN 17  // ESP32-S3 UART2 TX → CC2530 RX

// 修正的宏定义主题格式 - 根据您提供的正确格式
// 更新宏定义中的字段名，去掉空格
#define ALINK_BODY_FORMAT_A "{\"id\":%lu,\"params\":{\"Temp1\":%.0f,\"Humi1\":%.0f},\"version\":\"1.0\",\"method\":\"thing.event.property.post\"}"
#define ALINK_BODY_FORMAT_B "{\"id\":%lu,\"params\":{\"Temp2\":%.0f,\"Humi2\":%.0f},\"version\":\"1.0\",\"method\":\"thing.event.property.post\"}"
#define ALINK_TOPIC_PROP_POST "/sys/" PRODUCT_KEY "/" DEVICE_NAME "/thing/event/property/post"

// 全局变量
char CLIENT_ID[256];
char MQTT_USERNAME[100];
char MQTT_PASSWORD[100];

WiFiClient espClient;
PubSubClient client(espClient);

struct SensorData {
    float temperature;
    float humidity;
    bool valid;
    unsigned long timestamp;
    unsigned long lastUploadTime;  // 独立的上传时间
    char node;
};

SensorData nodeA = {0, 0, false, 0, 'A'};
SensorData nodeB = {0, 0, false, 0, 'B'};
String serialBuffer = "";
unsigned long lastUploadTime = 0;
bool wifiConnected = false;
bool timeSynced = false;

// 串口调试统计
unsigned long serialBytesReceived = 0;
unsigned long serialMessagesReceived = 0;
unsigned long lastSerialStats = 0;

// 函数声明
bool syncTime();
bool generateMqttConfig();
void hmacSha256(const char* key, const char* input, char* output);
void wifiInit();
void mqttCheckConnect();
void readSerialData();
bool parseSensorData(const String& data);
void mqttPostData();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void checkNetworkStatus();
void printSerialStats();
void printHexData(const String& data);
void checkSerialHealth();  // 改为健康检查，不发送数据

// 改进的时间同步函数
bool syncTime() {
  Serial.println("Syncing time via NTP...");
  
  configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "time.nist.gov");
  
  struct tm timeinfo;
  unsigned long startTime = millis();
  
  while (!getLocalTime(&timeinfo)) {
    if (millis() - startTime > 20000) {
      Serial.println("Time sync failed! Trying alternative NTP servers...");
      
      // 尝试备用NTP服务器
      configTime(8 * 3600, 0, "pool.ntp.org", "time.windows.com");
      startTime = millis();
      
      while (!getLocalTime(&timeinfo)) {
        if (millis() - startTime > 15000) {
          Serial.println("All time sync attempts failed!");
          return false;
        }
        delay(500);
        Serial.print(".");
      }
      break;
    }
    delay(500);
    Serial.print(".");
  }
  
  timeSynced = true;
  Serial.println("\nTime synced successfully!");
  
  // 打印当前时间
  if (getLocalTime(&timeinfo)) {
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print("Current time: ");
    Serial.println(timeStr);
    
    time_t now;
    time(&now);
    Serial.print("Timestamp: ");
    Serial.println((unsigned long)now);
  }
  
  return true;
}

// HMAC-SHA256计算
void hmacSha256(const char* key, const char* input, char* output) {
  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)input, strlen(input));
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  
  for(int i = 0; i < 32; i++) {
    sprintf(output + i * 2, "%02x", hmacResult[i]);
  }
  output[64] = '\0';
}

// 修复的MQTT配置生成
bool generateMqttConfig() {
  if (!timeSynced) {
    Serial.println("Time not synced, cannot generate MQTT config");
    return false;
  }
  
  // 获取当前时间戳
  time_t now;
  time(&now);
  unsigned long timestamp = (unsigned long)now;
  
  Serial.print("Generated timestamp: ");
  Serial.println(timestamp);
  
  // 生成clientId - 使用阿里云推荐格式
  snprintf(CLIENT_ID, sizeof(CLIENT_ID), 
           "%s|securemode=3,signmethod=hmacsha256,timestamp=%lu|", 
           PRODUCT_KEY, timestamp);
  
  // 生成username
  snprintf(MQTT_USERNAME, sizeof(MQTT_USERNAME), "%s&%s", DEVICE_NAME, PRODUCT_KEY);
  
  // 生成签名内容 - 修正的格式
  char signContent[256];
  snprintf(signContent, sizeof(signContent), 
           "clientId%sdeviceName%sproductKey%stimestamp%lu",
           PRODUCT_KEY, DEVICE_NAME, PRODUCT_KEY, timestamp);
  
  Serial.print("Sign content: ");
  Serial.println(signContent);
  
  // 生成密码
  hmacSha256(DEVICE_SECRET, signContent, MQTT_PASSWORD);
  
  Serial.println("=== MQTT Configuration ===");
  Serial.print("ClientId: "); Serial.println(CLIENT_ID);
  Serial.print("Username: "); Serial.println(MQTT_USERNAME);
  Serial.print("Password: "); Serial.println(MQTT_PASSWORD);
  Serial.print("Server: "); Serial.println(MQTT_SERVER);
  Serial.println("==========================");
  
  return true;
}

// WiFi初始化
void wifiInit() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWD);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > 30000) {
      Serial.println("\nWiFi connection failed!");
      wifiConnected = false;
      return;
    }
    delay(500);
    Serial.print(".");
  }
  
  wifiConnected = true;
  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// MQTT连接检查
void mqttCheckConnect() {
  if (client.connected()) return;
  
  static unsigned long lastConnectAttempt = 0;
  
  if (millis() - lastConnectAttempt < 10000) return;
  lastConnectAttempt = millis();
  
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }
  
  if (!timeSynced) {
    if (!syncTime()) {
      Serial.println("Time sync failed");
      return;
    }
  }
  
  Serial.println("Attempting MQTT connection...");
  
  if (!generateMqttConfig()) {
    Serial.println("MQTT config failed");
    return;
  }
  
  client.setServer(MQTT_SERVER, MQTT_PORT);
  
  // 尝试连接
  if (client.connect(CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("MQTT connected successfully!");
  } else {
    Serial.print("MQTT connection failed, state: ");
    Serial.println(client.state());
    
    // 详细的错误信息
    switch (client.state()) {
      case -4: Serial.println("MQTT_CONNECTION_TIMEOUT"); break;
      case -3: Serial.println("MQTT_CONNECTION_LOST"); break;
      case -2: Serial.println("MQTT_CONNECT_FAILED"); break;
      case -1: Serial.println("MQTT_DISCONNECTED"); break;
      case 1: Serial.println("MQTT_CONNECT_BAD_PROTOCOL"); break;
      case 2: Serial.println("MQTT_CONNECT_BAD_CLIENT_ID"); break;
      case 3: Serial.println("MQTT_CONNECT_UNAVAILABLE"); break;
      case 4: 
        Serial.println("MQTT_CONNECT_BAD_CREDENTIALS - Check:");
        Serial.println("1. Device triple (ProductKey, DeviceName, DeviceSecret)");
        Serial.println("2. Timestamp synchronization");
        Serial.println("3. Password generation");
        break;
      case 5: Serial.println("MQTT_CONNECT_UNAUTHORIZED"); break;
    }
  }
}

// MQTT回调函数
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message received [");
  Serial.print(topic);
  Serial.print("]: ");
  
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  Serial.println(message);
}

// 打印十六进制数据（用于调试）
void printHexData(const String& data) {
  Serial.print("Hex: ");
  for (unsigned int i = 0; i < data.length(); i++) {
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}

// 打印串口统计信息
void printSerialStats() {
  unsigned long currentTime = millis();
  if (currentTime - lastSerialStats >= 30000) { // 每30秒打印一次统计
    Serial.println("=== Serial Communication Statistics ===");
    Serial.print("Total bytes received: ");
    Serial.println(serialBytesReceived);
    Serial.print("Total messages received: ");
    Serial.println(serialMessagesReceived);
    Serial.print("Buffer length: ");
    Serial.println(serialBuffer.length());
    Serial.print("WiFi status: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    Serial.print("MQTT status: ");
    Serial.println(client.connected() ? "Connected" : "Disconnected");
    Serial.println("======================================");
    lastSerialStats = currentTime;
  }
}

// 串口健康检查（不发送数据，只检查状态）
void checkSerialHealth() {
  static unsigned long lastCheckTime = 0;
  unsigned long currentTime = millis();
  
  // 每15秒检查一次串口状态
  if (currentTime - lastCheckTime >= 15000) {
    Serial.println("=== SERIAL HEALTH CHECK ===");
    Serial.print("Serial2 available: ");
    Serial.println(Serial2.available());
    Serial.print("Total bytes received: ");
    Serial.println(serialBytesReceived);
    Serial.print("Total messages received: ");
    Serial.println(serialMessagesReceived);
    Serial.print("Current buffer: '");
    Serial.print(serialBuffer);
    Serial.println("'");
    Serial.println("===========================");
    lastCheckTime = currentTime;
  }
}

// 读取串口数据 - 增强调试
void readSerialData() {
  static unsigned long lastDataTime = 0;
  static bool dataTimeoutPrinted = false;
  
  while (Serial2.available()) {
    char c = Serial2.read();
    serialBytesReceived++;
    lastDataTime = millis();
    dataTimeoutPrinted = false;
    
    // 实时显示每个收到的字节（调试用）
    Serial.print("[RX] 0x");
    Serial.print(c, HEX);
    Serial.print(" ('");
    if (c >= 32 && c <= 126) {  // 可打印字符
      Serial.print(c);
    } else {
      Serial.print("?");
    }
    Serial.println("')");
    
    if (c == '\n') {
      if (serialBuffer.length() > 0) {
        serialMessagesReceived++;
        
        Serial.println("=== RAW SERIAL DATA RECEIVED ===");
        Serial.print("Length: ");
        Serial.println(serialBuffer.length());
        Serial.print("Content: '");
        Serial.print(serialBuffer);
        Serial.println("'");
        
        // 显示十六进制格式
        printHexData(serialBuffer);
        
        // 显示每个字符的ASCII码
        Serial.print("ASCII: ");
        for (unsigned int i = 0; i < serialBuffer.length(); i++) {
          Serial.print((int)serialBuffer[i]);
          Serial.print(" ");
        }
        Serial.println();
        
        Serial.println("=================================");
        
        // 解析传感器数据
        if (parseSensorData(serialBuffer)) {
          Serial.println("✓ Data parsed successfully");
        } else {
          Serial.println("✗ Failed to parse sensor data");
        }
        
        serialBuffer = "";
      }
    } else if (c != '\r') {
      serialBuffer += c;
    }
    
    // 防止缓冲区溢出
    if (serialBuffer.length() >= 128) {
      Serial.println("!!! Serial buffer overflow, clearing buffer !!!");
      serialBuffer = "";
    }
  }
  
  // 如果超过5秒没有收到数据，打印超时信息（但只打印一次）
  if (serialBytesReceived > 0 && millis() - lastDataTime > 5000 && !dataTimeoutPrinted) {
    Serial.println("!!! No data received for 5 seconds !!!");
    Serial.print("Current buffer: '");
    Serial.print(serialBuffer);
    Serial.println("'");
    dataTimeoutPrinted = true;
  }
}

bool parseSensorData(const String& data) {
    String input = data;
    input.trim();
    
    Serial.print("Parsing: '");
    Serial.print(input);
    Serial.println("'");
    
    // 只支持不带空格的格式：xxxxA 或 xxxxB
    // 格式要求：4位数字 + 1位节点标识符
    
    if (input.length() != 5) {
        Serial.print("✗ Invalid length. Expected 5 characters, got ");
        Serial.println(input.length());
        return false;
    }
    
    // 提取数字部分和节点标识
    String valueStr = input.substring(0, 4);
    char nodeID = input.charAt(4);
    
    Serial.print("Value string: '");
    Serial.print(valueStr);
    Serial.println("'");
    Serial.print("Node ID: '");
    Serial.print(nodeID);
    Serial.println("'");
    
    // 验证节点标识
    if (nodeID != 'A' && nodeID != 'B') {
        Serial.println("✗ Invalid node ID, expected 'A' or 'B'");
        return false;
    }
    
    // 验证前4个字符都是数字
    for (int i = 0; i < 4; i++) {
        if (!isdigit(valueStr[i])) {
            Serial.print("✗ Invalid character in sensor data at position ");
            Serial.print(i);
            Serial.print(": '");
            Serial.print(valueStr[i]);
            Serial.println("'");
            return false;
        }
    }
    
    // 解析温度和湿度
    String tempStr = valueStr.substring(0, 2);
    String humiStr = valueStr.substring(2, 4);
    
    float temp = tempStr.toFloat();
    float humi = humiStr.toFloat();
    
    Serial.print("Temperature: ");
    Serial.print(temp);
    Serial.println("°C");
    Serial.print("Humidity: ");
    Serial.print(humi);
    Serial.println("%");
    
    // 验证数据范围并存储
    if (temp >= 0 && temp <= 99 && humi >= 0 && humi <= 99) {
        SensorData* target = (nodeID == 'A') ? &nodeA : &nodeB;
        target->temperature = temp;
        target->humidity = humi;
        target->timestamp = millis();
        target->valid = true;
        
        Serial.printf("✓ Node %c data - Temp: %.0f°C, Humi: %.0f%%\n", 
                     nodeID, temp, humi);
        return true;
    }
    
    Serial.println("✗ Data out of valid range (temp: 0-99, humi: 0-99)");
    return false;
}

// 为每个节点独立的上传时间控制
void mqttPostData() {
    if (!client.connected()) return;
    
    unsigned long currentTime = millis();
    
    // 节点A独立上传
    if (nodeA.valid && (currentTime - nodeA.timestamp < 30000)) {
        if (currentTime - nodeA.lastUploadTime >= UPLOAD_INTERVAL) {
            if (publishNodeData(&nodeA, '1')) {
                nodeA.lastUploadTime = currentTime;
                // 不要立即设为false，等待下次更新
            }
        }
    }
    
    // 节点B独立上传
    if (nodeB.valid && (currentTime - nodeB.timestamp < 30000)) {
        if (currentTime - nodeB.lastUploadTime >= UPLOAD_INTERVAL) {
            if (publishNodeData(&nodeB, '2')) {
                nodeB.lastUploadTime = currentTime;
            }
        }
    }
}

bool publishNodeData(SensorData* data, char nodeNum) {
    char message[256];
    const char* format = (nodeNum == '1') ? ALINK_BODY_FORMAT_A : ALINK_BODY_FORMAT_B;
    
    snprintf(message, sizeof(message), format, 
             millis(), data->temperature, data->humidity);
    
    Serial.printf("Publishing Node %c data...\n", (nodeNum == '1') ? 'A' : 'B');
    bool success = client.publish(ALINK_TOPIC_PROP_POST, message);
    
    if (success) {
        Serial.println("✓ Publish successful");
    } else {
        Serial.println("✗ Publish failed");
    }
    
    return success;
}

// 检查网络状态
void checkNetworkStatus() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 10000) return;
  lastCheck = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    Serial.println("!!! WiFi connection lost !!!");
  }
}

void setup() {
  // 初始化调试串口（连接到电脑）
  Serial.begin(115200);
  
  // 初始化与CC2530通信的串口（UART2）
  Serial2.begin(115200, SERIAL_8N1, CC2530_RX_PIN, CC2530_TX_PIN);
  
  delay(1000);
  
  Serial.println("=== ESP32-S3 CC2530 Coordinator ===");
  Serial.println("Supported data format: 'xxxxA' or 'xxxxB' (no spaces)");
  Serial.println("  Example: '2434A' = Node A Temp:24°C Humi:34%");
  Serial.println("  Example: '1825B' = Node B Temp:18°C Humi:25%");
  Serial.println("===================================");
  
  // 硬件自检
  Serial.println("=== HARDWARE SELF-TEST ===");
  Serial.print("Serial2 initialized: ");
  Serial.println(Serial2 ? "YES" : "NO");
  
  // 注释掉测试数据发送
  // Serial.println("Sending test message to CC2530...");
  // Serial2.println("TEST FROM ESP32");
  delay(100);
  
  // 检查是否有数据可读
  Serial.print("Bytes available from CC2530: ");
  Serial.println(Serial2.available());
  
  Serial.println("===============================================");
  
  wifiInit();
  
  if (wifiConnected) {
    client.setCallback(mqttCallback);
    client.setKeepAlive(120);
  }
  
  Serial.println("System initialized. Waiting for sensor data from CC2530...");
}

void loop() {
  checkNetworkStatus();
  readSerialData();      // 从Serial2读取CC2530数据
  checkSerialHealth();   // 替换为健康检查，不发送数据
  mqttCheckConnect();
  mqttPostData();
  printSerialStats();
  
  client.loop();
  delay(100);
}