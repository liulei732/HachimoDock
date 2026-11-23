#include "hachi.h"
#include "xiangqi_rules.h"
#include "app_art.h"

enum { EASY, HARD, LOCAL };
enum { ACTIVE, MATE, BLOCKED, REPEAT, PERPETUAL, QUIET, RESIGNED };
enum { IDLE, VALIDATE, ROOT, REPLIES };
enum { BOARD_VIEW, MENU, CONFIRM_NEW, CONFIRM_RESIGN };
enum { MESSAGE_NONE, MESSAGE_HINT, MESSAGE_ILLEGAL, MESSAGE_EMPTY, MESSAGE_CONTINUE };
enum { HISTORY=256, CHOICES=9, LEFT_EDGE=19, TOP_EDGE=16, DX=24, DY=23 };

typedef struct { Move move; uint8_t quiet_before, gave_check; } Entry;
typedef struct { Move move; int score; } Choice;

static Position board, analysis;
static Entry history[HISTORY];
static uint32_t hashes[HISTORY];
static Move last, baseline_last, hint_move;
static uint8_t possible[SQUARES];
static int mode, turn, reason, loser, quiet_count, ply, base;
static int phase, active_check, selected, cursor, cursor_visible;
static int view, menu_row, pending_mode, confirm_yes, touch_x, touch_y;
static int sidebar_row = -1;
static int message, message_ms, has_hint;
static MoveScan scan;
static Choice choices[CHOICES];
static int choice_count, reply_index, reply_value, best_value, searching_hint;
static Move best_move;

#define ART(name,x,y) hachi_sprite((x),(y),name##_w,name##_h,art_##name,sizeof(art_##name))
#define HISTORY_INDEX(n) ((n)&(HISTORY-1))

static int sx(int square) { return LEFT_EDGE+file_of(square)*DX; }
static int sy(int square) { return TOP_EDGE+rank_of(square)*DY; }
static int computer_turn(void) { return mode!=LOCAL && turn==BLACK; }
static void status(int kind) { message=kind; message_ms=1850; }
static void clear_selection(void) {
    selected=-1;
    for(int i=0;i<SQUARES;++i) possible[i]=0;
}

static void save_match(void) {
    uint32_t words[7]={0};
    for(int id=1;id<=32;++id) {
        unsigned bit=(id-1)*7;
        uint32_t value=board.pos[id];
        words[bit/32] |= value<<(bit%32);
        if(bit%32>25) words[bit/32+1] |= value>>(32-bit%32);
    }
    for(int i=0;i<7;++i) hachi_save(i,(int)words[i]);
    uint32_t meta=(uint32_t)last.from | ((uint32_t)last.to<<7) |
        ((uint32_t)mode<<14) | ((uint32_t)turn<<16) | ((uint32_t)reason<<17) |
        ((uint32_t)loser<<20) | ((uint32_t)quiet_count<<21);
    uint32_t signature=8u | (position_hash(&board,turn)^meta)%8;
    hachi_save(7,(int)(meta | (signature<<28)));
}

static int restore_match(void) {
    uint32_t raw=(uint32_t)hachi_load(7,0);
    if(!(raw>>31)) return 0;
    uint32_t meta=raw&0x0fffffffu;
    uint32_t words[7];
    for(int i=0;i<7;++i) words[i]=(uint32_t)hachi_load(i,0);
    clear_position(&board);
    for(int id=1;id<=32;++id) {
        unsigned bit=(id-1)*7;
        uint32_t value=words[bit/32]>>(bit%32);
        if(bit%32>25) value|=words[bit/32+1]<<(32-bit%32);
        value &= 127;
        if(value==DEAD && type_of(id)!=KING) continue;
        if(value>=SQUARES || board.board[value]) return 0;
        if(type_of(id)==KING && !palace(side_of(id),file_of(value),rank_of(value))) return 0;
        put(&board,id,value);
    }
    turn=(meta>>16)&1;
    if((raw>>28)!=(8u | (position_hash(&board,turn)^meta)%8)) return 0;
    mode=(meta>>14)&3;
    reason=(meta>>17)&7;
    loser=(meta>>20)&1;
    quiet_count=(meta>>21)&127;
    last=(Move){meta&127,(meta>>7)&127,0};
    if(mode>LOCAL || reason>RESIGNED || quiet_count>120) return 0;
    if(last.from>=SQUARES || last.to>=SQUARES || !board.board[last.to]) last=(Move){DEAD,DEAD,0};
    baseline_last=last;
    ply=base=0;
    hashes[0]=position_hash(&board,turn);
    return 1;
}

