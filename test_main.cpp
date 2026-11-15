#include "px_engine.cpp"
#include <stdio.h>
#include <string>
#include <vector>

// FEN -> IO (engine board layout: index = row*8+col, row 0 = rank 8)
static void set_fen(const char* fen){
  memset(IO, 0, sizeof(IO));
  int r = 0, c = 0; const char* p = fen;
  for(; *p && *p != ' '; p++){
    if(*p == '/'){ r++; c = 0; }
    else if(*p >= '1' && *p <= '8') c += *p - '0';
    else {
      int col = (*p >= 'a') ? 8 : 0; char ch = *p | 32; int t = 0;
      switch(ch){ case 'p': t = 1; break; case 'n': t = 2; break; case 'b': t = 3; break; case 'r': t = 4; break; case 'q': t = 5; break; case 'k': t = 6; break; }
      IO[r * 8 + c] = t | col; c++;
    }
  }
  p++; IO[64] = (*p == 'b'); p += 2;
  int cas = 0; while(*p && *p != ' '){ if(*p == 'K') cas |= 1; if(*p == 'Q') cas |= 2; if(*p == 'k') cas |= 4; if(*p == 'q') cas |= 8; p++; } p++;
  IO[65] = cas;
  if(*p == '-') IO[66] = -1; else { int f = p[0] - 'a', rk = p[1] - '0'; IO[66] = (8 - rk) * 8 + f; }
  IO[67] = 0; IO[68] = 1;
}
static std::string sqn(int i){ std::string s; s += (char)('a' + (i & 7)); s += (char)('0' + 8 - (i >> 3)); return s; }

int main(){
  px_init();
  struct T { const char* fen; int d; int expect; } tests[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
  };
  int bad = 0;
  for(auto& t : tests){ set_fen(t.fen); int n = px_perft(t.d); printf("perft d%d %s -> %d (expect %d) %s\n", t.d, "", n, t.expect, n == t.expect ? "OK" : "FAIL"); if(n != t.expect) bad++; }

  // tactical sanity: mate in 1, mate in 2, hanging queen
  struct S { const char* fen; const char* best; int ms; } ss[] = {
    {"6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", "a1a8", 500},                       // back-rank mate
    {"r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4", "h5f7", 500}, // scholar's mate
    {"4k3/8/8/8/8/8/4q3/4K2R w K - 0 1", "", 300},
  };
  for(auto& s : ss){
    set_fen(s.fen); IO[69] = 2; IO[70] = s.ms; IO[71] = 64; IO[77] = 1; IO[78] = 0;
    double t0 = px_now(); px_search(); double dt = px_now() - t0;
    std::string mv = sqn(IO[0]) + sqn(IO[1]);
    printf("search %s -> %s  score=%d depth=%d nodes=%d  %.0fms  %s\n", s.fen, mv.c_str(), IO[3], IO[4], IO[5], dt, (s.best[0] && mv != s.best) ? "FAIL" : "ok");
    if(s.best[0] && mv != s.best) bad++;
  }
  // speed test: startpos 1500ms
  set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); IO[69] = 2; IO[70] = 1500; IO[71] = 64;
  px_search(); printf("startpos best %s%s depth=%d nodes=%d (%.0f knps)\n", sqn(IO[0]).c_str(), sqn(IO[1]).c_str(), IO[4], IO[5], IO[5] / 1500.0);
  // skill mode runs
  set_fen("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3"); IO[69] = 1; IO[70] = 400; IO[71] = 3; IO[72] = 4; IO[73] = 40000; IO[74] = 60; IO[75] = 40; IO[76] = 1; IO[77] = 7;
  px_search(); printf("skill hard: %s%s depth=%d status=%d\n", sqn(IO[0]).c_str(), sqn(IO[1]).c_str(), IO[4], IO[6]);
  set_fen("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3"); IO[69] = 0; IO[77] = 3;
  px_search(); printf("random: %s%s status=%d\n", sqn(IO[0]).c_str(), sqn(IO[1]).c_str(), IO[6]);
  // no legal moves (stalemate) and checkmate
  set_fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"); IO[69] = 2; IO[70] = 100; IO[71] = 10; px_search(); printf("stalemate status=%d (expect 0)\n", IO[6]); if(IO[6] != 0) bad++;
  printf(bad ? "SOME TESTS FAILED (%d)\n" : "ALL OK\n", bad);
  return bad != 0;
}
