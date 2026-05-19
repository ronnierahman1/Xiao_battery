// If you want to call your existing drawBatteryIcon(), declare a prototype here.
// !!! Do NOT edit your actual drawBatteryIcon() definition elsewhere.
// If your signature differs, adjust ONLY the prototype below or comment the call later.
// Example signatures I've seen in your projects:
//   void drawBatteryIcon(int x, int y, int w, int h, int percent);
//   void drawBatteryIcon(int x, int y, int percent);

// ----------------------------------------------------------------------

#include <Arduino.h>
#include "TFT_eSPI.h"     // EPaper class is available when EPAPER_ENABLE is defined in User_Setup.h

#ifdef EPAPER_ENABLE
EPaper epaper;
#endif

/************* USER WIRING & CALIBRATION *************/
// Pins: keep these aligned with your PCB nets
#define PIN_BAT              A0    // Divider from BAT_4V2 -> ADC
#define PIN_SYS3V3           A1    // Divider or direct sense of 3V3 rail -> ADC (your fixed PCB)

// Divider ratios (match your resistor values!)
// Example: 200k:200k -> 2.0 ; 330k:100k -> 4.3 ; direct 3V3 (no divider) -> 1.0
#define BAT_DIVIDER_RATIO    2.0f
#define SYS3V3_DIVIDER_RATIO 1.0f   // set to your actual ratio for the 3V3 sense path

// ADC / reference
#define ADC_MAX_COUNTS       4095.0f
#define ADC_REF_VOLTAGE      3.30f
#define CAL_GAIN_BAT         1.00f   // fine trim vs DMM (e.g. 0.985..1.015)
#define CAL_OFFS_BAT         0.00f
#define CAL_GAIN_3V3         1.00f
#define CAL_OFFS_3V3         0.00f

// ---------- Types FIRST (avoid Arduino auto-prototype issues) ----------
struct PowerInfo {
  float vbat;         // Battery voltage (V)
  int   pct;          // Estimated %
  float v3v3;         // System 3V3 rail (V) - THIS IS WHAT YOUR PCB SENSES
  const char* state;  // "Charging", "Charged", "On battery"
  int   rawBat;       // Raw ADC counts (debug)
  int   raw3v3;       // Raw ADC counts (debug)
};



// Li-ion 1S % mapping (conservative)
static int voltageToPercent(float v) {
  if (v <= 3.00f) return 0;
  if (v >= 4.20f) return 100;
  if (v < 3.50f)  return (int)((v - 3.00f) / 0.50f * 12.0f);        // 0–12%
  if (v < 3.80f)  return 12 + (int)((v - 3.50f) / 0.30f * 38.0f);   // 12–50%
  if (v < 4.00f)  return 50 + (int)((v - 3.80f) / 0.20f * 35.0f);   // 50–85%
  return 85 + (int)((v - 4.00f) / 0.20f * 15.0f);                   // 85–100%
}

#define CHARGED_VBAT         4.18f   // plateau threshold for "Charged"
#define RISE_THRESH_V_PER_MIN 0.010f // 10 mV/min indicates "Charging"

/************* Helpers *************/
static float readVoltageFromADC(int pin, float ratio, float gain, float offs) {
  const int N = 8;
  uint32_t acc = 0;
  for (int i = 0; i < N; ++i) { acc += analogRead(pin); delay(2); }
  float counts = acc / (float)N;
  if (counts > 4090) counts = 4090;  // guard occasional clip
  float v = (counts / ADC_MAX_COUNTS) * ADC_REF_VOLTAGE * ratio;
  v = v * gain + offs;
  return v;
}

static PowerInfo samplePower() {
  PowerInfo p{};
  analogReadResolution(12);
#if defined(ESP32) // XIAO ESP32-C3
  // Prevents the 4095 saturation that produced 6.60V
  analogSetAttenuation(ADC_11db);              // global
  analogSetPinAttenuation(PIN_BAT,   ADC_11db);
  analogSetPinAttenuation(PIN_SYS3V3, ADC_11db);
#endif

  p.rawBat  = analogRead(PIN_BAT);
  p.raw3v3  = analogRead(PIN_SYS3V3);

  p.vbat = readVoltageFromADC(PIN_BAT,    BAT_DIVIDER_RATIO,    CAL_GAIN_BAT,  CAL_OFFS_BAT);
  p.v3v3 = readVoltageFromADC(PIN_SYS3V3, SYS3V3_DIVIDER_RATIO, CAL_GAIN_3V3,  CAL_OFFS_3V3);
  p.pct  = voltageToPercent(p.vbat);
  p.state = "On battery"; // filled by slope logic below in loop()
  return p;
}

/************* Slope-based charging heuristic (no VBUS line available) *************/
static const char* inferChargeStateFromSlope(float v_now) {
  static float v_prev = NAN;
  static uint32_t t_prev = 0;

  uint32_t t_now = millis();
  float stateRise = NAN;
  const float dt_min = (t_now - t_prev) / 60000.0f;

  if (!isnan(v_prev) && dt_min > 0.5f) {
    stateRise = (v_now - v_prev) / dt_min;    // V per minute
  }

  const bool rising = (!isnan(stateRise) && stateRise > RISE_THRESH_V_PER_MIN);
  const bool topped = (v_now >= CHARGED_VBAT);

  const char* s = "On battery";
  if (rising && !topped) s = "Charging";
  else if (topped && (!isnan(stateRise) ? stateRise <= 0.0f : true)) s = "Charged";

  v_prev = v_now; t_prev = t_now;
  return s;
}

