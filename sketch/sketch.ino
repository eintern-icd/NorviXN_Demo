#include <Arduino.h>
#include <Preferences.h>
#include <CST816S.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Touch calibration. If a tap lands in the wrong lane, change these.
#define TOUCH_SWAP_XY 0
#define TOUCH_FLIP_X  0
#define TOUCH_FLIP_Y  0
// 1 = print raw touch coordinates on Serial (helps calibration)
#define DEBUG_TOUCH   1

#define ETH_CS_PIN 1
#define SD_CS_PIN  42

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;

public:
  LGFX(void) {
    {
      auto c = _bus.config();
      c.spi_host    = SPI2_HOST;
      c.spi_mode    = 0;
      c.freq_write  = 27000000;
      c.freq_read   = 16000000;
      c.spi_3wire   = false;
      c.use_lock    = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk    = 12;
      c.pin_mosi    = 11;
      c.pin_miso    = 13;
      c.pin_dc      = 46;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs          = 45;
      c.pin_rst         = 47;
      c.pin_busy        = -1;
      c.panel_width     = 240;
      c.panel_height    = 320;
      c.offset_x        = 0;
      c.offset_y        = 0;
      c.offset_rotation = 0;
      c.readable        = false;
      c.invert          = true;
      c.rgb_order       = false;
      c.dlen_16bit      = false;
      c.bus_shared      = true;
      _panel.config(c);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
LGFX_Sprite canvas(&tft);
lgfx::LGFXBase *g = &tft;
bool useCanvas = false;
Preferences prefs;
CST816S touch(8, 9, 47, 48);

const int SCR_W = 240;
const int SCR_H = 320;
const int LANES = 4;
const int LANE_W = 60;
const int CAR_W = 34;
const int CAR_H = 60;
const int MAX_ENEMY = 6;
const int HUD_H = 30;
const int PLAYER_Y = SCR_H - CAR_H - 18;

const uint16_t C_ROAD   = RGB565(48, 50, 56);
const uint16_t C_WHITE  = RGB565(235, 235, 235);
const uint16_t C_YELLOW = RGB565(255, 200, 0);
const uint16_t C_TIRE   = RGB565(15, 15, 15);
const uint16_t C_GLASS  = RGB565(120, 190, 235);
const uint16_t C_PLAYER = RGB565(30, 200, 90);
const uint16_t C_BLACK  = 0x0000;
const uint16_t C_RED    = RGB565(230, 40, 40);
const uint16_t C_ORANGE = RGB565(255, 140, 0);

const uint16_t ENEMY_COLORS[6] = {
  RGB565(220, 50, 50),
  RGB565(255, 130, 20),
  RGB565(160, 70, 220),
  RGB565(40, 190, 220),
  RGB565(240, 220, 60),
  RGB565(210, 210, 215)
};

struct Enemy {
  bool active;
  int lane;
  float y;
  uint16_t color;
};

Enemy enemies[MAX_ENEMY];

enum State { ST_TITLE, ST_PLAYING, ST_GAMEOVER };
State state = ST_TITLE;

float playerX;
int targetLane;
float scrollOffset;
float speed;
float elapsed;
float scoreF;
float spawnTimer;
int carsPassed;
uint32_t bestScore = 0;
uint32_t gameOverAt = 0;
uint32_t lastMicros = 0;

int laneCenter(int lane) {
  return lane * LANE_W + LANE_W / 2;
}

void readInput(int &lane, bool &tapped) {
  static bool wasDown = false;
  static uint32_t lastTouchMs = 0;
  lane = -1;
  tapped = false;

  if (touch.available()) {
    int x = touch.data.x;
    int y = touch.data.y;
#if DEBUG_TOUCH
    Serial.printf("touch raw x=%d y=%d event=%d points=%d\n", x, y, touch.data.event, touch.data.points);
#endif
#if TOUCH_SWAP_XY
    int t = x; x = y; y = t;
#endif
#if TOUCH_FLIP_X
    x = SCR_W - 1 - x;
#endif
#if TOUCH_FLIP_Y
    y = SCR_H - 1 - y;
#endif
    if (x < 0) x = 0;
    if (x > SCR_W - 1) x = SCR_W - 1;

    if (touch.data.event == 1) {
      wasDown = false;
    } else {
      lane = x / LANE_W;
      if (lane > LANES - 1) lane = LANES - 1;
      if (!wasDown) tapped = true;
      wasDown = true;
      lastTouchMs = millis();
    }
  } else if (wasDown && millis() - lastTouchMs > 250) {
    wasDown = false;
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'a' || c == 'A') { lane = max(0, targetLane - 1); tapped = true; }
    if (c == 'd' || c == 'D') { lane = min(LANES - 1, targetLane + 1); tapped = true; }
  }
}

void resetGame() {
  for (int i = 0; i < MAX_ENEMY; i++) enemies[i].active = false;
  targetLane = 1;
  playerX = laneCenter(targetLane);
  scrollOffset = 0;
  speed = 150;
  elapsed = 0;
  scoreF = 0;
  carsPassed = 0;
  spawnTimer = 0.4f;
}

void trySpawn() {
  for (int attempt = 0; attempt < 6; attempt++) {
    int lane = random(LANES);
    bool ok = true;
    bool seen[LANES] = {false, false, false, false};
    int nearLanes = 0;
    for (int i = 0; i < MAX_ENEMY; i++) {
      if (!enemies[i].active) continue;
      if (enemies[i].lane == lane && enemies[i].y < CAR_H + 90) ok = false;
      if (enemies[i].y < 130 && !seen[enemies[i].lane]) {
        seen[enemies[i].lane] = true;
        nearLanes++;
      }
    }
    if (!ok || nearLanes >= 2) continue;
    for (int i = 0; i < MAX_ENEMY; i++) {
      if (!enemies[i].active) {
        enemies[i].active = true;
        enemies[i].lane = lane;
        enemies[i].y = -CAR_H;
        enemies[i].color = ENEMY_COLORS[random(6)];
        return;
      }
    }
    return;
  }
}

bool updateGame(float dt, int lane) {
  elapsed += dt;
  speed = 150.0f + elapsed * 5.0f;
  if (speed > 450.0f) speed = 450.0f;
  scrollOffset += speed * dt;
  if (scrollOffset > 6000.0f) scrollOffset -= 6000.0f;

  if (lane >= 0) targetLane = lane;

  float tx = laneCenter(targetLane);
  float step = 800.0f * dt;
  if (playerX < tx) { playerX += step; if (playerX > tx) playerX = tx; }
  else if (playerX > tx) { playerX -= step; if (playerX < tx) playerX = tx; }

  spawnTimer -= dt;
  if (spawnTimer <= 0) {
    trySpawn();
    int level = 1 + (int)(elapsed / 10.0f);
    if (level >= 3 && random(100) < 35) trySpawn();
    float spacing = (float)random(150, 260);
    spawnTimer = spacing / speed;
  }

  scoreF += speed * dt * 0.05f;

  for (int i = 0; i < MAX_ENEMY; i++) {
    if (!enemies[i].active) continue;
    enemies[i].y += speed * dt;
    if (enemies[i].y > SCR_H) {
      enemies[i].active = false;
      carsPassed++;
      scoreF += 10;
      continue;
    }
    int ex = laneCenter(enemies[i].lane);
    float ey = enemies[i].y;
    bool hitX = fabsf(playerX - ex) < (CAR_W - 4);
    bool hitY = (ey + CAR_H - 6 > PLAYER_Y + 6) && (ey + 6 < PLAYER_Y + CAR_H - 6);
    if (hitX && hitY) return true;
  }
  return false;
}

void drawCar(int cx, int y, uint16_t col, bool player) {
  int x0 = cx - CAR_W / 2;
  g->fillRect(x0 - 3, y + 8, 4, 13, C_TIRE);
  g->fillRect(x0 - 3, y + CAR_H - 21, 4, 13, C_TIRE);
  g->fillRect(x0 + CAR_W - 1, y + 8, 4, 13, C_TIRE);
  g->fillRect(x0 + CAR_W - 1, y + CAR_H - 21, 4, 13, C_TIRE);
  g->fillRoundRect(x0, y, CAR_W, CAR_H, 9, col);
  g->fillRect(cx - 2, y + 4, 4, CAR_H - 8, player ? C_WHITE : C_BLACK);
  int gy = player ? y + 12 : y + CAR_H - 26;
  g->fillRoundRect(cx - 12, gy, 24, 14, 3, C_GLASS);
  if (player) {
    g->fillRect(x0 + 3, y + 2, 7, 4, C_YELLOW);
    g->fillRect(x0 + CAR_W - 10, y + 2, 7, 4, C_YELLOW);
    g->fillRect(x0 + 3, y + CAR_H - 5, 7, 3, C_RED);
    g->fillRect(x0 + CAR_W - 10, y + CAR_H - 5, 7, 3, C_RED);
  } else {
    g->fillRect(x0 + 3, y + CAR_H - 6, 7, 4, C_YELLOW);
    g->fillRect(x0 + CAR_W - 10, y + CAR_H - 6, 7, 4, C_YELLOW);
    g->fillRect(x0 + 3, y + 2, 7, 3, C_RED);
    g->fillRect(x0 + CAR_W - 10, y + 2, 7, 3, C_RED);
  }
}

void drawRoad() {
  g->fillScreen(C_ROAD);
  int off = (int)scrollOffset % 60;
  for (int d = 1; d < LANES; d++) {
    int x = d * LANE_W - 1;
    for (int y = off - 60; y < SCR_H; y += 60) {
      g->fillRect(x, y, 3, 30, C_WHITE);
    }
  }
  g->fillRect(0, 0, 4, SCR_H, C_YELLOW);
  g->fillRect(SCR_W - 4, 0, 4, SCR_H, C_YELLOW);
}

void drawHud() {
  g->fillRect(0, 0, SCR_W, HUD_H, C_BLACK);
  g->setFont(&fonts::Font2);
  g->setTextColor(C_WHITE);
  g->setTextDatum(lgfx::textdatum_t::middle_left);
  char buf[24];
  snprintf(buf, sizeof(buf), "SCORE %lu", (unsigned long)scoreF);
  g->drawString(buf, 8, HUD_H / 2);
  g->setTextDatum(lgfx::textdatum_t::middle_right);
  int level = 1 + (int)(elapsed / 10.0f);
  snprintf(buf, sizeof(buf), "%d km/h  LV%d", (int)(speed * 0.3f), level);
  g->drawString(buf, SCR_W - 8, HUD_H / 2);
}

void drawWorld() {
  drawRoad();
  for (int i = 0; i < MAX_ENEMY; i++) {
    if (enemies[i].active) drawCar(laneCenter(enemies[i].lane), (int)enemies[i].y, enemies[i].color, false);
  }
  drawCar((int)playerX, PLAYER_Y, C_PLAYER, true);
  drawHud();
}

void drawPanel(const char *title, const char *l1, const char *l2, const char *l3) {
  g->fillRoundRect(20, 80, 200, 170, 14, C_BLACK);
  g->drawRoundRect(20, 80, 200, 170, 14, C_YELLOW);
  g->setTextDatum(lgfx::textdatum_t::middle_center);
  g->setFont(&fonts::Font4);
  g->setTextColor(C_YELLOW);
  g->drawString(title, SCR_W / 2, 110);
  g->setTextColor(C_WHITE);
  g->drawString(l1, SCR_W / 2, 150);
  g->setFont(&fonts::Font2);
  g->drawString(l2, SCR_W / 2, 185);
  g->setTextColor(C_GLASS);
  g->drawString(l3, SCR_W / 2, 220);
}

void present() {
  if (useCanvas) canvas.pushSprite(0, 0);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\nNORVI Traffic Racer (touch)"));

  pinMode(ETH_CS_PIN, OUTPUT);
  digitalWrite(ETH_CS_PIN, HIGH);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  touch.begin();
  tft.init();
  tft.fillScreen(C_BLACK);

  prefs.begin("traffic", false);
  bestScore = prefs.getUInt("best", 0);
  randomSeed(micros() ^ esp_random());

  canvas.setColorDepth(16);
  useCanvas = canvas.createSprite(SCR_W, SCR_H) != nullptr;
  if (!useCanvas) {
    canvas.setPsram(true);
    useCanvas = canvas.createSprite(SCR_W, SCR_H) != nullptr;
  }
  g = useCanvas ? (lgfx::LGFXBase *)&canvas : (lgfx::LGFXBase *)&tft;
  Serial.println(useCanvas ? F("Double buffering ON") : F("Double buffering OFF (may flicker)"));

  resetGame();
  state = ST_TITLE;
  lastMicros = micros();
}

void loop() {
  uint32_t nowUs = micros();
  float dt = (nowUs - lastMicros) / 1000000.0f;
  lastMicros = nowUs;
  if (dt > 0.05f) dt = 0.05f;

  int lane;
  bool tapped;
  readInput(lane, tapped);

  if (state == ST_TITLE) {
    scrollOffset += 120.0f * dt;
    drawRoad();
    drawCar(laneCenter(1), PLAYER_Y, C_PLAYER, true);
    char best[24];
    snprintf(best, sizeof(best), "Best: %lu", (unsigned long)bestScore);
    drawPanel("TRAFFIC RACER", "Avoid the cars!", "Tap a lane to steer", best);
    present();
    if (tapped) {
      resetGame();
      state = ST_PLAYING;
      lastMicros = micros();
    }
  } else if (state == ST_PLAYING) {
    bool crashed = updateGame(dt, lane);
    drawWorld();
    present();
    if (crashed) {
      uint32_t finalScore = (uint32_t)scoreF;
      if (finalScore > bestScore) {
        bestScore = finalScore;
        prefs.putUInt("best", bestScore);
      }
      gameOverAt = millis();
      state = ST_GAMEOVER;
    }
  } else {
    drawWorld();
    g->fillCircle((int)playerX, PLAYER_Y + 10, 28, C_ORANGE);
    g->fillCircle((int)playerX, PLAYER_Y + 10, 16, C_YELLOW);
    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "Score: %lu", (unsigned long)scoreF);
    snprintf(l2, sizeof(l2), "Best: %lu   Passed: %d", (unsigned long)bestScore, carsPassed);
    drawPanel("GAME OVER", l1, l2, "Tap to restart");
    present();
    if (tapped && millis() - gameOverAt > 800) {
      resetGame();
      state = ST_PLAYING;
      lastMicros = micros();
    }
  }
}
