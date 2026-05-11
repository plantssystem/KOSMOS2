/* KOSMOS2 + PRA32-U2/M (All-in-One:  Waveshare Pico-Audio version) */
#include <Arduino.h>

// Optional: give Core1 8KB stack if needed
bool core1_separate_stack = false;

// ----------------------- PRA32-U2/M on Core1 (I2S + Waveshare Pico-Audio) ----------------------
#include <I2S.h>

I2S g_i2s_output(OUTPUT);

#define PRA32_U2_VERSION "v2.12.0"
#define PRA32_U2_MIDI_CH (0)

// I2S / DAC ピン定義（Waveshare Pico-Audio）
#define PRA32_U2_I2S_DAC_MUTE_OFF_PIN (22)

// Waveshare Pico-Audio:
// DIN  = GP26
// BCK  = GP27
// LRCK = GP28
#define PRA32_U2_I2S_DATA_PIN  (26)  // DIN
#define PRA32_U2_I2S_BCLK_PIN  (27)  // BCK
#define PRA32_U2_I2S_LRCLK_PIN (28)  // LRCK

// PRA32-U のバッファ設定

#define PRA32_U2_I2S_BUFFERS      (4)
#define PRA32_U2_I2S_BUFFER_WORDS (64)

#define PRA32_U2_NUMBER_OF_SYNTHS (4)

// 内部 MIDI ブリッジ（すでにあなたのコードにあるやつ）
enum MidiEvType : uint8_t { EV_NOTE_ON=0, EV_NOTE_OFF=1, EV_CC=2 };
struct MidiEvent { uint8_t type, d1, d2, ch; };
namespace MidiQ {
  constexpr size_t QSIZE=256; static volatile uint32_t head=0, tail=0; static MidiEvent q[QSIZE];
  inline bool push(const MidiEvent& ev){uint32_t h=head,n=(h+1)%QSIZE; if(n==tail) return false; q[h]=ev; head=n; return true;}
  inline bool pop(MidiEvent& out){uint32_t t=tail; if(t==head) return false; out=q[t]; tail=(t+1)%QSIZE; return true;}
}

inline void synth_note_on_core1(uint8_t note,uint8_t vel,uint8_t ch=0){
    MidiEvent ev{EV_NOTE_ON,note,vel,ch};

    // ★ 最大 100 回だけリトライ（数十マイクロ秒程度）
    for (int i = 0; i < 100; i++) {
        if (MidiQ::push(ev)) return;
        tight_loop_contents();
    }
    // ここまで来たら諦めて捨てる（アルペジオの時間軸を優先）
}

inline void synth_note_off_core1(uint8_t note,uint8_t ch=0){
    MidiEvent ev{EV_NOTE_OFF,note,0,ch};
    for (int i = 0; i < 100; i++) {
        if (MidiQ::push(ev)) return;
        tight_loop_contents();
    }
}

inline void midi_bridge_send_cc(uint8_t cc,uint8_t val,uint8_t ch=0){
    MidiEvent ev{EV_CC,cc,val,ch};
    for (int i = 0; i < 50; i++) {   // CC は優先度低めでリトライ回数も少なく
        if (MidiQ::push(ev)) return;
        tight_loop_contents();
    }
}

// PRA32-U synth 用のグローバル
uint8_t g_midi_ch = PRA32_U2_MIDI_CH;
#include "pra32-u2-common.h"
#include "pra32-u2-synth.h"

// ---- A/B/C/D の 4 パート独立シンセ ----
// A = ch0
// B = ch1
// C = ch2
// D = ch3

PRA32_U2_Synth<true, false, true, 0> g_synth;
PRA32_U2_Synth<true, false, true, 1> g_sub_synth;
PRA32_U2_Synth<true, false, true, 2> g_chord;
PRA32_U2_Synth<true, false, true, 3> g_seq;

float masterVolume = 1.0f;

void initSynths() {
    g_synth.initialize();
    g_sub_synth.initialize();
    g_chord.initialize();
    g_seq.initialize();
}

// ---- Core1 MIDI Receiver ----
void processMidiOnCore1() {
    MidiEvent ev;

    while (MidiQ::pop(ev)) {

        uint8_t ch = ev.ch;

        // ---- Note On ----
        if (ev.type == EV_NOTE_ON) {
            if (ch == 0) g_synth.note_on(ev.d1, ev.d2);
            else if (ch == 1) g_sub_synth.note_on(ev.d1, ev.d2);
            else if (ch == 2) g_chord.note_on(ev.d1, ev.d2);
            else if (ch == 3) g_seq.note_on(ev.d1, ev.d2);
        }

        // ---- Note Off ----
        else if (ev.type == EV_NOTE_OFF) {
            if (ch == 0) g_synth.note_off(ev.d1);
            else if (ch == 1) g_sub_synth.note_off(ev.d1);
            else if (ch == 2) g_chord.note_off(ev.d1);
            else if (ch == 3) g_seq.note_off(ev.d1);
        }

        // ---- CC ----
        else if (ev.type == EV_CC) {

            // CC100 = Program Change
            if (ev.d1 == 100) {
                uint8_t prog = ev.d2;

                if (ch == 0) g_synth.program_change(prog);
                else if (ch == 1) g_sub_synth.program_change(prog);
                else if (ch == 2) g_chord.program_change(prog);
                else if (ch == 3) g_seq.program_change(prog);
            }

            // CC7 = Volume
            else if (ev.d1 == 7) {
                masterVolume = (float)ev.d2 / 127.0f;
            }
        }
    }
}

#include "pico/multicore.h"

// ---- 音色番号----
int programA = 1;      // ch1 初期音色
int programB = 6;      // ch2 初期音色
int programC = 14;     // ch3 初期音色
int programD = 7;      // ch4 初期音色

// ------------------------------------------------------
// Core1: メイン処理（I2S + シンセ + MIDI受信）
// ------------------------------------------------------
void __not_in_flash_func(core1_main)() {

    // ---- I2S 初期化 ----
    //g_i2s_output.setSysClk(SAMPLING_RATE);
    g_i2s_output.setFrequency(SAMPLING_RATE);

    g_i2s_output.setDATA(PRA32_U2_I2S_DATA_PIN);
    g_i2s_output.setBCLK(PRA32_U2_I2S_BCLK_PIN);

    g_i2s_output.setBitsPerSample(16);
    g_i2s_output.setBuffers(PRA32_U2_I2S_BUFFERS, PRA32_U2_I2S_BUFFER_WORDS);
    g_i2s_output.begin();

    // ---- DAC ミュート解除 ----
    pinMode(PRA32_U2_I2S_DAC_MUTE_OFF_PIN, OUTPUT);
    digitalWrite(PRA32_U2_I2S_DAC_MUTE_OFF_PIN, HIGH);

    // ---- シンセ初期化 ----
    initSynths();

    // ★ 起動直後の内部状態リセット
    resetAllParts();

    // ---- シンセ内部のウォームアップ ----
    for (int i = 0; i < 400; i++) {   // 400サンプル ≒ 9ms
        int16_t dummyR;
        int32_t dummyL32, dummyR32;

        g_synth.process<false, false>(0, 0, dummyR, dummyL32, dummyR32);
        g_sub_synth.process<false, false>(0, 0, dummyR, dummyL32, dummyR32);
        g_chord.process<false, false>(0, 0, dummyR, dummyL32, dummyR32);
        g_seq.process<false, false>(0, 0, dummyR, dummyL32, dummyR32);
    }

    // ---- バッファ ----
    int16_t left_buffer[PRA32_U2_I2S_BUFFER_WORDS];
    int16_t right_buffer[PRA32_U2_I2S_BUFFER_WORDS];

    // ---- パートごとの存在感ゲイン ----
    /*
    const float gainA = 0.6f;
    const float gainB = 0.8f;
    const float gainC = 0.9f;
    const float gainD = 0.5f;
    */
    const float gainA = 0.8f;
    const float gainB = 0.8f;
    const float gainC = 0.6f;
    const float gainD = 0.5f;

    while (true) {

        // ---- MIDI 受信 ----
        processMidiOnCore1();

        // ---- 4 パート合成 ----
        for (uint32_t i = 0; i < PRA32_U2_I2S_BUFFER_WORDS; i++) {

            // A
            int16_t aL, aR;
            int32_t aL32, aR32;
            aL = g_synth.process<false, true>(0, 0, aR, aL32, aR32);

            // B
            int16_t bL, bR;
            int32_t bL32, bR32;
            bL = g_sub_synth.process<false, true>(0, 0, bR, bL32, bR32);

            // C
            int16_t cL, cR;
            int32_t cL32, cR32;
            cL = g_chord.process<false, true>(0, 0, cR, cL32, cR32);

            // D
            int16_t dL, dR;
            int32_t dL32, dR32;
            dL = g_seq.process<false, true>(0, 0, dR, dL32, dR32);

            // ---- 16bit ベースでミックス ----
            float mixL =
                aL * gainA +
                bL * gainB +
                cL * gainC +
                dL * gainD;

            float mixR =
                aR * gainA +
                bR * gainB +
                cR * gainC +
                dR * gainD;

            // クリップ
            mixL = constrain(mixL, -30000.0f, 30000.0f);
            mixR = constrain(mixR, -30000.0f, 30000.0f);

            left_buffer[i]  = (int16_t)(mixL * 0.35f * masterVolume);
            right_buffer[i] = (int16_t)(mixR * 0.35f * masterVolume);
        }

        // ---- I2S 出力 ----
        for (uint32_t i = 0; i < PRA32_U2_I2S_BUFFER_WORDS; i++) {
            g_i2s_output.write16(left_buffer[i], right_buffer[i]);
        }
    }
}

// ----------------------- Core0: KOSMOS2 (patched) -----------------------
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_TinyUSB.h>

Adafruit_USBD_MIDI usb_midi;

// ==== RGB565 カラー定義 ====
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_DARK_GRAY 0x2104
#define COLOR_GRAY 0x8410   // 中間グレー（RGB565）
#define COLOR_ORANGE 0xFD20   // 明るいオレンジ

// ==== Waveshare Pico-LCD-1.3 ピン定義 ====
#define LCD_DC   8
#define LCD_CS   9
#define LCD_RST 12
#define LCD_BL  13
#define LCD_SCK 10
#define LCD_MOSI 11
#define LCD_SPI SPI1

// ---- A/B/X/Y ----
#define KEY_A_PIN    15
#define KEY_B_PIN    17
#define KEY_X_PIN    19
#define KEY_Y_PIN    21

// ---- 十字キー（固定）----
#define LEFT_PIN   14
#define DOWN_PIN   16
#define UP_PIN     18
#define RIGHT_PIN  20

// ---- Joystick ----
#define JOY_X_PIN 6
#define JOY_Y_PIN 7
#define JOY_SW_PIN 0   // ★ ここを 8 から変更（最重要）

