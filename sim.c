/* Redwood simulation core.
 *
 * Model
 *  - Discrete ticks. Full-mesh gossip with per-message latency.
 *  - One global store of immutable blocks and transactions; every node keeps its
 *    own view (known blocks, mempool, best tip). Block validity is a pure
 *    function of the block and its parent, so it is computed once and cached.
 *  - Every block carries the full ledger state after it, so any fork can be
 *    validated and compared without replaying history.
 *  - "Signatures" are SHA-256(secret || message) checked through a simulation
 *    oracle; a node can only sign with its own secret, so it cannot forge
 *    another node's signature. This stands in for real public-key crypto.
 */
#include "redwood.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR_LEN 99
#define MSG_TX 0
#define MSG_BLOCK 1
#define REWRITE_CONF 3     /* confirmations the rewrite attacker waits for */
#define REWRITE_GIVEUP 8   /* give up when this many blocks behind */

/* ------------------------------------------------------------ allocation */

/* The simulation cannot continue after an allocation failure, so fail loudly
 * instead of dereferencing NULL. */
static void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p) {
        fprintf(stderr, "redwood: out of memory\n");
        abort();
    }
    return p;
}

static void *xrealloc(void *old, size_t size)
{
    void *p = realloc(old, size);
    if (!p) {
        fprintf(stderr, "redwood: out of memory\n");
        abort();
    }
    return p;
}

/* ------------------------------------------------------------------ names */

static const char *COIN_NAMES[COIN_COUNT] = {
    "Sequoia (SEQ) - Proof of Work",
    "Cedar (CDR) - Trusted Authorities",
    "Aspen (ASP) - Proof of Stake"};
static const char *COIN_TICKERS[COIN_COUNT] = {"SEQ", "CDR", "ASP"};

static const char *CHEAT_NAMES[CHEAT_COUNT] = {
    "Selfish mining", "History rewrite", "Split double-spend", "Inflate reward",
    "Forge proof", "Censor victim", "Equivocate"};

static const char *CHEAT_TIPS[CHEAT_COUNT] = {
    "Withhold found blocks on a private chain and release them when it is profitable.",
    "Pay a victim, wait for confirmations, then secretly build a longer fork that omits the payment.",
    "Send two conflicting payments (same nonce) to different halves of the network.",
    "Claim a block reward ten times the protocol amount.",
    "Broadcast blocks with a fake proof of work or a forged signature.",
    "Never include transactions sent by the victim node.",
    "Produce two different blocks for the same slot (PoA/PoS only)."};

static const char *REJECT_NAMES[] = {
    "none", "invalid parent", "bad height", "bad timestamp", "insufficient proof of work",
    "wrong difficulty", "not the slot leader", "bad signature", "wrong block reward",
    "invalid transaction", "bad transaction root"};

const char *coin_name(CoinType c) { return COIN_NAMES[c]; }
const char *coin_ticker(CoinType c) { return COIN_TICKERS[c]; }
const char *cheat_name(int i) { return CHEAT_NAMES[i]; }
const char *cheat_tip(int i) { return CHEAT_TIPS[i]; }
const char *reject_name(int r) { return REJECT_NAMES[r]; }

void hex_short(const uint8_t *h, char *out)
{
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[2 * i] = d[h[i] >> 4];
        out[2 * i + 1] = d[h[i] & 15];
    }
    out[8] = 0;
}

void hex_full(const uint8_t *h, char *out)
{
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = d[h[i] >> 4];
        out[2 * i + 1] = d[h[i] & 15];
    }
    out[64] = 0;
}

void node_behaviour(const Node *n, char *out, size_t cap)
{
    out[0] = 0;
    if (!n->cheats) {
        snprintf(out, cap, "Honest");
        return;
    }
    size_t len = 0;
    for (int i = 0; i < CHEAT_COUNT; i++)
        if (n->cheats & (1u << i))
            len += snprintf(out + len, cap - len, "%s%s", len ? ", " : "", CHEAT_NAMES[i]);
    if (n->attacks_ok || n->attacks_fail)
        snprintf(out + len, cap - len, "  [rewrites %d ok / %d failed]", n->attacks_ok, n->attacks_fail);
}

/* -------------------------------------------------------------- utilities */

static uint64_t rnd(Sim *s)
{
    uint64_t x = s->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    s->rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static double rndf(Sim *s) { return (double)(rnd(s) >> 11) / 9007199254740992.0; }
static int rndint(Sim *s, int n) { return n <= 0 ? 0 : (int)(rnd(s) % (uint64_t)n); }

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 | (uint64_t)p[2] << 40 | (uint64_t)p[3] << 32 |
           (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 | (uint64_t)p[6] << 8 | (uint64_t)p[7];
}

void sim_log(Sim *s, const char *fmt, ...)
{
    char *line = s->log[s->log_total % LOG_LINES];
    int n = snprintf(line, LOG_LEN, "[%6d] ", s->tick);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, LOG_LEN - n, fmt, ap);
    va_end(ap);
    s->log_total++;
}

static void sign_bytes(const Sim *s, int node, const uint8_t h[32], uint8_t out[32])
{
    uint8_t buf[64];
    memcpy(buf, s->nodes[node].secret, 32);
    memcpy(buf + 32, h, 32);
    sha256(buf, 64, out);
}

