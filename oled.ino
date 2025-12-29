/*
  ESP32 俄羅斯方塊遊戲（BLE 控制 + MQTT 只上傳最終成績）
  - BLE:
    - CMD  (WRITE): left/right/down/rotate/superdown/pause/reset/start
    - NAME (WRITE): 玩家姓名 (1~20字)
    - STATUS (NOTIFY/READ): 狀態訊息回傳給 App
  - MQTT:
    - Publish only: Tetris/Score  (retain=true)
  - OLED:
    - OLED1 (0x3C): 動畫（start/happy/waiting/sad）
    - OLED2 (0x3D): 分數/玩家顯示（若你第二塊 OLED 位址不同請改 OLED2_ADDR）
*/

#define MQTT_MAX_PACKET_SIZE 512

#include <MD_MAX72xx.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// BLE
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// 包含動畫數據
#include "start.h"
#include "happy.h"
#include "waiting.h"
#include "sad.h"

// ==================== OLED 設定 ====================
#define SDA_PIN 21
#define SCL_PIN 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// OLED1：動畫（通常 0x3C）
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// OLED2：分數顯示（通常 0x3D）
#define OLED2_ADDR 0x3D
Adafruit_SSD1306 display2(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oled2_ok = false;

// ==================== 動畫狀態枚舉 ====================
enum AnimationState {
  ANIM_START,    // 開始畫面（循環）
  ANIM_PLAYING,  // 遊戲進行中（不播動畫）
  ANIM_HAPPY,    // 消除方塊（單次）
  ANIM_WAITING,  // 暫停（循環）
  ANIM_SAD       // 遊戲結束（單次）
};

// 動畫控制變數
AnimationState currentAnimState = ANIM_START;
AnimationState targetAnimState = ANIM_START;
uint8_t currentFrame = 0;
unsigned long lastFrameTime = 0;
const AnimatedGIF* currentGIF = nullptr;
bool animationChanged = false;
bool playOnce = false;               // 是否只播放一次
bool animationFinished = false;      // 動畫是否播放完成
AnimationState returnState = ANIM_PLAYING;  // 單次動畫播完後回到的狀態

// ==================== WiFi 設定 ====================
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ==================== MQTT 設定（只上傳 Score） ====================
const char* mqtt_server = "MQTTGO.io";
const int mqtt_port = 1883;
const char* mqtt_client_id = "MQTTGO-9345814340";
const char* mqtt_topic_score = "Tetris/Score";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ==================== BLE (App 控制/姓名/狀態) ====================
// 自訂 UUID（App 端請用同一組）
#define BLE_SERVICE_UUID        "b1d2f000-7c1a-4b4a-9b2f-111111111111"
#define BLE_CHAR_CMD_UUID       "b1d2f001-7c1a-4b4a-9b2f-111111111111"  // App -> ESP32 寫入指令
#define BLE_CHAR_NAME_UUID      "b1d2f002-7c1a-4b4a-9b2f-111111111111"  // App -> ESP32 寫入姓名
#define BLE_CHAR_STATUS_UUID    "b1d2f003-7c1a-4b4a-9b2f-111111111111"  // ESP32 -> App notify 狀態

BLEServer* pServer = nullptr;
BLECharacteristic* pCharCmd = nullptr;
BLECharacteristic* pCharName = nullptr;
BLECharacteristic* pCharStatus = nullptr;
bool bleConnected = false;

// ==================== MAX7219 硬體設定 ====================
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define CS_PIN 5
#define NUM_MODULES 4
MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, CS_PIN, NUM_MODULES);

// ==================== 遊戲狀態 ====================
bool gamePaused = false;
bool gameOver = false;
bool gameStarted = false;
int gameScore = 0;
String playerName = "Player";

// 顯示尺寸
const int SCREEN_W = 8;
const int SCREEN_H = SCREEN_W * NUM_MODULES;

// 遊戲場地緩衝區
uint8_t field[SCREEN_H];

// 時間控制
unsigned long lastDrop = 0;
unsigned long dropInterval = 500;
const unsigned long refreshInterval = 33;
unsigned long lastRefresh = 0;

// 前一幀緩衝區
uint8_t prevBuf[NUM_MODULES][SCREEN_W];

// 指令緩衝（沿用你原本 newCommand / mqttCommand 的流程）
String mqttCommand = "";
bool newCommand = false;

