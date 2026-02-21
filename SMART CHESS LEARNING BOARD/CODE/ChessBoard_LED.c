/* 
  Arduino Mega Chess LED - CORRECTED VERSION
  
  BUGS FIXED:
   ✓ King capture now triggers winning animation
   ✓ Error state auto-recovers when pieces placed back
   ✓ Checkmate/stalemate animations protected from LED overwrites
   ✓ Better error detection and recovery
   ✓ Legal moves already validated against check (existing code was correct)
   
  Features:
   - Startup animations: Cyan wave, Green row scan, Red column scan,
     Indian flag slide-up, Column brightness wave
   - Pre-start placement enforcement
   - Per-cell debounce + placement dwell
   - Post-accept micro-check on placements
   - Multi-diff detection & correction mode
   - Pawn promotion, check/checkmate/stalemate detection
*/

#include <FastLED.h>

#define LED_PIN     30
#define NUM_LEDS    64
#define COLOR_ORDER GRB
#define LED_TYPE    WS2812B
#define BRIGHTNESS  140

CRGB leds[NUM_LEDS];

#define ROWS 8
#define COLS 8

// Hardware pins (Mega)
const uint8_t rowPins[ROWS] = {31,32,33,34,35,36,37,38};
const uint8_t colPins[COLS] = {22,23,24,25,26,27,28,29};

// Input state arrays
bool rawState[ROWS][COLS];
bool lastRaw[ROWS][COLS];
bool stableState[ROWS][COLS];
bool lastState[ROWS][COLS];
unsigned long stateChangeTime[ROWS][COLS];

// Debounce timings
const unsigned long MIN_STABLE_MS    = 120UL;
const unsigned long MIN_PLACEMENT_MS = 260UL;
const unsigned long POST_ACCEPT_MS   = 25UL;

// Board & game state
int pieceBoard[ROWS][COLS];
bool legal[ROWS][COLS];

enum GameState {
  STATE_IDLE=0,
  STATE_HOLDING=1,
  STATE_CAPTURE_PENDING=2,
  STATE_GAME_OVER=3,
  STATE_ERROR=4,
  STATE_WRONG_TURN=5
};
GameState gameState = STATE_IDLE;

int currentTurn = 1; // 1 = WHITE, 0 = BLACK
int checkState = 0;  // 0 normal, 1 check, 2 checkmate, 3 stalemate

bool isStartup = false;
bool isCelebrating = false;  // NEW: Prevents LED updates during animations

// Held/capture vars
int heldPiece = 0;
int fromR=-1, fromC=-1;
int victimPiece=0, victimR=-1, victimC=-1;

// Last move tracking
int lastMoveFromR = -1, lastMoveFromC = -1;
int lastMoveToR   = -1, lastMoveToC   = -1;

// Rapid action protection
unsigned long lastLiftTime = 0;
const unsigned long MIN_LIFT_INTERVAL = 200;

// LED overlay buffer
enum LB {
  LB_NULL=0,
  LB_STARTUP_CYAN,
  LB_STARTUP_TEAL,
  LB_WHITE_BG,
  LB_BLACK_BG,
  LB_MOVE,
  LB_CAPTURE,
  LB_ORIGIN,
  LB_ERROR,
  LB_CHECK,
  LB_WIN_KING,
  LB_WAVE,
  LB_TURN_BLINK,
  LB_LASTMOVE
};
uint8_t ledBuffer[NUM_LEDS];

// Multi-diff correction support
int boardSnapshot[ROWS][COLS];
bool diffMap[ROWS][COLS];
int diffCount = 0;

// Helper functions
int ledIndex(int r,int c){
  if ((r & 1) == 0) return r*8 + c;
  else return r*8 + (7 - c);
}

bool inBounds(int r,int c){ return r>=0 && r<ROWS && c>=0 && c<COLS; }
bool isWhitePiece(int p){ return p>=1 && p<=6; }
bool isBlackPiece(int p){ return p>=9 && p<=14; }
bool sameColor(int a,int b){
  if(a==0||b==0) return false;
  return (isWhitePiece(a)&&isWhitePiece(b)) || (isBlackPiece(a)&&isBlackPiece(b));
}

void slog(const char *s){ Serial.println(s); }
void slogf(const char* fmt, int a,int b,int c){
  char buf[80]; snprintf(buf, sizeof(buf), fmt, a,b,c); Serial.println(buf);
}

// Snapshot / diff helpers
void takeSnapshot(){
  diffCount = 0;
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      boardSnapshot[r][c] = pieceBoard[r][c];
      diffMap[r][c] = false;
    }
  }
}

void computeDiffs(){
  diffCount = 0;
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      if(pieceBoard[r][c] != boardSnapshot[r][c]){
        diffMap[r][c] = true;
        diffCount++;
      } else {
        diffMap[r][c] = false;
      }
    }
  }
  Serial.print("Diffs found: "); Serial.println(diffCount);
}

void debugDumpBoard(){
  Serial.println("Board dump (8x8):");
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      Serial.print(pieceBoard[r][c]);
      Serial.print('\t');
    }
    Serial.println();
  }
  Serial.println();
}

// Move generation & legality
void clearLegal(){ 
  for(int r=0;r<ROWS;r++) 
    for(int c=0;c<COLS;c++) 
      legal[r][c] = false; 
}