// 5x7 ASCII フォント (32〜127)
const uint8_t font5x7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
  {0x00,0x00,0x5F,0x00,0x00}, // 33 '!'
  {0x00,0x07,0x00,0x07,0x00}, // 34 '"'
  {0x14,0x7F,0x14,0x7F,0x14}, // 35 '#'
  {0x24,0x2A,0x7F,0x2A,0x12}, // 36 '$'
  {0x23,0x13,0x08,0x64,0x62}, // 37 '%'
  {0x36,0x49,0x55,0x22,0x50}, // 38 '&'
  {0x00,0x05,0x03,0x00,0x00}, // 39 '''
  {0x00,0x1C,0x22,0x41,0x00}, // 40 '('
  {0x00,0x41,0x22,0x1C,0x00}, // 41 ')'
  {0x14,0x08,0x3E,0x08,0x14}, // 42 '*'
  {0x08,0x08,0x3E,0x08,0x08}, // 43 '+'
  {0x00,0x50,0x30,0x00,0x00}, // 44 ','
  {0x08,0x08,0x08,0x08,0x08}, // 45 '-'
  {0x00,0x60,0x60,0x00,0x00}, // 46 '.'
  {0x20,0x10,0x08,0x04,0x02}, // 47 '/'
  {0x3E,0x51,0x49,0x45,0x3E}, // 48 '0'
  {0x00,0x42,0x7F,0x40,0x00}, // 49 '1'
  {0x42,0x61,0x51,0x49,0x46}, // 50 '2'
  {0x21,0x41,0x45,0x4B,0x31}, // 51 '3'
  {0x18,0x14,0x12,0x7F,0x10}, // 52 '4'
  {0x27,0x45,0x45,0x45,0x39}, // 53 '5'
  {0x3C,0x4A,0x49,0x49,0x30}, // 54 '6'
  {0x01,0x71,0x09,0x05,0x03}, // 55 '7'
  {0x36,0x49,0x49,0x49,0x36}, // 56 '8'
  {0x06,0x49,0x49,0x29,0x1E}, // 57 '9'
  {0x00,0x36,0x36,0x00,0x00}, // 58 ':'
  {0x00,0x56,0x36,0x00,0x00}, // 59 ';'
  {0x08,0x14,0x22,0x41,0x00}, // 60 '<'
  {0x14,0x14,0x14,0x14,0x14}, // 61 '='
  {0x00,0x41,0x22,0x14,0x08}, // 62 '>'
  {0x02,0x01,0x51,0x09,0x06}, // 63 '?'
  {0x32,0x49,0x79,0x41,0x3E}, // 64 '@'
  {0x7E,0x11,0x11,0x11,0x7E}, // 65 'A'
  {0x7F,0x49,0x49,0x49,0x36}, // 66 'B'
  {0x3E,0x41,0x41,0x41,0x22}, // 67 'C'
  {0x7F,0x41,0x41,0x22,0x1C}, // 68 'D'
  {0x7F,0x49,0x49,0x49,0x41}, // 69 'E'
  {0x7F,0x09,0x09,0x09,0x01}, // 70 'F'
  {0x3E,0x41,0x49,0x49,0x7A}, // 71 'G'
  {0x7F,0x08,0x08,0x08,0x7F}, // 72 'H'
  {0x00,0x41,0x7F,0x41,0x00}, // 73 'I'
  {0x20,0x40,0x41,0x3F,0x01}, // 74 'J'
  {0x7F,0x08,0x14,0x22,0x41}, // 75 'K'
  {0x7F,0x40,0x40,0x40,0x40}, // 76 'L'
  {0x7F,0x02,0x0C,0x02,0x7F}, // 77 'M'
  {0x7F,0x04,0x08,0x10,0x7F}, // 78 'N'
  {0x3E,0x41,0x41,0x41,0x3E}, // 79 'O'
  {0x7F,0x09,0x09,0x09,0x06}, // 80 'P'
  {0x3E,0x41,0x51,0x21,0x5E}, // 81 'Q'
  {0x7F,0x09,0x19,0x29,0x46}, // 82 'R'
  {0x46,0x49,0x49,0x49,0x31}, // 83 'S'
  {0x01,0x01,0x7F,0x01,0x01}, // 84 'T'
  {0x3F,0x40,0x40,0x40,0x3F}, // 85 'U'
  {0x1F,0x20,0x40,0x20,0x1F}, // 86 'V'
  {0x7F,0x20,0x18,0x20,0x7F}, // 87 'W'
  {0x63,0x14,0x08,0x14,0x63}, // 88 'X'
  {0x07,0x08,0x70,0x08,0x07}, // 89 'Y'
  {0x61,0x51,0x49,0x45,0x43}, // 90 'Z'
  {0x00,0x7F,0x41,0x41,0x00}, // 91 '['
  {0x02,0x04,0x08,0x10,0x20}, // 92 '\'
  {0x00,0x41,0x41,0x7F,0x00}, // 93 ']'
  {0x04,0x02,0x01,0x02,0x04}, // 94 '^'
  {0x40,0x40,0x40,0x40,0x40}, // 95 '_'
  {0x00,0x01,0x02,0x04,0x00}, // 96 '`'
  {0x20,0x54,0x54,0x54,0x78}, // 97 'a'
  {0x7F,0x48,0x44,0x44,0x38}, // 98 'b'
  {0x38,0x44,0x44,0x44,0x20}, // 99 'c'
  {0x38,0x44,0x44,0x48,0x7F}, // 100 'd'
  {0x38,0x54,0x54,0x54,0x18}, // 101 'e'
  {0x08,0x7E,0x09,0x01,0x02}, // 102 'f'
  {0x0C,0x52,0x52,0x52,0x3E}, // 103 'g'
  {0x7F,0x08,0x04,0x04,0x78}, // 104 'h'
  {0x00,0x44,0x7D,0x40,0x00}, // 105 'i'
  {0x20,0x40,0x44,0x3D,0x00}, // 106 'j'
  {0x7F,0x10,0x28,0x44,0x00}, // 107 'k'
  {0x00,0x41,0x7F,0x40,0x00}, // 108 'l'
  {0x7C,0x04,0x18,0x04,0x78}, // 109 'm'
  {0x7C,0x08,0x04,0x04,0x78}, // 110 'n'
  {0x38,0x44,0x44,0x44,0x38}, // 111 'o'
  {0x7C,0x14,0x14,0x14,0x08}, // 112 'p'
  {0x08,0x14,0x14,0x14,0x7C}, // 113 'q'
  {0x7C,0x08,0x04,0x04,0x08}, // 114 'r'
  {0x48,0x54,0x54,0x54,0x20}, // 115 's'
  {0x04,0x3F,0x44,0x40,0x20}, // 116 't'
  {0x3C,0x40,0x40,0x20,0x7C}, // 117 'u'
  {0x1C,0x20,0x40,0x20,0x1C}, // 118 'v'
  {0x3C,0x40,0x30,0x40,0x3C}, // 119 'w'
  {0x44,0x28,0x10,0x28,0x44}, // 120 'x'
  {0x0C,0x50,0x50,0x50,0x3C}, // 121 'y'
  {0x44,0x64,0x54,0x4C,0x44}, // 122 'z'
  {0x00,0x08,0x36,0x41,0x00}, // 123 '{'
  {0x00,0x00,0x7F,0x00,0x00}, // 124 '|'
  {0x00,0x41,0x36,0x08,0x00}, // 125 '}'
  {0x08,0x04,0x08,0x10,0x08}, // 126 '~'
};

// ============================================================
// ★ BPM テーブル（X ボタン用）
// ============================================================
const int BPM_TABLE[] = {20, 80, 120, 140, 240};
const int BPM_COUNT = 5;

// ---- テンポ（内部 BPM のみ）----
int baseBPM  = 120;   // 基本 BPM
int stepBPM  = 120;   // ステップ進行用 BPM

int bpmIndex = 2; // 初期値 120BPM（TABLE[2])

int transpose = 3;   // -24〜+24 くらいまで対応（2オクターブ）

const int TRANSPOSE_LIST[] = { -10, -5, -4, 0, +4, +5, +10 };
const int TRANSPOSE_COUNT = 7;

// 8分 × 16 のリズムパターン
int rhythmPatterns[6][16] = {
    // パターン0：交互（基礎）
    {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},

    // パターン1：全打ち（全打ち）
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},

    // パターン2：余白多め（呼吸）
    {1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0},

    // パターン3：連打入り（勢い）
    {1,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},

    // パターン4：後半寄り（タメ・尺八的）
    {0,0,1,0,0,1,0,1,1,0,1,1,0,1,0,1},

    // パターン5：3連（伸び）
    {1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0}
};

bool mainNoteExtended = false;

int currentPattern = 0;   // 現在のパターン
int mainDensity = 100;    // 発音率（0〜100%）

bool btnA = false;
bool btnB = false;
bool btnX = false;
bool btnY = false;

bool btnLeft = false;
bool btnRight = false;
bool btnUp = false;
bool btnDown = false;

bool btnSW = false;

// ★ 起動時アルペジオ再生済みフラグ
bool startupArpDone = false;

unsigned long autoModeTimer = 0;

unsigned long noteOffTime = 0;   // ノートを止める時刻
int noteLengthMin = 50;          // 最短音長（ミリ秒）
int noteLengthMax = 400;         // 最長音長（ミリ秒）

int16_t noteDots[240];     // x=0〜239 にノートの高さを保存
int16_t noteDotsY[240];
uint8_t noteDotsNote[240];

bool noteIsOnB = false;
uint8_t lastNoteB = 0;
unsigned long noteOffTimeB = 0;

bool noteIsOnMain = false;
uint8_t lastNoteMain = 0;
unsigned long noteOffTimeMain = 0;

// ★ 8分ステップ管理
unsigned long lastStep = 0;
// ★ 8分の長さ（BPM から計算）
unsigned long interval = 0;

unsigned long nextMainSilenceTime = 0;
unsigned long mainSilenceDuration = 0;
bool mainSilenceActive = false;

unsigned long lastMainStepTime = 0;
unsigned long nextSilenceTime = 0;
int mainPattern[16];   // 0 = 休符, 1 = 鳴く
bool pendingPatternChange = false;

uint16_t bpmColor = COLOR_GREEN;   // Start時は白、Stop時は赤

unsigned long lastClockMicros = 0;

// ---- スケール定義（度数） ----
const uint8_t SCALE_HEI[]   = { 0, 2, 4, 7, 9 };   // 平調子（ヨナ抜き長音階）
const uint8_t SCALE_MIYA[]  = { 0, 1, 5, 7, 10 };   // 都節（C/Db/F/G/Bb）
const uint8_t SCALE_INSEN[] = { 0, 1, 5, 7, 8 };    // 陰旋法（C/Db/F/G/Ab）
const uint8_t SCALE_PENTA[] = { 0, 3, 5, 7, 10 };   // マイナーペンタトニック

const int SCALE_HEI_SIZE   = 5;
const int SCALE_MIYA_SIZE  = 5;
const int SCALE_INSEN_SIZE = 5;
const int SCALE_PENTA_SIZE = 5;

int scaleMode = 0;  
// 0 = 平調子
// 1 = 都節
// 2 = 陰旋法
// 3 = ペンタトニック

// ★ TouchOSC からのスケール変更を管理するためのフラグ
int pendingScale = -1;

// ---- スケールごとの mainPattern 密度（鳴く確率 %） ----
int mainPatternDensity[3] = {
    80,
    60,
    70
};

// ---- スケールごとのアルペジオ速度（ミリ秒） ----
// 小さいほど速い
int arpSpeedTable[3] = {
    20,   // 平調子 → 明るく速い
    105,  // 都節   → 哀愁、少しゆっくり
    90    // 陰旋法 → 渋い、やや速め
};

