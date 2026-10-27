// Economy fix: automatic supply of building inputs.
//
// Every game hour CMMTrade::RunEachHour runs CMMTrade::CalcBuilding for each
// building. A building's output (and its efficiency, min(2 * production,
// 10000) / 100 %) is driven by its scarcest input:
//   production = inputs * 0.2 * min(stock * price class weight), capped at 10000
// and only trade convoys bring inputs. The convoys pick random routes and
// random cargo, so inputs run dry, efficiency drops, clan income (the sum of
// its buildings' efficiency) collapses, no new gliders are bought, and the
// clans end up abandoning buildings (COrganization: too many pilots garrisoned
// for too few active gliders).
//
// Before the hourly run we move goods from producers' surplus to buildings
// whose inputs are low, up to the stock needed for full efficiency. Producers
// keep a reserve, so convoys and the player still find goods to trade.
#include "engine.h"
#include "../common/net.h"
#include "../common/lang.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma pack(push, 1)
typedef struct {
    char name[32];
    float stock;
    float unit;           // trade unit (stock / unit = units for sale)
    float consumes;       // > 0: input
    float produces;       // > 0: output
    int price_class;      // 0, 1, 2: weight 1, 2, 4
    uint8_t active;       // a good the building deals in (others decay)
    uint8_t pad[3];
} good;                   // 0x38
typedef struct {
    float production;
    char name[32];
    uint8_t flag;
    uint8_t pad[3];
    int ngoods;
    good *goods;
} trade_rec;              // 0x30
#pragma pack(pop)

_Static_assert(sizeof(good) == 0x38, "good");
_Static_assert(sizeof(trade_rec) == 0x30, "trade_rec");

#define TRADE_RECS   0x004      // CMMTrade: trade_rec[]
#define TRADE_NRECS  0xb44      // int
#define STOCK_CAP    40000.0f   // CalcBuilding caps produced stock here

static int g_on, g_log;
static float g_rate, g_reserve;
typedef void (THISCALL *run_fn)(void *);
static run_fn o_run_hour;

static const float k_weight[3] = {1.0f, 2.0f, 4.0f};

static uint8_t *trade(void) { return A_TRADE_PTR ? *(uint8_t **)A_TRADE_PTR : NULL; }

// the building object of a record, if it is in the current location (only those are simulated)
static uint8_t *building_of(const trade_rec *r)
{
    typedef uint8_t *(THISCALL *find_fn)(void *, name32, int);
    name32 n;
    memcpy(n.s, r->name, 32);
    return ((find_fn)A_OBJLIST_FIND)(G_OBJECT_LIST, n, 0xb);
}

static int building_type(const trade_rec *r)
{
    uint8_t *b = building_of(r);
    return b && *(uint8_t **)(b + 0xc) ? *(int *)(*(uint8_t **)(b + 0xc) + 0x154) : -1;
}

static int valid(const trade_rec *r)
{
    return *(const int32_t *)r->name != -1 && r->name[0] && r->goods && r->ngoods > 0 && r->ngoods < 64 && building_of(r);
}

static float weight(const good *g)
{
    return g->price_class >= 0 && g->price_class < 3 ? k_weight[g->price_class] : 1.0f;
}

static float efficiency(const trade_rec *r)
{
    float e = r->production * 2.0f;
    return (e > 10000.0f ? 10000.0f : e) / 100.0f;
}

static int g_moves;
static float g_moved;

static void log_efficiency(uint8_t *t)
{
    trade_rec *recs = (trade_rec *)(t + TRADE_RECS);
    int n = *(int *)(t + TRADE_NRECS);
    char buf[512];
    int len = 0, cnt = 0;
    float sum = 0;
    for (int i = 0; i < n && i < 60 && len < (int)sizeof buf - 24; i++)
        if (valid(&recs[i])) {
            len += snprintf(buf + len, sizeof buf - len, " %.0f", efficiency(&recs[i]));
            sum += efficiency(&recs[i]);
            cnt++;
        }
    mlog("economy: %d transfers (%.0f units); efficiency avg %.0f%%:%s", g_moves, g_moved, cnt ? sum / cnt : 0, buf);
}