/* ----------------------------------------------------------- transactions */

static int tx_create(Sim *s, int from, int to, int64_t amount, uint32_t nonce)
{
    if (s->ntxs >= MAX_TXS) { s->limit_hit = 1; return -1; }
    Tx *t = &s->txs[s->ntxs];
    memset(t, 0, sizeof *t);
    t->from = (int16_t)from; t->to = (int16_t)to; t->amount = amount;
    t->nonce = nonce; t->tick = s->tick;
    uint8_t buf[16];
    buf[0] = (uint8_t)(from >> 8); buf[1] = (uint8_t)from;
    buf[2] = (uint8_t)(to >> 8);   buf[3] = (uint8_t)to;
    put64(buf + 4, (uint64_t)amount);
    put32(buf + 12, nonce);
    sha256(buf, 16, t->id);
    sign_bytes(s, from, t->id, t->sig);
    return s->ntxs++;
}

static int tx_sig_ok(const Sim *s, const Tx *t)
{
    uint8_t exp[32];
    sign_bytes(s, t->from, t->id, exp);
    return memcmp(exp, t->sig, 32) == 0;
}

static void compute_txroot(const Sim *s, const int32_t *ids, int n, uint8_t out[32])
{
    Sha256 c;
    sha256_init(&c);
    for (int i = 0; i < n; i++) sha256_update(&c, s->txs[ids[i]].id, 32);
    sha256_final(&c, out);
}

/* ---------------------------------------------------------------- network */

static int latency(Sim *s) { return s->cfg.latency + rndint(s, s->cfg.jitter + 1); }

static void send_msg(Sim *s, int type, int to, int id, int lat)
{
    if (lat < 1) lat = 1;
    if (lat > WHEEL - 1) lat = WHEEL - 1;
    int b = (s->tick + lat) % WHEEL;
    if (s->wn[b] == s->wcap[b]) {
        s->wcap[b] = s->wcap[b] ? s->wcap[b] * 2 : 256;
        s->wheel[b] = xrealloc(s->wheel[b], (size_t)s->wcap[b] * sizeof(Msg));
    }
    s->wheel[b][s->wn[b]++] = (Msg){(uint8_t)type, (int16_t)to, id};
}

/* subset: 0 = everyone, 1 = even-numbered nodes, 2 = odd-numbered nodes */
static int in_subset(int to, int subset) { return subset == 0 || (subset == 1) == (to % 2 == 0); }

static void broadcast_tx(Sim *s, int from, int id, int subset)
{
    for (int to = 0; to < s->nnodes; to++)
        if (to != from && in_subset(to, subset)) send_msg(s, MSG_TX, to, id, latency(s));
}

static void broadcast_block(Sim *s, int from, int id, int subset)
{
    for (int to = 0; to < s->nnodes; to++)
        if (to != from && in_subset(to, subset)) send_msg(s, MSG_BLOCK, to, id, latency(s));
}

static void mp_add(Node *n, int txid)
{
    if (n->mp_n >= MP_MAX) {
        memmove(n->mempool, n->mempool + 1, (MP_MAX - 1) * sizeof(int));
        n->mp_n--;
    }
    n->mempool[n->mp_n++] = txid;
}

static void node_receive_tx(Sim *s, int ni, int id)
{
    Node *n = &s->nodes[ni];
    if (n->known_tx[id]) return;
    n->known_tx[id] = 1;
    if (!tx_sig_ok(s, &s->txs[id])) return;
    mp_add(n, id);
}

/* -------------------------------------------------------- consensus rules */

static void header_bytes(const Block *b, CoinType c, uint8_t *buf)
{
    buf[0] = (uint8_t)c;
    put32(buf + 1, (uint32_t)b->height);
    put32(buf + 5, (uint32_t)b->time);
    buf[9] = (uint8_t)(b->proposer >> 8);
    buf[10] = (uint8_t)b->proposer;
    memcpy(buf + 11, b->prev, 32);
    memcpy(buf + 43, b->txroot, 32);
    put64(buf + 75, (uint64_t)b->reward);
    put64(buf + 83, b->target);
    put64(buf + 91, b->nonce);
}

static void block_hash(const Block *b, CoinType c, uint8_t out[32])
{
    uint8_t buf[HDR_LEN];
    header_bytes(b, c, buf);
    sha256(buf, HDR_LEN, out);
}

static int auth_count(const Sim *s)
{
    int c = 0;
    for (int i = 0; i < s->nnodes; i++) c += s->nodes[i].is_auth != 0;
    return c;
}

static int poa_leader(const Sim *s, int slot)
{
    int c = auth_count(s);
    if (!c) return -1;
    int k = slot % c;
    for (int i = 0; i < s->nnodes; i++)
        if (s->nodes[i].is_auth && k-- == 0) return i;
    return -1;
}

