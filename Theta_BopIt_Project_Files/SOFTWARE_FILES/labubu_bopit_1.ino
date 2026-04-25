#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Wire.h>
#include <Adafruit_BNO055.h>
#include "ESP_I2S.h"
#include <math.h>

// ─── Pin Definitions (verified) ──────────────────────────────────────────────
#define PIN_SDA         4
#define PIN_SCL         5
#define PIN_POT         6
#define PIN_BUSY_AUDIO  7
#define PIN_BL          8
#define PIN_DC_LCD      9
#define PIN_DISP_CS     10
#define PIN_MOSI        11
#define PIN_SCK         12
#define PIN_MISO        13
#define PIN_RESET_LCD   14
#define PIN_DATA_I2S    15
#define PIN_CLK_I2S     16
#define PIN_TX_AUDIO    18
#define PIN_RX_AUDIO    17
#define PIN_INT_IMU     38
#define PIN_SEL_CH      39
#define PIN_BOOT_IMU    40
#define PIN_RST_IMU     21

// ─── Hardware Config ─────────────────────────────────────────────────────────
#define SAMPLE_LEN          256

// ─── Game Tuning ─────────────────────────────────────────────────────────────
#define SCREAM_THRESH       800.0f
#define POT_TWIST_THRESH    300
#define INPUT_TIMEOUT_MS    4000
#define MIN_TIMEOUT_MS      500        // floor per spec
#define TIMEOUT_DECAY_PCT   12          // reduce timeout by 12% every SPEEDUP_INTERVAL actions
#define SPEEDUP_INTERVAL    5
#define POINTS_PER_SUCCESS  5           // per spec
#define WIN_SCORE           100         // per spec — game ends at 100

// ─── Wrong-Action Detection ──────────────────────────────────────────────────
// When a round is active for action X, we also monitor the OTHER inputs to
// detect the player performing the wrong action → instant fail.
// Mic is intentionally EXCLUDED from fail detection (or uses a much higher
// threshold) to avoid false triggers from ambient noise, the speaker itself
// playing a prompt, heavy breathing, etc.
#define WRONG_POT_THRESH    500     // larger than success threshold — must be a clear twist
#define WRONG_TILT_DEG      30.0f   // larger than cradle threshold — must be a clear tilt
#define WRONG_SCREAM_ENABLED 0      // 0 = never fail on loud noise, 1 = fail only on insane volume
#define WRONG_SCREAM_THRESH 1800.0f // only used if WRONG_SCREAM_ENABLED = 1 (much higher than success)

// ─── Audio Timing ────────────────────────────────────────────────────────────
// DFPlayer BUSY pin is active-LOW while playing. We block until it's HIGH
// (idle) or we hit the timeout below. Prevents the game from clipping a
// prompt or sitting forever on a dead DFPlayer.
#define AUDIO_WAIT_MAX_MS   3500        // hard ceiling for any single clip
#define AUDIO_START_MS      150         // give DFPlayer time to assert BUSY

// ─── Cradle (Tilt) Tuning ────────────────────────────────────────────────────
// Uses roll angle (gravity-referenced, no drift). Player must rock the device
// from one side to the other — like cradling a baby. Two side-changes = win.
#define CRADLE_TILT_DEG     22.0f   // how far you must tilt to register one side
#define CRADLE_HYST_DEG     8.0f    // must return below this before the other side counts
#define CRADLE_REQUIRED     2       // number of side-changes needed (left↔right)
#define CRADLE_SETTLE_DEG   30.0f   // if starting tilt > this, ask user to level it first
#define CRADLE_FRAME_MS     33      // ~30 fps redraw cap

// ─── Colors ──────────────────────────────────────────────────────────────────
#define COL_BG          0x0000
#define COL_TITLE       0xFFE0
#define COL_ACTION      0xF800
#define COL_SCORE       0x07FF
#define COL_SUCCESS     0x07E0
#define COL_FAIL        0xF800
#define COL_BAR         0x07E0
#define COL_BAR_BG      0x4208
#define COL_CRADLE      0xFD20   // orange
#define COL_LIT         0x07E0   // green when a side is hit
#define COL_WHITE       0xFFFF
#define COL_DIMWHITE    0x8410