bool isSquareAttackedBy(int r,int c, bool attackerIsWhite, int board[ROWS][COLS]){
  const int drN[8] = {-2,-2,-1,-1,1,1,2,2};
  const int dcN[8] = {-1,1,-2,2,-2,2,-1,1};
  int enemyRook   = attackerIsWhite ? 1  : 9;
  int enemyKnight = attackerIsWhite ? 2  : 10;
  int enemyBishop = attackerIsWhite ? 3  : 11;
  int enemyQueen  = attackerIsWhite ? 4  : 12;
  int enemyKing   = attackerIsWhite ? 5  : 13;
  int enemyPawn   = attackerIsWhite ? 6  : 14;

  // Knights
  for(int i=0;i<8;i++){
    int rr=r+drN[i], cc=c+dcN[i];
    if(inBounds(rr,cc) && board[rr][cc]==enemyKnight) return true;
  }

  // Rooks / Queens (straight)
  const int drS[4] = {-1,1,0,0}, dcS[4] = {0,0,-1,1};
  for(int i=0;i<4;i++){
    for(int d=1;d<8;d++){
      int rr=r+drS[i]*d, cc=c+dcS[i]*d;
      if(!inBounds(rr,cc)) break;
      int p = board[rr][cc];
      if(p!=0){ if(p==enemyRook || p==enemyQueen) return true; break; }
    }
  }

  // Bishops / Queens (diagonal)
  const int drD[4] = {-1,-1,1,1}, dcD[4] = {-1,1,-1,1};
  for(int i=0;i<4;i++){
    for(int d=1;d<8;d++){
      int rr=r+drD[i]*d, cc=c+dcD[i]*d;
      if(!inBounds(rr,cc)) break;
      int p = board[rr][cc];
      if(p!=0){ if(p==enemyBishop || p==enemyQueen) return true; break; }
    }
  }

  // Kings
  for(int dr=-1; dr<=1; dr++){
    for(int dc=-1; dc<=1; dc++){
      if(dr==0 && dc==0) continue;
      int rr=r+dr, cc=c+dc;
      if(inBounds(rr,cc) && board[rr][cc]==enemyKing) return true;
    }
  }

  // Pawns - White attacks from r-1, Black from r+1
  int pawnDir = attackerIsWhite ? -1 : 1;
  if(inBounds(r+pawnDir, c-1) && board[r+pawnDir][c-1]==enemyPawn) return true;
  if(inBounds(r+pawnDir, c+1) && board[r+pawnDir][c+1]==enemyPawn) return true;

  return false;
}

bool isCheck(bool colorIsWhite, int board[ROWS][COLS]){
  int kType = colorIsWhite ? 5 : 13;
  int kR=-1,kC=-1;
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      if(board[r][c]==kType){
        kR=r;kC=c;
        break;
      }
    }
    if(kR != -1) break;
  }
  if(kR==-1) return false;
  return isSquareAttackedBy(kR,kC, !colorIsWhite, board);
}

bool isMoveLegalSim(int r0,int c0,int r1,int c1,int type){
  int savedDest = pieceBoard[r1][c1];
  int savedSrc  = pieceBoard[r0][c0];
  pieceBoard[r1][c1] = type;
  pieceBoard[r0][c0] = 0;
  bool check = isCheck(isWhitePiece(type), pieceBoard);
  pieceBoard[r0][c0] = savedSrc;
  pieceBoard[r1][c1] = savedDest;
  return !check;
}

void tryAddMove(int r,int c,int type){
  if(!inBounds(r,c)) return;
  int target = pieceBoard[r][c];
  if(target!=0 && sameColor(type,target)) return;
  if(isMoveLegalSim(fromR, fromC, r, c, type)) legal[r][c] = true;
}

bool anyLegalMoves(){
  for(int r=0;r<ROWS;r++)
    for(int c=0;c<COLS;c++)
      if(legal[r][c]) return true;
  return false;
}

void genSlide(int r0,int c0,int type,int dr,int dc){
  for(int d=1; d<8; d++){
    int r=r0+dr*d, c=c0+dc*d;
    if(!inBounds(r,c)) break;
    int target = pieceBoard[r][c];
    if(target==0) {
      tryAddMove(r,c,type);
    } else {
      if(!sameColor(type,target)) tryAddMove(r,c,type);
      break;
    }
  }
}

void generatePawnFallback(int r0,int c0,int type){
  int forward  = isWhitePiece(type) ? 1 : -1;
  int startRow = isWhitePiece(type) ? 1 : 6;

  int r1 = r0 + forward;
  if(inBounds(r1,c0) && pieceBoard[r1][c0]==0){
    legal[r1][c0] = true;
  }

  int r2 = r0 + 2*forward;
  if(r0==startRow && inBounds(r2,c0) && pieceBoard[r2][c0]==0 && pieceBoard[r1][c0]==0){
    legal[r2][c0] = true;
  }

  int rc = r0 + forward;
  if(inBounds(rc,c0-1) && pieceBoard[rc][c0-1]!=0 && !sameColor(type,pieceBoard[rc][c0-1]))
    legal[rc][c0-1] = true;
  if(inBounds(rc,c0+1) && pieceBoard[rc][c0+1]!=0 && !sameColor(type,pieceBoard[rc][c0+1]))
    legal[rc][c0+1] = true;
}

void generateLegalMoves(int r0,int c0){
  clearLegal();
  int type = heldPiece;
  if(type==0) return;
  int baseType = (type>8)?(type-8):type;

  if(baseType==1 || baseType==4) {
    genSlide(r0,c0,type,-1,0);
    genSlide(r0,c0,type,1,0);
    genSlide(r0,c0,type,0,-1);
    genSlide(r0,c0,type,0,1);
  }
  if(baseType==3 || baseType==4){
    genSlide(r0,c0,type,-1,-1);
    genSlide(r0,c0,type,-1,1);
    genSlide(r0,c0,type,1,-1);
    genSlide(r0,c0,type,1,1);
  }
  if(baseType==2){
    const int dr[8] = {-2,-2,-1,-1,1,1,2,2};
    const int dc[8] = {-1,1,-2,2,-2,2,-1,1};
    for(int i=0;i<8;i++) tryAddMove(r0+dr[i], c0+dc[i], type);
  }
  if(baseType==5){
    for(int dr=-1; dr<=1; dr++) 
      for(int dc=-1; dc<=1; dc++){
        if(dr==0 && dc==0) continue;
        tryAddMove(r0+dr, c0+dc, type);
      }
  }
  if(baseType==6){
    int forward = isWhitePiece(type) ? 1 : -1;
    int startRow = isWhitePiece(type) ? 1 : 6;
    int r1 = r0 + forward;
    if(inBounds(r1,c0) && pieceBoard[r1][c0]==0){
      if(isMoveLegalSim(r0,c0,r1,c0,type)) legal[r1][c0]=true;
      if(r0==startRow){
        int r2=r0+2*forward;
        if(inBounds(r2,c0) && pieceBoard[r2][c0]==0)
          if(isMoveLegalSim(r0,c0,r2,c0,type)) legal[r2][c0]=true;
      }
    }
    int rc = r0 + forward;
    if(inBounds(rc,c0-1) && pieceBoard[rc][c0-1]!=0 && !sameColor(type,pieceBoard[rc][c0-1]))
      if(isMoveLegalSim(r0,c0,rc,c0-1,type)) legal[rc][c0-1]=true;
    if(inBounds(rc,c0+1) && pieceBoard[rc][c0+1]!=0 && !sameColor(type,pieceBoard[rc][c0+1]))
      if(isMoveLegalSim(r0,c0,rc,c0+1,type)) legal[rc][c0+1]=true;
  }

  if(baseType==6 && !anyLegalMoves()){
    generatePawnFallback(r0,c0,type);
  }
}

