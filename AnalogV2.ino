/*
 * use board teensy 3.2
 * select Serial + MIDI
 * 72 Mhz
 */

#include <Control_Surface.h>

#include <IntervalTimer.h>

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans12pt7b.h>

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);


#define MINIMUM_BPM 40
#define MAXIMUM_BPM 300
int bpm = 120;
int tempo_pos = 0;
#define TEMPO_PIN 6
#define TAP_PIN 11
#define MINIMUM_TAPS 3
#define EXIT_MARGIN 150 // If no tap after 150% of last tap interval -> measure and set

#define CLOCKS_PER_BEAT 24
IntervalTimer tempoTimer;

long minimumTapInterval = 60L * 1000 * 1000  / MAXIMUM_BPM;
long maximumTapInterval = 60L * 1000 * 1000  / MINIMUM_BPM;

volatile long firstTapTime = 0;
volatile long lastTapTime = 0;
volatile long timesTapped = 0;

USBMIDI_Interface usbmidi;  // Instantiate a MIDI Interface to use
HardwareSerialMIDI_Interface serialmidi {Serial1, MIDI_BAUD};
MIDI_PipeFactory<2> pipes;

// Instantiate an analog multiplexer
CD74HC4067 mux {
  A1,       // Analog input pin
  {2, 3, 4, 5} // Address pins S0, S1, S2
};
 
// Create an array of CCPotentiometer objects that send out MIDI Control Change 
// messages when you turn the potentiometers connected to the 8 inputs of the mux.
CCPotentiometer Potentiometers[] {
  { mux.pin(0), { MIDI_CC::Modulation_Wheel, CHANNEL_1 } }, // MOD
  { mux.pin(1), { MIDI_CC::General_Purpose_Controller_4, CHANNEL_1 } }, // DELAY
  { mux.pin(2), { MIDI_CC::General_Purpose_Controller_3, CHANNEL_1 } }, // REVERB
  { mux.pin(3), { MIDI_CC::Sound_Controller_6, CHANNEL_1 } }, // M2
  { mux.pin(4), { MIDI_CC::Sound_Controller_7, CHANNEL_1 } }, // M3
  { mux.pin(5), { MIDI_CC::Channel_Volume, CHANNEL_1 } }, // MASTER
  //{ mux.pin(6), { MIDI_CC::Effects_3, CHANNEL_1 } }, // TEMPO
  { mux.pin(7), { MIDI_CC::Sound_Controller_8, CHANNEL_1 } }, // M4
  { mux.pin(8), { MIDI_CC::Sound_Controller_5, CHANNEL_1 } }, // M1
};


CCButton   buttons[] {
  { 12, { 115, CHANNEL_1 } }, //NEXT
  { 10, { 116, CHANNEL_1 } }  //PREV
};

void updateBPM(int bpm) {
  display.setFont(&FreeSans18pt7b);
  display.fillRect(0, 0, 69, 64, SSD1306_BLACK);
  display.setCursor(0, 26);  
  display.print(bpm);
  display.display();

  tempoTimer.update(calculateIntervalMicroSecs(bpm));
}

void tapInput() {
  long now = micros();
  if (now - lastTapTime < minimumTapInterval) {
    return; // Debounce
  }

  if (timesTapped == 0) {
    firstTapTime = now;
  }

  timesTapped++;
  lastTapTime = now;
  Serial.println("Tap!");
}

long calculateIntervalMicroSecs(int bpm) {
  // Take care about overflows!
  return 60L * 1000 * 1000 / bpm / CLOCKS_PER_BEAT;
}

void sendClockPulse() {
    //Control_Surface.sendSysCommon
    usbmidi.sendRealTime(MIDIMessageType::TIMING_CLOCK);
    //Serial.println("MIDI CLK");
}


void setup() {

  Serial.begin(115200);

  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  // TODO read BPM from eeprom ??

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(&FreeSans12pt7b);
  display.setCursor(70, 26);
  display.print("bpm");
  updateBPM(bpm);

  // Attach the interrupt to send the MIDI clock and start the timer
  tempoTimer.begin(sendClockPulse, calculateIntervalMicroSecs(bpm));
  
  tempo_pos = mux.analogRead(TEMPO_PIN);
  pinMode(TAP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TAP_PIN), tapInput, CHANGE);

  // invert pot
  for (int i=0; i<8; i++) {
    Potentiometers[i].invert();
  }

  Control_Surface >> pipes >> usbmidi;    // Output to usbmidi
  usbmidi << pipes << serialmidi; // Input from serialmidi
  Control_Surface.begin();  // Initialize the Control Surface
}

void loop() {
  Control_Surface.loop();  // Update the Control Surface

  // Tempo POT
  int read = 8191 - mux.analogRead(TEMPO_PIN);
  if (abs (read - tempo_pos) > 96) {
    int new_bpm = map(read, 0, 8191, MINIMUM_BPM, MAXIMUM_BPM);
    tempo_pos = read;
    if (new_bpm != bpm) {
      bpm = new_bpm;
      Serial.printf("bpm=%d read=%d\n", bpm, read);
      updateBPM(bpm);
    }
  }

  // TAP Tempo  
  long now = micros();
  if (timesTapped > 0 && timesTapped < MINIMUM_TAPS && (now - lastTapTime) > maximumTapInterval) {
    // Single taps, not enough to calculate a BPM -> ignore!
    //    Serial.println("Ignoring lone taps!");
    timesTapped = 0;
  } else if (timesTapped >= MINIMUM_TAPS) {
    long avgTapInterval = (lastTapTime - firstTapTime) / (timesTapped - 1);
    if ((now - lastTapTime) > (avgTapInterval * EXIT_MARGIN / 100)) {
      int new_bpm = (int) 60L * 1000 * 1000 / avgTapInterval;
      updateBPM((int) new_bpm);
      
      timesTapped = 0;
    }
  } 
}
