#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// NORVI X1 display configuration (from official test firmware)
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

// Ball state
int ballX = 120;
int ballY = 160;
int radius = 18;
int dx = 4;
int dy = 3;
uint16_t ballColor = TFT_RED;

uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA, TFT_WHITE};
int colorIndex = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  tft.init();
  tft.setRotation(0);          // 240 wide x 320 tall
  tft.fillScreen(TFT_BLACK);

  Serial.println("NORVI X1 - bouncing ball animation ready");
}

void loop() {
  // Erase previous ball (draw black circle)
  tft.fillCircle(ballX, ballY, radius, TFT_BLACK);

  // Move
  ballX += dx;
  ballY += dy;

  // Bounce off edges and change color on hit
  if (ballX - radius <= 0 || ballX + radius >= tft.width()) {
    dx = -dx;
    ballX = constrain(ballX, radius, tft.width() - radius);
    colorIndex = (colorIndex + 1) % 7;
    ballColor = colors[colorIndex];
  }
  if (ballY - radius <= 0 || ballY + radius >= tft.height()) {
    dy = -dy;
    ballY = constrain(ballY, radius, tft.height() - radius);
    colorIndex = (colorIndex + 1) % 7;
    ballColor = colors[colorIndex];
  }

  // Draw new ball
  tft.fillCircle(ballX, ballY, radius, ballColor);

  delay(16);  // ~60 fps
}