// ─── DFPlayer Sound Bank ─────────────────────────────────────────────────────
// Uses DFPlayer command 0x0F (play file by name in folder). File numbers
// below correspond DIRECTLY to the 3-digit filename prefix on the SD card —
// file write order is IRRELEVANT with this command.
//
// Confirmed working via diag sketch: folder mode reads filenames correctly.
//
// Required SD layout (files 000-014, exactly as originally recorded):
//   /01/000.mp3  "Listen to exactly what I say..."       ← start
//   /01/001.mp3  "Twist my ear, or else!"                ← twist prompt
//   /01/002.mp3  "Tilt me, tilt me, tilt me!"            ← tilt prompt
//   /01/003.mp3  "Scream at me, you won't!"              ← scream prompt
//   /01/004.mp3  "Ha ha ha, you lost, LOSER!!!"          ┐
//   /01/005.mp3  "Too slow! Pathetic!"                   │ fail bank
//   /01/006.mp3  "Game over! I win, obviously!"          ┘
//   /01/007.mp3  "Too easy. Boring!"                     ┐
//   /01/008.mp3  "Faster! Keep up!"                      │ level-up bank
//   /01/009.mp3  "Finally a challenge. Maybe."           ┘
//   /01/010.mp3  "Fine. Next!"                           ┐
//   /01/011.mp3  "Lucky. Don't get cocky!"               │ success bank
//   /01/012.mp3  "Hmph. Again!"                          ┘
//   /01/013.mp3  "Good job, human..."                    ┐ win bank
//   /01/014.mp3  "New high score!"                       ┘
//
// Folder must be named exactly "01" (2 digits). Filenames must start with
// exactly 3 digits. Extensions .mp3 or .wav both work.

#define SFX_START           0   // file 000
#define SFX_PROMPT_TWIST    1   // file 001
#define SFX_PROMPT_TILT     2   // file 002
#define SFX_PROMPT_SCREAM   3   // file 003

const uint8_t SFX_FAIL_BANK[]    = { 4, 5, 6 };
const uint8_t SFX_LEVELUP_BANK[] = { 7, 8, 9 };
const uint8_t SFX_SUCCESS_BANK[] = { 10, 11, 12 };
const uint8_t SFX_WIN_BANK[]     = { 13, 14 };

#define SFX_FAIL_COUNT      (sizeof(SFX_FAIL_BANK))
#define SFX_LEVELUP_COUNT   (sizeof(SFX_LEVELUP_BANK))
#define SFX_SUCCESS_COUNT   (sizeof(SFX_SUCCESS_BANK))
#define SFX_WIN_COUNT       (sizeof(SFX_WIN_BANK))

// ─── Action Enum (declared early so helpers can reference it) ────────────────
enum Action { ACT_TWIST, ACT_SCREAM, ACT_CRADLE, ACT_COUNT };
const char* actionNames[] = { "TWIST IT!", "SCREAM!", "TILT IT!" };

// ─── Objects ─────────────────────────────────────────────────────────────────
Adafruit_ILI9341    tft = Adafruit_ILI9341(&SPI, PIN_DC_LCD, PIN_DISP_CS, PIN_RESET_LCD);
Adafruit_BNO055     bno(55, 0x28, &Wire);
I2SClass            i2s;
HardwareSerial      dfSerial(1);

// ─── Globals ─────────────────────────────────────────────────────────────────
int     score           = 0;
int     highScore       = 0;
int     timeoutMs       = INPUT_TIMEOUT_MS;
int     potBaseline     = 0;
int     roundCount      = 0;
int     twistCount      = 0;
int     screamCount     = 0;
int     cradleCount     = 0;
float   fastestReaction = 99999;
float   totalReaction   = 0;
bool    gameRunning     = false;
bool    dfPlayerOk      = false;
bool    imuOk           = false;

int16_t micBuf[SAMPLE_LEN];

