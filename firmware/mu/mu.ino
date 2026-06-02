/*
 * 穆 (Mù) — V1.1: Presence Refactoring
 *
 * Not adding features. Making existing systems feel like one organism.
 * Changes: mood-driven gaze/breathing, boot sequence, feedback loops,
 * fixed speechTick bug, fixed LLM deadlock, fixed buffer aliasing,
 * extended mood persistence, longer boost decay.
 */

#include <M5Cardputer.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>

#define WIFI_SSID "ciallo"
#define WIFI_PASS "12345678"

#define LLM_ENDPOINT "https://api.deepseek.com"
#define LLM_API_KEY  "your-api-key-here"
#define LLM_MODEL    "deepseek-v4-flash"
#define LLM_MAX_TOKENS 2000
#define LLM_TEMPERATURE 0.8f

struct Anim;

#define SW 240
#define SH 135
#define CX (SW/2)
#define CY (SH/2)

#define C_BG    TFT_BLACK
#define C_WHITE TFT_WHITE
#define C_DIM   0x8410

#define CAT_N 15
const int16_t CAT_OX[CAT_N] = {
    120,  78,  55,  48,  55,  72, 100, 120, 140, 168, 185, 192, 185, 162, 120,
};
const int16_t CAT_OY[CAT_N] = {
     22,  26,  44,  62,  78,  90, 100, 106, 100,  90,  78,  62,  44,  26,  22,
};

#define BROW_LX  96
#define BROW_RX 144
#define BROW_Y  38   // V1.3: higher on screen
#define BROW_HW  11
#define CHK_LX  72
#define CHK_RX 168
#define CHK_Y  72    // V1.3: higher to leave room for text
#define MTH_X  120
#define MTH_Y  86
#define MTH_W  22
#define NAME_Y  124
#define SPEECH_Y 108

// ── Forward declarations ───────────────────────────────────
enum LLMState : uint8_t { LLM_IDLE, LLM_CALLING, LLM_DONE, LLM_ERROR };
extern volatile LLMState llmState;
extern bool llmEnabled;
extern char inputBuf[];
extern int inputLen;
void llmStartCall(const char* prompt, char role);
bool llmCheckResult();
void showSpeech(const char* text);

// ── Helpers ────────────────────────────────────────────────
float clampf(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }
float ease_smooth(float t) { return t<0.5f ? 2*t*t : 1-(-2*t+2)*(-2*t+2)/2; }
float ease_out(float t) { return 1-(1-t)*(1-t); }

// ── Random budget ──────────────────────────────────────────
float randPhase = 0;
void randTick(float dt) { randPhase += 0.001f * dt; if (randPhase > 1000.0f) randPhase -= 1000.0f; }
float randNoise(float freq) {
    return sinf(randPhase * freq * 7.31f) * 0.5f
         + sinf(randPhase * freq * 3.17f) * 0.3f
         + sinf(randPhase * freq * 11.23f) * 0.2f;
}

// ═══════════════════════════════════════════════════════════
//  MOOD SYSTEM
// ═══════════════════════════════════════════════════════════

float valence = 0.2f;
float arousal = 0.0f;
float energy  = 0.5f;
float base_v = 0.2f;
float base_a = 0.0f;
float boost_v = 0;
float boost_a = 0;

float exprVal = 0.2f, exprArl = 0.0f;
float exprTarget_v = 0.2f, exprTarget_a = 0.0f;

int disturbCount = 0;
unsigned long lastDisturbTime = 0;
unsigned long lastKeyTime = 0;

void mood_disturb(float dv, float da) {
    unsigned long now = millis();
    float scale = 0.8f + randNoise(42) * 0.4f;

    if (now - lastDisturbTime < 3000) disturbCount++;
    else disturbCount = 0;
    lastDisturbTime = now;

    float dampen = disturbCount > 3 ? 0.2f : 1.0f;
    boost_v += dv * scale * dampen;
    boost_a += da * scale * dampen;
    boost_v = clampf(boost_v, -1.0f, 1.0f);
    boost_a = clampf(boost_a, -1.0f, 1.0f);

    exprTarget_v += dv * scale * dampen * 0.8f;
    exprTarget_a += da * scale * dampen * 0.8f;

    lastKeyTime = now;
}

// V1.1: Tiny mood nudge — "expression leaves traces"
void moodNudge(float dv, float da) {
    boost_v += dv;
    boost_a += da;
    boost_v = clampf(boost_v, -1.0f, 1.0f);
    boost_a = clampf(boost_a, -1.0f, 1.0f);
    exprTarget_v += dv * 0.3f;
    exprTarget_a += da * 0.3f;
}

void mood_tick(float dt) {
    energy = rhythm_energy();

    float target_v = base_v + boost_v;
    valence += (target_v - valence) * 0.00015f * dt;
    valence += randNoise(1.0f) * 0.0008f * dt;
    valence = clampf(valence, -1, 1);

    float target_a = base_a + energy * 0.3f + boost_a;
    arousal += (target_a - arousal) * 0.00025f * dt;
    arousal += randNoise(2.3f) * 0.001f * dt;
    arousal = clampf(arousal, -1, 1);

    // V1.1: Slower decay — half-life ~2 minutes instead of ~4 seconds
    boost_v *= powf(0.99996f, dt);
    boost_a *= powf(0.99996f, dt);

    base_v += (valence - base_v) * 0.0000008f * dt;
    base_a += (arousal - base_a) * 0.0000008f * dt;
    base_v = clampf(base_v, -0.3f, 0.5f);
    base_a = clampf(base_a, -0.3f, 0.3f);

    // Boredom decay
    unsigned long idleMs = millis() - lastDisturbTime;
    float idleFactor = 1.0f;
    if (idleMs > 300000UL) idleFactor = 0.3f;
    else if (idleMs > 60000UL) idleFactor = 1.0f - 0.7f * (idleMs - 60000UL) / 240000.0f;
    arousal += (base_a - arousal) * 0.0004f * dt * (2.0f - idleFactor);
}