// ---- スケールごとのアルペジオ幅（degree の増減幅） ----
int arpWidthTable[3] = {
    2,   // 平調子 → 跳ねる
    1,   // 都節   → 哀愁、狭い動き
    3    // 陰旋法 → 渋い、広い跳躍
};

// ---- スケールごとのアルペジオ方向 ----
//  1 = 上昇, -1 = 下降, 0 = ランダム
int arpDirTable[3] = {
     1   // 平調子 → 上昇
    -1,  // 都節   → 下降
    0    // 陰旋法 → ランダム
};

// ---- スケールごとのアルペジオ持続時間（方向反転回数） ----
int arpLengthTable[3] = {
    8,  // 平調子 → 長めに鳴く（明るく広がる）
    4,   // 都節   → 短め（哀愁、余韻を残す）
    30   // 陰旋法 → 長く粘る（尺八的）
};

// ---- スケールごとのアルペジオ間隔時間（ミリ秒） ----
// 小さいほど速い、値が大きいほどゆっくり
int arpTimeTable[3] = {
    40,   // 平調子（HEI） → 明るく速い
    70,   // 都節（MIYA） → 哀愁、ゆっくり
    50    // 陰旋法（INSEN） → 渋い、中速
};

// =====================================================
// ★ B パート専用リズムパターン（8分 × 16）
// =====================================================
int rhythmBPatterns[3][16] = {
    // パターン1：4分の地鳴り（ドン……ドン……）
    {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
    // パターン2：8分の鳴り
    {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
    // パターン3：3連
    {1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0},
};

int currentBPattern = 0;

int noteToY(uint8_t note) {
    note = constrain(note, 36, 84);
    return map(note, 36, 84, 240, 150);
}

int muteState = 0;  
// 0 = A mute
// 1 = B mute
// 2 = ALL mute
// 3 = A unmute
// 4 = B unmute
bool muteA = false;
bool muteB = false;
bool lastMuteA = false;
bool lastMuteB = false;
bool lastMuteC = false;
bool lastMuteD = false;

// ---- ボタン状態管理 ----
bool lastAState = false;
bool lastBState = false;

unsigned long pressStartA = 0;
unsigned long pressStartB = 0;

unsigned long lastStepA = 0;   // ← 外に出す（重要）
unsigned long lastStepB = 0;   // ← 外に出す（重要）

// ---- X ボタン状態管理 ----
bool lastX = false;
unsigned long pressStartX = 0;
unsigned long lastStepX = 0;

bool isPlaying = false;

// ---- グローバルに置く----
int degreeA = 0;   // スケール内の現在位置
int dirA    = 1;   // 上昇(+1) / 下降(-1)
int degreeArp = 0;
int dirArp    = 1;

int degreeB = 0;
int dirB = 1;
int degreeOutB = 0;

uint8_t lastChordNote = 0;
bool chordIsOn = false;
unsigned long chordOffTime = 0;
int degreeC = -1;
bool muteC = false;   // TouchOSC でミュートしたい場合

int degreeD = 3;  
bool muteD = false;
uint8_t lastDNote = 0;
int dDensity = 100;  
bool dGoingUp = true;
int dPhraseRemain = 0;   // モチーフの残りステップ

// =====================================================
// KOSMOS2 MIDI CC Receiver
// TouchOSC Controller 対応
// =====================================================

int ccDensity = 64;   // CC20
int ccPitch   = 64;   // CC21
int ccSpeed   = 0;    // CC22 (relative)
int ccScale   = 0;    // CC23 (0〜3)
int ccVolume  = 64;   // CC7

// 内部パラメータ
float density = 0.5f;
int pitchOffset = 0;
float speedMul = 1.0f;

bool manualMode = false;
unsigned long manualModeTimeout = 0;

struct ArpEngine {
    bool active = false;

    uint8_t baseNote = 60;
    int scaleMode = 0;

    int currentDegree = 0;
    int octave = 0;

    bool arpGoingUp = true;     // ★ 上昇フェーズか下降フェーズか
    int remainingUpSteps = 0;   // ★ 上昇の残りステップ
    int remainingDownSteps = 0; // ★ 下降の残りステップ

    uint32_t nextStepMs = 0;

    // ★ 上昇・下降でスピードを変える
    uint32_t upIntervalMs = 80;     // 上昇スピード
    uint32_t downIntervalMs = 120;  // 下降スピード

    bool noteOn = false;
    uint8_t lastNote = 0;

    bool stopOnUp = false;
    bool stopOnDown = false;
};

ArpEngine g_arp;

int noteLengthA = 90;   // A パート（今まで通り）
int noteLengthB = 600;  // ★ B パートは長め

bool noteIsOnD = false;
uint8_t lastNoteD = 0;
unsigned long noteOffTimeD = 0;

// -----------------------------------------------------
// MIDI CC 受信
// -----------------------------------------------------
void handleCC(uint8_t cc, uint8_t val) {

    switch (cc) {

        // ---------------------------------------------
        // Density（発音率） CC20
        // ---------------------------------------------
        case 20:
            ccDensity = val;
            // mainDensity に直接反映（0〜100%）
            mainDensity = map(val, 0, 127, 0, 100);
            break;

        // ---------------------------------------------
        // Pitch（音程オフセット） CC21
        // ---------------------------------------------
        case 21:
            ccPitch = val;
            pitchOffset = map(val, 0, 127, -24, +24);
            break;

        // ---------------------------------------------
        // Speed（テンポ倍率） CC22
        // ---------------------------------------------
        case 22:
            ccSpeed = val;
            speedMul = map(val, 0, 127, 50, 200) / 100.0f;

            // ★ テンポ変更時にステップを即リセット
            g_arp.nextStepMs = millis();      // Passage Engine
            break;

        // ---------------------------------------------
        // Scale（スケール切替） CC23
        // ---------------------------------------------
        case 23:
            ccScale = val;

            // KOSMOS2 の内部自動スケール切替を無効化
            pendingScale = -1;

            // TouchOSC の値をそのまま採用
            scaleMode = val % 3;

            // スケール変更時はパターン再生成
            executeRandom();
            break;

        // ---------------------------------------------
        // Volume（マスター音量） CC7
        // ---------------------------------------------
        case 7:
            ccVolume = val;
            masterVolume = (float)val / 127.0f;

            // ★ Core1 にも送る（ch=0でOK）
            midi_bridge_send_cc(7, val, 0);
            break;

        case 30:
            // ★ TouchOSC ガチャボタン
            programA = random(0, 17);
            programB = random(0, 17);
            programC = random(0, 17);
            programD = random(0, 17);

            // A
            if (programA == 16) muteA = true;
            else { muteA = false; midi_bridge_send_cc(100, programA, 0); }

            // B
            if (programB == 16) muteB = true;
            else { muteB = false; midi_bridge_send_cc(100, programB, 1); }

            // C
            if (programC == 16) muteC = true;
            else { muteC = false; midi_bridge_send_cc(100, programC, 2); }

            // D
            if (programD == 16) muteD = true;
            else { muteD = false; midi_bridge_send_cc(100, programD, 3); }

            drawProgramInfo();
            break;

        case 31:
            // ★ TouchOSC リセットボタン
            programA = 1;
            programB = 6;
            programC = 14;
            programD = 7;

            // ★ ミュートも全解除
            muteA = false;
            muteB = false;
            muteC = false;
            muteD = false;

            // ★ 音色を Core1 に再送信して同期
            midi_bridge_send_cc(100, programA, 0);
            midi_bridge_send_cc(100, programB, 1);
            midi_bridge_send_cc(100, programC, 2);
            midi_bridge_send_cc(100, programD, 3);

            drawProgramInfo();
            break;

        case 32:
            // ★ TouchOSC ミュートランダム
            muteA = (random(0, 2) == 0);
            muteB = (random(0, 2) == 0);
            muteC = (random(0, 2) == 0);
            muteD = (random(0, 2) == 0);

            // ★ ミュート解除されたパートは音色を再送信して同期
            if (!muteA) midi_bridge_send_cc(100, programA, 0);
            if (!muteB) midi_bridge_send_cc(100, programB, 1);
            if (!muteC) midi_bridge_send_cc(100, programC, 2);
            if (!muteD) midi_bridge_send_cc(100, programD, 3);

            drawProgramInfo();
            break;

    }
    // ★ CC を受け取ったら手動モードに入る
    manualMode = true;
    manualModeTimeout = millis() + 20000;  // ★ 20秒間は手動モード扱い

    drawCCValue(cc, val);
}

void usb_send_note_on(uint8_t note, uint8_t vel, uint8_t ch) {
    uint8_t msg[3] = { uint8_t(0x90 | (ch & 0x0F)), note, vel };
    usb_midi.write(msg, 3);
}

void usb_send_note_off(uint8_t note, uint8_t ch) {
    uint8_t msg[3] = { uint8_t(0x80 | (ch & 0x0F)), note, 0 };
    usb_midi.write(msg, 3);
}

inline void midi_bridge_send_note_on(uint8_t note, uint8_t vel, uint8_t ch=0){
    MidiEvent ev{EV_NOTE_ON, note, vel, ch};

    for (int i = 0; i < 100; i++) {
        if (MidiQ::push(ev)) {
            // ★ Core1 への送信が成功した瞬間に USB へも送る
            usb_send_note_on(note, vel, ch);
            return;
        }
        tight_loop_contents();
    }
}

inline void midi_bridge_send_note_off(uint8_t note, uint8_t ch=0){
    MidiEvent ev{EV_NOTE_OFF, note, 0, ch};

    for (int i = 0; i < 100; i++) {
        if (MidiQ::push(ev)) {
            usb_send_note_off(note, ch);
            return;
        }
        tight_loop_contents();
    }
}

void resetAllParts() {
    programA = 0;
    programA = 1;
    programB = 6;
    programC = 14;
    programD = 7;

    // ★ ミュートも全解除
    muteA = false;
    muteB = false;
    muteC = false;
    muteD = false;

    // ★ 音色を Core1 に再送信して同期
    midi_bridge_send_cc(100, programA, 0);
    midi_bridge_send_cc(100, programB, 1);
    midi_bridge_send_cc(100, programC, 2);
    midi_bridge_send_cc(100, programD, 3);
}

// ============================================================
// ★ Yボタン（リズムパターン切替）
// ============================================================
bool lastY = false;
unsigned long pressStartY = 0;
unsigned long lastStepY = 0;

const int RHYTHM_PATTERN_COUNT = 5;

void readButtons() {
    bool nowA = (digitalRead(KEY_A_PIN) == LOW);
    bool nowB = (digitalRead(KEY_B_PIN) == LOW);
    bool nowX = (digitalRead(KEY_X_PIN) == LOW);
    bool nowY = (digitalRead(KEY_Y_PIN) == LOW);

    // ============================================================
    // ★ A ボタン（A パートのみ）
    // ============================================================

    if (nowA && !lastAState) {
        pressStartA = millis();
    }

    if (!nowA && lastAState) {
        unsigned long dur = millis() - pressStartA;
        if (dur < 300) {

            // ★ A パート音色変更のみ
            programA = (programA + 1) % 16;
            midi_bridge_send_cc(100, programA, 0);  // ch0 = A パート
            drawProgramInfo();
        }
    }

    // ============================================================
    // ★ B ボタン（B パートのみ）
    // ============================================================

    if (nowB && !lastBState) {
        pressStartB = millis();
    }

    if (!nowB && lastBState) {
        unsigned long dur = millis() - pressStartB;
        if (dur < 300) {

            // ★ B パート音色変更のみ
            programB = (programB + 1) % 16;
            midi_bridge_send_cc(100, programB, 1);  // ch1 = B パート
            drawProgramInfo();
        }
    }

    // ============================================================
    // ★ A + B 同時押し → MIDI Start / Stop
    // ============================================================

    if (nowA && nowB && (!lastAState || !lastBState)) {

        if (!isPlaying) {
            // ★ 再生開始
            usb_midi.write(0xFA);   // MIDI Start
            isPlaying = true;
            bpmColor = COLOR_WHITE;   // ★ Start → 白
            drawTopText();
        } else {
            // ★ 停止
            usb_midi.write(0xFC);   // MIDI Stop
            isPlaying = false;
            bpmColor = COLOR_RED;   // ★ End → 赤
            drawTopText();
        }
    }

    // ============================================================
    // ★ X ボタン（全パート音色ガチャ：ミュートあり）
    // ============================================================

    if (nowX && !lastX) {
        pressStartX = millis();
    }

    if (!nowX && lastX) {
        unsigned long dur = millis() - pressStartX;
        if (dur < 300) {

            // ★ X 単独押しで全パート音色ガチャ（ミュートあり）
            programA = random(0, 17);   // 0〜15 = 音色, 16 = ミュート
            programB = random(0, 17);
            programC = random(0, 17);
            programD = random(0, 17);

            // ---- A パート ----
            if (programA == 16) {
                muteA = true;
            } else {
                muteA = false;
                midi_bridge_send_cc(100, programA, 0);
            }

            // ---- B パート ----
            if (programB == 16) {
                muteB = true;
            } else {
                muteB = false;
                midi_bridge_send_cc(100, programB, 1);
            }

            // ---- C パート ----
            if (programC == 16) {
                muteC = true;
            } else {
                muteC = false;
                midi_bridge_send_cc(100, programC, 2);
            }

            // ---- D パート ----
            if (programD == 16) {
                muteD = true;
            } else {
                muteD = false;
                midi_bridge_send_cc(100, programD, 3);
            }

            drawProgramInfo();
        }
    }

    // ============================================================
    // ★ Y ボタン（全パート音色リセット専用）
    // ============================================================

    if (nowY && !lastY) {
        pressStartY = millis();
    }

    if (!nowY && lastY) {
        unsigned long dur = millis() - pressStartY;
        if (dur < 300) {

            // ★ Y 単独押しで全パート音色リセット（JOY_SW 無視）
            programA = 1;
            programB = 6;
            programC = 14;
            programD = 7;

            midi_bridge_send_cc(100, programA, 0);
            midi_bridge_send_cc(100, programB, 1);
            midi_bridge_send_cc(100, programC, 2);
            midi_bridge_send_cc(100, programD, 3);

            drawProgramInfo();
        }
    }

    // ============================================================
    lastAState = nowA;
    lastBState = nowB;
    lastX = nowX;
    lastY = nowY;
}

void drawPlayIndicator(uint16_t color) {
    // 左下に ● を描く
    lcdFillRect(0, 220, 20, 20, COLOR_BLACK);  // 背景クリア
    lcdPrint(5, 222, "●", color, COLOR_BLACK, 1);
}

void drawScaleName() {
    lcdFillRect(0, 20, 240, 15, COLOR_BLACK);

    const char* name =
        (scaleMode == 0) ? "HEI" :
        (scaleMode == 1) ? "MIYA" :
                           "INSEN";

    lcdPrint(5, 22, name, COLOR_WHITE, COLOR_BLACK, 1);
}

void drawProgramInfo() {
    // 表示エリアをクリア（座標はあなたのUIに合わせて調整）
    lcdFillRect(0, 35, 240, 20, COLOR_BLACK);

    char buf[16];

    // ---- A パート ----
    if (muteA) sprintf(buf, "A:--");
    else       sprintf(buf, "A:%02d", programA);
    lcdPrint(5, 40, buf, COLOR_WHITE, COLOR_BLACK, 1);

    // ---- B パート ----
    if (muteB) sprintf(buf, "B:--");
    else       sprintf(buf, "B:%02d", programB);
    lcdPrint(65, 40, buf, COLOR_WHITE, COLOR_BLACK, 1);

    // ---- C パート ----
    if (muteC) sprintf(buf, "C:--");
    else       sprintf(buf, "C:%02d", programC);
    lcdPrint(125, 40, buf, COLOR_WHITE, COLOR_BLACK, 1);

    // ---- D パート ----
    if (muteD) sprintf(buf, "D:--");
    else       sprintf(buf, "D:%02d", programD);
    lcdPrint(185, 40, buf, COLOR_WHITE, COLOR_BLACK, 1);
}

// 中心値
int centerX = 0;
int centerY = 0;

// 移動平均用バッファ
const int FILTER_N = 20;
int bufX[FILTER_N];
int bufY[FILTER_N];
int bufIndex = 0;

// 現在の方向
int joyXState = 0;  // -1=左, 0=中心, 1=右
int joyYState = 0;

// 1回だけ反応
bool joyLeftOnce = false;
bool joyRightOnce = false;
bool joyUpOnce = false;
bool joyDownOnce = false;

void calibrateJoystick() {
  long sumX = 0;
  long sumY = 0;

  for (int i = 0; i < 50; i++) {
    sumX += analogRead(27);
    sumY += analogRead(26);
    delay(5);
  }

  centerX = sumX / 50;
  centerY = sumY / 50;

  // バッファ初期化
  for (int i = 0; i < FILTER_N; i++) {
    bufX[i] = centerX;
    bufY[i] = centerY;
  }
}

int joyX = 0;
int joyY = 0;

void readJoystick() {
  joyX = analogRead(JOY_X_PIN);
  joyY = analogRead(JOY_Y_PIN);

  btnSW = (digitalRead(JOY_SW_PIN) == LOW);
}

// ==== LCD コマンド送信 ====
void lcdCmd(uint8_t cmd) {
  digitalWrite(LCD_DC, LOW);
  digitalWrite(LCD_CS, LOW);
  LCD_SPI.transfer(cmd);
  digitalWrite(LCD_CS, HIGH);
}

void lcdData(uint8_t dat) {
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  LCD_SPI.transfer(dat);
  digitalWrite(LCD_CS, HIGH);
}

// ==== LCD 初期化（Waveshare 純正） ====
void lcdInit() {
  pinMode(LCD_CS, OUTPUT);
  pinMode(LCD_DC, OUTPUT);
  pinMode(LCD_RST, OUTPUT);
  pinMode(LCD_BL, OUTPUT);

  digitalWrite(LCD_BL, LOW);
  digitalWrite(LCD_CS, HIGH);

  LCD_SPI.setSCK(LCD_SCK);
  LCD_SPI.setTX(LCD_MOSI);
  LCD_SPI.begin();
  LCD_SPI.beginTransaction(SPISettings(24000000, MSBFIRST, SPI_MODE0));

  // ハードウェアリセット
  digitalWrite(LCD_RST, LOW); delay(10);
  digitalWrite(LCD_RST, HIGH); delay(120);

  // ==== Waveshare ST7789 初期化コマンド ====
  lcdCmd(0x36); lcdData(0x70);
  lcdCmd(0x3A); lcdData(0x05);
  lcdCmd(0xB2); lcdData(0x0C); lcdData(0x0C); lcdData(0x00); lcdData(0x33); lcdData(0x33);
  lcdCmd(0xB7); lcdData(0x35);
  lcdCmd(0xBB); lcdData(0x19);
  lcdCmd(0xC0); lcdData(0x2C);
  lcdCmd(0xC2); lcdData(0x01);
  lcdCmd(0xC3); lcdData(0x12);
  lcdCmd(0xC4); lcdData(0x20);
  lcdCmd(0xC6); lcdData(0x0F);
  lcdCmd(0xD0); lcdData(0xA4); lcdData(0xA1);
  lcdCmd(0xE0); for (int i = 0; i < 14; i++) lcdData(0x00);
  lcdCmd(0xE1); for (int i = 0; i < 14; i++) lcdData(0x00);
  lcdCmd(0x21);
  lcdCmd(0x11); delay(120);
  lcdCmd(0x29); delay(20);

  digitalWrite(LCD_BL, HIGH); // バックライト ON
}

// ==== 描画ユーティリティ ====
void lcdSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  lcdCmd(0x2A);
  lcdData(x0 >> 8); lcdData(x0 & 0xFF);
  lcdData(x1 >> 8); lcdData(x1 & 0xFF);

  lcdCmd(0x2B);
  lcdData(y0 >> 8); lcdData(y0 & 0xFF);
  lcdData(y1 >> 8); lcdData(y1 & 0xFF);

  lcdCmd(0x2C);
}

void lcdFill(uint16_t color) {
  lcdSetWindow(0, 0, 239, 239);
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  for (uint32_t i = 0; i < 240UL * 240UL; i++) {
    LCD_SPI.transfer(color >> 8);
    LCD_SPI.transfer(color & 0xFF);
  }
  digitalWrite(LCD_CS, HIGH);
}

void lcdDrawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || x >= 240 || y < 0 || y >= 240) return;
  lcdSetWindow(x, y, x, y);
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  LCD_SPI.transfer(color >> 8);
  LCD_SPI.transfer(color & 0xFF);
  digitalWrite(LCD_CS, HIGH);
}

