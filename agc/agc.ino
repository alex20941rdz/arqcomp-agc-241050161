// =====================================================================
//  AGC en ESP32 — Laboratorio 11 (Paso 3: Controlador DMA - CORREGIDO)
// =====================================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

typedef int16_t palabra_t;

struct CPU {
  uint16_t  PC;
  palabra_t A;
  uint16_t  Q;
  palabra_t IR;
  bool      detenida;
  uint32_t  ciclos;
};

void imprimirAncla() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t sal = esp_random() % 100000;
  Serial.printf("ANCLA %06llX-%s-%s-%05lu\n", mac & 0xFFFFFF, __DATE__, __TIME__, sal);
}

// ---------------- Mapa de memoria ----------------
#define RAM_INICIO   0x000
#define RAM_TAM      64
#define IO_INICIO    0x040
#define IO_TAM       32
#define ROM_INICIO   0x100
#define ROM_TAM      640

#define IO_JOY_X     0
#define IO_JOY_Y     1
#define IO_JOY_SW    2
#define IO_TICKS     3
#define IO_ACC_X     4
#define IO_ACC_Y     5
#define IO_ACC_Z     6
#define IO_GYR_X     7
#define IO_GYR_Y     8
#define IO_GYR_Z     9
#define IO_ALARMA    10
#define IO_COLA      11
#define IO_BOTONES   12
#define IO_INCL      13

#define IO_DMA_ORIGEN   14
#define IO_DMA_DESTINO  15
#define IO_DMA_CONTADOR 16
#define IO_DMA_CTRL     17
#define IO_DMA_ESTADO   18

#define VRAM_INICIO  0x800
#define VRAM_TAM     512
palabra_t VRAM[VRAM_TAM];
bool modoVideo = false;

palabra_t RAM[RAM_TAM];
volatile palabra_t IOREG[IO_TAM];
bool alarmaMem = false;

// --- VARIABLES DEL DMA MOVIDAS AQUÍ ARRIBA ---
uint32_t turnosDMA = 0;
uint32_t turnosCPU = 0;
uint32_t dmaT0 = 0, dmaDuracion = 0;
// ---------------------------------------------

palabra_t ROM[ROM_TAM] = {
  0x100B, 0x6108, 0x300A, 0x400A, 0x100B, 0x300C, 0x400B, 0x0100, 0x7000
};

palabra_t leerMem(uint16_t dir) {
  if (dir >= RAM_INICIO && dir < RAM_INICIO + RAM_TAM) return RAM[dir - RAM_INICIO];
  if (dir >= IO_INICIO  && dir < IO_INICIO + IO_TAM)   return IOREG[dir - IO_INICIO];
  if (dir >= ROM_INICIO && dir < ROM_INICIO + ROM_TAM) return ROM[dir - ROM_INICIO];
  if (dir >= VRAM_INICIO && dir < VRAM_INICIO + VRAM_TAM) return VRAM[dir - VRAM_INICIO];
  alarmaMem = true;
  return 0;
}

void escribirMem(uint16_t dir, palabra_t val) {
  if (dir >= RAM_INICIO && dir < RAM_INICIO + RAM_TAM) { RAM[dir - RAM_INICIO] = val; return; }
  if (dir >= IO_INICIO  && dir < IO_INICIO + IO_TAM) {
    if (dir - IO_INICIO != IO_TICKS) IOREG[dir - IO_INICIO] = val;
    return;
  }
  if (dir >= VRAM_INICIO && dir < VRAM_INICIO + VRAM_TAM) { VRAM[dir - VRAM_INICIO] = val; return; }
  alarmaMem = true;
}

// ---------------- CPU ----------------
CPU cpu;

void decodificar(palabra_t ins, uint8_t &op, uint16_t &dir) {
  op  = ((uint16_t)ins >> 12) & 0x0F;
  dir = (uint16_t)ins & 0x0FFF;
}

void cpuReset(CPU &c) {
  c.PC = ROM_INICIO; c.A = 0; c.Q = 0; c.IR = 0;
  c.detenida = false; c.ciclos = 0;
}