/* Stake-weighted lottery: the draw depends only on the parent block and slot. */
static int pos_leader(const Sim *s, const Block *p, int slot)
{
    uint8_t buf[36], h[32];
    memcpy(buf, p->hash, 32);
    put32(buf + 32, (uint32_t)slot);
    sha256(buf, 36, h);
    uint64_t total = 0;
    for (int i = 0; i < s->nnodes; i++) total += (uint64_t)(p->bal[i] > 0 ? p->bal[i] : 0);
    if (!total) return -1;
    uint64_t pick = get64(h) % total, acc = 0;
    for (int i = 0; i < s->nnodes; i++) {
        acc += (uint64_t)(p->bal[i] > 0 ? p->bal[i] : 0);
        if (pick < acc) return i;
    }
    return -1;
}

static int leader_for(const Sim *s, const Block *p, int slot)
{
    return s->cfg.coin == COIN_POA ? poa_leader(s, slot) : pos_leader(s, p, slot);
}

static uint64_t expected_target(const Sim *s, const Block *p)
{
    int h = p->height + 1;
    if (h % RETARGET_WINDOW != 0) return p->target;
    const Block *a = p;
    for (int i = 0; i < RETARGET_WINDOW - 1; i++) a = &s->blocks[a->parent];
    double actual = (double)(p->time - a->time);
    if (actual < 1) actual = 1;
    double expect = (double)(RETARGET_WINDOW - 1) * s->cfg.interval;
    double ratio = actual / expect;
    if (ratio < 0.25) ratio = 0.25;
    if (ratio > 4) ratio = 4;
    double nt = (double)p->target * ratio;
    if (nt >= 1.8446744073709552e19) return UINT64_MAX;
    if (nt < 1) return 1;
    return (uint64_t)nt;
}

static double block_work(const Sim *s, const Block *b)
{
    return s->cfg.coin == COIN_POW ? 18446744073709551616.0 / (double)(b->target ? b->target : 1) : 1.0;
}

static RejectReason check_block(Sim *s, Block *b)
{
    Block *p = &s->blocks[b->parent];
    if (p->validated == 2) return REJ_PARENT;
    if (b->height != p->height + 1) return REJ_HEIGHT;
    if (b->time < p->time || b->time > s->tick) return REJ_TIME;
    if (b->proposer < 0 || b->proposer >= s->nnodes) return REJ_LEADER;
    if (b->reward != s->cfg.reward) return REJ_REWARD;

    if (s->cfg.coin == COIN_POW) {
        if (b->target != expected_target(s, p)) return REJ_TARGET;
        if (get64(b->hash) >= b->target) return REJ_POW;
    } else {
        int slot = b->time / s->cfg.interval;
        if (slot <= p->slot) return REJ_TIME;
        int ld = leader_for(s, p, slot);
        if (ld < 0 || b->proposer != ld) return REJ_LEADER;
        uint8_t exp[32];
        sign_bytes(s, b->proposer, b->hash, exp);
        if (memcmp(exp, b->sig, 32) != 0) return REJ_SIG;
    }

    if (b->ntx > MAX_TX_PER_BLOCK) return REJ_TX;
    int64_t bal[MAX_NODES];
    uint32_t nn[MAX_NODES];
    memcpy(bal, p->bal, sizeof bal);
    memcpy(nn, p->nonces, sizeof nn);
    for (int i = 0; i < b->ntx; i++) {
        if (b->txs[i] < 0 || b->txs[i] >= s->ntxs) return REJ_TX;
        const Tx *t = &s->txs[b->txs[i]];
        if (t->from < 0 || t->from >= s->nnodes || t->to < 0 || t->to >= s->nnodes) return REJ_TX;
        if (!tx_sig_ok(s, t) || t->nonce != nn[t->from] || t->amount <= 0 || bal[t->from] < t->amount)
            return REJ_TX;
        bal[t->from] -= t->amount;
        bal[t->to] += t->amount;
        nn[t->from]++;
    }
    uint8_t root[32];
    compute_txroot(s, b->txs, b->ntx, root);
    if (memcmp(root, b->txroot, 32) != 0) return REJ_TXROOT;

    bal[b->proposer] += b->reward;
    memcpy(b->bal, bal, sizeof bal);
    memcpy(b->nonces, nn, sizeof nn);
    b->weight = p->weight + block_work(s, b);
    return REJ_NONE;
}

/* Validity is global and cached; ancestors are validated first. */
static int validate_block(Sim *s, int id)
{
    static int stack[MAX_BLOCKS];
    if (s->blocks[id].validated) return s->blocks[id].validated;
    int sp = 0;
    for (int c = id; c >= 0 && s->blocks[c].validated == 0; c = s->blocks[c].parent) stack[sp++] = c;
    while (sp > 0) {
        Block *b = &s->blocks[stack[--sp]];
        RejectReason r = check_block(s, b);
        b->reason = (uint8_t)r;
        b->validated = r == REJ_NONE ? 1 : 2;
    }
    return s->blocks[id].validated;
}

static int add_block(Sim *s, Block *tmpl)
{
    if (s->nblocks >= MAX_BLOCKS) { s->limit_hit = 1; return -1; }
    tmpl->id = s->nblocks;
    tmpl->created = s->tick;
    tmpl->slot = tmpl->time / s->cfg.interval;
    block_hash(tmpl, s->cfg.coin, tmpl->hash);
    s->blocks[s->nblocks] = *tmpl;
    return s->nblocks++;
}

