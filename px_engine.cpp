// px_engine.cpp -- "Engine Baru" for chesspx.
// Single-file C++17 chess engine, built to WebAssembly with Emscripten (standalone .wasm, no JS glue).
//
// Board layout is identical to the JS game: index = row*8 + col, row 0 = rank 8 (black home), row 7 = rank 1.
// Piece codes: 0 empty, 1..6 = white P N B R Q K, 9..14 = black P N B R Q K.
//
// Host (JS) talks to the engine through one shared int32 buffer (px_io) and two calls:
//   px_init()            once after instantiation
//   px_search()          reads the request from px_io, writes the answer to px_io
//
// Request layout (int32 slots in px_io):
//   0..63  board        64  side (0 white, 1 black)   65  castling bits (1 wK, 2 wQ, 4 bK, 8 bQ)
//   66     ep square or -1                            67  halfmove clock       68 fullmove number
//   69     mode: 0 random move, 1 skill (human-like), 2 best move (ultra/max)
//   70     time budget in ms        71 max depth     72 quiescence depth cap (skill mode)
//   73     temperature T * 1000     74 slip chance * 1000   75 mate-sight chance * 1000
//   76     style (0 balanced, 1 aggressive, 2 tactical, 3 defensive)
//   77     random seed              78 number of earlier game positions (rep history)
//   100..  rep history, 67 ints each: 64 board + side + castling + ep
// Answer layout:
//   0 from, 1 to, 2 promotion (0 none, 2 N, 3 B, 4 R, 5 Q), 3 score (cp, side to move), 4 depth, 5 nodes, 6 status (1 ok, 0 no move)
#include <stdint.h>
#include <string.h>

typedef uint64_t U64;

#ifdef __EMSCRIPTEN__
#define PX_EXPORT extern "C" __attribute__((used, visibility("default")))
extern "C" { __attribute__((import_module("env"), import_name("px_now"))) double px_now(void); }
#else
#include <chrono>
#define PX_EXPORT extern "C"
static double px_now(void){
  using namespace std::chrono;
  return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}
#endif

enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum { WHITE = 0, BLACK = 1 };
static inline int ptype(int p){ return p & 7; }
static inline int pcol(int p){ return p >> 3; }

static inline int iabs(int x){ return x < 0 ? -x : x; }
static const int INF = 32000, MATE = 30000, MATE_IN_MAX = MATE - 200;

// ---------------------------------------------------------------- move encoding
// bits 0-5 from, 6-11 to, 12-14 promotion piece type, 15 capture, 16 en passant, 17 castle, 18 double push
typedef int32_t Move;
enum { F_CAP = 1 << 15, F_EP = 1 << 16, F_CASTLE = 1 << 17, F_DBL = 1 << 18 };
static inline int mfrom(Move m){ return m & 63; }
static inline int mto(Move m){ return (m >> 6) & 63; }
static inline int mpromo(Move m){ return (m >> 12) & 7; }
static inline Move mk(int f, int t, int promo = 0, int flags = 0){ return f | (t << 6) | (promo << 12) | flags; }

// ---------------------------------------------------------------- position
struct Pos {
  int8_t b[64];
  int8_t side;
  uint8_t castle;
  int8_t ep;
  int16_t half;
  int16_t full;
  U64 key;
};

// ---------------------------------------------------------------- tables
static int RAY[64][8][7], RLEN[64][8];
static int KN[64][8], NKN[64], KG[64][8], NKG[64];
static int PAWNATT[2][64][2], NPAWNATT[2][64];
static U64 ZP[16][64], ZCAS[16], ZEP[64], ZSIDE;
static uint8_t CMASK[64];
static int MGV[7] = { 0, 100, 320, 330, 500, 900, 0 };
static int PSTW[7][64];          // middlegame piece-square, from White's view (same orientation as the board)
static int KEND[64];

static U64 rng_state = 0x9E3779B97F4A7C15ULL;
static U64 rnd64(){
  rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
  return rng_state;
}
static double rnd01(){ return (double)(rnd64() >> 11) / 9007199254740992.0; }

static const int PST_P[64] = {
  0,0,0,0,0,0,0,0, 50,50,50,50,50,50,50,50, 10,10,20,30,30,20,10,10, 5,5,10,25,25,10,5,5,
  0,0,0,20,20,0,0,0, 5,-5,-10,0,0,-10,-5,5, 5,10,10,-20,-20,10,10,5, 0,0,0,0,0,0,0,0 };
static const int PST_N[64] = {
  -50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,0,0,0,-20,-40, -30,0,10,15,15,10,0,-30, -30,5,15,20,20,15,5,-30,
  -30,0,15,20,20,15,0,-30, -30,5,10,15,15,10,5,-30, -40,-20,0,5,5,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50 };
static const int PST_B[64] = {
  -20,-10,-10,-10,-10,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,10,10,5,0,-10, -10,5,5,10,10,5,5,-10,
  -10,0,10,10,10,10,0,-10, -10,10,10,10,10,10,10,-10, -10,5,0,0,0,0,5,-10, -20,-10,-10,-10,-10,-10,-10,-20 };
static const int PST_R[64] = {
  0,0,0,0,0,0,0,0, 5,10,10,10,10,10,10,5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5,
  -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 0,0,0,5,5,0,0,0 };
static const int PST_Q[64] = {
  -20,-10,-10,-5,-5,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,5,5,5,0,-10, -5,0,5,5,5,5,0,-5,
  0,0,5,5,5,5,0,-5, -10,5,5,5,5,5,0,-10, -10,0,5,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20 };