// ==================== 當前方塊結構 ====================
struct Block {
  const int (*shape)[2];
  int len;
  int x, y;
  int rotation;
  char type;
} current;

// ==================== 七種俄羅斯方塊形狀定義 ====================
const int I_SHAPE[2][4][2] = {
  { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 } },
  { { -1, 1 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }
};
const int O_SHAPE[1][4][2] = {
  { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }
};
const int T_SHAPE[4][4][2] = {
  { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
  { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 0, 1 } },
  { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 1, 2 } },
  { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 2, 1 } }
};
const int L_SHAPE[4][4][2] = {
  { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 1, 2 } },
  { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 0, 1 } },
  { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 1, 2 } },
  { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }
};
const int J_SHAPE[4][4][2] = {
  { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 0, 2 } },
  { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
  { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 2 } },
  { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 2, 1 } }
};
const int S_SHAPE[2][4][2] = {
  { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },
  { { 1, 0 }, { 1, 1 }, { 2, 1 }, { 2, 2 } }
};
const int Z_SHAPE[2][4][2] = {
  { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },
  { { 2, 0 }, { 1, 1 }, { 2, 1 }, { 1, 2 } }
};

// ==================== Game Over 字母點陣圖 ====================
static const uint8_t PAT_G[8] = { 0x3C, 0x42, 0x40, 0x4E, 0x42, 0x42, 0x3C, 0x00 };
static const uint8_t PAT_A[8] = { 0x18, 0x24, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00 };
static const uint8_t PAT_M[8] = { 0x42, 0x66, 0x5A, 0x5A, 0x42, 0x42, 0x42, 0x00 };
static const uint8_t PAT_E[8] = { 0x7E, 0x40, 0x5C, 0x40, 0x40, 0x40, 0x7E, 0x00 };
static const uint8_t PAT_O[8] = { 0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00 };
static const uint8_t PAT_V[8] = { 0x42, 0x42, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00 };
static const uint8_t PAT_R[8] = { 0x7C, 0x42, 0x42, 0x7C, 0x48, 0x44, 0x42, 0x00 };
static const uint8_t PAT_P[8] = { 0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x40, 0x00 };

// ==================== BLE 輔助 ====================
void bleSendStatus(const String& s) {
  if (!bleConnected || pCharStatus == nullptr) return;
  pCharStatus->setValue(s.c_str());
  pCharStatus->notify();
}

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    (void)pServer;
    bleConnected = true;
    bleSendStatus("BLE 已連線");
  }
  void onDisconnect(BLEServer* pServer) override {
    bleConnected = false;
    pServer->startAdvertising(); // 斷線後重新廣播
  }
};

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string v = pCharacteristic->getValue();
    if (v.empty()) return;

    String cmd = String(v.c_str());
    cmd.trim();
    cmd.toLowerCase();

    mqttCommand = cmd;   // 沿用原本 loop() 指令處理
    newCommand = true;
  }
};

class NameCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string v = pCharacteristic->getValue();
    if (v.empty()) return;

    String name = String(v.c_str());
    name.trim();

    if (name.length() >= 1 && name.length() <= 20) {
      playerName = name;
      bleSendStatus("姓名已設定: " + playerName);
    } else {
      bleSendStatus("錯誤: 姓名須為 1-20 字元");
    }
  }
};

// ==================== OLED 動畫函數 ====================

// 設定要播放的動畫（支援單次播放模式）
void setAnimation(AnimationState newState, bool once = false, AnimationState nextState = ANIM_PLAYING) {
  if (targetAnimState != newState || playOnce != once) {
    targetAnimState = newState;
    animationChanged = true;
    playOnce = once;
    returnState = nextState;
    animationFinished = false;

    Serial.print("[OLED] 動畫切換請求: ");
    switch(newState) {
      case ANIM_START: Serial.print("START"); break;
      case ANIM_PLAYING: Serial.print("PLAYING (停止動畫)"); break;
      case ANIM_HAPPY: Serial.print("HAPPY"); break;
      case ANIM_WAITING: Serial.print("WAITING"); break;
      case ANIM_SAD: Serial.print("SAD"); break;
    }
    if (once) Serial.println(" (單次播放)");
    else Serial.println(" (循環播放)");
  }
}