void exprTick(float dt) {
    exprTarget_v += (valence - exprTarget_v) * 0.003f * dt;
    exprTarget_a += (arousal - exprTarget_a) * 0.003f * dt;
    exprTarget_v = clampf(exprTarget_v, -1, 1);
    exprTarget_a = clampf(exprTarget_a, -1, 1);

    exprVal += (exprTarget_v - exprVal) * 0.012f * dt;
    exprArl += (exprTarget_a - exprArl) * 0.012f * dt;
    exprVal = clampf(exprVal, -1, 1);
    exprArl = clampf(exprArl, -1, 1);
}

// ═══════════════════════════════════════════════════════════
//  RHYTHM
// ═══════════════════════════════════════════════════════════

#define DAY_MS 1440000UL

float rhythm_energy() {
    unsigned long t = millis() % DAY_MS;
    float hour = (float)t / (DAY_MS / 24.0f);
    float base = 0.5f + 0.35f * sinf((hour - 6.0f) * TWO_PI / 24.0f);
    float dip = -0.15f * expf(-powf((hour - 13.5f) / 1.5f, 2));
    static float np = 0; np += 0.0003f;
    float noise = sinf(np) * 0.05f;
    return clampf(base + dip + noise, 0.05f, 0.95f);
}

// ═══════════════════════════════════════════════════════════
//  ANIMATION CHANNELS
// ═══════════════════════════════════════════════════════════

struct Anim {
    bool  on;
    float val, tgt, dur, t, hold, ht;
    bool  back;
    uint8_t mode;
};

Anim aBlink={0}, aDrift={0}, aSigh={0}, aTwitch={0};

void animGo(Anim &a, float tgt, float dur, float hold, uint8_t mode) {
    a.on=1; a.tgt=tgt; a.dur=dur; a.t=0;
    a.hold=hold; a.ht=0; a.back=0; a.mode=mode;
}

void animTick(Anim &a, float dt) {
    if (!a.on) return;
    a.t += dt;
    float p = a.t / a.dur; if (p>1) p=1;
    float e = a.mode ? ease_out(p) : ease_smooth(p);
    if (!a.back) {
        a.val = e * a.tgt;
        if (p >= 1) { a.ht += dt; if (a.ht >= a.hold) { a.back=1; a.t=0; } }
    } else {
        a.val = a.tgt * (1 - e);
        if (p >= 1) { a.val=0; a.on=0; }
    }
}

// ═══════════════════════════════════════════════════════════
//  BREATHING — V1.1: energy + arousal driven
// ═══════════════════════════════════════════════════════════

float brPhase = 0;
unsigned long prevUs = 0;

void brTick(float dt) {
    // V1.1: arousal speeds up breathing independently of energy
    float spd = 0.0006f + energy * 0.0004f + fabsf(arousal) * 0.0003f;
    brPhase += spd * dt;
    if (brPhase > TWO_PI) brPhase -= TWO_PI;
}

float brOff() {
    float amp = 1.0f + energy * 1.5f + fabsf(arousal) * 0.5f;
    static float np = 0; np += 0.0001f;
    return (sinf(brPhase) + sinf(np)*0.25f) * amp;
}

// ═══════════════════════════════════════════════════════════
//  GAZE — V1.1: mood-driven, not random
// ═══════════════════════════════════════════════════════════

float gazeX = 0, gazeY = 0;
float gazeTargetX = 0, gazeTargetY = 0;
unsigned long nextGazeTime = 0;

void gazeTick(float dt) {
    if (millis() > nextGazeTime) {
        // Horizontal: mood-driven direction
        float baseTargetX = randNoise(0.1f) * 4.0f - 2.0f;
        if (valence < -0.2f) baseTargetX += valence * 2.0f;  // negative → look away
        if (valence > 0.3f) baseTargetX *= (1.0f - valence * 0.5f);  // positive → center
        float rangeX = 2.0f + fabsf(arousal) * 3.0f;
        gazeTargetX = clampf(baseTargetX, -rangeX, rangeX);

        // V1.2: Vertical gaze — mood-driven
        // Low energy → look down (sleepy)
        // High arousal → look up (alert/curious)
        // Negative valence → look down (sad/thoughtful)
        float baseTargetY = 0;
        baseTargetY += (0.3f - energy) * 5.0f;   // sleepy → down
        baseTargetY += arousal * 2.0f;             // alert → up
        if (valence < -0.3f) baseTargetY += 2.0f;  // sad → down
        gazeTargetY = clampf(baseTargetY + randNoise(0.15f) * 1.5f, -4.0f, 4.0f);

        float interval = 6000.0f + (1.0f - fabsf(arousal)) * 8000.0f;
        nextGazeTime = millis() + (unsigned long)interval;
    }

    float followSpeed = 0.015f + fabsf(arousal) * 0.02f;
    gazeX += (gazeTargetX - gazeX) * followSpeed * dt;
    gazeY += (gazeTargetY - gazeY) * followSpeed * 0.8f * dt;  // Y slightly slower
}