// ─── Forward Declarations ────────────────────────────────────────────────────
void    showAction(Action a);
void    showSuccess(float reactionMs);
void    showGameOver();
void    showTitle();
void    showCountdown();
bool    waitForAction(Action a, int timeout, float &reactionMs, bool &wrongAction);
Action  pickAction();
int     promptTrackFor(Action a);
void    startGame();
void    gameLoop();
void    gameOver(bool won);
void    playTrack(int t);
void    playAndWait(int track);
void    playRandomAndWait(const uint8_t* bank, size_t n);
void    waitForAudioToFinish();
void    centerText(const char* txt, int y, int size, uint16_t color);
void    drawProgressBar(int x, int y, int w, int h, float pct, uint16_t fg, uint16_t bg);
float   readMicRMS();
float   readRollDeg();
void    drawCradleScene(float roll, int sidesHit, bool leftLit, bool rightLit,
                        float elapsed, int timeout, bool dirty);
void    dfPlayTrack(uint16_t track);
void    dfSetVolume(uint8_t vol);
bool    detectWrongAction(Action expected, int wrongPotBase, float wrongTiltBase);

// ─── DFPlayer Raw Commands ───────────────────────────────────────────────────
// IMPORTANT: Uses command 0x0F (play specific file in folder) — NOT 0x03
// (play by index). With 0x0F, DFPlayer reads the 3-digit prefix from the
// filename (e.g. /01/005.mp3 → file 5). Order on the SD card doesn't matter.
//
// File layout required on SD:
//   /01/001.mp3, /01/002.mp3, ... /01/015.mp3
//   (Folder name: 2 digits. File prefix: 3 digits. Extra text after is OK.)
//
// The DFPlayer CHECKSUM field was omitted in the original 0x03 version — we
// compute it properly here because some clone modules reject unchecksummed
// 0x0F commands even though they accepted 0x03 silently.

static uint16_t dfChecksum(const uint8_t* frame /* 6 bytes from 0xFF to param_lo */) {
    uint16_t sum = 0;
    for (int i = 0; i < 6; i++) sum += frame[i];
    return -sum;  // two's complement per DFPlayer spec
}

// Play file NNN in folder FF (e.g. dfPlayFile(1, 5) → /01/005.mp3).
void dfPlayFile(uint8_t folder, uint8_t fileNum) {
    uint8_t body[6] = { 0xFF, 0x06, 0x0F, 0x00, folder, fileNum };
    uint16_t sum = dfChecksum(body);
    uint8_t cmd[10] = {
        0x7E, 0xFF, 0x06, 0x0F, 0x00, folder, fileNum,
        (uint8_t)(sum >> 8), (uint8_t)(sum & 0xFF), 0xEF
    };
    dfSerial.write(cmd, sizeof(cmd));
}

// Wrapper used by the rest of the game: all files live in /01/.
void dfPlayTrack(uint16_t track) {
    dfPlayFile(1, (uint8_t)track);
}

void dfSetVolume(uint8_t vol) {
    uint8_t body[6] = { 0xFF, 0x06, 0x06, 0x00, 0x00, vol };
    uint16_t sum = dfChecksum(body);
    uint8_t cmd[10] = {
        0x7E, 0xFF, 0x06, 0x06, 0x00, 0x00, vol,
        (uint8_t)(sum >> 8), (uint8_t)(sum & 0xFF), 0xEF
    };
    dfSerial.write(cmd, sizeof(cmd));
}

void playTrack(int t) {
    if (dfPlayerOk) {
        dfPlayTrack(t);
        delay(80);
    }
}

// ─── Wait for DFPlayer BUSY pin to go idle (HIGH) ────────────────────────────
void waitForAudioToFinish() {
    if (!dfPlayerOk) return;

    unsigned long t0 = millis();

    while ((millis() - t0) < AUDIO_START_MS) {
        if (digitalRead(PIN_BUSY_AUDIO) == LOW) break;
        delay(5);
    }

    unsigned long t1 = millis();
    while ((millis() - t1) < AUDIO_WAIT_MAX_MS) {
        if (digitalRead(PIN_BUSY_AUDIO) == HIGH) {
            unsigned long confirm = millis();
            bool stable = true;
            while ((millis() - confirm) < 30) {
                if (digitalRead(PIN_BUSY_AUDIO) == LOW) { stable = false; break; }
                delay(2);
            }
            if (stable) return;
        }
        delay(5);
    }
}