static void supply(uint8_t *t)
{
    trade_rec *recs = (trade_rec *)(t + TRADE_RECS);
    int n = *(int *)(t + TRADE_NRECS);
    if (n <= 0 || n > 60) return;
    int moves = 0;
    float moved = 0;
    for (int i = 0; i < n; i++) {
        trade_rec *r = &recs[i];
        if (!valid(r)) continue;
        int nin = 0;
        for (int k = 0; k < r->ngoods; k++)
            if (r->goods[k].consumes > 0 && r->goods[k].active) nin++;
        if (!nin) continue;
        // full efficiency: nin * 0.2 * min(stock * weight) >= 5000; aim a bit higher
        float target_value = 1.25f * 5000.0f / (0.2f * nin);
        for (int k = 0; k < r->ngoods; k++) {
            good *g = &r->goods[k];
            if (g->consumes <= 0 || !g->active) continue;
            float target = target_value / weight(g);
            if (target > STOCK_CAP) target = STOCK_CAP;
            float need = target - g->stock;
            if (need <= 0) continue;
            // producers of this good, richest first
            for (int pass = 0; pass < n && need > 0; pass++) {
                good *best = NULL;
                float avail = 0;
                for (int j = 0; j < n; j++) {
                    trade_rec *p = &recs[j];
                    if (j == i || !valid(p)) continue;
                    for (int m = 0; m < p->ngoods; m++) {
                        good *pg = &p->goods[m];
                        if (pg->produces <= 0 || strncmp(pg->name, g->name, 32)) continue;
                        float a = (pg->stock - g_reserve / weight(pg)) * g_rate;   // keep a reserve worth g_reserve
                        if (a > avail) { avail = a; best = pg; }
                    }
                }
                if (!best || avail < 1.0f) break;
                float x = avail < need ? avail : need;
                best->stock -= x;
                g->stock += x;
                need -= x;
                moved += x;
                moves++;
            }
        }
    }
    g_moves = moves;
    g_moved = moved;
}

static void THISCALL h_run_hour(void *self)
{
    g_moves = 0;
    g_moved = 0;
    if (g_on && net_mode() != NET_CLIENT) supply((uint8_t *)self);   // the host's economy is the one that runs
    o_run_hour(self);
    if (g_log) log_efficiency((uint8_t *)self);
}

// harness: "econhour" runs one hourly economy step now
void economy_run_hour(void)
{
    uint8_t *t = trade();
    if (t) h_run_hour(t);
}

// harness: "econ" logs every building's goods
void economy_dump(void)
{
    uint8_t *t = trade();
    if (!t) { mlog("econ: no trade manager"); return; }
    trade_rec *recs = (trade_rec *)(t + TRADE_RECS);
    int n = *(int *)(t + TRADE_NRECS);
    mlog("econ: %d buildings (vtbl %p)", n, *(void **)t);
    for (int i = 0; i < n && i < 60; i++) {
        trade_rec *r = &recs[i];
        if (!valid(r)) continue;
        mlog("econ: %-24.32s type %2d production %7.0f efficiency %3.0f%%", r->name, building_type(r), r->production, efficiency(r));
        for (int k = 0; k < r->ngoods; k++) {
            good *g = &r->goods[k];
            mlog("econ:     %-24.32s stock %8.0f unit %5.0f in %6.2f out %6.2f class %d%s", g->name, g->stock, g->unit,
                 g->consumes, g->produces, g->price_class, g->active ? "" : " (foreign)");
        }
    }
}

int economy_auto_supply(void) { return g_on; }

void economy_set_auto_supply(int on)
{
    g_on = on ? 1 : 0;
    WritePrivateProfileStringA("Economy", "AutoSupply", g_on ? "1" : "0", g_ini);
    mlog("economy: auto supply %s", g_on ? "on" : "off");
}

void aim2_economy_install(void)
{
    g_on = GetPrivateProfileIntA("Economy", "AutoSupply", 1, g_ini);
    g_log = GetPrivateProfileIntA("Economy", "Log", 0, g_ini);
    char b[32];
    GetPrivateProfileStringA("Economy", "SupplyRate", "0.5", b, sizeof b, g_ini);
    g_rate = (float)atof(b);
    GetPrivateProfileStringA("Economy", "ProducerReserve", "2000", b, sizeof b, g_ini);
    g_reserve = (float)atof(b);
    if (g_rate <= 0 || g_rate > 1) g_rate = 0.5f;
    if (g_reserve < 0 || g_reserve > STOCK_CAP) g_reserve = 2000;
    void *f = (void *)h_run_hour;
    uintptr_t slot = VT_TRADE + VF_TRADE_RUNHOUR;
    o_run_hour = *(run_fn *)slot;
    uint32_t cur = (uint32_t)(uintptr_t)o_run_hour;
    if (patch_bytes(slot, &cur, &f, 4))
        mlog("economy: auto supply %s (rate %.2f, producer reserve %.0f)", g_on ? "on" : "off", g_rate, g_reserve);
}