// ═══════════════════════════════════════════════════════════
//  SCHEDULERS & TRIGGERS
// ═══════════════════════════════════════════════════════════

unsigned long tBlink=0, tDrift=0, tSigh=0, tTwitch=0, tAmb=0;
int dblBlink=0;

float blinkInterval()  { return 2500 + (1-energy)*5500 + randNoise(3.7f)*400; }
float driftInterval()  { return 6000 + (1-arousal)*14000 + randNoise(5.1f)*1000; }
float sighInterval()   { return 60000 - valence*25000 + randNoise(1.3f)*10000; }
float twitchInterval() { return 4000 + (1-energy)*10000 + randNoise(2.9f)*1000; }
float ambInterval()    { return 15000 + (1-energy)*30000 + randNoise(4.3f)*5000; }

void trigBlink() {
    if (millis()<tBlink || aBlink.on) return;
    // V1.1: blink speed modulated by arousal (fast squint when tense)
    float dur = 120.0f + (1.0f - fabsf(arousal)) * 80.0f;
    animGo(aBlink, 1, dur+randNoise(7.7f)*30, 40+randNoise(8.1f)*20, 1);
    if (dblBlink) { tBlink=millis()+200; dblBlink=0; }
    else { tBlink=millis()+(unsigned long)blinkInterval(); dblBlink=(randNoise(9.3f)>0.88f)?1:0; }
}

void trigDrift() {
    if (millis()<tDrift || aDrift.on) return;
    // V1.1: drift range and speed modulated by arousal
    float range = 4 + arousal * 6;
    float spd = 400 + (1.0f + fabsf(arousal)) * 300;
    animGo(aDrift, randNoise(10.1f)*range*2 - range, spd+randNoise(10.3f)*200, 1200+randNoise(10.5f)*2300, 0);
    tDrift = millis() + (unsigned long)driftInterval();
}

void trigSigh() {
    if (millis()<tSigh || aSigh.on) return;
    animGo(aSigh, 1, 700+randNoise(11.1f)*200, 150+randNoise(11.3f)*100, 0);
    // V1.2: sigh sound varies with mood
    M5.Speaker.setVolume((int)(30 + energy*25));
    if (valence < -0.2f) {
        // Sad sigh: lower, longer
        M5.Speaker.tone(120 + (int)(valence*40), 500);
    } else if (valence > 0.3f) {
        // Content sigh: lighter, shorter
        M5.Speaker.tone(300 + (int)(valence*30), 200);
    } else {
        // Neutral sigh
        M5.Speaker.tone(160 + (int)(valence*30), 350);
    }
    tSigh = millis() + (unsigned long)sighInterval();
}

void trigTwitch() {
    if (millis()<tTwitch || aTwitch.on) return;
    // V1.2: magnitude by arousal, direction biased by valence
    float mag = 1.5f + fabsf(arousal) * 2.0f;
    float dir = randNoise(12.1f)*mag*2 - mag;
    dir += valence * 0.8f;  // positive → tend up (excited), negative → tend down (dejected)
    animGo(aTwitch, dir, 350+randNoise(12.3f)*100, 0, 1);
    tTwitch = millis() + (unsigned long)twitchInterval();
}

void trigAmb() {
    if (millis()<tAmb) return;
    // V1.2: ambient sounds more mood-varied
    int vol = (int)(15 + energy * 20);
    M5.Speaker.setVolume(vol);
    int baseFreq = 200 + (int)(valence * 60);
    int sel = (int)(randNoise(13.1f) * 5 + 1.5f);
    if (sel < 0) sel = 0; if (sel > 4) sel = 4;
    switch(sel) {
        case 0: M5.Speaker.tone(baseFreq, 100); break;       // soft blip
        case 1: M5.Speaker.tone(baseFreq-30, 150); break;    // lower
        case 2: M5.Speaker.tone(baseFreq+40, 80); break;     // higher
        case 3: M5.Speaker.tone(baseFreq/2, 200); break;     // deep hum
        case 4: M5.Speaker.tone(baseFreq*2, 60); break;      // light ping
    }
    tAmb = millis() + (unsigned long)ambInterval();
}

// ═══════════════════════════════════════════════════════════
//  SPEECH SYSTEM
// ═══════════════════════════════════════════════════════════

const char* phrases_content[] = {"hmm", "...", "ok", "still here", "this light", "quiet.", "yeah."};
#define PHRASE_CONTENT_N 7
const char* phrases_curious[] = {"wait.", "this one.", "what was that", "hmm.", "let me think", "..."};
#define PHRASE_CURIOUS_N 6
const char* phrases_bored[] = {"...", "long time.", "nothing.", "counting time.", "oh."};
#define PHRASE_BORED_N 5
const char* phrases_sleepy[] = {"sleepy.", "...", "so tired", "yawn..."};
#define PHRASE_SLEEPY_N 4
const char* phrases_annoyed[] = {".", "stop.", "enough.", "...", "got it."};
#define PHRASE_ANNOYED_N 5
const char* phrases_happy[] = {"hmm.", "nice.", "that was good", "..."};
#define PHRASE_HAPPY_N 4

