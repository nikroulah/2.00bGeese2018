#include <Servo.h>
#include <I2Cdev.h>
#include <MPU6050.h>
#include <Adafruit_NeoPixel.h>
#include "SD.h"
#define SD_ChipSelectPin 4
#include "TMRpcm.h"
#include "SPI.h"

#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    #include "Wire.h"
#endif

#define MIC A6
#define FSR A3
#define VIB 5
#define SPK 9
#define SV1 A0
#define SV2 A1
#define SV3 A2

unsigned long LONG_MAX = 4294967295;

//threshold values to test against
int32_t IDLE_ACCEL = 16000; // TODO figure out this value: equal to gravity
int32_t IDLE_GAIN = 250;

int32_t shout_thr = 65;
int32_t speech_thr = 15;
int32_t toss_thr = 3000;
int32_t throw_thr = 15000;
int32_t slam_thr = 20000;
int16_t hug_thr = 700;


//speech filter vars:
float speech_alpha = 0.1; 
float speech_past=0;
float speech_updated;

float hug_alpha = 0.1; 
float hug_past=0;
float hug_updated;

float accel_alpha = 0.1; 
float accel_past=0;
float accel_updated;

const unsigned long LONG_HUG_TIME = 5000;

//input detection variables
int speech_state = 0; //0 = no speech, 1 = whisper, 2 = talking, 3 = shouting 
int accel_state = 0; //0 = no acceleration, 1 = toss, 2 = throw, 3 = slam
int hug_state = 0; //0 = no hug, 1 = short hug, 2 = long hugno
unsigned long hug_start_time = 0; 
unsigned long vibrate_start_time = 0;
bool vibrate_on = false;


//circular buffers for better input handling
const byte BUF_SIZE = 8;
float speech_buf[BUF_SIZE] = {};
float* speech_wr = speech_buf;
float accel_buf[BUF_SIZE] = {};
float* accel_wr = accel_buf;


//Tracking variables
const byte idle = 0;
const byte happy = 1;
const byte sad = 2;
const byte excited = 3;
const byte angry = 4;
const byte wake = 5; 

byte prevState = 0;
byte state = 0;
bool asleep = true;
bool canAct = true;

unsigned long actionStart = LONG_MAX;

boolean active = false;
unsigned long timeSinceIdle = LONG_MAX;

//action states
const byte idling = 0;
const byte starting = 1;
const byte executing = 2;
const byte finishing = 3;
byte actionState = 0;

//Servo Creation
Servo servo1;
Servo servo2;
Servo servo3;

//LEDS creation
Adafruit_NeoPixel strip = Adafruit_NeoPixel(60, 6, NEO_GRB + NEO_KHZ800);

//Accelerometer Creation
MPU6050 accelgyro;
int16_t ax, ay, az;
int16_t gx, gy, gz;

//Speaker
TMRpcm tmrpcm;

uint16_t printCount = 0;

void debugPrint(String msg) {
  if (printCount % 1 == 0) {
    Serial.print(msg);
  }
}

void debugPrintln(String msg) {
  if (printCount % 1 == 0) {
    Serial.println(msg);
  }
}

void setup() {
  Serial.begin(9600);
  debugPrintln("setup()");
  tmrpcm.speakerPin = SPK;
  if (!SD.begin(SD_ChipSelectPin)) {
    Serial.println("SD fail");
    return;
  }
  tmrpcm.setVolume(7);
  //Initialize all servos
  debugPrintln("initializing servos...");
  servo1.attach(SV1);
  servo2.attach(SV2);
  servo3.attach(SV3);

  //Create Accelerometer Communication
  #if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    Wire.begin();
  #elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
    Fastwire::setup(400, true);
  #endif

  debugPrintln("initializing I2C...");
  accelgyro.initialize();

  debugPrintln("testing MPU6050 device connection...");
  debugPrintln(accelgyro.testConnection() ? "success" : "failure");
  analogWrite(VIB, 0);
  strip.begin();
  strip.show();
  
}

void speech_buf_write(float input) {
  *speech_wr = input;
  if (++speech_wr >= speech_buf+BUF_SIZE) {
    speech_wr = speech_buf;
  }
}

void accel_buf_write(float input) {
  *accel_wr = input;
  if (++accel_wr >= accel_buf+BUF_SIZE) {
    accel_wr = accel_buf;
  }
}

float* speech_buf_get() {
  float mn = 500000;
  float mx = 0;
  for (float* i = speech_buf; i < speech_buf+BUF_SIZE; ++i) {
    float el = abs(*i);
    if (el > mx) {
      mx = el;
    }
    if (el < mn) {
      mn = el;
    }
  }
  float result[] = {mn, mx};
  return result;
}