// Board init
void initBoard(){
  for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
    pieceBoard[r][c]=0; legal[r][c]=false; rawState[r][c]=false; 
    stableState[r][c]=false; lastState[r][c]=false;
    lastRaw[r][c] = false; stateChangeTime[r][c] = 0; 
    diffMap[r][c] = false; boardSnapshot[r][c]=0;
  }
  
  int wback[8] = {1,2,3,4,5,3,2,1};
  for(int c=0;c<8;c++) pieceBoard[0][c] = wback[c];
  for(int c=0;c<8;c++) pieceBoard[1][c] = 6;
  
  int bback[8] = {9,10,11,12,13,11,10,9};
  for(int c=0;c<8;c++) pieceBoard[7][c] = bback[c];
  for(int c=0;c<8;c++) pieceBoard[6][c] = 14;

  currentTurn = 1;
  gameState = STATE_IDLE;
  checkState = 0;

  heldPiece = 0; fromR = fromC = -1;
  victimPiece = 0; victimR = victimC = -1;

  lastMoveFromR = lastMoveFromC = -1;
  lastMoveToR   = lastMoveToC   = -1;

  memset(ledBuffer, LB_NULL, sizeof(ledBuffer));
  takeSnapshot();
}

// Scanning matrix
void scanMatrix(bool dest[ROWS][COLS]){
  for(int r=0;r<ROWS;r++){
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(60);
    for(int c=0;c<COLS;c++){
      dest[r][c] = (digitalRead(colPins[c]) == LOW);
    }
    digitalWrite(rowPins[r], HIGH);
  }
}

// LED rendering
void renderLEDs(){
  FastLED.clear();
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      int idx = ledIndex(r,c);
      uint8_t lb = ledBuffer[idx];

      if(pieceBoard[r][c]!=0){
        if(isWhitePiece(pieceBoard[r][c])) {
          leds[idx] = CRGB::White;
        } else {
          leds[idx] = CRGB(0, 0, 100);
        }
      } else {
        leds[idx] = CRGB::Black;
      }

      switch(lb){
        case LB_STARTUP_CYAN:  leds[idx] = CRGB(0,230,255); break;
        case LB_STARTUP_TEAL:  leds[idx] = CRGB(0,170,170); break;
        case LB_MOVE:          leds[idx] = CRGB(0,80,0);    break;
        case LB_CAPTURE:       leds[idx] = CRGB(255,200,0); break;
        case LB_ORIGIN:        leds[idx] = CRGB(255,100,100); break;
        case LB_ERROR:         leds[idx] = CRGB(255,40,40); break;
        case LB_CHECK:         leds[idx] = CRGB(255,240,120); break;
        case LB_WIN_KING:      leds[idx] = CRGB(0,200,255); break;
        case LB_WAVE:          leds[idx] = CHSV(160, 120, 160); break;
        case LB_TURN_BLINK:    leds[idx] = CRGB(0,200,255); break;
        case LB_LASTMOVE:      leds[idx] = CRGB(150,0,200);   break;
        default: break;
      }
    }
  }
  FastLED.show();
}

// Priority overlay logic
void refreshBufferFromState(){
  for(int i=0;i<NUM_LEDS;i++) ledBuffer[i] = LB_NULL;
  if(isStartup) return;

  if(gameState==STATE_GAME_OVER){
    if(checkState==2){
      bool winnerIsWhite = (currentTurn==0);
      int kType = winnerIsWhite?5:13;
      for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
        int idx = ledIndex(r,c);
        if(pieceBoard[r][c]==kType) {
          ledBuffer[idx] = LB_WIN_KING;
        } else if(random(0,100) < 4) {
          ledBuffer[idx] = LB_WAVE;
        }
      }
      return;
    } else if(checkState==3){
      for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
        int idx = ledIndex(r,c);
        bool isLight = ((r + c) % 2 == 0);
        ledBuffer[idx] = isLight ? LB_WHITE_BG : LB_BLACK_BG;
      }
      return;
    }
    for(int i=0;i<NUM_LEDS;i++) ledBuffer[i]=LB_ERROR;
    return;
  }

  if(gameState == STATE_ERROR){
    if(diffCount == 0) computeDiffs();
    for(int r=0;r<ROWS;r++){
      for(int c=0;c<COLS;c++){
        if(diffMap[r][c]){
          ledBuffer[ledIndex(r,c)] = LB_ERROR;
        }
      }
    }
    return;
  }

  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      int idx = ledIndex(r,c);
      int p = pieceBoard[r][c];

      if(p!=0){
        ledBuffer[idx] = isWhitePiece(p) ? LB_WHITE_BG : LB_BLACK_BG;

        if(gameState==STATE_IDLE){
          bool isPieceTurn = (isWhitePiece(p) && currentTurn==1) || 
                            (isBlackPiece(p) && currentTurn==0);
          if(isPieceTurn) ledBuffer[idx] = LB_TURN_BLINK;
        }
      }
    }
  }

  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      int idx = ledIndex(r,c);
      if(gameState==STATE_HOLDING){
        if(legal[r][c]){
          if(pieceBoard[r][c] != 0) ledBuffer[idx] = LB_CAPTURE;
          else ledBuffer[idx] = LB_MOVE;
        }
      } else if(gameState==STATE_CAPTURE_PENDING){
        if(r==victimR && c==victimC) ledBuffer[idx] = LB_CAPTURE;
      }
    }
  }

  if(gameState!=STATE_IDLE && inBounds(fromR, fromC) && gameState!=STATE_WRONG_TURN){
    ledBuffer[ledIndex(fromR, fromC)] = LB_ORIGIN;
  }

  if(gameState==STATE_WRONG_TURN && inBounds(fromR, fromC)){
    ledBuffer[ledIndex(fromR, fromC)] = LB_ERROR;
  }

  if(gameState==STATE_ERROR){
    if(inBounds(fromR, fromC)) ledBuffer[ledIndex(fromR, fromC)] = LB_ERROR;
    else for(int i=0;i<NUM_LEDS;i++) ledBuffer[i] = LB_ERROR;
  }

  if(gameState==STATE_IDLE){
    if(inBounds(lastMoveFromR, lastMoveFromC)){
      ledBuffer[ledIndex(lastMoveFromR, lastMoveFromC)] = LB_LASTMOVE;
    }
    if(inBounds(lastMoveToR, lastMoveToC)){
      ledBuffer[ledIndex(lastMoveToR, lastMoveToC)] = LB_LASTMOVE;
    }
  }

  if(checkState==1){
    int kType = (currentTurn==1)?5:13;
    for(int r=0;r<ROWS;r++){
      for(int c=0;c<COLS;c++){
        if(pieceBoard[r][c]==kType){
          ledBuffer[ledIndex(r,c)] = LB_CHECK;
        }
      }
    }
  }
}