/* Choose transactions from the node's mempool that apply cleanly on top of `parent`. */
static int try_add_tx(const Sim *s, int tid, int64_t *bal, uint32_t *nn, Block *b)
{
    const Tx *t = &s->txs[tid];
    if (b->ntx >= MAX_TX_PER_BLOCK) return 0;
    if (t->nonce != nn[t->from] || t->amount <= 0 || bal[t->from] < t->amount) return 0;
    bal[t->from] -= t->amount;
    bal[t->to] += t->amount;
    nn[t->from]++;
    b->txs[b->ntx++] = tid;
    return 1;
}

static void make_template(Sim *s, int ni, int parent, int time, int first_tx, Block *b, int variant)
{
    Node *nd = &s->nodes[ni];
    Block *p = &s->blocks[parent];
    memset(b, 0, sizeof *b);
    b->parent = parent;
    b->height = p->height + 1;
    memcpy(b->prev, p->hash, 32);
    b->time = time;
    b->slot = time / s->cfg.interval;
    b->proposer = (int16_t)ni;
    b->author = (int16_t)ni;
    b->reward = s->cfg.reward * ((nd->cheats & CH_INFLATE) ? 10 : 1);
    int64_t bal[MAX_NODES];
    uint32_t nn[MAX_NODES];
    memcpy(bal, p->bal, sizeof bal);
    memcpy(nn, p->nonces, sizeof nn);
    if (first_tx >= 0) try_add_tx(s, first_tx, bal, nn, b);
    for (int k = 0; k < nd->mp_n && b->ntx < MAX_TX_PER_BLOCK; k++) {
        int tid = nd->mempool[variant ? nd->mp_n - 1 - k : k];
        const Tx *t = &s->txs[tid];
        if ((nd->cheats & CH_CENSOR) && t->from == nd->victim) continue;
        try_add_tx(s, tid, bal, nn, b);
    }
    compute_txroot(s, b->txs, b->ntx, b->txroot);
}

/* ------------------------------------------------------- node block logic */

static int publish_chain(Sim *s, int ni, int upto, int limit);
static void node_receive_block(Sim *s, int ni, int id);

static int reorg_depth(const Sim *s, int a, int b)
{
    int d = 0;
    while (a != b) {
        int ha = s->blocks[a].height, hb = s->blocks[b].height;
        if (ha > hb) { a = s->blocks[a].parent; d++; }
        else if (hb > ha) b = s->blocks[b].parent;
        else { a = s->blocks[a].parent; b = s->blocks[b].parent; d++; }
    }
    return d;
}

static void mempool_prune(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    int a = n->tip;
    for (int i = 0; i < 12 && s->blocks[a].parent >= 0; i++) a = s->blocks[a].parent;
    int w = 0;
    for (int k = 0; k < n->mp_n; k++) {
        const Tx *t = &s->txs[n->mempool[k]];
        if (t->nonce < s->blocks[a].nonces[t->from]) continue;
        n->mempool[w++] = n->mempool[k];
    }
    n->mp_n = w;
}

static int mining_base(const Node *n) { return n->priv_tip >= 0 ? n->priv_tip : n->tip; }

/* Selfish-mining release policy, run when an honest block moves the public tip. */
static void selfish_policy(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    if (n->rw_phase == 1 || n->priv_tip < 0 || !(n->cheats & CH_SELFISH)) return;
    Block *pt = &s->blocks[n->priv_tip], *tp = &s->blocks[n->tip];
    if (pt->is_public) { n->priv_tip = -1; return; }
    int lead = pt->height - tp->height;
    if (lead < 0) {
        sim_log(s, "%s abandons its private chain (honest chain overtook it)", n->name);
        n->priv_tip = -1;
    } else if (lead <= 1) {
        int c = publish_chain(s, ni, n->priv_tip, 0);
        sim_log(s, "%s releases %d withheld block(s) to override the public chain", n->name, c);
    } else {
        publish_chain(s, ni, n->priv_tip, 1);
    }
}

static void consider_tip(Sim *s, int ni, int id)
{
    Node *n = &s->nodes[ni];
    Block *b = &s->blocks[id];
    if (!b->is_public || b->weight <= s->blocks[n->tip].weight) return;
    int old = n->tip;
    n->tip = id;
    int depth = reorg_depth(s, old, id);
    if (depth > 0) {
        n->reorgs++;
        if (depth > n->max_reorg) n->max_reorg = depth;
        s->reorg_epoch++;
        if (depth >= 2 && !b->rlogged) {
            b->rlogged = 1;
            sim_log(s, "REORG: %s dropped %d block(s) and switched to the chain ending at height %d (%s)",
                    n->name, depth, b->height, s->nodes[b->proposer].name);
        }
    }
    mempool_prune(s, ni);
    if (b->proposer != ni) selfish_policy(s, ni);
}