float* accel_buf_get() {
  float mn = 500000;
  float mx = 0;
  for (float* i = accel_buf; i < accel_buf+BUF_SIZE; ++i) {
    float el = abs(*i);
    if (el > mx) {
      mx = el;
    }
    if (el < mn) {
      mn = el;
    }
  }
  float result[] = {mn, mx};
  return result;
}

float speech_lowpass_step(float input) {
       speech_updated = input*(1-speech_alpha) + speech_past*(speech_alpha);
       speech_past = speech_updated;
       return speech_updated;
}
float hug_lowpass_filter(float input) {
       hug_updated = input*(1-hug_alpha) + hug_past*(hug_alpha);
       hug_past = hug_updated;
       return hug_updated;
}
float accel_lowpass_filter(float input) {
       accel_updated = input*(1-accel_alpha) + accel_past*(accel_alpha);
       accel_past = accel_updated;
       return accel_updated;
}

int detect_hug(int reading){
  
  reading = hug_lowpass_filter(abs(reading));
  
  if (hug_thr < reading && hug_state != 1){
    hug_state = 1;
    hug_start_time = millis();
    debugPrintln("hug detected");
    return 1;
  } else if (hug_thr < reading && millis() - hug_start_time >= LONG_HUG_TIME && hug_state != 2){
    hug_state = 2;
    debugPrintln("long hug detected");
    return 2;
  } else if (reading < hug_thr && hug_state != 0) {
    hug_state = 0;
    debugPrintln("end of hug");
    return hug_state;
  }
  return hug_state;
}


int detect_speech(int reading){
  
  float filtered_mic = abs(speech_lowpass_step(reading));
  speech_buf_write(filtered_mic);

  float* minmax = speech_buf_get();
  float mn = *minmax;
  float mx = *(minmax+1);
  
//  debugPrint("filtered_mic: ");
//  debugPrintln(String(filtered_mic));
  
  if (shout_thr <= mn && shout_thr <= mx && speech_state != 3){
    speech_state = 3;
    debugPrintln("shouts detected");
    return 3;
  } else if (speech_thr <= mn && speech_thr <= mx && speech_state != 2){
    speech_state = 0;
    debugPrintln("speech detected");
    return 2;
  } else if (mn < speech_thr && mx < speech_thr && speech_state != 0){
    speech_state = 0;
    debugPrintln("end of speech detected");
    return 0;
  }
//  debugPrint("no speech change, current state is: ");
//  debugPrintln(String(speech_state));
  return speech_state;
  
}

int detect_accel(int reading) {
  
  reading = accel_lowpass_filter(abs(reading));
  accel_buf_write(reading);

  float* minmax = accel_buf_get();
  float mn = *minmax;
  float mx = *(minmax+1);
  
  if (slam_thr <= mn && slam_thr <= mx && accel_state != 3) {
    accel_state = 3; 
    debugPrintln("slam detected");
    return 3;
  } else if (throw_thr <= mn && throw_thr <= mx && accel_state != 2) {
      accel_state = 2;  
      debugPrintln("throw detected");
      return 2;
  } else if (toss_thr <= mn && toss_thr <= mx && accel_state != 1) {
      accel_state = 1;  
      debugPrintln("toss detected");
      return 1;
  } else if (mn < toss_thr && mx < toss_thr && accel_state != 0) {
      accel_state = 0;
      debugPrintln("end of toss detected");
      return 0;
  }
//  debugPrint("no toss change, current state is: ");
//  debugPrintln(String(accel_state));
  return 0;
  
}

void setState(byte s) {
  prevState = state;
  state = s;
  canAct = false;
  actionState = starting;
}

void doAction() {
  switch (actionState) {
    case idling:
      laze();
      break;

    case starting:
      start();
      actionState = executing;
      break;

    case executing:
      execute();
      break;

    case finishing:
      finish();
      actionState = idling;
      canAct = true;
      break;
  }
}

void laze() {
  // idle actions, if any
}

void start() {
  // called on action start
  actionStart = millis();
}

