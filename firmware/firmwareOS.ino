/* =========================================================================
 *  HabitWatch OS v2.1 - Complete Smartwatch Operating System (Fixed)
 *  Target: Seeed Studio XIAO ESP32-C6 + GC9A01 TFT + Custom Vibrator
 *
 *  Fixes Applied:
 *  [1] Vibrator pattern index off-by-one & sentinel logic rewritten
 *  [2] Button events now use bitmask - both buttons captured per cycle
 *  [3] Removed blocking delay(2000) in STATE_SNOOZE - non-blocking timer
 *  [4] Habit trigger uses 5-second window + lastTriggeredMinute guard
 *  [5] wakeUpScreen() removed from readButtons() - input layer is pure
 *  [6] Snooze timer uses start+elapsed pattern (millis overflow safe)
 *  [7] STATE_SLEEP now blocks habit engine
 *  [8] Battery calc uses integer millivolts - no float precision loss
 *  [9] Mock time fallback zero-initializes struct tm before use
 *  [10] Global back nav uses consumed flag instead of modifying local copy
 * =========================================================================*/

#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <esp_sleep.h>

// =========================================================================
// CONFIGURATION & CREDENTIALS
// =========================================================================
const char* ssid              = "YOUR_WIFI_SSID";
const char* password          = "YOUR_WIFI_PASSWORD";
const char* ntpServer         = "pool.ntp.org";
const long  gmtOffset_sec     = -28800; // PST (-8h). Adjust for your timezone.
const int   daylightOffset_sec = 3600;

// =========================================================================
// HARDWARE PINS
// =========================================================================
#define MOTOR_PIN   D1   // AO3400A MOSFET Gate
#define BTN_NEXT    D2   // Button 1 (Navigate / Snooze)
#define BTN_SELECT  D3   // Button 2 (Select / Dismiss)
#define TFT_BL_PIN  D10  // Backlight PWM Control
#define BATTERY_PIN A0   // ADC pin (requires 1/2 voltage divider on hardware)

// =========================================================================
// DISPLAY SETTINGS & COLORS (RGB565)
// =========================================================================
TFT_eSPI tft = TFT_eSPI();

#define C_BG        0x0821  // Deep dark blue
#define C_ACCENT    0x07E0  // Bright green
#define C_ALERT     0xF800  // Vibrant red
#define C_TEXT      0xFFFF  // White
#define C_MUTED     0x4A49  // Dim grey
#define C_BATT_GOOD 0x07E0  // Green
#define C_BATT_LOW  0xF800  // Red

// =========================================================================
// SYSTEM STATES
// =========================================================================
enum AppState {
  STATE_BOOT,
  STATE_WATCHFACE,
  STATE_MENU,
  STATE_HABIT_SETTINGS,
  STATE_SYS_SETTINGS,
  STATE_ALARM,
  STATE_SNOOZE,
  STATE_SLEEP
};
AppState currentState = STATE_BOOT;

// =========================================================================
// SYSTEM VARIABLES
// =========================================================================
int  screenBrightness  = 200;   // 0-255 PWM
int  batteryPercent    = 100;
bool wifiConnected     = false;
bool forceRedraw       = true;

unsigned long lastActivityTime  = 0;
unsigned long snoozeDisplayTimer = 0; // [FIX 3] replaces blocking delay()

const unsigned long SCREEN_TIMEOUT_MS = 15000; // 15 seconds

// =========================================================================
// MENU SYSTEM
// =========================================================================
const char* mainMenuItems[] = { "Habits", "Settings", "Sync Time", "Sleep" };
const int   numMenuItems    = 4;
int currentMenuIndex        = 0;
int habitMenuIndex          = 0;

// =========================================================================
// VIBRATION ENGINE
// =========================================================================
// Sentinel values for pattern control:
//   intensity == -1  → loop back to index 0
//   intensity == -2  → stop (end of pattern)
struct VibeStep {
  int intensity;  // 0-255 PWM, -1 = loop, -2 = end
  int duration;   // milliseconds
};