static int publish_chain(Sim *s, int ni, int upto, int limit)
{
    Node *n = &s->nodes[ni];
    int path[512], np = 0;
    for (int c = upto; c >= 0 && !s->blocks[c].is_public && np < 512; c = s->blocks[c].parent) path[np++] = c;
    int count = 0;
    for (int k = np - 1; k >= 0; k--) {
        if (limit && count >= limit) break;
        int id = path[k];
        s->blocks[id].is_public = 1;
        broadcast_block(s, ni, id, 0);
        consider_tip(s, ni, id);
        count++;
    }
    n->released += count;
    if (count == np) n->priv_tip = -1;
    return count;
}

static void process_orphans(Sim *s, int ni, int parent)
{
    Node *n = &s->nodes[ni];
    int kids[MAX_ORPH], nk = 0;
    for (int i = 0; i < n->orph_n;) {
        if (s->blocks[n->orphans[i]].parent == parent) {
            kids[nk++] = n->orphans[i];
            n->orphans[i] = n->orphans[--n->orph_n];
        } else i++;
    }
    for (int i = 0; i < nk; i++) node_receive_block(s, ni, kids[i]);
}

static void node_receive_block(Sim *s, int ni, int id)
{
    Node *n = &s->nodes[ni];
    if (n->known_blk[id]) return;
    Block *b = &s->blocks[id];
    if (!n->known_blk[b->parent]) {
        for (int i = 0; i < n->orph_n; i++)
            if (n->orphans[i] == id) return;
        if (n->orph_n < MAX_ORPH) n->orphans[n->orph_n++] = id;
        if (s->blocks[b->parent].is_public) send_msg(s, MSG_BLOCK, ni, b->parent, 2 * latency(s));
        return;
    }
    if (validate_block(s, id) == 2) {
        n->known_blk[id] = 2;
        n->rejected++;
        if (!b->logged) {
            char hs[9];
            hex_short(b->hash, hs);
            b->logged = 1;
            if (b->author != b->proposer)
                sim_log(s, "REJECTED block %s (height %d) forged by %s in the name of %s: %s", hs, b->height,
                        s->nodes[b->author].name, s->nodes[b->proposer].name, reject_name(b->reason));
            else
                sim_log(s, "REJECTED block %s (height %d) from %s: %s", hs, b->height,
                        s->nodes[b->proposer].name, reject_name(b->reason));
        }
        process_orphans(s, ni, id);
        return;
    }
    n->known_blk[id] = 1;
    consider_tip(s, ni, id);
    process_orphans(s, ni, id);
}

/* A node has produced a block. It is either withheld or broadcast. */
static int node_found_block(Sim *s, int ni, Block *tmpl, int subset, int self_accept, int allow_private)
{
    Node *n = &s->nodes[ni];
    int id = add_block(s, tmpl);
    if (id < 0) return -1;
    Block *b = &s->blocks[id];
    if (s->cfg.coin != COIN_POW) sign_bytes(s, ni, b->hash, b->sig);
    n->mined++;
    int priv = allow_private && b->reward == s->cfg.reward &&
               (n->rw_phase == 1 || (n->cheats & CH_SELFISH));
    if (priv) {
        b->is_public = 0;
        validate_block(s, id);
        n->known_blk[id] = 1;
        if (n->rw_phase == 0 && s->blocks[b->parent].is_public)
            sim_log(s, "%s begins withholding blocks (private chain at height %d)", n->name, b->height);
        n->priv_tip = id;
        return id;
    }
    b->is_public = 1;
    broadcast_block(s, ni, id, subset);
    if (self_accept) node_receive_block(s, ni, id);
    return id;
}

/* A block that fails validation, broadcast to everyone but its author. */
static void forge_block(Sim *s, int ni, int claimed)
{
    Node *n = &s->nodes[ni];
    Block b;
    make_template(s, ni, n->tip, s->tick, -1, &b, 0);
    b.proposer = (int16_t)claimed;
    if (s->cfg.coin == COIN_POW) {
        b.target = expected_target(s, &s->blocks[n->tip]);
        b.nonce = rnd(s);
    }
    int id = add_block(s, &b);
    if (id < 0) return;
    if (s->cfg.coin != COIN_POW) sign_bytes(s, ni, s->blocks[id].hash, s->blocks[id].sig);
    s->blocks[id].is_public = 1;
    n->mined++;
    broadcast_block(s, ni, id, 0);
}

/* ------------------------------------------------------- honest behaviour */

static void pending_info(const Sim *s, int ni, int *cnt, int64_t *outflow)
{
    const Node *n = &s->nodes[ni];
    const Block *tp = &s->blocks[n->tip];
    *cnt = 0;
    *outflow = 0;
    for (int k = 0; k < n->mp_n; k++) {
        const Tx *t = &s->txs[n->mempool[k]];
        if (t->from == ni && t->nonce >= tp->nonces[ni]) { (*cnt)++; *outflow += t->amount; }
    }
}

static void gen_tx(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    Block *tp = &s->blocks[n->tip];
    int cnt;
    int64_t out;
    pending_info(s, ni, &cnt, &out);
    int64_t avail = tp->bal[ni] - out;
    if (avail < 1) return;
    int to = rndint(s, s->nnodes - 1);
    if (to >= ni) to++;
    int64_t cap = avail < 25 ? avail : 25;
    int64_t amt = 1 + (int64_t)rndint(s, (int)cap);
    int id = tx_create(s, ni, to, amt, tp->nonces[ni] + (uint32_t)cnt);
    if (id < 0) return;
    n->known_tx[id] = 1;
    mp_add(n, id);
    broadcast_tx(s, ni, id, 0);
}