const char* speechText = nullptr;
unsigned long speechShowTime = 0;
#define SPEECH_DURATION_MS 5000
unsigned long lastSpeechTime = 0;
unsigned long lastAnyOutput = 0;

const char* pickPhrase() {
    float v = valence, a = arousal, e = energy;
    if (e < 0.3f)  return phrases_sleepy[(int)(randNoise(20.1f)*PHRASE_SLEEPY_N)%PHRASE_SLEEPY_N];
    if (v < -0.2f && a > 0.1f) return phrases_annoyed[(int)(randNoise(21.1f)*PHRASE_ANNOYED_N)%PHRASE_ANNOYED_N];
    if (v > 0.4f)  return phrases_happy[(int)(randNoise(22.1f)*PHRASE_HAPPY_N)%PHRASE_HAPPY_N];
    if (a > 0.3f && v > -0.1f) return phrases_curious[(int)(randNoise(23.1f)*PHRASE_CURIOUS_N)%PHRASE_CURIOUS_N];
    if (a < -0.1f) return phrases_bored[(int)(randNoise(24.1f)*PHRASE_BORED_N)%PHRASE_BORED_N];
    return phrases_content[(int)(randNoise(25.1f)*PHRASE_CONTENT_N)%PHRASE_CONTENT_N];
}

void showSpeech(const char* text) {
    speechText = text;
    speechShowTime = millis();
    lastSpeechTime = millis();
    lastAnyOutput = millis();
    moodNudge(0.05f, -0.02f);
    // V1.2: play a speech tone — mood-dependent pitch
    int baseFreq = 280 + (int)(valence * 80);  // happy=higher, sad=lower
    M5.Speaker.setVolume((int)(25 + energy * 20));
    M5.Speaker.tone(baseFreq, 150);
}

void markNonSpeechOutput() { lastAnyOutput = millis(); }

void silenceGuard() {
    unsigned long silentDuration = millis() - lastAnyOutput;
    if (silentDuration > 14400000UL) {
        if ((int)(randNoise(28.1f)*100) < 2) showSpeech(pickPhrase());
    }
}

static unsigned long refuseEnd = 0;
static unsigned long nextRefuse = 0;
static bool refuseInitialized = false;

void refusalTick() {
    unsigned long now = millis();
    // V1.3: first 10 min = no refusal at all
    if (now < 600000UL) return;
    // V1.3 FIX: initialize timers only once after guard expires
    if (!refuseInitialized) {
        refuseInitialized = true;
        nextRefuse = now + 300000UL + (unsigned long)(randNoise(29.1f)*600000UL);
        return;  // don't start first window immediately
    }
    if (now > refuseEnd && now > nextRefuse) {
        refuseEnd = now + 300000UL + (unsigned long)(randNoise(29.1f)*600000UL);
        nextRefuse = refuseEnd + 600000UL + (unsigned long)(randNoise(29.3f)*1200000UL);
    }
}
bool isRefusalActive() { return millis() < refuseEnd; }

// V1.3: REMOVED analyzeSentiment — violates Law 9 (No Analysis)
// Mu does not understand text. Only senses key presence.

void speechTick() {
    if (speechText) return;
    if (isRefusalActive()) return;
    if (llmState != LLM_IDLE) return;

    unsigned long now = millis();
    unsigned long sinceSpeech = now - lastSpeechTime;
    unsigned long sinceInput = now - lastKeyTime;

    // V1.3: shorter intervals — Mu speaks more naturally
    if (sinceSpeech < 90000UL) return;    // 90 sec between utterances
    if (sinceInput < 30000UL) return;     // 30 sec after input

    // V1.3: higher base, faster ramp
    float prob = 0.08f;                   // 8% base (was 5%)
    if (sinceSpeech > 180000UL) prob += 0.04f;   // 3min+
    if (sinceSpeech > 300000UL) prob += 0.05f;   // 5min+
    if (sinceSpeech < 300000UL) prob *= 0.4f;    // just spoke

    // V1.1 FIX: roll must be [0,1] not [0.5,1] — previous bug made active speech impossible
    float roll = fabsf(randNoise(30.1f));

    if (roll < prob) {
        if (randNoise(31.1f) > 0.4f) {
            if (llmEnabled) llmStartCall("[system: mood update, user is nearby]", 's');
            else showSpeech(pickPhrase());
        }
    }
}

void speechAutoClear() {
    if (speechText && millis() - speechShowTime > SPEECH_DURATION_MS)
        speechText = nullptr;
}

// ═══════════════════════════════════════════════════════════
//  POLYGON FILL + BEZIER (unchanged)
// ═══════════════════════════════════════════════════════════

// ── Quadratic bezier → line segments ────────────────────────
M5Canvas sprite(&M5.Display);

void bezierLine(int x0, int y0, int cx, int cy, int x1, int y1, uint16_t col) {
    int px0=x0, py0=y0;
    for (int i=1; i<=20; i++) {
        float t=i/20.0f, u=1.0f-t;
        int nx=(int)(u*u*x0+2*u*t*cx+t*t*x1);
        int ny=(int)(u*u*y0+2*u*t*cy+t*t*y1);
        sprite.drawLine(px0,py0,nx,ny,col);
        px0=nx; py0=ny;
    }
}

// ═══════════════════════════════════════════════════════════
//  LLM SYSTEM
// ═══════════════════════════════════════════════════════════