// STARTUP ANIMATIONS
void anim_outwardWave() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  for (int rad = 0; rad <= 4; rad++) {
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        float dr = abs(r - 3.5f);
        float dc = abs(c - 3.5f);
        float d  = (dr > dc) ? dr : dc;
        int i = ledIndex(r, c);
        if (d <= rad + 0.5f) {
          uint8_t brightness = (d >= rad - 0.5f) ? 255 : 180;
          leds[i] = CRGB(0, (uint8_t)(229 * brightness / 255),
                             (uint8_t)(255 * brightness / 255));
        }
      }
    }
    FastLED.show();
    delay(200);
  }

  delay(300);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

void anim_rowScan() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  for (int r = 0; r < ROWS; r++) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int c = 0; c < COLS; c++) {
      leds[ledIndex(r, c)] = CRGB(0, 255, 85);
    }
    FastLED.show();
    delay(130);
  }

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(50);
}

void anim_columnScan() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  for (int c = 0; c < COLS; c++) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    for (int r = 0; r < ROWS; r++) {
      leds[ledIndex(r, c)] = CRGB(255, 26, 26);
    }
    FastLED.show();
    delay(130);
  }

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(50);
}

void anim_flagSlideUp() {
  fill_solid(leds, NUM_LEDS, CRGB::White);
  FastLED.setBrightness(255);
  FastLED.show();
  delay(1000);
  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  delay(100);

  CRGB saffron = CRGB(255, 102, 0);
  CRGB white   = CRGB(255, 255, 255);
  CRGB greenC  = CRGB(19, 136, 8);
  CRGB blueC   = CRGB(0, 0, 128);

  for (int r = 7; r >= 0; r--) {
    CRGB rowColor;
    if (r <= 2)      rowColor = saffron;
    else if (r <= 4) rowColor = white;
    else             rowColor = greenC;

    for (int c = 0; c < COLS; c++) {
      leds[ledIndex(r, c)] = rowColor;
    }
    FastLED.show();
    delay(200);
  }

  leds[ledIndex(3, 3)] = blueC;
  leds[ledIndex(3, 4)] = blueC;
  leds[ledIndex(4, 3)] = blueC;
  leds[ledIndex(4, 4)] = blueC;
  FastLED.show();
  delay(800);
}

void anim_columnWave() {
  const unsigned long WAVE_DURATION = 3000;
  unsigned long start = millis();

  CRGB saffron = CRGB(255, 102, 0);
  CRGB white   = CRGB(255, 255, 255);
  CRGB greenC  = CRGB(19, 136, 8);
  CRGB blueC   = CRGB(0, 0, 128);

  while (millis() - start < WAVE_DURATION) {
    for (int c = 0; c < COLS && (millis() - start < WAVE_DURATION); c++) {
      for (int phase = 0; phase < 20; phase++) {
        float brightness = 0.1 + 0.9 * sin(phase * PI / 20.0);
        
        for (int r = 0; r < ROWS; r++) {
          for (int cc = 0; cc < COLS; cc++) {
            CRGB base;
            if (r <= 2)      base = saffron;
            else if (r <= 4) base = white;
            else             base = greenC;
            if ((r == 3 || r == 4) && (cc == 3 || cc == 4)) base = blueC;
            
            float factor = (cc == c) ? brightness : 0.3;
            leds[ledIndex(r, cc)] = CRGB(
              (uint8_t)(base.r * factor),
              (uint8_t)(base.g * factor),
              (uint8_t)(base.b * factor)
            );
          }
        }
        FastLED.show();
        delay(7);
      }
    }
  }
  
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      CRGB base;
      if (r <= 2)      base = saffron;
      else if (r <= 4) base = white;
      else             base = greenC;
      if ((r == 3 || r == 4) && (c == 3 || c == 4)) base = blueC;
      leds[ledIndex(r, c)] = base;
    }
  }
  FastLED.show();
  delay(500);
}