static void inspect_turn(void) {
    active_check=checked(&board,turn);
    phase=reason ? IDLE : VALIDATE;
    begin_scan(&scan,turn);
}
static void new_match(int chosen) {
    start_position(&board);
    mode=chosen;
    turn=RED;
    reason=loser=quiet_count=ply=base=0;
    hashes[0]=position_hash(&board,turn);
    last=baseline_last=(Move){DEAD,DEAD,0};
    cursor=76;
    cursor_visible=has_hint=message_ms=0;
    clear_selection();
    inspect_turn();
    view=BOARD_VIEW;
}
static void complete(int why,int losing_side) {
    reason=why;
    loser=losing_side;
    phase=IDLE;
    has_hint=0;
    clear_selection();
    save_match();
    hachi_tone(330,120,38);
}
static void play_move(Move move) {
    Entry *entry=&history[HISTORY_INDEX(ply+1)];
    *entry=(Entry){.move=move,.quiet_before=quiet_count};
    apply_move(&board,move);
    last=move;
    quiet_count=move.capture ? 0 : quiet_count+1;
    turn^=1;
    ++ply;
    if(ply-base>=HISTORY) {
        ++base;
        baseline_last=history[HISTORY_INDEX(base)].move;
    }
    hashes[HISTORY_INDEX(ply)]=position_hash(&board,turn);
    has_hint=message_ms=0;
    clear_selection();
    inspect_turn();
    entry->gave_check=active_check;
    save_match();
    hachi_tone(move.capture ? 375 : 490,55,35);
}
static void undo_turn(void) {
    if(ply<=base) { status(MESSAGE_EMPTY); return; }
    do {
        Entry *entry=&history[HISTORY_INDEX(ply)];
        undo_move(&board,entry->move);
        quiet_count=entry->quiet_before;
        --ply;
        turn^=1;
    } while(mode!=LOCAL && turn==BLACK && ply>base);
    last=ply>base ? history[HISTORY_INDEX(ply)].move : baseline_last;
    reason=has_hint=message_ms=0;
    clear_selection();
    inspect_turn();
    save_match();
}
static int repetition(void) {
    int matches=0, start=ply;
    for(int i=ply-2;i>=base && ply-i<=quiet_count;i-=2) {
        if(hashes[HISTORY_INDEX(i)]==hashes[HISTORY_INDEX(ply)] && ++matches==2) {
            start=i;
            break;
        }
    }
    if(matches<2) return 0;
    int all[2]={1,1};
    for(int i=start+1;i<=ply;++i) {
        int mover=turn^1^((ply-i)&1);
        if(!history[HISTORY_INDEX(i)].gave_check) all[mover]=0;
    }
    if(all[RED]!=all[BLACK]) complete(PERPETUAL,all[BLACK] ? BLACK : RED);
    else complete(REPEAT,turn);
    return 1;
}

