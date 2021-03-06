/*Example code for OSC message send/recieve with ESP8266
Based on example code made available through CNMAT
Written by: Steven A. Bjornson | 19/12/15

Included is a Max/MSP patch for controlling a single LED on board.
Max patch uses CNMAT OSC externals*/
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
// #include <WiFiClient.h>
// #include <WiFiClientSecure.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

#include <OSCMessage.h>
#include <OSCBundle.h>

#include <Adafruit_NeoPixel.h>
#include "wifiCred.h" //used to store SSID and PASS

#include <Math.h>
#include <cstdint>

#include "MotionState.h"
#include "ImuDataContainer.h"

/*Voltage Measurement*/
// Voltage divider on ADC allows for a measurement of battery voltage.
// maybe these don't need to be preprocessor
#define V_RES float(1./256.) //the number of volts per step
#define R1 float(200.)
#define R2 float(33.)
#define V_SCALE float(R2 / (R1 + R2))
#define V_MIN float(2.6) //the undervoltage shutoff of voltage regulator
#define V_MAX float(4.2) //the theoretical maximum voltage of LiPo
#define SCALED_V_MIN float(V_SCALE * V_MIN / V_RES)
#define SCALED_V_MAX float(V_SCALE * V_MAX / V_RES)

#define abs(x) ((x)>0?(x):-(x)) //taken from STD

/*MPU*/
#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"

#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    #include "Wire.h"
#endif

MPU6050 mpu;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
VectorInt16 gyro;
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

IMUData mpu_data; //replaces the above

float gyrothresh = 0.003; //trigger for checking for activity
float accelthresh = 0.095; //trigger for checking for activity

#define sensitivity float(2000) //the curent sensativity of the gyroscop
//===========================================================================//

/*Internal Representation Object*/
MotionState self;

//MPU soft-wire
#define SDA_PIN 13
#define SCL_PIN 12

/*Neopixels*/
#define PIX_PIN 15
#define NUMPIX 12
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMPIX, PIX_PIN, NEO_GRB + NEO_KHZ800);
//===========================================================================//

int status = 0;     // the Wifi radio's status

IPAddress outIp(255, 255, 255, 255); //broadcast UDP
//===========================================================================//

/*UDP*/
WiFiUDP Udp; //make ESP8266 Udp instance
const unsigned int inPort = 8888;
const unsigned int outPort = 9999;
//===========================================================================//

/*UDP Scheduler*/
bool transmit_flag = 1;
long old_time, current_time = 0;
long schedule_time = 25; //interval between UDP sends *NEEDED! Will crash otherwise
uint8_t old_val = 0;
//===========================================================================//

/*OTA*/
int prog = 0; //keeps track of download prograss for OTA to be displayed on WS2812s
//===========================================================================//

/*Packet Counter*/
// countes every time a UDP packet is sent
uint64_t pack_count = 0;
//===========================================================================//

/*Interrupt Detection Routine*/
volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
    mpuInterrupt = true;
}
//===========================================================================//

/*OSC Callbacks*/

//Neopixel Control Callback
void leds(OSCMessage &msg)
{
  uint8_t n, r, g, b = 0;
  
  // change the colour of ALL pixelsels simultaneously
  if(msg.size() < 4)
  {
    if (msg.isInt(0))
      r = msg.getInt(0);
    if (msg.isInt(1))
      g = msg.getInt(1);
    if (msg.isInt(2))
      b = msg.getInt(2);
    
    for (uint16_t i = 0; i < NUMPIX; i++)
      strip.setPixelColor(i, r, g, b);
  }

  // change the colour of pixel 'n'.
  else
  {
    if (msg.isInt(0))
      n = msg.getInt(0);
    if (msg.isInt(1))
      r = msg.getInt(1);
    if (msg.isInt(2))
      g = msg.getInt(2);
    if (msg.isInt(3))
      b = msg.getInt(3);

    strip.setPixelColor(n, r, g, b);
  }
  strip.show();
}

// hsl endpoint
void hsl(OSCMessage &msg){
  uint16_t rgb[3];

  hslToRgb(msg.getFloat(0), msg.getFloat(1), msg.getFloat(2), rgb);
  for (uint16_t i = 0; i < NUMPIX; i++)
      strip.setPixelColor(i, rgb[0], rgb[1], rgb[2]);
  strip.show();
}

