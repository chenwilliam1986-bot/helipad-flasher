// 09 发布版：给别人用的一个固件，不带 WiFi，不用调试。
// 第一次上电：两只舵机转到装配角停住，小灯快闪 → 把右眼、曲柄竖直装上，拧好 → 按一下翘板 → 结束。
//   按完后两只回中位、不出力、亮灯；在中位装长刃和摇杆。之后就是正常使用：放上鼠标收拢灭灯，拿起鼠标立起亮灯。
// 正常使用的逻辑、灯效跟 08_main 一样（整体电路方案-v04 §9），两头位置用样机上调好的数（长刃立起 +9°）。
// 说明见同文件夹 说明.md；给别人的烧录包在 helipad/电路/烧录包/。
//
// 接线（v04 §2）：GPIO3 长刃舵机、GPIO4 右眼舵机、GPIO0 重力微动（NO 接 GPIO0、C 接 G）、
//   GPIO6 左臂灯带、GPIO7 右眼灯带、GPIO10 底座灯带。
// 急停：舵机在动时按板上 BOOT 键 → 两只立刻不出力，重新插电恢复。
// 重新装零件：平时按住 BOOT 键 5 秒 → 小灯快闪、清掉“已装好”。拆下曲柄和右眼后重新插电，回到第一次上电的流程。
// 插着电脑时不动舵机（整机别从电脑口取电），串口发 g 才继续。
//
// 开发板：esp32:esp32:esp32c3:CDCOnBoot=cdc。Adafruit NeoPixel 1.15.5。

#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

// ---------- 引脚 ----------
const int PIN_KEY = 0;     // 重力微动（翘板）：压下 = 低电平 = 鼠标在
const int PIN_BOOT = 9;    // 板上 BOOT 键
const int PIN_BLED = 8;    // 板载蓝灯，低电平亮
const int SPIN[2] = {3, 4};   // 0 长刃，1 右眼

// ---------- 灯带 ----------
const int NS[3] = {14, 20, 118};               // 左臂、右眼、底座颗数（2026-11-15 样机实测）
const int NMAXPX = 118;
const int NFX = 6;         // 0 常亮红，1 呼吸红，2 红色流光，3 彩虹循环，4 眼睛，5 贯穿流星
const uint32_t INTRO_MS = 1500;
const uint32_t FADE_MS = 500;
const float LED_MA_FULL = 12.0f;               // 每个颜色满值按 12 mA 算（样机实测红色约 9–10.5 mA/颗）
const float LED_MA_IDLE = 0.7f;
const float LED_LIMIT_MA = 2000.0f;            // 三条合计上限 2.0 A

// ---------- 舵机 ----------
const int US_CENTER = 1500;
const int US_MIN = 500, US_MAX = 2500;
const float US_PER_DEG = 2000.0f / 180.0f;     // 每度约 11.1 µs
const int STAND_SIGN = -1;                     // 立起（从前面看顺时针）= 脉宽减小（样机确认）
const float ASM_DEG[2] = {52.3f, 17.2f};       // 装配角：从中位往立起那边。长刃 = 曲柄臂竖直朝上；右眼 = 2 号块长直边竖直
const float STAND_DEG[2] = {24.6f + 9.0f, 51.4f};   // 立起位离中位多少度（长刃 +9° 是样机上调出来的）
const float FOLD_DEG[2] = {24.6f, 51.4f};      // 收拢位离中位多少度
const int MOVE_MS = 2000;                      // 立起 / 收拢全程用时
const float STAGGER = 0.5f;                    // 后动的那只在先动的走到一半时起步

// ---------- 翘板时间 ----------
const uint32_t DEBOUNCE_MS = 30;
const uint32_t PUT_MS = 1000;                  // 压满 1 秒 = 放了鼠标 → 收拢
const uint32_t LIFT_MS = 300;                  // 松开满 0.3 秒 = 拿起 → 立起
const uint32_t CLICK_MAX_MS = 400;
const uint32_t CLICK_GAP_MS = 500;
const uint32_t AFTER_MOVE_MS = 500;
const uint32_t REDO_HOLD_MS = 5000;            // BOOT 按住 5 秒 = 重新装零件