void playAndWait(int track) {
    if (!dfPlayerOk) return;
    dfPlayTrack(track);
    waitForAudioToFinish();
}

void playRandomAndWait(const uint8_t* bank, size_t n) {
    if (!dfPlayerOk || n == 0) return;
    uint8_t track = bank[random(0, (long)n)];
    playAndWait(track);
}

// ─── IMU Helper ──────────────────────────────────────────────────────────────
float readRollDeg() {
    if (!imuOk) return 0.0f;
    imu::Vector<3> euler = bno.getVector(Adafruit_BNO055::VECTOR_EULER);
    return (float)euler.y();
}

// ─── Mic RMS ─────────────────────────────────────────────────────────────────
float readMicRMS() {
    size_t bytesRead = i2s.readBytes((char*)micBuf, sizeof(micBuf));
    int count = bytesRead / sizeof(int16_t);
    if (count == 0) return 0.0f;

    float sumSq = 0;
    for (int i = 0; i < count; i++) {
        float s = (float)micBuf[i];
        sumSq += s * s;
    }
    return sqrtf(sumSq / (float)count);
}

// ─── Wrong-Action Detector ───────────────────────────────────────────────────
// Called each frame of waitForAction. Returns true if the player is clearly
// doing something OTHER than the expected action.
//
//   expected      - the action the player was asked to perform
//   wrongPotBase  - pot reading captured at round start (for detecting a
//                   twist when the expected action isn't twist)
//   wrongTiltBase - roll angle captured at round start (for detecting a
//                   tilt when the expected action isn't tilt)
//
// Mic is deliberately leaky: we don't fail on loud noise by default because
// ambient sound and speaker prompts can false-trigger. Set
// WRONG_SCREAM_ENABLED=1 if you want extra-loud noise to count as a fail.
bool detectWrongAction(Action expected, int wrongPotBase, float wrongTiltBase) {
    // Check unintended POT movement
    if (expected != ACT_TWIST) {
        int current = analogRead(PIN_POT);
        if (abs(current - wrongPotBase) > WRONG_POT_THRESH) return true;
    }

    // Check unintended TILT movement (roll axis)
    if (expected != ACT_CRADLE && imuOk) {
        float roll = readRollDeg();
        if (fabsf(roll - wrongTiltBase) > WRONG_TILT_DEG) return true;
    }

    // Check unintended SCREAM (optional — off by default)
#if WRONG_SCREAM_ENABLED
    if (expected != ACT_SCREAM) {
        float rms = readMicRMS();
        if (rms > WRONG_SCREAM_THRESH) return true;
    }
#endif

    return false;
}

// ─── Draw Helpers ────────────────────────────────────────────────────────────
void drawProgressBar(int x, int y, int w, int h, float pct, uint16_t fg, uint16_t bg) {
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    int filled = (int)(w * pct);
    if (filled > 0) tft.fillRect(x, y, filled, h, fg);
    if (filled < w) tft.fillRect(x + filled, y, w - filled, h, bg);
}

void centerText(const char* txt, int y, int size, uint16_t color) {
    tft.setTextSize(size);
    tft.setTextColor(color);
    int16_t x1, y1;
    uint16_t w, h;
    tft.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    tft.setCursor((320 - w) / 2, y);
    tft.print(txt);
}

// ─── Screens ─────────────────────────────────────────────────────────────────
void showTitle() {
    tft.fillScreen(COL_BG);
    centerText("LABUBU", 30, 4, COL_TITLE);
    centerText("BOP IT!", 80, 4, COL_ACTION);

    tft.setTextSize(2);
    tft.setTextColor(COL_DIMWHITE);
    tft.setCursor(20, 140);
    tft.print("Twist | Scream | Tilt");

    if (highScore > 0) {
        char hs[32];
        snprintf(hs, sizeof(hs), "Best: %d", highScore);
        centerText(hs, 180, 2, COL_SCORE);
    }

    centerText("Twist pot to start!", 210, 2, COL_SUCCESS);
}