// 初始化新動畫
void initAnimation() {
  currentAnimState = targetAnimState;
  currentFrame = 0;
  lastFrameTime = millis();
  animationChanged = false;
  animationFinished = false;

  switch(currentAnimState) {
    case ANIM_START:
      currentGIF = &start_gif;
      Serial.println("[OLED] ✓ 載入 START 動畫");
      break;
    case ANIM_HAPPY:
      currentGIF = &happy_gif;
      Serial.println("[OLED] ✓ 載入 HAPPY 動畫");
      break;
    case ANIM_WAITING:
      currentGIF = &waiting_gif;
      Serial.println("[OLED] ✓ 載入 WAITING 動畫");
      break;
    case ANIM_SAD:
      currentGIF = &sad_gif;
      Serial.println("[OLED] ✓ 載入 SAD 動畫");
      break;
    case ANIM_PLAYING:
      currentGIF = nullptr;
      display.clearDisplay();
      display.display();
      Serial.println("[OLED] ✓ 清空顯示（遊戲中）");
      break;
  }
}

// 更新 OLED 動畫（非阻塞）
void updateOLEDAnimation() {
  if (animationChanged) initAnimation();

  if (currentGIF == nullptr || currentAnimState == ANIM_PLAYING) return;

  if (playOnce && animationFinished) {
    Serial.println("[OLED] ✓ 單次動畫播放完成，返回遊戲狀態");
    setAnimation(returnState, false);
    return;
  }

  unsigned long now = millis();
  uint16_t frameDelay = currentGIF->delays[currentFrame];

  if (now - lastFrameTime >= frameDelay) {
    display.clearDisplay();

    // 繪製當前幀
    for (uint16_t y = 0; y < currentGIF->height; y++) {
      for (uint16_t x = 0; x < currentGIF->width; x++) {
        uint16_t byteIndex = (y * currentGIF->width + x) / 8;
        uint8_t bitIndex = 7 - ((y * currentGIF->width + x) % 8);

        if (currentGIF->frames[currentFrame][byteIndex] & (1 << bitIndex)) {
          display.drawPixel(x, y, SSD1306_WHITE);
        }
      }
    }

    display.display();

    currentFrame++;
    if (currentFrame >= currentGIF->frame_count) {
      if (playOnce) {
        animationFinished = true;
        currentFrame = currentGIF->frame_count - 1;
      } else {
        currentFrame = 0;
      }
    }
    lastFrameTime = now;
  }
}

// ==================== OLED2：分數顯示 ====================
void updateScoreOLED2() {
  if (!oled2_ok) return;

  display2.clearDisplay();
  display2.setTextColor(SSD1306_WHITE);

  display2.setTextSize(1);
  display2.setCursor(0, 0);
  display2.print("Player:");
  display2.setCursor(0, 12);
  display2.print(playerName);

  display2.setTextSize(2);
  display2.setCursor(0, 28);
  display2.print("Score:");
  display2.setCursor(0, 46);
  display2.print(gameScore);

  display2.display();
}

// ==================== WiFi & MQTT 函數 ====================
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.println("========================================");
  Serial.print("[WiFi] 連接到 WiFi SSID: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WiFi] ✓ WiFi 已連接成功！");
    Serial.print("[WiFi] IP 位址: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] 訊號強度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println();
    Serial.println("[WiFi] ✗ WiFi 連接失敗！");
  }
  Serial.println("========================================");
}

// 空 callback（保留 setCallback 用，不訂閱任何 topic）
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  (void)topic;
  (void)payload;
  (void)length;
}

void reconnectMQTT() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 3) {
    Serial.println("[MQTT] ----------------------------------------");
    Serial.print("[MQTT] 嘗試連接 MQTT Broker (");
    Serial.print(attempts + 1);
    Serial.println("/3)...");

    if (mqttClient.connect(mqtt_client_id)) {
      Serial.println("[MQTT] ✓ MQTT 連接成功（僅發佈 Tetris/Score）");
      Serial.println("[MQTT] ----------------------------------------");
      return;
    } else {
      int state = mqttClient.state();
      Serial.print("[MQTT] ✗ 連接失敗！錯誤碼: ");
      Serial.println(state);
      attempts++;
      if (attempts < 3) {
        Serial.println("[MQTT] 2 秒後重試...");
        delay(2000);
      }
    }
  }

  if (!mqttClient.connected()) {
    Serial.println("[MQTT] ✗ 無法連接 MQTT，將在背景繼續嘗試...");
  }
  Serial.println("[MQTT] ----------------------------------------");
}