void brightness(OSCMessage &msg)
{
  int temp = 0;
  if (msg.isInt(0)){
    if (msg.getInt(0) < 0)
      temp = 0;
    else if (msg.getInt(0) > 255)
      temp = 255;
    else
      temp = msg.getInt(0);
  }
  else if(msg.isFloat(0)){
    if (msg.getFloat(0) < 0)
      temp = 0;
    else if (msg.getFloat(0) > 1.)
      temp = 255;
    else
      temp = msg.getFloat(0) * 255;
  }

  strip.setBrightness(temp);
  strip.show();
}

//change schedule_time
void update_interval(OSCMessage &msg)
{
  if(msg.isInt(0))
    schedule_time = long(msg.getInt(0));
  if(msg.isFloat(0))
    schedule_time = long(msg.getFloat(0));

  transmit_flag = 1;
  if (schedule_time == 0)
    transmit_flag = 0;
  else if (schedule_time < 10)
    schedule_time = 10;
}

//callback to restart ESP
void reset(OSCMessage &msg)
{
  ESP.reset();
}

void accelThresh(OSCMessage &msg){
  if(msg.isFloat(0))
    self.setAccelThresh(msg.getFloat(0));
}

void gyroThresh(OSCMessage &msg){
  if(msg.isFloat(0))
    self.setGyroThresh(msg.getFloat(0));
}

void motionDecay(OSCMessage &msg){
  int temp = 0;
  if(msg.isInt(0))
    temp = msg.getInt(0);
  else if(msg.isFloat(0))
    temp = int(msg.getFloat(0));

  self.setMotionDecay(temp);
}
//===========================================================================//

void hslToRgb(double h, double sl, double l, uint16_t *rgb)
{
      double v;
      double r,g,b;

      if (h == 1.)
        h = 0.;

      r = l;   // default to grey
      g = l;
      b = l;
      v = (l <= 0.5) ? (l * (1.0 + sl)) : (l + sl - l * sl);
      if (v > 0)
      {
            double m;
            double sv;
            int sextant;
            double fract, vsf, mid1, mid2;

            m = l + l - v;
            sv = (v - m ) / v;
            h *= 6.0;
            sextant = (int)h;
            fract = h - sextant;
            vsf = v * sv * fract;
            mid1 = m + vsf;
            mid2 = v - vsf;
            switch (sextant)
            {
                  case 0:
                        r = v;
                        g = mid1;
                        b = m;
                        break;
                  case 1:
                        r = mid2;
                        g = v;
                        b = m;
                        break;
                  case 2:
                        r = m;
                        g = v;
                        b = mid1;
                        break;
                  case 3:
                        r = m;
                        g = mid2;
                        b = v;
                        break;
                  case 4:
                        r = mid1;
                        g = m;
                        b = v;
                        break;
                  case 5:
                        r = v;
                        g = m;
                        b = mid2;
                        break;
            }
      }
      rgb[0] = r * 255.0f;
      rgb[1] = g * 255.0f;
      rgb[2] = b * 255.0f;
}