/* ------------------------------------------------------------ cheating */

static void split_spend(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    Block *tp = &s->blocks[n->tip];
    int cnt;
    int64_t out;
    pending_info(s, ni, &cnt, &out);
    int64_t avail = tp->bal[ni] - out;
    if (avail < 4 || s->nnodes < 3) return;
    int a = rndint(s, s->nnodes - 1);
    if (a >= ni) a++;
    int b;
    do { b = rndint(s, s->nnodes); } while (b == ni || b == a);
    int64_t amt = avail * 6 / 10;
    uint32_t nonce = tp->nonces[ni] + (uint32_t)cnt;
    int ta = tx_create(s, ni, a, amt, nonce), tb = tx_create(s, ni, b, amt, nonce);
    if (ta < 0 || tb < 0) return;
    n->known_tx[ta] = n->known_tx[tb] = 1;
    mp_add(n, ta);
    broadcast_tx(s, ni, ta, 1);
    broadcast_tx(s, ni, tb, 2);
    n->splits++;
    sim_log(s, "%s double-spends %lld coins (nonce %u): to %s for even nodes, to %s for odd nodes",
            n->name, (long long)amt, nonce, s->nodes[a].name, s->nodes[b].name);
}

static void rewrite_begin(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    Block *tp = &s->blocks[n->tip];
    int cnt;
    int64_t out;
    pending_info(s, ni, &cnt, &out);
    if (cnt > 0 || tp->bal[ni] < 20) return;
    int to = rndint(s, s->nnodes - 1);
    if (to >= ni) to++;
    int64_t amt = tp->bal[ni] / 5;
    uint32_t nonce = tp->nonces[ni];
    int p = tx_create(s, ni, to, amt, nonce), pp = tx_create(s, ni, ni, amt, nonce);
    if (p < 0 || pp < 0) return;
    n->known_tx[p] = n->known_tx[pp] = 1;
    mp_add(n, p);
    broadcast_tx(s, ni, p, 0);
    n->rw_phase = 1;
    n->rw_p = p;
    n->rw_pp = pp;
    n->rw_fork = n->tip;
    n->priv_tip = n->tip;
    n->rw_start = s->tick;
    sim_log(s, "%s pays %lld coins to %s and starts a secret fork from height %d that pays itself instead",
            n->name, (long long)amt, s->nodes[to].name, tp->height);
}

static void rewrite_check(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    if (n->rw_phase != 1) return;
    Block *pt = &s->blocks[n->priv_tip], *tp = &s->blocks[n->tip];
    int depth = 0, c = n->tip;
    for (int i = 0; i < 64 && c >= 0 && !depth; i++) {
        Block *cb = &s->blocks[c];
        for (int k = 0; k < cb->ntx; k++)
            if (cb->txs[k] == n->rw_p) depth = tp->height - cb->height + 1;
        c = cb->parent;
    }
    if (!pt->is_public && depth >= REWRITE_CONF && pt->weight > tp->weight) {
        int cnt = publish_chain(s, ni, n->priv_tip, 0);
        n->attacks_ok++;
        n->rw_phase = 0;
        n->rw_cooldown = 3 * s->cfg.interval;
        sim_log(s, "%s releases a secret chain of %d block(s): the %d-confirmation payment is erased",
                n->name, cnt, depth);
    } else if (tp->height - pt->height > REWRITE_GIVEUP || s->tick - n->rw_start > 80 * s->cfg.interval) {
        n->priv_tip = -1;
        n->rw_phase = 0;
        n->attacks_fail++;
        n->rw_cooldown = 3 * s->cfg.interval;
        sim_log(s, "%s abandons its rewrite attempt (honest chain is too far ahead)", n->name);
    }
}

/* ------------------------------------------------------------- producers */

static void pow_mine(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    if (n->hashpower <= 0) return;
    int base = mining_base(n);
    int first = (n->rw_phase == 1 && n->priv_tip == n->rw_fork) ? n->rw_pp : -1;
    Block b;
    make_template(s, ni, base, s->tick, first, &b, 0);
    b.target = expected_target(s, &s->blocks[base]);

    /* The 99-byte header spans two SHA-256 blocks and the nonce sits in the second,
     * so the first block's state is computed once and each attempt costs one compression. */
    uint8_t buf[HDR_LEN], blk[64];
    uint32_t mid[8], st[8];
    Sha256 ctx;
    header_bytes(&b, COIN_POW, buf);
    sha256_init(&ctx);
    sha256_compress(ctx.h, buf);
    memcpy(mid, ctx.h, sizeof mid);
    memset(blk, 0, sizeof blk);
    memcpy(blk, buf + 64, HDR_LEN - 64);
    blk[HDR_LEN - 64] = 0x80;
    put64(blk + 56, (uint64_t)HDR_LEN * 8);
    uint64_t start = rnd(s);
    for (int i = 0; i < n->hashpower; i++) {
        put64(blk + 91 - 64, start + (uint64_t)i);
        memcpy(st, mid, sizeof st);
        sha256_compress(st, blk);
        if ((((uint64_t)st[0] << 32) | st[1]) < b.target) {
            b.nonce = start + (uint64_t)i;
            node_found_block(s, ni, &b, 0, 1, 1);
            break;
        }
    }
}