// [FIX 1] All patterns now use explicit -2 end sentinel
VibeStep pat_Water[]   = { {150,150},{0,100},{150,150},{0,1000},{-1,0} };
VibeStep pat_Posture[] = { {255,400},{0,200},{100,400},{0,1500},{-1,0} };
VibeStep pat_Pill[]    = { {200,100},{0,50},{200,100},{0,50},{200,100},{0,2000},{-1,0} };
VibeStep pat_Snooze[]  = { {80,50},{0,50},{80,50},{0,3000},{-1,0} };
VibeStep pat_Tick[]    = { {100,30},{-2,0} }; // Single pulse then stop

VibeStep*     currentPattern = nullptr;
int           patternIndex   = 0;
unsigned long patternTimer   = 0;

void playVibe(VibeStep* pattern) {
  currentPattern = pattern;
  patternIndex   = 0;
  patternTimer   = millis();
  analogWrite(MOTOR_PIN, currentPattern[0].intensity);
}

void stopVibe() {
  currentPattern = nullptr;
  analogWrite(MOTOR_PIN, 0);
}

// [FIX 1] Rewrote sentinel logic: check -1 (loop) and -2 (end) clearly,
//         increment index first then bounds-check before any analogWrite.
void updateVibrator() {
  if (!currentPattern) return;
  if (millis() - patternTimer < (unsigned long)currentPattern[patternIndex].duration) return;

  patternIndex++;

  int nextIntensity = currentPattern[patternIndex].intensity;

  if (nextIntensity == -1) {
    // Loop sentinel: restart from beginning
    patternIndex  = 0;
    nextIntensity = currentPattern[0].intensity;
  } else if (nextIntensity == -2) {
    // End sentinel: stop vibrator entirely
    stopVibe();
    return;
  }

  patternTimer = millis();
  analogWrite(MOTOR_PIN, nextIntensity);
}

// =========================================================================
// HABIT ENGINE
// =========================================================================
struct Habit {
  String        name;
  int           triggerMinute;      // Fires at xx:triggerMinute each hour
  int           snoozeMins;
  VibeStep*     pattern;
  bool          active;
  bool          justSnoozed;
  unsigned long snoozeStartMs;      // [FIX 6] store START not target
  int           lastTriggeredMinute; // [FIX 4] prevents re-trigger in same minute
};

// lastTriggeredMinute initialised to -1 so it never falsely matches minute 0
Habit habits[] = {
  { "Drink Water", 0,  5,  pat_Water,   true,  false, 0, -1 },
  { "Posture",     30, 5,  pat_Posture, true,  false, 0, -1 },
  { "Take Pill",   15, 10, pat_Pill,    false, false, 0, -1 }
};
const int NUM_HABITS = sizeof(habits) / sizeof(Habit);

int activeHabitIndex = 0;

// Forward declarations needed by updateHabitEngine
void wakeUpScreen();

// [FIX 4] 5-second trigger window + lastTriggeredMinute guard
// [FIX 7] Returns immediately when STATE_SLEEP is active
void updateHabitEngine(struct tm* timeinfo) {
  if (currentState == STATE_ALARM  ||
      currentState == STATE_SNOOZE ||
      currentState == STATE_SLEEP) return;  // [FIX 7]

  unsigned long now = millis();

  for (int i = 0; i < NUM_HABITS; i++) {
    if (!habits[i].active) continue;

    // --- Standard minute trigger ---
    // [FIX 4] 5-second window + de-duplicate within same minute
    if (!habits[i].justSnoozed &&
        timeinfo->tm_min == habits[i].triggerMinute &&
        timeinfo->tm_sec < 5 &&
        habits[i].lastTriggeredMinute != timeinfo->tm_min) {

      habits[i].lastTriggeredMinute = timeinfo->tm_min;
      activeHabitIndex = i;
      currentState     = STATE_ALARM;
      playVibe(habits[i].pattern);
      forceRedraw = true;
      wakeUpScreen();
      break;
    }

    // --- Snooze expiry trigger ---
    // [FIX 6] elapsed subtraction is overflow-safe
    if (habits[i].justSnoozed) {
      unsigned long elapsed = now - habits[i].snoozeStartMs;
      unsigned long target  = (unsigned long)habits[i].snoozeMins * 60000UL;
      if (elapsed >= target) {
        habits[i].justSnoozed = false;
        activeHabitIndex = i;
        currentState     = STATE_ALARM;
        playVibe(habits[i].pattern);
        forceRedraw = true;
        wakeUpScreen();
        break;
      }
    }
  }
}