static int square_value(int kind,int side,int square) {
    int advance=side==RED ? 9-rank_of(square) : rank_of(square);
    int center=4-abs_i(file_of(square)-4);
    if(kind==PAWN) return advance*10+center*3;
    if(kind==HORSE) return center*7+(advance>4 ? 4 : advance)*5;
    if(kind==CANNON) return center*5+(advance>5 ? -7 : advance*3);
    if(kind==ROOK) return advance*4+center*2;
    return kind==KING ? -advance*5 : center;
}
static int move_value(const Position *p,Move move) {
    int id=p->board[move.from], kind=type_of(id), side=side_of(id);
    return (move.capture ? piece_value[type_of(move.capture)] : 0)
        +square_value(kind,side,move.to)-square_value(kind,side,move.from);
}
static void start_search(int for_hint) {
    searching_hint=for_hint;
    choice_count=0;
    phase=ROOT;
    has_hint=0;
    begin_scan(&scan,turn);
}
static void record_choice(Move move,int value) {
    int at=0;
    while(at<choice_count && choices[at].score>=value) ++at;
    if(at>=CHOICES) return;
    if(choice_count<CHOICES) ++choice_count;
    for(int j=choice_count-1;j>at;--j) choices[j]=choices[j-1];
    choices[at]=(Choice){move,value};
}
static void finish_search(Move move) {
    phase=IDLE;
    if(searching_hint) {
        hint_move=move;
        has_hint=1;
        status(MESSAGE_HINT);
    } else play_move(move);
}
static void start_reply(void) {
    for(int i=0;i<SQUARES;++i) analysis.board[i]=board.board[i];
    for(int i=0;i<=32;++i) analysis.pos[i]=board.pos[i];
    apply_move(&analysis,choices[reply_index].move);
    reply_value=-10000;
    begin_scan(&scan,turn^1);
}
static void advance_work(void) {
    for(int work=0;work<9 && phase!=IDLE;++work) {
        Move m;
        int current=phase;
        int ready=scan_step(current==REPLIES ? &analysis : &board,&scan,&m);
        if(!ready) continue;
        if(current==VALIDATE) {
            if(ready<0) { complete(active_check ? MATE : BLOCKED,turn); return; }
            if(repetition()) return;
            if(quiet_count>=120) { complete(QUIET,turn); return; }
            if(computer_turn()) start_search(0);
            else phase=IDLE;
            continue;
        }
        if(current==ROOT) {
            if(ready>0) {
                int value=move_value(&board,m);
                if(!searching_hint) {
                    apply_move(&board,m);
                    uint32_t hash=position_hash(&board,turn^1);
                    undo_move(&board,m);
                    for(int i=ply-1;i>=base && i>=ply-15;i-=2)
                        if(hashes[HISTORY_INDEX(i)]==hash) value-=42;
                    if(mode==EASY) value+=(int)((hash>>8)%17);
                }
                record_choice(m,value);
            } else if(!choice_count) {
                complete(active_check ? MATE : BLOCKED,turn);
                return;
            } else if(mode==EASY && !searching_hint) {
                finish_search(choices[0].move);
                return;
            } else {
                phase=REPLIES;
                reply_index=0;
                best_value=-20000;
                start_reply();
            }
            continue;
        }
        if(ready>0) {
            int value=move_value(&analysis,m);
            if(value>reply_value) reply_value=value;
        } else {
            int value=choices[reply_index].score-reply_value;
            if(value>best_value) { best_value=value; best_move=choices[reply_index].move; }
            if(++reply_index==choice_count) { finish_search(best_move); return; }
            start_reply();
        }
    }
}

static void pick_square(int square) {
    if(reason || computer_turn() || phase==VALIDATE) return;
    if(phase!=IDLE) phase=IDLE; /* Cancel a requested hint if the player moves. */
    has_hint=message_ms=0;
    int id=board.board[square];
    if(id && side_of(id)==turn) {
        int previous=selected;
        clear_selection();
        if(square==previous) return;
        selected=square;
        uint8_t targets[18];
        int count=collect_targets(&board,id,targets);
        for(int i=0;i<count;++i)
            if(legal(&board,square,targets[i])) possible[targets[i]]=1;
        hachi_tone(620,36,22);
    } else if(selected>=0) {
        if(possible[square]) play_move((Move){selected,square,id});
        else { status(MESSAGE_ILLEGAL); hachi_tone(208,48,22); }
    }
}
static void menu_action(int item) {
    if(item==3) { view=BOARD_VIEW; return; }
    if(item==4) {
        if(reason || computer_turn()) return;
        view=CONFIRM_RESIGN;
    } else if(!reason && last.from!=DEAD) {
        pending_mode=item;
        view=CONFIRM_NEW;
    } else {
        new_match(item);
        save_match();
    }
    confirm_yes=0;
}
static void confirm_action(int yes) {
    int prior=view;
    if(!yes) { view=MENU; return; }
    view=BOARD_VIEW;
    if(prior==CONFIRM_RESIGN) complete(RESIGNED,turn);
    else { new_match(pending_mode); save_match(); }
}
static void sidebar(int item) {
    if(item==0) undo_turn();
    if(item==1 && !reason && !computer_turn() && phase==IDLE) start_search(1);
    if(item==2) { view=MENU; menu_row=mode; }
}

