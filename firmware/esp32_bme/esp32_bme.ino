/*
  Samuel Yow
  ESp32 Eink Weather Monitor

  Upload data to cloud based on this tutorial by Rui Santos https://RandomNerdTutorials.com/visualize-esp32-esp8266-sensor-readings-from-anywhere/

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
#include <Fonts/Picopixel.h>

#include <NTPClient.h>
#include <WiFiUdp.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

#include "WeatherBitmap.h" //Local Bitmap
#include "Secrets.h" //For various private info
#define BAT_MONITOR 35

#define BAT_USED 1
#define USE_WIFI 1
#define USE_EINK 1

#define BUTTON 14
const uint64_t awake_time = 10 * 60 * 1000; //Awake after pressing button for 15mins
bool upload_mode = false;

// Replace with your network credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

const char* OTA_ssid = OTA_SSID;
const char* OTA_password = OTA_PASSWORD;


//Sensor
Adafruit_BME280 bme;
#define SEALEVELPRESSURE_HPA 1013.25
float Temperature, Humidity, Pressure, HeatIndex, Altitude;
//Set LEDPIN
#if defined(ESP32FEATHER)
  #define LED_PIN 2
#elif defined(ESP32DEVKIT)
  #define LED_PIN 13
#endif

//Connection Timings
#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define SLEEP_MINUTES 15
const uint64_t TIME_TO_SLEEP = SLEEP_MINUTES*60;  
#define MAX_TRIES 10

//Eink
#define EPD_BUSY  32  // to EPD BUSY
#define EPD_CS    15  // to EPD CS
#define EPD_RST   27 // to EPD RST
#define EPD_DC    33 // to EPD DC
#define EPD_SCK   5 // to EPD CLK
#define EPD_MISO  19 // Master-In Slave-Out not used, as no data from display //21 // Had to swap due to errors in PCB
#define EPD_MOSI  21 // to EPD DIN  //19

//MH-ET Live 2.9" 128 X 296
//source: https://forum.arduino.cc/t/help-with-waveshare-epaper-display-with-adafruit-huzzah32-esp32-feather-board/574300/8 
GxEPD2_BW<GxEPD2_290_T94, GxEPD2_290_T94::HEIGHT> display(GxEPD2_290_T94(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

//Timer Server and Records for Singapore
#define NTP_OFFSET  28800 // In seconds
#define NTP_INTERVAL 60 * 1000    // In miliseconds
#define NTP_ADDRESS  "sg.pool.ntp.org"

RTC_DATA_ATTR uint8_t RTC_Hours = 0;
RTC_DATA_ATTR uint8_t RTC_Minutes = 0;
RTC_DATA_ATTR uint8_t RTC_Seconds = 0;

//Graphing
#define MAX_GRAPH_PTS 20
RTC_DATA_ATTR float HeatIndexRecords[MAX_GRAPH_PTS]; //use less, so there are smoother points
RTC_DATA_ATTR uint8_t RecordFillPosition = 0;
RTC_DATA_ATTR bool FullCycle = false; //max length of data points have been collected

//OTA
AsyncWebServer server(80);

int16_t error_code = 0;
/* 0 No Error
 * 1 Wifi 
 * 100 or more is http error code
 */


void setupWebServer(){
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Hi! I am ESP32.");
  });

  AsyncElegantOTA.begin(&server);    // Start ElegantOTA
  server.begin();
  Serial.println("HTTP server started");
}

void initEink(){
  if(!USE_EINK) return;
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
  delay(100);

  display.init(115200); // default 10ms reset pulse, e.g. for bare panels with DESPI-C02
  display.setRotation(3);     // landscape orientaion 
  display.setFullWindow();  
  display.setFont(); //display.setFont(&Picopixel);
  display.setTextColor(GxEPD_BLACK, GxEPD_WHITE); 
  display.firstPage();
}

void clearEink(){
  if(!USE_EINK) return;
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
  if(!USE_EINK) return;
  else display.print("No Wifi");
}

