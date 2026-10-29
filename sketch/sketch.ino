/*
 * NORVI X1 + X-R4 + X-Q8 + X-DI4 + XY-MD02 (CPU RS485)
 * R4 CH1 & Q8 CH1 : 3s ON / 3s OFF
 * DI4 CH1         : 24V digital input status
 * XY-MD02         : Temp & Humidity via Modbus RTU on CPU RS485
 */

#include <Wire.h>
#include <SPI.h>
#include <PCA9538.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------- Display ----------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX(void) {
    {
      auto c = _bus.config();
      c.spi_host    = SPI2_HOST;
      c.spi_mode    = 0;
      c.freq_write  = 40000000;
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
      c.pin_cs           = 45;
      c.pin_rst          = 47;
      c.pin_busy         = -1;
      c.panel_width      = 240;
      c.panel_height     = 320;
      c.offset_x         = 0;
      c.offset_y         = 0;
      c.offset_rotation  = 0;
      c.readable         = false;
      c.invert           = true;
      c.rgb_order        = false;
      c.dlen_16bit       = false;
      c.bus_shared       = false;
      _panel.config(c);
    }
    setPanel(&_panel);
  }
};
LGFX tft;

// ---------- I2C expanders ----------
#define R4_ADDR   0x73   // DIP1 OFF, DIP2 OFF
#define Q8_ADDR   0x70   // DIP1 ON,  DIP2 ON
#define SDA_PIN   8
#define SCL_PIN   9
#define PCA_RESET 21
#define CH1       1

PCA9538 r4(R4_ADDR);
PCA9538 q8(Q8_ADDR);

// ---------- DI4 (direct GPIO) ----------
#define DI1_PIN   5      // DI4 Channel 1

// ---------- CPU RS485 (XY-MD02) ----------
#define RS485_TX  15
#define RS485_RX  16
HardwareSerial RS485(1);   // UART1

// XY-MD02 defaults
#define MD02_ADDR     1
#define MD02_BAUD     9600
#define REG_TEMP      0x0001
#define REG_HUMI      0x0002

// ---------- Timing ----------
const unsigned long ON_MS  = 3000;
const unsigned long OFF_MS = 3000;
bool outputsOn = false;
unsigned long lastToggle = 0;
unsigned long cycleCount = 0;
unsigned long lastMd02Poll = 0;
const unsigned long MD02_INTERVAL = 2000;

float temperature = NAN;
float humidity    = NAN;
bool  md02Ok      = false;
bool  diState     = false;

// ---------- CRC16 Modbus ----------
uint16_t modbusCRC(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else         crc >>= 1;
    }
  }
  return crc;
}

// Read two holding registers from MD02 (temp + humidity)
bool readMD02() {
  uint8_t req[8];
  req[0] = MD02_ADDR;
  req[1] = 0x03;                 // Read Holding Registers
  req[2] = highByte(REG_TEMP);
  req[3] = lowByte(REG_TEMP);
  req[4] = 0x00;
  req[5] = 0x02;                 // 2 registers
  uint16_t crc = modbusCRC(req, 6);
  req[6] = lowByte(crc);
  req[7] = highByte(crc);

  // Flush RX
  while (RS485.available()) RS485.read();

  RS485.write(req, 8);
  RS485.flush();

  // Wait for response: addr + func + bytecount + 4 data + 2 crc = 9 bytes
  uint8_t resp[9];
  uint8_t got = 0;
  unsigned long start = millis();
  while (got < 9 && millis() - start < 500) {
    if (RS485.available()) {
      resp[got++] = RS485.read();
    }
  }
  if (got < 9) return false;
  if (resp[0] != MD02_ADDR || resp[1] != 0x03 || resp[2] != 4) return false;

  uint16_t rxCrc = (resp[8] << 8) | resp[7];
  if (modbusCRC(resp, 7) != rxCrc) return false;

  int16_t tRaw = (resp[3] << 8) | resp[4];
  int16_t hRaw = (resp[5] << 8) | resp[6];
  temperature = tRaw / 10.0f;
  humidity    = hRaw / 10.0f;
  return true;
}

// ---------- UI ----------
uint16_t colBg     = 0;
uint16_t colCard   = 0;
uint16_t colAccent = 0;
uint16_t colOn     = 0;
uint16_t colOff    = 0;
uint16_t colMuted  = 0;

void initColors() {
  colBg     = tft.color565(12, 16, 28);
  colCard   = tft.color565(22, 28, 48);
  colAccent = tft.color565(0, 170, 255);
  colOn     = tft.color565(0, 200, 100);
  colOff    = tft.color565(200, 50, 60);
  colMuted  = tft.color565(140, 150, 170);
}

void drawHeader() {
  tft.fillRect(0, 0, 240, 36, tft.color565(18, 30, 60));
  tft.setTextColor(TFT_WHITE);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setCursor(16, 24);
  tft.print("NORVI Control");
  tft.fillRect(0, 36, 240, 2, colAccent);
}

void drawStatusPill(int x, int y, bool on) {
  uint16_t c = on ? colOn : colOff;
  tft.fillRoundRect(x, y, 56, 20, 10, c);
  tft.setTextColor(TFT_WHITE);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setCursor(on ? x + 14 : x + 12, y + 15);
  tft.print(on ? "ON" : "OFF");
}

