#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include "DHT.h"
#include "word.h"
#include "config.h"


//引脚定义
#define DHTPIN 14 //dht11 温湿度读取引脚
#define DHTTYPE DHT11

#define OLED_SDA 21
#define OLED_SCL 22
#define LM35PIN 36 //lm35 温度传感器引脚
#define SERVOPIN 18//喂食舵机引脚
#define SpeadPonit 4 //读取跑圈数字接口

#define FEED_ONE 16 //喂食继电器1
#define FEED_TWO 17 //喂食继电器2

//对象声明
DHT dht(DHTPIN, DHTTYPE);//dht对象
WiFiClient espClient;//wifi对象
PubSubClient client(espClient);//mqtt对象
Servo servo1;//喂食舵机
Adafruit_SSD1306 display(128, 64, &Wire, -1);//oled显示屏


int pos = 0;//喂食舵机初始位置

//---------------计算速度全局变量-----------------------
int lastSts = 1; //default 1
long lapCount = 0;//总圈数★
long singleLapCount = 0; //单次运动圈数
float totalRun = 0.0; //总里程★
float tempTime1 = 0.0; // 长期处于识别区超时
float endTime = 0; //用于计算平均速度
float notRunTime = 0; //用于识别处于未转动状态，超过1 秒则结束一次平均速度计算
boolean calSpeedFlg = false;//计算平均速度flg
float singleSpeed = 0.0; //实时平均速度★
//---------------计算速度全局变量-----------------------


//mqtt回调函数
void callback(String topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);   
  }
  Serial.println();

  //自动喂食任务
  if(topic.equals("feedFood")){
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, payload);
      
      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
      }
    
      int feedRunTime = doc["runTime"];
      
      digitalWrite(FEED_ONE, HIGH);
      digitalWrite(FEED_TWO, LOW);
      delay(1000);
  
      digitalWrite(FEED_ONE, LOW);
      digitalWrite(FEED_TWO, HIGH);
      delay(1000*feedRunTime);
  
      digitalWrite(FEED_ONE, HIGH);
      digitalWrite(FEED_TWO, HIGH);
  }
}

//mqtt重连函数
void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
//    client.publish("outTopic", "hello world");
      //订阅喂食
      client.subscribe("feedFood");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

//taskOne：每一秒循环
void taskOne(void *parameter){

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextColor(WHITE);//开像素点发光
  
  while(1){

    display.clearDisplay();//清屏
    display.drawLine(0,11,128,11,WHITE);
    
    if ((WiFi.status() == WL_CONNECTED)) {
      display.drawBitmap(0, 0, wifiIcon, 10, 10, WHITE);
      //mqtt重连
      if (!client.connected()) {
        reconnect();
        vTaskDelay(1000);
      }else{
        display.drawBitmap(14, 0, mqIcon, 10, 10, WHITE); 
      }
      client.loop();
    }else{
      //wifi重连
      WiFi.begin(ssid, password);
      while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(1000);
      }
    }
  
    //环境温度
    display.setCursor(1,15);
    display.print("T:");
    display.setCursor(15,15);
    int t = dht.readTemperature();
    if(t <100){
      char t_char[6];
      dtostrf(t, 2, 0, t_char);
      display.print(t_char);
    }else{
      display.print(20);
    }
    
    //加热垫温度
    display.setCursor(25, 15);
    display.print("|");
    int temperature = analogRead(LM35PIN);
    float voltage = temperature * (5.0 / 4096.0);
    int temp = voltage*100;
    display.setCursor(30, 15);
    display.print(String(temp));

    display.setCursor(1,25);
    display.print("ODO:");
    display.setCursor(30,25);
    //运动速度相关显示
    char totalRun_char[6];
    if(totalRun>999){
    //超过999米变更为km单位
      dtostrf(totalRun/1000, 1, 3, totalRun_char);
      strcat(totalRun_char, " km");
      display.print(totalRun_char);
    }else{
      dtostrf(totalRun, 1, 1, totalRun_char);
      strcat(totalRun_char, " m");
      display.print(totalRun_char);
    }
    
    //实时速度
    display.setCursor(1, 46);
    display.print("Speed:");
    display.setCursor(40, 46);
    char currSpeed_char[5];
    dtostrf(singleSpeed, 1, 1, currSpeed_char);
    strcat(currSpeed_char, " m/s");
    display.print(currSpeed_char);
  
    display.setCursor(1, 54);
    display.print("lapCount:");
    display.setCursor(55, 54);
    display.print(lapCount);

    display.display();//屏幕显示
    
    vTaskDelay(1000);
  }
   vTaskDelete(NULL);
}

