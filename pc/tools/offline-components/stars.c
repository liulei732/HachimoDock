#include "screen.h"
#include "stars_art.h"

enum { COLS=8, ROWS=7, CELLS=COLS*ROWS, FREE=1, DAILY=2 };
static unsigned seed, removed[2], score, day, results[2], today;
static int grid[CELLS], colors[CELLS], group[CELLS], count;
static int x, y, menu, choice, game_exists, finished, confirm;
static unsigned mix(unsigned v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15;
    v *= 0x846ca68bu; return v ^ (v >> 16);
}

static int color(int pos) { return grid[pos] < 0 ? -1 : colors[grid[pos]]; }

/* Original IDs travel with their tiles. Gravity preserves order within a
 * column, and empty columns disappear; the seed and two masks reconstruct it. */
static void rebuild(void) {
    unsigned rng = seed;
    int column = 0;
    for (int i=0; i<CELLS; ++i) { grid[i]=-1; rng=rng*1664525u+1013904223u; colors[i]=(rng>>16)%5; }
    colors[1]=colors[0]; /* Every initial board has at least one legal move. */
    for (int col=0; col<COLS; ++col) {
        int height=0;
        for (int row=ROWS-1; row>=0; --row) {
            unsigned id=row*COLS+col;
            if ((removed[id/32] >> (id%32)) & 1u) continue;
            grid[(ROWS-1-height)*COLS+column]=id;
            ++height;
        }
        if (height) ++column;
    }
    finished=1;
    for (int i=0; i<CELLS; ++i) {
        int c=color(i);
        if (c>=0 && ((i%COLS && c==color(i-1)) || (i>=COLS && c==color(i-COLS)))) finished=0;
    }
}

static void select_group(void) {
    unsigned visited[2]={0,0};
    int start=y*COLS+x, target=color(start);
    count=0;
    if (target<0) return;
    group[count++]=start; visited[start/32] |= 1u << (start%32);
    for (int j=0; j<count; ++j) {
        int p=group[j], next[4]={p-1,p+1,p-COLS,p+COLS};
        for (int k=0; k<4; ++k) {
            int n=next[k];
            if ((k==0 && p%COLS==0) || (k==1 && p%COLS==COLS-1)) continue;
            if (n<0 || n>=CELLS || (visited[n/32] & (1u << (n%32))) || color(n)!=target) continue;
            visited[n/32] |= 1u << (n%32); group[count++]=n;
        }
    }
}

static unsigned signature(const unsigned *v) {
    unsigned h=0x53544131; /* layout version 1 */
    for (int i=0;i<7;++i) h=mix(h^v[i]);
    return h;
}

static void store(void) {
    unsigned v[7]={seed,removed[0],removed[1],score,day,results[0],results[1]};
    for (int i=0;i<7;++i) hachi_save(i,(int)v[i]);
    hachi_save(7,(int)signature(v));
}

static void remember_result(void) {
    if (!day) return;
    unsigned record=(day<<14)|score;
    for (int i=0; i<2; ++i) {
        if ((results[i]>>14)==day) {
            if (record>results[i]) results[i]=record;
            return;
        }
    }
    results[1]=results[0]; results[0]=record;
}

void hachi_init(void) {
    hachi_canvas640(); menu=1; choice=FREE;
    unsigned v[7];
    for (int i=0;i<7;++i) v[i]=(unsigned)hachi_load(i,0);
    if (signature(v)!=(unsigned)hachi_load(7,0) || !v[0] || v[2]>>24 || v[3]>5*CELLS*CELLS || v[4]>65535) return;
    seed=v[0]; removed[0]=v[1]; removed[1]=v[2]; score=v[3];
    day=v[4]; results[0]=v[5]; results[1]=v[6];
    game_exists=1; rebuild(); choice=finished ? FREE : 0;
}

static void new_game(unsigned challenge, unsigned new_seed) {
    day=challenge; seed=new_seed ? new_seed : 1;
    removed[0]=removed[1]=score=0; x=y=0;
    game_exists=1; menu=confirm=0;
    rebuild(); select_group(); store();
}

static void choose(void) {
    if (choice==0) {
        if (game_exists && !finished) { menu=0; select_group(); }
        return;
    }
    if (choice==DAILY && !today) return;
    if (game_exists && !finished && !confirm) { confirm=1; return; }
    if (choice==DAILY) new_game(today,mix(today^0x534b59u));
    else new_game(0,hachi_random_u32());
}