void hachi_init(void) {
    touch_x=-1000;
    if(restore_match()) {
        view=BOARD_VIEW;
        cursor=last.to==DEAD ? 76 : last.to;
        clear_selection();
        inspect_turn();
        status(MESSAGE_CONTINUE);
    } else {
        new_match(EASY);
        view=MENU;
    }
}
void hachi_event(int event,int x,int y) {
    if(event==HACHI_KEY_DOWN) {
        touch_x=-1000;
        if(view==MENU) {
            if(x==HACHI_UP) menu_row=(menu_row+4)%5;
            if(x==HACHI_DOWN) menu_row=(menu_row+1)%5;
            if(x==HACHI_A) menu_action(menu_row);
            if(x==HACHI_B) view=BOARD_VIEW;
        } else if(view==CONFIRM_NEW || view==CONFIRM_RESIGN) {
            if(x==HACHI_LEFT || x==HACHI_RIGHT) confirm_yes^=1;
            if(x==HACHI_A) confirm_action(confirm_yes);
            if(x==HACHI_B) confirm_action(0);
        } else if(sidebar_row>=0) {
            if(x==HACHI_LEFT || x==HACHI_B) sidebar_row=-1;
            else if(x==HACHI_UP) sidebar_row=(sidebar_row+2)%3;
            else if(x==HACHI_DOWN) sidebar_row=(sidebar_row+1)%3;
            else if(x==HACHI_A) {
                int item=sidebar_row;
                sidebar_row=-1;
                sidebar(item);
            }
        } else if(x==HACHI_RIGHT && file_of(cursor)==8) {
            sidebar_row=0;
        } else if(x==HACHI_A) {
            cursor_visible=1;
            pick_square(cursor);
        } else if(x==HACHI_B) {
            if(selected>=0) clear_selection();
            else sidebar(2);
        } else {
            if(x==HACHI_LEFT && file_of(cursor)>0) --cursor;
            if(x==HACHI_RIGHT && file_of(cursor)<8) ++cursor;
            if(x==HACHI_UP && rank_of(cursor)>0) cursor-=9;
            if(x==HACHI_DOWN && rank_of(cursor)<9) cursor+=9;
            cursor_visible=1;
        }
        return;
    }
    if(event==HACHI_TOUCH_DOWN) { sidebar_row=-1; touch_x=x; touch_y=y; return; }
    if(event==HACHI_TOUCH_CANCEL) { touch_x=-1000; return; }
    if(event!=HACHI_TOUCH_UP) return;
    int valid=abs_i(touch_x-x)<=8 && abs_i(touch_y-y)<=8;
    touch_x=-1000;
    if(!valid) return;
    if(view==MENU) {
        if(x>=75 && x<245 && y>=95 && y<164) menu_action((y-95)/24);
        else if(x>=75 && x<245 && y>=169 && y<191) menu_action(x<160 ? 3 : 4);
    } else if(view==CONFIRM_NEW || view==CONFIRM_RESIGN) {
        if(x>=74 && x<246 && y>=135 && y<160) confirm_action(x>=167);
    } else if(x>=235 && x<=311 && y>=137 && y<224) {
        sidebar((y-137)/30);
    } else if(x>=7 && x<=222 && y>=5 && y<=234) {
        cursor=((y-TOP_EDGE+DY/2)/DY)*9+(x-LEFT_EDGE+DX/2)/DX;
        cursor_visible=0;
        pick_square(cursor);
    }
}