//每2分钟执行
void taskTwo(void *parameter){
  while(1){
    //30s 前总圈数
    int currLapCount = lapCount;
    //等待一分钟
    vTaskDelay(1000*60*2);
    //如果一分钟后总圈数大于一分钟前
    if(currLapCount < lapCount){
      //通过mqtt 发送总圈数到服务器
      String payload = "{\"msg\":[1,#lapCount]}";
      payload.replace("#lapCount",String(lapCount));
      int len= payload.length()+1;
      char char_array[len];
      payload.toCharArray(char_array,len);
      client.publish("t_active",char_array);
    }
  }
  vTaskDelete(NULL);
}


//主函数初始化
void setup()
{
  
  Serial.begin(115200);
  //喂食继电器引脚
  pinMode(FEED_ONE, OUTPUT);
  pinMode(FEED_TWO, OUTPUT);

  digitalWrite(FEED_ONE, HIGH);
  digitalWrite(FEED_TWO, HIGH);
  
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  //MQTT服务器设定
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  //dht11 温湿度传感器启动
  dht.begin();

  servo1.attach(SERVOPIN, 500, 2400);
  
  //跑圈红外传感器引脚
  pinMode(SpeadPonit, INPUT);

  //从服务器获取 初始化 lapCount
  WiFiClientSecure *client = new WiFiClientSecure;
  if(client) {
    HTTPClient https;
    client -> setCACert(rootCACertificate);
    {
      HTTPClient https;
      if (https.begin(*client, web_server+"/getMaxLapCount?hamsterId=1")) {
        int httpCode = https.GET();
        if (httpCode > 0) {
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            String payload = https.getString();
            StaticJsonDocument<128> doc;
            deserializeJson(doc, payload);
            lapCount = doc["lapCount"];
            totalRun = lapCount * Perimeter;
          }
        }
      }else {
        Serial.printf("[HTTPS] Unable to connect\n");
      }
    }
    delete client;
  }
  
  //子任务设定
  xTaskCreatePinnedToCore(
    taskOne,   /* Task function. */
    "TaskOne", /* String with name of task. */
    15000,     /* Stack size in bytes. */
    NULL,      /* Parameter passed as input of the task */
    1,         /* Priority of the task. */
    NULL,      /* Task handle. */
    0          /* Task Core. */
    );  
  //子任务设定
  xTaskCreatePinnedToCore(
    taskTwo,   /* Task function. */
    "TaskTwo", /* String with name of task. */
    15000,     /* Stack size in bytes. */
    NULL,      /* Parameter passed as input of the task */
    1,         /* Priority of the task. */
    NULL,      /* Task handle. */
    1          /* Task Core. */
    );
}

void loop()
{
    int currSts = digitalRead(SpeadPonit);

//    Serial.print("参数：");
//    Serial.println(currSts);

    //一圈识别开始
    if (lastSts == 1 && currSts == 0) {
      tempTime1 = millis();//用于识别区超时
      if (calSpeedFlg == false) {
        calSpeedFlg = true; //在这里 开启calSpeedFlg Flg
        singleLapCount = 0; //单次运动圈数，从开启flg开始置为0
        singleSpeed = 0.0;
      }
    }

     //一圈识别结束
  if (lastSts == 0 && currSts == 1 ) {
    float tempTime2 = millis();//用于识别区超时，理论上状态跃迁是[1,0][0,0][0,0][0,1]
    
    //超时条件，如果一直处于识别区域超过1秒，则重置lastSts
    //强制转换为[1,1]状态进入到平均速度结算
    if ((tempTime2 - tempTime1) > 1000 ) {
      lastSts = 1;
    } else {
      //未超时，总圈数增加
      lapCount++;
      totalRun = lapCount * Perimeter;
      singleLapCount++;
      if (calSpeedFlg == true){
        if(singleLapCount > 1) {//第一圈速度不准，忽略
          float tempT = millis();
          float useTime = (tempT - notRunTime) / 1000.0;
          singleSpeed = Perimeter / useTime;
        }
      }
    }
    notRunTime = millis();//每次完成一圈识别的下一个状态是[1,1],刷新notRunTime,用于计算平均速度
  }

  //运动结束变量清零
  if (lastSts == 1 && currSts == 1) {
    if (calSpeedFlg == true) {
      endTime = millis();
      if ((endTime - notRunTime) > 3000) {
        //大于3秒间隔，识别为一次连贯运动，可以进行平均速度计算了
        calSpeedFlg = false;//关闭计算平均速度flg
        singleSpeed = 0.0; //运动结束时 将实时速度清零
        endTime =0.0;
        notRunTime = 0.0;
        singleLapCount = 0;
      }
    }
  }

  //最后更新lastSts
  lastSts = currSts;
    
//  Serial.print("内存：");
//  Serial.println(ESP.getFreeHeap());

  delay(10);
}
