/*
  Blind Maze Console - ESP32-S3

  - Stepper (28BYJ-48 + ULN2003) moves a plastic wall
  - 3 buttons: turn left, turn right, forward
  - LED "seen": current room was already visited
  - 4 LEDs: absolute facing direction N/E/S/W
  - RGB LED: status flash (green = moved, red = wall) + victory rainbow
  - Buzzer: feedback + victory jingle
*/

#include <Arduino.h>

// Pins
const int PIN_BTN_LEFT   = 4;
const int PIN_BTN_RIGHT  = 6;
const int PIN_BTN_FWD    = 5;

const int PIN_LED_SEEN   = 8;
const int PIN_LED_DIR[4] = {15, 18, 17, 16};   // N, E, S, W

const int PIN_RGB_R = 9;
const int PIN_RGB_G = 10;
const int PIN_RGB_B = 11;
const bool RGB_COMMON_ANODE = false;  

const int PIN_BUZZER = 12;

const int PIN_STEP[4] = {13, 14, 21, 47};      // ULN2003 IN1..IN4

// Maze Parameters
const int W = 8;
const int H = 8;

const int  FLAP_STEPS       = 1024;   //Rotation Steps for Wall/Opening ~ 90Degrees
const uint32_t STEP_DELAY_US = 1500;  // Time per rotations

// Maze Struct
// Wall bits per room: 1=N, 2=E, 4=S, 8=W (1 = Wall, 0 = Opening)
enum { N = 0, E = 1, S = 2, WEST = 3 };
const uint8_t WALLBIT[4] = {1, 2, 4, 8};
const int DX[4] = {0, 1, 0, -1};
const int DY[4] = {-1, 0, 1, 0};

uint8_t  cells[W * H];
bool     visited[W * H];

int px, py, facing;
int goalX = W - 1, goalY = H - 1;
bool seenFlag = false;

inline int idx(int x, int y) { return y * W + x; }

// Colors
struct Color { uint8_t r, g, b; };
const Color PALETTE[] = {
  {255, 0, 0},     // red
  {255, 90, 0},    // orange
  {255, 220, 0},   // yellow
  {0, 255, 0},     // green
  {0, 255, 255},   // cyan
  {0, 0, 255},     // blue
  {170, 0, 255},   // purple
  {255, 255, 255}  // white
};
const int PALETTE_N = sizeof(PALETTE) / sizeof(PALETTE[0]);

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (RGB_COMMON_ANODE) { r = 255 - r; g = 255 - g; b = 255 - b; }
  ledcWrite(PIN_RGB_R, r);
  ledcWrite(PIN_RGB_G, g);
  ledcWrite(PIN_RGB_B, b);
}
void setRGBPalette(int i) { setRGB(PALETTE[i].r, PALETTE[i].g, PALETTE[i].b); }

// Buttons
struct Button {
  int pin;
  bool stable = HIGH;
  bool lastRead = HIGH;
  uint32_t changedAt = 0;
  bool pressedEvent = false;

  void begin(int p) { pin = p; pinMode(pin, INPUT_PULLUP); }
  void update() {
    bool r = digitalRead(pin);
    uint32_t now = millis();
    if (r != lastRead) { lastRead = r; changedAt = now; }
    if ((now - changedAt) > 30 && r != stable) {
      stable = r;
      if (stable == LOW) pressedEvent = true;
    }
  }
  bool pressed() { bool e = pressedEvent; pressedEvent = false; return e; }
};

Button btnLeft, btnRight, btnFwd;

// Motor
const uint8_t HALFSTEP_SEQ[8][4] = {
  {1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},
  {0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}
};
long stepPos = 0;       // current position in steps, 0 = open
long stepTarget = 0;
int  seqIndex = 0;
uint32_t lastStepUs = 0;

void coilsOff() { for (int i = 0; i < 4; i++) digitalWrite(PIN_STEP[i], LOW); }

void stepperRun() {
  if (stepPos == stepTarget) return;
  uint32_t now = micros();
  if (now - lastStepUs < STEP_DELAY_US) return;
  lastStepUs = now;

  if (stepTarget > stepPos) { seqIndex = (seqIndex + 1) & 7; stepPos++; }
  else                      { seqIndex = (seqIndex + 7) & 7; stepPos--; }

  for (int i = 0; i < 4; i++) digitalWrite(PIN_STEP[i], HALFSTEP_SEQ[seqIndex][i]);
  if (stepPos == stepTarget) coilsOff(); //Heat Prevention
}

void updateFlap() {
  bool wallAhead = cells[idx(px, py)] & WALLBIT[facing];
  stepTarget = wallAhead ? FLAP_STEPS : 0;
}

// ---------------- SOUND ----------------
void beep(int freq, int ms) { tone(PIN_BUZZER, freq, ms); }

