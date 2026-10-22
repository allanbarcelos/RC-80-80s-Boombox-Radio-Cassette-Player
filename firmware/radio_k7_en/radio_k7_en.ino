/*
  RC-80 — 80s AM/FM radio cassette player with Arduino Nano
  ---------------------------------------------------------------
  Radio:      Si4732-A10 (I2C, "PU2CLR SI4735" library)
  Tuning:     the TUNING knob turns the capstan; the dial cord carries the needle and
              turns the multi-turn potentiometer (10 turns) -> the Arduino reads the
              needle position and tunes the Si4732 to the frequency it is pointing at.
  Tape:       CRP42602Y logic mechanism (auto-reverse, 12 V motor, 9 V solenoid)
              Solenoid sequences based on the crp42602y_ctrl library by
              Elehobica (BSD-2-Clause) — https://github.com/elehobica/pico_crp42602y_ctrl
  Keys:       6 piano keys on top, read through a resistor ladder on A7.
  Door:       passive latch + SG90 micro servo that releases it (STOP/EJ).
  Power:      the switch on the VOLUME pot turns everything on/off (via a MOSFET).

  Pins (Arduino Nano):
    D2  ROTATION_SENS  (CRP42602Y pin 2)               input, pull-up
    D3  latch servo (SG90, orange signal wire)
    D4  GEAR_STATUS_SW (CRP42602Y pin 3)               input, pull-up (LOW = in function)
    D5  CASSETTE_DETECT_SW (CRP42602Y pin 7)           input, pull-up (LOW = cassette present)
    D6  SOLENOID   -> IRLZ44N gate (HIGH = pull)
    D7  MOTOR 12 V -> base of the BC337 that drives the IRF9540N (HIGH = motor on)
    D8  audio RELAY (HIGH = tape, LOW = radio)
    D9  amplifier MUTE (see AMP_MUTE_LEVEL)
    D10 "FM STEREO" LED (with 1 kOhm)
    D12 Si4732 RESET
    A0  tuning multi-turn potentiometer (wiper)
    A1  BAND switch microswitch (closed = FM)
    A4  SDA  -> level shifter -> Si4732 SDIO
    A5  SCL  -> level shifter -> Si4732 SCLK
    A6  battery voltage (100k / 33k divider)
    A7  keys (resistor ladder, 10k pull-up)

  Dial calibration (once): hold STOP/EJ and switch the radio on.
    LED blinks slowly: move the needle to 88 and press PLAY.
    LED blinks fast:   move the needle to 108 and press PLAY. Done (saved in EEPROM).
*/
#include <SI4735.h>
#include <Servo.h>
#include <EEPROM.h>

// ------------------------------------------------------------ pins
const uint8_t PIN_ROT = 2, PIN_SERVO = 3, PIN_GEAR = 4, PIN_CASS = 5;
const uint8_t PIN_SOL = 6, PIN_MOTOR = 7, PIN_RELAY = 8, PIN_MUTE = 9, PIN_LED = 10, PIN_RESET = 12;
const uint8_t PIN_TUNE = A0, PIN_BAND = A1, PIN_VBAT = A6, PIN_KEYS = A7;

// ------------------------------------------------------------ settings
const bool    AMP_MUTE_LEVEL = HIGH;      // level that mutes the amplifier (PAM8610: MUTE active HIGH)
const uint8_t SERVO_REST = 20;            // arm away from the pawl
const uint8_t SERVO_RELEASE = 75;         // arm lifts the pawl and releases the door
const float   VBAT_K = (100.0 + 33.0) / 33.0 * 5.0 / 1023.0;
const float   VBAT_LOW = 9.9, VBAT_EMPTY = 9.3;
const uint16_t FM_MIN = 8800, FM_MAX = 10800;   // in 10 kHz units
const uint8_t  FM_DEEMPH = 2;               // 2 = 75 us (Americas), 1 = 50 us (Europe and most other regions)
// AM band: 10 kHz steps in the Americas (530-1600, matches the printed dial).
// Europe/Asia/Africa/Oceania use 9 kHz: set AM_STEP = 9, AM_MIN = 531, AM_MAX = 1602.
const uint16_t AM_MIN = 530,  AM_MAX = 1600;    // in kHz
const uint8_t  AM_STEP = 10;                    // in kHz
const uint32_t MOTOR_IDLE_OFF_MS = 30000;       // switch the idle motor off after this time