void execute() {
  // called continuously through action
  unsigned long t = millis() - actionStart;
  switch(state) {
    case excited:
      wobble();
      setLights(1,1.0); 
      tmrpcm.play("HL1_S.WAV");
      if (t < 200) {
        vibrate_on = true;
      } else if (t < 400) {
        setLights(1,1.0); 
        vibrate_on = false;
      } else if (t < 600) {
        setLights(1,1.0); 
        vibrate_on = true;
      } else if (t < 800) {
        setLights(1,1.0); 
        vibrate_on = false;
      } else {
        setLights(1,1.0); 
        actionState = finishing;
      }
      break;

    case happy:
      setLights(2,0.7);
      wobble();
      tmrpcm.play("HL1_S.WAV");
      if (t < 200) {
        vibrate_on = true;
      } else if (t < 600) {
        setLights(2,0.7);
        vibrate_on = false;
      } else if (t < 1000) {
        setLights(2,0.7);
      } else {
        setLights(2,0.7);
        actionState = finishing;
      }
      break;

    case angry:
      push(true);
      setLights(0,0.9);
      if (t < 200) {
        vibrate_on = true;
      } else if (t < 400) {
        setLights(0,0.9);
        push(false);
      } else if (t < 600) {
        setLights(0,0.9);
        push(true);
      } else if (t < 800) {
        setLights(0,0.9);
      } else {
        setLights(0,0.9);
        vibrate_on = false;
        push(false);
        actionState = finishing;
      }
      break;

    case sad:
      setLights(4,0.3);
      if (t < 500) {
        wobble();
        setLights(4,0.3);
        vibrate_on = true;
        vibrate_on = false;
      } else if (t < 1000) {
        setLights(4,0.3);
      } else {
        setLights(4,0.3);
        actionState = finishing;
      }
      break;
    
    case wake:
      push(true);
      setLights(5,1.0); 
      setState(excited);
      break;
  }
}

void finish() {
  actionStart = LONG_MAX;
}

void setLights(int color, float brightness){
  int red, blue, green; 
  switch(color){
    case 0:
     //RED
     red = 255;
     green = 0;
     blue = 0;
     break;
    case 1:
     //ORANGE
     red = 255;
     green = 153;
     blue = 51;
     break;
    case 2:
      //YELLOW
      red = 255;
      green = 255;
      blue = 0;
      break;
    case 3:
      //GREEN
      red = 0;
      green = 220;
      blue = 20;
      break;
    case 4:
      //BLUE
      red = 51;
      green = 51;
      blue = 255;
      break;
    case 5:
      //WHITE
      red = 255;
      green = 255;
      blue = 255;
      break;
  }
  float prob = brightness *100;
  for (int i = 0; i < 30; i++) {
    if (rand() %100 <= prob) {
      strip.setPixelColor(i, red,blue, green);
    }
  }
  strip.show();
}

void set_servos(int servo1_val, int servo2_val, int servo3_val) {
  servo1.write(servo1_val);
  servo2.write(servo2_val);
  servo3.write(servo3_val);
}

void wobble() {
    int modulod_by = 1080;
    unsigned long current = millis() % modulod_by;
    if (current %2 == 0) {
    int reading_1 = servo1.read();
    int reading_2 = servo2.read();
    int reading_3 = servo3.read();
    if (current < (modulod_by/3)) {
      servo1.write(reading_1+1);
      servo2.write(reading_2-1);
      servo3.write(reading_3-1);
    } else if (current < (2 * modulod_by/3)) {
      servo1.write(reading_1-1);
      servo2.write(reading_2+1);
      servo3.write(reading_3-1);
    } else {
      servo1.write(reading_1-1);
      servo2.write(reading_2-1);
      servo3.write(reading_3+1);
    }
  }
}


void push(bool up){
  if (up) {
    servo1.write(7);
    servo2.write(7);
    servo3.write(7);
  } else {
    servo1.write(90);
    servo2.write(90);
    servo3.write(90);
  }
}

void loop() {
  
  //Check each sensor's value
  accelgyro.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
  int gain = abs(analogRead(MIC)-IDLE_GAIN); // range 0-1023
  detect_speech(gain);
  
  int32_t accel_mag = pow(pow(ax,2)+pow(ay,2)+pow(az,2), .5)-IDLE_ACCEL;
  detect_accel(accel_mag);

  int force = analogRead(FSR);
  detect_hug(force);
  
  unsigned long currentTime = millis();

  debugPrint("accel_mag = ");
  debugPrint(String(accel_mag));
  debugPrint(",\tgain = ");
  debugPrint(String(gain));
  debugPrint(",\tforce = ");
  debugPrintln(String(force));

//  return;

//  if (canAct)
//    setState(excited);
//  doAction();
//  return;
  
  //Check sensor values against the thresholds
  //If a sensor is within a threshold, do the action associated with said threshold.
  if (canAct) {
    timeSinceIdle = currentTime;
  } else if (timeSinceIdle + 1500 > currentTime && canAct || hug_state == 2) {
    setState(idle);
    asleep = true;
  }

  if (canAct) {
    if (hug_state == 1 && asleep) {
  //    setState(wake);
      asleep = false;
    } else if (accel_state == 3) {
      setState(angry);
    } else if (accel_state == 1 || speech_state == 2) {
      setState(happy);
    } else if (accel_state == 2 || speech_state == 3) {
      setState(sad);
    } else if (speech_state == 1 || hug_state == 1) {
      setState(happy);
    }
  }

  if(vibrate_on) {
    analogWrite(VIB, 153);
  } else {
    analogWrite(VIB, 0);
  }

  printCount++;
  doAction();
}