void lcdDrawChar(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size) {
  if (c < 32 || c > 126) return;
  const uint8_t *bitmap = font5x7[c - 32];

  for (int col = 0; col < 5; col++) {
    uint8_t line = bitmap[col];
    for (int row = 0; row < 7; row++) {
      uint16_t drawColor = (line & 0x01) ? color : bg;
      lcdFillRect(x + col * size, y + row * size, size, size, drawColor);
      line >>= 1;
    }
  }
}

void lcdFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  lcdSetWindow(x, y, x + w - 1, y + h - 1);
  digitalWrite(LCD_DC, HIGH);
  digitalWrite(LCD_CS, LOW);
  for (int i = 0; i < w * h; i++) {
    LCD_SPI.transfer(color >> 8);
    LCD_SPI.transfer(color & 0xFF);
  }
  digitalWrite(LCD_CS, HIGH);
}

void lcdPrint(int16_t x, int16_t y, const char *str, uint16_t color, uint16_t bg, uint8_t size) {
  while (*str) {
    lcdDrawChar(x, y, *str, color, bg, size);
    x += 6 * size;
    str++;
  }
}

// =====================================================
//  UI パラメータ
// =====================================================
int steps = 16;
int hits = 5;
int rotation = 0;

int pattern[16];
int prob[16];

int currentStep = 0;
int selectedStep = 0;

// =====================================================
//  Euclid パターン生成
// =====================================================
void makeEuclid(int steps, int hits, int rot, int *pattern) {
  for (int i = 0; i < steps; i++) pattern[i] = 0;

  int bucket = 0;
  for (int i = 0; i < steps; i++) {
    bucket += hits;
    if (bucket >= steps) {
      bucket -= steps;
      int idx = (i + rot) % steps;
      pattern[idx] = 1;   // ★ int で 1 を入れる
    }
  }
}

// =====================================================
//  UI 全体描画
// =====================================================
void drawUI() {
    drawTopText();
    updateProbabilityBars();
    updateStepBars();
    updateStepDots();
    drawRandomMode();
    drawProgramInfo();
}