// keys: STOP/EJ, PLAY, REW, F.FWD, PAUSE, A-B (resistors 0, 1k, 2k2, 3k9, 6k8, 15k)
enum Key { K_NONE = -1, K_STOP = 0, K_PLAY, K_REW, K_FF, K_PAUSE, K_AB };
const int KEY_ADC[6] = {0, 93, 184, 288, 414, 614};

SI4735 rx;
Servo latch;

// ------------------------------------------------------------ calibration (EEPROM)
struct Cal { uint16_t magic, lo, hi; } cal;
const uint16_t CAL_MAGIC = 0x52C8;

// ------------------------------------------------------------ state
enum Deck { D_STOP, D_PLAY, D_FF, D_REW };
Deck deck = D_STOP;
bool headA = true;            // side A (true) / side B (false)
bool paused = false;
bool motorOn = false;
bool tapeAudio = false;
uint32_t lastGearMs = 0, lastActivityMs = 0;
volatile uint32_t lastRotMs = 0;
bool bandFM = true;
uint16_t curFreq = 0;
float tuneFilt = -1;

void onRotation() { lastRotMs = millis(); }

// ------------------------------------------------------------ helpers
void ampMute(bool m) { digitalWrite(PIN_MUTE, m ? AMP_MUTE_LEVEL : !AMP_MUTE_LEVEL); }
bool gearInFunc() { return digitalRead(PIN_GEAR) == LOW; }
bool hasCassette() { return digitalRead(PIN_CASS) == LOW; }
void sol(bool pull) { digitalWrite(PIN_SOL, pull ? HIGH : LOW); }
float vbat() { return analogRead(PIN_VBAT) * VBAT_K; }

void motor(bool on) {
  if (on && !motorOn) { digitalWrite(PIN_MOTOR, HIGH); motorOn = true; delay(500); }   // let the motor settle
  if (!on && motorOn) { digitalWrite(PIN_MOTOR, LOW); motorOn = false; }
}

void selectTape(bool tape) {
  if (tape == tapeAudio) return;
  ampMute(true); delay(30);
  digitalWrite(PIN_RELAY, tape ? HIGH : LOW);
  rx.setAudioMute(tape);
  delay(60); ampMute(false);
  tapeAudio = tape;
}

// ------------------------------------------------------------ CRP42602Y
// Function sequence: the gear turns 190 degrees in ~400 ms; the solenoid selects the
// head direction, whether the head engages (PLAY) and the reel direction.
bool gearFunc(bool headDirA, bool liftHead, bool reelFwdA) {
  motor(true);
  lastGearMs = millis();
  sol(true);       delay(20);
  sol(!headDirA);  delay(80);
  sol(false);      delay(50);
  sol(liftHead);   delay(150);
  sol(reelFwdA);   delay(100);
  sol(false);      delay(20);
  for (uint8_t i = 0; i < 15; i++) { if (gearInFunc()) return true; delay(20); }
  return false;
}
// Return sequence (stop): the rest of the gear turn, ~360 ms
bool gearReturn() {
  motor(true);
  lastGearMs = millis();
  sol(true); delay(20); sol(false); delay(360);
  for (uint8_t i = 0; i < 15; i++) { if (!gearInFunc()) return true; delay(20); }
  return false;
}