// 取代原本 publishFraction：不走 MQTT，只更新 OLED2
void publishFraction(int linesCleared) {
  (void)linesCleared;
  updateScoreOLED2();
}

void publishScore() {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] ✗ 未連接，無法發送最終成績");
    bleSendStatus("MQTT 未連線，成績未上傳");
    return;
  }

  char scoreStr[150];
  sprintf(scoreStr, "{\"玩家\":\"%s\",\"分數\":%d,\"時間\":%lu}",
          playerName.c_str(), gameScore, millis()/1000);

  if (mqttClient.publish(mqtt_topic_score, scoreStr, true)) {
    Serial.println("[MQTT] ========================================");
    Serial.print("[MQTT] ✓ 最終成績已發送: ");
    Serial.println(scoreStr);
    Serial.println("[MQTT] ========================================");
    bleSendStatus(String("遊戲結束，已上傳分數: ") + gameScore);
  } else {
    Serial.println("[MQTT] ✗ 最終成績發送失敗！");
    bleSendStatus("成績上傳失敗");
  }
}

// ==================== 遊戲函數 ====================
void clearAll() {
  mx.clear();
}

void showPauseScreen() {
  Serial.println("[Display] 顯示暫停畫面");
  clearAll();

  const uint8_t* pat = PAT_P;
  uint8_t rot[8] = {};

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (pat[y] & (1 << x)) {
        int nx = 7 - y;
        int ny = x;
        rot[ny] |= (1 << nx);
      }
    }
  }

  for (int m = 1; m <= 2; m++) {
    for (int row = 0; row < 8; row++) {
      mx.setRow(m, row, rot[row]);
    }
  }

  for (int m = 0; m < NUM_MODULES; m++)
    for (int r = 0; r < SCREEN_W; r++)
      prevBuf[m][r] = 0;

  setAnimation(ANIM_WAITING, false);
}

const uint8_t* letterPattern(char c) {
  switch (c) {
    case 'G': return PAT_G;
    case 'A': return PAT_A;
    case 'M': return PAT_M;
    case 'E': return PAT_E;
    case 'O': return PAT_O;
    case 'V': return PAT_V;
    case 'R': return PAT_R;
    case 'P': return PAT_P;
    default: return PAT_E;
  }
}

void gameOverSequence() {
  Serial.println("");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║         🎮 遊 戲 結 束 🎮            ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.print("║  玩家: ");
  Serial.print(playerName);
  for (int i = playerName.length(); i < 32; i++) Serial.print(" ");
  Serial.println("║");
  Serial.print("║  最終分數: ");
  Serial.print(gameScore);
  for (int i = String(gameScore).length(); i < 26; i++) Serial.print(" ");
  Serial.println("║");
  Serial.println("╚════════════════════════════════════════╝");

  gameOver = true;
  gameStarted = false;

  setAnimation(ANIM_SAD, true, ANIM_START);

  // BLE 狀態
  bleSendStatus(String("遊戲結束，分數: ") + gameScore);

  // MQTT 上傳最終成績
  publishScore();

  // 閃爍動畫
  for (int i = 0; i < 3; i++) {
    clearAll();
    delay(500);
    for (int m = 0; m < NUM_MODULES; m++)
      for (int r = 0; r < SCREEN_W; r++)
        mx.setRow(m, r, 0xFF);
    delay(500);
  }

  // 顯示 "GAME"
  const char* w1 = "GAME";
  for (int seg = 0; seg < 4; seg++) {
    const uint8_t* pat = letterPattern(w1[seg]);
    uint8_t rot[8] = {};
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 8; x++) {
        if (pat[y] & (1 << x)) {
          int nx = 7 - y;
          int ny = x;
          rot[ny] |= (1 << nx);
        }
      }
    }
    int module = NUM_MODULES - 1 - seg;
    for (int row = 0; row < 8; row++) {
      mx.setRow(module, row, rot[row]);
    }
  }
  delay(1000);

  // 顯示 "OVER"
  const char* w2 = "OVER";
  for (int seg = 0; seg < 4; seg++) {
    const uint8_t* pat = letterPattern(w2[seg]);
    uint8_t rot[8] = {};
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 8; x++) {
        if (pat[y] & (1 << x)) {
          int nx = 7 - y;
          int ny = x;
          rot[ny] |= (1 << nx);
        }
      }
    }
    int module = NUM_MODULES - 1 - seg;
    for (int row = 0; row < 8; row++) {
      mx.setRow(module, row, rot[row]);
    }
  }
  delay(2000);

  Serial.println("[Game] 💡 透過 BLE 發送 'start' 或 'reset' 開始新遊戲");
}