void drawOneProbabilityBar(int i) {
    int x0 = 5;
    int y0 = 60;
    int barWidth = 12;
    int maxHeight = 40;
    int gap = 3;

    int x = x0 + i * (barWidth + gap);
    int h = map(prob[i], 0, 100, 0, maxHeight);

    // 背景クリア（このバーの範囲だけ）
    lcdFillRect(x, y0, barWidth, maxHeight, COLOR_BLACK);

    // バー描画
    lcdFillRect(x, y0 + (maxHeight - h), barWidth, h, COLOR_CYAN);

    // 選択枠
    if (i == selectedStep) {
        lcdDrawPixel(x - 1, y0 - 1, COLOR_YELLOW);
        lcdDrawPixel(x + barWidth, y0 - 1, COLOR_YELLOW);
        lcdDrawPixel(x - 1, y0 + maxHeight, COLOR_YELLOW);
        lcdDrawPixel(x + barWidth, y0 + maxHeight, COLOR_YELLOW);
    }
}

void updateProbabilityBars() {
    static int lastProb[16];
    static int lastSelected = -1;

    for (int i = 0; i < 16; i++) {
        if (prob[i] != lastProb[i] || selectedStep != lastSelected) {
            drawOneProbabilityBar(i);
            lastProb[i] = prob[i];
        }
    }
    lastSelected = selectedStep;
}

void updateStepBars() {
    static int lastStep = -1;
    static int lastPattern[16];

    int x0 = 5;
    int y0 = 120;
    int barWidth = 12;
    int barHeight = 20;
    int gap = 3;

    // ★ パターン変化チェック
    bool patternChanged = false;
    for (int i = 0; i < 16; i++) {
        if (mainPattern[i] != lastPattern[i]) {
            patternChanged = true;
            break;
        }
    }

    // ★ パターンが変わったら全バーを描き直す
    if (patternChanged) {
        for (int i = 0; i < 16; i++) {
            int x = x0 + i * (barWidth + gap);

            uint16_t color;
            if (mainPattern[i] == 1)
                color = COLOR_GREEN;   // パターンON → 緑色
            else
                color = COLOR_GRAY;     // パターンOFF → 灰色

            lcdFillRect(x, y0, barWidth, barHeight, color);
            lastPattern[i] = mainPattern[i];
        }
    }

    // ★ ステップ移動時
    if (currentStep != lastStep) {

        // 前ステップを元の色に戻す
        if (lastStep >= 0) {
            int x = x0 + lastStep * (barWidth + gap);

            uint16_t color;
            if (mainPattern[lastStep] == 1)
                color = COLOR_GREEN;
            else
                color = COLOR_GRAY;

            lcdFillRect(x, y0, barWidth, barHeight, color);
        }

        // ★ 現在ステップを赤 or オレンジで塗る
        int x = x0 + currentStep * (barWidth + gap);

        if (mainNoteExtended) {
            lcdFillRect(x, y0, barWidth, barHeight, COLOR_ORANGE);  // 伸びてるとき
        } else {
            lcdFillRect(x, y0, barWidth, barHeight, COLOR_RED);     // 通常
        }

        lastStep = currentStep;
    }
}

void updateStepDots() {
    static int lastStep = -1;

    if (currentStep != lastStep) {
        int x0 = 5;
        int y0 = 160;
        int gap = 14;

        // 前のステップを灰色に戻す
        if (lastStep >= 0) {
            lcdPrint(x0 + lastStep * gap, y0, "・", COLOR_DARK_GRAY, COLOR_BLACK, 1);
        }

        // 現在のステップを赤に
        lcdPrint(x0 + currentStep * gap, y0, "●", COLOR_RED, COLOR_BLACK, 1);

        lastStep = currentStep;
    }
}

// 長押し判定用
unsigned long holdStartHits = 0;
unsigned long holdStartRot  = 0;

unsigned long swPressStart = 0;

// ランダムモード
// 0 = Euclidランダム
// 1 = ステップランダム
// 2 = 両方ランダム
int randomMode = 1;


bool updateEuclidEdit() {

  bool changed = false;  // ★ 変更があったかどうか

  // --- まず同時押しを最優先で判定する ---
  if (btnLeft && btnRight) {
    executeRandom();
    changed = true;   // ★ ランダム実行は確実に変更
    return changed;
  }

  if (btnUp && btnDown) {
    randomMode = (randomMode + 1) % 3;
    drawRandomMode();
    changed = true;   // ★ モード変更も変更扱い
    return changed;
  }

  // --- ここから単押し処理（hits / rotation） ---
  static unsigned long holdStartHits = 0;
  static unsigned long holdStartRot  = 0;

  int oldHits = hits;
  int oldRot  = rotation;

  // hits 編集（左右）
  if (btnRight) {
    if (holdStartHits == 0) {
      holdStartHits = millis();
      hits++;
    } else if (millis() - holdStartHits > 300) {
      if ((millis() - holdStartHits) % 50 == 0) hits++;
    }
  }
  else if (btnLeft) {
    if (holdStartHits == 0) {
      holdStartHits = millis();
      hits--;
    } else if (millis() - holdStartHits > 300) {
      if ((millis() - holdStartHits) % 50 == 0) hits--;
    }
  }
  else {
    holdStartHits = 0;
  }

  // rotation 編集（上下）
  if (btnUp) {
    if (holdStartRot == 0) {
      holdStartRot = millis();
      rotation++;
    } else if (millis() - holdStartRot > 300) {
      if ((millis() - holdStartRot) % 50 == 0) rotation++;
    }
  }
  else if (btnDown) {
    if (holdStartRot == 0) {
      holdStartRot = millis();
      rotation--;
    } else if (millis() - holdStartRot > 300) {
      if ((millis() - holdStartRot) % 50 == 0) rotation--;
    }
  }
  else {
    holdStartRot = 0;
  }

  // ★ hits または rotation が変わったら changed = true
  if (hits != oldHits || rotation != oldRot) {
    changed = true;
  }

  // Euclid パターンを再生成
  makeEuclid(steps, hits, rotation, pattern);

  return changed;
}

// =====================================================
// ★ ランダムモード実行
//    randomMode:
//      0 = Euclid ランダム（steps/hits/rotation）
//      1 = Step ランダム（prob[]）
//      2 = 両方ランダム
// =====================================================
void executeRandom() {

    // -----------------------------------------
    // ★ Euclid（リズム）→ 全モードで鳴く密度を統一
    // -----------------------------------------
    if (randomMode == 0 || randomMode == 2) {

        steps = 16;                 // ★ 全モード8分固定
        hits  = random(3, 6);       // ★ 鳴く密度を低めに統一（最重要）
        rotation = random(0, steps);

        makeEuclid(steps, hits, rotation, pattern);
    }

    // -----------------------------------------
    // ★ Step（音程揺らぎ）→ 全モードで揺らぎを弱める
    // -----------------------------------------
    if (randomMode == 1 || randomMode == 2) {

        for (int i = 0; i < 16; i++) {

            // ★ 全モードで揺らぎ弱め（pattern が 1 になりすぎない）
            pattern[i] = random(-1, 1);   // -1,0
        }
    }

    // -----------------------------------------
    // ★ Probability（鳴く確率）→ 全モードで統一
    // -----------------------------------------
    for (int i = 0; i < 16; i++) {

        if (pattern[i] != 0) {
            prob[i] = random(50, 80);   // ★ 鳴く確率を中程度に統一
        } else {
            prob[i] = random(10, 40);   // ★ 休符ステップは低め
        }
    }

    // -----------------------------------------
    // ★ MainPattern（8分の ON/OFF）ランダム生成
    // -----------------------------------------
    int density = mainPatternDensity[scaleMode];  // ★ スケールごとの密度

    for (int i = 0; i < 16; i++) {
      int r = random(0, 100);
      mainPattern[i] = (r < density ? 1 : 0);
    }
}

// ---- 音名テーブル（シャープ表記）----
const char* noteNames[] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// ---- 音名を返す関数 ----
const char* getNoteName(uint8_t note) {
    int idx = note % 12;
    return noteNames[idx];
}

void drawRandomMode() {
    // 画面右上に表示（Key の1行下）
    const int x = 180;   // 右寄せ位置（あなたの画面幅に合わせて調整可）
    const int y = 18;    // ← Key が y=0 なので、1行下へ

    // 背景を黒で塗りつぶして上書き（重なり防止）
    lcdFillRect(x, y, 60, 16, COLOR_BLACK);

    // モード文字
    const char* label = "";
    if (randomMode == 0) label = "E";   // Euclid
    if (randomMode == 1) label = "B";   // Both
    if (randomMode == 2) label = "S";   // Step

    // 白文字・黒背景・フォントサイズ2
    lcdPrint(x, y, label, COLOR_WHITE, COLOR_BLACK, 2);
}

uint16_t noteColor(uint8_t note) {
    note = constrain(note, 36, 84);
    int n = note % 12;

    switch (n) {
        case 0:  return COLOR_RED;      // C
        case 2:  return COLOR_ORANGE;   // D
        case 4:  return COLOR_YELLOW;   // E
        case 5:  return COLOR_GREEN;    // F
        case 7:  return COLOR_CYAN;     // G
        case 9:  return COLOR_BLUE;     // A
        case 11: return COLOR_MAGENTA;  // B
        default: return COLOR_GRAY;     // 半音
    }
}

void pushNoteDot(uint8_t note) {
    int y = noteToY(note);

    for (int i = 0; i < 239; i++) {
        noteDotsY[i] = noteDotsY[i+1];
        noteDotsNote[i] = noteDotsNote[i+1];
    }

    noteDotsY[239] = y;
    noteDotsNote[239] = note;
}

void drawNoteDots() {
    lcdFillRect(0, 150, 240, 90, COLOR_BLACK);

    for (int x = 0; x < 240; x++) {
        int y = noteDotsY[x];
        if (y >= 150 && y < 240) {
            uint8_t note = noteDotsNote[x];
            lcdDrawPixel(x, y, noteColor(note));
        }
    }
}

void sendNoteOnCh(uint8_t note, uint8_t velocity, uint8_t ch) {
    uint8_t msg[3] = { (uint8_t)(0x90 | (ch & 0x0F)), note, velocity };
    usb_midi.write(msg, 3);
    midi_bridge_send_note_on(note, velocity, ch);
}

void sendNoteOffCh(uint8_t note, uint8_t ch) {
    uint8_t msg[3] = { (uint8_t)(0x80 | (ch & 0x0F)), note, 0 };
    usb_midi.write(msg, 3);
    midi_bridge_send_note_off(note, ch);
}

const uint8_t scale[] = { 0, 2, 4, 7, 9 };  
// Cメジャーペンタの度数（0=ルート）
const int scaleSize = 5;

uint8_t generateNote() {

    // 80%でスケール音、20%でランダム
    bool useScale = (rand() % 100) < 80;

    if (useScale) {
        // スケール音
        int degree = rand() % scaleSize;  // 0〜4
        int octave = 48;                  // C3を基準にする
        return octave + scale[degree];
    } else {
        // ランダム音（味付け）
        return 48 + (rand() % 24);  // C3〜B4
    }
}

uint8_t lastNote = 0;
bool noteIsOn = false;

unsigned long clockInterval = (60000UL / baseBPM) / 24;
unsigned long lastClockTime = 0;