void deckStop() {
  if (gearInFunc()) gearReturn();
  deck = D_STOP; paused = false;
  selectTape(false);
}
void deckPlay(bool sideA) {
  if (!hasCassette()) return;
  if (paused) { paused = false; motor(true); return; }
  if (gearInFunc()) gearReturn();
  headA = sideA;
  selectTape(true);
  if (gearFunc(headA, true, headA)) { deck = D_PLAY; lastRotMs = millis(); }
}
void deckWind(bool forward) {   // FF/REW relative to the side being played
  if (!hasCassette()) return;
  if (gearInFunc()) gearReturn();
  ampMute(true);
  bool reelA = forward ? headA : !headA;
  if (gearFunc(headA, false, reelA)) { deck = forward ? D_FF : D_REW; lastRotMs = millis(); }
}
void ejectDoor() {
  deckStop();
  latch.attach(PIN_SERVO);
  latch.write(SERVO_RELEASE); delay(450);
  latch.write(SERVO_REST);    delay(350);
  latch.detach();             // no pulses: the servo neither buzzes nor draws current
}

// end of tape: the reels stop turning
void checkTapeEnd() {
  if (deck == D_STOP || paused || !gearInFunc()) return;
  uint32_t now = millis();
  if (now - lastGearMs < 1200) return;
  noInterrupts(); uint32_t lr = lastRotMs; interrupts();
  if (now - lr < 1500) return;
  if (deck == D_PLAY && headA) deckPlay(false);       // auto-reverse: side A finished -> play side B
  else deckStop();                                     // side B finished, or FF/REW reached the end
  if (deck == D_STOP) ampMute(false);
}

// ------------------------------------------------------------ keys
int readKey() {
  int v = analogRead(PIN_KEYS);
  for (int i = 0; i < 6; i++) if (abs(v - KEY_ADC[i]) < 40) return i;
  return K_NONE;
}
int keyEvent() {               // returns the key once per press (debounced)
  static int last = K_NONE, stable = K_NONE; static uint32_t t = 0;
  int k = readKey();
  if (k != last) { last = k; t = millis(); return K_NONE; }
  if (millis() - t > 30 && k != stable) { stable = k; return k; }
  return K_NONE;
}

// ------------------------------------------------------------ tuning
uint16_t readTuneRaw() {        // average of 64 readings (more resolution, less noise)
  uint32_t s = 0;
  for (uint8_t i = 0; i < 64; i++) s += analogRead(PIN_TUNE);
  return s / 16;                // 0..4092
}
uint16_t posToFreq(float pos) {
  float t = (pos - cal.lo) / float(cal.hi - cal.lo);
  t = constrain(t, 0.0, 1.0);
  if (bandFM) {
    uint16_t f = FM_MIN + t * (FM_MAX - FM_MIN);
    return (f + 5) / 10 * 10;                            // 100 kHz steps
  }
  float f = AM_MIN * pow(float(AM_MAX) / AM_MIN, t);     // the AM scale on the dial is logarithmic
  return AM_MIN + (uint16_t(f) - AM_MIN + AM_STEP / 2) / AM_STEP * AM_STEP;
}
void setBand(bool fm) {
  bandFM = fm;
  if (fm) { rx.setFM(8400, 10800, FM_MIN, 10); rx.setFMDeEmphasis(FM_DEEMPH); rx.setFmStereoOn(); }
  else    { rx.setAM(520, 1710, AM_MIN, AM_STEP); rx.setAvcAmMaxGain(48); }
  rx.setVolume(63);
  curFreq = 0; tuneFilt = -1;
}
void tuneLoop() {
  float raw = readTuneRaw();
  tuneFilt = tuneFilt < 0 ? raw : tuneFilt * 0.8 + raw * 0.2;
  uint16_t f = posToFreq(tuneFilt);
  if (f != curFreq) { rx.setFrequency(f); curFreq = f; }
}

