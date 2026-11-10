/* Redwood - a cryptocurrency consensus testbed.
 * Simulation core: no GUI dependencies. */
#ifndef REDWOOD_H
#define REDWOOD_H

#include <stdint.h>
#include "sha256.h"

#define MAX_NODES 16
#define MAX_TX_PER_BLOCK 12
#define MAX_BLOCKS 120000
#define MAX_TXS 400000
#define MP_MAX 2048
#define MAX_ORPH 64
#define WHEEL 64          /* message delivery wheel; max latency is WHEEL-1 ticks */
#define LOG_LINES 4000
#define LOG_LEN 220
#define HIST_CAP 4096
#define RETARGET_WINDOW 10

typedef struct Sim Sim;

typedef enum { COIN_POW, COIN_POA, COIN_POS, COIN_COUNT } CoinType;

/* Cheat behaviours (bit flags per node). */
enum {
    CH_SELFISH = 1,    /* withhold blocks, release strategically */
    CH_REWRITE = 2,    /* pay someone, then secretly rebuild history without the payment */
    CH_SPLIT = 4,      /* send conflicting payments to two halves of the network */
    CH_INFLATE = 8,    /* claim an oversized block reward */
    CH_FORGE = 16,     /* broadcast blocks with a fake proof of work / signature */
    CH_CENSOR = 32,    /* refuse to include a victim's transactions */
    CH_EQUIVOCATE = 64 /* produce two different blocks for one slot (PoA/PoS) */
};
#define CHEAT_COUNT 7

typedef enum {
    REJ_NONE, REJ_PARENT, REJ_HEIGHT, REJ_TIME, REJ_POW, REJ_TARGET,
    REJ_LEADER, REJ_SIG, REJ_REWARD, REJ_TX, REJ_TXROOT
} RejectReason;

typedef struct {
    int16_t from, to;
    int64_t amount;
    uint32_t nonce;
    int32_t tick;
    uint8_t id[32];
    uint8_t sig[32];
} Tx;

typedef struct {
    uint8_t hash[32], prev[32], txroot[32], sig[32];
    int32_t id, parent, height, time, slot;
    int16_t proposer;            /* claimed proposer (part of the header) */
    int16_t author;              /* node that actually created the block (simulation-only) */
    uint16_t ntx;
    int32_t txs[MAX_TX_PER_BLOCK];
    int64_t reward;
    uint64_t nonce, target;
    double weight;               /* cumulative work (PoW) or length (PoA/PoS) */
    uint8_t is_public;           /* has been broadcast */
    uint8_t validated;           /* 0 unknown, 1 valid, 2 invalid */
    uint8_t reason;              /* RejectReason when invalid */
    uint8_t logged, rlogged;
    int32_t created;
    int64_t bal[MAX_NODES];      /* ledger state after this block */
    uint32_t nonces[MAX_NODES];
} Block;

typedef struct {
    char name[24];
    /* configuration */
    int is_auth;                 /* PoA: trusted authority */
    int hashpower;               /* PoW: hashes per tick */
    int64_t init_balance;        /* genesis balance (also PoS stake) */
    double activity;             /* probability of sending a payment each tick */
    unsigned cheats;
    int victim;                  /* target of CH_CENSOR */
    /* runtime */
    uint8_t secret[32];
    int tip, priv_tip;
    uint8_t *known_blk;          /* 0 unknown, 1 accepted, 2 rejected */
    uint8_t *known_tx;
    int *mempool;
    int mp_n;
    int orphans[MAX_ORPH];
    int orph_n;
    int rw_phase, rw_p, rw_pp, rw_fork, rw_start, rw_cooldown;
    /* statistics */
    int mined, rejected, reorgs, max_reorg, released;
    int attacks_ok, attacks_fail, splits;
} Node;

typedef struct {
    CoinType coin;
    int interval;                /* target block time (PoW) or slot length (PoA/PoS), in ticks */
    int64_t reward;
    int latency, jitter;         /* message delay = latency + [0..jitter] ticks */
    uint64_t seed;
    double cheat_rate;           /* per-tick chance that an idle active cheater launches an attack */
} Config;

typedef struct { uint8_t type; int16_t to; int32_t id; } Msg;

struct Sim {
    Config cfg;
    int tick;
    int nnodes;
    Node nodes[MAX_NODES];
    Block *blocks;
    int nblocks;
    Tx *txs;
    int ntxs;
    Msg *wheel[WHEEL];
    int wn[WHEEL], wcap[WHEEL];
    uint64_t rng;
    char (*log)[LOG_LEN];
    int log_total;
    int reset_count;
    int64_t (*hist)[MAX_NODES];
    int *histt;
    int hist_n, hist_step;
    int reorg_epoch;
    int limit_hit;
};

Sim *sim_create(void);
void sim_free(Sim *s);
void sim_reset(Sim *s);                 /* new chain, same nodes and settings */
int sim_add_node(Sim *s);
void sim_remove_last_node(Sim *s);
void sim_step(Sim *s);                  /* advance one tick */
int sim_best_tip(Sim *s);               /* heaviest tip among all nodes */
void sim_log(Sim *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

const char *coin_name(CoinType c);
const char *coin_ticker(CoinType c);
const char *cheat_name(int idx);
const char *cheat_tip(int idx);
const char *reject_name(int r);
void hex_short(const uint8_t *h, char *out);   /* out >= 9 bytes  */
void hex_full(const uint8_t *h, char *out);    /* out >= 65 bytes */
void node_behaviour(const Node *n, char *out, size_t cap);

#endif