void spawnBlock() {
  int r = random(7);
  int sx = SCREEN_W / 2 - 2;
  current.rotation = 0;

  char blockType = ' ';
  switch (r) {
    case 0:
      current = { I_SHAPE[0], 4, sx, 0, 0, 'I' };
      blockType = 'I';
      break;
    case 1:
      current = { O_SHAPE[0], 4, sx, 0, 0, 'O' };
      blockType = 'O';
      break;
    case 2:
      current = { T_SHAPE[0], 4, sx, 0, 0, 'T' };
      blockType = 'T';
      break;
    case 3:
      current = { L_SHAPE[0], 4, sx, 0, 0, 'L' };
      blockType = 'L';
      break;
    case 4:
      current = { J_SHAPE[0], 4, sx, 0, 0, 'J' };
      blockType = 'J';
      break;
    case 5:
      current = { S_SHAPE[0], 4, sx, 0, 0, 'S' };
      blockType = 'S';
      break;
    case 6:
      current = { Z_SHAPE[0], 4, sx, 0, 0, 'Z' };
      blockType = 'Z';
      break;
  }

  Serial.print("[Game] 🎲 新方塊: ");
  Serial.println(blockType);
}

void resetGame() {
  Serial.println("");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║       🎮 遊 戲 重 新 開 始 🎮         ║");
  Serial.println("╠════════════════════════════════════════╣");
  Serial.print("║  玩家: ");
  Serial.print(playerName);
  for (int i = playerName.length(); i < 32; i++) Serial.print(" ");
  Serial.println("║");
  Serial.println("║  分數: 0                               ║");
  Serial.println("╚════════════════════════════════════════╝");

  memset(field, 0, sizeof(field));
  clearAll();
  for (int m = 0; m < NUM_MODULES; m++)
    for (int r = 0; r < SCREEN_W; r++)
      prevBuf[m][r] = 0;

  gameScore = 0;
  gamePaused = false;
  gameOver = false;
  gameStarted = true;

  setAnimation(ANIM_PLAYING, false);

  spawnBlock();
  lastDrop = millis();
  lastRefresh = millis();

  publishFraction(0);
  bleSendStatus(playerName + " 開始新遊戲");
}

void writeBuffer() {
  uint8_t buf[NUM_MODULES][SCREEN_W] = {};

  for (int y = 0; y < SCREEN_H; y++) {
    uint8_t row = field[y];
    if (!row) continue;
    int mod = NUM_MODULES - 1 - (y / SCREEN_W);
    int bit = 1 << (7 - (y % SCREEN_W));
    for (int x = 0; x < SCREEN_W; x++) {
      if (row & (1 << x)) buf[mod][x] |= bit;
    }
  }

  for (int i = 0; i < current.len; i++) {
    int xx = current.x + current.shape[i][0];
    int yy = current.y + current.shape[i][1];
    if (xx < 0 || xx >= SCREEN_W || yy < 0 || yy >= SCREEN_H) continue;
    int mod = NUM_MODULES - 1 - (yy / SCREEN_W);
    int bit = 1 << (7 - (yy % SCREEN_W));
    buf[mod][xx] |= bit;
  }

  for (int m = 0; m < NUM_MODULES; m++) {
    for (int r = 0; r < SCREEN_W; r++) {
      if (buf[m][r] != prevBuf[m][r]) {
        mx.setRow(m, r, buf[m][r]);
        prevBuf[m][r] = buf[m][r];
      }
    }
  }
}

bool checkCollision(int nx, int ny) {
  for (int i = 0; i < current.len; i++) {
    int xx = nx + current.shape[i][0];
    int yy = ny + current.shape[i][1];
    if (xx < 0 || xx >= SCREEN_W || yy >= SCREEN_H) return true;
    if (yy >= 0 && (field[yy] & (1 << xx))) return true;
  }
  return false;
}

