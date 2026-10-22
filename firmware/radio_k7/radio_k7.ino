/*
  RC-80 — Rádio toca-fitas AM/FM anos 80 com Arduino Nano
  ---------------------------------------------------------------
  Rádio:      Si4732-A10 (I2C, biblioteca "PU2CLR SI4735")
  Sintonia:   botão TUNING gira o cabrestante; o cordão leva a agulha e gira o
              potenciômetro multivoltas (10 voltas) -> o Arduino lê a posição da
              agulha e sintoniza o Si4732 na frequência que ela está mostrando.
  Fita:       mecanismo lógico CRP42602Y (auto-reverse, motor 12 V, solenoide 9 V)
              Sequências do solenoide baseadas na biblioteca crp42602y_ctrl de
              Elehobica (BSD-2-Clause) — https://github.com/elehobica/pico_crp42602y_ctrl
  Teclas:     6 teclas piano no topo, lidas por escada de resistores em A7.
  Porta:      trava passiva + micro servo SG90 que solta a trava (STOP/EJ).
  Energia:    a chave do potenciômetro de VOLUME liga/desliga tudo (via MOSFET).

  Pinos (Arduino Nano):
    D2  ROTATION_SENS  (pino 2 do CRP42602Y)           entrada, pull-up
    D3  servo da trava (SG90, sinal laranja)
    D4  GEAR_STATUS_SW (pino 3 do CRP42602Y)           entrada, pull-up (LOW = em função)
    D5  CASSETTE_DETECT_SW (pino 7 do CRP42602Y)       entrada, pull-up (LOW = com fita)
    D6  SOLENOIDE  -> gate do IRLZ44N (HIGH = puxa)
    D7  MOTOR 12 V -> base do BC337 que liga o IRF9540N (HIGH = motor ligado)
    D8  RELÉ de áudio (HIGH = fita, LOW = rádio)
    D9  MUTE do amplificador (ver AMP_MUTE_LEVEL)
    D10 LED "FM STEREO" (com 1 kΩ)
    D12 RESET do Si4732
    A0  potenciômetro multivoltas da sintonia (cursor)
    A1  microchave da chave BAND (fechada = FM)
    A4  SDA  -> conversor de nível -> Si4732 SDIO
    A5  SCL  -> conversor de nível -> Si4732 SCLK
    A6  tensão da bateria (divisor 100k / 33k)
    A7  teclas (escada de resistores, pull-up 10k)

  Calibração da escala (uma vez): segure STOP/EJ e ligue o rádio.
    O LED pisca devagar: leve a agulha até 88 e aperte PLAY.
    O LED pisca rápido:  leve a agulha até 108 e aperte PLAY. Pronto (fica gravado).
*/
#include <SI4735.h>
#include <Servo.h>
#include <EEPROM.h>

// ------------------------------------------------------------ pinos
const uint8_t PIN_ROT = 2, PIN_SERVO = 3, PIN_GEAR = 4, PIN_CASS = 5;
const uint8_t PIN_SOL = 6, PIN_MOTOR = 7, PIN_RELAY = 8, PIN_MUTE = 9, PIN_LED = 10, PIN_RESET = 12;
const uint8_t PIN_TUNE = A0, PIN_BAND = A1, PIN_VBAT = A6, PIN_KEYS = A7;

// ------------------------------------------------------------ ajustes
const bool    AMP_MUTE_LEVEL = HIGH;      // nível que silencia o amplificador (PAM8610: MUTE ativo em ALTO)
const uint8_t SERVO_REST = 20;            // braço longe da lingueta
const uint8_t SERVO_RELEASE = 75;         // braço levanta a lingueta e solta a porta
const float   VBAT_K = (100.0 + 33.0) / 33.0 * 5.0 / 1023.0;
const float   VBAT_LOW = 9.9, VBAT_EMPTY = 9.3;
const uint16_t FM_MIN = 8800, FM_MAX = 10800;   // em 10 kHz
const uint16_t AM_MIN = 530,  AM_MAX = 1600;    // em kHz (passo de 10 kHz, Brasil)
const uint32_t MOTOR_IDLE_OFF_MS = 30000;       // desliga o motor parado sem uso

// teclas: STOP/EJ, PLAY, REW, F.FWD, PAUSE, A-B (resistores 0, 1k, 2k2, 3k9, 6k8, 15k)
enum Key { K_NONE = -1, K_STOP = 0, K_PLAY, K_REW, K_FF, K_PAUSE, K_AB };
const int KEY_ADC[6] = {0, 93, 184, 288, 414, 614};

SI4735 rx;
Servo latch;

// ------------------------------------------------------------ calibração (EEPROM)
struct Cal { uint16_t magic, lo, hi; } cal;
const uint16_t CAL_MAGIC = 0x52C8;

// ------------------------------------------------------------ estado
enum Deck { D_STOP, D_PLAY, D_FF, D_REW };
Deck deck = D_STOP;
bool headA = true;            // lado A (true) / lado B (false)
bool paused = false;
bool motorOn = false;
bool tapeAudio = false;
uint32_t lastGearMs = 0, lastActivityMs = 0;
volatile uint32_t lastRotMs = 0;
bool bandFM = true;
uint16_t curFreq = 0;
float tuneFilt = -1;

void onRotation() { lastRotMs = millis(); }