void setup() {
  self.initialize(&mpu_data, accelthresh, gyrothresh, 500); //initialize physical state representation

  //Initialize serial and wait for port to open:
  Serial.begin(115200); 
  while (!Serial) {
    ; // wait for serial port to connect. Needed for Leonardo only
  }
  //=========================================================================//

  /*Neopixels*/
  strip.begin();
  strip.show(); // Initialize all pixels to 'off'
  //=========================================================================//

  /* join I2C bus */
  //(I2Cdev library doesn't do this automatically)
  #if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
      Wire.begin(SDA_PIN, SCL_PIN);
      int TWBR = 24; // 400kHz I2C clock (200kHz if CPU is 8MHz). Comment this line if having compilation difficulties with TWBR.
  #elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
      Fastwire::setup(400, true);
  #endif
  //=========================================================================//

  /*MPU6050*/
  mpu.initialize();
  devStatus = mpu.dmpInitialize();

  // supply your own gyro offsets here, scaled for min sensitivity
  mpu.setXGyroOffset(220);
  mpu.setYGyroOffset(76);
  mpu.setZGyroOffset(-85);
  mpu.setZAccelOffset(1788); // 1688 factory default for my test chip

  // make sure it worked (returns 0 if so)
  if (devStatus == 0) {
      // turn on the DMP, now that it's ready
      //Serial.println(F("Enabling DMP..."));
      mpu.setDMPEnabled(true);

      // enable Arduino interrupt detection
      //Serial.println(F("Enabling interrupt detection (Arduino external interrupt 0)..."));
      attachInterrupt(4, dmpDataReady, RISING);
      mpuIntStatus = mpu.getIntStatus();

      // set our DMP Ready flag so the main loop() function knows it's okay to use it
      //Serial.println(F("DMP ready! Waiting for first interrupt..."));
      dmpReady = true;

      // get expected DMP packet size for later comparison
      packetSize = mpu.dmpGetFIFOPacketSize();
  } else {
      // ERROR!
      // 1 = initial memory load failed
      // 2 = DMP configuration updates failed
      // (if it's going to break, usually the code will be 1)
      // Serial.print(F("DMP Initialization failed (code "));
      // Serial.print(devStatus);
      // Serial.println(F(")"));
  }
  //===========================================================================//
  
  uint8_t wifi_loading = 0;

  // attempt to connect to Wifi network:
  while ( status != WL_CONNECTED) 
  { 
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(SSID);
    
    // Connect to WPA/WPA2 network: 
    WiFi.mode(WIFI_STA);   
    status = WiFi.begin(SSID, PASS);
    
    //LEDs cycle through R/G/B while connecting
    for (uint16_t i = 0; i < NUMPIX; ++i)
    {
      switch(wifi_loading % 3){
        case 0:
          strip.setPixelColor(i, 255, 0, 0);
          break;
        case 1:
          strip.setPixelColor(i, 0, 255, 0);
          break;
        case 2:
          strip.setPixelColor(i, 0, 0, 255);
          break;
      }
    }
    strip.show();
    wifi_loading++;


    // wait 1 seconds for connection:
    delay(1000);
  }
  
  //clear
  for (uint16_t i = 0; i < NUMPIX; i++)
    strip.setPixelColor(i, 0);
  strip.show();

  // you're connected now, so print out the data:
  // Serial.println("You're connected to the network");
  // Serial.print("IP address: ");
  // Serial.println(WiFi.localIP());
  //=========================================================================//

  /*Setup OSC*/
  Udp.begin(inPort); //input Udp stream

  /*Over the Air Updates*/  
  //Progress Bar
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("End");

    //LEDs turn blue when update complete
    for (uint16_t i = 0; i < NUMPIX; i++)
      strip.setPixelColor(i, 0, 0, 255);
    strip.show();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\n", (progress / (total / 100)));

    if (prog != progress/float(total) * (NUMPIX+1))
    {
      for (uint16_t i = 0; i < NUMPIX; ++i)
      {
        if(i < prog)
          strip.setPixelColor(i, 0, 255, 0);
        else
          strip.setPixelColor(i, 0);
      }
      strip.show();

      prog = progress/float(total) * NUMPIX;
    }

  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");

    //LEDs turn red if failed to update
    for (uint16_t i = 0; i < NUMPIX; i++)
      strip.setPixelColor(i, 255, 0, 0);
    strip.show();
  });

  //Start OTA Service
  ArduinoOTA.begin();
  //=========================================================================//

}