void showCountdown() {
    for (int i = 3; i >= 1; i--) {
        tft.fillScreen(COL_BG);
        char num[2];
        snprintf(num, sizeof(num), "%d", i);
        centerText(num, 80, 8, COL_TITLE);
        delay(700);
    }
    tft.fillScreen(COL_BG);
    centerText("GO!", 80, 8, COL_SUCCESS);
    delay(500);
}

void showAction(Action a) {
    tft.fillScreen(COL_BG);

    tft.setTextSize(2);
    tft.setTextColor(COL_SCORE);
    tft.setCursor(10, 10);
    tft.printf("Score: %d", score);

    tft.setTextSize(1);
    tft.setTextColor(COL_DIMWHITE);
    tft.setCursor(230, 10);
    tft.printf("Time: %.1fs", timeoutMs / 1000.0f);

    centerText(actionNames[a], 60, 4, COL_ACTION);

    switch (a) {
        case ACT_TWIST:  centerText("Rotate the ear!",      110, 2, COL_DIMWHITE); break;
        case ACT_SCREAM: centerText("Yell at it!",          110, 2, COL_DIMWHITE); break;
        case ACT_CRADLE: centerText("Rock side to side!",   110, 2, COL_DIMWHITE); break;
        default: break;
    }
}

void showSuccess(float reactionMs) {
    tft.fillScreen(COL_BG);
    centerText("NICE!", 40, 5, COL_SUCCESS);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", score);
    centerText(buf, 100, 6, COL_WHITE);

    snprintf(buf, sizeof(buf), "%.0f ms", reactionMs);
    centerText(buf, 170, 2, COL_SCORE);

    if (reactionMs <= fastestReaction) {
        centerText("NEW BEST!", 200, 2, COL_TITLE);
    }
}

