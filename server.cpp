/*
 * ═══════════════════════════════════════════════════════════════════════
 *  ARBITRAGE GAME — C++ BACKEND
 *
 *  Build:   g++ -std=c++17 -O2 -pthread -o server server.cpp
 *  Run:     ./server
 *  Listens: http://0.0.0.0:8080
 *
 *  Dependency: httplib.h (single header)
 *
 *  Endpoints:
 *    GET  /              → index.html
 *    GET  /state         → full game state JSON
 *    POST /select        → { "idx": N, "delta": +1 or -1 }
 *    POST /execute       → execute current trade
 *    POST /clear         → clear all selections
 *    POST /step-window   → called by frontend when countdown hits 0
 *    POST /start-window  → called after inter-window pause ends
 * ═══════════════════════════════════════════════════════════════════════
 */

#include "httplib.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <algorithm>
#include <numeric>
#include <random>
#include <sstream>
#include <cmath>
#include <chrono>

/* ════════════════════════════════════════════════════════════════════════════
   RNG SETUP
   ════════════════════════════════════════════════════════════════════════════*/

std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
int    randInt   (int    a, int    b) { return std::uniform_int_distribution<int>   (a, b)(rng); }
double randDouble(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
int    clampI    (int    v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

/* ════════════════════════════════════════════════════════════════════════════
   DATA TYPES
   ════════════════════════════════════════════════════════════════════════════*/

// One good inside a card bundle, e.g. {"gold", 3} means 3 gold bars
struct GoodQty { std::string good; int qty; };

// A market card — a bundle of goods at a specific bid/ask
struct Card {
    std::vector<GoodQty> goods;
    int  ask   = 0;
    int  bid   = 0;
    bool empty = true;
};

// FIX: struct member syntax was wrong — used commas instead of semicolons
// WRONG: struct ArbResult {int buyIdx, int sellIdx, pnl};
struct ArbResult { int buyIdx; int sellIdx; int pnl; };

// FIX: same issue — mixed comma/semicolon, and std::string must come last or
//      be separated properly. Reordered so primitives group together.
// WRONG: struct TradeRecord {int id, pnl, pts, std::string desc; };
struct TradeRecord { int id; int pnl; int pts; std::string desc; };

/* ════════════════════════════════════════════════════════════════════════════
   GAME CONFIG
   ════════════════════════════════════════════════════════════════════════════*/

// Underlying fair-value prices per unit of each good
const std::map<std::string,int> BASE = {
    {"gold",18},{"silver",7},{"oil",12},{"wheat",3},{"copper",2},
    {"corn",1},{"cocoa",2},{"rice",4},{"coffee",4},{"platinum",20},{"sugar",6}
};
const std::map<std::string,std::string> LABELS = {
    {"gold","Gold"},{"silver","Silver"},{"oil","Oil"},{"wheat","Wheat"},{"copper","Copper"},
    {"corn","Corn"},{"cocoa","Cocoa"},{"rice","Rice"},{"platinum","Platinum"},
    {"coffee","Coffee"},{"sugar","Sugar"}
};
const std::vector<std::string> GOODS = {
    "gold","oil","silver","copper","wheat","corn","cocoa","rice","platinum","coffee","sugar"
};

// Static level thresholds
struct LevelDef    { std::string name; int next; };
const std::vector<LevelDef> STATIC_LEVELS = {
    {"Beginner",3},{"Novice",7},{"Trader",12},{"Pro",18}
};

// Dynamic/boss level config: name, points to unlock, window seconds
struct BossSubDef { std::string name; int pts; int window; };
const std::vector<BossSubDef> BOSS_SUBS = {
    {"Boss I",18,18},{"Boss II",25,15},{"Legend",33,12},{"Apex",40,10}
};

/* ════════════════════════════════════════════════════════════════════════════
   SCENARIO DEFINITIONS  (static levels)
   Each scenario guarantees a positive arb:
     buys[]  = cards player must BUY  (goods composition + how many units)
     sells[] = cards player must SELL (goods composition + how many units)
   Goods net to zero across both sides.
   Prices are reverse-engineered so sell revenue > buy cost.
   ════════════════════════════════════════════════════════════════════════════*/

struct ScenarioSide { std::vector<GoodQty> goods; int qty; };
struct Scenario     { std::vector<ScenarioSide> buys, sells; };

// Helper macros to keep scenario definitions readable
#define G1(g,q)          std::vector<GoodQty>{{g,q}}
#define G2(g,q,h,r)      std::vector<GoodQty>{{g,q},{h,r}}
#define G3(g,q,h,r,k,s)  std::vector<GoodQty>{{g,q},{h,r},{k,s}}
#define SS(gv,n)          ScenarioSide{gv,n}

const std::vector<std::vector<Scenario>> SCENARIOS = {
  /* ── Level 0: Beginner ── single good, qty 1 */
  {
    { {SS(G1("oil",   1),1)}, {SS(G1("oil",   1),1)} },
    { {SS(G1("gold",  1),1)}, {SS(G1("gold",  1),1)} },
    { {SS(G1("wheat", 1),1)}, {SS(G1("wheat", 1),1)} },
    { {SS(G1("copper",1),1)}, {SS(G1("copper",1),1)} },
    { {SS(G1("silver",1),1)}, {SS(G1("silver",1),1)} },
  },
  /* ── Level 1: Novice ── bundles vs singles, qty up to 2 */
  {
    { {SS(G1("gold",  1),1), SS(G1("oil",   1),1)}, {SS(G2("gold",  1,"oil",   1),1)} },
    { {SS(G1("silver",1),1), SS(G1("oil",   1),1)}, {SS(G2("silver",1,"oil",   1),1)} },
    { {SS(G1("gold",  1),1), SS(G1("silver",1),1)}, {SS(G2("gold",  1,"silver",1),1)} },
    { {SS(G1("wheat", 1),1), SS(G1("copper",1),1)}, {SS(G2("wheat", 1,"copper",1),1)} },
    { {SS(G1("gold",  1),1), SS(G1("copper",1),1)}, {SS(G2("gold",  1,"copper",1),1)} },
    { {SS(G2("gold",  1,"oil",   1),1)}, {SS(G1("gold",  1),1), SS(G1("oil",   1),1)} },
    { {SS(G2("silver",1,"oil",   1),1)}, {SS(G1("silver",1),1), SS(G1("oil",   1),1)} },
    { {SS(G2("wheat", 1,"copper",1),1)}, {SS(G1("wheat", 1),1), SS(G1("copper",1),1)} },
    { {SS(G1("oil",  2),1)},  {SS(G1("oil",  1),2)} },
    { {SS(G1("gold", 2),1)},  {SS(G1("gold", 1),2)} },
    { {SS(G1("silver",2),1)}, {SS(G1("silver",1),2)} },
    { {SS(G1("wheat", 2),1)}, {SS(G1("wheat", 1),2)} },
    { {SS(G1("copper",2),1)}, {SS(G1("copper",1),2)} },
    { {SS(G1("oil",  1),2)},  {SS(G1("oil",  2),1)} },
    { {SS(G1("gold", 1),2)},  {SS(G1("gold", 2),1)} },
  },
  /* ── Level 2: Trader ── qty up to 3, multi-card legs */
  {
    { {SS(G1("gold",  1),2)}, {SS(G1("gold",  2),1)} },
    { {SS(G1("oil",   1),2)}, {SS(G1("oil",   2),1)} },
    { {SS(G1("silver",1),2)}, {SS(G1("silver",2),1)} },
    { {SS(G1("wheat", 1),2)}, {SS(G1("wheat", 2),1)} },
    { {SS(G1("copper",1),2)}, {SS(G1("copper",2),1)} },
    { {SS(G1("oil",   1),3)}, {SS(G1("oil",   3),1)} },
    { {SS(G1("gold",  1),3)}, {SS(G1("gold",  3),1)} },
    { {SS(G1("silver",1),3)}, {SS(G1("silver",3),1)} },
    { {SS(G1("wheat", 1),3)}, {SS(G1("wheat", 3),1)} },
    { {SS(G1("gold",  1),2), SS(G1("oil",   1),1)}, {SS(G2("gold",  2,"oil",   1),1)} },
    { {SS(G1("gold",  1),2), SS(G1("silver",1),1)}, {SS(G2("gold",  2,"silver",1),1)} },
    { {SS(G1("oil",   1),2), SS(G1("gold",  1),1)}, {SS(G2("oil",   2,"gold",  1),1)} },
    { {SS(G1("oil",   1),2), SS(G1("silver",1),1)}, {SS(G2("oil",   2,"silver",1),1)} },
    { {SS(G1("silver",1),2), SS(G1("oil",   1),1)}, {SS(G2("silver",2,"oil",   1),1)} },
    { {SS(G1("silver",1),2), SS(G1("gold",  1),1)}, {SS(G2("silver",2,"gold",  1),1)} },
    { {SS(G1("wheat", 1),2), SS(G1("copper",1),1)}, {SS(G2("wheat", 2,"copper",1),1)} },
    { {SS(G1("copper",1),2), SS(G1("wheat", 1),1)}, {SS(G2("copper",2,"wheat", 1),1)} },
    { {SS(G2("gold",  2,"oil",   1),1)}, {SS(G1("gold",  1),2), SS(G1("oil",   1),1)} },
    { {SS(G2("oil",   2,"silver",1),1)}, {SS(G1("oil",   1),2), SS(G1("silver",1),1)} },
    { {SS(G2("silver",2,"gold",  1),1)}, {SS(G1("silver",1),2), SS(G1("gold",  1),1)} },
    { {SS(G2("wheat", 2,"copper",1),1)}, {SS(G1("wheat", 1),2), SS(G1("copper",1),1)} },
    { {SS(G2("gold",  1,"oil",   1),2)}, {SS(G2("gold",  2,"oil",   2),1)} },
    { {SS(G2("gold",  1,"silver",1),2)}, {SS(G2("gold",  2,"silver",2),1)} },
    { {SS(G2("oil",   1,"silver",1),2)}, {SS(G2("oil",   2,"silver",2),1)} },
    { {SS(G2("wheat", 1,"copper",1),2)}, {SS(G2("wheat", 2,"copper",2),1)} },
    { {SS(G1("silver",2),1), SS(G1("oil",  1),2)}, {SS(G2("silver",2,"oil",  2),1)} },
    { {SS(G1("gold",  2),1), SS(G1("wheat",1),2)}, {SS(G2("gold",  2,"wheat",2),1)} },
    { {SS(G1("copper",2),1), SS(G1("oil",  1),2)}, {SS(G2("copper",2,"oil",  2),1)} },
    { {SS(G1("gold",1),1), SS(G1("oil",   1),1), SS(G1("silver",1),1)}, {SS(G3("gold",1,"oil",1,"silver",1),1)} },
    { {SS(G1("gold",1),1), SS(G1("wheat", 1),1), SS(G1("copper",1),1)}, {SS(G3("gold",1,"wheat",1,"copper",1),1)} },
    { {SS(G1("oil", 1),1), SS(G1("wheat", 1),1), SS(G1("copper",1),1)}, {SS(G3("oil",1,"wheat",1,"copper",1),1)} },
    { {SS(G3("gold",1,"oil",1,"silver",1),1)}, {SS(G1("gold",1),1), SS(G1("oil",   1),1), SS(G1("silver",1),1)} },
    { {SS(G3("oil",1,"wheat",1,"copper",1),1)},{SS(G1("oil",1), 1), SS(G1("wheat", 1),1), SS(G1("copper",1),1)} },
  },
  /* ── Level 3: Pro ── qty up to 4, four/five goods */
  {
    { {SS(G1("gold",  1),3)}, {SS(G1("gold",  3),1)} },
    { {SS(G1("oil",   1),3)}, {SS(G1("oil",   3),1)} },
    { {SS(G1("silver",1),3)}, {SS(G1("silver",3),1)} },
    { {SS(G1("gold",  1),4)}, {SS(G1("gold",  4),1)} },
    { {SS(G1("oil",   1),4)}, {SS(G1("oil",   4),1)} },
    { {SS(G1("copper",1),4)}, {SS(G1("copper",4),1)} },
    { {SS(G1("wheat", 1),4)}, {SS(G1("wheat", 4),1)} },
    { {SS(G2("gold",  2,"oil",   1),2)}, {SS(G2("gold",  4,"oil",   2),1)} },
    { {SS(G2("silver",2,"oil",   1),2)}, {SS(G2("silver",4,"oil",   2),1)} },
    { {SS(G2("gold",  2,"silver",1),2)}, {SS(G2("gold",  4,"silver",2),1)} },
    { {SS(G2("copper",2,"wheat", 1),2)}, {SS(G2("copper",4,"wheat", 2),1)} },
    { {SS(G2("gold",  3,"oil",   1),1)}, {SS(G1("gold",  1),3), SS(G1("oil",   1),1)} },
    { {SS(G2("silver",3,"oil",   1),1)}, {SS(G1("silver",1),3), SS(G1("oil",   1),1)} },
    { {SS(G2("gold",  3,"silver",1),1)}, {SS(G1("gold",  1),3), SS(G1("silver",1),1)} },
    { {SS(G2("wheat", 3,"copper",1),1)}, {SS(G1("wheat", 1),3), SS(G1("copper",1),1)} },
    { {SS(G1("gold",  1),3), SS(G1("silver",1),1)}, {SS(G2("gold",  3,"silver",1),1)} },
    { {SS(G1("oil",   1),3), SS(G1("copper",1),1)}, {SS(G2("oil",   3,"copper",1),1)} },
    { {SS(G1("silver",1),3), SS(G1("wheat", 1),1)}, {SS(G2("silver",3,"wheat", 1),1)} },
    { {SS(G2("gold",  1,"oil",   1),2), SS(G1("silver",1),2)}, {SS(G3("gold",  2,"oil",   2,"silver",2),1)} },
    { {SS(G2("oil",   1,"silver",1),2), SS(G1("copper",1),2)}, {SS(G3("oil",   2,"silver",2,"copper",2),1)} },
    { {SS(G2("gold",  1,"wheat", 1),2), SS(G1("copper",1),2)}, {SS(G3("gold",  2,"wheat", 2,"copper",2),1)} },
    { {SS(G3("gold",  2,"oil",   2,"silver",2),1)}, {SS(G2("gold",  1,"oil",   1),2), SS(G1("silver",1),2)} },
    { {SS(G3("oil",   2,"wheat", 2,"copper",2),1)}, {SS(G2("oil",   1,"wheat", 1),2), SS(G1("copper",1),2)} },
    { {SS(G1("wheat",3),2)},                        {SS(G1("wheat",2),3)} },
    { {SS(G1("copper",3),2)},                       {SS(G1("copper",2),3)} },
    { {SS(G1("copper",4),1), SS(G1("silver",2),1)}, {SS(G2("copper",2,"silver",1),2)} },
    { {SS(G1("wheat", 4),1), SS(G1("oil",   2),1)}, {SS(G2("wheat", 2,"oil",   1),2)} },
    { {SS(G1("gold",2),1),  SS(G1("oil",   2),1)},  {SS(G2("gold",  2,"oil",   2),1)} },
    { {SS(G1("gold",2),1),  SS(G1("silver",2),1)},  {SS(G2("gold",  2,"silver",2),1)} },
    { {SS(G1("oil", 2),1),  SS(G1("copper",2),1)},  {SS(G2("oil",   2,"copper",2),1)} },
    { {SS(G1("wheat",2),1), SS(G1("silver",2),1)},  {SS(G2("wheat", 2,"silver",2),1)} },
    { {SS(G2("gold",  2,"oil",   2),1)}, {SS(G1("gold",  2),1), SS(G1("oil",   2),1)} },
    { {SS(G2("gold",  2,"silver",2),1)}, {SS(G1("gold",  2),1), SS(G1("silver",2),1)} },
    { {SS(G2("oil",   2,"copper",2),1)}, {SS(G1("oil",   2),1), SS(G1("copper",2),1)} },
    { {SS(G2("gold",  1,"oil",   1),3)}, {SS(G1("gold",  3),1), SS(G1("oil",   3),1)} },
    { {SS(G2("silver",1,"copper",1),3)}, {SS(G1("silver",3),1), SS(G1("copper",3),1)} },
    { {SS(G2("wheat", 1,"gold",  1),3)}, {SS(G1("wheat", 3),1), SS(G1("gold",  3),1)} },
    { {SS(G1("gold",  3),1), SS(G1("silver",3),1)}, {SS(G2("gold",  1,"silver",1),3)} },
    { {SS(G1("oil",   3),1), SS(G1("copper",3),1)}, {SS(G2("oil",   1,"copper",1),3)} },
    { {SS(G1("wheat", 3),1), SS(G1("oil",   3),1)}, {SS(G2("wheat", 1,"oil",   1),3)} },
    { {SS(G3("gold",1,"oil",1,"silver",1),4)}, {SS(G1("gold",4),1), SS(G1("oil",4),1), SS(G1("silver",4),1)} },
    { {SS(G3("oil",1,"wheat",1,"copper",1),4)},{SS(G1("oil",4),1),  SS(G1("wheat",4),1),SS(G1("copper",4),1)} },
    { {SS(G1("gold",4),1), SS(G1("oil",4),1), SS(G1("silver",4),1)}, {SS(G3("gold",1,"oil",1,"silver",1),4)} },
    { {SS(G1("wheat",4),1),SS(G1("copper",4),1),SS(G1("oil",4),1)},  {SS(G3("wheat",1,"copper",1,"oil",1),4)} },
  },
};

#undef G1
#undef G2
#undef G3
#undef SS

/* ════════════════════════════════════════════════════════════════════════════
   BOSS PAIR TEMPLATES
   Two cards with identical goods composition, priced so one is cheap (buy)
   and one is expensive (sell). Guaranteed positive arb per pair.
   ════════════════════════════════════════════════════════════════════════════*/

const std::vector<std::vector<std::vector<GoodQty>>> BOSS_PAIRS = {
  /* Boss I — simple singles */
  {
    {{"gold",1}}, {{"oil",1}}, {{"silver",1}}, {{"wheat",1}}, {{"copper",1}},
  },
  /* Boss II — qty-2 singles + two-good bundles */
  {
    {{"gold",2}}, {{"oil",2}}, {{"silver",2}}, {{"wheat",2}}, {{"copper",2}},
    {{"gold",1},{"oil",1}},    {{"gold",1},{"silver",1}}, {{"oil",1},{"silver",1}},
    {{"wheat",1},{"copper",1}},{{"gold",1},{"copper",1}}, {{"oil",1},{"wheat",1}},
    {{"silver",1},{"copper",1}},
  },
  /* Legend — qty-3 singles, [2A+1B] bundles, [1A+1B] at qty 2 */
  {
    {{"gold",3}}, {{"oil",3}}, {{"silver",3}}, {{"wheat",3}}, {{"copper",3}},
    {{"gold",2},{"oil",1}},    {{"gold",2},{"silver",1}}, {{"oil",2},{"gold",1}},
    {{"oil",2},{"silver",1}},  {{"silver",2},{"gold",1}}, {{"silver",2},{"oil",1}},
    {{"wheat",2},{"copper",1}},{{"copper",2},{"wheat",1}},{{"gold",2},{"wheat",1}},
    {{"oil",2},{"copper",1}},
    {{"gold",2},{"silver",2}}, {{"oil",2},{"copper",2}},  {{"wheat",2},{"gold",2}},
  },
  /* Apex — qty-4, [3A+1B], [2A+2B], [2A+1B+1C], [3A+2B], [2A+2B+1C] */
  {
    {{"gold",4}}, {{"oil",4}}, {{"silver",4}}, {{"wheat",4}}, {{"copper",4}},
    {{"gold",3},{"oil",1}},    {{"gold",3},{"silver",1}},  {{"oil",3},{"gold",1}},
    {{"oil",3},{"silver",1}},  {{"silver",3},{"gold",1}},  {{"silver",3},{"copper",1}},
    {{"wheat",3},{"oil",1}},   {{"copper",3},{"wheat",1}},
    {{"gold",2},{"oil",2}},    {{"gold",2},{"silver",2}},  {{"oil",2},{"silver",2}},
    {{"oil",2},{"copper",2}},  {{"wheat",2},{"silver",2}}, {{"copper",2},{"gold",2}},
    {{"wheat",2},{"oil",2}},
    {{"gold",2},{"oil",1},{"silver",1}},   {{"gold",2},{"silver",1},{"copper",1}},
    {{"oil",2},{"gold",1},{"wheat",1}},    {{"oil",2},{"silver",1},{"copper",1}},
    {{"silver",2},{"gold",1},{"oil",1}},   {{"silver",2},{"wheat",1},{"copper",1}},
    {{"wheat",2},{"gold",1},{"copper",1}}, {{"copper",2},{"oil",1},{"silver",1}},
    {{"gold",3},{"oil",2}},    {{"gold",3},{"silver",2}},  {{"oil",3},{"copper",2}},
    {{"silver",3},{"wheat",2}},{{"wheat",3},{"copper",2}}, {{"copper",3},{"gold",2}},
    {{"gold",2},{"oil",2},{"silver",1}},   {{"gold",2},{"silver",2},{"copper",1}},
    {{"oil",2},{"silver",2},{"wheat",1}},  {{"oil",2},{"wheat",2},{"copper",1}},
    {{"silver",2},{"copper",2},{"gold",1}},{{"wheat",2},{"copper",2},{"oil",1}},
    {{"gold",2},{"wheat",2},{"copper",1}}, {{"oil",2},{"gold",2},{"copper",1}},
  },
};

// Filler cards — goods that won't accidentally form arb pairs
// These use the "exotic" goods from BASE that don't appear in BOSS_PAIRS
const std::vector<std::vector<GoodQty>> FILLERS = {
    {{"corn",1}},  {{"cocoa",1}},  {{"corn",2},{"coffee",1}},
    {{"platinum",1},{"rice",1}},   {{"sugar",3}}, {{"corn",2},{"cocoa",1}},
    {{"rice",2}},  {{"coffee",2}}, {{"platinum",1}},
};

/* ════════════════════════════════════════════════════════════════════════════
   GAME STATE  — one global instance for single-player
   ════════════════════════════════════════════════════════════════════════════*/

struct GameState
{
  // Scoring
  int arbPts = 0, wins = 0, losses = 0, tradeCount = 0;

  // Progression
  int level = 0; // 0-3 = static levels, 4 = boss
  int bossSubIdx = 0;
  bool liveMode = false;

  std::vector<TradeRecord> history;

  // Board: 8 grid slots + player qty selection per slot
  std::array<Card, 8> slots;
  std::array<int, 8> qty = {};

  // Boss: underlying mid prices and window state
  std::map<std::string, int> liveMids, windowOpenMids;
  int currWindowDuration = 18;
  std::vector<ArbResult> lastArbs;
  int lastBestPnl = 0;
  bool executedThisWindow = false;

  GameState() { for(auto& s:slots) s.empty=true; }
};
#include <mutex>
#include <unordered_map>

inline std::unordered_map<std::string, GameState>& getSessionMap() {
    static std::unordered_map<std::string, GameState> instances;
    return instances;
}

inline std::mutex& getSessionMutex() {
    static std::mutex mtx;
    return mtx;
}

// Thread-safe session controller (Notice the closing brace fix!)
inline GameState& getOrCreateSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(getSessionMutex());
    
    // Safely look up or instantiate the state inside the map bucket
    return getSessionMap()[sessionId];
}

/* ════════════════════════════════════════════════════════════════════════════
   UTILITY FUNCTIONS
   ════════════════════════════════════════════════════════════════════════════*/

// Canonical key for goods composition — used to match identical cards
std::string goodsKey(const std::vector<GoodQty>& g){
    auto s=g;
    std::sort(s.begin(),s.end(),[](auto& a,auto& b){ return a.good<b.good; });
    std::string k;
    for(auto& x:s){ if(!k.empty()) k+="|"; k+=x.good+":"+std::to_string(x.qty); }
    return k;
}

// Fair value of a card = sum(base_price[g] * qty)
int fairVal(const std::vector<GoodQty>& g){
    int t=0; for(auto& x:g) t+=BASE.at(x.good)*x.qty; return t;
}

// Build a priced card: ask = mid+spread, bid = max(mid-spread, 1)
Card makeCard(const std::vector<GoodQty>& g, int mid, int sp){
    Card c; c.goods=g; c.ask=mid+sp; c.bid=std::max(mid-sp,1); c.empty=false; return c;
}

// Human-readable card title: "2× Gold + 1× Oil"
std::string cardTitle(const Card& c){
    std::string t;
    for(auto& x:c.goods){
        if(!t.empty()) t+=" + ";
        t+=std::to_string(x.qty)+"× "+LABELS.at(x.good);
    }
    return t;
}

// Return a shuffled [0..7] for random grid placement
std::vector<int> shuffled8(){
    std::vector<int> p(8); std::iota(p.begin(),p.end(),0);
    std::shuffle(p.begin(),p.end(),rng); return p;
}

/* ════════════════════════════════════════════════════════════════════════════
   ARB DETECTION
   Scan all pairs (i,j) where slots share identical goods composition.
   pnl = slots[j].bid - slots[i].ask  (buy i, sell j)
   Keep only positive pnl, best direction per pair, sorted desc.
   ════════════════════════════════════════════════════════════════════════════*/

std::vector<ArbResult> findAllArbs(GameState& G){
    std::map<std::string,ArbResult> best;
    for(int i=0;i<8;i++){
        if(G.slots[i].empty) continue;
        for(int j=0;j<8;j++){
            if(i==j||G.slots[j].empty) continue;
            if(goodsKey(G.slots[i].goods)!=goodsKey(G.slots[j].goods)) continue;
            int pnl=G.slots[j].bid-G.slots[i].ask;
            if(pnl<=0) continue;
            std::string k=std::to_string(std::min(i,j))+"-"+std::to_string(std::max(i,j));
            if(!best.count(k)||pnl>best[k].pnl) best[k]={i,j,pnl};
        }
    }
    std::vector<ArbResult> r;
    for(auto& kv:best) r.push_back(kv.second);
    std::sort(r.begin(),r.end(),[](auto& a,auto& b){ return a.pnl>b.pnl; });
    return r;
}

/* ════════════════════════════════════════════════════════════════════════════
   STATIC CARD PLACEMENT
   1. Pick a random scenario for the current level
   2. Price buy-side at fair value ± 8% noise
   3. Reverse-engineer sell-side so total revenue = cost + arbAmount
   4. Shuffle all cards into random grid positions
   5. Random filler cards in empty slots (p=0.125 each)
   ════════════════════════════════════════════════════════════════════════════*/

void placeStaticCards(GameState& G){
    G.qty.fill(0);
    for(auto& s:G.slots) s.empty=true;

    const auto& scList=SCENARIOS[G.level];
    const auto& sc=scList[randInt(0,(int)scList.size()-1)];
    int arb=randInt(1,12);

    std::vector<Card> cards;
    std::vector<int>  qtys;

    for(const auto& side:sc.buys){
        int fair=fairVal(side.goods);
        int mid=std::max(2,(int)std::round(fair*(1.0+randDouble(-0.08,0.08))));
        int sp=std::max(1,(int)std::round(fair*0.06));
        cards.push_back(makeCard(side.goods,mid,sp));
        qtys.push_back(side.qty);
    }

    int totalCost=0;
    for(int i=0;i<(int)cards.size();i++) totalCost+=cards[i].ask*qtys[i];
    int targetRev=totalCost+arb;

    int totalSF=0;
    for(const auto& s:sc.sells) totalSF+=fairVal(s.goods)*s.qty;
    if(!totalSF) totalSF=1;

    for(const auto& side:sc.sells){
        int sf=fairVal(side.goods)*side.qty;
        int bid=(int)std::round((double)targetRev*sf/totalSF/side.qty);
        int sp=std::max(1,(int)std::round(fairVal(side.goods)*0.06));
        cards.push_back(makeCard(side.goods,bid+sp,sp));
        qtys.push_back(side.qty);
    }

    auto pos=shuffled8();
    for(int i=0;i<(int)cards.size();i++) G.slots[pos[i]]=cards[i];

    static const std::vector<std::vector<GoodQty>> EX={
        {{"corn",1}},{{"cocoa",1}},{{"corn",2},{"coffee",1}},
        {{"platinum",1},{"rice",1}},{{"sugar",3}},{{"corn",2},{"cocoa",1}}
    };
    for(int i=(int)cards.size();i<8;i++){
        if(randDouble(0,1)<0.125){
            const auto& g=EX[randInt(0,(int)EX.size()-1)];
            int f=fairVal(g), sp=std::max(1,(int)std::round(f*0.07));
            G.slots[pos[i]]=makeCard(g,f,sp);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   BOSS CARD PLACEMENT
   Two guaranteed arb pairs planted deterministically:
     Best pair      → bid_high - ask_low = bestGap ($6-12)
     Secondary pair → bid_high - ask_low = secGap  ($2-5)
   Remaining slots: decoy cards drawn from the same BOSS_PAIRS pool
   (look realistic but have no partner) with 75%/25% presence chance.
   ════════════════════════════════════════════════════════════════════════════*/

void placeBossCards(GameState& G){
    G.qty.fill(0);
    G.executedThisWindow=false;
    for(auto& s:G.slots) s.empty=true;

    auto pairs=BOSS_PAIRS[G.bossSubIdx];
    std::shuffle(pairs.begin(),pairs.end(),rng);

    const auto& bd=pairs[0];
    const auto& sd=pairs.size()>1?pairs[1]:pairs[0];

    std::vector<Card> defs;
    std::set<std::string> usedKeys;

    {
        int f=fairVal(bd);
        int sp=std::max(1,(int)std::round(f*0.06));
        int ml=std::max(2,(int)std::round(f*(1+randDouble(-0.05,0.05))));
        int gap=randInt(6,12);
        defs.push_back(makeCard(bd,ml,sp));
        defs.push_back(makeCard(bd,ml+gap+sp*2,sp));
        usedKeys.insert(goodsKey(bd));
    }

    {
        int f=fairVal(sd);
        int sp=std::max(1,(int)std::round(f*0.06));
        int ml=std::max(2,(int)std::round(f*(1+randDouble(-0.05,0.05))));
        int gap=randInt(2,5);
        defs.push_back(makeCard(sd,ml,sp));
        defs.push_back(makeCard(sd,ml+gap+sp*2,sp));
        usedKeys.insert(goodsKey(sd));
    }

    std::vector<std::vector<GoodQty>> decoyPool;
    for(const auto& p:pairs){
        if(!usedKeys.count(goodsKey(p))){
            decoyPool.push_back(p);
            usedKeys.insert(goodsKey(p));
        }
    }
    for(int sub=0;sub<(int)BOSS_PAIRS.size();sub++){
        if(sub==G.bossSubIdx) continue;
        for(const auto& p:BOSS_PAIRS[sub]){
            if(!usedKeys.count(goodsKey(p))){
                decoyPool.push_back(p);
                usedKeys.insert(goodsKey(p));
            }
        }
    }
    std::shuffle(decoyPool.begin(),decoyPool.end(),rng);

    int decoyIdx=0;
    for(int i=0;i<4;i++){
        if(randDouble(0,1)<0.25){
            defs.push_back(Card{});
            continue;
        }
        if(decoyIdx>=(int)decoyPool.size()){
            defs.push_back(Card{});
            continue;
        }
        const auto& goods=decoyPool[decoyIdx++];
        int f=fairVal(goods);
        int sp=std::max(1,(int)std::round(f*0.07));
        int mid=std::max(2,(int)std::round(f*(1+randDouble(-0.10,0.10))));
        defs.push_back(makeCard(goods,mid,sp));
    }

    auto pos=shuffled8();
    for(int i=0;i<(int)defs.size();i++) G.slots[pos[i]]=defs[i];

    G.lastArbs    =findAllArbs(G);
    G.lastBestPnl =G.lastArbs.empty()?0:G.lastArbs[0].pnl;
}

/* ════════════════════════════════════════════════════════════════════════════
   MID PRICE ENGINE  (boss mode only)
   Prices only step BETWEEN windows — frozen during a window.
   ════════════════════════════════════════════════════════════════════════════*/

void initMids(GameState& G){
    for(const auto& g:GOODS) G.liveMids[g]=BASE.at(g);
}

void stepMids(GameState& G){
    for(const auto& g:GOODS){
        int n=(int)std::round(G.liveMids[g]*(1+randDouble(-0.1,0.1)));
        G.liveMids[g]=clampI(n,(int)std::round(BASE.at(g)*0.5),(int)std::round(BASE.at(g)*2));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   GOODS NET CALCULATION
   +qty = buying (goods received), -qty = selling (goods delivered)
   Valid trade requires every good's net == 0.
   ════════════════════════════════════════════════════════════════════════════*/

std::map<std::string,int> computeNet(GameState& G){
    std::map<std::string,int> net;
    for(int i=0;i<8;i++){
        if(G.slots[i].empty||G.qty[i]==0) continue;
        for(const auto& x:G.slots[i].goods) net[x.good]+=G.qty[i]*x.qty;
    }
    return net;
}

bool isBalanced(const std::map<std::string,int>& net){
    if(net.empty()) return false;
    for(const auto& kv:net) if(kv.second!=0) return false;
    return true;
}

/* ════════════════════════════════════════════════════════════════════════════
   LEVEL-UP CHECK  (called after every executed trade)
   ════════════════════════════════════════════════════════════════════════════*/

std::string checkLevelUp(GameState& G){
    if(!G.liveMode&&G.arbPts>=25&&G.level<4){
        G.level=4; G.liveMode=true;
        initMids(G);
        G.windowOpenMids=G.liveMids;
        G.currWindowDuration=BOSS_SUBS[0].window;
        placeBossCards(G);
        return "BOSS_UNLOCK";
    }
    if(!G.liveMode){
        for(int l=3;l>=0;l--){
            if(G.level<=l&&G.arbPts>=STATIC_LEVELS[l].next){
                G.level=l+1;
                if(G.level<4) return "LEVEL_UP:"+STATIC_LEVELS[G.level].name;
                break;
            }
        }
    }
    if(G.liveMode){
        for(int i=(int)BOSS_SUBS.size()-1;i>G.bossSubIdx;i--){
            if(G.arbPts>=BOSS_SUBS[i].pts){
                G.bossSubIdx=i;
                G.currWindowDuration=BOSS_SUBS[i].window;
                return "BOSS_SUB:"+BOSS_SUBS[i].name+":"+std::to_string(BOSS_SUBS[i].window);
            }
        }
    }
    return "";
}

/* ════════════════════════════════════════════════════════════════════════════
   JSON SERIALISATION
   ════════════════════════════════════════════════════════════════════════════*/

std::string jStr(const std::string& s){
    std::string o="\"";
    for(char c:s){
        if(c=='"')   o+="\\\"";
        else if(c=='\\') o+="\\\\";
        else o+=c;
    }
    return o+"\"";
}

int jInt(const std::string& body,const std::string& key){
    auto p=body.find("\""+key+"\":"); if(p==std::string::npos) return 0;
    p+=key.size()+3;
    while(p<body.size()&&body[p]==' ') p++;
    std::string n;
    if(p<body.size()&&body[p]=='-'){n+='-';p++;}
    while(p<body.size()&&std::isdigit(body[p])) n+=body[p++];
    return n.empty()?0:std::stoi(n);
}

std::string slotJson(const GameState& G, int i){
    const Card& c=G.slots[i];
    if(c.empty) return "{\"empty\":true,\"idx\":"+std::to_string(i)+"}";
    std::string g="[";
    for(int k=0;k<(int)c.goods.size();k++){
        if(k) g+=",";
        g+="{\"good\":"+jStr(c.goods[k].good)
          +",\"qty\":"+std::to_string(c.goods[k].qty)
          +",\"label\":"+jStr(LABELS.at(c.goods[k].good))+"}";
    }
    g+="]";
    return "{\"empty\":false,\"idx\":"+std::to_string(i)
          +",\"ask\":"+std::to_string(c.ask)
          +",\"bid\":"+std::to_string(c.bid)
          +",\"goods\":"+g
          +",\"title\":"+jStr(cardTitle(c))+"}";
}

std::string stateJson(const GameState& G){
    int tot=G.wins+G.losses;
    int wr=tot?clampI((int)std::round((double)G.wins/tot*100),0,100):0;

    std::string lvlName; int lvlPrev=0,lvlTarget=1,lvlProg=0;
    if(!G.liveMode&&G.level<(int)STATIC_LEVELS.size()){
        lvlName=STATIC_LEVELS[G.level].name;
        lvlTarget=STATIC_LEVELS[G.level].next;
        lvlPrev=G.level>0?STATIC_LEVELS[G.level-1].next:0;
    } else if(G.liveMode){
        lvlName=BOSS_SUBS[G.bossSubIdx].name;
        lvlPrev=BOSS_SUBS[G.bossSubIdx].pts;
        lvlTarget=G.bossSubIdx+1<(int)BOSS_SUBS.size()?BOSS_SUBS[G.bossSubIdx+1].pts:lvlPrev+30;
    }
    int span=lvlTarget-lvlPrev; if(span<=0) span=1;
    lvlProg=clampI((int)std::round((double)(G.arbPts-lvlPrev)/span*100),0,100);

    std::string s="{";
    s+="\"arbPts\":"+std::to_string(G.arbPts)+",";
    s+="\"wins\":"+std::to_string(G.wins)+",";
    s+="\"losses\":"+std::to_string(G.losses)+",";
    s+="\"tradeCount\":"+std::to_string(G.tradeCount)+",";
    s+="\"winRate\":"+std::to_string(wr)+",";
    s+="\"liveMode\":"+(G.liveMode?std::string("true"):std::string("false"))+",";
    s+="\"level\":"+std::to_string(G.level)+",";
    s+="\"bossSubIdx\":"+std::to_string(G.bossSubIdx)+",";
    s+="\"levelName\":"+jStr(lvlName)+",";
    s+="\"levelProg\":"+std::to_string(lvlProg)+",";
    s+="\"levelTarget\":"+std::to_string(lvlTarget)+",";
    s+="\"windowDuration\":"+std::to_string(G.currWindowDuration)+",";
    s+="\"lastBestPnl\":"+std::to_string(G.lastBestPnl)+",";
    s+="\"slots\":[";
    for(int i=0;i<8;i++){if(i)s+=","; s+=slotJson(G, i);}
    s+="],";
    s+="\"qty\":[";
    for(int i=0;i<8;i++){if(i)s+=","; s+=std::to_string(G.qty[i]);}
    s+="],";
    s+="\"liveMids\":{";
    for(int i=0;i<(int)GOODS.size();i++){
        if(i)s+=",";
        int m=G.liveMids.count(GOODS[i])?G.liveMids.at(GOODS[i]):BASE.at(GOODS[i]);
        s+=jStr(GOODS[i])+":"+std::to_string(m);
    }
    s+="},";
    s+="\"windowOpenMids\":{";
    for(int i=0;i<(int)GOODS.size();i++){
        if(i)s+=",";
        int m=G.windowOpenMids.count(GOODS[i])?G.windowOpenMids.at(GOODS[i]):BASE.at(GOODS[i]);
        s+=jStr(GOODS[i])+":"+std::to_string(m);
    }
    s+="},";
    s+="\"lastArbs\":[";
    for(int i=0;i<(int)G.lastArbs.size()&&i<3;i++){
        if(i)s+=",";
        const auto& a=G.lastArbs[i];
        s+="{\"pnl\":"+std::to_string(a.pnl)
          +",\"buyIdx\":"+std::to_string(a.buyIdx)
          +",\"sellIdx\":"+std::to_string(a.sellIdx)
          +",\"buyCard\":"+jStr(G.slots[a.buyIdx].empty?"?":cardTitle(G.slots[a.buyIdx]))
          +",\"sellCard\":"+jStr(G.slots[a.sellIdx].empty?"?":cardTitle(G.slots[a.sellIdx]))
          +",\"buyAsk\":"+std::to_string(G.slots[a.buyIdx].empty?0:G.slots[a.buyIdx].ask)
          +",\"sellBid\":"+std::to_string(G.slots[a.sellIdx].empty?0:G.slots[a.sellIdx].bid)
          +"}";
    }
    s+="],";
    s+="\"history\":[";
    for(int i=0;i<(int)G.history.size()&&i<5;i++){
        if(i)s+=",";
        const auto& h=G.history[i];
        s+="{\"id\":"+std::to_string(h.id)+",\"desc\":"+jStr(h.desc)
          +",\"pnl\":"+std::to_string(h.pnl)+",\"pts\":"+std::to_string(h.pts)+"}";
    }
    s+="]";
    s+="}";
    return s;
}

/* ════════════════════════════════════════════════════════════════════════════
   STATIC FILE HELPER
   ════════════════════════════════════════════════════════════════════════════*/

std::string readFile(const std::string& path){
    std::ifstream f(path); if(!f) return "";
    return std::string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());
}

/* ════════════════════════════════════════════════════════════════════════════
   CORS HELPER
   ════════════════════════════════════════════════════════════════════════════*/

void addCors(httplib::Response& res){
    res.set_header("Access-Control-Allow-Origin","*");
    res.set_header("Access-Control-Allow-Methods","GET,POST,OPTIONS");
    res.set_header("Access-Control-Allow-Headers","Content-Type, X-User-Session");
}

/* ════════════════════════════════════════════════════════════════════════════
   REST ROUTES
   ════════════════════════════════════════════════════════════════════════════*/

void setupRoutes(httplib::Server& svr){

    svr.Options(".*",[](const httplib::Request&,httplib::Response& res){ addCors(res); });

    svr.Get("/",[](const httplib::Request&,httplib::Response& res){
        std::string html=readFile("index.html");
        if(html.empty()){ res.status=404; res.set_content("index.html not found","text/plain"); return; }
        res.set_content(html,"text/html");
    });

    svr.Get("/images/(.*)", [](const httplib::Request& req, httplib::Response& res) {
        addCors(res);
        std::string filename = req.matches[1];
        if(filename.find("..") != std::string::npos){
            res.status = 403;
            res.set_content("Forbidden", "text/plain");
            return;
        }
        std::string path = "images/" + filename;
        std::ifstream f(path, std::ios::binary);
        if(!f){
            res.status = 404;
            res.set_content("Not found: " + path, "text/plain");
            return;
        }
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string ctype = "image/png";
        auto dot = filename.rfind('.');
        if(dot != std::string::npos){
            std::string ext = filename.substr(dot);
            if(ext == ".svg")             ctype = "image/svg+xml";
            else if(ext == ".png")        ctype = "image/png";
            else if(ext == ".jpg"|| ext == ".jpeg") ctype = "image/jpeg";
            else if(ext == ".webp")       ctype = "image/webp";
            else if(ext == ".gif")        ctype = "image/gif";
        }
        res.set_content(data, ctype);
    });

    svr.Get("/state",[](const httplib::Request& req,httplib::Response& res){
        addCors(res);
        
        // FIX: Check both casing variants to ensure cross-browser compatibility
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        if(!G.liveMode && G.slots[0].empty && G.slots[1].empty && G.slots[2].empty && G.slots[3].empty) {
            placeStaticCards(G);
        }
        
        res.set_content(stateJson(G),"application/json");
    });

    svr.Post("/select",[](const httplib::Request& req,httplib::Response& res){
        addCors(res);
        
        // FIX: Check both casing variants to ensure cross-browser compatibility
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        int idx=jInt(req.body,"idx"), delta=jInt(req.body,"delta");
        if(idx>=0&&idx<8&&!G.slots[idx].empty) G.qty[idx]+=delta;
        res.set_content(stateJson(G),"application/json");
    });

    svr.Post("/execute",[](const httplib::Request& req,httplib::Response& res){
        addCors(res);
        
        // FIX: Check both casing variants to ensure cross-browser compatibility
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        
        bool any=false; for(int q:G.qty) if(q){any=true;break;}
        if(!any){
            res.set_content("{\"error\":\"Nothing selected\",\"state\":"+stateJson(G)+"}","application/json");
            return;
        }
        auto net=computeNet(G);
        if(!isBalanced(net)){
            res.set_content("{\"error\":\"Not balanced\",\"state\":"+stateJson(G)+"}","application/json");
            return;
        }

        int cost=0,rev=0; std::string desc;
        for(int i=0;i<8;i++){
            if(G.slots[i].empty) continue;
            if(G.qty[i]>0){
                cost+=G.qty[i]*G.slots[i].ask;
                if(!desc.empty()) desc+=" / ";
                desc+="Buy "+std::to_string(G.qty[i])+"× ["+cardTitle(G.slots[i])+"]";
            }
            if(G.qty[i]<0){
                rev+=(-G.qty[i])*G.slots[i].bid;
                if(!desc.empty()) desc+=" / ";
                desc+="Sell "+std::to_string(-G.qty[i])+"× ["+cardTitle(G.slots[i])+"]";
            }
        }
        int pnl=rev-cost;
        int pts=0; std::string msg,mtype;

        if(G.liveMode){
            if(pnl>0){
                if(pnl>=G.lastBestPnl){ pts=2; msg="Best arb! +2 pts — $"+std::to_string(pnl)+" spread."; mtype="great"; }
                else                  { pts=1; msg="+1 pt — arb $"+std::to_string(pnl)+". Best was $"+std::to_string(G.lastBestPnl)+"."; mtype="good"; }
                G.wins++;
            } else {
                pts=-1; msg="-1 pt. Lost $"+std::to_string(std::abs(pnl))+"."; mtype="bad"; G.losses++;
            }
            G.executedThisWindow=true;
        } else {
            if(pnl>0){ pts=1; msg="+1 pt! Spread $"+std::to_string(pnl)+"."; mtype="good"; G.wins++; }
            else if(pnl<0){ pts=-1; msg="-1 pt. Lost $"+std::to_string(std::abs(pnl))+"."; mtype="bad"; G.losses++; }
            else { msg="Flat."; mtype="info"; }
        }

        G.arbPts+=pts; G.tradeCount++;
        G.history.insert(G.history.begin(),{G.tradeCount,pnl,pts,desc});

        std::string extra=checkLevelUp(G);
        if(!G.liveMode){ G.qty.fill(0); placeStaticCards(G); }
        else            { G.qty.fill(0); }

        res.set_content(
            "{\"msg\":" + jStr(msg) + ",\n\"msgType\":" + jStr(mtype) +
            ",\"pts\":" + std::to_string(pts) + ",\n\"pnl\":" + std::to_string(pnl) +
            ",\"extra\":" + jStr(extra) + ",\n\"state\":" + stateJson(G) + "}",
            "application/json"
        );
    });

    svr.Post("/clear",[](const httplib::Request& req,httplib::Response& res){
        addCors(res);
        
        // FIX: Check both casing variants to ensure cross-browser compatibility
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        G.qty.fill(0);
        res.set_content(stateJson(G),"application/json");
    });

    svr.Post("/step-window", [](const httplib::Request& req, httplib::Response& res) {
        addCors(res);
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        
        std::string msg = "Window expired.";
        std::string mtype = "info";
        
        // If the player didn't execute a trade, penalize them
        if (!G.executedThisWindow) {
            G.arbPts = std::max(0, G.arbPts - 1);
            msg = "Window expired! -1 pt penalty for inactivity.";
            mtype = "bad";
        }
        
        G.windowOpenMids = G.liveMids; // Store snapshot before stepping
        stepMids(G);                   // Step underlying fair values
        
        res.set_content(
            "{\"msg\":" + jStr(msg) + ",\n\"msgType\":" + jStr(mtype) +
            ",\"state\":" + stateJson(G) + "}", 
            "application/json"
        );
    });

    svr.Post("/start-window", [](const httplib::Request& req, httplib::Response& res) {
        addCors(res);
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        
        placeBossCards(G); // Repopulate the workspace matrix
        
        res.set_content(stateJson(G), "application/json");
    });

    svr.Post("/clear", [](const httplib::Request& req, httplib::Response& res) {
        addCors(res);
        std::string sessionId = req.get_header_value("X-User-Session");
        if(sessionId.empty()) sessionId = req.get_header_value("x-user-session");
        if(sessionId.empty()) sessionId = "guest_default";
        
        GameState& G = getOrCreateSession(sessionId);
        G.qty.fill(0);
        
        res.set_content(stateJson(G), "application/json");
    });
}

/* ════════════════════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════════════════════*/

int main(){
    httplib::Server svr;
    setupRoutes(svr);
    
    // Render dynamic environment port allocation
    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;
    
    std::cout << "Multiplayer Arb server listening on port: " << port << "\n";
    svr.listen("0.0.0.0", port);
    return 0;
}