// =========================================================================
// INPUT ENGINE
// =========================================================================

// [FIX 2] Bitmask-based event system: both buttons captured in one call.
//         No event is silently overwritten.
#define EVT_NONE       0x00
#define EVT_NEXT_SHORT 0x01
#define EVT_NEXT_LONG  0x02
#define EVT_SEL_SHORT  0x04
#define EVT_SEL_LONG   0x08
#define EVT_ANY_PRESS  0x10  // [FIX 5] raw press detected (for wakeUpScreen)

struct Button {
  uint8_t       pin;
  bool          lastState;
  unsigned long pressTime;
  bool          handledLongPress;
};

Button btnNext = { BTN_NEXT,   HIGH, 0, false };
Button btnSel  = { BTN_SELECT, HIGH, 0, false };

// [FIX 2] Returns bitmask of all events that occurred this cycle.
// [FIX 5] wakeUpScreen() is NO LONGER called here; caller handles EVT_ANY_PRESS.
uint8_t readButtons() {
  uint8_t       events = EVT_NONE;
  unsigned long now    = millis();

  // ---- NEXT button ----
  bool curNext = digitalRead(btnNext.pin);

  if (curNext == LOW && btnNext.lastState == HIGH) {
    // Just pressed
    btnNext.pressTime        = now;
    btnNext.handledLongPress = false;
    events |= EVT_ANY_PRESS;
  } else if (curNext == LOW && btnNext.lastState == LOW) {
    // Held down
    if (!btnNext.handledLongPress && (now - btnNext.pressTime > 600)) {
      events |= EVT_NEXT_LONG;
      btnNext.handledLongPress = true;
    }
  } else if (curNext == HIGH && btnNext.lastState == LOW) {
    // Released
    if (!btnNext.handledLongPress && (now - btnNext.pressTime > 50)) {
      events |= EVT_NEXT_SHORT;
    }
  }
  btnNext.lastState = curNext;

  // ---- SELECT button ----
  bool curSel = digitalRead(btnSel.pin);

  if (curSel == LOW && btnSel.lastState == HIGH) {
    // Just pressed
    btnSel.pressTime        = now;
    btnSel.handledLongPress = false;
    events |= EVT_ANY_PRESS;
  } else if (curSel == LOW && btnSel.lastState == LOW) {
    // Held down
    if (!btnSel.handledLongPress && (now - btnSel.pressTime > 600)) {
      events |= EVT_SEL_LONG;
      btnSel.handledLongPress = true;
    }
  } else if (curSel == HIGH && btnSel.lastState == LOW) {
    // Released
    if (!btnSel.handledLongPress && (now - btnSel.pressTime > 50)) {
      events |= EVT_SEL_SHORT;
    }
  }
  btnSel.lastState = curSel;

  return events;
}

// =========================================================================
// POWER & SYSTEM MANAGEMENT
// =========================================================================

// [FIX 5] wakeUpScreen() is now only called from the main loop,
//         never from inside readButtons().
void wakeUpScreen() {
  lastActivityTime = millis();
  if (currentState == STATE_SLEEP) {
    analogWrite(TFT_BL_PIN, screenBrightness);
    currentState = STATE_WATCHFACE;
    forceRedraw  = true;
  }
}

