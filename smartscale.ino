#include "HX711.h"
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <ezButton.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>

// WiFi/endpoint setup
const char *SSID = "xxxxxxxx";
const char *PWD = "xxxxxxxx";
const int INIT_CONNECTION_IP_DISPLAY_TIME = 10000;
char buffer[250];
WebServer server(80);

//SSD1306 i2c OLED module setup
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
const unsigned int SCREEN_REFRESH_DELAY = 200;
unsigned long last_screen_refresh = 0;
unsigned short current_screen = 0;

// HX711 load cell ADC module setup
const int LOADCELL_DOUT_PIN = 33;
const int LOADCELL_SCK_PIN = 25;
// >> default calibration value, will check EEPROM for user-set val
const int DEFAULT_SCALE_CAL = 440;
const unsigned int SCALE_READING_DELAY = 800;
unsigned long last_scale_reading = 0;
int known_cal_weight_input = 0;
float current_scale_reading = 0;
int eeprom_cal = 0;
long eeprom_offset = 0;
HX711 scale;

// Button interface setup
const int SW_LEFT = 14;
const int SW_RIGHT = 12;
const unsigned int BUTTON_COUNT_DELAY = 400;
unsigned long last_count_check = 0;
ezButton btn_left(SW_LEFT, INPUT_PULLUP);
ezButton btn_right(SW_RIGHT, INPUT_PULLUP);

// Read config values for load cell
// * Address 0,1 - 16 bit int - calibration value, usually set once for a new load cell
// * Address 2,3,4,5 - 32 bit long - offset value, to preserve tare functionality after reset
void read_scale_config_from_eeprom() {
  EEPROM.begin(6);
  short hb = EEPROM.read(1);
  if(hb==255) eeprom_cal = 0;
  else {
    short lb = EEPROM.read(0);
    eeprom_cal = lb | hb << 8; 
  }
  short b1 = EEPROM.read(2);
  short b2 = EEPROM.read(3);
  short b3 = EEPROM.read(4);
  short b4 = EEPROM.read(5);
  eeprom_offset = ((b1 << 24) + (b2 << 16) + (b3 << 8) + (b4));
  EEPROM.end();
}

// Set the calibration on the HX711 library and persist to EEPROM
void set_scale_calibration() {
  EEPROM.begin(6);
  float sample = scale.get_units(10);
  int cal_value = int(sample/float(known_cal_weight_input));
  scale.set_scale(float(cal_value));
  short hb = highByte(cal_value);
  short lb = lowByte(cal_value);
  EEPROM.write(1,hb);
  EEPROM.write(0,lb);
  current_screen = 0;
  EEPROM.end();
}

// Set the tare on HX711 library and persist to EEPROM
void set_scale_tare() {
  EEPROM.begin(6);
  scale.tare(10);
  long offset = scale.get_offset();
  short b1 = (int)((offset >> 24) & 0xFF);
  short b2 = (int)((offset >> 16) & 0xFF);
  short b3 = (int)((offset >> 8) & 0xFF);
  short b4 = (int)(offset & 0xFF);
  EEPROM.write(2,b1);
  EEPROM.write(3,b2);
  EEPROM.write(4,b3);
  EEPROM.write(5,b4);
  EEPROM.end();
}

// Initialize scale with HX711 library, check for existing config in EEPROM
void setup_scale() {
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  read_scale_config_from_eeprom();
  scale.set_scale(eeprom_cal ? float(eeprom_cal) : float(DEFAULT_SCALE_CAL));
  if(eeprom_offset) scale.set_offset(eeprom_offset);
}

// Runs in loop - refresh the scale reading from HX711
void get_scale_reading() {
  if(millis() - last_scale_reading > SCALE_READING_DELAY && current_screen == 0) {
    current_scale_reading = scale.get_units(4);
    last_scale_reading = millis();
  }
}