static void pop(void) {
    if (count<2 || finished) return;
    for (int j=0;j<count;++j) {
        unsigned id=grid[group[j]]; removed[id/32]|=1u<<(id%32);
    }
    score+=5*count*count; rebuild(); select_group();
    if (finished) remember_result();
    store();
}

void hachi_event(int type,int a,int b) {
    if (type==HACHI_TOUCH_UP) {
        if (menu) {
            if (a<24 || a>=616 || b<108 || b>=288) return;
            int row=(b-108)/60;
            if (row!=choice) confirm=0;
            choice=row; choose();
        } else if (a>=420 && b>=360) {
            menu=1; confirm=0; choice=finished ? FREE : 0;
        } else if (finished && a>=420 && b>=260 && b<354) {
            new_game(day,seed);
        } else if (!finished && a>=20 && a<20+48*COLS && b>=76 && b<76+48*ROWS) {
            int col=(a-20)/48,row=(b-76)/48;
            if (x==col && y==row) pop();
            else { x=col; y=row; select_group(); }
        }
    } else if (type==HACHI_KEY_DOWN) {
        if (menu) {
            if (a==HACHI_B) { confirm=0; if (game_exists && !finished) { menu=0;select_group(); } }
            else if (a==HACHI_A) choose();
            else if (a==HACHI_DOWN || a==HACHI_RIGHT) { choice=(choice+1)%3;confirm=0; }
            else if (a==HACHI_UP || a==HACHI_LEFT) { choice=(choice+2)%3;confirm=0; }
        } else if (a==HACHI_B) { menu=1; confirm=0;choice=finished ? FREE : 0; }
        else if (finished) { if (a==HACHI_A) new_game(day,seed); }
        else if (a==HACHI_A) pop();
        else {
            if (a==HACHI_LEFT) x=(x+COLS-1)%COLS;
            if (a==HACHI_RIGHT) x=(x+1)%COLS;
            if (a==HACHI_UP) y=(y+ROWS-1)%ROWS;
            if (a==HACHI_DOWN) y=(y+1)%ROWS;
            select_group();
        }
    }
}

void hachi_frame(int ms) {
    (void)ms;
    hachi_rect(0,0,640,480,0xf6efe3);
    if (menu) {
        hachi_snapshot_t clock;
        today=0;
        if (hachi_clock_read(HACHI_CLOCK_LOCAL,&clock)==HACHI_READY && clock.time_ms<5662224000000ULL
            && clock.value>=-840 && clock.value<=840) {
            int64_t local=(int64_t)clock.time_ms+(int64_t)clock.value*60000;
            if (local>=1577836800000LL) today=(unsigned)(local/86400000);
        }
        label(28,30,32,0x55405a,"消星星 · 一局");
        const char *items[]={game_exists && !finished ? "继续上局" : "没有待续棋局","自由新局",today ? "每日一局" : "每日一局 · 等待校时"};
        for (int i=0;i<3;++i) {
            if (choice==i) hachi_rect(20,108+60*i,600,54,0xe4d4b9);
            label(40,120+60*i,23,0x48394c,items[i]);
        }
        label(32,317,21,0x55405a,"昨日最佳");
        int found=0;
        for (int i=0;i<2;++i) if (today && (results[i]>>14)==today-1) { number(170,317,results[i]&16383);found=1; }
        if (!found) label(170,317,19,0x67615d,today ? "未完成" : "等待校时");
        label(32,414,20,0x48394c,confirm ? "再确认一次替换上局，副键取消" : "上下选择，主键确认");
        return;
    }
    label(20,20,28,0x48394c,day ? "每日一局" : "自由一局");
    hachi_rect(18+x*48,74+y*48,47,47,0x4a4843);
    for (int i=0;i<CELLS;++i) {
        int c=color(i);
        if (c>=0) hachi_sprite(20+(i%COLS)*48,76+(i/COLS)*48,42,42,star_art[c],star_size[c]);
    }
    label(432,100,23,0x48394c,"分数"); number(434,147,score);
    label(432,212,21,0x48394c,finished ? "结算" : "本组得分");
    if (!finished) number(434,248,count>=2 ? 5*count*count : 0);
    if (finished) label(432,287,20,0x48394c,"重玩此局");
    label(432,384,20,0x48394c,"返回选局");
    label(22,438,18,0x67615d,"方向键选星，主键消除，副键选局");
}