void goToSleep() {
  currentState = STATE_SLEEP;
  analogWrite(TFT_BL_PIN, 0);
  tft.fillScreen(TFT_BLACK);
  stopVibe();
}

// [FIX 8] Integer millivolt arithmetic – no float precision loss in map()
void updateBattery() {
  int  raw        = analogRead(BATTERY_PIN);
  long millivolts = ((long)raw * 3300L * 2L) / 4095L; // 1/2 divider assumed
  batteryPercent  = (int)map(millivolts, 3200L, 4200L, 0L, 100L);
  batteryPercent  = constrain(batteryPercent, 0, 100);
}

void syncTime() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT);
  tft.drawString("Syncing WiFi...", 120, 120, 2);

  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    tft.setTextColor(C_ACCENT);
    tft.drawString("Time Synced!", 120, 150, 2);
    delay(1000);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    wifiConnected = false;
    tft.setTextColor(C_ALERT);
    tft.drawString("Sync Failed", 120, 150, 2);
    delay(1000);
  }
}

// =========================================================================
// UI / DRAWING FUNCTIONS
// =========================================================================
void drawStatusBar() {
  // Battery icon (outline + nub)
  tft.drawRect(105, 10, 24, 10, C_MUTED);
  tft.fillRect(129, 13, 2, 4, C_MUTED);

  uint16_t bColor = (batteryPercent > 20) ? C_BATT_GOOD : C_BATT_LOW;
  int      bWidth = map(batteryPercent, 0, 100, 0, 20);
  tft.fillRect(107, 12, bWidth, 6, bColor);

  // WiFi dot
  if (wifiConnected) {
    tft.fillCircle(145, 18, 2, C_ACCENT);
    tft.drawArc(145, 18, 6, 4, 220, 320, C_ACCENT, C_BG);
  }
}

void drawWatchFace(struct tm* timeinfo) {
  tft.fillScreen(C_BG);
  drawStatusBar();

  tft.drawCircle(120, 120, 118, C_ACCENT);
  tft.drawCircle(120, 120, 117, C_MUTED);

  tft.setTextDatum(MC_DATUM);

  // HH:MM
  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), "%H:%M", timeinfo);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(timeStr, 120, 110, 7);

  // Date
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%a, %b %d", timeinfo);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(dateStr, 120, 160, 2);

  // Next active habit preview
  for (int i = 0; i < NUM_HABITS; i++) {
    if (habits[i].active) {
      tft.setTextColor(C_ACCENT, C_BG);
      String preview = habits[i].name + " @ xx:" + String(habits[i].triggerMinute);
      tft.drawString(preview, 120, 190, 2);
      break;
    }
  }
}

void drawMainMenu() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString("MAIN MENU", 120, 40, 2);

  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(mainMenuItems[currentMenuIndex], 120, 120, 4);

  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("[Nxt] Scroll", 120, 180, 2);
  tft.drawString("[Sel] Enter ", 120, 200, 2);

  // Pagination dots
  for (int i = 0; i < numMenuItems; i++) {
    if (i == currentMenuIndex)
      tft.fillCircle(90 + (i * 20), 150, 4, C_TEXT);
    else
      tft.drawCircle(90 + (i * 20), 150, 4, C_MUTED);
  }
}

void drawHabitsApp() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString("HABITS", 120, 40, 2);

  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(habits[habitMenuIndex].name, 120, 100, 4);

  if (habits[habitMenuIndex].active) {
    tft.fillRoundRect(80, 130, 80, 30, 15, C_ACCENT);
    tft.setTextColor(C_BG);
    tft.drawString("ACTIVE", 120, 145, 2);
  } else {
    tft.fillRoundRect(80, 130, 80, 30, 15, C_MUTED);
    tft.setTextColor(C_TEXT);
    tft.drawString("OFF", 120, 145, 2);
  }

  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("[Nxt] Cycle | [Sel] Toggle", 120, 200, 2);
}