void placeBlock() {
  for (int i = 0; i < current.len; i++) {
    int xx = current.x + current.shape[i][0];
    int yy = current.y + current.shape[i][1];
    if (yy >= 0 && yy < SCREEN_H) field[yy] |= (1 << xx);
  }

  int linesCleared = 0;
  for (int y = 0; y < SCREEN_H; y++) {
    if (field[y] == 0xFF) {
      linesCleared++;
      for (int j = y; j > 0; j--) field[j] = field[j - 1];
      field[0] = 0;
    }
  }

  if (linesCleared > 0) {
    gameScore += linesCleared * 100;
    Serial.print("[Game] 🎉 消除 ");
    Serial.print(linesCleared);
    Serial.print(" 行！目前分數: ");
    Serial.println(gameScore);

    publishFraction(linesCleared);
    setAnimation(ANIM_HAPPY, true, ANIM_PLAYING);
  }
}

void rotateBlock() {
  int limit = (current.type == 'I' || current.type == 'S' || current.type == 'Z') ? 2
              : (current.type == 'O' ? 1 : 4);

  int nr = (current.rotation + 1) % limit;
  const int(*ns)[2] = nullptr;

  if (current.type == 'I') ns = I_SHAPE[nr];
  else if (current.type == 'O') ns = O_SHAPE[0];
  else if (current.type == 'T') ns = T_SHAPE[nr];
  else if (current.type == 'L') ns = L_SHAPE[nr];
  else if (current.type == 'J') ns = J_SHAPE[nr];
  else if (current.type == 'S') ns = S_SHAPE[nr];
  else if (current.type == 'Z') ns = Z_SHAPE[nr];

  Block bak = current;
  current.shape = ns;
  current.rotation = nr;

  if (checkCollision(current.x, current.y)) {
    current = bak;
    Serial.println("[Game] ↻ 旋轉失敗（碰撞）");
  } else {
    Serial.println("[Game] ↻ 方塊旋轉");
  }
}

// ==================== setup() ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║   ESP32 俄羅斯方塊遊戲 (BLE + MQTT)   ║");
  Serial.println("║   - BLE 控制/姓名/狀態               ║");
  Serial.println("║   - MQTT 只上傳 Tetris/Score         ║");
  Serial.println("╚════════════════════════════════════════╝");

  randomSeed(analogRead(0));

  // I2C init
  Wire.begin(SDA_PIN, SCL_PIN);

  // OLED1：動畫
  Serial.println("[OLED] 初始化 OLED1（動畫）...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] ✗ OLED1 初始化失敗！");
  } else {
    Serial.println("[OLED] ✓ OLED1 初始化成功");
    display.clearDisplay();
    display.display();
    setAnimation(ANIM_START, false);
    Serial.println("[OLED] 🎬 START 動畫（循環）");
  }

  // OLED2：分數（可選）
  Serial.println("[OLED2] 初始化 OLED2（分數）...");
  if (display2.begin(SSD1306_SWITCHCAPVCC, OLED2_ADDR)) {
    oled2_ok = true;
    display2.clearDisplay();
    display2.display();
    Serial.println("[OLED2] ✓ OLED2 初始化成功");
  } else {
    Serial.println("[OLED2] ✗ OLED2 初始化失敗（若你有第二塊 OLED，請確認 I2C 位址）");
  }

  // MAX7219
  Serial.println("[Display] 初始化 LED 矩陣...");
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, MAX_INTENSITY / 2);
  mx.clear();

  for (int m = 0; m < NUM_MODULES; m++) {
    for (int r = 0; r < SCREEN_W; r++)
      prevBuf[m][r] = 0;
  }

  // BLE init
  Serial.println("[BLE] 初始化 BLE...");
  BLEDevice::init("Tetris-ESP32");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  pCharCmd = pService->createCharacteristic(
    BLE_CHAR_CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharCmd->setCallbacks(new CmdCallbacks());

  pCharName = pService->createCharacteristic(
    BLE_CHAR_NAME_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharName->setCallbacks(new NameCallbacks());

  pCharStatus = pService->createCharacteristic(
    BLE_CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
  );
  pCharStatus->addDescriptor(new BLE2902());

  pService->start();
  BLEDevice::startAdvertising();
  Serial.println("[BLE] ✓ BLE 已啟動，等待 App 連線");

  // WiFi
  setup_wifi();

  // MQTT
  Serial.println("[MQTT] 初始化 MQTT（只發佈 Score）...");
  mqttClient.setBufferSize(512);
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(15);

  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
  }

  // 初始刷新 OLED2
  publishFraction(0);

  Serial.println("[System] ✓ 初始化完成！");
  Serial.println("[Game] 💡 透過 BLE 發送 'start' 或 'reset' 指令開始遊戲");
}

