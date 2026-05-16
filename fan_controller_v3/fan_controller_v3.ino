/*
  Fan Controller v2 - Arduino Nano
  Changes from v1:
    - Split UI: left half = PWM%, right half = fan icon + RPM
    - Animated spinning fan icon (spin speed scales with RPM)
    - Larger, more readable RPM display
    - No hold-to-repeat: each physical press = 1 step change
    - Title in yellow strip (top 16px), rest of UI in blue area

  Hardware:
    - AVC 4-wire PWM fan (DBTA0838B2G, 12V 4.1A)
    - 0.96" I2C SSD1306 OLED (yellow/blue split type)
    - 3 buttons: UP, DOWN, RESET
    - 12V 5A supply + 5V BEC for Arduino

  Wiring:
    Display (I2C):  GND→GND, VDD→5V, SCK→A5, SDA→A4
    Fan:  Red→12V, Black→GND, Blue(PWM)→D3, Yellow(tach)→D2
    Buttons (pin to GND, internal pullup):
      UP→D4, DOWN→D5, RESET→D6
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// ---------- Display ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_ADDR    0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- Fan ----------
#define FAN_PWM_PIN     3
#define FAN_TACH_PIN    2
#define FAN_MIN_PCT    10
#define FAN_MAX_PCT    80
#define FAN_STEP        5

// ---------- Buttons ----------
#define BTN_UP          4
#define BTN_DOWN        5
#define BTN_RESET       6
#define DEBOUNCE_MS    40

// ---------- EEPROM ----------
#define EEPROM_ADDR     0

// ---------- RPM ----------
volatile unsigned long tachPulses = 0;
unsigned long lastRpmCalc = 0;
unsigned int currentRPM = 0;

// ---------- Fan animation ----------
unsigned long lastFrameTime = 0;
uint8_t fanFrame = 0;  // 0..3 rotation index

// ---------- State ----------
int fanSpeed = FAN_MIN_PCT;

// ---------- Button (single-shot, no auto-repeat) ----------
struct Button {
  uint8_t pin;
  bool lastStableState;     // HIGH = not pressed, LOW = pressed
  bool lastReading;
  unsigned long lastChangeTime;
};

Button btnUp    = {BTN_UP,    HIGH, HIGH, 0};
Button btnDown  = {BTN_DOWN,  HIGH, HIGH, 0};
Button btnReset = {BTN_RESET, HIGH, HIGH, 0};

// ---------- ISR ----------
void tachISR() {
  tachPulses++;
}

// ---------- 25kHz PWM on D3 via Timer2 ----------
void setupPWM25k() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  TCCR2A = _BV(COM2B1) | _BV(WGM20);
  TCCR2B = _BV(WGM22)  | _BV(CS21);
  OCR2A  = 39;
  OCR2B  = 0;
}

void setFanPWM(int percent) {
  percent = constrain(percent, 0, 100);
  OCR2B = map(percent, 0, 100, 0, OCR2A);
}

// ---------- EEPROM ----------
void saveSpeed(int pct) {
  EEPROM.update(EEPROM_ADDR, (uint8_t)pct);
}

int loadSpeed() {
  int pct = (int)EEPROM.read(EEPROM_ADDR);
  if (pct < FAN_MIN_PCT || pct > FAN_MAX_PCT) return FAN_MIN_PCT;
  pct = ((pct - FAN_MIN_PCT + FAN_STEP / 2) / FAN_STEP) * FAN_STEP + FAN_MIN_PCT;
  return constrain(pct, FAN_MIN_PCT, FAN_MAX_PCT);
}

// ---------- Button: returns true only on the edge of a new press ----------
bool buttonPressed(Button &btn) {
  bool reading = digitalRead(btn.pin);
  unsigned long now = millis();

  if (reading != btn.lastReading) {
    btn.lastChangeTime = now;
    btn.lastReading = reading;
  }

  if ((now - btn.lastChangeTime) > DEBOUNCE_MS) {
    if (reading != btn.lastStableState) {
      btn.lastStableState = reading;
      // Trigger only on press down (HIGH -> LOW)
      if (reading == LOW) {
        return true;
      }
    }
  }
  return false;
}

// ---------- Fan icon ----------
// Draw an animated fan at center (cx, cy) with blade radius r.
// `frame` (0..3) rotates the blades.
void drawFanIcon(int cx, int cy, int r, uint8_t frame) {
  // Outer circle
  display.drawCircle(cx, cy, r, SSD1306_WHITE);
  display.drawCircle(cx, cy, r + 1, SSD1306_WHITE);

  // Hub
  display.fillCircle(cx, cy, 3, SSD1306_WHITE);

  // Blades - 4 curved lines rotated by frame index
  // We draw 4 simple curved blade shapes using line pairs.
  // Angle offsets for each frame (in degrees): 0, 22, 45, 67
  const int angleOffsets[4] = {0, 22, 45, 67};
  int a0 = angleOffsets[frame & 0x03];

  for (int b = 0; b < 4; b++) {
    float angleDeg = a0 + b * 90;
    float rad1 = radians(angleDeg);
    float rad2 = radians(angleDeg + 35);  // blade sweep

    // Inner point (near hub)
    int x1 = cx + (int)(4 * cos(rad1));
    int y1 = cy + (int)(4 * sin(rad1));
    // Outer point (near rim)
    int x2 = cx + (int)((r - 2) * cos(rad2));
    int y2 = cy + (int)((r - 2) * sin(rad2));

    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);

    // Second line for a thicker blade look
    float rad1b = radians(angleDeg + 5);
    float rad2b = radians(angleDeg + 30);
    int x1b = cx + (int)(4 * cos(rad1b));
    int y1b = cy + (int)(4 * sin(rad1b));
    int x2b = cx + (int)((r - 2) * cos(rad2b));
    int y2b = cy + (int)((r - 2) * sin(rad2b));
    display.drawLine(x1b, y1b, x2b, y2b, SSD1306_WHITE);
  }
}

// ---------- Display ----------
void drawDisplay() {
  display.clearDisplay();

  // ===== Yellow strip (top 16 px): title =====
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(22, 0);
  display.print(F("P2S FAN CONTROL"));
  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  // ===== Left half (0..63): big PWM % =====
  // Centered in left half
  display.setTextSize(3);
  char buf[5];
  snprintf(buf, sizeof(buf), "%d", fanSpeed);
  int nDigits = strlen(buf);
  int numW = nDigits * 18;          // text size 3 = 18 px per char
  int blockW = numW + 14;           // + "%" at text size 2 (~14 px)
  int xStart = (64 - blockW) / 2;   // center in left half (0..63)
  if (xStart < 1) xStart = 1;

  display.setCursor(xStart, 22);
  display.print(buf);
  display.setTextSize(2);
  display.setCursor(xStart + numW + 1, 28);
  display.print(F("%"));

  // Progress bar 10 --- 80 in left half, below number
  // Layout: bar at y=48..53, labels at y=56
  display.setTextSize(1);
  // Bar outline: width 62, leaves room for edges
  display.drawRect(1, 48, 62, 6, SSD1306_WHITE);
  // Bar fill
  int fillW = map(fanSpeed, FAN_MIN_PCT, FAN_MAX_PCT, 0, 58);
  if (fillW > 0) display.fillRect(3, 50, fillW, 2, SSD1306_WHITE);
  // Labels below bar
  display.setCursor(0, 56);
  display.print(FAN_MIN_PCT);
  display.setCursor(50, 56);
  display.print(FAN_MAX_PCT);

  // ===== Vertical divider =====
  display.drawLine(64, 14, 64, 63, SSD1306_WHITE);

  // ===== Right half (64..127): fan icon + RPM =====
  // Fan icon centered in upper right
  drawFanIcon(96, 32, 14, fanFrame);

  // RPM value at bottom right, size 1 so we fit "RPM: 9999"
  display.setTextSize(1);
  char rpmBuf[12];
  if (currentRPM > 0) {
    snprintf(rpmBuf, sizeof(rpmBuf), "%u RPM", currentRPM);
  } else {
    snprintf(rpmBuf, sizeof(rpmBuf), "--- RPM");
  }
  int rpmW = strlen(rpmBuf) * 6;
  int rpmX = 64 + (64 - rpmW) / 2;
  display.setCursor(rpmX, 54);
  display.print(rpmBuf);

  display.display();
}

// ---------- Setup ----------
void setup() {
  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_RESET, INPUT_PULLUP);

  pinMode(FAN_TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachISR, FALLING);

  setupPWM25k();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (true);
  }
  display.clearDisplay();
  display.display();

  fanSpeed = loadSpeed();
  setFanPWM(fanSpeed);

  // Splash
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 2);
  display.print(F("P2S Filter Fan"));
  display.setCursor(30, 30);
  display.print(F("Ready at "));
  display.print(fanSpeed);
  display.print(F("%"));
  display.display();
  delay(1500);

  drawDisplay();
}

// ---------- Loop ----------
void loop() {
  bool uiDirty = false;

  // Buttons (single-shot only, no repeat)
  if (buttonPressed(btnUp)) {
    if (fanSpeed < FAN_MAX_PCT) {
      fanSpeed = min(fanSpeed + FAN_STEP, FAN_MAX_PCT);
      setFanPWM(fanSpeed);
      saveSpeed(fanSpeed);
      uiDirty = true;
    }
  }

  if (buttonPressed(btnDown)) {
    if (fanSpeed > FAN_MIN_PCT) {
      fanSpeed = max(fanSpeed - FAN_STEP, FAN_MIN_PCT);
      setFanPWM(fanSpeed);
      saveSpeed(fanSpeed);
      uiDirty = true;
    }
  }

  if (buttonPressed(btnReset)) {
    if (fanSpeed != FAN_MIN_PCT) {
      fanSpeed = FAN_MIN_PCT;
      setFanPWM(fanSpeed);
      saveSpeed(fanSpeed);
      uiDirty = true;
    }
  }

  unsigned long now = millis();

  // Update RPM once per second
  if (now - lastRpmCalc >= 1000) {
    noInterrupts();
    unsigned long pulses = tachPulses;
    tachPulses = 0;
    interrupts();
    currentRPM = (pulses * 60) / 2;
    lastRpmCalc = now;
    uiDirty = true;
  }

  // Advance fan animation frame based on RPM
  // Only animate when the fan is actually spinning (RPM > 0).
  // This avoids a misleading "fake" spin when no fan is connected.
  unsigned int rpm = currentRPM;
  if (rpm > 0) {
    unsigned long frameInterval;
    if (rpm < 300) {
      frameInterval = 400;   // very slow
    } else if (rpm > 6000) {
      frameInterval = 40;    // very fast
    } else {
      // Linear map: 300 RPM -> 400ms, 6000 RPM -> 40ms
      frameInterval = map(rpm, 300, 6000, 400, 40);
    }

    if (now - lastFrameTime >= frameInterval) {
      lastFrameTime = now;
      fanFrame = (fanFrame + 1) & 0x03;
      uiDirty = true;
    }
  } else {
    // No fan detected: keep the icon frozen at frame 0
    fanFrame = 0;
    lastFrameTime = now;  // prevent accumulated "catch-up" animation when fan starts
  }

  if (uiDirty) {
    drawDisplay();
  }
}