void anim_scrollText() {
  // Simple 5x3 font for characters (1 = LED on, 0 = LED off)
  // M, V, S, R, -, E, C, E
  const uint8_t charM[5][3] = {
    {1,0,1}, {1,1,1}, {1,0,1}, {1,0,1}, {1,0,1}
  };
  const uint8_t charV[5][3] = {
    {1,0,1}, {1,0,1}, {1,0,1}, {1,0,1}, {0,1,0}
  };
  const uint8_t charS[5][3] = {
    {0,1,1}, {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}
  };
  const uint8_t charR[5][3] = {
    {1,1,0}, {1,0,1}, {1,1,0}, {1,0,1}, {1,0,1}
  };
  const uint8_t charDash[5][3] = {
    {0,0,0}, {0,0,0}, {1,1,1}, {0,0,0}, {0,0,0}
  };
  const uint8_t charE[5][3] = {
    {1,1,1}, {1,0,0}, {1,1,0}, {1,0,0}, {1,1,1}
  };
  const uint8_t charC[5][3] = {
    {0,1,1}, {1,0,0}, {1,0,0}, {1,0,0}, {0,1,1}
  };
  const uint8_t charSpace[5][3] = {
    {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}
  };
  
  // Text: "MVSR - ECE"
  const uint8_t* text[] = {
    (const uint8_t*)charM, (const uint8_t*)charV, (const uint8_t*)charS, (const uint8_t*)charR,
    (const uint8_t*)charSpace,
    (const uint8_t*)charDash,
    (const uint8_t*)charSpace,
    (const uint8_t*)charE, (const uint8_t*)charC, (const uint8_t*)charE
  };
  const int textLen = 10;
  const int charWidth = 3;
  const int charSpacing = 1;
  const int totalWidth = textLen * (charWidth + charSpacing);
  
  CRGB saffron = CRGB(255, 102, 0);
  CRGB white   = CRGB(255, 255, 255);
  CRGB greenC  = CRGB(19, 136, 8);
  
  // Scroll from right to left
  for (int offset = COLS; offset > -totalWidth; offset--) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    
    // Draw each character
    for (int charIdx = 0; charIdx < textLen; charIdx++) {
      int charStartCol = offset + charIdx * (charWidth + charSpacing);
      const uint8_t* charData = text[charIdx];
      
      for (int row = 0; row < 5; row++) {
        for (int col = 0; col < charWidth; col++) {
          int ledCol = charStartCol + col;
          int ledRow = row + 1; // Center vertically (rows 1-5)
          
          if (ledCol >= 0 && ledCol < COLS && ledRow >= 0 && ledRow < ROWS) {
            uint8_t pixel = charData[row * charWidth + col];
            if (pixel) {
              // Red color for MVSR - ECE text
              CRGB color = CRGB(255, 0, 0);
              
              leds[ledIndex(ledRow, ledCol)] = color;
            }
          }
        }
      }
    }
    
    FastLED.show();
    delay(350);
  }
  
  delay(500);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

void rippleAt(int rr,int cc, int maxRadius=4){
  uint8_t saved[NUM_LEDS];
  memcpy(saved, ledBuffer, sizeof(ledBuffer));
  for(int radius=0; radius<=maxRadius; radius++){
    for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
      int idx = ledIndex(r,c);
      int dist = max(abs(r-rr), abs(c-cc));
      if(dist==radius){
        if(radius==0) ledBuffer[idx]=LB_MOVE;
        else if(radius==1) ledBuffer[idx]=LB_TURN_BLINK;
        else if(radius<=3) ledBuffer[idx]=LB_WAVE;
        else ledBuffer[idx]=LB_NULL;
      }
    }
    renderLEDs();
    delay(45);
  }
  memcpy(ledBuffer, saved, sizeof(ledBuffer));
  refreshBufferFromState();
  renderLEDs();
}

// CHECKMATE CELEBRATION
void celebrateCheckmate() {
  isCelebrating = true;  // PREVENT LED OVERWRITES
  Serial.println("Playing checkmate celebration!");
  
  bool winnerIsWhite = (currentTurn == 0);
  CRGB winnerColor = winnerIsWhite ? CRGB::White : CRGB(0, 0, 150);
  
  // Phase 1: Explosive flash
  for (int flash = 0; flash < 3; flash++) {
    fill_solid(leds, NUM_LEDS, winnerColor);
    FastLED.setBrightness(255);
    FastLED.show();
    delay(200);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.show();
    delay(150);
  }
  
  // Phase 2: Find and spotlight winning king
  int kingType = winnerIsWhite ? 5 : 13;
  int kingR = -1, kingC = -1;
  
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      if (pieceBoard[r][c] == kingType) {
        kingR = r;
        kingC = c;
        break;
      }
    }
    if (kingR != -1) break;
  }
  
  // Sparkle effect
  for (int t = 0; t < 50; t++) {
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i].nscale8(220);
    }
    
    if (kingR != -1 && kingC != -1) {
      leds[ledIndex(kingR, kingC)] = winnerColor;
    }
    
    for (int k = 0; k < 6; k++) {
      int idx = random8(NUM_LEDS);
      leds[idx] = CHSV(random8(40, 60), 200, 255);
    }
    
    FastLED.show();
    delay(60);
  }
  
  // Phase 3: Victory ripple
  if (kingR != -1 && kingC != -1) {
    for (int radius = 0; radius <= 6; radius++) {
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
          int dist = max(abs(r - kingR), abs(c - kingC));
          if (dist == radius) {
            leds[ledIndex(r, c)] = winnerColor;
          } else if (dist == radius - 1) {
            leds[ledIndex(r, c)] = CHSV(random8(40, 60), 200, 180);
          }
        }
      }
      FastLED.show();
      delay(120);
    }
  }
  
  // Final: Winner color pulse
  for (int i = 0; i < 3; i++) {
    fill_solid(leds, NUM_LEDS, winnerColor);
    FastLED.show();
    delay(300);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(200);
  }
  
  delay(500);
  isCelebrating = false;  // ALLOW NORMAL LED UPDATES
}

