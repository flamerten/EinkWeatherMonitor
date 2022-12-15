/*
  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/visualize-esp32-esp8266-sensor-readings-from-anywhere/
  
  Deep sleep added
*/

#define ESP32FEATHER //set as true if using feather
//#define ESP32DEVKIT

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include <SPI.h>
#include <GxEPD2_BW.h>
//#include <Fonts/Picopixel.h>

#include <NTPClient.h>
#include <WiFiUdp.h>

#include "WeatherBitmap.h" //Local Bitmap
#include "Secrets.h" //For various private info
#define BAT_MONITOR 35
#define BAT_USED 1

// Replace with your network credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// REPLACE with your Domain name and URL path or IP address with path
const char* serverName = BLUEHOST_POST;

// Keep this API Key value to be compatible with the PHP code provided in the project page. 
// If you change the apiKeyValue value, the PHP file /post-data.php also needs to have the same key 
String apiKeyValue = ESP32_API_KEY;

//Sensor
Adafruit_BME280 bme;
#define SEALEVELPRESSURE_HPA 1013.25
float temperature, humidity, pressure;

//Set LEDPIN
#if defined(ESP32FEATHER)
  #define LED_PIN 2
#elif defined(ESP32DEVKIT)
  #define LED_PIN 13
#endif

//Connection Timings
#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define SLEEP_MINUTES 10
const uint64_t TIME_TO_SLEEP = SLEEP_MINUTES*60;  
uint64_t time_now;
int8_t max_tries = 10;


//Eink
#define EPD_BUSY  32  // to EPD BUSY
#define EPD_CS    15  // to EPD CS
#define EPD_RST   27 // to EPD RST
#define EPD_DC    33 // to EPD DC
#define EPD_SCK   5 // to EPD CLK
#define EPD_MISO  21 // Master-In Slave-Out not used, as no data from display
#define EPD_MOSI  19 // to EPD DIN