static void drawBatteryIcon(float level, int x, int y, int w, int h)
{  
  int topX1, topY1, bodyX1, bodyY1, bodyY2;
  //draw battery head
  topX1 = x ;
  topY1 = y + h/2 - 3 ;
  epaper.drawRect(topX1, topY1, w/10, 6, TFT_BLACK);

  bodyX1 = topX1 + w/10;
  bodyY1 = y;
  bodyY2 = y + h;
  // draw battery body
  epaper.drawRect(bodyX1, bodyY1, w - w/10, h, TFT_BLACK);
  // draw battery level in 3 reactangle
  int emptyLevel = (int)((w - w/10 -4) * constrain(100.0-level, 0, 100) / 100.0f);
  int chargeLevel = (int)((w - w/10 -4) * constrain(level, 0, 100) / 100.0f);
  epaper.fillRect(bodyX1+2+emptyLevel, bodyY1+2, chargeLevel, h - 4, TFT_BLACK);
  
  // int textX1 = x+w+5;
  // int textY1 = y+5;
  epaper.setTextSize(1);
  epaper.drawString( String((int)level) + "%", x+w+5, y+1);

}

/************* Drawing *************/
static void drawBatteryPanel(PowerInfo p) {
  // Full screen; adjust to a quadrant if you need (x,y,w,h)
  const int W = epaper.width();
  const int H = epaper.height();

  epaper.fillRect(0, 0, W, H, TFT_WHITE);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  // Title
  epaper.setTextSize(2);
  epaper.drawString("Power Status", 8, 8);
  epaper.drawFastHLine(0, 30, W, TFT_BLACK);

  // Body
  int y = 44;
  char line[64];

  epaper.setTextSize(3);
  snprintf(line, sizeof(line), "VBAT: %.2f V", p.vbat);
  epaper.drawString(line, 8, y); y += 28;

  snprintf(line, sizeof(line), "Level: %d %%", p.pct);
  epaper.drawString(line, 8, y); y += 28;

  epaper.setTextSize(2);
  snprintf(line, sizeof(line), "State: %s", p.state);
  epaper.drawString(line, 8, y); y += 22;

  // Honest label for the rail you are actually sensing on this PCB
  snprintf(line, sizeof(line), "3V3 rail: %.2f V", p.v3v3);
  epaper.drawString(line, 8, y); y += 18;

  // Simple gauge
  y += 6;
  int gx = 8, gw = W - 16, gh = 12;
  int filled = (int)(gw * constrain(p.pct, 0, 100) / 100.0f);
  epaper.drawRect(gx, y, gw, gh, TFT_BLACK);
  if (filled > 2) epaper.fillRect(gx + 1, y + 1, filled - 2, gh - 2, TFT_BLACK);

  // Optional: call your existing battery icon without changing it
  // Adjust the call to match your actual signature or comment it out.
  // drawBatteryIcon(W - 60, 8, 50, 24, p.pct);    // example placement/top-right

  // Debug (small)
  y += gh + 8;
  epaper.setTextSize(1);
  snprintf(line, sizeof(line), "RawBAT:%d Raw3V3:%d", p.rawBat, p.raw3v3);
  epaper.drawString(line, 8, y);
}

/************* Arduino entry points *************/
void setup() {
#ifdef EPAPER_ENABLE
  // First sample & paint
  PowerInfo p = samplePower();
  epaper.begin();
  epaper.fillScreen(TFT_WHITE);

  p.state = inferChargeStateFromSlope(p.vbat); // initializes internal history
  drawBatteryPanel(p);
    drawBatteryIcon(p.pct, 10, 250, 50,25);
    drawBatteryIcon(p.pct, 10, 310, 40,20);
    drawBatteryIcon(p.pct, 10, 360, 30,15);
    drawBatteryIcon(p.pct, 10, 400, 20,10);
    drawBatteryIcon(p.pct, epaper.width()-50, 5, 20,10);
  epaper.update(); // push to glass
#endif

  Serial.begin(115200);
  delay(50);
}

void loop() {
#ifdef EPAPER_ENABLE
  static uint32_t last = 0;
  PowerInfo p = samplePower();
  if (millis() - last >= 10000UL) {   // auto-refresh every 10 s
    last = millis();

    p.state = inferChargeStateFromSlope(p.vbat);

    drawBatteryPanel(p);
    drawBatteryIcon(p.pct, 10, 250, 50,25);
    drawBatteryIcon(p.pct, 10, 310, 40,20);
    drawBatteryIcon(p.pct, 10, 360, 30,15);
    drawBatteryIcon(p.pct, 10, 400, 20,10);
    drawBatteryIcon(p.pct, epaper.width()-50, 5, 20,10);
    epaper.update();  // IMPORTANT: ePaper needs explicit update

    // Serial log
    Serial.printf("VBAT=%.3fV  %d%%  3V3=%.3fV  State=%s  RawBAT=%d Raw3V3=%d\n",
                  p.vbat, p.pct, p.v3v3, p.state, p.rawBat, p.raw3v3);
  }
#endif
}