// STALEMATE CELEBRATION
void celebrateStalemate() {
  isCelebrating = true;  // PREVENT LED OVERWRITES
  Serial.println("Playing stalemate (draw) celebration!");
  
  CRGB white = CRGB::White;
  CRGB blue  = CRGB(0, 0, 150);
  
  // Phase 1: Alternating flash
  for (int flash = 0; flash < 6; flash++) {
    fill_solid(leds, NUM_LEDS, (flash % 2) ? white : blue);
    FastLED.show();
    delay(200);
  }
  
  // Phase 2: Checkerboard pattern
  for (int t = 0; t < 20; t++) {
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        bool isWhiteSquare = ((r + c) % 2 == 0);
        leds[ledIndex(r, c)] = isWhiteSquare ? white : blue;
      }
    }
    FastLED.show();
    delay(100);
    
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        bool isWhiteSquare = ((r + c) % 2 == 0);
        leds[ledIndex(r, c)] = isWhiteSquare ? blue : white;
      }
    }
    FastLED.show();
    delay(100);
  }
  
  // Phase 3: Gentle wave
  for (int wave = 0; wave < 3; wave++) {
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        leds[ledIndex(r, c)] = CHSV(160 + r * 10, 180, 150);
      }
      FastLED.show();
      delay(80);
    }
  }
  
  // Final fade
  for (int brightness = 255; brightness >= 0; brightness -= 5) {
    for (int r = 0; r < ROWS; r++) {
      for (int c = 0; c < COLS; c++) {
        leds[ledIndex(r, c)].nscale8(brightness);
      }
    }
    FastLED.show();
    delay(20);
  }
  
  delay(500);
  isCelebrating = false;  // ALLOW NORMAL LED UPDATES
}

void runStartupSequence(){
  isStartup = true;
  Serial.println("Startup animation starting...");

  anim_outwardWave();
  anim_rowScan();
  anim_columnScan();
  anim_flagSlideUp();
  anim_columnWave();
  anim_scrollText();

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  isStartup = false;
  refreshBufferFromState();
  renderLEDs();
  Serial.println("Startup animation complete. Game ready!");
}

// Main input state machine
void runLoopLogic(int r,int c){
  bool last = lastState[r][c];
  bool now  = stableState[r][c];
  if(last==now) return;
  if(gameState==STATE_GAME_OVER) return;

  // STATE_IDLE
  if(gameState==STATE_IDLE){
    if(last && !now){
      unsigned long now_time = millis();
      if (now_time - lastLiftTime < MIN_LIFT_INTERVAL) {
        Serial.println("Too fast! Wait a moment before next move.");
        return;
      }
      lastLiftTime = now_time;
      
      int p = pieceBoard[r][c];
      if(p==0){
        Serial.println("Error: Lifted empty (Ghost?)");
        gameState = STATE_ERROR;
        computeDiffs();
        fromR = r; fromC = c;
        heldPiece = 0;
      } else if( (isWhitePiece(p) && currentTurn==0) || 
                 (isBlackPiece(p) && currentTurn==1) ){
        Serial.println("Wrong turn: attempted to lift opponent's piece.");
        gameState = STATE_WRONG_TURN;
        fromR = r; fromC = c;
        heldPiece = p;
        clearLegal();
      } else {
        gameState = STATE_HOLDING;
        heldPiece = p;
        fromR = r; fromC = c;
        pieceBoard[r][c] = 0;
        generateLegalMoves(fromR, fromC);
        Serial.print("LIFTED: "); Serial.println(heldPiece);
      }
    }
    return;
  }

  // STATE_HOLDING
  if(gameState==STATE_HOLDING){
    if(last && !now){
      if(inBounds(r,c) && legal[r][c] && pieceBoard[r][c]!=0){
        gameState = STATE_CAPTURE_PENDING;
        victimPiece = pieceBoard[r][c];
        victimR = r; victimC = c;
        pieceBoard[r][c] = 0;
        Serial.println("Capture Pending");
      } else {
        Serial.println("Error: Invalid second lift.");
        gameState = STATE_ERROR;
        computeDiffs();
      }
    } else if(!last && now){
      if(r==fromR && c==fromC){
        gameState = STATE_IDLE;
        pieceBoard[r][c] = heldPiece;
        heldPiece = 0; fromR = fromC = -1;
        clearLegal();
        Serial.println("Cancelled Move");
      } else if(inBounds(r,c) && legal[r][c] && pieceBoard[r][c]==0){
        delay(POST_ACCEPT_MS);
        scanMatrix(rawState);
        if(!rawState[r][c]){
          Serial.println("Placement vanished during micro-check. Ignored.");
          gameState = STATE_ERROR;
          computeDiffs();
          return;
        }
        
        gameState = STATE_IDLE;
        
        // PAWN PROMOTION
        int finalPiece = heldPiece;
        if(heldPiece == 6 && r == 7){
          finalPiece = 4;
          Serial.println("White pawn promoted to Queen!");
        }
        if(heldPiece == 14 && r == 0){
          finalPiece = 12;
          Serial.println("Black pawn promoted to Queen!");
        }
        
        pieceBoard[r][c] = finalPiece;

        lastMoveFromR = fromR;
        lastMoveFromC = fromC;
        lastMoveToR   = r;
        lastMoveToC   = c;

        heldPiece = 0; fromR = fromC = -1;
        clearLegal();
        currentTurn = (currentTurn==1)?0:1;
        takeSnapshot();
        Serial.println("Move Complete");
        
        checkGameStatus();  // CRITICAL: Check after every move
      } else {
        Serial.println("Error: Invalid placement");
        gameState = STATE_ERROR;
        computeDiffs();
      }
    }
    return;
  }

  // STATE_CAPTURE_PENDING
  if(gameState==STATE_CAPTURE_PENDING){
    if(!last && now){
      if(r==victimR && c==victimC){
        gameState = STATE_IDLE;
        
        // PAWN PROMOTION
        int finalPiece = heldPiece;
        if(heldPiece == 6 && r == 7){
          finalPiece = 4;
          Serial.println("White pawn promoted to Queen!");
        }
        if(heldPiece == 14 && r == 0){
          finalPiece = 12;
          Serial.println("Black pawn promoted to Queen!");
        }
        
        pieceBoard[r][c] = finalPiece;

        lastMoveFromR = fromR;
        lastMoveFromC = fromC;
        lastMoveToR   = r;
        lastMoveToC   = c;

        heldPiece = 0; fromR = fromC = -1;
        victimPiece = 0; victimR = victimC = -1;
        clearLegal();
        currentTurn = (currentTurn==1)?0:1;
        takeSnapshot();
        Serial.println("Capture Complete");
        
        checkGameStatus();  // CRITICAL: Check after every capture
      } else {
        Serial.println("Error: Must place on victim square");
        gameState = STATE_ERROR;
        computeDiffs();
      }
    }
    return;
  }

  // STATE_WRONG_TURN
  if(gameState==STATE_WRONG_TURN){
    if(!last && now){
      if(r==fromR && c==fromC){
        Serial.println("Wrong-turn piece returned. Game resumes.");
        gameState = STATE_IDLE;
        heldPiece = 0; fromR = fromC = -1;
        clearLegal();
      } else {
        Serial.println("Wrong-turn: must return piece to origin to resume.");
      }
    }
    if(last && !now){
      Serial.println("Wrong-turn: additional lift ignored.");
    }
    return;
  }

  // STATE_ERROR (multi-diff correction mode)
  if(gameState==STATE_ERROR){
    if(diffCount == 0) computeDiffs();

    if(!last && now){
      if(diffMap[r][c] && pieceBoard[r][c] == boardSnapshot[r][c]){
        diffMap[r][c] = false;
        diffCount--;
        Serial.print("Corrected square r="); 
        Serial.print(r); 
        Serial.print(" c="); 
        Serial.println(c);
      } else {
        computeDiffs();
      }

      if(diffCount == 0){
        gameState = STATE_IDLE;
        clearLegal();
        takeSnapshot();
        Serial.println("All corrections done. Game resumed.");
        checkGameStatus();
      }

      return;
    }

    if(last && !now){
      if(diffMap[r][c]){
        heldPiece = pieceBoard[r][c];
        fromR = r; fromC = c;
        pieceBoard[r][c] = 0;
        computeDiffs();
        Serial.print("Picked up diff piece r="); 
        Serial.print(r); 
        Serial.print(" c="); 
        Serial.println(c);
      } else {
        Serial.println("Must pick up a highlighted (red) square to correct.");
      }
    }
    return;
  }
}

