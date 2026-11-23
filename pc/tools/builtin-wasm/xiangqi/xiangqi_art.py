"""Supersampled ink, carved wood and seal artwork for the Xiangqi canvas."""
import random
from PIL import ImageDraw
from app_art import canvas, gradient, line, scale_box, text, S

CREAM = "#f6e5ba"
GOLD = "#e6c286"
MUTED = "#a6b8a8"


def oval(im, box, fill, outline=None):
    ImageDraw.Draw(im).ellipse(scale_box(box), fill, outline, S)


def button(im, x, y, w, h, label, primary=False):
    gradient(im, (x, y, w, h), 7, "#e2c084" if primary else "#344b42",
             "#bd9259" if primary else "#293e37", "#b49568" if primary else "#687c68")
    text(im, (x+w/2, y+h/2), label, 11, "#292a21" if primary else CREAM, center=True)


def board_art():
    im = canvas(320, 240)
    gradient(im, (0, 0, 320, 240), 0, "#253b36", "#172925")
    gradient(im, (4, 4, 222, 232), 12, "#8a5e3f", "#503b2e", "#ae8253")
    gradient(im, (8, 7, 214, 225), 9, "#e4be85", "#bc8c56", "#b2895b")
    rng = random.Random(27)
    for _ in range(118):
        x, y = rng.randrange(11,207), rng.randrange(9,230)
        w = rng.randrange(4, min(26, 218-x))
        line(im, (x,y,x+w,y), rng.choice(("#d3ab73","#d9b17a","#c99962","#dbb681")))
    # The outside files cross the river; the interior lines stop at both banks.
    for c in range(9):
        x = 19+c*24
        if c in (0,8):
            line(im,(x,16,x,223),"#75543b")
        else:
            line(im,(x,16,x,108),"#886240")
            line(im,(x,131,x,223),"#886240")
    for r in range(10):
        y=16+r*23
        line(im,(19,y,211,y),"#75543b" if r in (0,4,5,9) else "#886240")
    for y in (16,177):
        line(im,(91,y,139,y+46),"#896441")
        line(im,(139,y,91,y+46),"#896441")
    for r,columns in ((2,(1,7)),(3,(0,2,4,6,8)),(6,(0,2,4,6,8)),(7,(1,7))):
        for c in columns:
            x,y=19+c*24,16+r*23
            for a in (-1,1):
                if not 0<=c+a<=8:
                    continue
                for b in (-1,1):
                    line(im,(x+3*a,y+6*b,x+3*a,y+3*b,x+6*a,y+3*b),"#886240")
    text(im,(68,119),"楚 河",13,"#704e35",center=True)
    text(im,(161,119),"漢 界",13,"#704e35",center=True)
    for i in range(3):
        line(im,(96+i*4,119,100+i*4,116,104+i*4,119,108+i*4,116,112+i*4,119),"#a3784e")
    gradient(im,(232,5,82,230),11,"#334d43","#233a33","#607462")
    gradient(im,(240,13,13,14),3,"#b7654e","#8f4435","#d99166")
    text(im,(246.5,19.5),"弈",8,CREAM,center=True)
    text(im,(259,16),"墨间棋局",8,MUTED)
    text(im,(273,41),"中国象棋",16,CREAM,center=True)
    line(im,(240,51,306,51),"#617560")
    gradient(im,(240,56,66,19),6,"#485f4d","#394e43","#687b63")
    gradient(im,(239,82,68,30),8,"#293f37","#20342e","#627864")
    gradient(im,(239,116,68,17),5,"#394e42","#2c4139")
    button(im,239,139,68,24,"悔棋")
    button(im,239,169,68,24,"提示")
    button(im,239,199,68,24,"菜单")
    return im


def piece_art(glyph, red):
    im = canvas(23, 23)
    oval(im,(1,2,21,22),"#775437")
    oval(im,(1,0,21,20),"#e6bc82","#815833")
    oval(im,(2,1,20,19),"#f2d4a0","#d0a477")
    oval(im,(3,2,19,18),"#efd1a0","#bb8062" if red else "#9c896a")
    text(im,(11,10.5),glyph,13,"#8c352b" if red else "#29362d",center=True)
    return im


def make_art():
    art = {"board":board_art()}
    for side, glyphs in enumerate(("帥仕相傌俥炮兵","將士象馬車砲卒")):
        for i,glyph in enumerate(glyphs):
            art[f"piece_{side*7+i}"] = piece_art(glyph,side==0)
    for key, label, size, color in [
        ("local","双人同屏",10,CREAM),("easy","人机 · 入门",10,CREAM),("hard","人机 · 进阶",10,CREAM),
        ("red","红方行棋",12,"#ffbb9d"),("black","黑方行棋",12,"#e7dfc3"),
        ("red_win","红方胜",13,"#ffbb9d"),("black_win","黑方胜",13,CREAM),("drawn","和棋",13,CREAM),
        ("move","点棋子选落点",8,MUTED),("check","将军 · 请应将",8,"#ffc99b"),
        ("thinking","电脑思考中",8,GOLD),("hint","参考走法",8,GOLD),("illegal","此处不能走",8,"#ffc9a1"),
        ("mate","将死",9,GOLD),("blocked","困毙",9,GOLD),("repeat","重复局面",8,GOLD),
        ("perpetual","单方长将",8,GOLD),("quiet","无吃子和局",8,GOLD),
        ("resigned","认输",9,GOLD),("empty","暂无可悔步数",8,MUTED),("continued","已恢复棋局",8,GOLD),
    ]:
        im=canvas(65,16)
        text(im,(32,8),label,size,color,center=True)
        art[key]=im
    im=canvas(198,157)
    gradient(im,(0,0,198,157),14,"#435c4d","#233a32","#c8a574")
    text(im,(16,12),"对局设置",17,CREAM)
    text(im,(17,36),"选模式开新局，或返回当前对局",9,MUTED)
    button(im,14,53,170,21,"人机对弈 · 入门",True)
    button(im,14,77,170,21,"人机对弈 · 进阶")
    button(im,14,101,170,21,"双人同屏 · 轮流落子")
    button(im,14,127,80,21,"返回棋局")
    button(im,104,127,80,21,"认输")
    art["menu"]=im
    im=canvas(198,104)
    gradient(im,(0,0,198,104),13,"#435b4d","#283f36","#c8a574")
    button(im,13,67,79,25,"取消")
    button(im,106,67,79,25,"确认",True)
    art["confirm_panel"]=im
    for key,title,note in [
        ("new_confirm","开始新局？","确认后将替换当前棋局"),
        ("lose_confirm","确认认输？","当前行棋方将判负"),
    ]:
        im=canvas(160,49)
        text(im,(80,14),title,17,CREAM,center=True)
        text(im,(80,39),note,9,MUTED,center=True)
        art[key]=im
    return art
