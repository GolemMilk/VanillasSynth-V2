/*
  Vanilla Synth V2 (Gen.2) — MASTER LOCKED (DualCore PRO)  [LIGHT TAP PINGPONG DELAY] + WAVE SWITCH
  ------------------------------------------------------------------------------------------------
  Core1: Audio only (I2S, synth, FX, scope capture)
  Core0: UI only (MIDI rx -> event queue, pots, OLED, boot animation, wave switch)

  LOCKED BASE HW:
  - Raspberry Pi Pico 2 + Arduino IDE (rp2040 core 5.5.0)
  - I2S DAC PCM5102A:
      BCLK=GP10, LRCK auto=GP11, DATA=GP12, MUTE/XSMT=GP19 HIGH
  - MIDI IN: 6N138/6N136 -> UART0 RX=GP1 (Serial1 31250)
  - 74HC4051 (8 pots A-H):
      COM -> GP26 (ADC0) via 1k
      S0/S1/S2 -> GP2/3/4
      analogReadResolution(12)
  - OLED SSD1306 128x64 I2C:
      SDA=GP6 (Wire1), SCL=GP7 (Wire1)

  Wave Switch:
  - GP14 -> Alternate (latching) switch -> GND
  - INPUT_PULLUP
  - Every press toggles level HIGH/LOW -> we advance wave on ANY stable change (debounced)

  POTs:
    CH0 A Attack
    CH1 B Tone (base LPF cutoff)
    CH2 C Tail (release)
    CH3 D Volume (true mute at 0)
    CH4 E Phaser mix
    CH5 F Ladder mix
    CH6 G Delay mix (LIGHT: tapped ping-pong 3 echoes, tempo auto)
    CH7 H Octaver mix

  MIDI:
    - NoteOn/Off
    - Pitch Bend
    - Mod Wheel (CC1)
    - Aftertouch (Channel Pressure) -> drive
*/

#include <Arduino.h>
#include <I2S.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <EEPROM.h>
#include "pico/multicore.h"

#include "MidiQueue.h"

// ----- I2S macro clash fix (LOCKED) -----
#ifdef I2S
#undef I2S
#endif
static I2S i2s;

// ===================== PIN LOCKED =====================
static const uint8_t PIN_I2S_BCLK = 10;   // GP10
static const uint8_t PIN_I2S_DATA = 12;   // GP12
static const uint8_t PIN_DAC_MUTE = 19;   // GP19 HIGH=unmute

static const uint8_t PIN_MIDI_RX  = 1;    // GP1 -> UART0 RX (Serial1)

static const uint8_t PIN_MUX_S0   = 2;    // GP2
static const uint8_t PIN_MUX_S1   = 3;    // GP3
static const uint8_t PIN_MUX_S2   = 4;    // GP4
static const uint8_t PIN_MUX_COM  = 26;   // GP26 ADC0

static const uint8_t PIN_OLED_SDA = 6;    // GP6  (Wire1 SDA)
static const uint8_t PIN_OLED_SCL = 7;    // GP7  (Wire1 SCL)

// Wave switch (alternate latch) pin
static const uint8_t PIN_WAVE_BTN = 14;   // GP14 -> Latch SW to GND

// ===================== AUDIO CONST =====================
static const uint32_t SAMPLE_RATE = 44100;
static const float    DT          = 1.0f / (float)SAMPLE_RATE;

// ===================== 74HC4051 ch =====================
enum {
  CH_A_ATTACK = 0,
  CH_B_TONE   = 1,
  CH_C_TAIL   = 2,
  CH_D_VOL    = 3,
  CH_E_PHASER = 4,
  CH_F_LADDER = 5,
  CH_G_DELAY  = 6,
  CH_H_OCT    = 7,
  CH_COUNT    = 8
};

// ===================== WAVE SELECT =====================
// 0:SINE 1:SAW 2:SQR 3:TRI
static volatile uint8_t  g_waveCur = 1;             // Core1 reads
static volatile uint8_t  g_waveReq = 1;             // Core0 updates
static volatile uint32_t g_waveOverlayUntilMs = 0;  // overlay timer (ms)

static const char* WAVE_NAME[4] = { "SINE", "SAW", "SQR", "TRI" };

// ===================== OLED =====================
U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ===================== SHARED PARAMS (Core0 -> Core1) =====================
// 起動直後「VOLが未読で無音」を避けるため、VOLだけ0.5初期化（他は0）
static volatile float g_pot[CH_COUNT] = {0,0,0,0.5f,0,0,0,0};

static volatile float g_bpm    = 120.0f;  // smoothed bpm
static volatile float g_noteUS = 0.0f;    // smoothed note interval (us)

static volatile uint8_t  g_voiceCount = 0;
static volatile uint32_t g_clipHoldUntilMs = 0;

// CC / AT / Pitchbend shared
static volatile float g_mod = 0.0f;    // CC1 0..1
static volatile float g_pb  = 0.0f;    // -1..+1
static volatile float g_at  = 0.0f;    // aftertouch 0..1

// Scope buffer (Core1 writes, Core0 reads)
static const int   SCOPE_W = 128;
static volatile uint16_t g_scopeSeq = 0;
static volatile int16_t  g_scopeBuf[SCOPE_W];