void PrintAsyncIP(){
  if(!USE_EINK) return;
  if(WiFi.status() == WL_CONNECTED){
    display.print("Async:");
    display.print(WiFi.localIP());
    display.print("/update");
  }
  else display.print("No OTA Wifi");

}

void PrintBatLevel(){
  float BatPercent = ( analogRead(BAT_MONITOR) / 4096.0) * 3.3 * 2;
  Serial.printf("Bat Level: %f",BatPercent);
  Serial.println();

  if(!USE_EINK) return;
  const uint8_t height = 4;
  uint8_t width = 10;  
  //BatPercent
  float percentage = width * ( BatPercent - 3.0)/(3.8 - 3.0);
  uint8_t bat_w = percentage; //cast back to int
  
  bat_w = min(bat_w,width); //restrict to 8

  Serial.printf("Bat_w:%i",bat_w);
  Serial.println();
  //x,y,w,h
  display.drawRect(0,0,width+1,height,GxEPD_BLACK); //Bat Shape( + 1 cause its the outline)
  display.fillRect(0,0,bat_w,height,GxEPD_BLACK); //amt left
  display.fillRect(10,1,2,2,GxEPD_BLACK);

  display.setFont(&Picopixel);
  display.setTextSize(1);
  display.setCursor(15,height); //Picopixel starts at the bottomleft/centre?
  display.print(BatPercent,2); //2dp precision of bat level
  display.setFont(); //

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
  
  if(USE_EINK){
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
  }


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

  //Serial.println(time_string);
  
  //display.display(); at the end
  
}