// ==================== loop() ====================
void loop() {
  // 1) OLED 動畫（最優先）
  updateOLEDAnimation();

  // 2) MQTT 連線維護（只為了能上傳 Score）
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      static unsigned long lastReconnectAttempt = 0;
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 10000) {
        lastReconnectAttempt = now;
        Serial.println("[MQTT] ⚠ 連線中斷，嘗試重新連接...");
        reconnectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }

  // 3) 處理 BLE 指令（沿用原本 newCommand/mqttCommand 流程）
  if (newCommand) {
    newCommand = false;
    mqttCommand.trim();
    mqttCommand.toLowerCase();

    Serial.print("[CTRL] 🎯 執行指令: ");
    Serial.println(mqttCommand);

    if (mqttCommand == "reset" || mqttCommand == "start") {
      Serial.println("[Game] 🔄 收到 reset/start 指令");
      resetGame();
      return;
    }
    else if (mqttCommand == "pause") {
      if (!gameOver && gameStarted) {
        gamePaused = !gamePaused;
        if (gamePaused) {
          Serial.println("[Game] ⏸ 遊戲已暫停");
          showPauseScreen();
          bleSendStatus("遊戲已暫停");
        } else {
          Serial.println("[Game] ▶ 遊戲繼續");
          clearAll();
          for (int m = 0; m < NUM_MODULES; m++)
            for (int r = 0; r < SCREEN_W; r++)
              prevBuf[m][r] = 0;
          writeBuffer();

          setAnimation(ANIM_PLAYING, false);
          bleSendStatus("遊戲繼續");
        }
      }
      return;
    }
    else if (!gamePaused && !gameOver && gameStarted) {
      if (mqttCommand == "left") {
        if (!checkCollision(current.x - 1, current.y)) {
          current.x--;
          Serial.println("[Input] ← 向左移動");
        }
      }
      else if (mqttCommand == "right") {
        if (!checkCollision(current.x + 1, current.y)) {
          current.x++;
          Serial.println("[Input] → 向右移動");
        }
      }
      else if (mqttCommand == "down") {
        if (!checkCollision(current.x, current.y + 1)) {
          current.y++;
          Serial.println("[Input] ↓ 向下移動");
        }
      }
      else if (mqttCommand == "rotate") {
        rotateBlock();
      }
      else if (mqttCommand == "superdown") {
        Serial.println("[Input] ⚡ SUPERDOWN 啟動！加速下降中...");
        int steps = 0;

        while (!checkCollision(current.x, current.y + 1)) {
          current.y++;
          steps++;
          writeBuffer();
          delay(30);
        }

        Serial.print("[Input] ⚡ SuperDown 完成！下降了 ");
        Serial.print(steps);
        Serial.println(" 格");

        lastDrop = millis();
        bleSendStatus(String("SuperDown 下降 ") + steps + " 格");
      }
      else {
        Serial.print("[Input] ⚠ 未知指令: ");
        Serial.println(mqttCommand);
        bleSendStatus("未知指令: " + mqttCommand);
      }
    }
  }

  // 4) 遊戲結束/暫停/未開始 -> 不跑遊戲邏輯
  if (gameOver || gamePaused || !gameStarted) {
    return;
  }

  unsigned long now = millis();

  // 自動下落
  if (now - lastDrop > dropInterval) {
    lastDrop = now;
    if (!checkCollision(current.x, current.y + 1)) {
      current.y++;
    } else {
      bool hitTop = false;
      for (int i = 0; i < current.len; i++) {
        if (current.y + current.shape[i][1] == 0) {
          hitTop = true;
          break;
        }
      }
      if (hitTop) {
        gameOverSequence();
        return;
      } else {
        placeBlock();
        spawnBlock();
      }
    }
  }

  // 刷新顯示
  if (now - lastRefresh >= refreshInterval) {
    writeBuffer();
    lastRefresh = now;
  }
}