void drawSmallCard(int y, const char* title, const char* value, bool highlight) {
  tft.fillRoundRect(8, y, 224, 48, 8, colCard);
  tft.drawRoundRect(8, y, 224, 48, 8, highlight ? colAccent : tft.color565(40, 50, 70));

  tft.setTextColor(colMuted);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setCursor(18, y + 18);
  tft.print(title);

  tft.setTextColor(TFT_WHITE);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setCursor(18, y + 40);
  tft.print(value);
}

void drawOutputCard(int y, const char* title, bool on) {
  uint16_t bg = on ? tft.color565(0, 50, 35) : colCard;
  tft.fillRoundRect(8, y, 224, 52, 8, bg);
  tft.drawRoundRect(8, y, 224, 52, 8, on ? colOn : tft.color565(40, 50, 70));

  tft.setTextColor(TFT_WHITE);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setCursor(18, y + 22);
  tft.print(title);

  drawStatusPill(160, y + 16, on);
}

void fullRedraw() {
  tft.fillScreen(colBg);
  drawHeader();

  // Outputs
  drawOutputCard(44,  "R4 Relay CH1", outputsOn);
  drawOutputCard(100, "Q8 Transistor CH1", outputsOn);

  // DI4
  char diBuf[24];
  snprintf(diBuf, sizeof(diBuf), "%s", diState ? "ACTIVE (24V)" : "IDLE");
  drawSmallCard(156, "DI4 Channel 1", diBuf, diState);

  // MD02
  char mdBuf[32];
  if (md02Ok) {
    snprintf(mdBuf, sizeof(mdBuf), "%.1f C  %.1f %%", temperature, humidity);
  } else {
    snprintf(mdBuf, sizeof(mdBuf), "No response");
  }
  drawSmallCard(210, "XY-MD02 (RS485)", mdBuf, md02Ok);

  // Footer
  tft.fillRect(0, 268, 240, 52, tft.color565(15, 20, 35));
  tft.setTextColor(colMuted);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setCursor(12, 290);
  tft.printf("Cycle #%lu", cycleCount);

  unsigned long elapsed = millis() - lastToggle;
  unsigned long phaseMs = outputsOn ? ON_MS : OFF_MS;
  float progress = constrain((float)elapsed / phaseMs, 0.0f, 1.0f);
  int barW = (int)(216 * progress);
  tft.fillRoundRect(12, 300, 216, 8, 4, tft.color565(35, 45, 65));
  tft.fillRoundRect(12, 300, barW, 8, 4, colAccent);
}

void updateFooterOnly() {
  unsigned long elapsed = millis() - lastToggle;
  unsigned long phaseMs = outputsOn ? ON_MS : OFF_MS;
  float progress = constrain((float)elapsed / phaseMs, 0.0f, 1.0f);
  int barW = (int)(216 * progress);
  tft.fillRoundRect(12, 300, 216, 8, 4, tft.color565(35, 45, 65));
  tft.fillRoundRect(12, 300, barW, 8, 4, colAccent);
}

void setOutputs(bool on) {
  r4.digitalWrite(CH1, on ? HIGH : LOW);
  q8.digitalWrite(CH1, on ? HIGH : LOW);
  outputsOn = on;
  if (on) cycleCount++;
  Serial.printf("[%lu] Outputs %s\n", millis(), on ? "ON" : "OFF");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("NORVI X1 | R4+Q8+DI4+MD02");

  // PCA reset
  pinMode(PCA_RESET, OUTPUT);
  digitalWrite(PCA_RESET, LOW);
  delay(10);
  digitalWrite(PCA_RESET, HIGH);
  delay(50);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(30);

  r4.pinMode(CH1, OUTPUT);
  q8.pinMode(CH1, OUTPUT);
  r4.digitalWrite(CH1, LOW);
  q8.digitalWrite(CH1, LOW);

  // DI4
  pinMode(DI1_PIN, INPUT);

  // RS485 for MD02 (auto direction on CPU)
  RS485.begin(MD02_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);

  // Display
  tft.init();
  tft.setRotation(0);
  initColors();
  fullRedraw();

  lastToggle = millis();
  setOutputs(true);
  fullRedraw();
}

void loop() {
  unsigned long now = millis();

  // Output toggle
  unsigned long phaseMs = outputsOn ? ON_MS : OFF_MS;
  if (now - lastToggle >= phaseMs) {
    lastToggle = now;
    setOutputs(!outputsOn);
    fullRedraw();
  }

  // DI4 read
  bool newDi = digitalRead(DI1_PIN);
  if (newDi != diState) {
    diState = newDi;
    fullRedraw();
    Serial.printf("DI4 CH1 = %s\n", diState ? "HIGH" : "LOW");
  }

  // MD02 poll
  if (now - lastMd02Poll >= MD02_INTERVAL) {
    lastMd02Poll = now;
    bool ok = readMD02();
    if (ok != md02Ok || ok) {
      md02Ok = ok;
      fullRedraw();
      if (ok) {
        Serial.printf("MD02  T=%.1f C  H=%.1f %%\n", temperature, humidity);
      } else {
        Serial.println("MD02  no response");
      }
    }
  }

  updateFooterOnly();
  delay(40);
}