int mapf(double x, double in_min, double in_max, double out_min, double out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void PrintGraph(float HI){
  //UpdateValues
  HeatIndexRecords[RecordFillPosition] = HI;
  float DataSet[MAX_GRAPH_PTS]; //Not the most memory efficient i think

  //find max and min
  float max_hi = -90; float min_hi = 90; 
  uint8_t pos; uint8_t no_data_points = 0;
  uint16_t origin_x = 230;
  uint16_t origin_y = 73 ; //Top left corner
  uint8_t y_length = 22; uint8_t x_length = 41; //axis lengths
  String Title;

  
  if(FullCycle){
    for(int i = 0; i < MAX_GRAPH_PTS; i ++){
      pos = (RecordFillPosition + 1 + i)%MAX_GRAPH_PTS; //RecordFillPostion is the Current Point so the +1 point is the first point in the graph
      if(HeatIndexRecords[pos] > max_hi) max_hi = HeatIndexRecords[pos];
      if(HeatIndexRecords[pos] < min_hi) min_hi = HeatIndexRecords[pos];

      DataSet[no_data_points] = HeatIndexRecords[pos];
      no_data_points ++;
    }     
  }
  else{
    for(int pos = 0; pos <= RecordFillPosition; pos++){
      if(HeatIndexRecords[pos] > max_hi) max_hi = HeatIndexRecords[pos];
      if(HeatIndexRecords[pos] < min_hi) min_hi = HeatIndexRecords[pos];

      DataSet[no_data_points] = HeatIndexRecords[pos];
      no_data_points ++;    
    }
  }

  Serial.printf("Max: %f Min: %f",max_hi,min_hi); Serial.println();
  Serial.printf("No data points: %i",no_data_points); Serial.println();
  Serial.printf("HI: %f",HI); Serial.println();

  //Init Axis - also print the number of data points - maybe w title - maybe in terms of minutes?
  if(no_data_points < 10)  Title = "HI - 0" + String(no_data_points) + " pts";
  else                     Title = "HI - " + String(no_data_points) + " pts";

  if(USE_EINK){
    display.drawFastVLine(origin_x,origin_y,      y_length,GxEPD_BLACK); // Y axis;
    display.drawFastHLine(origin_x,origin_y + y_length ,x_length,GxEPD_BLACK); // X axis;

    
    //set max and min to be slightly below and above axis - mark them
    display.drawFastHLine(origin_x-2,origin_y + 1,3,GxEPD_BLACK); //Max 
    display.drawFastHLine(origin_x-2,origin_y + y_length - 1 ,3,GxEPD_BLACK); //Min

    display.setFont(&Picopixel);
    display.setTextSize(1);

    display.setCursor(235,70);
    display.print(Title);
    
    display.setCursor(origin_x-2-18,origin_y + 1 + 2); 
    display.print(max_hi,2); 

    display.setCursor(origin_x-2-18,origin_y + y_length - 1 + 2); //Picopixel starts at the bottomleft/centre?
    display.print(min_hi,2); 
  }

  Serial.printf("MaxCoord(%i,%i) MinCoord(%i,%i)",origin_x,origin_y+1,origin_x,origin_y+y_length-1);
  Serial.println();

  //Plot points
  
  int x,y, x_prev,y_prev;
  int increment  = (x_length - 1)/MAX_GRAPH_PTS;
  Serial.println("----------------------------");
  for(int i = 0; i < no_data_points; i++){
    x = origin_x + i*increment + 1;
    if(min_hi == max_hi) y = 10;
    else y = mapf(DataSet[i],min_hi,max_hi,0,20);
    
    if(y < 10) Serial.printf("Map Y: %i  ",y);
    else       Serial.printf("Map Y: %i ",y);
    
    y = origin_y + y_length -1 - y; 
    
    if(USE_EINK){
      display.drawPixel(x,y,GxEPD_BLACK);
      if(i!=0) display.drawLine(x,y,x_prev,y_prev,GxEPD_BLACK);
    }
    
    Serial.printf("HI: %f X: %i, Y: %i", DataSet[i],x,y); Serial.println();
    x_prev = x; y_prev = y;
  }

  Serial.println("----------------------------");
  

  //Update
  if(RecordFillPosition >= MAX_GRAPH_PTS-1){
    RecordFillPosition = 0;
    FullCycle = true;
  }
  else RecordFillPosition++;

}

void connectWifi(){
  WiFi.mode(WIFI_STA);
  uint32_t WifiConnectStart = millis();

  if(upload_mode) WiFi.begin(OTA_ssid, OTA_password);
  else            WiFi.begin(ssid, password);

  while ( (WiFi.status() != WL_CONNECTED) && (millis() - WifiConnectStart <= 5000)) { //try to connect for 5s
    delay(500);
    Serial.print(".");
  }

  if(WiFi.status() != WL_CONNECTED){
    error_code = 1;
    Serial.println("Unable to connect to Wifi");
  }
  if(upload_mode && error_code != 1) setupWebServer();

}

void updateBottom(String msg){
  display.setTextSize(1); //For Error codes
  display.setCursor(0,125); //numbers and words appear diff?
  display.setFont();
  display.setPartialWindow(0, 115, 296, 128);
  display.firstPage();

  String HoursString   = String(RTC_Hours) +":";
  if(RTC_Hours < 10)     HoursString = "0" + HoursString;
  String MinutesString = String(RTC_Minutes) +":";
  if(RTC_Minutes < 10)   MinutesString = "0" + MinutesString;
  String SecondsString = String(RTC_Seconds);
  if(RTC_Seconds < 10)   SecondsString = "0" + SecondsString;  
  
  String time_string = HoursString + MinutesString + SecondsString;

  do{
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(0,120); //numbers and words appear diff?
    display.println(msg);

    display.setCursor(296-50,128-8); //Calculate height and width to put at bottom right
    display.print(time_string);
  }while(display.nextPage());
}

void setup() {
  if(!USE_EINK) Serial.begin(115200); //will hang the esp32 if used with epaper display

  initEink();
  pinMode(BAT_MONITOR,INPUT);
  Serial.printf("Deep Sleep set up for %is",TIME_TO_SLEEP);
  Serial.println();

  esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();

  upload_mode = (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0);
  Serial.printf("Wakeup Cause %i:",wakeup_cause);
  
  
  #if defined(ESP32FEATHER)
    Wire.begin(22,20);
    delay(1000); //no need anything if normal wire
  #endif

  // (you can also pass in a Wire library object like &Wire2)
  bool status = bme.begin(0x76);
  if (!status) {
    Serial.println("Could not find a valid BME280 sensor, check wiring or change I2C address!");
    display.setTextSize(2);
    display.println("BME Sensor error");
    display.display();
    esp_deep_sleep_start(); // Hope to reset 
  }
  //Weather Monitoring Mode
  bme.setSampling(Adafruit_BME280::MODE_FORCED, // Forced Measurements - perform one measurement, store and then sleep
                  Adafruit_BME280::SAMPLING_X1, // temperature
                  Adafruit_BME280::SAMPLING_X1, // pressure
                  Adafruit_BME280::SAMPLING_X1, // humidity
                  Adafruit_BME280::FILTER_OFF   );

  Serial.print("RTC Memory -> ");
  Serial.printf("%i:%i:%i",RTC_Hours,RTC_Minutes,RTC_Seconds);
  Serial.println();

  bme.takeForcedMeasurement();
  Temperature = bme.readTemperature();
  Humidity = bme.readHumidity();
  Pressure = bme.readPressure()/100.0F; //in hPa
  HeatIndex = computeHeatIndex(Temperature,Humidity); //HeatIndex = bme.readHeatIndex(true); - pending push request
  Altitude = bme.readAltitude(SEALEVELPRESSURE_HPA);

  //Increment RTC and then print it to the Eink Device
  if(upload_mode) RTC_Minutes = RTC_Minutes + SLEEP_MINUTES/2; //Averages to about half, since upload mode does not occur at 10mins 
  else            RTC_Minutes = RTC_Minutes + SLEEP_MINUTES;
  
  if(RTC_Minutes >= 60){
    RTC_Minutes = RTC_Minutes%60;
    RTC_Hours   = (RTC_Hours + 1)%24;
  }

  //Display
  if(USE_EINK){
    do{
      display.fillScreen(GxEPD_WHITE); //Clear screen
      printData(Temperature,Humidity,Pressure); //includes the time as well
      if(BAT_USED) PrintBatLevel();

      PrintGraph(HeatIndex);
    }
    while(display.nextPage());

    display.display();
  }
  else{
      printData(Temperature,Humidity,Pressure); //includes the time as well
      if(BAT_USED) PrintBatLevel();
      PrintGraph(HeatIndex);
  }

  if((wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) || (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0)){
    connectWifi(); //Only look for wifi from the start.

    if(error_code != 1){
      // Update internal clock only when connected to WiFi
      Serial.println("Updating TimeClient");
      
      WiFiUDP ntpUDP;
      NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);
      
      uint8_t TC_Updates = 5;
      
      while(!timeClient.update()){
        timeClient.forceUpdate();
        TC_Updates--;
        if(TC_Updates <= 0){
          error_code = 2;
          break;
        }
      }
  
      if(error_code == 0){ // no errors 
        RTC_Hours = timeClient.getHours();
        RTC_Minutes = timeClient.getMinutes();
        RTC_Seconds = timeClient.getSeconds();
        Serial.println("TimeClient Updated");
      }
      else{
        Serial.println("Time Client failed to update"); //Simply use RTC
      }
    }

  }
  

  
  if(upload_mode && error_code != 1)            updateBottom("Async:" + (WiFi.localIP()).toString() + "/update");
  else if((upload_mode) && (error_code == 1))   updateBottom("No OTA Wifi");
  else if(error_code == 1)                      updateBottom("No Wifi");
  else if(error_code == 2)                      updateBottom("TimeClient Error");
  else if(error_code != 0)                      updateBottom("HTTP Error:" + String(error_code));
  else                                          updateBottom(""); //No errors  - error code = 0, so empty string is good

  esp_sleep_enable_ext0_wakeup(GPIO_NUM_14, LOW); 
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  if(!upload_mode){
    Serial.println("Going to sleep");
    display.powerOff();

    esp_deep_sleep_start();
  }
  
}

void loop() {
  //If here it keeps checking the wifi manager until the awake time exceeds
  if( (millis() > awake_time) || (digitalRead(BUTTON) == LOW)){
    updateBottom("No Upload");

    Serial.println("Going to sleep");
    display.powerOff();
    esp_deep_sleep_start();
  }

}