void calibrate() {
  for (uint8_t step = 0; step < 2; step++) {
    while (readKey() != K_NONE) delay(10);
    uint16_t blink = step == 0 ? 500 : 120;
    while (readKey() != K_PLAY) { digitalWrite(PIN_LED, (millis() / blink) & 1); delay(5); }
    uint16_t v = readTuneRaw();
    if (step == 0) cal.lo = v; else cal.hi = v;
  }
  cal.magic = CAL_MAGIC;
  EEPROM.put(0, cal);
  digitalWrite(PIN_LED, LOW);
}

// ------------------------------------------------------------ setup / loop
void setup() {
  pinMode(PIN_ROT, INPUT_PULLUP); pinMode(PIN_GEAR, INPUT_PULLUP); pinMode(PIN_CASS, INPUT_PULLUP);
  pinMode(PIN_BAND, INPUT_PULLUP);
  pinMode(PIN_SOL, OUTPUT); pinMode(PIN_MOTOR, OUTPUT); pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_MUTE, OUTPUT); pinMode(PIN_LED, OUTPUT);
  sol(false); digitalWrite(PIN_MOTOR, LOW); digitalWrite(PIN_RELAY, LOW);
  ampMute(true);
  attachInterrupt(digitalPinToInterrupt(PIN_ROT), onRotation, CHANGE);

  EEPROM.get(0, cal);
  if (cal.magic != CAL_MAGIC || cal.hi <= cal.lo + 200) { cal.lo = 400; cal.hi = 3700; }
  if (readKey() == K_STOP) {                               // STOP/EJ held at power-up
    delay(300);
    if (readKey() == K_STOP) calibrate();
  }

  Wire.begin();
  rx.getDeviceI2CAddress(PIN_RESET);
  rx.setup(PIN_RESET, 0);                                  // 0 = FM
  delay(300);
  setBand(digitalRead(PIN_BAND) == LOW);
  if (gearInFunc()) gearReturn();                          // switched off while playing: return to STOP
  motor(false);
  ampMute(false);
  lastActivityMs = millis();
}

void loop() {
  uint32_t now = millis();

  // BAND switch
  bool fm = digitalRead(PIN_BAND) == LOW;
  if (fm != bandFM) { ampMute(true); setBand(fm); delay(50); if (!tapeAudio) ampMute(false); }

  // keys
  int k = keyEvent();
  if (k != K_NONE) lastActivityMs = now;
  switch (k) {
    case K_STOP:  if (deck != D_STOP || gearInFunc()) { deckStop(); ampMute(false); } else ejectDoor(); break;
    case K_PLAY:  deckPlay(headA); ampMute(false); break;
    case K_REW:   deckWind(false); break;
    case K_FF:    deckWind(true); break;
    case K_PAUSE: if (deck == D_PLAY) { paused = !paused; motor(!paused); if (!paused) lastRotMs = millis(); } break;
    case K_AB:    headA = !headA; if (deck == D_PLAY) deckPlay(headA); break;
  }

  // end of tape / auto-reverse
  checkTapeEnd();

  // motor: only on with a cassette or while in function; switches off when idle
  if (deck == D_STOP && !gearInFunc()) {
    if (hasCassette() && now - lastActivityMs < MOTOR_IDLE_OFF_MS) { /* keep ready */ }
    else motor(false);
  }

  // radio
  if (!tapeAudio) {
    tuneLoop();
    static uint32_t tq = 0;
    if (now - tq > 400) {
      tq = now;
      if (bandFM) { rx.getCurrentReceivedSignalQuality(); digitalWrite(PIN_LED, rx.getCurrentPilot()); }
      else digitalWrite(PIN_LED, LOW);
    }
  } else digitalWrite(PIN_LED, LOW);

  // battery
  static uint32_t tb = 0;
  if (now - tb > 2000) {
    tb = now;
    float v = vbat();
    if (v < VBAT_EMPTY) { deckStop(); ampMute(true); }
    else if (v < VBAT_LOW) { digitalWrite(PIN_LED, HIGH); delay(60); digitalWrite(PIN_LED, LOW); }   // blink: charge me
  }
}