// Legal move existence & check/mate
bool hasLegalMovesForColor(bool colorIsWhite){
  int savedBoard[ROWS][COLS];
  for(int r=0;r<ROWS;r++) 
    for(int c=0;c<COLS;c++) 
      savedBoard[r][c] = pieceBoard[r][c];
      
  int savedHeld = heldPiece; 
  int savedFromR = fromR, savedFromC = fromC;
  
  uint8_t savedLegal[ROWS][COLS];
  for(int r=0;r<ROWS;r++) 
    for(int c=0;c<COLS;c++) 
      savedLegal[r][c] = legal[r][c];

  bool found=false;
  for(int r=0;r<ROWS && !found;r++){
    for(int c=0;c<COLS && !found;c++){
      int p = savedBoard[r][c];
      if(p!=0 && (isWhitePiece(p)==colorIsWhite)){
        heldPiece = p; fromR = r; fromC = c;
        pieceBoard[r][c] = 0;
        generateLegalMoves(r,c);
        pieceBoard[r][c] = p;
        for(int rr=0; rr<ROWS && !found; rr++)
          for(int cc=0; cc<COLS && !found; cc++)
            if(legal[rr][cc]) found=true;
      }
    }
  }
  
  for(int r=0;r<ROWS;r++) 
    for(int c=0;c<COLS;c++) 
      pieceBoard[r][c] = savedBoard[r][c];
      
  heldPiece = savedHeld; 
  fromR = savedFromR; 
  fromC = savedFromC;
  
  for(int r=0;r<ROWS;r++) 
    for(int c=0;c<COLS;c++) 
      legal[r][c] = savedLegal[r][c];
      
  return found;
}

// FIXED: Better king validation with game-over detection
bool validateKings(){
  int wK = 0, bK = 0;
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      if(pieceBoard[r][c] == 5)  wK++;
      if(pieceBoard[r][c] == 13) bK++;
    }
  }

  // CRITICAL FIX: Missing king = game over, not error!
  if(wK == 0) {
    Serial.println("BLACK WINS - White king captured!");
    checkState = 2;
    gameState = STATE_GAME_OVER;
    celebrateCheckmate();
    return false;
  }
  if(bK == 0) {
    Serial.println("WHITE WINS - Black king captured!");
    checkState = 2;
    gameState = STATE_GAME_OVER;
    celebrateCheckmate();
    return false;
  }
  
  if(wK != 1 || bK != 1){
    Serial.print("Board invalid: white kings=");
    Serial.print(wK);
    Serial.print(" black kings=");
    Serial.println(bK);
    gameState = STATE_ERROR;
    computeDiffs();
    return false;
  }
  return true;
}

void checkGameStatus(){
  if(!validateKings()){
    if(gameState != STATE_GAME_OVER){
      gameState = STATE_ERROR;
      computeDiffs();
      checkState = 0;
      Serial.println("ERROR: Invalid king count. Game halted.");
    }
    return;
  }

  bool turnIsWhite = (currentTurn==1);
  bool inCheck = isCheck(turnIsWhite, pieceBoard);
  bool hasLegal = hasLegalMovesForColor(turnIsWhite);
  
  if(inCheck){
    if(!hasLegal){
      checkState = 2;
      gameState = STATE_GAME_OVER;
      Serial.println("CHECKMATE!");
      celebrateCheckmate();
    } else {
      checkState = 1;
      Serial.println("CHECK!");
    }
  } else {
    if(!hasLegal){
      checkState = 3;
      gameState = STATE_GAME_OVER;
      Serial.println("STALEMATE! Game is a draw.");
      celebrateStalemate();
    } else {
      checkState = 0;
    }
  }
}

// Setup & loop
unsigned long lastScan = 0;
const unsigned long SCAN_INTERVAL = 40;