// ------------------------------------------------------------ utilitários
void ampMute(bool m) { digitalWrite(PIN_MUTE, m ? AMP_MUTE_LEVEL : !AMP_MUTE_LEVEL); }
bool gearInFunc() { return digitalRead(PIN_GEAR) == LOW; }
bool hasCassette() { return digitalRead(PIN_CASS) == LOW; }
void sol(bool pull) { digitalWrite(PIN_SOL, pull ? HIGH : LOW); }
float vbat() { return analogRead(PIN_VBAT) * VBAT_K; }

void motor(bool on) {
  if (on && !motorOn) { digitalWrite(PIN_MOTOR, HIGH); motorOn = true; delay(500); }   // espera o motor estabilizar
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
// Sequência de função: a engrenagem gira 190° em ~400 ms; o solenoide escolhe
// direção da cabeça, se a cabeça encosta (PLAY) e o sentido dos carretéis.
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
// Sequência de volta (parar): o resto da volta da engrenagem, ~360 ms
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
void deckWind(bool forward) {   // FF/REW relativos ao lado que está tocando
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
  latch.detach();             // sem pulso: o servo não zumbe nem gasta
}

// fim da fita: os carretéis param de girar
void checkTapeEnd() {
  if (deck == D_STOP || paused || !gearInFunc()) return;
  uint32_t now = millis();
  if (now - lastGearMs < 1200) return;
  noInterrupts(); uint32_t lr = lastRotMs; interrupts();
  if (now - lr < 1500) return;
  if (deck == D_PLAY && headA) deckPlay(false);       // auto-reverse: lado A terminou -> toca o B
  else deckStop();                                     // lado B terminou, ou FF/REW chegou ao fim
  if (deck == D_STOP) ampMute(false);
}

// ------------------------------------------------------------ teclas
int readKey() {
  int v = analogRead(PIN_KEYS);
  for (int i = 0; i < 6; i++) if (abs(v - KEY_ADC[i]) < 40) return i;
  return K_NONE;
}
int keyEvent() {               // devolve a tecla uma vez por aperto (com debounce)
  static int last = K_NONE, stable = K_NONE; static uint32_t t = 0;
  int k = readKey();
  if (k != last) { last = k; t = millis(); return K_NONE; }
  if (millis() - t > 30 && k != stable) { stable = k; return k; }
  return K_NONE;
}

// ------------------------------------------------------------ sintonia
uint16_t readTuneRaw() {        // média de 64 leituras (mais resolução, menos ruído)
  uint32_t s = 0;
  for (uint8_t i = 0; i < 64; i++) s += analogRead(PIN_TUNE);
  return s / 16;                // 0..4092
}
uint16_t posToFreq(float pos) {
  float t = (pos - cal.lo) / float(cal.hi - cal.lo);
  t = constrain(t, 0.0, 1.0);
  if (bandFM) {
    uint16_t f = FM_MIN + t * (FM_MAX - FM_MIN);
    return (f + 5) / 10 * 10;                            // passo de 100 kHz
  }
  float f = AM_MIN * pow(float(AM_MAX) / AM_MIN, t);     // escala AM é logarítmica no mostrador
  return (uint16_t(f) + 5) / 10 * 10;                    // passo de 10 kHz
}
void setBand(bool fm) {
  bandFM = fm;
  if (fm) { rx.setFM(8400, 10800, FM_MIN, 10); rx.setFMDeEmphasis(2); rx.setFmStereoOn(); }   // 75 µs (Brasil)
  else    { rx.setAM(520, 1710, AM_MIN, 10); rx.setAvcAmMaxGain(48); }
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
  if (readKey() == K_STOP) {                               // STOP/EJ apertado ao ligar
    delay(300);
    if (readKey() == K_STOP) calibrate();
  }

  Wire.begin();
  rx.getDeviceI2CAddress(PIN_RESET);
  rx.setup(PIN_RESET, 0);                                  // 0 = FM
  delay(300);
  setBand(digitalRead(PIN_BAND) == LOW);
  if (gearInFunc()) gearReturn();                          // desligou tocando: volta para STOP
  motor(false);
  ampMute(false);
  lastActivityMs = millis();
}

void loop() {
  uint32_t now = millis();

  // chave BAND
  bool fm = digitalRead(PIN_BAND) == LOW;
  if (fm != bandFM) { ampMute(true); setBand(fm); delay(50); if (!tapeAudio) ampMute(false); }

  // teclas
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

  // fim da fita / auto-reverse
  checkTapeEnd();

  // motor: só fica ligado com fita ou em função; desliga se ficar parado
  if (deck == D_STOP && !gearInFunc()) {
    if (hasCassette() && now - lastActivityMs < MOTOR_IDLE_OFF_MS) { /* deixa pronto */ }
    else motor(false);
  }

  // rádio
  if (!tapeAudio) {
    tuneLoop();
    static uint32_t tq = 0;
    if (now - tq > 400) {
      tq = now;
      if (bandFM) { rx.getCurrentReceivedSignalQuality(); digitalWrite(PIN_LED, rx.getCurrentPilot()); }
      else digitalWrite(PIN_LED, LOW);
    }
  } else digitalWrite(PIN_LED, LOW);

  // bateria
  static uint32_t tb = 0;
  if (now - tb > 2000) {
    tb = now;
    float v = vbat();
    if (v < VBAT_EMPTY) { deckStop(); ampMute(true); }
    else if (v < VBAT_LOW) { digitalWrite(PIN_LED, HIGH); delay(60); digitalWrite(PIN_LED, LOW); }   // pisca: carregar
  }
}