/* PoA/PoS block production for a private fork: any past slot where we lead is usable. */
static void backdated_produce(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    int cur = s->tick / s->cfg.interval;
    Block *p = &s->blocks[n->priv_tip];
    int first = (n->priv_tip == n->rw_fork) ? n->rw_pp : -1;
    int lo = p->slot + 1;
    if (cur - lo > 400) lo = cur - 400;
    for (int slot = lo; slot <= cur; slot++) {
        if (leader_for(s, p, slot) != ni) continue;
        Block b;
        make_template(s, ni, n->priv_tip, slot * s->cfg.interval, first, &b, 0);
        node_found_block(s, ni, &b, 0, 1, 1);
        break;
    }
}

static void slot_produce(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    if (n->rw_phase == 1) { backdated_produce(s, ni); return; }
    int ival = s->cfg.interval;
    if (s->tick % ival) return;
    int slot = s->tick / ival;
    int base = mining_base(n);
    Block *p = &s->blocks[base];
    if (slot <= p->slot) return;
    int ld = leader_for(s, p, slot);
    if (ld < 0) return;
    if (ld == ni) {
        Block b;
        make_template(s, ni, base, s->tick, -1, &b, 0);
        if ((n->cheats & CH_EQUIVOCATE) && !(n->cheats & CH_SELFISH)) {
            Block b2;
            make_template(s, ni, base, s->tick, -1, &b2, 1);
            b.nonce = 0;
            b2.nonce = 1;
            node_found_block(s, ni, &b, 1, 1, 0);
            node_found_block(s, ni, &b2, 2, 0, 0);
            sim_log(s, "%s equivocates in slot %d: two different blocks sent to different halves", n->name, slot);
        } else {
            node_found_block(s, ni, &b, 0, 1, 1);
        }
    } else if (n->cheats & CH_FORGE) {
        forge_block(s, ni, ld);
    }
}

static void node_act(Sim *s, int ni)
{
    Node *n = &s->nodes[ni];
    if (n->rw_phase == 0 && rndf(s) < n->activity) gen_tx(s, ni);
    if ((n->cheats & CH_SPLIT) && rndf(s) < s->cfg.cheat_rate) split_spend(s, ni);

    if (n->cheats & CH_REWRITE) {
        if (n->rw_cooldown > 0) n->rw_cooldown--;
        else if (n->rw_phase == 0 && n->priv_tip < 0 && rndf(s) < s->cfg.cheat_rate) rewrite_begin(s, ni);
    } else if (n->rw_phase == 1) {
        n->priv_tip = -1;
        n->rw_phase = 0;
    }
    if (!(n->cheats & CH_SELFISH) && n->rw_phase == 0 && n->priv_tip >= 0)
        publish_chain(s, ni, n->priv_tip, 0);   /* cheat switched off: flush */

    if (s->cfg.coin == COIN_POW) {
        pow_mine(s, ni);
        if ((n->cheats & CH_FORGE) && s->tick % s->cfg.interval == 0) forge_block(s, ni, ni);
    } else {
        slot_produce(s, ni);
    }
    rewrite_check(s, ni);
}

/* ---------------------------------------------------------------- driver */

int sim_best_tip(Sim *s)
{
    int best = s->nodes[0].tip;
    for (int i = 1; i < s->nnodes; i++)
        if (s->blocks[s->nodes[i].tip].weight > s->blocks[best].weight) best = s->nodes[i].tip;
    return best;
}

static void record_history(Sim *s)
{
    if (s->tick % s->hist_step) return;
    if (s->hist_n >= HIST_CAP) {
        for (int i = 0; i < HIST_CAP / 2; i++) {
            memcpy(s->hist[i], s->hist[2 * i], sizeof s->hist[i]);
            s->histt[i] = s->histt[2 * i];
        }
        s->hist_n = HIST_CAP / 2;
        s->hist_step *= 2;
        if (s->tick % s->hist_step) return;
    }
    const Block *b = &s->blocks[sim_best_tip(s)];
    memcpy(s->hist[s->hist_n], b->bal, sizeof b->bal);
    s->histt[s->hist_n++] = s->tick;
}

void sim_step(Sim *s)
{
    if (s->limit_hit) return;
    s->tick++;
    int b = s->tick % WHEEL;
    int cnt = s->wn[b];
    Msg *w = s->wheel[b];
    s->wn[b] = 0;
    for (int i = 0; i < cnt; i++) {
        Msg m = w[i];
        if (m.type == MSG_TX) node_receive_tx(s, m.to, m.id);
        else node_receive_block(s, m.to, m.id);
    }
    int start = s->tick % s->nnodes;
    for (int k = 0; k < s->nnodes; k++) node_act(s, (start + k) % s->nnodes);
    record_history(s);
    if (s->limit_hit) sim_log(s, "Storage limit reached - simulation halted. Reset to start a new chain.");
}