void loop() {
  ArduinoOTA.handle(); //check OTA
  
  //if (!dmpReady) return; // if programming failed, don't try to do anything

  /*Scheduler*/
  //possible make object in future  
  current_time = millis(); 
  
  // wait for MPU interrupt or extra packet(s) available
  if (!mpuInterrupt && fifoCount < packetSize && current_time - old_time > schedule_time && transmit_flag) {
    /*OSC Out*/
    //declare a bundle
    OSCBundle bndl;

    //deals with generating the output endpoints automatically (adding ChipID for unique)
    //note: in the future this should be handled better (object or at least function)
    char concat[80];

    sprintf(concat, "/%06x%s", ESP.getChipId(), "/time");
    bndl.add(concat).add((int32_t)millis()); //time since active :: indicates a connection
    
    sprintf(concat, "/%06x%s", ESP.getChipId(), "/ypr");
    bndl.add(concat).add(self.getYPR()[0]).add(self.getYPR()[1]).add(self.getYPR()[2]); //yaw/pitch/roll

    sprintf(concat, "/%06x%s", ESP.getChipId(), "/accel");
    bndl.add(concat).add(self.getAccel()[0]).add(self.getAccel()[1]).add(self.getAccel()[2]); //raw acceleration

    sprintf(concat, "/%06x%s", ESP.getChipId(), "/gyro");
    bndl.add(concat).add(self.getGyro()[2]).add(self.getGyro()[1]).add(self.getGyro()[0]); //raw gyroscope

    sprintf(concat, "/%06x%s", ESP.getChipId(), "/batt");
    bndl.add(concat).add(float((analogRead(A0) >> 2)-SCALED_V_MIN)/(SCALED_V_MAX - SCALED_V_MIN)); //battery voltage [0,1]
    
    sprintf(concat, "/%06x%s", ESP.getChipId(), "/side");
    bndl.add(concat).add(self.whichSide()).add(self.sideValue());

    sprintf(concat, "/%06x%s", ESP.getChipId(), "/motion");
    bndl.add(concat).add((int32_t)self.isMotion());

    sprintf(concat, "/%06x%s", ESP.getChipId(), "/debug");
    bndl.add(concat).add(mpu_data.gravity.x).add(mpu_data.gravity.y).add(mpu_data.gravity.z);

    Udp.beginPacket(outIp, outPort);
    bndl.send(Udp); // send the bytes to the SLIP stream  
    Udp.endPacket(); // mark the end of the OSC Packet
    bndl.empty(); // empty the bundle to free room for a new one
    //=======================================================================//

    pack_count += 1;
    old_time = millis();
  }
  //=========================================================================//
  /*FIX THIS TO ONLY BE BUNDLE?*/
  /*OSC In*/
  #ifdef BUNDLE
    //Bundle example
    OSCBundle OSCin;
  #else
    // single message example
    OSCMessage OSCin;
  #endif
  
  int size;
 
  if( (size = Udp.parsePacket())>0)
  {
    while(size--)
     OSCin.fill(Udp.read());

    if(!OSCin.hasError())
    {
      OSCin.dispatch("/leds", leds);
      OSCin.dispatch("/update", update_interval);
      OSCin.dispatch("/reset", reset);
      OSCin.dispatch("/alpha", brightness);
      OSCin.dispatch("/hsl", hsl);
      OSCin.dispatch("/accelThresh", accelThresh);
      OSCin.dispatch("/gyroThresh", gyroThresh);
      OSCin.dispatch("/motionDecay", motionDecay);

    }
  }
  //=========================================================================//

  /*MPU6050 Processes*/
  mpuInterrupt = false; // reset interrupt flag and get INT_STATUS byte
  mpuIntStatus = mpu.getIntStatus();

  // get current FIFO count
  fifoCount = mpu.getFIFOCount();

  // check for overflow (this should never happen unless our code is too inefficient)
  if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
      // reset so we can continue cleanly
      mpu.resetFIFO();
      //Serial.println(F("FIFO overflow!"));
  } 
  // otherwise, check for DMP data ready interrupt (this should happen frequently)
  else if (mpuIntStatus & 0x02) {
    // wait for correct available data length, should be a VERY short wait
    while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

    // read a packet from FIFO
    mpu.getFIFOBytes(fifoBuffer, packetSize);
    
    // track FIFO count here in case there is > 1 packet available
    // (this lets us immediately read more without waiting for an interrupt)
    fifoCount -= packetSize;

    mpu.dmpGetQuaternion(&mpu_data.q, fifoBuffer);
    mpu.dmpGetGravity(&mpu_data.gravity, &mpu_data.q);
    mpu.dmpGetAccel(&mpu_data.aa, fifoBuffer);
    mpu.dmpGetLinearAccel(&mpu_data.aaReal, &mpu_data.aa, &mpu_data.gravity);
    mpu.dmpGetGyro(&mpu_data.gyro, fifoBuffer);
    mpu.dmpGetYawPitchRoll(mpu_data.ypr, &mpu_data.q, &mpu_data.gravity);
    self.update(); //update internal physical representation
  }
  //=========================================================================//
}