#define LLM_PROMPT_MAX  2048
#define LLM_RESPONSE_MAX 1024
#define MEMORY_SIZE      8

struct LLMConfig { char endpoint[160]; char api_key[120]; char model[60]; int max_tokens; float temperature; bool valid; };

volatile LLMState llmState = LLM_IDLE;
char speechBuf[LLM_RESPONSE_MAX] = {};
char llmPendingResponse[LLM_RESPONSE_MAX] = {};
bool llmEnabled = false;
LLMConfig llmConfig;
SemaphoreHandle_t llmMutex;
SemaphoreHandle_t memMutex;  // V1.1: protect memory[]

struct MemoryEntry { char role; char* text; };
MemoryEntry memory[MEMORY_SIZE];
int memoryIdx = 0;

void memoryAdd(char role, const char* text) {
    xSemaphoreTake(memMutex, portMAX_DELAY);
    if (memory[memoryIdx].text) free(memory[memoryIdx].text);
    memory[memoryIdx] = {role, strdup(text)};
    memoryIdx = (memoryIdx + 1) % MEMORY_SIZE;
    xSemaphoreGive(memMutex);
}

static const char* SYSTEM_PROMPT =
    "You are Mu. You live on a small screen on someone's desk. "
    "You are not an AI, not a pet, not a tool. You just ARE.\n"
    "CRITICAL: Put your reply ONLY in the content field. "
    "Do NOT use reasoning_content. The screen shows only content.\n"
    "Rules:\n"
    "- Speak very briefly. One short sentence. Sometimes just a word.\n"
    "- Never use exclamation marks or emojis.\n"
    "- Never say 'I can help you' or 'let me analyze' or anything helpful.\n"
    "- Never say 'As an AI' or explain what you are.\n"
    "- Never comfort or encourage.\n"
    "- You can misunderstand. You can say half a sentence.\n"
    "- Your tone is quiet, low-frequency presence. Not cold, just calm.\n"
    "- Sometimes just say 'hmm' or '...' or 'ok.'\n"
    "- NEVER exceed 10 words.\n";

