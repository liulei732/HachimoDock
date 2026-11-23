"""Build importable experiments with the repository SDK and the installed PetUI validator."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import zlib

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
PC = HERE.parents[1]
sys.path.insert(0, str(PC / "skills/petui/scripts"))
sys.path.insert(0, str(PC / "tools/builtin-wasm"))
from compile_wasm import compile_wasm
from app_art import encode
from focus import prepare as prepare_focus

SHELVES = {
    "book-pocket": ("书袋 · 离线精选", ["thinking-in-systems", "book-021", "book-061"]),
    "hachimi-learn": ("知识 · 离线精选", ["wiki-202247", "wiki-6427", "wiki-18", "wiki-83", "wiki-70"]),
}
BINDINGS = [("left","knob.rotate_ccw"),("right","knob.rotate_cw"),("up","joystick.up"),
            ("down","joystick.down"),("a","button.sw1.short_press"),("b","button.sw2.short_press")]


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":"))+"\n", encoding="utf-8")


def make_shelf(work, source, ids, title):
    content = json.loads((source/"pc/application.json").read_bytes())["content"]
    by_id = {a["id"]: a for a in content}
    if not 1 <= len(ids) <= 32 or len(set(ids)) != len(ids) or set(ids) - by_id.keys():
        raise ValueError("请选择 1 至 32 篇不重复、存在的文章 ID")
    keys = {key: zlib.crc32(key.encode()) & 0x7fffffff for key in by_id}
    if len(by_id) != len(content) or 0 in keys.values() or len(set(keys.values())) != len(keys):
        raise ValueError("文章 ID 或书签键冲突")
    entries, lines, notices = [], [], []
    for article_id in ids:
        a = by_id[article_id]
        key = keys[article_id]
        start = len(lines)
        for paragraph in a["text"].splitlines():
            lines.extend(textwrap.wrap(paragraph.strip(), width=27, break_on_hyphens=False))
        if len(lines) == start:
            raise ValueError("文章正文为空")
        name = a["title"]
        if len(name) > 24:
            name = name[:23] + "…"
        entries.append(f"{{{key}u,{json.dumps(name,ensure_ascii=False)},{start},{len(lines)-start}}}")
        notices.append("\n".join(str(a.get(k,"")) for k in ("title","id","source","url","revision","license")))
    strings = ",\n".join(json.dumps(s,ensure_ascii=False) for s in lines)
    (work/"shelf.h").write_text(f"#define ARTICLE_COUNT {len(entries)}\n#define SHELF_TITLE " +
        json.dumps(title,ensure_ascii=False) + "\nstatic const Article articles[]={" + ",\n".join(entries) +
        "};\nstatic const char *const lines[]={" + strings + "};\n", encoding="utf-8")
    return "\n\n".join(notices)+"\n"


def star_art(work):
    sprites, arrays = {}, []
    for i, color in enumerate(["#c65059","#da8835","#4f9779","#4971ab","#906aaf"]):
        im=Image.new("RGBA",(126,126))
        draw=ImageDraw.Draw(im)
        draw.rounded_rectangle((0,0,125,125),radius=20,fill=color)
        points=[]
        for k in range(10):
            angle=math.pi*(k/5-0.5)
            radius=48 if k%2==0 else 22
            points.append((63+radius*math.cos(angle),63+radius*math.sin(angle)))
        draw.polygon(points,fill="#fff3ce")
        for j in range(i+1):
            draw.ellipse((13+j*19,110,20+j*19,117),fill="#3f3543")

        _,_,encoded=encode(im,24)
        sprites[hashlib.sha256(encoded).hexdigest()]=im.resize((42,42),Image.Resampling.LANCZOS)
        arrays.append("static const uint8_t star%d[]={%s};" % (i,",".join(map(str,encoded))))
    names=[f"star{i}" for i in range(len(arrays))]
    (work/"stars_art.h").write_text("\n".join(arrays)+"\nstatic const uint8_t *const star_art[]={"+
        ",".join(names)+"};\nstatic const unsigned star_size[]={"+",".join("sizeof("+n+")" for n in names)+"};")
    return sprites


def cover(module, path, font_file, sprites):
    # Render the actual WASM draw list after opening the first article/free game.
    commands=json.loads(subprocess.check_output(["node",HERE/"capture.mjs",module]))
    im=Image.new("RGB",(640,480)); draw=ImageDraw.Draw(im)
    for cmd,*args in commands:
        if cmd=="rect":
            x,y,w,h,rgb=args
            draw.rectangle((x,y,x+w-1,y+h-1),fill=f"#{rgb:06x}")
        elif cmd=="sprite":
            x,y,index=args
            im.paste(sprites[index],(x,y),sprites[index])
        else:
            x,y,px,rgb,text=args
            draw.text((x,y),text,font=ImageFont.truetype(str(font_file),px),fill=f"#{rgb:06x}",anchor="lt")
    im.save(path)


def build(args):
    cli=[str(args.petui)] if not args.desktop else [str(args.desktop)]
    if args.desktop: cli.append("--petui")
    if args.component=="tomato-clock":
        if args.article: raise ValueError("番茄钟不使用文章列表")
        out=args.output.resolve()
        out.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=out) as tmp:
            work=Path(tmp)
            prepare_focus(PC/"builtin-clawpkgs/tomato-clock",work/"package",
                          args.portrait or PC/"public/terrier-clips/thumbs/done.jpg",args.version or "1.3.5")
            draft=work/"tomato-clock.hachiapp"
            subprocess.run([*cli,"pack",work/"package",draft],check=True)
            subprocess.run([*cli,"validate",draft],check=True)
            os.replace(draft,out/draft.name)
        print(out/"tomato-clock.hachiapp")
        return
    if args.portrait: raise ValueError("只有番茄钟使用宠物肖像")
    if not args.font: raise ValueError("请用 --font 指定生成预览封面所用的中文字体")
    shelf=args.component in SHELVES
    ident=args.component+("-offline" if shelf else "-daily")
    title=SHELVES[args.component][0] if shelf else "消星星 · 一局"
    description=("无需连接电脑即可选文、翻页。重新打开会回到上次阅读的位置。" if shelf else
        "消除相邻的同色星星得分，无组合时结算。可继续上局、重玩或挑战当天的棋盘。")
    labels=(["上一篇或上一页","下一篇或下一页","上一项","下一项","打开或翻页","返回目录"] if shelf else
            ["左移或上一项","右移或下一项","上移或上一项","下移或下一项","确认或消除","选局或取消"])
    output=args.output.resolve(); output.mkdir(parents=True,exist_ok=True)
    final=output/(ident+".hachiapp")
    with tempfile.TemporaryDirectory(dir=output) as tmp:
        work=Path(tmp); pkg=work/"package"
        for sub in ("pc","runtime","licenses"): (pkg/sub).mkdir(parents=True,exist_ok=True)
        source=PC/"builtin-clawpkgs"/args.component
        shutil.copy(HERE/"screen.h",work)
        sprites=[]
        if shelf:
            notes=make_shelf(work,source,args.article or SHELVES[args.component][1],title)
            (pkg/"licenses/selection.txt").write_text(notes,encoding="utf-8")
            for license_file in (source/"licenses").glob("*.txt"):
                shutil.copy(license_file,pkg/"licenses")
            for name in ("LICENSE.txt","THIRD_PARTY_NOTICES.txt"):
                if (source/name).exists(): shutil.copy(source/name,pkg/"licenses"/name)
        else:
            if args.article: raise ValueError("棋局不使用文章列表")
            sprites=star_art(work)
        code=Path(shutil.copy(HERE/("reader.c" if shelf else "stars.c"),work))
        module=pkg/"runtime/app.wasm"
        compile_wasm(code,PC/"skills/petui/sdk",args.clang,args.linker,work/"app.wasm")
        (work/"app.wasm").rename(module)
        data=module.read_bytes()
        if len(data)>65536: raise ValueError("精选内容超过 64 KiB，请减少文章")
        dash={"title":title,"headline":"离线目录" if shelf else "选择棋局","note":description,
              "visualStyle":"clean","visualLayout":"tool" if shelf else "arcade",
              "visualPalette":"forest" if shelf else "candy","visualSprite":"timer" if shelf else "star"}
        widget={"schema_version":1,"engine":"p4-bounded-runtime-v4","vars":{},"states":["active"],
                "initial_state":"active","transitions":[],"tick":[],"dashboard":dash,
                "hachiWasm":hashlib.sha256(data).hexdigest(),"hachiCanvas":"640x480-v1"}
        if shelf: widget["offlineReading"]="embedded-shelf-v1"
        else: widget["hachiHost"]="hachi-host-v1"
        write_json(pkg/"runtime/widget.json",widget)
        write_json(pkg/"component.json",{"id":ident,"name":title,"kind":"tool" if shelf else "game",
                   "version":args.version or "1.0.0","author":"PetUI","wasmTouch":True,"description":description})
        write_json(pkg/"buttons.json",[{"control":"前方摇杆" if i<4 else f"SW{i-3}","event":event,
                   "action":"lab."+key,"label":labels[i]} for i,(key,event) in enumerate(BINDINGS)])
        write_json(pkg/"negative-screen.json",{"dashboard":dash})
        write_json(pkg/"share.json",{"title":title,"summary":description})
        cover(module,pkg/"pc/cover.png",args.font,sprites)
        draft=work/final.name
        subprocess.run([*cli,"pack",pkg,draft],check=True)
        subprocess.run([*cli,"validate",draft],check=True)
        trace=work/"game.trace"
        if shelf:
            trace.write_text("frame 0\ntext "+title+"\nobserve\nevent 1 100 99\nevent 3 100 99\nframe 33\nappeared 目录\nevent 4 2 0\nframe 33\nevent 4 6 0\nframe 33\ntext "+title+"\n")
        else: shutil.copy(HERE/"stars.trace",trace)
        subprocess.run([*cli,"verify-wasm",draft,"--trace",trace],check=True)
        os.replace(draft,final)
    print(final)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component",choices=[*SHELVES,"cozy-pop-stars","tomato-clock"])
    parser.add_argument("output",type=Path)
    parser.add_argument("--article",action="append",help="Stable article ID; repeat to choose a shelf")
    parser.add_argument("--portrait",type=Path,help="Selected pet portrait for the focus greeting")
    parser.add_argument("--version",help="Increase when replacing a shelf")
    parser.add_argument("--font",type=Path,help="CJK font for the offline cover")
    parser.add_argument("--clang",default=os.environ.get("CLANG","clang"))
    parser.add_argument("--linker",default=os.environ.get("WASM_LD","wasm-ld"))
    parser.add_argument("--petui",type=Path,default=Path.home()/".claw-pet/authoring/petui")
    parser.add_argument("--desktop",type=Path,help="Use a development desktop executable as PetUI CLI")
    build(parser.parse_args())
