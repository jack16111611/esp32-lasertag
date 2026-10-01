/*
 * PROJECT: Society 1v1 ESP32 Advanced Laser Tag - GOD TIER BUILD
 * PLATFORM: Wokwi / Arduino IDE -> ESP32-WROOM-32
 * ENGINE: Dual-Core FreeRTOS, Non-Blocking Audio, NVS Memory, Async WiFi
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <Preferences.h>

// ==========================================
// 1. HARDWARE PINS
// ==========================================
#define PIN_IR_TX      14  
#define PIN_TRIGGER    15  
#define PIN_RELOAD     4   
#define PIN_RX_HEAD    33  
#define PIN_RX_FRONT_L 13  
#define PIN_RX_FRONT_R 12  
#define PIN_RX_BACK_L  27  
#define PIN_RX_BACK_R  26  
#define PIN_HAPTIC     5   
#define PIN_AUDIO      25    
#define PIN_OLED_SDA   21  
#define PIN_OLED_SCL   22  
#define PIN_MUZZLE     2
#define PIN_LDR        34

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);
Preferences prefs;

// ==========================================
// 2. MULTITHREADING & SHARED STATE
// ==========================================
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t PLAYER_ID = 1; // CHANGE TO 2 FOR SECOND GUN

struct PlayerStats {
  int health = 100;
  int maxHealth = 100;
  int ammo = 30;
  int maxAmmo = 30;
  int kills = 0;
  int deaths = 0;
  int classDmg = 10;
  int fireMode = 0; // 0=Single, 1=Burst, 2=Auto
  float overheat = 0.0;
  String className = "ASSAULT";
  String zone = "SAFE";
  String gameMode = "DEATHMATCH"; 
  bool isDead = false;
  bool isJammed = false;
  unsigned long lastActionTime = 0; // For stealth
  
  // Engineer Mine
  String activeMineSSID = "";
};

struct GamePacket {
  uint8_t playerID;
  int health;
  uint8_t cmd; // 0=Idle, 1=SonarPing, 2=Decoy, 3=MineDropped
  float decoyRSSI; 
  float decoyAngle;
  char mineSSID[32];
};

volatile PlayerStats myStats;
volatile GamePacket myPacket;
volatile GamePacket enemyPacket;

// Radar & Enemy Tracking
volatile float ewmaRSSI = -100.0;
volatile float enemyAngle = 0.0;
volatile int enemyLastHealth = 100;
volatile unsigned long lastEnemyPing = 0;

uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Hardware Globals
IRsend irsend(PIN_IR_TX);
IRrecv irrecvHead(PIN_RX_HEAD);
IRrecv irrecvFrontL(PIN_RX_FRONT_L);
IRrecv irrecvFrontR(PIN_RX_FRONT_R);
IRrecv irrecvBackL(PIN_RX_BACK_L);
IRrecv irrecvBackR(PIN_RX_BACK_R);

// ==========================================
// 3. NON-BLOCKING PERIPHERAL ENGINE (Audio/Haptics)
// ==========================================
int audioFreqs[4] = {0};
int audioDurs[4] = {0};
int audioStep = 0;
unsigned long nextAudioTime = 0;

unsigned long hapticEndTime = 0;
unsigned long muzzleEndTime = 0;

void playSequence(int f1, int d1, int f2=0, int d2=0, int f3=0, int d3=0) {
  audioFreqs[0] = f1; audioDurs[0] = d1;
  audioFreqs[1] = f2; audioDurs[1] = d2;
  audioFreqs[2] = f3; audioDurs[2] = d3;
  audioFreqs[3] = 0;  audioDurs[3] = 0;
  audioStep = 0;
  nextAudioTime = millis();
}

void processPeripherals() {
  unsigned long now = millis();
  
  // Audio Sequencer
  if (audioStep < 4 && now >= nextAudioTime) {
    if (audioFreqs[audioStep] > 0) {
      ledcWriteTone(0, audioFreqs[audioStep]);
      nextAudioTime = now + audioDurs[audioStep];
    } else {
      ledcWriteTone(0, 0); // Stop
    }
    audioStep++;
  }

  // Hardware Timers
  digitalWrite(PIN_HAPTIC, (now < hapticEndTime) ? HIGH : LOW);
  digitalWrite(PIN_MUZZLE, (now < muzzleEndTime) ? HIGH : LOW);
}

void triggerHaptic(int duration) { hapticEndTime = millis() + duration; }
void triggerMuzzle() { muzzleEndTime = millis() + 40; }

// ==========================================
// 4. ESP-NOW CALLBACK (IDF 5.x)
// ==========================================
void IRAM_ATTR OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  GamePacket incoming;
  memcpy(&incoming, data, sizeof(incoming));
  
  if(incoming.playerID == PLAYER_ID) return; // Ignore self
  
  portENTER_CRITICAL_ISR(&statsMux);
  memcpy((void*)&enemyPacket, &incoming, sizeof(GamePacket));
  
  // EWMA Smoothing for Radar (80% Old, 20% New)
  float rawRSSI = (float)recv_info->rx_ctrl->rssi;
  
  // Decoy Override
  if (incoming.cmd == 2) {
    ewmaRSSI = incoming.decoyRSSI;
    enemyAngle = incoming.decoyAngle;
  } else {
    ewmaRSSI = (0.2 * rawRSSI) + (0.8 * ewmaRSSI);
  }

  // Kill Confirmed Logic (Enemy died)
  if (incoming.health <= 0 && enemyLastHealth > 0) {
    myStats.kills++;
    prefs.putInt("kills", prefs.getInt("kills", 0) + 1); // Save to NVS
    if(myStats.className == "VAMPIRE") myStats.health = min(myStats.maxHealth, myStats.health + 25);
    playSequence(800, 100, 1200, 200); // Level up sound
  }
  enemyLastHealth = incoming.health;

  // Sonar Ping Alarm (Enemy pinged us)
  if (incoming.cmd == 1) {
    playSequence(2000, 100, 0, 50, 2000, 100); 
  }
  
  lastEnemyPing = millis();
  portEXIT_CRITICAL_ISR(&statsMux);
}

// ==========================================
// 5. CORE 0: WIFI, WEB SERVER, MEDBAYS
// ==========================================
const char* htmlUI = R"rawliteral(
<!DOCTYPE html><html><body style="background:#111;color:#0F0;font-family:monospace;text-align:center;">
<h1>LASER TAG GOD-MODE</h1>
<a href="/assault" style="color:#0FF;">CLASS: ASSAULT</a> | <a href="/sniper" style="color:#0FF;">CLASS: SNIPER</a> | <a href="/vampire" style="color:#0FF;">CLASS: VAMPIRE</a> | <a href="/jug" style="color:#0FF;">CLASS: JUGGERNAUT</a><br><br>
<a href="/revive" style="color:#0F0;">REVIVE</a> | <a href="/nightfall" style="color:#F0F;">MODE: NIGHTFALL</a> | <a href="/dm" style="color:#FF0;">MODE: DEATHMATCH</a>
</body></html>
)rawliteral";

void applyClass(String cls, int hp, int ammo, int dmg) {
  portENTER_CRITICAL(&statsMux);
  myStats.className = cls; myStats.maxHealth = hp; myStats.health = hp;
  myStats.maxAmmo = ammo; myStats.ammo = ammo; myStats.classDmg = dmg;
  myStats.isDead = false;
  portEXIT_CRITICAL(&statsMux);
  server.send(200, "text/html", htmlUI);
}

void TaskComms(void *pvParameters) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(String("LaserTag_Admin_P" + String(PLAYER_ID)).c_str(), "12345678");

  esp_now_init();
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
  esp_now_add_peer(&peerInfo);

  server.on("/", [](){ server.send(200, "text/html", htmlUI); });
  server.on("/assault", [](){ applyClass("ASSAULT", 100, 30, 10); });
  server.on("/sniper", [](){ applyClass("SNIPER", 100, 5, 40); });
  server.on("/vampire", [](){ applyClass("VAMPIRE", 100, 30, 10); });
  server.on("/jug", [](){ applyClass("JUGGERNAUT", 200, 50, 8); });
  server.on("/revive", [](){ applyClass(myStats.className, myStats.maxHealth, myStats.maxAmmo, myStats.classDmg); });
  server.on("/nightfall", [](){ myStats.gameMode = "NIGHTFALL"; server.send(200, "text/html", htmlUI); });
  server.on("/dm", [](){ myStats.gameMode = "DEATHMATCH"; server.send(200, "text/html", htmlUI); });
  server.begin();

  bool scanActive = false;
  unsigned long lastScan = 0;
  String topSSID = "";

  for (;;) {
    server.handleClient();
    unsigned long now = millis();

    // Async Map Scanner
    if (!scanActive && (now - lastScan > 3000)) {
      WiFi.scanNetworks(true, true);
      scanActive = true;
    }

    if (scanActive && WiFi.scanComplete() >= 0) {
      bool inMed = false, inTox = false, inSafe = false;
      int bestRSSI = -100;

      for (int i = 0; i < WiFi.scanComplete(); i++) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        
        if (rssi > bestRSSI) { bestRSSI = rssi; topSSID = ssid; }

        if (ssid == "Medbay_Zone" && rssi > -70) inMed = true;
        if (ssid == "Toxic_Zone" && rssi > -70) inTox = true;
        if (ssid == "Safe_Zone" && rssi < -85) inSafe = true; // Storm damage if weak
        
        // Landmine Logic (Engineer)
        if (ssid == String(enemyPacket.mineSSID) && rssi > -60) {
           portENTER_CRITICAL(&statsMux);
           myStats.health -= 40; // Hit mine
           portEXIT_CRITICAL(&statsMux);
           playSequence(100, 500); // Explosion
           triggerHaptic(800);
        }
      }

      portENTER_CRITICAL(&statsMux);
      if (inMed && myStats.health < myStats.maxHealth && !myStats.isDead) { myStats.health += 5; myStats.zone = "MEDBAY"; playSequence(600, 50); }
      else if (inTox && !myStats.isDead) { myStats.health -= 2; myStats.zone = "TOXIC"; playSequence(150, 50); triggerHaptic(100); }
      else if (inSafe && !myStats.isDead) { myStats.health -= 1; myStats.zone = "STORM"; }
      else { myStats.zone = "GARDEN"; }
      portEXIT_CRITICAL(&statsMux);

      WiFi.scanDelete();
      scanActive = false;
      lastScan = now;
    }

    // Stealth Logic & ESP-NOW Broadcast
    portENTER_CRITICAL(&statsMux);
    bool stealthActive = (now - myStats.lastActionTime > 15000) && myStats.className != "JUGGERNAUT";
    myPacket.playerID = PLAYER_ID;
    myPacket.health = myStats.health;
    portEXIT_CRITICAL(&statsMux);

    if (!stealthActive) {
      esp_now_send(broadcastMac, (uint8_t*)&myPacket, sizeof(myPacket));
    }
    
    // Reset volatile commands
    myPacket.cmd = 0; 
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// 6. CORE 1: COMBAT, OLED, SENSORS
// ==========================================
unsigned long lastFire = 0;
unsigned long lastHit = 0;
unsigned long lastReloadPress = 0;
unsigned long triggerHoldStart = 0;
int burstShotsLeft = 0;

void fireGun(unsigned long now) {
  portENTER_CRITICAL(&statsMux);
  if (myStats.ammo <= 0 || myStats.isDead || myStats.isJammed) { portEXIT_CRITICAL(&statsMux); return; }
  
  myStats.ammo--;
  myStats.overheat += 15.0;
  myStats.lastActionTime = now;
  int dmg = myStats.classDmg;
  bool lowAmmo = (myStats.ammo <= 3);
  portEXIT_CRITICAL(&statsMux);

  // 32-Bit Payload: [ID][Damage][0xFF][0xFF]
  uint32_t payload = ((uint32_t)PLAYER_ID << 24) | ((uint32_t)dmg << 16) | 0xFFFF;
  irsend.sendNEC(payload, 32);

  playSequence(880, 40, 440, 40); // Pew
  triggerMuzzle();
  triggerHaptic(lowAmmo ? 100 : 40); // Stutter if low ammo
  if(lowAmmo) playSequence(150, 50); // Click-clack
  lastFire = now;
}

void TaskCombat(void *pvParameters) {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  pinMode(PIN_RELOAD, INPUT_PULLUP);
  pinMode(PIN_HAPTIC, OUTPUT);
  pinMode(PIN_MUZZLE, OUTPUT);
  ledcSetup(0, 2000, 8); ledcAttachPin(PIN_AUDIO, 0);

  irsend.begin();
  irrecvHead.enableIRIn(); irrecvFrontL.enableIRIn(); irrecvFrontR.enableIRIn();
  irrecvBackL.enableIRIn(); irrecvBackR.enableIRIn();

  decode_results results;

  for (;;) {
    unsigned long now = millis();
    processPeripherals();

    // ── Input & Mechanics ────────────────────────────────────
    bool trg = (digitalRead(PIN_TRIGGER) == LOW);
    bool rld = (digitalRead(PIN_RELOAD) == LOW);

    portENTER_CRITICAL(&statsMux);
    // Overheat Decay
    if (myStats.overheat > 0) myStats.overheat -= 0.5;
    if (myStats.overheat >= 100 && !myStats.isJammed) {
      myStats.isJammed = true;
      playSequence(100, 300, 50, 500); // Jammed sound
    }
    if (myStats.overheat <= 0) myStats.isJammed = false;
    
    int mode = myStats.fireMode;
    String cls = myStats.className;
    bool isDead = myStats.isDead;
    portEXIT_CRITICAL(&statsMux);

    // Tactical: Decoy (Trigger + Reload)
    if (trg && rld) {
      myPacket.cmd = 2; myPacket.decoyRSSI = ewmaRSSI; myPacket.decoyAngle = enemyAngle;
      playSequence(1500, 100); 
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    // Reload / Sonar / Fire Mode Toggle
    if (rld && !trg) {
      if (now - lastReloadPress < 400) { // Double tap
        portENTER_CRITICAL(&statsMux);
        myStats.fireMode = (myStats.fireMode + 1) % 3;
        portEXIT_CRITICAL(&statsMux);
        playSequence(600, 50, 800, 50); 
        vTaskDelay(400 / portTICK_PERIOD_MS);
      } else {
        triggerHoldStart = now;
        while(digitalRead(PIN_RELOAD) == LOW) { processPeripherals(); vTaskDelay(10); }
        
        if (millis() - triggerHoldStart > 1000) { // Long Press = Sonar
          myPacket.cmd = 1; playSequence(2500, 200); 
          portENTER_CRITICAL(&statsMux); myStats.lastActionTime = now; portEXIT_CRITICAL(&statsMux);
        } else { // Normal Reload
          portENTER_CRITICAL(&statsMux); myStats.ammo = myStats.maxAmmo; portEXIT_CRITICAL(&statsMux);
          playSequence(300, 100, 0, 50, 300, 100);
        }
      }
      lastReloadPress = now;
    }

    // Firing Logic
    int fireRate = (cls == "SNIPER") ? 1500 : 200;
    if (trg && !isDead && (now - lastFire > fireRate)) {
      if (mode == 0) { fireGun(now); while(digitalRead(PIN_TRIGGER) == LOW) { processPeripherals(); vTaskDelay(10); } } // Single
      else if (mode == 1 && burstShotsLeft == 0) { burstShotsLeft = 3; } // Burst Init
      else if (mode == 2) { fireGun(now); } // Auto
    }
    
    // Process Burst
    if (burstShotsLeft > 0 && (now - lastFire > 100)) {
      fireGun(now); burstShotsLeft--;
    }

    // ── Hit Detection ─────────────────────────────────────────
    if (!isDead && (now - lastHit > 250)) {
      String loc = ""; uint32_t val = 0;
      if (irrecvHead.decode(&results)) { loc = "HEAD"; val = results.value; irrecvHead.resume(); }
      else if (irrecvBackL.decode(&results)) { loc = "BACK"; val = results.value; irrecvBackL.resume(); }
      else if (irrecvBackR.decode(&results)) { loc = "BACK"; val = results.value; irrecvBackR.resume(); }
      else if (irrecvFrontL.decode(&results)) { loc = "FRONT"; val = results.value; irrecvFrontL.resume(); }
      else if (irrecvFrontR.decode(&results)) { loc = "FRONT"; val = results.value; irrecvFrontR.resume(); }

      if (loc != "" && (val & 0xFFFF) == 0xFFFF) { // Checksum matched
        uint8_t attacker = (val >> 24) & 0xFF;
        if (attacker != PLAYER_ID) {
          int baseDmg = (val >> 16) & 0xFF;
          float mult = (loc == "HEAD") ? 1.8 : (loc == "BACK") ? 1.5 : 1.0;
          
          portENTER_CRITICAL(&statsMux);
          myStats.health = max(0, myStats.health - (int)(baseDmg * mult));
          myStats.lastActionTime = now;
          if (myStats.health == 0) { myStats.isDead = true; prefs.putInt("deaths", prefs.getInt("deaths",0)+1); }
          portEXIT_CRITICAL(&statsMux);

          // Advanced Audio Direction/Distance
          if(myStats.health == 0) { playSequence(100, 800, 50, 1000); triggerHaptic(1000); }
          else {
            if(loc == "BACK") playSequence(600, 100, 400, 100); // 2-Tone Warning
            else if(ewmaRSSI < -80) playSequence(150, 150); // Far sniper THUD
            else playSequence(1000, 50); // Close PING
            triggerHaptic(200);
          }
          
          if(loc == "FRONT") enemyAngle = -PI/2.0;
          else if(loc == "BACK") enemyAngle = PI/2.0;
          
          lastHit = now;
        }
      }
    }

    // ── OLED Rendering ────────────────────────────────────────
    portENTER_CRITICAL(&statsMux);
    bool nightfall = (myStats.gameMode == "NIGHTFALL");
    int hp = myStats.health; int am = myStats.ammo; int kl = myStats.kills;
    String z = myStats.zone; float heat = myStats.overheat;
    portEXIT_CRITICAL(&statsMux);

    // LDR Stealth Dimming
    int lightLevel = analogRead(PIN_LDR);
    display.dim(lightLevel < 1000);

    display.clearDisplay();
    if (!nightfall) {
      // UI
      display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
      display.setCursor(64, 0); display.print("HP:"); display.print(hp);
      display.drawRect(64, 10, 60, 6, SSD1306_WHITE);
      display.fillRect(65, 11, max(0, (int)map(hp, 0, 100, 0, 58)), 4, SSD1306_WHITE);
      display.setCursor(64, 20); display.print("AMMO:"); display.print(am);
      display.setCursor(64, 30); display.print("KILLS:"); display.print(kl);
      display.setCursor(64, 40); display.print(z);
      
      // Overheat Bar
      display.drawRect(64, 50, 60, 4, SSD1306_WHITE);
      display.fillRect(64, 50, max(0, (int)map(heat, 0, 100, 0, 60)), 4, SSD1306_WHITE);

      // Radar
      int cx = 32, cy = 32, r = 30;
      display.drawCircle(cx, cy, r, SSD1306_WHITE); display.fillCircle(cx, cy, 2, SSD1306_WHITE);
      
      if (now - lastEnemyPing < 3000) { // Only show if recent
        int dist = constrain(map((int)ewmaRSSI, -30, -90, 5, 28), 5, 28);
        display.fillCircle(cx + (dist * cos(enemyAngle)), cy + (dist * sin(enemyAngle)), 3, SSD1306_WHITE);
      }

      if (isDead) { display.fillRect(0, 20, 128, 24, SSD1306_BLACK); display.setCursor(10, 28); display.setTextSize(2); display.print("YOU DIED"); }
    }
    display.display();

    vTaskDelay(33 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// 7. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  prefs.begin("lasertag", false);
  myStats.kills = prefs.getInt("kills", 0);
  myStats.deaths = prefs.getInt("deaths", 0);

  xTaskCreatePinnedToCore(TaskComms, "Comms", 20000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskCombat, "Combat", 16000, NULL, 2, NULL, 1);
}

void loop() { vTaskDelete(NULL); }