void cpuPaso(CPU &c) {
  if (c.detenida) return;
  c.IR = leerMem(c.PC);
  uint16_t pcAnterior = c.PC;
  c.PC++;
  uint8_t op; uint16_t dir;
  decodificar(c.IR, op, dir);
  palabra_t t;
  switch (op) {
    case 0: c.Q = pcAnterior + 1; c.PC = dir; break;
    case 1: c.A = leerMem(dir); break;
    case 2: c.A = -leerMem(dir); break;
    case 3: c.A = c.A + leerMem(dir); break;
    case 4: escribirMem(dir, c.A); break;
    case 5: t = leerMem(dir); escribirMem(dir, c.A); c.A = t; break;
    case 6: if (c.A == 0) c.PC = dir; break;
    case 7: c.detenida = true; break;
    default: c.detenida = true; alarmaMem = true;
  }
  if (alarmaMem) c.detenida = true;
  c.ciclos++;
}

#define N_SUMA 4

void prepararPrograma() {
  RAM[10] = 0;
  RAM[11] = N_SUMA;
  RAM[12] = -1;
  RAM[0] = 20;
  RAM[1] = 10;
  RAM[2] = IO_INICIO + IO_ALARMA;
  cpuReset(cpu);
}

// ---------------- MPU6050 ----------------
#define MPU_DIR 0x68
void mpuDespertar() {
  Wire.beginTransmission(MPU_DIR);
  Wire.write(0x6B); Wire.write(0); Wire.endTransmission();
}
void mpuLeer() {
  Wire.beginTransmission(MPU_DIR);
  Wire.write(0x3B); Wire.endTransmission(false);
  Wire.requestFrom(MPU_DIR, 14);
  int16_t v[7];
  for (int i = 0; i < 7; i++) {
    int16_t alto = Wire.read(); int16_t bajo = Wire.read();
    v[i] = (alto << 8) | (bajo & 0xFF);
  }
  IOREG[IO_ACC_X] = v[0]; IOREG[IO_ACC_Y] = v[1]; IOREG[IO_ACC_Z] = v[2];
  IOREG[IO_GYR_X] = v[4]; IOREG[IO_GYR_Y] = v[5]; IOREG[IO_GYR_Z] = v[6];
}

void leerEntrada() {
  IOREG[IO_JOY_X] = analogRead(34);
  IOREG[IO_JOY_Y] = analogRead(35);
  IOREG[IO_JOY_SW] = digitalRead(32);
  IOREG[IO_BOTONES] = (!digitalRead(25)) | (!digitalRead(26)) << 1 | (!digitalRead(27)) << 2 | (!digitalRead(32)) << 3;
}

// ---------------- Interrupciones y Tareas ----------------
hw_timer_t *timerAGC = nullptr;
volatile uint32_t ticksISR = 0;
volatile bool tareaPendiente = false;
uint32_t periodoMs = 100;

#define COLA_CAP 8
volatile uint8_t colaTareas[COLA_CAP];
volatile uint8_t colaHead = 0, colaTail = 0, colaN = 0;
portMUX_TYPE candadoCola = portMUX_INITIALIZER_UNLOCKED;
volatile bool hay1202 = false;

bool IRAM_ATTR encolar(uint8_t tarea) {
  bool cupo = false;
  portENTER_CRITICAL_SAFE(&candadoCola);
  if (colaN < COLA_CAP) {
    colaTareas[colaTail] = tarea; colaTail = (colaTail + 1) % COLA_CAP; colaN++; cupo = true;
  }
  portEXIT_CRITICAL_SAFE(&candadoCola);
  return cupo;
}

bool desencolar(uint8_t &tarea) {
  bool habia = false;
  portENTER_CRITICAL(&candadoCola);
  if (colaN > 0) {
    tarea = colaTareas[colaHead]; colaHead = (colaHead + 1) % COLA_CAP; colaN--; habia = true;
  }
  portEXIT_CRITICAL(&candadoCola);
  return habia;
}

void IRAM_ATTR pedir(uint8_t tarea) {
  if (!encolar(tarea)) { IOREG[IO_ALARMA] = 1202; hay1202 = true; }
}