void drawTopText() {
    static int lastSteps = -1;
    static int lastHits = -1;
    static int lastRot = -1;
    static int lastTranspose = 999;
    static int lastNote = -1;
    static int lastProbStep = -1;
    static int lastProbVal = -1;

    char buf[32];

    // --- Euclid 情報 ---
    if (steps != lastSteps || hits != lastHits || rotation != lastRot) {
        // 前の文字を黒で上書き
        lcdFillRect(0, 0, 240, 15, COLOR_BLACK);

        sprintf(buf, "Euclid  S:%d H:%d R:%d", steps, hits, rotation);
        lcdPrint(5, 5, buf, COLOR_WHITE, COLOR_BLACK, 1);

        lastSteps = steps;
        lastHits = hits;
        lastRot = rotation;
    }

    // --- Key 表示 ---
    if (transpose != lastTranspose) {
        lcdFillRect(130, 0, 50, 15, COLOR_BLACK);

        sprintf(buf, "Key:%+d", transpose);
        lcdPrint(130, 5, buf, COLOR_YELLOW, COLOR_BLACK, 1);

        lastTranspose = transpose;
    }

    // --- Note 表示 ---
    if (lastNoteMain  != lastNote) {
        lcdFillRect(180, 0, 60, 15, COLOR_BLACK);

        sprintf(buf, "Note:%s", getNoteName(lastNoteMain ));
        lcdPrint(180, 5, buf, COLOR_CYAN, COLOR_BLACK, 1);

        lastNote = lastNoteMain ;
    }

    // --- Probability 情報 ---
    if (selectedStep != lastProbStep || prob[selectedStep] != lastProbVal) {
        lcdFillRect(0, 18, 240, 20, COLOR_BLACK);

        sprintf(buf, "Prob Step:%d %d%%", selectedStep, prob[selectedStep]);
        lcdPrint(5, 23, buf, COLOR_WHITE, COLOR_BLACK, 1);

        lastProbStep = selectedStep;
        lastProbVal = prob[selectedStep];
    }

    // --- BPM 表示 ---
    static int lastBPM = -1;
    static uint16_t lastColor = 0xFFFF;

    if (stepBPM != lastBPM || bpmColor != lastColor) {
        lcdFillRect(130, 20, 60, 20, COLOR_BLACK);

        char buf[16];
        sprintf(buf, "BPM:%d", stepBPM);
        lcdPrint(130, 25, buf, bpmColor, COLOR_BLACK, 1);

        lastBPM = stepBPM;
        lastColor = bpmColor;
    }
}

// -----------------------------------------------------
// ★ スケール配列の中で note が何番目かを返す
//    見つからない場合は最も近い音を返す（安全）
// -----------------------------------------------------
int findScaleIndex(uint8_t note) {
    int bestIndex = 0;
    int bestDiff  = 9999;

    for (int i = 0; i < scaleSize; i++) {
        int diff = abs((int)note - (int)scale[i]);
        if (diff < bestDiff) {
            bestDiff  = diff;
            bestIndex = i;
        }
    }
    return bestIndex;
}

// =====================================================
// ★ メインメロディ生成（陰音階＋パターン揺らぎ＋トリルフラグ）
// =====================================================
uint8_t generateNoteA(bool &trillFlag, int &degreeOut) {

    // ★ pitchOffset を base に反映
    int base = 48 + pitchOffset;

    // ---- スケール選択 ----
    const uint8_t* scale;
    int scaleSize;

    if (scaleMode == 0) { 
        scale = SCALE_HEI;  
        scaleSize = SCALE_HEI_SIZE; 
    }
    else if (scaleMode == 1) { 
        scale = SCALE_MIYA; 
        scaleSize = SCALE_MIYA_SIZE; 
    }
    else if (scaleMode == 2) { 
        scale = SCALE_INSEN; 
        scaleSize = SCALE_INSEN_SIZE; 
    }
    else {
        scale = SCALE_PENTA; 
        scaleSize = SCALE_PENTA_SIZE; 
    }

    // ---- ランダム方向転換 ----
    if (random(0, 100) < 20) {
        dirA = -dirA;
    }

    // ---- degree 更新 ----
    degreeA += dirA;

    // ---- 範囲外なら反転 ----
    if (degreeA < 0) {
        degreeA = 1;
        dirA = 1;
    }
    if (degreeA >= scaleSize) {
        degreeA = scaleSize - 2;
        dirA = -1;
    }

    degreeOut = degreeA;

    // ---- MIDI ノート生成 ----
    uint8_t note = base + scale[degreeA];

    // transpose を最後に加算
    note += transpose;

    return note;
}

int downBias = 50;
int upBias = 50;

// B の刻み間隔（4分 or 8分）
int noteBDiv = 2;  
// 2 = 4分（8分×2）
// 1 = 8分（8分×1）

uint8_t generateNoteB(int &degreeOutB) {

    // ---- スケール選択（A パートと同じ）----
    const uint8_t* scale;
    int scaleSize;

    if (scaleMode == 0) { 
        scale = SCALE_HEI;  
        scaleSize = SCALE_HEI_SIZE; 
    }
    else if (scaleMode == 1) { 
        scale = SCALE_MIYA; 
        scaleSize = SCALE_MIYA_SIZE; 
    }
    else if (scaleMode == 2) { 
        scale = SCALE_INSEN; 
        scaleSize = SCALE_INSEN_SIZE; 
    }
    else {
        scale = SCALE_PENTA; 
        scaleSize = SCALE_PENTA_SIZE; 
    }

    // ---- B パートの基準音（1オクターブ低い C1=24）----
    int base = 24 + pitchOffset;

    // ---- ランダム方向転換（自然な揺れ）----
    if (random(0, 100) < 15) {
        dirB = -dirB;
    }

    // ---- degree 更新 ----
    degreeB += dirB;

    // ---- 範囲外なら反転して戻す ----
    if (degreeB < 0) {
        degreeB = 1;
        dirB = 1;
    }
    if (degreeB >= scaleSize) {
        degreeB = scaleSize - 2;
        dirB = -1;
    }

    degreeOutB = degreeB;

    // ---- MIDI ノート生成 ----
    uint8_t note = base + scale[degreeB];

    // transpose を最後に加算
    note += transpose;

    return note;
}


// =====================================================
// ★ モード切替インターバル
// =====================================================
unsigned long lastModeChange = 0;
unsigned long modeInterval   = 10000;  // 初期値（あとでランダムに更新）

// =====================================================
// ★ モード切替インターバル
// =====================================================
unsigned long ccDisplayTimer = 0;

void drawCCValue(byte cc, byte value) {
    // 下部エリアをクリア（高さ20px）
    lcdFillRect(0, 220, 240, 20, 0x0000);  // 黒で塗りつぶし

    char buf[32];
    sprintf(buf, "CC %d : %d", cc, value);

    // 白文字で表示
    lcdPrint(4, 222, buf, 0xFFFF, 0x0000, 1);

    ccDisplayTimer = millis();
}

void drawEuclidParams() {
    // 上部エリアをクリア（高さ20px）
    lcdFillRect(0, 0, 240, 20, 0x0000);  // 黒

    char buf[32];
    sprintf(buf, "EUC S:%d H:%d R:%d", steps, hits, rotation);

    // 白文字で表示
    lcdPrint(4, 4, buf, 0xFFFF, 0x0000, 1);
}

// ★ 小休止用
unsigned long lastRest = 0;
bool inRest = false;
int restStepsRemaining = 0; // ★ 休止ステップ数

int sustainBias = 125;

bool ghost = false;

// ★ キーチェンジ用
unsigned long lastKeyChange = 0;
unsigned long nextKeyChangeInterval = 20000;

unsigned long nextRestInterval = 20000; // 初期値

// ★ 微細揺らぎ
void microBpmNudge() {
    int n = random(-3, 4);  // -3〜+3
    baseBPM = constrain(baseBPM + n, 40, 260);
}

int snapToHeichoshi(int raw, int transpose) {

    const int heichoshi[5] = {0, 1, 5, 7, 8};

    int pitch = (raw - transpose) % 12;
    if (pitch < 0) pitch += 12;

    int best = heichoshi[0];
    int bestDiff = abs(pitch - heichoshi[0]);

    for (int i = 1; i < 5; i++) {
        int diff = abs(pitch - heichoshi[i]);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = heichoshi[i];
        }
    }

    return (raw - pitch) + best;
}

unsigned long silenceLength = 0;

void randomizeMainPattern() {
    for (int i = 0; i < 16; i++) {

        int r = random(0, 100);

        if (i > 0 && mainPattern[i-1] == 0) {
            // 連続休符を避ける → 休符後は鳴く確率を上げる
            mainPattern[i] = (r < 20 ? 0 : 1);
        } else {
            // 通常
            mainPattern[i] = (r < 40 ? 0 : 1);
        }
    }
}

void drawAllStepBars() {
    int x0 = 5;
    int y0 = 120;
    int barWidth = 12;
    int barHeight = 20;
    int gap = 3;

    // 背景を黒でクリア
    lcdFillRect(0, 120, 240, 20, COLOR_BLACK);

    for (int i = 0; i < 16; i++) {
        int x = x0 + i * (barWidth + gap);

        // 起動時は全部白
        lcdFillRect(x, y0, barWidth, barHeight, COLOR_WHITE);
    }
}

int phraseDir = 1;   // +1 = 上行, -1 = 下降

void sendControlChange(uint8_t cc, uint8_t value, uint8_t channel = 0) {
    midi_bridge_send_cc(cc, value, channel);
}

void sendAllNotesOff() {
    for (int ch = 0; ch < 16; ch++) {
        sendControlChange(123, 0, ch);  // CC123 = All Notes Off
    }
}

void safeNoteOffA() {
    if (noteIsOnMain) {
        sendNoteOffCh(lastNoteMain, 0);
        noteIsOnMain = false;
    }
}

int mainDegree = 8;
bool mainGoingDown = true;

uint8_t mapToScale(uint8_t note, const uint8_t* sc, int scSize) {
    int base = (note / 12) * 12;
    int best = base + sc[0];
    int bestDist = abs(note - best);

    for (int i = 1; i < scSize; i++) {
        int cand = base + sc[i];
        int dist = abs(note - cand);
        if (dist < bestDist) {
            best = cand;
            bestDist = dist;
        }
    }
    return best;
}

int findNearestDegree(uint8_t note, const uint8_t* sc, int scSize, int transpose) {
    int root = 60 + transpose;
    int rel = note - root;  // 半音差

    // スケールの中で最も近い度数を探す
    int bestDeg = 0;
    int bestDist = abs(rel - sc[0]);

    for (int i = 1; i < scSize; i++) {
        int dist = abs(rel - sc[i]);
        if (dist < bestDist) {
            bestDist = dist;
            bestDeg = i;
        }
    }
    return bestDeg;
}

void drawSplash() {
    lcdFill(COLOR_BLACK);
    lcdPrint(62, 100, "KOSMOS2", COLOR_WHITE, COLOR_BLACK, 3);
    lcdPrint(106, 135, "v2.0.0", COLOR_DARK_GRAY, COLOR_BLACK, 1);
    delay(10000);
}

