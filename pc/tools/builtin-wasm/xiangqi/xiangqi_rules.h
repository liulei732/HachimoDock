#ifndef HACHI_XIANGQI_RULES_H
#define HACHI_XIANGQI_RULES_H

#include <stdint.h>

enum { RED, BLACK, KING, GUARD, ELEPHANT, HORSE, ROOK, CANNON, PAWN };
enum { SQUARES = 90, DEAD = 127 };
typedef struct { uint8_t board[SQUARES], pos[33]; } Position;
typedef struct { uint8_t from, to, capture; } Move;
typedef struct { int id, end, at, count; uint8_t targets[18]; } MoveScan;

static const uint8_t pieces[16] = {
    KING,GUARD,GUARD,ELEPHANT,ELEPHANT,HORSE,HORSE,ROOK,ROOK,CANNON,CANNON,PAWN,PAWN,PAWN,PAWN,PAWN
};
static const uint8_t initial_black[16] = {4,3,5,2,6,1,7,0,8,19,25,27,29,31,33,35};
static const int piece_value[] = {0,0,0,115,120,320,620,345,100};
static int file_of(int s) { return s%9; }
static int rank_of(int s) { return s/9; }
static int side_of(int id) { return id>16 ? BLACK : RED; }
static int type_of(int id) { return pieces[(id-1)&15]; }
static int general_of(int side) { return 1+16*side; }
static int abs_i(int n) { return n<0 ? -n : n; }
static int palace(int side, int x, int y) {
    return x>=3 && x<=5 && (side==RED ? y>=7 : y<=2);
}
static int crossed(int side,int rank) { return side==RED ? rank<=4 : rank>=5; }

static void clear_position(Position *p) {
    for(int i=0;i<SQUARES;++i) p->board[i]=0;
    for(int id=0;id<=32;++id) p->pos[id]=DEAD;
}
static void put(Position *p,int id,int square) {
    p->pos[id]=square;
    if(square!=DEAD) p->board[square]=id;
}
static void start_position(Position *p) {
    clear_position(p);
    for(int i=0;i<16;++i) {
        put(p,17+i,initial_black[i]);
        put(p,1+i,89-initial_black[i]);
    }
}
static int blockers(const Position *p,int from,int to,int step) {
    int count=0;
    for(int i=from+step;i!=to;i+=step)
        if(p->board[i] && ++count>1) break;
    return count;
}
static int pattern(const Position *p,int id,int from,int to) {
    if(from==to) return 0;
    int side=side_of(id), type=type_of(id);
    int x=file_of(from), y=rank_of(from), tx=file_of(to), ty=rank_of(to);
    int dx=tx-x, dy=ty-y, a=abs_i(dx), b=abs_i(dy);
    switch(type) {
    case KING:
        if(!dx && p->board[to]==general_of(side^1))
            return blockers(p,from,to,dy<0 ? -9 : 9)==0;
        return a+b==1 && palace(side,tx,ty);
    case GUARD:
        return a==1 && b==1 && palace(side,tx,ty);
    case ELEPHANT:
        return a==2 && b==2 && !crossed(side,ty) && !p->board[(from+to)/2];
    case HORSE:
        if(a==2 && b==1) return !p->board[from+dx/2];
        if(a==1 && b==2) return !p->board[from+(dy/2)*9];
        return 0;
    case PAWN:
        return (!dx && dy==(side==RED ? -1 : 1)) || (!dy && a==1 && crossed(side,y));
    case ROOK:
    case CANNON:
        if(dx && dy) return 0;
        return blockers(p,from,to,dx ? (dx<0 ? -1 : 1) : (dy<0 ? -9 : 9))
            == (type==CANNON && p->board[to] ? 1 : 0);
    default: return 0;
    }
}
static int checked(const Position *p,int side) {
    int square=p->pos[general_of(side)];
    for(int i=general_of(side^1);i<general_of(side^1)+16;++i)
        if(p->pos[i]!=DEAD && pattern(p,i,p->pos[i],square)) return 1;
    return 0;
}
static void apply_move(Position *p,Move m) {
    int id=p->board[m.from];
    p->board[m.from]=0;
    put(p,id,m.to);
    if(m.capture) p->pos[m.capture]=DEAD;
}
static void undo_move(Position *p,Move m) {
    put(p,p->board[m.to],m.from);
    p->board[m.to]=m.capture;
    if(m.capture) p->pos[m.capture]=m.to;
}
static int legal(Position *p,int from,int to) {
    if(from<0 || to<0 || from>=SQUARES || to>=SQUARES) return 0;
    int id=p->board[from], target=p->board[to];
    if(!id || (target && (side_of(id)==side_of(target) || type_of(target)==KING))
        || !pattern(p,id,from,to)) return 0;
    Move m={from,to,target};
    apply_move(p,m);
    int result=!checked(p,side_of(id));
    undo_move(p,m);
    return result;
}

static int collect_targets(const Position *p,int id,uint8_t *out) {
    static const int8_t vectors[][2] = {
        {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1},
        {2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}
    };
    int from=p->pos[id], type=type_of(id), side=side_of(id);
    int count=0, x=file_of(from), y=rank_of(from);
    if(type==ROOK || type==CANNON) {
        for(int i=0;i<4;++i) {
            int tx=x, ty=y, screen=0;
            for(;;) {
                tx+=vectors[i][0]; ty+=vectors[i][1];
                if(tx<0 || tx>=9 || ty<0 || ty>=10) break;
                int s=ty*9+tx, target=p->board[s];
                if(!target) {
                    if(!screen) out[count++]=s;
                } else if(type==CANNON && !screen) screen=1;
                else {
                    if(side_of(target)!=side && type_of(target)!=KING) out[count++]=s;
                    break;
                }
            }
        }
        return count;
    }
    int begin=0,end=4,scale=1;
    if(type==GUARD || type==ELEPHANT) { begin=4; end=8; scale=type==ELEPHANT ? 2 : 1; }
    if(type==HORSE) { begin=8; end=16; }
    for(int i=begin;i<end;++i) {
        int tx=x+vectors[i][0]*scale, ty=y+vectors[i][1]*scale;
        if(tx<0 || tx>=9 || ty<0 || ty>=10) continue;
        int s=ty*9+tx, target=p->board[s];
        if(target && (side_of(target)==side || type_of(target)==KING)) continue;
        if(pattern(p,id,from,s)) out[count++]=s;
    }
    return count;
}
static void begin_scan(MoveScan *scan,int side) {
    *scan=(MoveScan){.id=general_of(side)-1,.end=general_of(side)+16};
}
/* At most one piece expansion or one king-safety check per invocation. */
static int scan_step(Position *p,MoveScan *scan,Move *move) {
    if(scan->at==scan->count) {
        if(++scan->id>=scan->end) return -1;
        scan->at=scan->count=0;
        if(p->pos[scan->id]!=DEAD) scan->count=collect_targets(p,scan->id,scan->targets);
        return 0;
    }
    int from=p->pos[scan->id], to=scan->targets[scan->at++];
    if(!legal(p,from,to)) return 0;
    *move=(Move){from,to,p->board[to]};
    return 1;
}
static uint32_t position_hash(const Position *p,int side) {
    uint32_t hash=2166136261u;
    for(int s=0;s<SQUARES;++s) {
        int id=p->board[s];
        hash=(hash^(id ? type_of(id)+8*side_of(id) : 0))*16777619u;
    }
    return (hash^side)*16777619u;
}

#endif