// Process button input from the scale's original 2 buttons
// >> Individual button presses are debounced and handled by ezbutton library
// >> We also do a further "debounce" to check for multi-press/combos
void process_btn_input() {
  btn_left.loop();
  btn_right.loop();
  if(millis() - last_count_check > BUTTON_COUNT_DELAY) {
    //calibration process is active
    if(current_screen==1) {
      //save calibration and return to scale screen if both buttons pressed
      if(btn_left.getCount()==1&&btn_right.getCount()==1) {
        set_scale_calibration();
      }
      //left (UNIT) button - decrease value
      else if(btn_left.getCount()>=1&&!btn_right.getCount()) {
          if(btn_left.getCount()==2 && known_cal_weight_input>10) known_cal_weight_input -= 10;
          else if(known_cal_weight_input>0) known_cal_weight_input--;         
      }  
      //right (TARE/PWR) button - increase value
      else if(btn_right.getCount()>=1&&!btn_left.getCount()) {
          if(btn_right.getCount()==2 && known_cal_weight_input<19990) known_cal_weight_input += 10;
          else if(known_cal_weight_input<20000) known_cal_weight_input++;
      }    
    }
    else {
      //start calibration if both buttons pressed
      if(btn_left.getCount()==1&&btn_right.getCount()==1) {
        scale.set_scale();
        scale.tare();
        current_screen = 1;
      }
      //left (UNIT) button only pressed
      else if(btn_left.getCount()>=1&&!btn_right.getCount()) {
        // current_screen = 2;
      }  
      //right (TARE/PWR) button only pressed
      else if(btn_right.getCount()>=1&&!btn_left.getCount()) {
        set_scale_tare();
      }    
    }
    btn_right.resetCount();
    btn_left.resetCount();
    last_count_check = millis();
  }
}

// Handler for http endpoint for current scale value
void get_reading_for_web() {
  String response = "{\"val\":\"" + String(current_scale_reading) + "\"}";
  response.toCharArray(buffer,250);
  server.send(200, "application/json", buffer);
}

// Set up wifi connectivity and web server
void setup_web_server() {
  WiFi.begin(SSID, PWD);
  display.clearDisplay();    
  display.setTextSize(1);           
  display.setTextColor(SSD1306_WHITE); 
  display.setCursor(0,0);
  display.print(F("Connecting"));
  while (WiFi.status() != WL_CONNECTED) {
    display.print(".");
    display.display();
    delay(400);
  }
  display.clearDisplay();    
  display.setCursor(0,0);            
  display.print(F("Connected to WiFi!"));
  display.drawLine(0,10,display.width()-1, 10, SSD1306_WHITE);
  display.setCursor(8,12);
  display.println(F("IP Address:"));
  display.setCursor(8,24);
  display.println(WiFi.localIP());
  display.display();
  server.on("/read_scale", get_reading_for_web);	
  server.begin();
  // Once connected, pause for a preconfigured amount of time to give user a chance
  // to see IP address.
  delay(INIT_CONNECTION_IP_DISPLAY_TIME);
}

void setup_display() {
    // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    // Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }
}

void setup() {
  btn_left.setDebounceTime(50);
  btn_right.setDebounceTime(50);
  setup_scale();
  setup_display();
  setup_web_server();
}

// Calibration screen, shown when user presses both buttons at once
void screen_calibrate() {
    display.clearDisplay();    
    display.setTextSize(1);             
    display.setTextColor(SSD1306_WHITE);        
    display.setCursor(0,0);
    display.println(F("  Scale Calibration"));
    display.drawLine(0,10,display.width()-1, 10, SSD1306_WHITE);
    display.setCursor(8,18);
    display.print(F("Known val (g):"));
    display.setCursor(100,18);
    display.println(known_cal_weight_input);
    display.display();
}

// Default screen, display the current scale reading in grams
void screen_scale_reading() {
    display.clearDisplay();    
    display.setTextSize(1);           
    display.setTextColor(SSD1306_WHITE);  
    display.setCursor(0,0);
    display.println(F("Filament Remaining:"));
    display.drawLine(0,10,display.width()-1, 10, SSD1306_WHITE);
    display.setCursor(6, 18);
    display.print("g");
    display.setCursor(16,18);
    display.setTextSize(2);
    display.print(current_scale_reading, 1);
    display.display();
}

void loop() {
  server.handleClient();	
  process_btn_input();
  get_scale_reading();
  if(millis() - last_screen_refresh > SCREEN_REFRESH_DELAY) {
    switch(current_screen) {
      case 1:
        screen_calibrate();
      break;
      default:
        screen_scale_reading();
      break;
    }
    last_screen_refresh = millis();
  }
}