// ===================== HELPERS =====================
static inline float clampf(float x, float lo, float hi) { if (x < lo) return lo; if (x > hi) return hi; return x; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline int16_t f2s16(float x) { x = clampf(x, -1.0f, 1.0f); return (int16_t)(x * 32767.0f); }
static inline float s162f(int16_t s) { return (float)s / 32767.0f; }
static inline float sat_soft(float v) { float a = fabsf(v); return v / (1.0f + a); }

/* ===== DRIFT (analog-like slow pitch wander) : Core1-safe ===== */
static volatile float driftMul = 1.0f;

static float  driftCents = 1.5f;
static float  driftTargetCents = 0.0f;
static uint32_t driftCtr = 0;

// Core1-safe PRNG (xorshift32)
static uint32_t driftRng = 0x12345678u;

static inline void driftSeed(uint32_t s)
{
  if (s == 0) s = 0xA5A5A5A5u;
  driftRng = s;
}

static inline float driftRandSigned()
{
  uint32_t x = driftRng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  driftRng = x;

  float u = (float)(x & 0x00FFFFFFu) / 16777215.0f;
  return (u * 2.0f - 1.0f);
}

static inline void driftTick()
{
  constexpr float DRIFT_DEPTH_CENTS = 1.5f;
  constexpr uint32_t DRIFT_UPDATE_SAMPLES = (uint32_t)(SAMPLE_RATE / 200);
  constexpr float DRIFT_SLEW = 0.00005f;

  if (++driftCtr >= DRIFT_UPDATE_SAMPLES) {
    driftCtr = 0;
    driftTargetCents = driftRandSigned() * DRIFT_DEPTH_CENTS;
  }
  driftCents += (driftTargetCents - driftCents) * DRIFT_SLEW;
  driftMul = 1.0f + driftCents * 0.00057762265f; // ln(2)/1200
}

// ===================== MUX READ =====================
static inline void muxSelect(uint8_t ch) {
  digitalWrite(PIN_MUX_S0, (ch & 0x01) ? HIGH : LOW);
  digitalWrite(PIN_MUX_S1, (ch & 0x02) ? HIGH : LOW);
  digitalWrite(PIN_MUX_S2, (ch & 0x04) ? HIGH : LOW);
}
static uint16_t readMux(uint8_t ch) {
  muxSelect(ch);
  delayMicroseconds(40);
  (void)analogRead(PIN_MUX_COM);
  delayMicroseconds(10);
  return (uint16_t)analogRead(PIN_MUX_COM);
}
static inline float norm12(uint16_t v) { return (float)v / 4095.0f; }

// ===================== POT SMOOTH (Core0) =====================
static uint16_t potRaw[CH_COUNT] = {0};
static float    potSm [CH_COUNT] = {0};

static const float POT_SMOOTH_A = 0.08f;
static const float POT_DEADBAND = 0.006f;
static const float POT_ENDSNAP  = 0.010f;
static const float POT_HUD_TH   = 0.020f;

static inline float snap01(float x) {
  if (x < POT_ENDSNAP) return 0.0f;
  if (x > 1.0f - POT_ENDSNAP) return 1.0f;
  return x;
}
static inline float potCurve(float x) {
  return powf(clampf(x,0,1), 1.35f);
}
static void readAllPotsSmoothed() {
  for (int ch=0; ch<CH_COUNT; ch++) {
    potRaw[ch] = readMux((uint8_t)ch);
    float v = norm12(potRaw[ch]);   // 0..1

    float d = fabsf(v - potSm[ch]);
    if (d < POT_DEADBAND) v = potSm[ch];

    potSm[ch] += POT_SMOOTH_A * (v - potSm[ch]);
    float shaped = snap01(potCurve(potSm[ch]));
    g_pot[ch] = shaped;
  }
}

// ===================== OLED HUD / SCOPE (Core0) =====================
static const uint32_t OLED_PERIOD_MS = 80;
static const uint32_t HUD_HOLD_MS   = 1200;

static int      hudIndex = -1;
static uint32_t hudUntilMs = 0;
static float    potPrevHUD[CH_COUNT] = {0};

static const char* POT_FULL[CH_COUNT] = {
  "ATTACK","TONE","TAIL","VOLUME","PHASER","LADDER","DELAY","OCTAVER"
};

static void hudTouchDetect(const float potNow[CH_COUNT]) {
  uint32_t now = millis();
  for (int i=0;i<CH_COUNT;i++) {
    float d = fabsf(potNow[i] - potPrevHUD[i]);
    if (d > POT_HUD_TH) {
      potPrevHUD[i] = potNow[i];
      hudIndex = i;
      hudUntilMs = now + HUD_HOLD_MS;
    }
  }
}

static inline int scopeY(int16_t s, float vol01) {
  float dispGain = 1.0f + 2.2f * (1.0f - clampf(vol01, 0.0f, 1.0f));
  float x = (float)s / 32767.0f;
  x = clampf(x * dispGain, -1.0f, 1.0f);
  int mid = 32;
  int amp = 26;
  return mid - (int)(x * amp);
}

static inline float getBpmSafe() { return clampf((float)g_bpm, 40.0f, 240.0f); }

// -------- “絵文字っぽい”波形アイコン（自前描画）--------
// (x,y) は左上、scale=1で 16x10 程度、scale=3で大きめ
static void drawWaveIcon(int x, int y, uint8_t wv, int scale)
{
  int s = (scale < 1) ? 1 : scale;
  int W = 16 * s;
  int H = 10 * s;

  // 枠を薄く（絵文字っぽさ）
  // u8g2.drawRFrame(x, y, W, H, 2*s);

  // ベースライン
  int midY = y + (H/2);
  // u8g2.drawHLine(x, midY, W);

  // 波形を描く
  if ((wv & 3) == 0) { // SINE
    // ざっくり正弦っぽい 4セグメント
    int x0 = x;
    int x1 = x + (W/4);
    int x2 = x + (W/2);
    int x3 = x + (3*W/4);
    int x4 = x + W;

    int a  = (H/2) - s;
    u8g2.drawLine(x0, midY,      x1, midY - a);
    u8g2.drawLine(x1, midY - a,  x2, midY);
    u8g2.drawLine(x2, midY,      x3, midY + a);
    u8g2.drawLine(x3, midY + a,  x4, midY);
  }
  else if ((wv & 3) == 1) { // SAW
    // 斜め上→落下
    u8g2.drawLine(x, y + H - 1, x + W - 1, y + 1);
    u8g2.drawLine(x + W - 1, y + 1, x + W - 1, y + H - 1);
  }
  else if ((wv & 3) == 2) { // SQUARE
    int top = y + 1;
    int bot = y + H - 2;
    int x1 = x + (W/4);
    int x2 = x + (W/2);
    int x3 = x + (3*W/4);

    u8g2.drawLine(x,  bot, x1, bot);
    u8g2.drawLine(x1, bot, x1, top);
    u8g2.drawLine(x1, top, x2, top);
    u8g2.drawLine(x2, top, x2, bot);
    u8g2.drawLine(x2, bot, x3, bot);
    u8g2.drawLine(x3, bot, x3, top);
    u8g2.drawLine(x3, top, x + W, top);
  }
  else { // TRI
    int top = y + 1;
    int bot = y + H - 2;
    int xm  = x + (W/2);
    u8g2.drawLine(x,  bot, xm, top);
    u8g2.drawLine(xm, top, x + W, bot);
  }
}

static void drawHUD(int idx, int pct) {
  u8g2.setFont(u8g2_font_8x13B_tf);
  u8g2.drawStr(0, 14, POT_FULL[idx]);

  char buf[16];
  snprintf(buf, sizeof(buf), "%3d%%", pct);
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawStr(78, 28, buf);

  int x=0, y=44, w=128, h=10;
  u8g2.drawFrame(x, y, w, h);
  int fill = (pct * (w-2)) / 100;
  if (fill < 0) fill = 0;
  if (fill > (w-2)) fill = (w-2);
  u8g2.drawBox(x+1, y+1, fill, h-2);

  u8g2.setFont(u8g2_font_5x8_tf);
  char st[28];
  static int bpmHold = 0;

  float bpmNow = getBpmSafe();
  int bpmRounded = (int)(bpmNow + 0.5f);
  if (fabsf(bpmNow - (float)bpmHold) > 1.0f) {
    bpmHold = bpmRounded;
  }

  int bpm = bpmHold;
  snprintf(st, sizeof(st), "BPM:%d  V:%d", bpm, (int)g_voiceCount);
  u8g2.drawStr(0, 63, st);

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(68, 10, "VanillaSynth");
  u8g2.drawStr(81, 63, "Ver 2.0");

  // 左下：波形アイコン常時
  drawWaveIcon(2, 54, (uint8_t)g_waveReq, 1);
}

static void drawScope(const int16_t* bufLocal, float vol01) {
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawHLine(1, 32, 126);

  int py = scopeY(bufLocal[0], vol01);
  for (int x=1; x<128; x++) {
    int y = scopeY(bufLocal[x], vol01);
    u8g2.drawLine(x-1, py, x, y);
    py = y;
  }

  u8g2.setFont(u8g2_font_5x8_tf);
  char st[28];
  int bpm = (int)(getBpmSafe() + 0.5f);
  snprintf(st, sizeof(st), "BPM:%d V:%d", bpm, (int)g_voiceCount);
  u8g2.drawStr(2, 10, st);

  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(65, 10, "VanillaSynth");
  u8g2.drawStr(88, 61, "Ver 2.0");

  // 左下：波形アイコン常時
  drawWaveIcon(4, 52, (uint8_t)g_waveReq, 1);
}

// ===================== BOOT ANIMATION (Core0) =====================
static void drawGolem(int x, int y, int phase) {
  u8g2.drawRFrame(x+2, y+0, 12, 9, 2);
  u8g2.drawBox(x+6,  y+3, 2, 2);
  u8g2.drawBox(x+11, y+3, 2, 2);
  u8g2.drawHLine(x+7, y+7, 5);

  if (phase >= 1) {
    u8g2.drawRFrame(x+1, y+9, 14, 8, 2);
    u8g2.drawVLine(x+4,  y+10, 6);
    u8g2.drawVLine(x+12, y+10, 6);
  }
  if (phase >= 2) {
    u8g2.drawBox(x+4,  y+17, 3, 2);
    u8g2.drawBox(x+10, y+17, 3, 2);
    u8g2.drawRFrame(x+16, y+12, 3, 4, 1);
    u8g2.drawBox  (x+16, y+11, 3, 1);
  }
}

static int bootPickNoRepeat3() {
  EEPROM.begin(64);
  int last = EEPROM.read(0);
  if (last < 0 || last > 2) last = 0;

  uint32_t seed = micros() ^ (uint32_t)analogRead(PIN_MUX_COM);
  randomSeed(seed);
  int cur = random(0, 3);
  if (cur == last) cur = (cur + 1 + random(0,2)) % 3;

  EEPROM.write(0, (uint8_t)cur);
  EEPROM.commit();
  return cur;
}

static void bootAnimation() {
  int story = bootPickNoRepeat3();

  const uint32_t dur = 2200;
  const uint32_t t0 = millis();

  const int px[5] = { 2, 70, 56, 0, 112 };
  const int py[5] = { 2, 16, 42, 54, 54 };
  int spawn = random(0,5);

  while (millis() - t0 < dur) {
    uint32_t t = millis() - t0;
    float p = (float)t / (float)dur;

    int x;
    if (p < 0.50f) {
      float q = p / 0.50f;
      x = (int)lerpf(140, 18, q);
    } else if (p < 0.75f) {
      x = 18;
    } else {
      float q = (p - 0.75f) / 0.25f;
      x = (int)lerpf(18, -140, q);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB18_tf);
    u8g2.drawStr(x, 30, "Vanilla");
    u8g2.setFont(u8g2_font_helvB14_tf);
    u8g2.drawStr(x+22, 52, "Synth V2");

    int mid = 50;
    for (int i=0;i<128;i++) {
      float tt = (float)t * 0.006f;
      float v = 0.0f;
      float ph = 0.12f*i + tt;

      if (story==0) {
        v = sinf(ph);
      } else if (story==1) {
        v = (sinf(ph) >= 0.0f) ? 1.0f : -1.0f;
      } else {
        float s = fmodf(ph / (2.0f*PI), 1.0f);
        if (s < 0) s += 1.0f;
        v = (s < 0.5f) ? (4.0f*s - 1.0f) : (3.0f - 4.0f*s);
      }

      int yy = mid - (int)(v * 12.0f);
      if (yy<0) yy=0; if (yy>63) yy=63;
      u8g2.drawPixel(i, yy);
    }

    int phase = (t < 700) ? 0 : (t < 1400 ? 1 : 2);
    int gx = px[spawn];
    int gy = py[spawn];
    drawGolem(gx, gy, phase);

    u8g2.sendBuffer();
    delay(16);
  }
}

// ===================== MIDI PARSER (Core0) =====================
static uint8_t midiRunningStatus = 0;
static uint8_t midiData1 = 0;
static bool    midiHaveData1 = false;

static uint32_t lastClockUs = 0;
static uint32_t lastNoteOnUs = 0;
static float    noteUsSm = 0.0f;

static void midiHandleRealtime(uint8_t b)
{
  static const uint8_t PPQN = 20;

  static uint32_t quarterAccUs = 0;
  static uint8_t  quarterCnt   = 0;
  static float quarterUsAvg = 0.0f;

  if (b == 0xFA || b == 0xFB || b == 0xFC) {
    lastClockUs = 0;
    quarterAccUs = 0;
    quarterCnt = 0;
    quarterUsAvg = 0.0f;
    return;
  }
  if (b != 0xF8) return;

  uint32_t now = micros();

  if (lastClockUs != 0) {
    uint32_t dt = now - lastClockUs;

    if (dt >= 2000 && dt <= 200000) {
      quarterAccUs += dt;
      quarterCnt++;

      if (quarterCnt >= PPQN) {
        float qUs = (float)quarterAccUs;

        if (quarterUsAvg <= 0.0f) quarterUsAvg = qUs;
        else {
          float err = fabsf(qUs - quarterUsAvg) / quarterUsAvg;
          float a = (err < 0.02f) ? 0.08f : 0.25f;
          quarterUsAvg = (1.0f - a) * quarterUsAvg + a * qUs;
        }

        float bpm = 60.0f * 1e6f / quarterUsAvg;
        g_bpm = clampf(bpm, 40.0f, 240.0f);

        quarterAccUs = 0;
        quarterCnt = 0;
      }
    }
  }

  lastClockUs = now;
}

static void midiNoteOn(uint8_t note, uint8_t vel) {
  uint32_t now = micros();
  if (lastNoteOnUs != 0) {
    float dt = (float)(now - lastNoteOnUs);
    if (noteUsSm <= 0.0f) noteUsSm = dt;
    else noteUsSm = 0.90f * noteUsSm + 0.10f * dt;
    g_noteUS = noteUsSm;
  }
  lastNoteOnUs = now;
  evt_push(EVT_NOTE_ON, note, vel);
}
static void midiNoteOff(uint8_t note) {
  evt_push(EVT_NOTE_OFF, note, 0);
}

static void midiHandleCC(uint8_t cc, uint8_t val) {
  float v = (float)val / 127.0f;
  if (cc == 1) g_mod = v;
}

static void midiHandlePitchBend(uint8_t lsb, uint8_t msb) {
  int v14 = ((int)msb << 7) | (int)lsb;
  float x = ((float)v14 - 8192.0f) / 8192.0f;
  g_pb = clampf(x, -1.0f, 1.0f);
}

static void midiHandleChannel(uint8_t st, uint8_t d1, uint8_t d2) {
  uint8_t type = st & 0xF0;
  if (type == 0x90) {
    if (d2 == 0) midiNoteOff(d1);
    else midiNoteOn(d1, d2);
  } else if (type == 0x80) {
    midiNoteOff(d1);
  } else if (type == 0xB0) {
    midiHandleCC(d1, d2);
  } else if (type == 0xE0) {
    midiHandlePitchBend(d1, d2);
  } else if (type == 0xD0) {
    g_at = (float)d1 / 127.0f;
  }
}

static void midiParseByte(uint8_t b) {
  if (b >= 0xF8) { midiHandleRealtime(b); return; }

  if (b & 0x80) {
    midiRunningStatus = b;
    midiHaveData1 = false;
    return;
  }
  if (midiRunningStatus == 0) return;

  if ((midiRunningStatus & 0xF0) == 0xD0) {
    midiHandleChannel(midiRunningStatus, b, 0);
    return;
  }

  if (!midiHaveData1) {
    midiData1 = b;
    midiHaveData1 = true;
  } else {
    uint8_t d1 = midiData1;
    uint8_t d2 = b;
    midiHaveData1 = false;
    midiHandleChannel(midiRunningStatus, d1, d2);
  }
}

// ===================== AUDIO ENGINE (Core1) =====================
struct Voice {
  bool  active=false;
  bool  gate=false;
  uint8_t note=0;
  float freq=0.0f;
  float phase=0.0f; // 0..1
  float env=0.0f;   // 0..1
};
static Voice voices[4];

static inline float midiNoteToHz(uint8_t note) {
  return 440.0f * powf(2.0f, ((int)note - 69) / 12.0f);
}

static int findVoiceByNote(uint8_t note) {
  for (int i=0;i<4;i++) if (voices[i].active && voices[i].note==note) return i;
  return -1;
}
static int allocVoice(uint8_t note) {
  int idx = findVoiceByNote(note);
  if (idx >= 0) return idx;
  for (int i=0;i<4;i++) if (!voices[i].active) return i;
  int best=0; float minEnv=voices[0].env;
  for (int i=1;i<4;i++) if (voices[i].env < minEnv) { minEnv=voices[i].env; best=i; }
  return best;
}
static void audio_noteOn(uint8_t note, uint8_t vel) {
  (void)vel;
  int v = allocVoice(note);
  voices[v].active=true;
  voices[v].gate=true;
  voices[v].note=note;
  voices[v].freq=midiNoteToHz(note);
  voices[v].phase=0.0f;
  voices[v].env=0.0f;
}
static void audio_noteOff(uint8_t note) {
  int v = findVoiceByNote(note);
  if (v>=0) voices[v].gate=false;
}

// ---- Filters / FX ----
struct OnePoleLP {
  float z=0;
  float process(float x, float a){ z += a*(x-z); return z; }
};
static OnePoleLP lpfL, lpfR;

struct Allpass {
  float z=0;
  float process(float x, float g){
    float y = -g*x + z;
    z = x + g*y;
    return y;
  }
};
static Allpass ap[6];
static OnePoleLP phaserSplitLP;
static float phaserLfoPhase = 0.0f;

struct Ladder {
  float s1=0,s2=0,s3=0,s4=0;
  float process(float x, float cutoff, float drive, float fb){
    float xd = x*(1.0f + 6.0f*drive);
    float u  = xd - fb*s4;
    float g = 0.0015f + 0.9985f*cutoff;
    s1 += g*(sat_soft(u )-sat_soft(s1));
    s2 += g*(sat_soft(s1)-sat_soft(s2));
    s3 += g*(sat_soft(s2)-sat_soft(s3));
    s4 += g*(sat_soft(s3)-sat_soft(s4));
    return s4;
  }
};
static Ladder ladderL, ladderR;

// ---- Delay (LIGHT tapped ping-pong) ----
static const float    MAX_DELAY_SEC     = 0.75f;
static const uint32_t MAX_DELAY_SAMPLES = (uint32_t)(SAMPLE_RATE * MAX_DELAY_SEC);
static int16_t *tapDelayBuf = nullptr;
static uint32_t tapW = 0;

static inline float chooseDelaySecondsAuto(float bpm, float noteUS) {
  bpm = clampf(bpm, 40.0f, 240.0f);
  float q = 60.0f / bpm;

  const float divs[] = { 0.75f, 0.5f, 0.333333f, 0.25f, 1.0f };
  float target = q * 0.75f;

  if (noteUS > 0.0f) {
    float sec = noteUS * 1e-6f;
    float best = q * divs[0];
    float bestErr = fabsf(sec - best);
    for (float d : divs) {
      float cand = q * d;
      float err = fabsf(sec - cand);
      if (err < bestErr) { bestErr=err; best=cand; }
    }
    target = best;
  }
  return clampf(target, 0.04f, MAX_DELAY_SEC);
}

static const float ATTACK_MIN_SEC = 0.0016f;
static const float ATTACK_MAX_SEC = 0.2200f;

static const float VOL_MUTE_TH = 0.0020f;

static const float    CLIP_TH = 0.98f;
static const uint32_t CLIP_HOLD_MS = 500;

static const int SCOPE_DECIM = 32;
static int scopeDecim = 0;
static int scopeWrite = 0;

// ===================== CORE1 AUDIO LOOP =====================
void __not_in_flash_func(core1_entry)();

void __not_in_flash_func(core1_entry)() {
  tapDelayBuf = (int16_t*)malloc(MAX_DELAY_SAMPLES * sizeof(int16_t));
  if (tapDelayBuf) memset(tapDelayBuf, 0, MAX_DELAY_SAMPLES * sizeof(int16_t));

  const int BLK = 32;
  float delaySampsCur = 0.75f * 0.25f * (float)SAMPLE_RATE;
  float atSm = 0.0f;

  while (1) {
    // apply wave request (cheap sync)
    g_waveCur = g_waveReq;

    // MIDI event batch
    MidiEvt e;
    for (int k=0;k<16;k++) {
      if (!evt_pop(e)) break;
      if (e.type == EVT_NOTE_ON) audio_noteOn(e.note, e.vel);
      else if (e.type == EVT_NOTE_OFF) audio_noteOff(e.note);
    }

    float attackN   = (float)g_pot[CH_A_ATTACK];
    float toneN     = (float)g_pot[CH_B_TONE];
    float tailN     = (float)g_pot[CH_C_TAIL];
    float volN      = (float)g_pot[CH_D_VOL];
    float phaserMix = (float)g_pot[CH_E_PHASER];
    float ladderMix = (float)g_pot[CH_F_LADDER];
    float delayMix  = (float)g_pot[CH_G_DELAY];
    float octMix    = (float)g_pot[CH_H_OCT];

    delayMix = powf(clampf(delayMix,0,1), 0.80f);
    bool mute = (volN <= VOL_MUTE_TH);

    float attackTime  = lerpf(ATTACK_MAX_SEC, ATTACK_MIN_SEC, attackN);
    float releaseTime = lerpf(0.020f, 1.200f, tailN);
    float aAtk = 1.0f - expf(-DT / attackTime);
    float aRel = 1.0f - expf(-DT / releaseTime);

    float baseLpfA = 0.003f + 0.20f * toneN;

    float pb = (float)g_pb;
    float pbSemi = 2.0f * pb;
    float pbMul = powf(2.0f, pbSemi / 12.0f);

    float at = clampf((float)g_at, 0.0f, 1.0f);
    atSm = 0.90f * atSm + 0.10f * at;

    float mod = clampf((float)g_mod, 0.0f, 1.0f);

    float bpm = clampf((float)g_bpm, 40.0f, 240.0f);
    float noteUS = (float)g_noteUS;
    float delaySec = chooseDelaySecondsAuto(bpm, noteUS);
    float delaySampsTarget = delaySec * (float)SAMPLE_RATE;
    delaySampsCur = 0.98f * delaySampsCur + 0.02f * delaySampsTarget;
    uint32_t dSamps = (uint32_t)clampf(delaySampsCur, 1.0f, (float)(MAX_DELAY_SAMPLES-1));

    float g1 = 0.62f;
    float g2 = 0.38f;
    float g3 = 0.22f;

    float tailSoft = lerpf(1.0f, 0.85f, tailN);
    g1 *= tailSoft; g2 *= tailSoft; g3 *= tailSoft;

    float rateHz = 0.65f + 1.10f * mod;
    float lfoInc = rateHz * DT;
    float hpLP_a = 0.0025f;

    float ladderCut   = clampf(toneN, 0.02f, 0.98f);
    float ladderDrive = 0.35f + 0.65f * atSm;
    float ladderFb    = 0.35f;

    for (int n=0; n<BLK; n++) {
      float dry = 0.0f;
      int activeCount = 0;

      if (!mute) {
        phaserLfoPhase += lfoInc;
        if (phaserLfoPhase >= 1.0f) phaserLfoPhase -= 1.0f; // ★重要: -= 1.0f
        float vib = sinf(2.0f * PI * phaserLfoPhase) * (0.0030f * mod);
        driftTick();

        uint8_t wave = (uint8_t)g_waveCur & 3;

        for (int i=0;i<4;i++) {
          if (!voices[i].active) continue;

          float ph = voices[i].phase;
          float y;

          switch (wave) {
            case 0: y = sinf(2.0f * PI * ph); break;            // SINE
            case 1: y = 2.0f * ph - 1.0f; break;                // SAW
            case 2: y = (ph < 0.5f) ? 1.0f : -1.0f; break;      // SQUARE
           default: y = (ph < 0.5f) ? (4.0f*ph-1.0f) : (3.0f-4.0f*ph); break; // TRI
          }

          static const float WAVE_GAIN[4] = {
            1.9f,  // SINE
            0.6f,  // SAW
            0.55f,  // SQUARE
            1.6f   // TRI
          };

          y *= WAVE_GAIN[wave];

          float f = voices[i].freq * pbMul * (1.0f + vib) * driftMul;

          voices[i].phase += f * DT;
          if (voices[i].phase >= 1.0f) voices[i].phase -= 1.0f;

          if (voices[i].gate) {
            voices[i].env += aAtk * (1.0f - voices[i].env);
          } else {
            voices[i].env += aRel * (0.0f - voices[i].env);
            if (voices[i].env < 0.0005f) {
              voices[i].env = 0.0f;
              voices[i].active = false;
            }
          }

          dry += y * voices[i].env;
          activeCount++;
        }
      }

      g_voiceCount = (uint8_t)activeCount;

      // ---- poly normalize ----
      // 4voiceのときに必ず飽和しないようにしつつ、1voice時の音圧も確保
      float polyGain = (activeCount <= 0) ? 0.0f : (1.0f / sqrtf((float)activeCount));
      // 係数は好み。まずこれで「でかいのに割れない」を作る
      dry *= 0.62f * polyGain;

      dry *= lerpf(0.0f, 1.2f, volN);

      float L = dry, R = dry;

      L = lpfL.process(L, baseLpfA);
      R = lpfR.process(R, baseLpfA);

      struct DCBlock {
      float x1=0, y1=0;
      float process(float x){
      // 0.995〜0.999の範囲でOK。0.997が無難
      const float R = 0.997f;
      float y = x - x1 + R * y1;
      x1 = x; y1 = y;
      return y;
       }
      };
static DCBlock dcL, dcR;

      if (!mute && octMix > 0.001f) {
        float x = 0.5f * (L + R);
        static float octLP = 0.0f;
        float y2 = x * x;
        float y  = (x >= 0.0f) ? y2 : -y2;
        octLP += 0.01f * (y - octLP);
        float oct = y - octLP;

        float sparkle = 0.0f;
        if (octMix > 0.5f) {
          float t = (octMix - 0.5f) * 2.0f;
          sparkle = 0.8f * t * (x * x * x);
        }

        float octGain = 3.8f;
        float octSig  = sat_soft(octGain * oct + sparkle);

        float dryKeep = 1.0f - 0.45f * octMix;
        L = dryKeep * L + octMix * octSig;
        R = dryKeep * R + octMix * octSig;
      }

      if (!mute && phaserMix > 0.001f) {
        float x = 0.5f * (L + R);

        float lp = phaserSplitLP.process(x, hpLP_a);
        float hp = x - lp;

        float lfo = sinf(2.0f * PI * (phaserLfoPhase * 0.53f + 0.12f));
        float g = 0.10f + 0.78f * (0.5f * (lfo + 1.0f));

        float p = hp;
        p = ap[0].process(p, g);
        p = ap[1].process(p, g);
        p = ap[2].process(p, g);
        p = ap[3].process(p, g);
        p = ap[4].process(p, g);
        p = ap[5].process(p, g);

        static float phFb = 0.0f;
        phFb = 0.86f * phFb + 0.14f * p;
        float fbAmt = 0.25f + 0.70f * phaserMix;
        p += fbAmt * phFb;

        float y = lp + p;
        L = lerpf(L, y, phaserMix);
        R = lerpf(R, y, phaserMix);
      }

      if (!mute && ladderMix > 0.001f) {
        float yL = ladderL.process(L, ladderCut, ladderDrive, ladderFb) * 1.25f;
        float yR = ladderR.process(R, ladderCut, ladderDrive, ladderFb) * 1.25f;
        L = lerpf(L, yL, ladderMix);
        R = lerpf(R, yR, ladderMix);
      }

      if (!mute && tapDelayBuf && delayMix > 0.001f) {
        float monoIn = 0.5f * (L + R);

        tapDelayBuf[tapW] = f2s16(monoIn);

        uint32_t w = tapW;
        uint32_t r1 = (w + MAX_DELAY_SAMPLES - dSamps) % MAX_DELAY_SAMPLES;
        uint32_t r2 = (w + MAX_DELAY_SAMPLES - (2 * dSamps)) % MAX_DELAY_SAMPLES;
        uint32_t r3 = (w + MAX_DELAY_SAMPLES - (3 * dSamps)) % MAX_DELAY_SAMPLES;

        float e1 = s162f(tapDelayBuf[r1]) * g1;
        float e2 = s162f(tapDelayBuf[r2]) * g2;
        float e3 = s162f(tapDelayBuf[r3]) * g3;

        float wetL = e1 + 0.35f * e2 + e3;
        float wetR = 0.35f * e1 + e2 + 0.35f * e3;

        float wetGain = lerpf(0.0f, 0.95f, delayMix);

        L = L + wetGain * wetL;
        R = R + wetGain * wetR;

        tapW++;
        if (tapW >= MAX_DELAY_SAMPLES) tapW = 0;
      } else if (tapDelayBuf) {
        tapDelayBuf[tapW] = 0;
        tapW++;
        if (tapW >= MAX_DELAY_SAMPLES) tapW = 0;
      }

      if (mute) { L = 0.0f; R = 0.0f; }

      // ---- soft clip limiter ----
      L = sat_soft(L);
      R = sat_soft(R);

      L = clampf(L, -0.84f, 0.84f);
      R = clampf(R, -0.84f, 0.84f);

      if (fabsf(L) > CLIP_TH || fabsf(R) > CLIP_TH) {
      g_clipHoldUntilMs = millis() + CLIP_HOLD_MS;
      }

      if (++scopeDecim >= SCOPE_DECIM) {
        scopeDecim = 0;
        int16_t s = f2s16(0.5f*(L+R));
        g_scopeBuf[scopeWrite] = s;
        scopeWrite++;
        if (scopeWrite >= SCOPE_W) {
          scopeWrite = 0;
          g_scopeSeq++;
        }
      }

      i2s.write(f2s16(L));
      i2s.write(f2s16(R));
    }
  }
}

// ===================== WAVE SWITCH (Core0) =====================
// オルタネイトSW: 押すたびに HIGH/LOW が反転する。
// → 「安定した状態変化」を検出したら 1回だけ wave++
// （LOWのみ/エッジのみだと2回押しになるのでNG）
static void pollWaveSwitch(uint32_t nowMs)
{
  static uint8_t rawLast = HIGH;
  static uint8_t stable  = HIGH;
  static uint32_t tLastChange = 0;

  const uint32_t DEBOUNCE_MS = 25;

  uint8_t raw = (uint8_t)digitalRead(PIN_WAVE_BTN);

  if (raw != rawLast) {
    rawLast = raw;
    tLastChange = nowMs;
  }

  // 一定時間変わらなければ「安定」とみなす
  if ((uint32_t)(nowMs - tLastChange) >= DEBOUNCE_MS) {
    if (stable != raw) {
      stable = raw;

      // ★ここが肝：HIGH->LOW でも LOW->HIGH でも 1回だけ進める
      g_waveReq = (uint8_t)((g_waveReq + 1) & 3);
      g_waveOverlayUntilMs = nowMs + 700; // 700ms overlay
    }
  }
}

// overlay: 中央 左に大きい波形アイコン、右に波形名
static void drawWaveOverlay(uint32_t nowMs)
{
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  uint8_t wv = (uint8_t)g_waveReq;

  // 左：大アイコン
  // だいたい中央(縦)に合わせる
  int iconX = 18;
  int iconY = 18;
  drawWaveIcon(iconX, iconY, wv, 3);

  // 右：波形名（見た目バランス取り）
  const char* nm = WAVE_NAME[wv & 3];
  u8g2.setFont(u8g2_font_helvB18_tf);
  int tw = u8g2.getStrWidth(nm);
  int textX = 128 - tw - 10;
  int textY = 42;
  if (textX < 60) textX = 60;
  u8g2.drawStr(textX, textY, nm);

  // 下に小アイコンも（おまけで分かりやすい）
  drawWaveIcon(2, 54, wv, 1);

  u8g2.sendBuffer();
}

// ===================== SETUP / LOOP (Core0) =====================
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(PIN_DAC_MUTE, OUTPUT);
  digitalWrite(PIN_DAC_MUTE, HIGH);

  pinMode(PIN_MUX_S0, OUTPUT);
  pinMode(PIN_MUX_S1, OUTPUT);
  pinMode(PIN_MUX_S2, OUTPUT);

  // Wave switch
  pinMode(PIN_WAVE_BTN, INPUT_PULLUP);

  analogReadResolution(12);
  driftSeed((uint32_t)micros() ^ (uint32_t)analogRead(PIN_MUX_COM));
  randomSeed((uint32_t)micros() ^ (uint32_t)analogRead(PIN_MUX_COM));

  Serial.begin(115200);

  // MIDI
  Serial1.setRX(PIN_MIDI_RX);
  Serial1.begin(31250);

  // I2S
  i2s.setBCLK(PIN_I2S_BCLK);
  i2s.setDATA(PIN_I2S_DATA);
  i2s.begin(SAMPLE_RATE);

  // OLED (Wire1)
  Wire1.setSDA(PIN_OLED_SDA);
  Wire1.setSCL(PIN_OLED_SCL);
  Wire1.begin();
  Wire1.setClock(400000);

  u8g2.setBusClock(400000);
  u8g2.begin();
  u8g2.setContrast(200);

  bootAnimation();

  for (int i=0;i<CH_COUNT;i++) potPrevHUD[i]=0.0f;

  multicore_launch_core1(core1_entry);
}