void playStartupArp() {
    uint8_t notes[3] = { 52, 60, 69 }; // E3, C4, A4
    for (int i = 0; i < 3; i++) {
        midi_bridge_send_note_on(notes[i], 100, 0);
        delay(180);
        midi_bridge_send_note_off(notes[i], 0);
    }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("Core0: setup start");

  usb_midi.begin();  

  multicore_launch_core1(core1_main);

  lcdInit();
  
  drawSplash();

  lcdFill(COLOR_BLACK);

  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);
  pinMode(KEY_X_PIN, INPUT_PULLUP);
  pinMode(KEY_Y_PIN, INPUT_PULLUP);

  for (int i = 0; i < 16; i++) prob[i] = 100;

  executeRandom();
  transpose += 5;
  
  // ---- 起動時アルペジオ初期化（美しい低音スタート） ----
  int baseDegree = SCALE_HEI[0];   // 平調子の最も低い度数（D）
  int baseOct = 6;                 // C6

  lastNoteMain = baseOct * 12 + baseDegree + transpose;

  nextSilenceTime = millis() + 2500;

  randomizeMainPattern();

  // ---- 起動時は必ず陰音階でスタート ----
  scaleMode = 2;        // 陰音階

  // ---- autoMode の暴走防止（起動直後に切り替わらないように）----
  autoModeTimer = millis() + 10000;  // 10秒後に初回切替

  degreeA = 0;
  dirA = 1;

  nextMainSilenceTime = millis() + random(8000, 15000);  // 8〜15秒後
  
  drawAllStepBars();

  for (int i = 0; i < 240; i++) noteDots[i] = -1;

  lastX = (digitalRead(KEY_X_PIN) == LOW);

  Serial.println("Core0: setup done, Core1 launched");
  
  masterVolume = 0.5f;
  midi_bridge_send_cc(7, 64, 0);
}

// ★ アルペジオ終了後に即メインへ吸着させるフラグ
bool forceMainStep = false;
bool arpWantsToEnd = false;   // ★ 終了希望フラグ（実際にはまだ終わらない）

const uint8_t* getScale(int mode, int &size) {
    if (mode == 0) { size = SCALE_HEI_SIZE;   return SCALE_HEI; }
    if (mode == 1) { size = SCALE_MIYA_SIZE;  return SCALE_MIYA; }
    if (mode == 1) { size = SCALE_INSEN_SIZE; return SCALE_INSEN; }
    size = SCALE_PENTA_SIZE; return SCALE_PENTA;
}

void arp_start(uint8_t baseNote, int scaleMode, int bpm) {
    g_arp.active = true;
    g_arp.baseNote = baseNote;
    g_arp.scaleMode = scaleMode;

    int scSize;
    getScale(scaleMode, scSize);

    // ★ 開始は必ず上昇
    g_arp.arpGoingUp = true;

    // ★ 上昇は低めから
    g_arp.currentDegree = 0;
    g_arp.octave = 0;

    // ★ 長さ候補
    const int lengthChoices[] = {4, 8};
    const int lengthCount = 2;

    // ★ 上昇・下降の長さを候補からランダム選択
    g_arp.remainingUpSteps   = lengthChoices[random(0, lengthCount)];
    //g_arp.remainingDownSteps = lengthChoices[random(0, lengthCount)];
    g_arp.remainingDownSteps = random(24, 48);

    // ★ 停止は「下降が終わったときだけ」
    g_arp.stopOnUp = false;                 // 上昇では絶対止まらない
    g_arp.stopOnDown = true;

    // 基本スピード
    uint32_t baseUp   = 60000UL / bpm / 8;  // 上昇
    uint32_t baseDown = 60000UL / bpm / 8;  // 下降

    // ランダム倍率（0.7〜1.3倍）
    float upMul   = random(70, 130) / 100.0f;
    float downMul = random(70, 130) / 100.0f;

    g_arp.upIntervalMs   = baseUp   * upMul;
    g_arp.downIntervalMs = baseDown * downMul;
}

void arp_stop() {
    if (g_arp.noteOn) {
        midi_bridge_send_note_off(g_arp.lastNote, 0);
        g_arp.noteOn = false;
    }
    g_arp.active = false;
}

void arp_update(uint32_t nowMs) {
    if (!g_arp.active) return;
    
    g_arp.baseNote = 60 + transpose + pitchOffset;

    if (nowMs < g_arp.nextStepMs) return;

    // 前のノートを切る
    if (g_arp.noteOn) {
        midi_bridge_send_note_off(g_arp.lastNote, 0);
        g_arp.noteOn = false;
    }

    int scSize;
    const uint8_t* sc = getScale(g_arp.scaleMode, scSize);

    // ★ ノート決定（共通）
    uint8_t note = g_arp.baseNote + g_arp.octave * 12 + sc[g_arp.currentDegree];

    // ノートオン
    midi_bridge_send_note_on(note, 90, 0);
    g_arp.lastNote = note;
    g_arp.noteOn = true;

    // =====================================================
    // ★ 上昇フェーズ
    // =====================================================
    if (g_arp.arpGoingUp) {

        int up = random(1, 3);  // 1〜2音上昇
        g_arp.currentDegree += up;

        // スケール上端処理
        if (g_arp.currentDegree >= scSize) {
            g_arp.currentDegree -= scSize;
            g_arp.octave++;
        }

        g_arp.remainingUpSteps--;
        if (g_arp.remainingUpSteps <= 0) {

            // ★ 上昇では絶対に止めない
            // → 下降へ切り替え
            g_arp.arpGoingUp = false;
            g_arp.currentDegree = scSize - 1;

            g_arp.nextStepMs = nowMs + g_arp.downIntervalMs;
            return;
        }

        g_arp.nextStepMs = nowMs + g_arp.upIntervalMs;
        return;
    }

    // =====================================================
    // ★ 下降フェーズ
    // =====================================================
    int drop = random(1, 3);  // 1〜2音下降
    g_arp.currentDegree -= drop;

    // スケール下端処理
    if (g_arp.currentDegree < 0) {
        g_arp.currentDegree = scSize - 1;
        g_arp.octave--;

        if (g_arp.octave < 0) {
            arp_stop();
            return;
        }
    }

    g_arp.remainingDownSteps--;
    if (g_arp.remainingDownSteps <= 0) {

        // ★ 下降後だけ停止判定
        if (g_arp.stopOnDown) {
            arp_stop();
            return;
        }

        // ★ 停止しない → 上昇へ戻る
        g_arp.arpGoingUp = true;
        g_arp.currentDegree = 0;
        g_arp.octave = random(0, 2);
        g_arp.remainingUpSteps = random(6, 20);

        g_arp.nextStepMs = nowMs + g_arp.upIntervalMs;
        return;
    }

    // 通常下降の次ステップ
    g_arp.nextStepMs = nowMs + g_arp.downIntervalMs;
}

// ★ アルペジオ頻度アップ用
unsigned long nextArpChanceTime = 0;
unsigned long lastStepD = 0;