static const int PST_K[64] = {
  -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30,
  -20,-30,-30,-40,-40,-30,-30,-20, -10,-20,-20,-20,-20,-20,-20,-10, 20,20,0,0,0,0,20,20, 20,30,10,0,0,10,30,20 };
static const int PST_KE[64] = {
  -50,-40,-30,-20,-20,-30,-40,-50, -30,-20,-10,0,0,-10,-20,-30, -30,-10,20,30,30,20,-10,-30, -30,-10,30,40,40,30,-10,-30,
  -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30, -30,-30,0,0,0,0,-30,-30, -50,-30,-30,-30,-30,-30,-30,-50 };
static const int PASSED[8] = { 0, 5, 10, 20, 35, 60, 100, 0 };

static void init_tables(){
  static const int DR[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
  static const int DC[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
  static const int NR[8] = { -2, -2, -1, -1, 1, 1, 2, 2 };
  static const int NC[8] = { -1, 1, -2, 2, -2, 2, -1, 1 };
  for(int s = 0; s < 64; s++){
    int r = s >> 3, c = s & 7;
    for(int d = 0; d < 8; d++){
      int n = 0, rr = r + DR[d], cc = c + DC[d];
      while(rr >= 0 && rr < 8 && cc >= 0 && cc < 8){ RAY[s][d][n++] = rr * 8 + cc; rr += DR[d]; cc += DC[d]; }
      RLEN[s][d] = n;
    }
    NKN[s] = 0; NKG[s] = 0;
    for(int d = 0; d < 8; d++){
      int rr = r + NR[d], cc = c + NC[d];
      if(rr >= 0 && rr < 8 && cc >= 0 && cc < 8) KN[s][NKN[s]++] = rr * 8 + cc;
      rr = r + DR[d]; cc = c + DC[d];
      if(rr >= 0 && rr < 8 && cc >= 0 && cc < 8) KG[s][NKG[s]++] = rr * 8 + cc;
    }
    for(int col = 0; col < 2; col++){
      int dir = col == WHITE ? -1 : 1, n = 0;
      for(int dc = -1; dc <= 1; dc += 2){
        int rr = r + dir, cc = c + dc;
        if(rr >= 0 && rr < 8 && cc >= 0 && cc < 8) PAWNATT[col][s][n++] = rr * 8 + cc;
      }
      NPAWNATT[col][s] = n;
    }
    CMASK[s] = 0;
  }
  CMASK[60] = 3; CMASK[56] = 2; CMASK[63] = 1; CMASK[4] = 12; CMASK[0] = 8; CMASK[7] = 4;
  rng_state = 0x2545F4914F6CDD1DULL;
  for(int p = 0; p < 16; p++) for(int s = 0; s < 64; s++) ZP[p][s] = rnd64();
  for(int i = 0; i < 16; i++) ZCAS[i] = rnd64();
  for(int i = 0; i < 64; i++) ZEP[i] = rnd64();
  ZSIDE = rnd64();
  const int* T[7] = { 0, PST_P, PST_N, PST_B, PST_R, PST_Q, PST_K };
  for(int t = 1; t <= 6; t++) for(int s = 0; s < 64; s++) PSTW[t][s] = T[t][s];
  for(int s = 0; s < 64; s++) KEND[s] = PST_KE[s];
}

// ---------------------------------------------------------------- position helpers
static U64 hash_pos(const Pos& p){
  U64 h = 0;
  for(int i = 0; i < 64; i++) if(p.b[i]) h ^= ZP[p.b[i]][i];
  h ^= ZCAS[p.castle];
  if(p.ep >= 0) h ^= ZEP[p.ep];
  if(p.side == BLACK) h ^= ZSIDE;
  return h;
}

static bool attacked(const Pos& p, int sq, int by){
  const int8_t* b = p.b;
  const int pawn = by == WHITE ? 1 : 9, knight = pawn + 1, bishop = pawn + 2, rook = pawn + 3, queen = pawn + 4, king = pawn + 5;
  const int opp = by ^ 1;
  for(int i = 0; i < NPAWNATT[opp][sq]; i++) if(b[PAWNATT[opp][sq][i]] == pawn) return true;
  for(int i = 0; i < NKN[sq]; i++) if(b[KN[sq][i]] == knight) return true;
  for(int i = 0; i < NKG[sq]; i++) if(b[KG[sq][i]] == king) return true;
  for(int d = 0; d < 4; d++){
    for(int i = 0; i < RLEN[sq][d]; i++){
      int pc = b[RAY[sq][d][i]];
      if(pc){ if(pc == rook || pc == queen) return true; break; }
    }
  }
  for(int d = 4; d < 8; d++){
    for(int i = 0; i < RLEN[sq][d]; i++){
      int pc = b[RAY[sq][d][i]];
      if(pc){ if(pc == bishop || pc == queen) return true; break; }
    }
  }
  return false;
}

static int king_sq(const Pos& p, int col){
  int k = col == WHITE ? 6 : 14;
  for(int i = 0; i < 64; i++) if(p.b[i] == k) return i;
  return -1;
}
static bool in_check(const Pos& p, int col){
  int k = king_sq(p, col);
  return k >= 0 && attacked(p, k, col ^ 1);
}

struct MoveList { Move m[256]; int score[256]; int n; };

static void gen_moves(const Pos& p, MoveList& L, bool capsOnly){
  L.n = 0;
  const int us = p.side, them = us ^ 1;
  const int8_t* b = p.b;
  for(int s = 0; s < 64; s++){
    int pc = b[s];
    if(!pc || pcol(pc) != us) continue;
    int t = ptype(pc);
    if(t == PAWN){
      int dir = us == WHITE ? -8 : 8, promoRow = us == WHITE ? 0 : 7, startRow = us == WHITE ? 6 : 1;
      int r = s >> 3, to = s + dir;
      if(!b[to]){
        if((to >> 3) == promoRow){
          for(int pr = QUEEN; pr >= KNIGHT; pr--) L.m[L.n++] = mk(s, to, pr);
        } else if(!capsOnly){
          L.m[L.n++] = mk(s, to);
          if(r == startRow && !b[to + dir]) L.m[L.n++] = mk(s, to + dir, 0, F_DBL);
        }
      }
      for(int i = 0; i < NPAWNATT[us][s]; i++){
        int ct = PAWNATT[us][s][i], tp = b[ct];
        if(tp && pcol(tp) == them){
          if((ct >> 3) == promoRow){
            for(int pr = QUEEN; pr >= KNIGHT; pr--) L.m[L.n++] = mk(s, ct, pr, F_CAP);
          } else L.m[L.n++] = mk(s, ct, 0, F_CAP);
        } else if(!tp && ct == p.ep){
          L.m[L.n++] = mk(s, ct, 0, F_CAP | F_EP);
        }
      }
    } else if(t == KNIGHT || t == KING){
      const int* tg = t == KNIGHT ? KN[s] : KG[s];
      int n = t == KNIGHT ? NKN[s] : NKG[s];
      for(int i = 0; i < n; i++){
        int to = tg[i], tp = b[to];
        if(!tp){ if(!capsOnly) L.m[L.n++] = mk(s, to); }
        else if(pcol(tp) == them) L.m[L.n++] = mk(s, to, 0, F_CAP);
      }
      if(t == KING && !capsOnly){
        if(us == WHITE && s == 60){
          if((p.castle & 1) && !b[61] && !b[62] && b[63] == 4 &&
             !attacked(p, 60, BLACK) && !attacked(p, 61, BLACK) && !attacked(p, 62, BLACK)) L.m[L.n++] = mk(60, 62, 0, F_CASTLE);
          if((p.castle & 2) && !b[57] && !b[58] && !b[59] && b[56] == 4 &&
             !attacked(p, 60, BLACK) && !attacked(p, 59, BLACK) && !attacked(p, 58, BLACK)) L.m[L.n++] = mk(60, 58, 0, F_CASTLE);
        } else if(us == BLACK && s == 4){
          if((p.castle & 4) && !b[5] && !b[6] && b[7] == 12 &&
             !attacked(p, 4, WHITE) && !attacked(p, 5, WHITE) && !attacked(p, 6, WHITE)) L.m[L.n++] = mk(4, 6, 0, F_CASTLE);
          if((p.castle & 8) && !b[1] && !b[2] && !b[3] && b[0] == 12 &&
             !attacked(p, 4, WHITE) && !attacked(p, 3, WHITE) && !attacked(p, 2, WHITE)) L.m[L.n++] = mk(4, 2, 0, F_CASTLE);
        }
      }
    } else {
      int d0 = t == BISHOP ? 4 : 0, d1 = t == ROOK ? 4 : 8;
      for(int d = d0; d < d1; d++){
        for(int i = 0; i < RLEN[s][d]; i++){
          int to = RAY[s][d][i], tp = b[to];
          if(!tp){ if(!capsOnly) L.m[L.n++] = mk(s, to); continue; }
          if(pcol(tp) == them) L.m[L.n++] = mk(s, to, 0, F_CAP);
          break;
        }
      }
    }
  }
}

// copy-make. returns false when the move leaves our own king in check.
static bool make_move(const Pos& p, Move m, Pos& n){
  n = p;
  const int from = mfrom(m), to = mto(m), us = p.side, pc = p.b[from];
  U64 k = p.key;
  k ^= ZCAS[p.castle];
  if(p.ep >= 0) k ^= ZEP[p.ep];
  int captured = n.b[to];
  if(m & F_EP){
    int cap = us == WHITE ? to + 8 : to - 8;
    captured = n.b[cap];
    k ^= ZP[captured][cap]; n.b[cap] = 0;
  } else if(captured){
    k ^= ZP[captured][to];
  }
  k ^= ZP[pc][from]; n.b[from] = 0;
  int placed = mpromo(m) ? (mpromo(m) | (us << 3)) : pc;
  n.b[to] = (int8_t)placed; k ^= ZP[placed][to];
  if(m & F_CASTLE){
    int rf, rt, rook = us == WHITE ? 4 : 12;
    if(to > from){ rf = us == WHITE ? 63 : 7; rt = to - 1; } else { rf = us == WHITE ? 56 : 0; rt = to + 1; }
    n.b[rf] = 0; n.b[rt] = (int8_t)rook; k ^= ZP[rook][rf]; k ^= ZP[rook][rt];
  }
  n.castle = (uint8_t)(p.castle & ~(CMASK[from] | CMASK[to]));
  n.ep = -1;
  if(m & F_DBL){
    int foe = us == WHITE ? 9 : 1, tc = to & 7;
    if((tc > 0 && n.b[to - 1] == foe) || (tc < 7 && n.b[to + 1] == foe)) n.ep = (int8_t)((from + to) / 2);
  }
  n.half = (captured || ptype(pc) == PAWN) ? 0 : p.half + 1;
  n.full = p.side == BLACK ? p.full + 1 : p.full;
  n.side = (int8_t)(us ^ 1);
  k ^= ZCAS[n.castle];
  if(n.ep >= 0) k ^= ZEP[n.ep];
  k ^= ZSIDE;
  n.key = k;
  int ks = ptype(pc) == KING ? to : king_sq(n, us);
  return !(ks >= 0 && attacked(n, ks, us ^ 1));
}

// ---------------------------------------------------------------- evaluation (White's view, centipawns)
static int evaluate_white(const Pos& p){
  const int8_t* b = p.b;
  int pawnsOnFile[2][8], pawnMin[2][8], pawnMax[2][8];
  for(int c = 0; c < 2; c++) for(int f = 0; f < 8; f++){ pawnsOnFile[c][f] = 0; pawnMin[c][f] = 8; pawnMax[c][f] = -1; }
  int npm = 0, nbish[2] = { 0, 0 }, pawnCount = 0, wk = -1, bk = -1;
  for(int i = 0; i < 64; i++){
    int pc = b[i]; if(!pc) continue;
    int t = ptype(pc), c = pcol(pc);
    if(t == PAWN){ int f = i & 7, r = i >> 3; pawnsOnFile[c][f]++; if(r < pawnMin[c][f]) pawnMin[c][f] = r; if(r > pawnMax[c][f]) pawnMax[c][f] = r; pawnCount++; }
    else if(t == KING){ if(c == WHITE) wk = i; else bk = i; }
    else { npm += MGV[t]; if(t == BISHOP) nbish[c]++; }
  }
  if(pawnCount == 0 && npm <= 330) return 0;
  double ph = npm / 5600.0; if(ph > 1) ph = 1;
  int s = 0;
  for(int i = 0; i < 64; i++){
    int pc = b[i]; if(!pc) continue;
    int t = ptype(pc), c = pcol(pc), sg = c == WHITE ? 1 : -1;
    int pi = c == WHITE ? i : ((7 - (i >> 3)) * 8 + (i & 7));
    int v;
    if(t == KING) v = (int)(PST_K[pi] * ph + PST_KE[pi] * (1 - ph));
    else v = MGV[t] + PSTW[t][pi];
    int f = i & 7, r = i >> 3;
    if(t == PAWN){
      int o = c ^ 1; bool passed = true;
      for(int ff = (f > 0 ? f - 1 : 0); ff <= (f < 7 ? f + 1 : 7) && passed; ff++){
        if(pawnsOnFile[o][ff]){
          if(c == WHITE ? pawnMin[o][ff] < r : pawnMax[o][ff] > r) passed = false;
        }
      }
      if(passed){ int adv = c == WHITE ? 6 - r : r - 1; if(adv < 0) adv = 0; if(adv > 7) adv = 7; v += (int)(PASSED[adv] * (0.6 + 0.8 * (1 - ph))); }
      if(pawnsOnFile[c][f] > 1) v -= 8;
      if(!(f > 0 && pawnsOnFile[c][f - 1]) && !(f < 7 && pawnsOnFile[c][f + 1])) v -= 10;
    } else if(t == ROOK){
      int own = pawnsOnFile[c][f], opp = pawnsOnFile[c ^ 1][f];
      if(!own && !opp) v += 15; else if(!own) v += 8;
    }
    s += sg * v;
  }
  if(nbish[0] >= 2) s += 30;
  if(nbish[1] >= 2) s -= 30;
  // knight/bishop/rook/queen mobility (cheap: count empty or capturable target squares)
  for(int i = 0; i < 64; i++){
    int pc = b[i]; if(!pc) continue;
    int t = ptype(pc), c = pcol(pc), sg = c == WHITE ? 1 : -1, cnt = 0;
    if(t == KNIGHT){
      for(int k = 0; k < NKN[i]; k++){ int tp = b[KN[i][k]]; if(!tp || pcol(tp) != c) cnt++; }
      s += sg * (cnt - 4) * 3;
    } else if(t == BISHOP || t == ROOK || t == QUEEN){
      int d0 = t == BISHOP ? 4 : 0, d1 = t == ROOK ? 4 : 8;
      for(int d = d0; d < d1; d++) for(int k = 0; k < RLEN[i][d]; k++){
        int tp = b[RAY[i][d][k]];
        if(!tp){ cnt++; continue; }
        if(pcol(tp) != c) cnt++;
        break;
      }
      int base = t == BISHOP ? 6 : t == ROOK ? 6 : 10, w = t == QUEEN ? 1 : (t == ROOK ? 2 : 3);
      s += sg * (cnt - base) * w;
    }
  }
  // king shelter in the middlegame
  if(wk >= 0 && bk >= 0){
    if(ph > 0.35){
      double w = (ph - 0.35) / 0.4; if(w > 1) w = 1;
      int sh = 0;
      for(int col = 0; col < 2; col++){
        int k = col == WHITE ? wk : bk, r = k >> 3, c = k & 7, dir = col == WHITE ? -1 : 1, part = 0;
        if(col == WHITE ? r < 6 : r > 1) continue;
        for(int dc = -1; dc <= 1; dc++){
          int cc = c + dc; if(cc < 0 || cc > 7) continue;
          int r1 = r + dir, r2 = r + 2 * dir, own = col == WHITE ? 1 : 9;
          if(r1 >= 0 && r1 < 8 && b[r1 * 8 + cc] == own) part += 10;
          else if(r2 >= 0 && r2 < 8 && b[r2 * 8 + cc] == own) part += 5;
          else part -= 8;
        }
        sh += col == WHITE ? part : -part;
      }
      s += (int)(sh * w);
    }
    if(ph < 0.3 && (s > 300 || s < -300)){        // drive the lone king to the edge, bring ours closer
      bool win = s > 0; int lk = win ? bk : wk, kk = win ? wk : bk;
      int lr = lk >> 3, lc = lk & 7, wr = kk >> 3, wc = kk & 7;
      int cd = iabs(2 * lr - 7) + iabs(2 * lc - 7), dist = iabs(lr - wr) + iabs(lc - wc);
      double f = 1 - ph / 0.3; if(f < 0) f = 0;
      s += (win ? 1 : -1) * (int)((cd * 6 + (14 - dist) * 3) * f);
    }
  }
  return s;
}
static inline int evaluate(const Pos& p){
  int v = evaluate_white(p);
  return (p.side == WHITE ? v : -v) + 8;       // tempo
}

// ---------------------------------------------------------------- search
struct TTE { U64 key; int16_t score; int16_t depth; uint32_t move; uint8_t flag; uint8_t age; };
static const int TT_BITS = 19;                 // 524288 entries * 24 bytes ~ 12 MB
static TTE TT[1 << TT_BITS];
static uint8_t tt_age = 0;

enum { TT_EXACT = 0, TT_LOWER = 1, TT_UPPER = 2 };

static Move killers[128][2];
static int32_t history_tbl[2][64][64];
static U64 path_keys[256];          // keys along the current search path (for repetition)
static U64 game_keys[512]; static int game_nkeys = 0;
static double search_deadline = 1e18, search_start = 0;
static bool stop_flag = false;
static uint64_t nodes = 0;
static int g_max_qd = 99;

static const int NO_MOVE_ROOT = 0;

static inline bool time_up(){
  if(stop_flag) return true;
  if((nodes & 2047) == 0 && px_now() >= search_deadline){ stop_flag = true; return true; }
  return false;
}

static int score_to_tt(int s, int ply){ return s >= MATE_IN_MAX ? s + ply : (s <= -MATE_IN_MAX ? s - ply : s); }
static int score_from_tt(int s, int ply){ return s >= MATE_IN_MAX ? s - ply : (s <= -MATE_IN_MAX ? s + ply : s); }

static const int LVAL[7] = { 0, 100, 320, 330, 500, 900, 20000 };

static void score_moves(const Pos& p, MoveList& L, Move ttm, int ply){
  for(int i = 0; i < L.n; i++){
    Move m = L.m[i]; int sc = 0;
    if(m == ttm) sc = 1000000;
    else if(m & F_CAP){
      int victim = (m & F_EP) ? 1 : ptype(p.b[mto(m)]), att = ptype(p.b[mfrom(m)]);
      sc = 100000 + LVAL[victim] * 16 - LVAL[att] / 16 + (mpromo(m) ? LVAL[mpromo(m)] : 0);
    } else if(mpromo(m)) sc = 90000 + LVAL[mpromo(m)];
    else if(ply < 128 && m == killers[ply][0]) sc = 80000;
    else if(ply < 128 && m == killers[ply][1]) sc = 79000;
    else sc = history_tbl[p.side][mfrom(m)][mto(m)];
    L.score[i] = sc;
  }
}
static inline void pick_next(MoveList& L, int start){
  int bi = start;
  for(int i = start + 1; i < L.n; i++) if(L.score[i] > L.score[bi]) bi = i;
  if(bi != start){ Move tm = L.m[start]; L.m[start] = L.m[bi]; L.m[bi] = tm; int ts = L.score[start]; L.score[start] = L.score[bi]; L.score[bi] = ts; }
}

static bool is_repetition(const Pos& p, int ply){
  // earlier positions on the current search path (same side to move => step by 2)
  int lim = p.half < ply ? p.half : ply;
  for(int i = 2; i <= lim; i += 2) if(path_keys[ply - i] == p.key) return true;
  // positions from the real game, only back to the last irreversible move
  if(p.half > ply){
    int back = p.half - ply, cnt = 0;
    for(int i = game_nkeys - 1, k = 0; i >= 0 && k < back; i--, k++){
      if(game_keys[i] == p.key && (((k + ply) & 1) == 1)) { cnt++; if(cnt >= 1) return true; }
    }
  }
  return false;
}

static bool insufficient(const Pos& p){
  int minors = 0, bcol[2] = { 0, 0 }, nb = 0;
  for(int i = 0; i < 64; i++){
    int pc = p.b[i]; if(!pc) continue;
    int t = ptype(pc);
    if(t == KING) continue;
    if(t == KNIGHT || t == BISHOP){ minors++; if(t == BISHOP){ bcol[(((i >> 3) + (i & 7)) & 1)]++; nb++; } }
    else return false;
  }
  if(minors <= 1) return true;
  if(nb == minors && (bcol[0] == 0 || bcol[1] == 0)) return true;
  return false;
}

static int qsearch(const Pos& p, int alpha, int beta, int ply, int qd){
  nodes++;
  if(time_up()) return 0;
  if(ply >= 120) return evaluate(p);
  bool chk = in_check(p, p.side);
  int stand = -INF;
  if(!chk){
    stand = evaluate(p);
    if(stand >= beta) return stand;
    if(stand > alpha) alpha = stand;
  }
  if(qd <= 0 && !chk) return stand;
  MoveList L; gen_moves(p, L, !chk);
  score_moves(p, L, 0, ply);
  int best = chk ? -MATE + ply : stand, legal = 0;
  for(int i = 0; i < L.n; i++){
    pick_next(L, i);
    Move m = L.m[i];
    if(!chk){
      int victim = (m & F_EP) ? 1 : (m & F_CAP) ? ptype(p.b[mto(m)]) : 0;
      if(stand + LVAL[victim] + 200 < alpha && !mpromo(m)) continue;     // delta pruning
      if((m & F_CAP) && !mpromo(m) && LVAL[ptype(p.b[mfrom(m)])] > LVAL[victim] + 150){
        // clearly losing capture: skip when the target square is defended by a pawn
        int to = mto(m), foe = p.side ^ 1; bool pd = false;
        for(int k = 0; k < NPAWNATT[p.side][to]; k++) if(p.b[PAWNATT[p.side][to][k]] == (foe == WHITE ? 1 : 9)) pd = true;
        if(pd) continue;
      }
    }
    Pos n; if(!make_move(p, m, n)) continue;
    legal++;
    path_keys[ply] = p.key;
    int v = -qsearch(n, -beta, -alpha, ply + 1, qd - 1);
    if(stop_flag) return 0;
    if(v > best){ best = v; if(v > alpha){ alpha = v; if(v >= beta) return v; } }
  }
  if(chk && legal == 0) return -MATE + ply;
  return best;
}

static int negamax(const Pos& p, int depth, int alpha, int beta, int ply, bool allowNull){
  nodes++;
  if(time_up()) return 0;
  path_keys[ply] = p.key;
  bool pv = beta - alpha > 1;
  if(ply > 0){
    if(p.half >= 100) return 0;
    if(is_repetition(p, ply) || insufficient(p)) return 0;
    // mate distance pruning
    int a2 = alpha > -MATE + ply ? alpha : -MATE + ply, b2 = beta < MATE - ply - 1 ? beta : MATE - ply - 1;
    if(a2 >= b2) return a2;
  }
  bool chk = in_check(p, p.side);
  if(chk) depth++;
  if(depth <= 0) return qsearch(p, alpha, beta, ply, g_max_qd);
  if(ply >= 120) return evaluate(p);

  TTE& e = TT[p.key & ((1 << TT_BITS) - 1)];
  Move ttm = 0;
  if(e.key == p.key){
    ttm = (Move)e.move;
    if(e.depth >= depth && ply > 0){
      int sc = score_from_tt(e.score, ply);
      if(e.flag == TT_EXACT) return sc;
      if(e.flag == TT_LOWER && sc >= beta) return sc;
      if(e.flag == TT_UPPER && sc <= alpha) return sc;
    }
  }
  int stat = chk ? -INF : evaluate(p);
  // reverse futility
  if(!pv && !chk && depth <= 4 && stat - 110 * depth >= beta && beta > -MATE_IN_MAX && beta < MATE_IN_MAX) return stat - 110 * depth;
  // null move
  if(!pv && !chk && allowNull && depth >= 3 && stat >= beta){
    int pieces = 0;
    for(int i = 0; i < 64; i++){ int pc = p.b[i]; if(pc && pcol(pc) == p.side && ptype(pc) != PAWN && ptype(pc) != KING) pieces++; }
    if(pieces > 0){
      Pos n = p; n.side ^= 1; n.ep = -1; n.half++;
      n.key = p.key ^ ZSIDE; if(p.ep >= 0) n.key ^= ZEP[p.ep];
      int R = 2 + depth / 4 + (stat - beta > 200 ? 1 : 0);
      int v = -negamax(n, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
      if(stop_flag) return 0;
      if(v >= beta) return v >= MATE_IN_MAX ? beta : v;
    }
  }
  MoveList L; gen_moves(p, L, false);
  score_moves(p, L, ttm, ply);
  int best = -INF, legal = 0, origAlpha = alpha; Move bestMove = 0;
  Move quiets[64]; int nq = 0;
  for(int i = 0; i < L.n; i++){
    pick_next(L, i);
    Move m = L.m[i];
    bool quiet = !(m & F_CAP) && !mpromo(m);
    Pos n; if(!make_move(p, m, n)) continue;
    legal++;
    bool givesCheck = in_check(n, n.side);
    // futility / late move pruning for quiet moves
    if(!pv && !chk && quiet && !givesCheck && best > -MATE_IN_MAX){
      if(depth <= 3 && stat + 120 * depth + 60 <= alpha) continue;
      if(depth <= 3 && legal > 6 + depth * 4) continue;
    }
    int v;
    int newDepth = depth - 1;
    if(legal == 1){
      v = -negamax(n, newDepth, -beta, -alpha, ply + 1, true);
    } else {
      int red = 0;
      if(depth >= 3 && legal > 3 && quiet && !chk && !givesCheck){
        red = 1 + (legal > 8) + (depth > 6);
        if(pv && red > 0) red--;
        if(red > newDepth - 1) red = newDepth - 1;
        if(red < 0) red = 0;
      }
      v = -negamax(n, newDepth - red, -alpha - 1, -alpha, ply + 1, true);
      if(v > alpha && red > 0) v = -negamax(n, newDepth, -alpha - 1, -alpha, ply + 1, true);
      if(v > alpha && v < beta) v = -negamax(n, newDepth, -beta, -alpha, ply + 1, true);
    }
    if(stop_flag) return 0;
    if(v > best){
      best = v; bestMove = m;
      if(v > alpha){
        alpha = v;
        if(v >= beta){
          if(quiet && ply < 128){
            if(killers[ply][0] != m){ killers[ply][1] = killers[ply][0]; killers[ply][0] = m; }
            int bonus = depth * depth;
            history_tbl[p.side][mfrom(m)][mto(m)] += bonus;
            for(int q = 0; q < nq; q++) history_tbl[p.side][mfrom(quiets[q])][mto(quiets[q])] -= bonus;
            if(history_tbl[p.side][mfrom(m)][mto(m)] > 60000){
              for(int a = 0; a < 2; a++) for(int x = 0; x < 64; x++) for(int y = 0; y < 64; y++) history_tbl[a][x][y] >>= 1;
            }
          }
          break;
        }
      }
    }
    if(quiet && nq < 64) quiets[nq++] = m;
  }
  if(legal == 0) return chk ? -MATE + ply : 0;
  e.key = p.key; e.score = (int16_t)score_to_tt(best, ply); e.depth = (int16_t)depth; e.move = (uint32_t)bestMove;
  e.flag = best >= beta ? TT_LOWER : (best > origAlpha ? TT_EXACT : TT_UPPER); e.age = tt_age;
  return best;
}

// ---------------------------------------------------------------- driver
struct RootRes { Move m[256]; int sc[256]; int n; int depth; int bestIdx; };

// Iterative deepening over all root moves. `margin` > 0 (skill mode) searches non-best root moves with a
// narrowed window like the JS skill bot, giving every move a usable (bounded) score.
static int search_root(const Pos& root, int maxDepth, double budgetMs, int margin, RootRes& out, uint64_t& nodesOut){
  search_start = px_now(); search_deadline = search_start + budgetMs; stop_flag = false; nodes = 0;
  tt_age++;
  memset(killers, 0, sizeof(killers));
  for(int a = 0; a < 2; a++) for(int x = 0; x < 64; x++) for(int y = 0; y < 64; y++) history_tbl[a][x][y] >>= 2;
  MoveList L; gen_moves(root, L, false);
  Move legal[256]; int nl = 0;
  for(int i = 0; i < L.n; i++){ Pos n; if(make_move(root, L.m[i], n)) legal[nl++] = L.m[i]; }
  out.n = nl; out.depth = 0; out.bestIdx = 0;
  for(int i = 0; i < nl; i++){ out.m[i] = legal[i]; out.sc[i] = -INF; }
  if(nl == 0){ nodesOut = 0; return 0; }
  // initial ordering
  MoveList O; O.n = nl; for(int i = 0; i < nl; i++) O.m[i] = legal[i];
  score_moves(root, O, 0, 0);
  for(int i = 0; i < nl; i++) pick_next(O, i);
  int order[256]; for(int i = 0; i < nl; i++){ for(int j = 0; j < nl; j++) if(O.m[i] == out.m[j]){ order[i] = j; break; } }
  int bestScore = -INF;
  for(int d = 1; d <= maxDepth; d++){
    int cur[256]; for(int i = 0; i < nl; i++) cur[i] = -INF;
    int best = -INF, bestI = order[0]; bool completed = true;
    for(int oi = 0; oi < nl; oi++){
      int mi = order[oi]; Move m = out.m[mi];
      Pos n; make_move(root, m, n);
      path_keys[0] = root.key;
      int lo = (margin > 0 && best > -INF) ? best - margin : -INF;
      int v;
      if(margin > 0){
        v = -negamax(n, d - 1, -INF, -lo, 1, true);
      } else if(oi == 0){
        v = -negamax(n, d - 1, -INF, INF, 1, true);
      } else {
        v = -negamax(n, d - 1, -best - 1, -best, 1, true);
        if(!stop_flag && v > best) v = -negamax(n, d - 1, -INF, -best, 1, true);
      }
      if(stop_flag){ completed = false; break; }
      cur[mi] = v;
      if(v > best){ best = v; bestI = mi; }
    }
    if(!completed) break;                         // the unfinished depth is discarded, the previous one stands
    for(int i = 0; i < nl; i++) out.sc[i] = cur[i];
    out.depth = d; out.bestIdx = bestI; bestScore = best;
    // reorder for the next depth: best move first, the rest by score (stable)
    int ord2[256], k = 0; ord2[k++] = bestI;
    for(int i = 0; i < nl; i++) if(order[i] != bestI) ord2[k++] = order[i];
    for(int i = 2; i < nl; i++){ int x = ord2[i], j = i - 1; while(j >= 1 && cur[ord2[j]] < cur[x]){ ord2[j + 1] = ord2[j]; j--; } ord2[j + 1] = x; }
    for(int i = 0; i < nl; i++) order[i] = ord2[i];
    if(bestScore >= MATE_IN_MAX && margin == 0) break;
    if(px_now() - search_start > budgetMs * 0.55 && d >= 3) break;     // another iteration will not finish
  }
  nodesOut = nodes;
  return bestScore;
}

// ---------------------------------------------------------------- host interface
static int32_t IO[8192];
PX_EXPORT int32_t* px_io(void){ return IO; }
static int px_ready = 0;

static void load_board(const int32_t* a, Pos& p){      // 67 ints: 64 board, side, castling, ep
  for(int i = 0; i < 64; i++) p.b[i] = (int8_t)a[i];
  p.side = (int8_t)a[64]; p.castle = (uint8_t)a[65]; p.ep = (int8_t)a[66];
  p.half = 0; p.full = 1;
  p.key = hash_pos(p);
}
static void load_pos(const int32_t* a, Pos& p){
  load_board(a, p);
  p.half = (int16_t)a[67]; p.full = (int16_t)a[68];
}

PX_EXPORT void px_init(void){
  init_tables();
  memset(TT, 0, sizeof(TT));
  memset(history_tbl, 0, sizeof(history_tbl));
  px_ready = 1;
}

PX_EXPORT void px_clear(void){ memset(TT, 0, sizeof(TT)); memset(history_tbl, 0, sizeof(history_tbl)); }

// exp(-x) for 0 <= x <= 60 without libm: exp(-x) = (exp(-x/64))^64 with a short Taylor series
static double px_exp_neg(double x){
  double y = -x / 64.0;
  double e = 1.0 + y * (1.0 + y * (0.5 + y * (1.0 / 6.0 + y * (1.0 / 24.0 + y / 120.0))));
  for(int i = 0; i < 6; i++) e *= e;
  return e;
}

static int style_bonus(const Pos& p, Move m, int style){
  if(style == 0) return 0;
  Pos n; if(!make_move(p, m, n)) return 0;
  bool gives = in_check(n, n.side);
  int b = 0; bool cap = (m & F_CAP) != 0;
  if(style == 1){ if(cap) b += 45; if(gives) b += 30; }
  else if(style == 2){ if(cap) b += 25; if(gives) b += 50; }
  else if(style == 3){
    // moved piece left hanging: attacked by them and not defended by us
    int to = mto(m), us = p.side;
    if(attacked(n, to, us ^ 1) && !attacked(n, to, us)) b -= 60;
    if(!cap) b += 10;
  }
  return b;
}

PX_EXPORT int px_search(void){
  if(!px_ready) px_init();
  const int32_t* a = IO;
  Pos root; load_pos(a, root);
  int mode = a[69]; double ms = a[70]; int maxDepth = a[71], qd = a[72];
  double T = a[73] / 1000.0, oops = a[74] / 1000.0, mateSight = a[75] / 1000.0;
  int style = a[76]; uint32_t seed = (uint32_t)a[77]; int nrep = a[78];
  rng_state = 0x9E3779B97F4A7C15ULL ^ ((U64)seed * 0x100000001B3ULL); if(!rng_state) rng_state = 1;
  for(int i = 0; i < 5; i++) rnd64();
  if(nrep > 100) nrep = 100;
  game_nkeys = 0;
  for(int i = 0; i < nrep; i++){
    Pos r; load_board(a + 100 + i * 67, r);
    game_keys[game_nkeys++] = r.key;
  }
  MoveList L; gen_moves(root, L, false);
  Move legal[256]; int nl = 0;
  for(int i = 0; i < L.n; i++){ Pos n; if(make_move(root, L.m[i], n)) legal[nl++] = L.m[i]; }
  IO[3] = 0; IO[4] = 0; IO[5] = 0;
  if(nl == 0){ IO[0] = IO[1] = IO[2] = 0; IO[6] = 0; return 0; }
  Move chosen = legal[0]; int score = 0, depthDone = 0; uint64_t nd = 0;
  if(mode == 0){
    chosen = legal[(int)(rnd01() * nl) % nl];
  } else if(nl == 1){
    chosen = legal[0];
  } else if(mode == 2){
    g_max_qd = 99;
    RootRes R; score = search_root(root, maxDepth > 0 ? maxDepth : 64, ms, 0, R, nd); depthDone = R.depth;
    chosen = R.depth > 0 ? R.m[R.bestIdx] : legal[0];
    if(R.depth == 0){          // not even depth 1 finished: take the best-looking capture / any legal move
      MoveList O; O.n = nl; for(int i = 0; i < nl; i++) O.m[i] = legal[i];
      score_moves(root, O, 0, 0); pick_next(O, 0); chosen = O.m[0];
    }
  } else {
    g_max_qd = qd > 0 ? qd : 0;
    // sparse endgames are searched deeper
    int npm = 0; for(int i = 0; i < 64; i++){ int pc = root.b[i]; if(pc && ptype(pc) != KING && ptype(pc) != PAWN) npm += MGV[ptype(pc)]; }
    int cap = maxDepth + ((npm <= 2400 && maxDepth >= 3) ? 2 : 0);
    int margin = (int)(T * 7); if(margin < 60) margin = 60;
    RootRes R; score = search_root(root, cap, ms, margin, R, nd); depthDone = R.depth;
    if(R.depth == 0){
      MoveList O; O.n = nl; for(int i = 0; i < nl; i++) O.m[i] = legal[i];
      score_moves(root, O, 0, 0); pick_next(O, 0); chosen = O.m[0];
    } else {
      int topI = R.bestIdx, top = R.sc[topI];
      bool takeBest = false;
      if(top >= MATE_IN_MAX && rnd01() >= mateSight) takeBest = true;      // sees a forced mate and plays it
      if(takeBest){
        chosen = R.m[topI];
      } else {
        bool slip = rnd01() < oops;                                          // occasional "mental slip": judge by a static glance
        double Tt = slip ? T * 2 + 30 : T;
        if(Tt < 0.01) Tt = 0.01;
        double bs = -1e18, sc[256], w[256], tot = 0;
        for(int i = 0; i < R.n; i++){
          double s0 = R.sc[i];
          if(slip){ Pos n; make_move(root, R.m[i], n); int ev = evaluate_white(n); s0 = root.side == WHITE ? ev : -ev; }
          if(s0 <= -INF + 1) s0 = -3000;
          s0 += style_bonus(root, R.m[i], style);
          if(s0 > 3000) s0 = 3000;
          if(s0 < -3000) s0 = -3000;
          sc[i] = s0; if(s0 > bs) bs = s0;
        }
        for(int i = 0; i < R.n; i++){
          double x = (bs - sc[i]) / Tt; if(x > 60) x = 60;
          w[i] = px_exp_neg(x); tot += w[i];
        }
        double r = rnd01() * tot; chosen = R.m[0];
        for(int i = 0; i < R.n; i++){ r -= w[i]; if(r <= 0){ chosen = R.m[i]; break; } }
      }
    }
  }
  IO[0] = mfrom(chosen); IO[1] = mto(chosen); IO[2] = mpromo(chosen);
  IO[3] = score; IO[4] = depthDone; IO[5] = (int32_t)(nd > 2000000000ULL ? 2000000000ULL : nd); IO[6] = 1;
  return 1;
}

// perft for self-tests (native + wasm): px_io holds the position as for px_search; returns the node count (low 31 bits)
static uint64_t perft(const Pos& p, int depth){
  if(depth == 0) return 1;
  MoveList L; gen_moves(p, L, false);
  uint64_t n = 0;
  for(int i = 0; i < L.n; i++){ Pos c; if(make_move(p, L.m[i], c)) n += depth == 1 ? 1 : perft(c, depth - 1); }
  return n;
}
PX_EXPORT int px_perft(int depth){
  if(!px_ready) px_init();
  Pos p; load_pos(IO, p);
  return (int)(perft(p, depth) & 0x7fffffff);
}