static void make_genesis(Sim *s)
{
    Block *g = &s->blocks[0];
    memset(g, 0, sizeof *g);
    g->parent = -1;
    g->is_public = 1;
    g->validated = 1;
    g->reward = 0;
    if (s->cfg.coin == COIN_POW) {
        double hp = 0;
        for (int i = 0; i < s->nnodes; i++) hp += s->nodes[i].hashpower;
        if (hp < 1) hp = 1;
        double expect = hp * s->cfg.interval;
        g->target = expect <= 1 ? UINT64_MAX : (uint64_t)(18446744073709551615.0 / expect);
        if (g->target == 0) g->target = 1;
    }
    uint8_t buf[16];
    memcpy(buf, "redwood-genesis!", 16);
    buf[15] = (uint8_t)s->cfg.coin;
    sha256(buf, 16, g->hash);
    for (int i = 0; i < s->nnodes; i++) g->bal[i] = s->nodes[i].init_balance;
    s->nblocks = 1;
}

void sim_reset(Sim *s)
{
    s->tick = 0;
    s->ntxs = 0;
    s->limit_hit = 0;
    s->rng = s->cfg.seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
    if (!s->rng) s->rng = 88172645463325252ULL;
    for (int b = 0; b < WHEEL; b++) s->wn[b] = 0;
    s->log_total = 0;
    s->reset_count++;
    s->reorg_epoch++;
    s->hist_n = 0;
    s->hist_step = 4;
    make_genesis(s);
    for (int i = 0; i < s->nnodes; i++) {
        Node *n = &s->nodes[i];
        uint8_t buf[32];
        memcpy(buf, "redwood-key-----", 16);
        put64(buf + 16, s->cfg.seed);
        put32(buf + 24, (uint32_t)i);
        put32(buf + 28, 0x52454457u);
        sha256(buf, 32, n->secret);
        n->tip = 0;
        n->priv_tip = -1;
        memset(n->known_blk, 0, MAX_BLOCKS);
        memset(n->known_tx, 0, MAX_TXS);
        n->known_blk[0] = 1;
        n->mp_n = 0;
        n->orph_n = 0;
        n->rw_phase = n->rw_cooldown = 0;
        n->mined = n->rejected = n->reorgs = n->max_reorg = n->released = 0;
        n->attacks_ok = n->attacks_fail = n->splits = 0;
    }
    record_history(s);
    sim_log(s, "New %s chain, %d nodes, seed %llu", coin_name(s->cfg.coin), s->nnodes,
            (unsigned long long)s->cfg.seed);
    if (s->cfg.coin == COIN_POA && auth_count(s) == 0)
        sim_log(s, "WARNING: no node is marked as an authority; no blocks can be produced");
}

int sim_add_node(Sim *s)
{
    if (s->nnodes >= MAX_NODES) return -1;
    int i = s->nnodes++;
    Node *n = &s->nodes[i];
    memset(n, 0, sizeof *n);
    snprintf(n->name, sizeof n->name, "Node %d", i);
    n->hashpower = 200;
    n->init_balance = 1000;
    n->activity = 0.10;
    n->victim = i == 0 ? 1 : 0;
    n->priv_tip = -1;
    n->known_blk = xcalloc(MAX_BLOCKS, 1);
    n->known_tx = xcalloc(MAX_TXS, 1);
    n->mempool = xcalloc(MP_MAX, sizeof(int));
    return i;
}

void sim_remove_last_node(Sim *s)
{
    if (s->nnodes <= 3) return;
    Node *n = &s->nodes[--s->nnodes];
    free(n->known_blk);
    free(n->known_tx);
    free(n->mempool);
    memset(n, 0, sizeof *n);
    for (int i = 0; i < s->nnodes; i++)
        if (s->nodes[i].victim >= s->nnodes) s->nodes[i].victim = 0;
}

Sim *sim_create(void)
{
    Sim *s = xcalloc(1, sizeof *s);
    s->blocks = xcalloc(MAX_BLOCKS, sizeof(Block));
    s->txs = xcalloc(MAX_TXS, sizeof(Tx));
    s->log = xcalloc(LOG_LINES, LOG_LEN);
    s->hist = xcalloc(HIST_CAP, sizeof *s->hist);
    s->histt = xcalloc(HIST_CAP, sizeof(int));
    s->cfg.coin = COIN_POW;
    s->cfg.interval = 12;
    s->cfg.reward = 50;
    s->cfg.latency = 1;
    s->cfg.jitter = 2;
    s->cfg.seed = 1;
    s->cfg.cheat_rate = 0.02;
    for (int i = 0; i < 8; i++) sim_add_node(s);
    for (int i = 0; i < 3; i++) s->nodes[i].is_auth = 1;
    sim_reset(s);
    return s;
}

void sim_free(Sim *s)
{
    if (!s) return;
    while (s->nnodes > 0) {
        Node *n = &s->nodes[--s->nnodes];
        free(n->known_blk);
        free(n->known_tx);
        free(n->mempool);
    }
    for (int b = 0; b < WHEEL; b++) free(s->wheel[b]);
    free(s->blocks);
    free(s->txs);
    free(s->log);
    free(s->hist);
    free(s->histt);
    free(s);
}
