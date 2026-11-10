/* Headless Redwood runner, useful for scripted experiments and regression testing.
 *
 *   redwood-cli <coin 0|1|2> <ticks> [seed] [node:cheatmask[:hashpower[:balance]] ...]
 *
 * coin: 0 = Sequoia (PoW), 1 = Cedar (PoA), 2 = Aspen (PoS)
 * cheatmask bits: 1 selfish, 2 rewrite, 4 split, 8 inflate, 16 forge, 32 censor, 64 equivocate
 */
#include "redwood.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int coin = argc > 1 ? atoi(argv[1]) : 0;
    int ticks = argc > 2 ? atoi(argv[2]) : 5000;
    Sim *s = sim_create();
    s->cfg.coin = (CoinType)(coin % COIN_COUNT);
    s->cfg.interval = coin == 0 ? 12 : 6;
    if (argc > 3) s->cfg.seed = strtoull(argv[3], NULL, 10);
    for (int i = 4; i < argc; i++) {
        int node = 0, mask = 0, hp = -1, bal = -1;
        if (sscanf(argv[i], "%d:%d:%d:%d", &node, &mask, &hp, &bal) >= 2 && node >= 0 && node < s->nnodes) {
            s->nodes[node].cheats = (unsigned)mask;
            if (hp >= 0) s->nodes[node].hashpower = hp;
            if (bal >= 0) s->nodes[node].init_balance = bal;
        }
    }
    sim_reset(s);
    for (int t = 0; t < ticks && !s->limit_hit; t++) sim_step(s);

    int best = sim_best_tip(s);
    Block *b = &s->blocks[best];
    int64_t supply = 0, genesis = 0;
    for (int i = 0; i < s->nnodes; i++) { supply += b->bal[i]; genesis += s->nodes[i].init_balance; }
    printf("%s | ticks %d | blocks stored %d | txs %d\n", coin_name(s->cfg.coin), s->tick, s->nblocks, s->ntxs);
    printf("best height %d | supply %lld (expected %lld) %s\n", b->height, (long long)supply,
           (long long)(genesis + s->cfg.reward * b->height),
           supply == genesis + s->cfg.reward * b->height ? "OK" : "MISMATCH");
    int main_cnt[MAX_NODES] = {0};
    for (int c = best; c > 0; c = s->blocks[c].parent) main_cnt[s->blocks[c].proposer]++;
    for (int i = 0; i < s->nnodes; i++) {
        Node *n = &s->nodes[i];
        char beh[200];
        node_behaviour(n, beh, sizeof beh);
        printf("%-7s tip h=%-5d bal=%-7lld made=%-4d main=%-4d rejected=%-3d reorgs=%-3d maxdepth=%-2d | %s\n",
               n->name, s->blocks[n->tip].height, (long long)s->blocks[n->tip].bal[i], n->mined,
               main_cnt[i], n->rejected, n->reorgs, n->max_reorg, beh);
    }
    int from = s->log_total > 25 ? s->log_total - 25 : 0;
    printf("--- last log lines ---\n");
    for (int i = from; i < s->log_total; i++) printf("%s\n", s->log[i % LOG_LINES]);
    sim_free(s);
    return 0;
}