void bonk()      { delay(500); beep(2000, 120); delay(130); beep(1500, 200); }  // bump into a wall
void click()     { delay(500); beep(2700, 25); }                                // turning
void stepSound() { delay(500); beep(2400, 60); }                                // moving

void victoryJingle() {
  const int notes[] = {523, 659, 784, 1047, 784, 1047};
  const int durs[]  = {150, 150, 150, 300, 150, 450};
  for (int i = 0; i < 6; i++) {
    beep(notes[i], durs[i]);
    // Rainbow LED
    setRGBPalette(i % PALETTE_N);
    delay(durs[i] + 30);
  }
  noTone(PIN_BUZZER);
}

// Maze gen (iterative randomized DFS) 
void generateMaze() {
  for (int i = 0; i < W * H; i++) {
    cells[i] = 0x0F;
    visited[i] = false;
  }
  bool carved[W * H] = {false};
  int stack[W * H];
  int sp = 0;

  int cx = 0, cy = 0;
  carved[idx(cx, cy)] = true;
  stack[sp++] = idx(cx, cy);

  while (sp > 0) {
    int cur = stack[sp - 1];
    cx = cur % W; cy = cur / W;

    int options[4], n = 0;
    for (int d = 0; d < 4; d++) {
      int nx = cx + DX[d], ny = cy + DY[d];
      if (nx >= 0 && nx < W && ny >= 0 && ny < H && !carved[idx(nx, ny)]) options[n++] = d;
    }

    if (n == 0) { sp--; continue; }

    int d = options[random(n)];
    int nx = cx + DX[d], ny = cy + DY[d];
    cells[idx(cx, cy)] &= ~WALLBIT[d];
    cells[idx(nx, ny)] &= ~WALLBIT[(d + 2) & 3];
    carved[idx(nx, ny)] = true;
    stack[sp++] = idx(nx, ny);
  }

  px = 0; py = 0;
  facing = E;
  visited[idx(px, py)] = true;
  seenFlag = false;
}

// UI
uint32_t rgbOffAt = 0;            // when the RGB status flash ends

void updateDirLeds() {
  for (int i = 0; i < 4; i++) digitalWrite(PIN_LED_DIR[i], i == facing ? HIGH : LOW);
}

void updateSeenLed() { digitalWrite(PIN_LED_SEEN, seenFlag ? HIGH : LOW); }

// Short colored flash on the RGB LED
void flashRGB(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  setRGB(r, g, b);
  rgbOffAt = millis() + ms;
}

void updateRgb() {
  if (rgbOffAt && millis() >= rgbOffAt) {
    setRGB(0, 0, 0);
    rgbOffAt = 0;
  }
}

void enterRoom() {
  int i = idx(px, py);
  seenFlag = visited[i];
  visited[i] = true;
  updateSeenLed();
  updateFlap();
}

void newGame() {
  generateMaze();
  updateDirLeds();
  updateSeenLed();
  rgbOffAt = 0;
  updateFlap();
}

#include "telegram_map.h"

void setup() {
  Serial.begin(115200);

  btnLeft.begin(PIN_BTN_LEFT);
  btnRight.begin(PIN_BTN_RIGHT);
  btnFwd.begin(PIN_BTN_FWD);

  pinMode(PIN_LED_SEEN, OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(PIN_LED_DIR[i], OUTPUT);
  for (int i = 0; i < 4; i++) { pinMode(PIN_STEP[i], OUTPUT); digitalWrite(PIN_STEP[i], LOW); }

  ledcAttach(PIN_RGB_R, 5000, 8);
  ledcAttach(PIN_RGB_G, 5000, 8);
  ledcAttach(PIN_RGB_B, 5000, 8);
  setRGB(0, 0, 0);

  pinMode(PIN_BUZZER, OUTPUT);

  // Random seed from the hardware random number generator
  randomSeed(esp_random());

  // Need to set the flap to the "open/passage" position by hand before powering on.
  stepPos = 0;
  stepTarget = 0;

  newGame();
  beep(880, 80);
  startTelegram();
}

void loop() {
  btnLeft.update();
  btnRight.update();
  btnFwd.update();

  if (btnLeft.pressed()) {
    facing = (facing + 3) & 3;
    click();
    updateDirLeds();
    updateFlap();
  }

  if (btnRight.pressed()) {
    facing = (facing + 1) & 3;
    click();
    updateDirLeds();
    updateFlap();
  }

  if (btnFwd.pressed()) {
    if (cells[idx(px, py)] & WALLBIT[facing]) {
      flashRGB(255, 0, 0, 400);
      bonk();
    } else {
      px += DX[facing];
      py += DY[facing];
      flashRGB(0, 255, 0, 300);
      stepSound();
      enterRoom();
      if (px == goalX && py == goalY) {
        victoryJingle();
        telegramNotifyWin();
        newGame();
      }
    }
  }

  updateRgb();
  stepperRun();
}