void IRAM_ATTR onTimer() {
  ticksISR++; IOREG[IO_TICKS] = (palabra_t)ticksISR;
  pedir(1); pedir(2); pedir(4);
  if (ticksISR % 3 == 0) pedir(3);
}

volatile uint32_t eventosEntr = 0;
volatile uint32_t ultimoEntr = 0;
#define ANTIRREBOTE_MS 50

void IRAM_ATTR onEntr() {
  uint32_t ahora = millis();
  if (ahora - ultimoEntr >= ANTIRREBOTE_MS) {
    eventosEntr++; ultimoEntr = ahora; pedir(3);
  }
}

void iniciarTimer(uint32_t ms) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (timerAGC == nullptr) { timerAGC = timerBegin(1000000); timerAttachInterrupt(timerAGC, &onTimer); }
  timerAlarm(timerAGC, ms * 1000, true, 0);
#else
  if (timerAGC == nullptr) { timerAGC = timerBegin(0, 80, true); timerAttachInterrupt(timerAGC, &onTimer, true); }
  timerAlarmWrite(timerAGC, ms * 1000, true); timerAlarmEnable(timerAGC);
#endif
  periodoMs = ms;
}

// ---------------- DSKY en el OLED ----------------
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
int verbo = 6, sustantivo = 1;

void registrosDelSustantivo(int32_t r[3]) {
  r[0] = r[1] = r[2] = 0;
  switch (sustantivo) {
    case 1:  r[0] = leerMem(RAM[0]); r[1] = leerMem(RAM[1]); r[2] = leerMem(RAM[2]); break;
    case 20: r[0] = IOREG[IO_GYR_X]; r[1] = IOREG[IO_GYR_Y]; r[2] = IOREG[IO_GYR_Z]; break;
    case 21: r[0] = IOREG[IO_COLA];  r[1] = IOREG[IO_ALARMA]; r[2] = ticksISR; break;
    case 22: r[0] = IOREG[IO_DMA_CONTADOR]; r[1] = IOREG[IO_DMA_ESTADO]; r[2] = turnosCPU; break;
    case 23: r[0] = IOREG[IO_INCL]; r[1] = IOREG[IO_ACC_Y]; r[2] = IOREG[IO_ACC_Z]; break;
    case 60: r[0] = IOREG[IO_JOY_X]; r[1] = IOREG[IO_JOY_Y]; r[2] = IOREG[IO_BOTONES]; break;
    case 70: r[0] = cpu.PC; r[1] = cpu.A; r[2] = (uint16_t)cpu.IR; break;
    case 71: r[0] = cpu.Q;  r[1] = cpu.ciclos; r[2] = cpu.detenida; break;
  }
}