void loop() {
  static uint32_t nextOledMs = 0;

  while (Serial1.available() > 0) {
    midiParseByte((uint8_t)Serial1.read());
  }

  uint32_t nowMs = millis();

  // wave switch polling (debounced)
  pollWaveSwitch(nowMs);

  // pots & HUD
  static uint32_t lastCtrlMs = 0;
  if ((nowMs - lastCtrlMs) >= 10) {
    lastCtrlMs = nowMs;
    readAllPotsSmoothed();

    float potNow[CH_COUNT];
    for (int i=0;i<CH_COUNT;i++) potNow[i] = (float)g_pot[i];
    hudTouchDetect(potNow);
  }

  // OLED update (fixed rate scheduler)
  if ((int32_t)(nowMs - nextOledMs) >= 0) {
    nextOledMs += OLED_PERIOD_MS;

    // overlay?
    if ((int32_t)((uint32_t)g_waveOverlayUntilMs - nowMs) > 0) {
      drawWaveOverlay(nowMs);
      return;
    }

    static int16_t  bufLocal[SCOPE_W];
    uint16_t seq1 = g_scopeSeq;
    for (int i=0;i<SCOPE_W;i++) bufLocal[i] = (int16_t)g_scopeBuf[i];
    uint16_t seq2 = g_scopeSeq;
    if (seq1 != seq2) {
      for (int i=0;i<SCOPE_W;i++) bufLocal[i] = (int16_t)g_scopeBuf[i];
    }

    bool hudActive = (hudIndex >= 0) && ((int32_t)(hudUntilMs - nowMs) > 0);

    u8g2.clearBuffer();
    float vol01 = (float)g_pot[CH_D_VOL];

    if (hudActive) {
      int pct = (int)(clampf((float)g_pot[hudIndex], 0.0f, 1.0f) * 100.0f + 0.5f);
      drawHUD(hudIndex, pct);
    } else {
      drawScope(bufLocal, vol01);
    }

    u8g2.sendBuffer();
  }
}