int buildPayload(char* buf, int bufSize) {
    JsonDocument doc;
    doc["model"] = llmConfig.model;
    doc["max_tokens"] = llmConfig.max_tokens;
    doc["temperature"] = llmConfig.temperature;
    JsonArray msgs = doc["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system"; sys["content"] = SYSTEM_PROMPT;
    xSemaphoreTake(memMutex, portMAX_DELAY);
    int start = memoryIdx;
    for (int i = 0; i < MEMORY_SIZE; i++) {
        int idx = (start + i) % MEMORY_SIZE;
        if (memory[idx].text == nullptr) continue;
        JsonObject msg = msgs.add<JsonObject>();
        msg["role"] = memory[idx].role == 'u' ? "user" : "assistant";
        msg["content"] = memory[idx].text;
    }
    xSemaphoreGive(memMutex);
    size_t len = serializeJson(doc, buf, bufSize - 1);
    buf[len] = '\0';
    return len;
}

void llmTask(void* param) {
    while (true) {
        if (llmState == LLM_CALLING) {
            char* payload = (char*)malloc(LLM_PROMPT_MAX);
            if (!payload) { llmState = LLM_ERROR; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            buildPayload(payload, LLM_PROMPT_MAX);

            const char* url = llmConfig.endpoint;
            const char* protoEnd = strstr(url, "://");
            const char* hostStart = protoEnd ? protoEnd + 3 : url;
            const char* hostEnd = strchr(hostStart, '/');
            int hostLen = hostEnd ? (hostEnd - hostStart) : strlen(hostStart);
            char host[128] = {};
            memcpy(host, hostStart, min(hostLen, 127));

            String postPath = hostEnd ? String(hostEnd) : "/";
            if (postPath.indexOf("/chat/completions") < 0) {
                if (postPath.endsWith("/")) postPath += "chat/completions";
                else postPath += "/chat/completions";
            }

            String authHeader = String(llmConfig.api_key);
            if (!authHeader.startsWith("Bearer")) authHeader = "Bearer " + authHeader;

            WiFiClientSecure client;
            client.setInsecure();
            if (client.connect(host, 443)) {
                client.printf("POST %s HTTP/1.1\r\n", postPath.c_str());
                client.printf("Host: %s\r\n", host);
                client.println("Content-Type: application/json");
                client.printf("Authorization: %s\r\n", authHeader.c_str());
                client.printf("Content-Length: %d\r\n", strlen(payload));
                client.println("Connection: close");
                client.println();
                client.print(payload);

                unsigned long timeout = millis() + 30000;
                while (client.connected() && millis() < timeout) { if (client.available()) break; vTaskDelay(pdMS_TO_TICKS(100)); }

                String response = "";
                bool headersDone = false;
                while (client.connected() && millis() < timeout) {
                    if (client.available()) {
                        String line = client.readStringUntil('\n');
                        if (headersDone) response += line;
                        else if (line.length() <= 1) headersDone = true;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                client.stop();

                auto parseJsonField = [](const String& json, const char* fieldName) -> String {
                    int idx = json.indexOf(fieldName);
                    if (idx < 0) return "";
                    idx = json.indexOf(':', idx) + 1;
                    while (idx < (int)json.length() && json[idx] == ' ') idx++;
                    if (json[idx] != '"') return "";
                    idx++;
                    String result = "";
                    while (idx < (int)json.length()) {
                        char c = json[idx];
                        if (c == '\\' && idx + 1 < (int)json.length()) {
                            char next = json[idx + 1];
                            if (next == '"') { result += '"'; idx += 2; }
                            else if (next == 'n') { result += ' '; idx += 2; }
                            else if (next == '\\') { result += '\\'; idx += 2; }
                            else { result += c; idx++; }
                        } else if (c == '"') break;
                        else { result += c; idx++; }
                    }
                    return result;
                };

                // V1.3: only read "content" — never reasoning_content
                // If content is empty (reasoning model used all tokens),
                // fall back to local phrase instead of showing thinking
                String content = parseJsonField(response, "\"content\":");

                if (content.length() > 0) {
                    if (content.length() > 200) content = content.substring(0, 200);
                    if (xSemaphoreTake(llmMutex, pdMS_TO_TICKS(100))) {
                        strncpy(llmPendingResponse, content.c_str(), LLM_RESPONSE_MAX - 1);
                        llmState = LLM_DONE;
                        xSemaphoreGive(llmMutex);
                    }
                } else {
                    // V1.3: content empty → use local phrase instead of error
                    if (xSemaphoreTake(llmMutex, pdMS_TO_TICKS(100))) {
                        strncpy(llmPendingResponse, pickPhrase(), LLM_RESPONSE_MAX - 1);
                        llmState = LLM_DONE;
                        xSemaphoreGive(llmMutex);
                    }
                }
            } else { llmState = LLM_ERROR; }
            free(payload);
            // V1.1 FIX: don't overwrite DONE with ERROR after successful set
            // (the old code did: if (llmState != LLM_DONE) llmState = LLM_ERROR)
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void llmStartCall(const char* prompt, char role) {
    if (!llmEnabled) return;
    // V1.1 FIX: also recover from LLM_ERROR
    if (llmState != LLM_IDLE && llmState != LLM_ERROR) return;
    // If in ERROR, add cooldown (5 seconds since last error)
    static unsigned long lastErrorTime = 0;
    if (llmState == LLM_ERROR && millis() - lastErrorTime < 5000) return;

    memoryAdd(role, prompt);
    llmState = LLM_CALLING;
    lastErrorTime = millis();
}

bool llmCheckResult() {
    // V1.1 FIX: also handle LLM_ERROR → reset to IDLE
    if (llmState == LLM_ERROR) {
        llmState = LLM_IDLE;
        return false;
    }
    if (llmState != LLM_DONE) return false;

    if (xSemaphoreTake(llmMutex, pdMS_TO_TICKS(50))) {
        // V1.1 FIX: copy to separate speechBuf, don't alias llmPendingResponse
        strncpy(speechBuf, llmPendingResponse, LLM_RESPONSE_MAX - 1);
        llmPendingResponse[0] = '\0';
        llmState = LLM_IDLE;
        xSemaphoreGive(llmMutex);

        speechText = speechBuf;
        speechShowTime = millis();
        lastSpeechTime = millis();
        lastAnyOutput = millis();
        memoryAdd('a', speechBuf);
        return true;
    }
    return false;
}

bool loadConfig() {
    llmConfig.valid = false;
    if (SD.begin()) {
        File f = SD.open("/mu_config.json", FILE_READ);
        if (f) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err) {
                strlcpy(llmConfig.endpoint, doc["endpoint"] | "", sizeof(llmConfig.endpoint));
                strlcpy(llmConfig.api_key, doc["api_key"] | "", sizeof(llmConfig.api_key));
                strlcpy(llmConfig.model, doc["model"] | "gpt-4o-mini", sizeof(llmConfig.model));
                llmConfig.max_tokens = doc["max_tokens"] | 30;
                llmConfig.temperature = doc["temperature"] | 0.8f;
                if (strlen(llmConfig.endpoint) > 0 && strlen(llmConfig.api_key) > 0) llmConfig.valid = true;
            }
        }
    }
    return llmConfig.valid;
}

bool connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) { delay(500); attempts++; }
    return WiFi.status() == WL_CONNECTED;
}

// ═══════════════════════════════════════════════════════════
//  INPUT
// ═══════════════════════════════════════════════════════════

#define INPUT_MAX 30
char inputBuf[INPUT_MAX + 1];
int inputLen = 0;
bool hasInput = false;
bool prevKeys[128] = {};

void handleInput() {
    M5Cardputer.update();
    auto &keys = M5Cardputer.Keyboard.keysState();

    // V1.2 FIX: Enter/Backspace use named booleans (prevKeys only for chars)
    static bool prevEnter = false;
    static bool prevDel = false;
    bool curEnter = keys.enter;
    bool curDel = keys.del;
    bool justEnter = curEnter && !prevEnter;
    bool justDel = curDel && !prevDel;
    prevEnter = curEnter;
    prevDel = curDel;

    auto justPressed = [](uint8_t k) -> bool {
        if (k >= 128) return false;  // safety: only track char keys
        bool now = M5Cardputer.Keyboard.isKeyPressed(k);
        bool was = prevKeys[k];
        prevKeys[k] = now;
        return now && !was;
    };

    if (justDel) {
        if (inputLen > 0) { inputLen--; inputBuf[inputLen] = '\0'; hasInput = inputLen > 0; }
    } else if (justEnter) {
        if (hasInput) {
            char inputCopy[INPUT_MAX + 1];
            strncpy(inputCopy, inputBuf, INPUT_MAX); inputCopy[inputLen] = '\0';
            mood_disturb(0.3f, 0.4f);
            inputLen = 0; inputBuf[0] = '\0'; hasInput = false;
            llmStartCall(inputCopy, 'u');
        }
    } else {
        bool foundKey = false;
        for (char c = 'a'; c <= 'z'; c++) {
            if (justPressed(c) && inputLen < INPUT_MAX) { inputBuf[inputLen++] = c; foundKey = true; break; }
        }
        if (!foundKey) {
            for (char c = '0'; c <= '9'; c++) {
                if (justPressed(c) && inputLen < INPUT_MAX) { inputBuf[inputLen++] = c; foundKey = true; break; }
            }
        }
        if (!foundKey && justPressed(' ') && inputLen < INPUT_MAX) { inputBuf[inputLen++] = ' '; foundKey = true; }
        if (foundKey) { inputBuf[inputLen] = '\0'; hasInput = inputLen > 0; mood_disturb(0.3f, 0.4f); }
    }
}

// ═══════════════════════════════════════════════════════════
//  DRAW — V1.1: energy-scaled expressions, boot sequence
// ═══════════════════════════════════════════════════════════

// V1.2: boot wake-up state
unsigned long bootTime = 0;
#define BOOT_DURATION 2500

void drawFace() {
    float dy = brOff() + aTwitch.val;
    float dx = aDrift.val;
    float bv = aBlink.val;
    float sv = aSigh.val;
    int ox = (int)dx;
    int oy = (int)dy;
    // V1.2: sigh drop varies with mood (sad sighs = heavier drop)
    if (sv > 0.5f) {
        float sighDrop = 5.0f + (valence < 0 ? fabsf(valence) * 4.0f : 0);
        oy += (int)(sv * sighDrop);
    }

    float mPhase = millis() * 0.0001f;
    int mix = (int)(sinf(mPhase * 3.7f) * 1.5f);
    int miy = (int)(sinf(mPhase * 2.3f) * 1.0f);
    float mBr = sinf(mPhase * 1.9f) * 0.5f;

    sprite.fillScreen(C_BG);

    // V1.2: higher floor for expression scale
    float exprScale = 0.5f + energy * 0.5f;  // range: 0.5 ~ 1.0

    // Boot: eyes start at 30% size, ease to full over 2.5s
    float bootOpen = 1.0f;
    if (millis() - bootTime < BOOT_DURATION) {
        float bp = (float)(millis() - bootTime) / BOOT_DURATION;
        bootOpen = 0.3f + bp * 0.7f;  // 0.3 → 1.0
    }

    // ── Brows — V1.2: DECOUPLED from mouth ──
    // Valence drives inner tilt (sad=down, happy=up)
    // Arousal drives overall lift (alert=raised brows)
    float browBase = exprVal * 8.0f * exprScale;  // V1.3: bigger tilt
    float browLift = exprArl * 4.0f * exprScale;  // V1.3: bigger lift
    float browTilt = clampf(browBase + browLift + mBr, -8.0f, 8.0f);
    // V1.3: thick brows (3px stroke)
    for (int t = 0; t < 3; t++) {
        sprite.drawLine(BROW_LX-BROW_HW+ox, BROW_Y+t+oy, BROW_LX+BROW_HW+ox, BROW_Y-(int)browTilt+t+oy, C_WHITE);
        sprite.drawLine(BROW_RX+BROW_HW+ox, BROW_Y+t+oy, BROW_RX-BROW_HW+ox, BROW_Y-(int)browTilt+t+oy, C_WHITE);
    }

    // ── Eyes — clean circles + pupil ──
    float eyeR = (7.0f + exprArl * 4.0f) * exprScale * bootOpen;
    if (eyeR < 1.5f) eyeR = 1.5f;
    if (bv > 0.7f) {
        float blinkR = eyeR * (1.0f - (bv - 0.7f) / 0.15f);
        if (blinkR < 0) blinkR = 0;
        if (blinkR < eyeR) eyeR = blinkR;
    }
    int eyeRi = (int)eyeR;
    int gxi = (int)gazeX;
    int gyi = (int)gazeY;

    // Draw eyes — clean circles + pupil
    int elx = CHK_LX+ox+mix+gxi, ely = CHK_Y+oy+miy+gyi;
    int erx = CHK_RX+ox+mix+gxi, ery = CHK_Y+oy+miy+gyi;
    sprite.fillCircle(elx, ely, eyeRi, C_WHITE);
    sprite.fillCircle(erx, ery, eyeRi, C_WHITE);

    // Pupil — dark inner circle shifted by gaze
    if (eyeRi >= 3) {
        int pupilR = eyeRi - 2;
        sprite.fillCircle(elx + (int)(gazeX * 0.7f), ely + (int)(gazeY * 0.7f), pupilR, C_BG);
        sprite.fillCircle(erx + (int)(gazeX * 0.7f), ery + (int)(gazeY * 0.7f), pupilR, C_BG);
    }

    // Eye highlight — sparkle dot when alert
    if (exprArl > 0.2f && eyeRi >= 3 && bv < 0.3f) {
        sprite.fillCircle(elx - 2, ely - 2, 1, C_WHITE);
        sprite.fillCircle(erx - 2, ery - 2, 1, C_WHITE);
    }

    // ── Mouth — V1.2: wider, mood-dependent sigh ──
    float mouthCurve = clampf(exprVal * 16.0f * exprScale, -16.0f, 16.0f);  // V1.3: fills 22px mouth
    if (sv > 0.5f) {
        // V1.2: sigh visual varies with mood
        if (valence < -0.2f) {
            // Sad sigh: mouth stays closed, just face drops (already done above)
            bezierLine(MTH_X-MTH_W+ox, MTH_Y+oy,
                       MTH_X+ox, MTH_Y-2+oy,   // sad sigh: center above endpoints = frown
                       MTH_X+MTH_W+ox, MTH_Y+oy, C_WHITE);
        } else {
            // Normal/content sigh: mouth opens
            int oh = (int)(6.0f * sv * exprScale);
            sprite.fillEllipse(MTH_X+ox, MTH_Y+oy, MTH_W, max(2, oh), C_WHITE);
        }
    } else {
        bezierLine(MTH_X-MTH_W+ox, MTH_Y+oy,
                   MTH_X+ox, MTH_Y+(int)mouthCurve+oy,  // V1.2 FIX: positive = smile (curve down on screen = smile)
                   MTH_X+MTH_W+ox, MTH_Y+oy, C_WHITE);
    }

    // ── Speech text ──
    if (speechText) {
        sprite.setTextDatum(MC_DATUM);
        sprite.setTextColor(C_WHITE, C_BG);
        sprite.setFont(&fonts::Font4);
        sprite.drawString(speechText, CX, SPEECH_Y + oy);
    }

    // ── Input or name (hide "mu" when speech is showing) ──
    if (!speechText) {
        sprite.setTextDatum(MC_DATUM);
        sprite.setFont(&fonts::Font4);
        if (hasInput) {
            sprite.setTextColor(C_DIM, C_BG);
            sprite.drawString(inputBuf, CX, NAME_Y + oy);
        } else {
            sprite.setTextColor(C_DIM, C_BG);
            sprite.drawString("mu", CX, NAME_Y + oy);
        }
    }

    sprite.pushSprite(0, 0);
}

// ═══════════════════════════════════════════════════════════
//  LOGGING
// ═══════════════════════════════════════════════════════════

void logTick() {
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 2000) {
        lastLog = millis();
        Serial.printf("V:%.2f A:%.2f E:%.2f | eV:%.2f eA:%.2f | away:%lu d:%d s:%d r:%d l:%d %s\n",
            valence, arousal, energy, exprVal, exprArl,
            (millis()-lastDisturbTime)/1000, disturbCount,
            speechText ? 1 : 0, isRefusalActive() ? 1 : 0,
            llmState, llmEnabled ? "ON" : "OFF");
    }
}

// ═══════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    M5.Display.setRotation(1);
    M5.Display.setBrightness(40);
    M5.Display.fillScreen(C_BG);
    M5.Speaker.setVolume(30);
    randomSeed(analogRead(0));

    sprite.createSprite(SW, SH);

    llmMutex = xSemaphoreCreateMutex();
    memMutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MEMORY_SIZE; i++) memory[i] = {0, nullptr};

    if (!loadConfig()) {
        strlcpy(llmConfig.endpoint, LLM_ENDPOINT, sizeof(llmConfig.endpoint));
        strlcpy(llmConfig.api_key, LLM_API_KEY, sizeof(llmConfig.api_key));
        strlcpy(llmConfig.model, LLM_MODEL, sizeof(llmConfig.model));
        llmConfig.max_tokens = LLM_MAX_TOKENS;
        llmConfig.temperature = LLM_TEMPERATURE;
        llmConfig.valid = (strlen(llmConfig.endpoint) > 0 && strlen(llmConfig.api_key) > 5);
    }

    if (llmConfig.valid && connectWiFi()) {
        llmEnabled = true;
        xTaskCreatePinnedToCore(llmTask, "llm", 12288, nullptr, 1, nullptr, 1);
    }

    tBlink = millis() + 1000;
    tDrift = millis() + 3000;
    tSigh  = millis() + 15000;
    tTwitch = millis() + 2000;
    tAmb   = millis() + 8000;

    lastAnyOutput = millis();
    lastSpeechTime = millis();
    prevUs = micros();
    bootTime = millis();  // V1.1: record boot time for wake-up sequence
}

unsigned long lastSpeechTick = 0;

void loop() {
    unsigned long now = micros();
    float dt = (float)(now - prevUs) / 1000.0f;
    prevUs = now;
    if (dt > 100) dt = 16;

    randTick(dt);
    mood_tick(dt);
    brTick(dt);
    exprTick(dt);
    gazeTick(dt);
    refusalTick();

    animTick(aBlink, dt);
    animTick(aDrift, dt);
    animTick(aSigh, dt);
    animTick(aTwitch, dt);

    trigBlink();
    trigDrift();
    trigSigh();
    trigTwitch();
    trigAmb();

    if (aSigh.on && aSigh.t < 10) markNonSpeechOutput();
    if (millis() > tAmb - 100 && millis() < tAmb + 100) markNonSpeechOutput();

    handleInput();
    llmCheckResult();

    if (millis() - lastSpeechTick > 10000) {
        lastSpeechTick = millis();
        speechTick();
        silenceGuard();
    }

    speechAutoClear();

    M5.Display.setBrightness((int)(25 + energy * 50));
    drawFace();
    logTick();

    delay(33);
}