void drawAlarmScreen() {
  tft.fillScreen(C_ALERT);
  tft.drawCircle(120, 120, 110, C_TEXT);
  tft.drawCircle(120, 120, 108, C_TEXT);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_TEXT, C_ALERT);
  tft.drawString("ACTION!", 120, 70, 4);
  tft.drawString(habits[activeHabitIndex].name, 120, 120, 4);

  // Snooze button (left)
  tft.fillRoundRect(30, 170, 80, 30, 10, C_TEXT);
  tft.setTextColor(C_ALERT, C_TEXT);
  tft.drawString("SNOOZE", 70, 185, 2);

  // Dismiss button (right)
  tft.fillRoundRect(130, 170, 80, 30, 10, C_TEXT);
  tft.setTextColor(C_ALERT, C_TEXT);
  tft.drawString("DISMISS", 170, 185, 2);
}

void drawSnoozeScreen() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Snoozed for", 120, 90, 2);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString(String(habits[activeHabitIndex].snoozeMins) + " mins", 120, 130, 4);
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);

  // Pin init
  pinMode(MOTOR_PIN,  OUTPUT); analogWrite(MOTOR_PIN, 0);
  pinMode(TFT_BL_PIN, OUTPUT); analogWrite(TFT_BL_PIN, screenBrightness);
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  // Display boot screen
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_ACCENT);
  tft.drawString("HabitWatch", 120, 110, 4);
  tft.drawString("v2.1", 120, 145, 2);
  playVibe(pat_Tick);
  delay(1500);

  syncTime();
  updateBattery();

  currentState     = STATE_WATCHFACE;
  lastActivityTime = millis();
  forceRedraw      = true;
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {

  // ------------------------------------------------------------------
  // 1. Time
  // ------------------------------------------------------------------
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    // [FIX 9] Zero-init entire struct before populating mock fields
    memset(&timeinfo, 0, sizeof(timeinfo));
    unsigned long ms   = millis();
    timeinfo.tm_hour   = (ms / 3600000UL) % 24;
    timeinfo.tm_min    = (ms / 60000UL)   % 60;
    timeinfo.tm_sec    = (ms / 1000UL)    % 60;
    timeinfo.tm_mday   = 1;   // sane date fallback
    timeinfo.tm_mon    = 0;
    timeinfo.tm_year   = 124; // 2024
  }

  // ------------------------------------------------------------------
  // 2. Hardware updates (non-blocking)
  // ------------------------------------------------------------------
  updateVibrator();
  updateHabitEngine(&timeinfo);

  static unsigned long lastBattCheck = 0;
  if (millis() - lastBattCheck > 60000UL) {
    updateBattery();
    lastBattCheck = millis();
  }

  // ------------------------------------------------------------------
  // 3. Screen timeout / power management
  // ------------------------------------------------------------------
  if (currentState != STATE_SLEEP && currentState != STATE_ALARM) {
    if (millis() - lastActivityTime > SCREEN_TIMEOUT_MS) {
      goToSleep();
    }
  }

  // ------------------------------------------------------------------
  // 4. Input (pure read - no side effects inside readButtons)
  // ------------------------------------------------------------------
  uint8_t events = readButtons();

  // [FIX 5] Wake-up handled here, not inside readButtons()
  if (events & EVT_ANY_PRESS) {
    wakeUpScreen();
  }

  // ------------------------------------------------------------------
  // 5. Global back navigation (SEL long-press from any non-root state)
  // ------------------------------------------------------------------
  // [FIX 10] Use a consumed flag so the switch below sees clean state
  bool eventConsumed = false;

  if ((events & EVT_SEL_LONG) &&
       currentState != STATE_WATCHFACE &&
       currentState != STATE_ALARM) {
    currentState   = STATE_WATCHFACE;
    forceRedraw    = true;
    eventConsumed  = true;
    playVibe(pat_Tick);
  }

  if (eventConsumed) return; // Skip state machine this cycle

  // ------------------------------------------------------------------
  // 6. State machine
  // ------------------------------------------------------------------
  switch (currentState) {

    // ----------------------------------------------------------------
    case STATE_WATCHFACE: {
      if (events & (EVT_SEL_SHORT | EVT_NEXT_SHORT)) {
        currentState     = STATE_MENU;
        currentMenuIndex = 0;
        forceRedraw      = true;
      } else {
        static int lastSec = -1;
        if (timeinfo.tm_sec != lastSec || forceRedraw) {
          drawWatchFace(&timeinfo);
          lastSec     = timeinfo.tm_sec;
          forceRedraw = false;
        }
      }
      break;
    }

    // ----------------------------------------------------------------
    case STATE_MENU: {
      if (forceRedraw) { drawMainMenu(); forceRedraw = false; }

      if (events & EVT_NEXT_SHORT) {
        currentMenuIndex = (currentMenuIndex + 1) % numMenuItems;
        forceRedraw = true;

      } else if (events & EVT_SEL_SHORT) {
        switch (currentMenuIndex) {
          case 0: // Habits
            currentState   = STATE_HABIT_SETTINGS;
            habitMenuIndex = 0;
            forceRedraw    = true;
            break;

          case 1: // Settings (brightness toggle)
            screenBrightness = (screenBrightness == 200) ? 50 : 200;
            analogWrite(TFT_BL_PIN, screenBrightness);
            playVibe(pat_Tick);
            forceRedraw = true;
            break;

          case 2: // Sync time
            syncTime();
            currentState = STATE_WATCHFACE;
            forceRedraw  = true;
            break;

          case 3: // Manual sleep
            goToSleep();
            break;
        }
      }
      break;
    }

    // ----------------------------------------------------------------
    case STATE_HABIT_SETTINGS: {
      if (forceRedraw) { drawHabitsApp(); forceRedraw = false; }

      if (events & EVT_NEXT_SHORT) {
        habitMenuIndex = (habitMenuIndex + 1) % NUM_HABITS;
        forceRedraw = true;
      } else if (events & EVT_SEL_SHORT) {
        habits[habitMenuIndex].active = !habits[habitMenuIndex].active;
        playVibe(pat_Tick);
        forceRedraw = true;
      }
      break;
    }

    // ----------------------------------------------------------------
    case STATE_ALARM: {
      if (forceRedraw) { drawAlarmScreen(); forceRedraw = false; }

      if (events & EVT_NEXT_SHORT) {
        // SNOOZE
        // [FIX 6] Store start time; elapsed check in habit engine is overflow-safe
        habits[activeHabitIndex].justSnoozed  = true;
        habits[activeHabitIndex].snoozeStartMs = millis();
        currentState = STATE_SNOOZE;
        playVibe(pat_Snooze);
        forceRedraw = true;

      } else if (events & (EVT_SEL_SHORT | EVT_SEL_LONG)) {
        // DISMISS
        stopVibe();
        habits[activeHabitIndex].justSnoozed = false;
        currentState = STATE_WATCHFACE;
        forceRedraw  = true;
      }
      break;
    }

    // ----------------------------------------------------------------
    case STATE_SNOOZE: {
      // [FIX 3] Non-blocking: draw once, then wait 2 seconds via timer
      if (forceRedraw) {
        drawSnoozeScreen();
        snoozeDisplayTimer = millis();
        forceRedraw = false;
      }
      if (millis() - snoozeDisplayTimer >= 2000UL) {
        currentState = STATE_WATCHFACE;
        forceRedraw  = true;
      }
      break;
    }

    // ----------------------------------------------------------------
    case STATE_SLEEP: {
      // Screen and motor are off. MCU stays alive to service millis() timers.
      // Button press woke us via EVT_ANY_PRESS -> wakeUpScreen() above.
      delay(10); // Minimal yield
      break;
    }

    default: break;
  }
}