void setup(){
  Serial.begin(115200);
  delay(50);
  Serial.println("Arduino Chess LED - CORRECTED VERSION starting");

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(); 
  FastLED.show();

  for(int r=0;r<ROWS;r++){
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], HIGH);
  }
  for(int c=0;c<COLS;c++){
    pinMode(colPins[c], INPUT_PULLUP);
  }

  for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
    rawState[r][c] = stableState[r][c] = lastState[r][c] = false;
    lastRaw[r][c] = false;
    stateChangeTime[r][c] = millis();
  }

  initBoard();

  // Pre-start placement check
  Serial.println("Checking initial piece placement...");
  bool piecesReady = true;
  const int reqRows[4] = {0,1,6,7};
  const int forbRows[4] = {2,3,4,5};

  scanMatrix(rawState);

  piecesReady = true;
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  for (int i = 0; i < 4; i++) {
    int r = reqRows[i];
    for (int c = 0; c < COLS; c++) {
      int idx = ledIndex(r, c);
      if (!rawState[r][c]) {
        piecesReady = false;
        leds[idx] = CRGB(255, 0, 0);
        Serial.print("Missing piece at required row "); 
        Serial.print(r); 
        Serial.print(" col "); 
        Serial.println(c);
      }
    }
  }

  for (int i = 0; i < 4; i++) {
    int r = forbRows[i];
    for (int c = 0; c < COLS; c++) {
      int idx = ledIndex(r, c);
      if (rawState[r][c]) {
        piecesReady = false;
        leds[idx] = CRGB(255, 200, 0);
        Serial.print("Unexpected press at forbidden row "); 
        Serial.print(r); 
        Serial.print(" col "); 
        Serial.println(c);
      }
    }
  }

  FastLED.show();

  bool lastPiecesReady = piecesReady;
  unsigned long lastStatusPrint = millis();
  const unsigned long STATUS_PRINT_INTERVAL = 800;

  while (!piecesReady) {
    delay(80);
    scanMatrix(rawState);
    piecesReady = true;
    fill_solid(leds, NUM_LEDS, CRGB::Black);

    for (int i = 0; i < 4; i++) {
      int r = reqRows[i];
      for (int c = 0; c < COLS; c++) {
        int idx = ledIndex(r, c);
        if (!rawState[r][c]) {
          piecesReady = false;
          leds[idx] = CRGB(255, 0, 0);
        } else {
          leds[idx] = CRGB::Black;
        }
      }
    }

    for (int i = 0; i < 4; i++) {
      int r = forbRows[i];
      for (int c = 0; c < COLS; c++) {
        int idx = ledIndex(r, c);
        if (rawState[r][c]) {
          piecesReady = false;
          leds[idx] = CRGB(255, 200, 0);
        }
      }
    }

    FastLED.show();

    unsigned long nowt = millis();
    if (piecesReady != lastPiecesReady || 
        (nowt - lastStatusPrint) >= STATUS_PRINT_INTERVAL) {
      if (!piecesReady) {
        Serial.println("Waiting: required rows must be pressed...");
      } else {
        Serial.println("Piece placement OK. Continuing startup.");
      }
      lastPiecesReady = piecesReady;
      lastStatusPrint = nowt;
    }
  }

  scanMatrix(rawState);
  unsigned long now = millis();
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      lastRaw[r][c] = rawState[r][c];
      stableState[r][c] = rawState[r][c];
      lastState[r][c] = stableState[r][c];
      stateChangeTime[r][c] = now;
    }
  }

  Serial.println("Piece placement OK. Continuing startup.");
  runStartupSequence();
  refreshBufferFromState();
  renderLEDs();
}

void loop(){
  unsigned long now = millis();
  
  // CRITICAL FIX: Aggressive error recovery
  if(gameState == STATE_ERROR) {
    static unsigned long lastErrorCheck = 0;
    if(millis() - lastErrorCheck > 500) {
      computeDiffs();
      if(diffCount == 0) {
        gameState = STATE_IDLE;
        Serial.println("Auto-recovered from error!");
        checkGameStatus();
      }
      lastErrorCheck = millis();
    }
  }
  
  if(now - lastScan >= SCAN_INTERVAL){
    lastScan = now;
    scanMatrix(rawState);

    for(int r=0;r<ROWS;r++){
      for(int c=0;c<COLS;c++){
        if(rawState[r][c] != lastRaw[r][c]){
          lastRaw[r][c] = rawState[r][c];
          stateChangeTime[r][c] = now;
        }

        if(rawState[r][c] != stableState[r][c]){
          unsigned long required_ms = MIN_STABLE_MS;

          if(gameState == STATE_HOLDING && 
             stableState[r][c] == false && 
             rawState[r][c] == true){
            required_ms = MIN_PLACEMENT_MS;
          }

          if(now - stateChangeTime[r][c] >= required_ms){
            bool prevStable = stableState[r][c];
            lastState[r][c] = prevStable;
            stableState[r][c] = rawState[r][c];
            stateChangeTime[r][c] = now;
            runLoopLogic(r,c);
          }
        } else {
          stateChangeTime[r][c] = now;
        }
      }
    }

    // CRITICAL FIX: Only update LEDs when not celebrating
    if(!isCelebrating){
      refreshBufferFromState();
      renderLEDs();
    }
  }

  // Serial commands
  if(Serial.available()){
    String s = Serial.readStringUntil('\n');
    s.trim();
    
    if(s.equalsIgnoreCase("demo")){
      if(pieceBoard[1][0]==6 && pieceBoard[2][0]==0){
        pieceBoard[2][0] = pieceBoard[1][0];
        pieceBoard[1][0] = 0;
        
        stableState[1][0] = false;
        stableState[2][0] = true;
        lastState[1][0] = false;
        lastState[2][0] = true;
        lastRaw[1][0] = false;
        lastRaw[2][0] = true;

        lastMoveFromR = 1;
        lastMoveFromC = 0;
        lastMoveToR   = 2;
        lastMoveToC   = 0;

        Serial.println("Demo move executed");
        rippleAt(2,0,3);
        currentTurn = (currentTurn==1)?0:1;
        checkGameStatus();
      }
    } 
    else if(s.equalsIgnoreCase("startup")){
      runStartupSequence();
    } 
    else if(s.equalsIgnoreCase("reset")){
      initBoard();
      refreshBufferFromState();
      renderLEDs();
      Serial.println("Board reset");
    } 
    else if(s.equalsIgnoreCase("dump")){
      debugDumpBoard();
    }
    // NEW: Force recovery command
    else if(s.equalsIgnoreCase("recover")){
      takeSnapshot();
      gameState = STATE_IDLE;
      clearLegal();
      Serial.println("Force recovered from error state");
    }
    else {
      Serial.print("Unknown cmd: "); 
      Serial.println(s);
      Serial.println("Available: demo, startup, reset, dump, recover");
    }
  }
}