// ---------- 存在板子里的 ----------
Preferences prefs;
bool assembled = false;    // 曲柄、右眼装好了
bool poseOpen = false;     // 上次停在立起(true) / 收拢(false)
bool lightOn = true;
int fx = 5;                // 灯效，默认贯穿流星
const int BRI = 120;       // 亮度 0–255

void loadPrefs() {
  prefs.begin("rel09", true);
  assembled = prefs.getBool("asm", false);
  poseOpen = prefs.getBool("pose", false);
  lightOn = prefs.getBool("light", true);
  fx = constrain(prefs.getInt("fx", 5), 0, NFX - 1);
  prefs.end();
}
void savePrefs() {
  prefs.begin("rel09", false);
  prefs.putBool("asm", assembled); prefs.putBool("pose", poseOpen);
  prefs.putBool("light", lightOn); prefs.putInt("fx", fx);
  prefs.end();
}

// ================= 灯带 =================
Adafruit_NeoPixel stripL(NS[0], 6, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripR(NS[1], 7, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripB(NS[2], 10, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel* strips[3] = {&stripL, &stripR, &stripB};
uint8_t px[3][NMAXPX][3];
bool ledsBlack = false;
bool intro = false, introNext = false;
uint32_t introT0 = 0, fadeT0 = 0;
bool moving = false;

// 唯一的发数据出口：按 RGB 总和估电流，超 2.0 A 整体按比例调暗
void ledShow() {
  float colorMa = 0, idleMa = 0;
  for (int s = 0; s < 3; s++) for (int i = 0; i < NS[s]; i++) {
    colorMa += (px[s][i][0] + px[s][i][1] + px[s][i][2]) / 255.0f * LED_MA_FULL;
    idleMa += LED_MA_IDLE;
  }
  float k = 1.0f;
  if (colorMa + idleMa > LED_LIMIT_MA && colorMa > 0) k = (LED_LIMIT_MA - idleMa) / colorMa;
  for (int s = 0; s < 3; s++) {
    for (int i = 0; i < NS[s]; i++)
      strips[s]->setPixelColor(i, (uint8_t)(px[s][i][0] * k), (uint8_t)(px[s][i][1] * k), (uint8_t)(px[s][i][2] * k));
    strips[s]->show();
  }
}
void ledBlack() { memset(px, 0, sizeof(px)); ledShow(); ledsBlack = true; }

float rootFrac(int s, int i) { return NS[s] > 1 ? (float)i / (NS[s] - 1) : 0; }   // 离进线那头多远（0–1）
// 贯穿流星的路径：长刃（尖 → 根）→ 右眼 → 底座（起点 → 终点）
int pathPos(int s, int i) {
  if (s == 0) return NS[0] - 1 - i;
  return (s == 1 ? NS[0] : NS[0] + NS[1]) + i;
}
// 一颗灯在当前灯效下的颜色（0–255，还没乘亮度）。s：0 长刃，1 右眼，2 底座
void fxColor(int s, int i, uint32_t t, uint8_t* r, uint8_t* g, uint8_t* b) {
  int n = NS[s];
  float R = 255, G = 0, B = 0;                     // 0 常亮红
  if (fx == 1) {                                   // 呼吸红：只有底座呼吸
    float a = (s == 2) ? 0.15f + 0.85f * (0.5f - 0.5f * cosf(2 * PI * (t % 4000) / 4000.0f)) : 0.8f;
    R = 255 * a;
  } else if (fx == 2) {                            // 红色流光
    float a = 0;
    if (s == 2) {
      int p = (t / 18) % n;
      int d1 = (p - i + n) % n, d2 = (i - (n - 1 - p) + n) % n;
      if (d1 < 12) a = 1 - d1 / 12.0f;
      if (d2 < 12) a = max(a, 1 - d2 / 12.0f);
      R = 40 + 215 * a; G = 60 * a; B = 40 * a;
    } else if (s == 0) {
      float h = (t % 2500) / 600.0f * 1.3f, d = h - rootFrac(s, i);
      if (d >= 0 && d < 0.3f) a = 1 - d / 0.3f;
      R = 90 + 165 * a; G = 70 * a; B = 50 * a;
    } else R = 150;
  } else if (fx == 3) {                            // 彩虹循环
    uint16_t hue = (s == 2) ? (uint16_t)(i * 65536UL / n + t * 12) : (uint16_t)(t * 6 + s * 9000);
    uint32_t c = Adafruit_NeoPixel::ColorHSV(hue, 255, 255);
    R = (c >> 16) & 255; G = (c >> 8) & 255; B = c & 255;
  } else if (fx == 4) {                            // 眼睛：右眼每 5 秒眨两下
    if (s != 1) R = 90;
    else { uint32_t tt = t % 5000; R = (tt < 120 || (tt >= 240 && tt < 360)) ? 20 : 255; }
  } else if (fx == 5) {                            // 贯穿流星
    int total = NS[0] + NS[1] + NS[2];
    int head = (t % ((uint32_t)(total + 16) * 12 + 800)) / 12;
    int d = head - pathPos(s, i);
    float a = (d >= 0 && d < 16) ? 1 - d / 16.0f : 0;
    R = 12 + 243 * a; G = 170 * a * a; B = 120 * a * a;
  }
  *r = (uint8_t)R; *g = (uint8_t)G; *b = (uint8_t)B;
}
// 立起开场（1.5 秒）：底座从后面两头往前流到正前方 → 长刃从根亮到尖 → 右眼亮
float introMask(int s, int i, uint32_t it) {
  float m;
  if (s == 2) m = (it / 900.0f * 1.15f - min(i, NS[2] - 1 - i) / (NS[2] / 2.0f)) * 8;
  else if (s == 0) m = (((float)it - 900) / 400.0f * 1.2f - rootFrac(0, i)) * 6;
  else m = ((float)it - 1200) / 300.0f;
  return constrain(m, 0.0f, 1.0f);
}
// 每 30 ms 刷一帧。只有“立起 + 灯开 + 舵机没在动”才亮
void ledTick() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 30) return;
  last = now;
  if (moving || !poseOpen || !lightOn) { if (!ledsBlack) ledBlack(); return; }
  if (ledsBlack) {
    ledsBlack = false; fadeT0 = now;
    if (introNext) { introNext = false; intro = true; introT0 = now; }
  }
  if (intro && now - introT0 >= INTRO_MS) intro = false;
  float k = (intro ? 1.0f : min(1.0f, (now - fadeT0) / (float)FADE_MS)) * BRI / 255.0f;
  for (int s = 0; s < 3; s++) for (int i = 0; i < NS[s]; i++) {
    uint8_t r, g, b;
    fxColor(s, i, now, &r, &g, &b);
    float m = intro ? introMask(s, i, now - introT0) : 1.0f;
    float w = (m > 0 && m < 1) ? 255 * (1 - m) : 0;
    px[s][i][0] = (uint8_t)(min(255.0f, r * m + w) * k);
    px[s][i][1] = (uint8_t)(min(255.0f, g * m + w) * k);
    px[s][i][2] = (uint8_t)(min(255.0f, b * m + w) * k);
  }
  ledShow();
}

// ================= 舵机 =================
int curUs[2] = {US_CENTER, US_CENTER};
uint32_t tMoveEnd = 0;
bool estop = false;

int usAt(float degToStand) { return US_CENTER + STAND_SIGN * (int)lroundf(degToStand * US_PER_DEG); }
int standUs(int k) { return usAt(STAND_DEG[k]); }
int foldUs(int k) { return usAt(-FOLD_DEG[k]); }
void servoWrite(int k, int us) {
  us = constrain(us, US_MIN, US_MAX);
  curUs[k] = us;
  ledcWrite(SPIN[k], (uint32_t)us * 16384UL / 20000UL);
}
void servoOff(int k) { ledcWrite(SPIN[k], 0); }
float ease(float x) { if (x <= 0) return 0; if (x >= 1) return 1; return 0.5f - 0.5f * cosf(PI * x); }

// 两只各走到目标；first 先动，另一只在 first 走到一半时起步。走完两只不出力。
// 返回 false = 中途按了 BOOT 急停。
bool moveTwo(int tgt0, int tgt1, int first) {
  int from[2] = {curUs[0], curUs[1]};
  int tgt[2] = {tgt0, tgt1};
  uint32_t dur[2], off[2];
  for (int k = 0; k < 2; k++) {
    int span = max(abs(standUs(k) - foldUs(k)), 1);
    dur[k] = max((uint32_t)((uint64_t)MOVE_MS * abs(tgt[k] - from[k]) / span), (uint32_t)300);
  }
  off[first] = 0;
  off[1 - first] = (uint32_t)(STAGGER * dur[first]);
  moving = true;
  ledBlack();                                     // 动之前灯先全灭
  digitalWrite(PIN_BLED, LOW);
  bool ok = true;
  uint32_t t = 0, tEnd = max(off[0] + dur[0], off[1] + dur[1]);
  while (true) {
    for (int k = 0; k < 2; k++) {
      float x = (t < off[k]) ? 0 : (float)(t - off[k]) / dur[k];
      servoWrite(k, from[k] + (int)((tgt[k] - from[k]) * ease(x)));
    }
    if (t >= tEnd) break;
    if (digitalRead(PIN_BOOT) == LOW) { ok = false; break; }
    delay(20);
    t += 20;
  }
  if (ok) delay(150);
  servoOff(0); servoOff(1);
  digitalWrite(PIN_BLED, HIGH);
  moving = false;
  tMoveEnd = millis();
  if (!ok) { estop = true; Serial.println("急停：两只不出力。重新插电恢复。"); }
  return ok;
}
// 立起：右眼先动、长刃后动；收拢：长刃先动、右眼后动
void setPose(bool open) {
  Serial.println(open ? "立起" : "收拢");
  bool ok = open ? moveTwo(standUs(0), standUs(1), 1) : moveTwo(foldUs(0), foldUs(1), 0);
  if (!ok) return;
  poseOpen = open;
  if (open) introNext = true;
  savePrefs();
}

// ================= 翘板 =================
bool keyDown = false, kLastRaw = false;
uint32_t kRawChange = 0, kChange = 0, pressStart = 0, lastRelease = 0;
int clicks = 0;
bool seqOk = false;

void keyTick() {
  uint32_t now = millis();
  bool r = digitalRead(PIN_KEY) == LOW;
  if (r != kLastRaw) { kLastRaw = r; kRawChange = now; }
  if (now - kRawChange < DEBOUNCE_MS || r == keyDown) return;
  keyDown = r;
  kChange = now;
  if (keyDown) {
    pressStart = now;
    if (clicks == 0) seqOk = (now - lastRelease >= CLICK_GAP_MS);
  } else {
    bool isShort = now - pressStart < CLICK_MAX_MS;
    if (isShort && seqOk && poseOpen && now - tMoveEnd >= AFTER_MOVE_MS) clicks++;
    else clicks = 0;
    lastRelease = now;
  }
}
// 单按 = 开关灯，连按两下 = 换灯效
void clickTick() {
  if (clicks == 0 || keyDown || millis() - lastRelease < CLICK_GAP_MS) return;
  if (clicks == 1) lightOn = !lightOn;
  else { fx = (fx + 1) % NFX; lightOn = true; }
  clicks = 0;
  ledsBlack = true;
  if (!lightOn) ledBlack();
  savePrefs();
}

// ================= 第一次上电：装曲柄和右眼 =================
bool anyKey() { return digitalRead(PIN_KEY) == LOW || digitalRead(PIN_BOOT) == LOW; }
void assemble() {
  Serial.println("第一次上电：两只舵机停在装配角。右眼长直边竖直压上，曲柄臂竖直朝上压上，拧好中心螺丝，按一下翘板。");
  servoWrite(1, usAt(ASM_DEG[1]));
  servoWrite(0, usAt(ASM_DEG[0]));
  delay(1500);
  for (int i = 0; i < 5; i++) px[2][i][1] = 12;   // 底座开头 5 颗暗绿：等你装
  ledShow(); ledsBlack = false;
  uint32_t tDown = 0;
  while (true) {                                  // 小灯快闪，等按一下（按稳 50 ms 才算）
    digitalWrite(PIN_BLED, (millis() / 100) % 2 ? LOW : HIGH);
    if (!anyKey()) tDown = 0;
    else if (tDown == 0) tDown = millis();
    else if (millis() - tDown >= 50) break;
    delay(5);
  }
  while (anyKey()) delay(5);                      // 等松手再动
  delay(300);
  // 回中位（右眼先动），两只不出力，亮灯 = 装完了。在中位装长刃、摇杆（P、Q 螺丝在中位穿得进，结构核对过）。
  // 之后按“立起”对待：第一次放上鼠标就从中位收拢，从此正常使用
  if (!moveTwo(US_CENTER, US_CENTER, 1)) return;
  assembled = true;
  poseOpen = true;
  introNext = true;
  savePrefs();
}

// 插着电脑：不动舵机、不亮灯带，等串口发 g
void usbGate() {
  uint32_t t0 = millis();
  bool host = false;
  while (millis() - t0 < 1500) { if (HWCDC::isPlugged()) { host = true; break; } delay(10); }
  if (!host) return;
  uint32_t tSay = 0;
  while (true) {
    uint32_t m = millis() % 2000;
    digitalWrite(PIN_BLED, (m < 60 || (m >= 180 && m < 240)) ? LOW : HIGH);
    if (tSay == 0 || millis() - tSay > 10000) {
      tSay = millis();
      Serial.printf("停机坪 09 发布版  编译于 " __DATE__ " " __TIME__ "。%s。现在插着电脑，不动舵机；拔下来插充电头就开始。\n",
                    assembled ? "已装好" : "还没装曲柄和右眼");
    }
    while (Serial.available()) if (Serial.read() == 'g') { digitalWrite(PIN_BLED, HIGH); return; }
    delay(10);
  }
}

void setup() {
  pinMode(PIN_KEY, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BLED, OUTPUT);
  digitalWrite(PIN_BLED, HIGH);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  loadPrefs();
  for (int s = 0; s < 3; s++) strips[s]->begin();
  ledBlack();
  ledcAttach(SPIN[0], 50, 14); ledcAttach(SPIN[1], 50, 14);
  servoOff(0); servoOff(1);

  usbGate();

  if (!assembled) assemble();
  else {
    // 按上次停的姿态认当前位置：位置没被掰过的话第一次动作不会跳
    curUs[0] = poseOpen ? standUs(0) : foldUs(0);
    curUs[1] = poseOpen ? standUs(1) : foldUs(1);
    if (poseOpen) introNext = true;
  }
  keyDown = kLastRaw = digitalRead(PIN_KEY) == LOW;
  kChange = kRawChange = lastRelease = tMoveEnd = millis();
}

void loop() {
  if (estop) { digitalWrite(PIN_BLED, (millis() / 1000) % 2 ? LOW : HIGH); delay(20); return; }   // 急停后慢闪，等重新插电

  // BOOT 按住 5 秒：清“已装好”，两只不出力，小灯快闪，等拆零件后重新插电
  static uint32_t bootT0 = 0;
  if (digitalRead(PIN_BOOT) == HIGH) bootT0 = 0;
  else if (bootT0 == 0) bootT0 = millis();
  else if (millis() - bootT0 >= REDO_HOLD_MS) {
    assembled = false; poseOpen = false; savePrefs();
    ledBlack(); servoOff(0); servoOff(1);
    Serial.println("已清掉“已装好”。拆下曲柄和右眼，重新插电。");
    while (true) { digitalWrite(PIN_BLED, (millis() / 100) % 2 ? LOW : HIGH); delay(20); }
  }

  keyTick();
  uint32_t now = millis();
  if (!poseOpen && !keyDown && now - kChange >= LIFT_MS) setPose(true);                       // 拿起 → 立起
  else if (poseOpen && keyDown && now - kChange >= PUT_MS) { clicks = 0; setPose(false); }    // 放下 → 收拢
  else clickTick();
  ledTick();
  delay(2);
}