static void draw_piece(int id,int x,int y) {
    switch((type_of(id)-KING)+7*side_of(id)) {
    case 0: ART(piece_0,x,y); break; case 1: ART(piece_1,x,y); break;
    case 2: ART(piece_2,x,y); break; case 3: ART(piece_3,x,y); break;
    case 4: ART(piece_4,x,y); break; case 5: ART(piece_5,x,y); break;
    case 6: ART(piece_6,x,y); break; case 7: ART(piece_7,x,y); break;
    case 8: ART(piece_8,x,y); break; case 9: ART(piece_9,x,y); break;
    case 10: ART(piece_10,x,y); break; case 11: ART(piece_11,x,y); break;
    case 12: ART(piece_12,x,y); break; case 13: ART(piece_13,x,y); break;
    }
}
static void border(int x,int y,int w,int h,int color) {
    hachi_rect(x+3,y,w-6,1,color); hachi_rect(x+3,y+h-1,w-6,1,color);
    hachi_rect(x,y+3,1,h-6,color); hachi_rect(x+w-1,y+3,1,h-6,color);
}
static void mark(int square,int color) { border(sx(square)-11,sy(square)-11,23,23,color); }
static void draw(void) {
    ART(board,0,0);
    if(mode==LOCAL) ART(local,240,57);
    else if(mode==HARD) ART(hard,240,57);
    else ART(easy,240,57);
    if(reason==REPEAT || reason==QUIET) ART(drawn,240,89);
    else if(reason ? loser==BLACK : turn==RED) {
        if(reason) ART(red_win,240,89);
        else ART(red,240,89);
    } else {
        if(reason) ART(black_win,240,89);
        else ART(black,240,89);
    }
    if(reason==MATE) ART(mate,240,116);
    else if(reason==BLOCKED) ART(blocked,240,116);
    else if(reason==REPEAT) ART(repeat,240,116);
    else if(reason==QUIET) ART(quiet,240,116);
    else if(reason==PERPETUAL) ART(perpetual,240,116);
    else if(reason==RESIGNED) ART(resigned,240,116);
    else if(phase==ROOT || phase==REPLIES || computer_turn()) ART(thinking,240,116);
    else if(message_ms && message==MESSAGE_HINT) ART(hint,240,116);
    else if(message_ms && message==MESSAGE_ILLEGAL) ART(illegal,240,116);
    else if(message_ms && message==MESSAGE_EMPTY) ART(empty,240,116);
    else if(message_ms && message==MESSAGE_CONTINUE) ART(continued,240,116);
    else if(active_check) ART(check,240,116);
    else ART(move,240,116);
    for(int id=1;id<=32;++id)
        if(board.pos[id]!=DEAD) draw_piece(id,sx(board.pos[id])-11,sy(board.pos[id])-10);
    if(last.from!=DEAD) {
        mark(last.from,0x9a6d3d);
        mark(last.to,0x8c5c36);
    }
    if(active_check) mark(board.pos[general_of(turn)],0xc6442d);
    if(selected>=0) mark(selected,0x286856);
    if(sidebar_row>=0) border(235,137+30*sidebar_row,77,26,0xffe3a3);
    else if(cursor_visible) mark(cursor,0xffe3a3);
    for(int i=0;i<SQUARES;++i) {
        if(!possible[i]) continue;
        if(board.board[i]) mark(i,0x4b8b70);
        else {
            hachi_rect(sx(i)-3,sy(i)-2,7,5,0x71957c);
            hachi_rect(sx(i)-2,sy(i)-3,5,7,0x4a7964);
        }
    }
    if(has_hint) { mark(hint_move.from,0x397456); mark(hint_move.to,0x397456); }
    if(view==MENU) {
        ART(menu,61,42);
        if(menu_row<3) border(75,95+menu_row*24,170,21,0xffe3a3);
        else border(menu_row==3 ? 75 : 165,169,80,21,0xffe3a3);
    } else if(view==CONFIRM_NEW || view==CONFIRM_RESIGN) {
        ART(confirm_panel,61,68);
        if(view==CONFIRM_NEW) ART(new_confirm,80,77);
        else ART(lose_confirm,80,77);
        border(confirm_yes ? 167 : 74,135,79,25,0xffe3a3);
    }
}

void hachi_frame(int elapsed_ms) {
    if(message_ms>0) message_ms-=elapsed_ms;
    if(view==BOARD_VIEW) advance_work();
    draw();
}