//MH-ET Live 2.9" 128 X 296
//source: https://forum.arduino.cc/t/help-with-waveshare-epaper-display-with-adafruit-huzzah32-esp32-feather-board/574300/8 
GxEPD2_BW<GxEPD2_290_T94, GxEPD2_290_T94::HEIGHT> display(GxEPD2_290_T94(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

//Timer Server and Records for Singapore
#define NTP_OFFSET  28800 // In seconds
#define NTP_INTERVAL 60 * 1000    // In miliseconds
#define NTP_ADDRESS  "sg.pool.ntp.org"

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);

RTC_DATA_ATTR uint8_t Hours = 0;
RTC_DATA_ATTR uint8_t Minutes = 0;
RTC_DATA_ATTR uint8_t Seconds = 0;

int16_t error_code = 0;
/* 0 No Error
 * 1 Wifi 
 * 100 or more is http error code
 */

void initEink(){
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
  delay(100);

  display.init(115200); // default 10ms reset pulse, e.g. for bare panels with DESPI-C02
  display.setRotation(3);     // landscape orientaion 
  display.setFullWindow();  
  display.setFont(); //display.setFont(&Picopixel);
  display.setTextColor(GxEPD_BLACK, GxEPD_WHITE); 
}

void clearEink(){
  display.firstPage(); //Completely clear screen
  do
  {
    display.fillScreen(GxEPD_WHITE);
  }
  while (display.nextPage());
  display.fillScreen(GxEPD_WHITE); 
  delay(100);  
}

void PrintNoWifi(){
  display.print("No Wifi");  
}

void PrintHTTPFail(int code){
  display.print("HTTP Error: " + String(code));  
}

void PrintBatLevel(){
  float BatPercent = ( analogRead(BAT_MONITOR) / 4096.0) * 3.3 * 2;
  Serial.printf("Bat Level: %f",BatPercent);
  Serial.println();

  const uint8_t height = 4;
  uint8_t width = 10;  
  //BatPercent
  float percentage = width * ( BatPercent - 3.0)/(3.8 - 3.0);
  uint8_t bat_w = percentage; //cast back to int
  
  bat_w = min(bat_w,width); //restrict to 8

  Serial.printf("Bat_w:%i",bat_w);
  Serial.println();
  //x,y,w,h
  display.drawRect(0,0,width+1,height,GxEPD_BLACK); //Bat Shape
  display.fillRect(0,0,bat_w,height,GxEPD_BLACK); //amt left
  display.fillRect(10,1,2,2,GxEPD_BLACK);

}

bool UploadData(float temp, float hum,float pres){
  WiFi.begin(ssid, password);
  Serial.print("Connecting --> ");
  time_now = millis();
  
  while(WiFi.status() != WL_CONNECTED) { 
    delay(10);
    //Serial.print(".");
    if(millis() - time_now >= 10000){
      error_code = 1;
      Serial.println("Failed");
      return false; 
    }
  }

  Serial.println("Success");
  Serial.println("");
  Serial.print("Connected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());

  WiFiClient client;
  HTTPClient http;
  http.begin(client, serverName);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String httpRequestData = "api_key=" + apiKeyValue + "&value1=" + String(temp)
                         + "&value2=" + String(hum) + "&value3=" + String(pres) + "";
  Serial.print("httpRequestData: ");
  Serial.println(httpRequestData);

  int httpResponseCode;
  //max_tries
  do{
    httpResponseCode = http.POST(httpRequestData);
    Serial.printf("Attempt %i. Error Code: %i",10-max_tries,httpResponseCode);
    Serial.println();
    if(max_tries <= 0){
      error_code = httpResponseCode;
      http.end();
      return false;
    }

    delay(100);

    max_tries--;
  }
  while( (httpResponseCode != 200) );


  http.end();
  

}

float computeHeatIndex(float temperature, float humidity){
  /*
   *Source: https://github.com/adafruit/DHT-sensor-library/blob/master/DHT.cpp
   *Formula based on https://byjus.com/heat-index-formula/
   *Temp in degrees C, humidity %
   */

  float tempF = temperature * 1.8 + 32;
  float hi = 0.5 * (tempF + 61.0 + ((tempF - 68.0) * 1.2) + (humidity * 0.094));

  if (hi > 79) {
    hi = -42.379 + 2.04901523 * tempF + 10.14333127 * humidity +
         -0.22475541 * tempF * humidity +
         -0.00683783 * pow(tempF, 2) +
         -0.05481717 * pow(humidity, 2) +
         0.00122874 * pow(tempF, 2) * humidity +
         0.00085282 * tempF * pow(humidity, 2) +
         -0.00000199 * pow(tempF, 2) * pow(humidity, 2);

    if ((humidity < 13) && (tempF >= 80.0) &&
        (tempF <= 112.0))
      hi -= ((13.0 - humidity) * 0.25) *
            sqrt((17.0 - abs(tempF - 95.0)) * 0.05882);

    else if ((humidity > 85.0) && (tempF >= 80.0) &&
             (tempF <= 87.0))
      hi += ((humidity - 85.0) * 0.1) * ((87.0 - tempF) * 0.2);
  }  

  return (hi - 32) * 0.5556; //convert back to celcius


}

void printData(float temp, float hum,float pres){
  float HeatIndex = computeHeatIndex(temp,hum);
  float Altitude = bme.readAltitude(SEALEVELPRESSURE_HPA);

  display.drawInvertedBitmap(0,0,WeatherBitmapArray,296,128, GxEPD_BLACK); //Not too sure why it must be inverted?
  uint8_t y_row_1 = 22; uint8_t y_row_2 = 75; int y_increment = 10;
  uint8_t x_col_1 = 45; uint8_t x_col_2 = 152; uint8_t x_col_3 = 238; 

  //display.setCursor(0, 0);
  display.setTextSize(1);

  display.setCursor(x_col_1,y_row_1);                 display.print("Temperature");
  display.setCursor(x_col_1,y_row_1 + y_increment);   display.print(String(temp) + (char)247 + "C"); //degrees symbol

  display.setCursor(x_col_1,y_row_2);                 display.print("Pressure");
  display.setCursor(x_col_1,y_row_2 + y_increment);   display.print( String(int(pres)) + "hPa");

  display.setCursor(x_col_2,y_row_1);                 display.print("Humidity");
  display.setCursor(x_col_2,y_row_1 + y_increment);   display.print( String(hum) + "%");

  display.setCursor(x_col_2,y_row_2);                 display.print("Altitude");
  display.setCursor(x_col_2,y_row_2 + y_increment);   display.print( String(Altitude) + "m");

  display.setCursor(x_col_3,y_row_1);                 display.print("HeatIndex");
  display.setCursor(x_col_3,y_row_1 + y_increment);   display.print(String(HeatIndex) + (char)247 + "C"); //degrees symbol


  //128 X 296  
  //String text = "00:00:00";
/*
  int16_t tbx, tby; uint16_t tbw, tbh; // boundary box window
  display.getTextBounds(text, 0, 0, &tbx, &tby, &tbw, &tbh); // it works for origin 0, 0, fortunately (negative tby!)
  uint16_t x = ((display.width() - tbw) / 2) - tbx;
  uint16_t y = ((display.height() - tbh) / 2) - tby;
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(x, y);
    display.print(text);
  }
  while (display.nextPage());

  Serial.printf("width: %i ",tbw);
  Serial.printf("height: %i ",tbh);
  Serial.println();
*/
  String HoursString = String(Hours) +":";
  if(Hours < 10)     HoursString = "0" + HoursString;
  String MinutesString = String(Minutes) +":";
  if(Minutes < 10)   MinutesString = "0" + MinutesString;
  String SecondsString = String(Seconds);
  if(Seconds < 10)   SecondsString = "0" + SecondsString;  
  
  String time_string = HoursString + MinutesString + SecondsString;
  
  display.setTextSize(1);
  display.setCursor(296-50,128-8); //Calculate height and width to put at bottom right

  display.print(time_string);

  //Serial.println(time_string);
  
  //display.display(); at the end
}


void setup() {
  //Serial.begin(115200); //will hang the esp32 if used with epaper display

  initEink();
  pinMode(BAT_MONITOR,INPUT);    
  //delay(1000);

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.printf("Deep Sleep set up for %is",TIME_TO_SLEEP);
  Serial.println();
  
  
  #if defined(ESP32FEATHER)
    Wire.begin(22,20);
    delay(1000); //no need anything if normal wire
  #endif

  // (you can also pass in a Wire library object like &Wire2)
  bool status = bme.begin(0x76);
  if (!status) {
    Serial.println("Could not find a valid BME280 sensor, check wiring or change I2C address!");
    pinMode(LED_PIN,OUTPUT); digitalWrite(LED_PIN,HIGH);
    while(1);
  }
  //Weather Monitoring Mode
  bme.setSampling(Adafruit_BME280::MODE_FORCED, // Forced Measurements - perform one measurement, store and then sleep
                  Adafruit_BME280::SAMPLING_X1, // temperature
                  Adafruit_BME280::SAMPLING_X1, // pressure
                  Adafruit_BME280::SAMPLING_X1, // humidity
                  Adafruit_BME280::FILTER_OFF   );

  Serial.print("RTC Memory ->");
  Serial.printf("%i:%i:%i",Hours,Minutes,Seconds);
  Serial.println();

  
}

void loop() {
  //Check WiFi connection status
  bme.takeForcedMeasurement();
  temperature = bme.readTemperature();
  humidity = bme.readHumidity();
  pressure = bme.readPressure()/100.0F; //in hPa

  UploadData(temperature,humidity,pressure);

  if(error_code == 1){ // No Wifi
    Minutes = Minutes + SLEEP_MINUTES;

    if(Minutes + SLEEP_MINUTES >= 60){
      Minutes = Minutes%60;
      Hours = (Hours + 1)%24;
    } 

    Serial.println("RTC Mem Used");
  }
  else{
    Serial.println("TimeClient Updated");
    timeClient.update();
    Hours = timeClient.getHours();
    Minutes = timeClient.getMinutes();
    Seconds = timeClient.getSeconds();
  }

  display.firstPage();
  do{
    display.fillScreen(GxEPD_WHITE); //Clear screen
    printData(temperature,humidity,pressure); //includes the time as well
    if(BAT_USED) PrintBatLevel();

    display.setTextSize(1);
    display.setCursor(0,128-8);
    
    if(error_code == 1) PrintNoWifi();
    else if(error_code != 0 ) PrintHTTPFail(error_code); 
  }
  while(display.nextPage());

  display.display();

  display.powerOff();
  
  Serial.println("Going to sleep");
  esp_deep_sleep_start();
}