void loop() {
    // ★ 安定 MIDI Clock（24ppqn）
    unsigned long nowMicros = micros();
    unsigned long clockIntervalMicros = (60000000UL / baseBPM) / 24;

    if (nowMicros - lastClockMicros >= clockIntervalMicros) {
        lastClockMicros += clockIntervalMicros;  // ← これが最重要（揺れゼロ）
        usb_midi.write(0xF8);  // MIDI Clock
    }

    // manualMode 中はサイレンスを強制解除
    if (manualMode) {
        mainSilenceActive = false;
    }
    
    // =====================================================
    // ★ USB MIDI 受信（Adafruit_USBD_MIDI 用）
    // =====================================================
    static uint8_t msg[3];
    static int idx = 0;

    while (usb_midi.available()) {

        uint8_t b = usb_midi.read();

        // ステータスバイト
        if (b & 0x80) {
            idx = 0;
            msg[idx++] = b;
            continue;
        }

        // データバイト
        if (idx < 3) {
            msg[idx++] = b;
        }

        // 3バイト揃ったら処理
        if (idx == 3) {

            uint8_t status = msg[0] & 0xF0;
            uint8_t d1 = msg[1];
            uint8_t d2 = msg[2];

            // ★ CC（TouchOSC）
            if (status == 0xB0) {
                handleCC(d1, d2);
            }

            idx = 0;
        }
    }
    // ★ 手動モードのタイムアウト判定
    if (manualMode && millis() > manualModeTimeout) {
        manualMode = false;
    }

    static unsigned long lastFrame = 0;
    unsigned long now_us = micros();
    if (now_us - lastFrame < 300) return;
    lastFrame = now_us;

    uint32_t now = millis();

    if (interval < 10) interval = 10;

    // =====================================================
    // NoteOff（メイン / B）
    // =====================================================
    if (noteIsOnMain && now >= noteOffTimeMain) {
        midi_bridge_send_note_off(lastNoteMain, 0);
        noteIsOnMain = false;
    }
    if (noteIsOnB && now >= noteOffTimeB) {
        midi_bridge_send_note_off(lastNoteB, 1);
        noteIsOnB = false;
    }

    // =====================================================
    // メインサイレンス制御（開始）
    // =====================================================
    if (!manualMode && !mainSilenceActive && now >= nextMainSilenceTime) {

        mainSilenceActive = true;

        // メインを無音化
        for (int i = 0; i < 16; i++) mainPattern[i] = 0;

        // メイン音を強制停止
        if (noteIsOnMain) {
            midi_bridge_send_note_off(lastNoteMain, 0);
            noteIsOnMain = false;
        }

        // B パートも強制停止
        if (noteIsOnB) {
            midi_bridge_send_note_off(lastNoteB, 1);
            noteIsOnB = false;
        }

        // アルペジオも強制停止
        if (g_arp.active) {
            arp_stop();
        }

        mainSilenceDuration = now + random(5000, 8000);
    }

    // =====================================================
    // メインサイレンス制御（終了）
    // =====================================================
    if (!manualMode && mainSilenceActive && now >= mainSilenceDuration) {

        mainSilenceActive = false;
        
        // ★ ここでパターン更新を必ず行う
        currentPattern = random(0, 6);
        memcpy(mainPattern, rhythmPatterns[currentPattern], sizeof(mainPattern));

        arp_start(60 + transpose + pitchOffset, scaleMode, stepBPM);
        executeRandom();
        
        if (!manualMode) {
            nextMainSilenceTime = now + random(30000, 50000);
        }
    }

    // =====================================================
    // 入力
    // =====================================================
    readButtons();
    readJoystick();

    // =====================================================
    // 呼吸 BPM / 自動トランスポーズ
    // =====================================================
    stepBPM = baseBPM * speedMul;

    static unsigned long nextTransposeTime = millis() + random(5000, 15000);
    if (now >= nextTransposeTime) {

        // ★ 7つの候補からランダムに選ぶ
        int idx = random(0, TRANSPOSE_COUNT);
        transpose = TRANSPOSE_LIST[idx];

        nextTransposeTime = now + random(5000, 15000);
        drawTopText();
    }

    // =====================================================
    // ランダムでアルペジオを挿入
    // =====================================================
    if (!g_arp.active && now >= nextArpChanceTime) {
        nextArpChanceTime = now + random(8000, 12000);
        if (!g_arp.active) {
            uint8_t base = 60 + transpose + pitchOffset;
            arp_start(base, scaleMode, stepBPM);
        }
    }

    // =====================================================
    // モード切替
    // =====================================================
    static int pendingScale = -1;
    static int pendingRandom = -1;

    if (now >= autoModeTimer) {

        autoModeTimer = now + random(80000, 120000);

        // ★ ランダムモード切替（既存）
        pendingRandom = (randomMode + 1) % 3;

        // ★ スケール切替（0→1→2→0…）
        pendingScale = (scaleMode + 1) % 3;

        drawRandomMode();
    }

    if (!g_arp.active) {

        if (pendingRandom != -1) {
            randomMode = pendingRandom;
            pendingRandom = -1;
        }

        if (pendingScale != -1) {
            scaleMode = pendingScale;
            pendingScale = -1;

            //drawScaleName(); 
            // ★ スケールが変わったらパターン再生成
            executeRandom();
        }
    }

    // =====================================================
    // Aパート　メインステップ（8分 × 16）
    // =====================================================
    interval = 60000UL / max(stepBPM, 30) / 4;
 
    if (now - lastMainStepTime >= interval) {
        lastMainStepTime = now;
        currentStep = (currentStep + 1) % 16;

        // メインパターン更新（無音中は上書きしない）
        if (currentStep == 0 && !mainSilenceActive) {
             currentPattern = random(0, 6);
            memcpy(mainPattern, rhythmPatterns[currentPattern], sizeof(mainPattern));
        }

        // ★ アルペジオ中はメインを鳴らさない
        if (!g_arp.active) {

            // NoteOff
            if (noteIsOnMain && now >= noteOffTimeMain) {
                midi_bridge_send_note_off(lastNoteMain, 0);
                noteIsOnMain = false;
            }

            // ★ パターン × 発音率（density）
            bool shouldPlay =
                ((!mainSilenceActive) || manualMode) &&
                (!muteA) &&
                (mainPattern[currentStep] == 1) &&
                (random(0, 100) < mainDensity);

            if (shouldPlay) {

                // ---- 素直な上昇下降だけにする ----
                if (mainGoingDown) {
                    mainDegree--;
                    if (mainDegree <= 0) {
                        mainDegree = 0;
                        mainGoingDown = false;
                    }
                } else {
                    mainDegree++;
                    if (mainDegree >= 7) {   // 0〜7 の範囲で往復
                        mainDegree = 7;
                        mainGoingDown = true;
                    }
                }

                // ---- スケール取得 ----
                const uint8_t* sc;
                int scSize;
                if (scaleMode == 0) { sc = SCALE_HEI;   scSize = SCALE_HEI_SIZE; }
                else if (scaleMode == 1) { sc = SCALE_MIYA;  scSize = SCALE_MIYA_SIZE; }
                else if (scaleMode == 2) { sc = SCALE_INSEN; scSize = SCALE_INSEN_SIZE; }
                else { sc = SCALE_PENTA; scSize = SCALE_PENTA_SIZE; }

                int idx = mainDegree % scSize;
                uint8_t noteA = 60 + transpose + pitchOffset + sc[idx];

                // ---- 発音 ----
                // BPM で基本の強さを決める
                int velA = map(stepBPM, 30, 140, 75, 115);
                // density（0〜100）で強さをスケール
                velA = velA * (mainDensity / 100.0f);
                // 最低でも 10 は確保
                velA = max(10, velA);
                midi_bridge_send_note_on(noteA, velA, 0);
                pushNoteDot(noteA);
                
                lastNoteMain = noteA;
                noteIsOnMain = true;

                // ★ ノート長は固定（density の影響なし）
                float finalLen = interval * 0.95;
                noteOffTimeMain = now + finalLen;
            } else {
                if (noteIsOnMain) {
                    midi_bridge_send_note_off(lastNoteMain, 0);
                    noteIsOnMain = false;
                }
            }
        }


        // =================================================
        // B パート（無音中は鳴らさない）
        // =================================================
        if (!mainSilenceActive || manualMode) {

            // ★ パターンは小節の頭だけで変える
            if (currentStep == 0) {
                currentBPattern = random(0, 3);  // 0,1,2
            }

            // ★ B パートの発音条件
            if (!muteB &&
                rhythmBPatterns[currentBPattern][currentStep] == 1 &&
                (random(0, 100) < mainDensity)) {

                uint8_t noteB = generateNoteB(degreeOutB);

                // 前の音を切る
                if (noteIsOnB) {
                    midi_bridge_send_note_off(lastNoteB, 1);
                }

                // 新しい音を鳴らす
                int velB = 55 * (mainDensity / 100.0f);
                velB = max(10, velB);
                midi_bridge_send_note_on(noteB, velB, 1);
                pushNoteDot(noteB);

                lastNoteB = noteB;
                noteIsOnB = true;

                // ★ B パートは長いサスティン
                noteOffTimeB = now + noteLengthB;   // 例：400〜600ms
            }
        }

        // =================================================
        // C パート（旋律的 + 少し低く寄り添う / レイドバック）
        // =================================================
        if (!mainSilenceActive || manualMode) {

            bool triggerC = false;

            // ★ 4分音符の「8分後ろ」にずらす（レイドバック）
            if (currentStep % 4 == 2) {
                triggerC = true;
            }

            // ★ 追加の 8 分音符（20%）
            else if (currentStep % 2 == 0 && random(0,100) < 20) {
                triggerC = true;
            }

            if (triggerC && !muteC && (random(0,100) < mainDensity)) {

                // ---- スケール取得 ----
                const uint8_t* sc;
                int scSize;
                if (scaleMode == 0) { sc = SCALE_HEI;   scSize = SCALE_HEI_SIZE; }
                else if (scaleMode == 1) { sc = SCALE_MIYA;  scSize = SCALE_MIYA_SIZE; }
                else if (scaleMode == 2) { sc = SCALE_INSEN; scSize = SCALE_INSEN_SIZE; }
                else { sc = SCALE_PENTA; scSize = SCALE_PENTA_SIZE; }

                // ---- C パート degree の“旋律的な動き” ----
                int r = random(0, 100);

                if (r < 40) {
                    // ① 方向性のある動き
                    int dir = (random(0,2)==0 ? -1 : 1);
                    degreeC += dir;
                }
                else if (r < 80) {
                    // ② 跳躍（3度・4度・5度）
                    int jumps[3] = {2, 3, 4};
                    int j = jumps[random(0,3)];
                    degreeC += (random(0,2)==0 ? -j : j);
                }
                else if (r < 95) {
                    // ③ A パートとの和声（+3, +5, -5）
                    int harm[3] = {2, 4, -4};
                    degreeC = mainDegree + harm[random(0,3)];
                }
                else {
                    // ④ たまにランダム
                    degreeC = random(0, scSize);
                }

                // ---- ★ 全体を少し低く寄り添わせる（基準を -1）----
                degreeC -= 1;

                // ---- ★ たまに 4 度下げる（確率 40% に増加）----
                if (random(0,100) < 40) {
                    degreeC -= 2;
                }

                // 範囲制限
                if (degreeC < 0) degreeC = 0;
                if (degreeC >= scSize) degreeC = scSize - 1;

                // ---- ノート番号 ----
                uint8_t root = 60 + transpose + pitchOffset + sc[degreeC];

                // ---- 前の音を止める ----
                if (chordIsOn) {
                    midi_bridge_send_note_off(lastChordNote, 2);
                    chordIsOn = false;
                }

                // ---- 発音（音量抑えめ）----
                int velC = 50 * (mainDensity / 100.0f);
                velC = max(10, velC);
                midi_bridge_send_note_on(root, velC, 2);
                pushNoteDot(root);
                
                lastChordNote = root;
                chordIsOn = true;

                // ---- 音長：全音符ベース + density で短縮 ----
                float densityFactor = mainDensity / 100.0f;

                float maxLen = interval * 4.0f;   // 全音符
                float minLen = interval * 0.5f;   // 8分音符

                float finalLen = minLen + (maxLen - minLen) * densityFactor;

                chordOffTime = now + finalLen;
            }

            // ---- NoteOff ----
            if (chordIsOn && now >= chordOffTime) {
                midi_bridge_send_note_off(lastChordNote, 2);
                chordIsOn = false;
            }
        }

        updateStepBars();
        updateStepDots();
        drawTopText();
    }

    // =================================================
    // D パート（和風で控えめな 16分アルペジオ）
    // =================================================
    static unsigned long lastStepD = 0;

    if (now - lastStepD >= interval) {
        lastStepD = now;

        // ★ 休止モードでは停止
        if (mainSilenceActive) {
            if (noteIsOnD) {
                midi_bridge_send_note_off(lastNoteD, 3);
                noteIsOnD = false;
            }
            return;
        }

        // ★ density（控えめ）
        if (random(0,100) >= mainDensity * 0.6) {   // 60% だけ反映
            if (noteIsOnD) {
                midi_bridge_send_note_off(lastNoteD, 3);
                noteIsOnD = false;
            }
            return;
        }

        if (!muteD) {

            // ---- スケール取得 ----
            const uint8_t* sc;
            int scSize;
            if (scaleMode == 0) { sc = SCALE_HEI;   scSize = SCALE_HEI_SIZE; }
            else if (scaleMode == 1) { sc = SCALE_MIYA;  scSize = SCALE_MIYA_SIZE; }
            else if (scaleMode == 2) { sc = SCALE_INSEN; scSize = SCALE_INSEN_SIZE; }
            else { sc = SCALE_PENTA; scSize = SCALE_PENTA_SIZE; }

            // =================================================
            // ★ 和風ロジック：小さな“揺れ”を中心にする
            // =================================================

            if (dPhraseRemain <= 0) {
                dPhraseRemain = random(3, 6);      // 短いフレーズ
                dGoingUp = (random(0,100) < 50);
            }

            // ---- 和風の揺れ：±1 を中心にする ----
            int r = random(0,100);
            if (r < 70) {
                degreeD += (dGoingUp ? 1 : -1);    // 70%：±1
            }
            else if (r < 90) {
                degreeD += (dGoingUp ? 2 : -2);    // 20%：±2
            }
            else {
                degreeD += (random(0,2)==0 ? -3 : 3); // 10%：アクセント
            }

            // ---- 端で折り返す ----
            if (degreeD < 2) {
                degreeD = 2;
                dGoingUp = true;
            }
            if (degreeD >= scSize - 1) {
                degreeD = scSize - 1;
                dGoingUp = false;
            }

            dPhraseRemain--;

            // ---- C パートに寄り添う（控えめ）----
            if (random(0,100) < 25) {
                degreeD = max(0, degreeC - 1);   // C より少し下
            }

            // ---- ノート番号 ----
            uint8_t note = 72 + transpose + pitchOffset + sc[degreeD];

            // ---- 前の音を止める ----
            if (noteIsOnD) {
                midi_bridge_send_note_off(lastNoteD, 3);
                noteIsOnD = false;
            }

            // ---- 発音（かなり控えめ）----
            int velD = 28 * (mainDensity / 100.0f);
            velD = max(5, velD);
            midi_bridge_send_note_on(note, velD, 3);
            pushNoteDot(note);

            lastNoteD = note;
            noteIsOnD = true;

            // ---- NoteOff ----
            noteOffTimeD = now + (interval * 0.55);  // 少し短め
        }
    }

    // ---- D パート NoteOff ----
    if (noteIsOnD && now >= noteOffTimeD) {
        midi_bridge_send_note_off(lastNoteD, 3);
        noteIsOnD = false;
    }

    // =====================================================
    // アルペジオ更新
    // =====================================================
    arp_update(now);

    // =====================================================
    // UI
    // =====================================================
    static uint32_t lastUI = 0;
    if (now - lastUI >= 16) {
        drawUI();
        drawNoteDots(); 
        lastUI = now;
    }
}