void showGameOver() {
    tft.fillScreen(COL_BG);
    bool won = (score >= WIN_SCORE);

    if (won) {
        centerText("YOU",  15, 5, COL_SUCCESS);
        centerText("WON!", 65, 5, COL_SUCCESS);
    } else {
        centerText("GAME", 15, 5, COL_FAIL);
        centerText("OVER", 65, 5, COL_FAIL);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    centerText(buf, 120, 3, COL_WHITE);

    if (score >= highScore) {
        highScore = score;
        centerText("NEW HIGH SCORE!", 155, 2, COL_TITLE);
    } else {
        snprintf(buf, sizeof(buf), "Best: %d", highScore);
        centerText(buf, 155, 2, COL_DIMWHITE);
    }

    tft.setTextSize(1);
    tft.setTextColor(COL_SCORE);

    tft.setCursor(20, 185);
    tft.printf("Twists: %d  Screams: %d  Cradles: %d", twistCount, screamCount, cradleCount);

    if (roundCount > 0) {
        float avgReaction = totalReaction / roundCount;
        tft.setCursor(20, 200);
        tft.printf("Avg reaction: %.0f ms", avgReaction);
        tft.setCursor(20, 215);
        tft.printf("Fastest: %.0f ms", fastestReaction < 99999 ? fastestReaction : 0);
    }

    tft.setCursor(20, 230);
    tft.setTextColor(COL_DIMWHITE);
    tft.print("Twist pot to play again");
}

// ─── Cradle Scene ────────────────────────────────────────────────────────────
void drawCradleScene(float roll, int sidesHit, bool leftLit, bool rightLit,
                     float elapsed, int timeout, bool dirty) {
    const int cx = 160, baseY = 145, armLen = 55;

    if (dirty) {
        tft.fillRect(0, 130, 320, 80, COL_BG);
        tft.drawCircle(40, baseY + armLen, 24, leftLit ? COL_LIT : COL_DIMWHITE);
        tft.drawCircle(40, baseY + armLen, 25, leftLit ? COL_LIT : COL_DIMWHITE);
        tft.drawCircle(280, baseY + armLen, 24, rightLit ? COL_LIT : COL_DIMWHITE);
        tft.drawCircle(280, baseY + armLen, 25, rightLit ? COL_LIT : COL_DIMWHITE);
        tft.fillCircle(cx, baseY, 4, COL_WHITE);
    } else {
        tft.drawCircle(40,  baseY + armLen, 24, leftLit  ? COL_LIT : COL_DIMWHITE);
        tft.drawCircle(40,  baseY + armLen, 25, leftLit  ? COL_LIT : COL_DIMWHITE);
        tft.drawCircle(280, baseY + armLen, 24, rightLit ? COL_LIT : COL_DIMWHITE);
        tft.drawCircle(280, baseY + armLen, 25, rightLit ? COL_LIT : COL_DIMWHITE);
    }

    tft.fillRect(cx - armLen - 12, baseY + 1, (armLen + 12) * 2, armLen + 14, COL_BG);

    float drawRoll = roll;
    if (drawRoll >  90.0f) drawRoll =  90.0f;
    if (drawRoll < -90.0f) drawRoll = -90.0f;

    float rad = drawRoll * (float)DEG_TO_RAD;
    int px = cx + (int)(armLen * sinf(rad));
    int py = baseY + (int)(armLen * cosf(rad));

    tft.drawLine(cx, baseY, px, py, COL_CRADLE);
    tft.drawLine(cx + 1, baseY, px + 1, py, COL_CRADLE);
    tft.fillCircle(px, py, 8, COL_CRADLE);

    float pct = 1.0f - (elapsed / (float)timeout);
    drawProgressBar(10, 220, 300, 12, pct, COL_BAR, COL_BAR_BG);

    char ind[12];
    snprintf(ind, sizeof(ind), "%d/%d", sidesHit, CRADLE_REQUIRED);
    tft.setTextSize(2);
    tft.setTextColor(COL_SCORE);
    tft.fillRect(140, 195, 40, 18, COL_BG);
    tft.setCursor(140, 197);
    tft.print(ind);
}

// ─── Input Detection ─────────────────────────────────────────────────────────
// Returns true if player successfully performed the expected action.
// Sets wrongAction=true if player failed by doing the WRONG action (vs timing out).
bool waitForAction(Action a, int timeout, float &reactionMs, bool &wrongAction) {
    unsigned long start = millis();
    wrongAction = false;

    // Per-action local state
    int   potBase     = 0;
    float lastRoll    = 0;
    int   sidesHit    = 0;
    int   lastSide    = 0;
    bool  armed       = true;
    bool  leftLit     = false;
    bool  rightLit    = false;
    unsigned long lastDraw = 0;
    bool  firstDraw   = true;

    // Baselines for wrong-action detection — captured once at round start so
    // small drifts in the non-target sensor don't false-trigger.
    int   wrongPotBase  = analogRead(PIN_POT);
    float wrongTiltBase = readRollDeg();

    if (a == ACT_TWIST) {
        potBase = wrongPotBase;  // reuse the reading
    }

    if (a == ACT_CRADLE) {
        float startRoll = readRollDeg();
        if (fabsf(startRoll) > CRADLE_SETTLE_DEG) {
            tft.fillRect(0, 130, 320, 80, COL_BG);
            centerText("Hold level...", 155, 2, COL_DIMWHITE);
            unsigned long settleStart = millis();
            while (fabsf(readRollDeg()) > CRADLE_SETTLE_DEG &&
                   (millis() - settleStart) < 800) {
                delay(20);
            }
            start = millis();
            // Re-capture baselines after settle so the grace motion doesn't
            // get flagged as wrong-action.
            wrongPotBase  = analogRead(PIN_POT);
            wrongTiltBase = readRollDeg();
        }
        lastRoll = readRollDeg();
        lastSide = 0;
    }

    while ((millis() - start) < (unsigned long)timeout) {
        float elapsed = millis() - start;
        float pct = 1.0f - (elapsed / (float)timeout);
        bool triggered = false;

        // ── Wrong-action check runs BEFORE the expected-action check so that
        // a clear wrong move fails immediately even if the correct action
        // also happens to register on the same frame.
        if (detectWrongAction(a, wrongPotBase, wrongTiltBase)) {
            wrongAction = true;
            reactionMs = (float)(millis() - start);
            return false;
        }

        switch (a) {
            case ACT_TWIST: {
                int current = analogRead(PIN_POT);
                if (abs(current - potBase) > POT_TWIST_THRESH) triggered = true;
                drawProgressBar(10, 220, 300, 12, pct, COL_BAR, COL_BAR_BG);
                delay(15);
                break;
            }

            case ACT_SCREAM: {
                float rms = readMicRMS();
                int barW = constrain(map((int)rms, 0, 2000, 0, 300), 0, 300);
                tft.fillRect(10, 180, 300, 16, COL_BG);
                tft.fillRect(10, 180, barW, 16, COL_TITLE);
                int threshX = map((int)SCREAM_THRESH, 0, 2000, 0, 300);
                tft.drawFastVLine(10 + threshX, 180, 16, COL_FAIL);
                if (rms > SCREAM_THRESH) triggered = true;
                drawProgressBar(10, 220, 300, 12, pct, COL_BAR, COL_BAR_BG);
                delay(15);
                break;
            }

            case ACT_CRADLE: {
                float roll = readRollDeg();
                roll = 0.6f * roll + 0.4f * lastRoll;
                lastRoll = roll;

                if (lastSide == 0) {
                    if (roll <= -CRADLE_TILT_DEG) {
                        lastSide = -1;
                        sidesHit++;
                        leftLit = true;
                        armed = false;
                    } else if (roll >= CRADLE_TILT_DEG) {
                        lastSide = +1;
                        sidesHit++;
                        rightLit = true;
                        armed = false;
                    }
                } else {
                    if (!armed && fabsf(roll) < CRADLE_HYST_DEG) {
                        armed = true;
                    }
                    if (armed && lastSide == -1 && roll >= CRADLE_TILT_DEG) {
                        lastSide = +1;
                        sidesHit++;
                        rightLit = true;
                        armed = false;
                    } else if (armed && lastSide == +1 && roll <= -CRADLE_TILT_DEG) {
                        lastSide = -1;
                        sidesHit++;
                        leftLit = true;
                        armed = false;
                    }
                }

                unsigned long now = millis();
                if (now - lastDraw >= CRADLE_FRAME_MS || firstDraw) {
                    drawCradleScene(roll, sidesHit, leftLit, rightLit,
                                    elapsed, timeout, firstDraw);
                    lastDraw = now;
                    firstDraw = false;
                }

                if (sidesHit >= CRADLE_REQUIRED) triggered = true;

                delay(5);
                break;
            }

            default:
                break;
        }

        if (triggered) {
            reactionMs = (float)(millis() - start);
            return true;
        }
    }

    reactionMs = (float)timeout;
    return false;
}

// ─── Game Flow ───────────────────────────────────────────────────────────────
Action pickAction() {
    static Action last = ACT_COUNT;
    Action a;
    while (true) {
        a = (Action)random(0, ACT_COUNT);
        if (a == last) continue;
        if (a == ACT_CRADLE && !imuOk) continue;
        break;
    }
    last = a;
    return a;
}

void startGame() {
    score = 0;
    timeoutMs = INPUT_TIMEOUT_MS;
    roundCount = 0;
    twistCount = 0;
    screamCount = 0;
    cradleCount = 0;
    fastestReaction = 99999;
    totalReaction = 0;
    gameRunning = true;

    playAndWait(SFX_START);
    showCountdown();
}

int promptTrackFor(Action a) {
    switch (a) {
        case ACT_TWIST:  return SFX_PROMPT_TWIST;
        case ACT_SCREAM: return SFX_PROMPT_SCREAM;
        case ACT_CRADLE: return SFX_PROMPT_TILT;
        default:         return SFX_PROMPT_TWIST;
    }
}

void gameLoop() {
    Action a = pickAction();
    showAction(a);

    playAndWait(promptTrackFor(a));

    float reactionMs = 0;
    bool  wrongAction = false;
    bool  success = waitForAction(a, timeoutMs, reactionMs, wrongAction);

    if (success) {
        score += POINTS_PER_SUCCESS;
        roundCount++;
        totalReaction += reactionMs;
        if (reactionMs < fastestReaction) fastestReaction = reactionMs;

        switch (a) {
            case ACT_TWIST:  twistCount++;  break;
            case ACT_SCREAM: screamCount++; break;
            case ACT_CRADLE: cradleCount++; break;
            default: break;
        }

        if (score >= WIN_SCORE) {
            showSuccess(reactionMs);
            delay(400);
            playRandomAndWait(SFX_WIN_BANK, SFX_WIN_COUNT);
            gameOver(true);
            return;
        }

        playRandomAndWait(SFX_SUCCESS_BANK, SFX_SUCCESS_COUNT);
        showSuccess(reactionMs);

        if (roundCount % SPEEDUP_INTERVAL == 0) {
            int newTimeout = (timeoutMs * (100 - TIMEOUT_DECAY_PCT)) / 100;
            if (newTimeout < MIN_TIMEOUT_MS) newTimeout = MIN_TIMEOUT_MS;
            if (newTimeout != timeoutMs) {
                timeoutMs = newTimeout;
                centerText("SPEED UP!", 220, 2, COL_TITLE);
                playRandomAndWait(SFX_LEVELUP_BANK, SFX_LEVELUP_COUNT);
            }
        }

        delay(400);
    } else {
        // Fail — either timeout or wrong action. Briefly show why on screen.
        if (wrongAction) {
            tft.fillRect(0, 150, 320, 30, COL_BG);
            centerText("WRONG ACTION!", 155, 3, COL_FAIL);
            delay(300);  // brief visual beat before the insult
        }
        playRandomAndWait(SFX_FAIL_BANK, SFX_FAIL_COUNT);
        gameOver(false);
    }
}

void gameOver(bool won) {
    gameRunning = false;
    showGameOver();
    (void)won;
    delay(3000);
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("Labubu Bop It — booting...");

    // LCD
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);
    pinMode(PIN_RESET_LCD, OUTPUT);
    digitalWrite(PIN_RESET_LCD, LOW);
    delay(20);
    digitalWrite(PIN_RESET_LCD, HIGH);
    delay(150);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_DISP_CS);
    tft.begin(20000000UL);
    tft.setRotation(1);
    tft.fillScreen(COL_BG);
    Serial.println("LCD OK");

    // BNO055
    Wire.begin(PIN_SDA, PIN_SCL);
    pinMode(PIN_RST_IMU, OUTPUT);
    digitalWrite(PIN_RST_IMU, LOW);
    delay(20);
    digitalWrite(PIN_RST_IMU, HIGH);
    delay(50);
    if (!bno.begin()) {
        Serial.println("BNO055 FAIL");
        centerText("IMU FAIL", 100, 3, COL_FAIL);
        imuOk = false;
    } else {
        bno.setExtCrystalUse(true);
        imuOk = true;
        Serial.println("BNO055 OK");
    }

    // PDM Mic
    pinMode(PIN_SEL_CH, OUTPUT);
    digitalWrite(PIN_SEL_CH, LOW);
    i2s.setPinsPdmRx(PIN_CLK_I2S, PIN_DATA_I2S);
    if (!i2s.begin(I2S_MODE_PDM_RX, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.println("PDM Mic FAIL");
        centerText("MIC FAIL", 100, 3, COL_FAIL);
        while (1);
    }
    Serial.println("PDM Mic OK");

    // Pot
    pinMode(PIN_POT, INPUT);
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);
    Serial.println("Pot OK");

    // DFPlayer
    pinMode(PIN_BUSY_AUDIO, INPUT_PULLUP);
    dfSerial.begin(9600, SERIAL_8N1, PIN_RX_AUDIO, PIN_TX_AUDIO);
    delay(1000);
    dfSetVolume(30);
    dfPlayerOk = true;
    Serial.println("DFPlayer init sent");

    randomSeed(analogRead(PIN_POT) ^ micros());

    showTitle();
    Serial.println("Ready — twist pot to start!");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    if (gameRunning) {
        gameLoop();
    } else {
        int baseline = analogRead(PIN_POT);
        while (!gameRunning) {
            int current = analogRead(PIN_POT);
            if (abs(current - baseline) > POT_TWIST_THRESH) {
                startGame();
            }
            delay(50);
        }
    }
}
