#ifndef SND_H
#define SND_H

/* XMB 風の UI 操作音 */
typedef enum {
    SND_MOVE = 0,   /* カーソル移動 (短いティック) */
    SND_OK,         /* 決定 (上昇する二音) */
    SND_CANCEL,     /* 戻る (低い単音) */
} SndEffect;

int snd_init(void);
void snd_shutdown(void);

/* 再生中でも音楽とミックスされて鳴る。連打時は後勝ち */
void snd_play(SndEffect e);

#endif