void refrescarOLED() {
  int32_t r[3];
  registrosDelSustantivo(r);
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(0, 0);
  if (IOREG[IO_ALARMA] != 0) { oled.fillRect(0, 0, 24, 9, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
  oled.print("PROG"); oled.setTextColor(SSD1306_WHITE);
  oled.printf("  V %02d   N %02d", verbo, sustantivo);
  for (int i = 0; i < 3; i++) {
    oled.setCursor(0, 18 + i * 14);
    if (verbo == 1) oled.printf("R%d  %06lo", i + 1, (unsigned long)(r[i] & 0xFFFF));
    else            oled.printf("R%d  %+06ld", i + 1, (long)r[i]);
  }
  oled.display();
}

// ---------------- Imagen en ROM y memoria de video ----------------
#define IMAGEN_INICIO 0x140

void quemarImagen() {
  for (int y = 0; y < 64; y++) {
    for (int k = 0; k < 8; k++) {
      uint16_t v = 0;
      for (int b = 0; b < 16; b++) {
        int x = k * 16 + b;
        bool encendido = (x == 0 || x == 127 || y == 0 || y == 63 || x / 2 == y || (127 - x) / 2 == y);
        v = (v << 1) | encendido;
      }
      ROM[IMAGEN_INICIO - ROM_INICIO + y * 8 + k] = (palabra_t)v;
    }
  }
}

void dibujarVRAM() {
  oled.clearDisplay();
  for (int k = 0; k < VRAM_TAM; k++) {
    int fila = k / 8;
    int col0 = (k % 8) * 16;
    for (int b = 0; b < 16; b++)
      if ((uint16_t)VRAM[k] & (1 << (15 - b))) oled.drawPixel(col0 + b, fila, SSD1306_WHITE);
  }
  oled.display();
}

// ---------------- Controlador DMA ----------------
bool dmaArrancado() { return IOREG[IO_DMA_CTRL] & 1; }

void dmaUnaPalabra() {
  escribirMem(IOREG[IO_DMA_DESTINO], leerMem(IOREG[IO_DMA_ORIGEN]));
  IOREG[IO_DMA_ORIGEN]   = IOREG[IO_DMA_ORIGEN] + 1;
  IOREG[IO_DMA_DESTINO]  = IOREG[IO_DMA_DESTINO] + 1;
  IOREG[IO_DMA_CONTADOR] = IOREG[IO_DMA_CONTADOR] - 1;
  turnosDMA++;
  if (IOREG[IO_DMA_CONTADOR] <= 0 || alarmaMem) {
    IOREG[IO_DMA_ESTADO] = alarmaMem ? 3 : 2;
    IOREG[IO_DMA_CTRL] = IOREG[IO_DMA_CTRL] & ~1;
    dmaDuracion = micros() - dmaT0;
    pedir(5);
  }
}

void turno(int n) {
  if (dmaArrancado()) {
    if (IOREG[IO_DMA_ESTADO] != 1) {
      IOREG[IO_DMA_ESTADO] = 1;
      dmaT0 = micros(); turnosDMA = 0; turnosCPU = 0;
    }
    if (IOREG[IO_DMA_CTRL] & 2) {
      while (dmaArrancado()) dmaUnaPalabra();
      return;
    }
    if (n % 2 == 0) { dmaUnaPalabra(); return; }
    turnosCPU++;
  }
  cpuPaso(cpu);
}

// ---------------- Instrumento ----------------
#define FILTRO_MAX 64
float ventana[FILTRO_MAX];
int nFiltro = 1, idxV = 0;
float inclCruda = 0, inclFiltrada = 0;
bool modoInstrumento = false;

float filtrar(float x) {
  ventana[idxV] = x; idxV = (idxV + 1) % nFiltro;
  float suma = 0;
  for (int i = 0; i < nFiltro; i++) suma += ventana[i];
  return suma / nFiltro;
}

void cambiarFiltro(int n) {
  nFiltro = constrain(n, 1, FILTRO_MAX);
  for (int i = 0; i < FILTRO_MAX; i++) ventana[i] = inclCruda;
  idxV = 0;
}

void medirInclinacion() {
  float ay = IOREG[IO_ACC_Y]; float az = IOREG[IO_ACC_Z];
  inclCruda = atan2(ay, az) * 180.0 / PI;
  inclFiltrada = filtrar(inclCruda);
  IOREG[IO_INCL] = (palabra_t)(inclFiltrada * 100);
  if (modoInstrumento) Serial.printf("filtrada:%.2f,cruda:%.2f\n", inclFiltrada, inclCruda);
}

// ---------------- Comandos del Monitor Serie ----------------
void atenderSerie() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n'); cmd.trim(); cmd.toUpperCase();
  if (cmd == "M") { reporteTiempos(); return; }
  if (cmd == "D") {
    Serial.printf("DMA: turnos quitados a la CPU=%lu  turnos que la CPU uso=%lu  duracion=%lu us  estado=%d\n",
                  (unsigned long)turnosDMA, (unsigned long)turnosCPU, (unsigned long)dmaDuracion, IOREG[IO_DMA_ESTADO]);
    return;
  }
  if (cmd.startsWith("W")) {
    int coma = cmd.indexOf(',');
    if (coma > 1) {
      uint16_t dir = strtol(cmd.substring(1, coma).c_str(), nullptr, 16);
      palabra_t val = (palabra_t)strtol(cmd.substring(coma + 1).c_str(), nullptr, 16);
      escribirMem(dir, val);
      Serial.printf("OK %03X = %04X\n", dir, (uint16_t)val);
    }
    return;
  }
  if (cmd == "B") {
    Serial.flush(); uint32_t t0 = micros();
    for (int i = 0; i < 1024; i++) Serial.write('U');
    Serial.flush(); uint32_t dt = micros() - t0;
    Serial.printf("\n1024 bytes en %lu us\n", (unsigned long)dt);
    return;
  }
  if (cmd.startsWith("F")) { cambiarFiltro(cmd.substring(1).toInt()); Serial.printf("Filtro = %d muestras\n", nFiltro); return; }
  if (cmd.startsWith("T")) {
    long ms = cmd.substring(1).toInt();
    if (ms >= 1 && ms <= 1000) { iniciarTimer(ms); Serial.printf("Periodo = %ld ms\n", ms); }
    return;
  }
  if (cmd.startsWith("V")) {
    int v = cmd.substring(1, 3).toInt(); int n = cmd.indexOf('N');
    if (v == 15) { cpuPaso(cpu); }
    else if (v == 16) { while (!cpu.detenida) cpuPaso(cpu); }
    else if (v == 37) { cpuReset(cpu); }
    else if (v == 35) { modoVideo = !modoVideo; }
    else if (v == 36) { memset(VRAM, 0, sizeof(VRAM)); }
    else { verbo = v; modoInstrumento = (v == 27); }
    if (n > 0) sustantivo = cmd.substring(n + 1).toInt();
    Serial.printf("V %02d  N %02d\n", verbo, sustantivo);
  }
}

// ---------------- Tareas ----------------
uint32_t tiempoTotal[6] = {0};
uint32_t vecesTarea[6]  = {0};

void ejecutarTarea(uint8_t tarea) {
  uint32_t t0 = micros();
  switch (tarea) {
    case 1: mpuLeer(); medirInclinacion(); break;
    case 2: leerEntrada(); break;
    case 3: if (modoVideo) dibujarVRAM(); else refrescarOLED(); break;
    case 4: for (int i = 0; i < 10; i++) turno(i); break;
    case 5: escribirMem(21, 1); break;
  }
  tiempoTotal[tarea] += micros() - t0; vecesTarea[tarea]++;
}

void reporteTiempos() {
  for (int t = 1; t <= 5; t++) {
    uint32_t prom = vecesTarea[t] ? tiempoTotal[t] / vecesTarea[t] : 0;
    Serial.printf("Tarea %d: %lu us en promedio (%lu veces)\n", t, (unsigned long)prom, (unsigned long)vecesTarea[t]);
    tiempoTotal[t] = 0; vecesTarea[t] = 0;
  }
}

void recuperar1202() {
  portENTER_CRITICAL(&candadoCola);
  colaHead = 0; colaTail = 0; colaN = 0;
  portEXIT_CRITICAL(&candadoCola);
  cpuReset(cpu); encolar(1); RAM[20]++; hay1202 = false;
}

uint32_t ultimoReporte = 0;

void setup() {
  Serial.begin(115200); delay(300); imprimirAncla();
  pinMode(32, INPUT_PULLUP); pinMode(25, INPUT_PULLUP); pinMode(26, INPUT_PULLUP); pinMode(27, INPUT_PULLUP);
  Wire.begin(21, 22); Wire.setClock(400000); mpuDespertar();
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED no responde en 0x3C");
  prepararPrograma();
  quemarImagen();
  attachInterrupt(digitalPinToInterrupt(32), onEntr, FALLING);
  iniciarTimer(100);
  Serial.println("AGC listo. Escribe V06N20 y presiona Enter.");
}

void loop() {
  atenderSerie();
  uint8_t tarea;
  if (desencolar(tarea)) ejecutarTarea(tarea);
  IOREG[IO_COLA] = colaN;
  if (hay1202) recuperar1202();
  if (!modoInstrumento && millis() - ultimoReporte >= 1000) {
    ultimoReporte = millis(); int32_t r[3];
    registrosDelSustantivo(r);
    Serial.printf("T=%lu V=%02d N=%02d R1=%ld R2=%ld R3=%ld\n", (unsigned long)ticksISR, verbo, sustantivo, (long)r[0], (long)r[1], (long)r[2]);
  }
}
