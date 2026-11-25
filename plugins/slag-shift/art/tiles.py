"""Draw Slag Shift's native-resolution screens and pack the tile maps."""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT.parents[1]
GLYPHS = dict(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/-", [
    "0E111F1111","1E111E111E","0F1010100F","1E1111111E","1F101E101F",
    "1F101E1010","0F1013110F","11111F1111","1F0404041F","070202120C",
    "11121C1211","101010101F","111B151111","1119151311","0E1111110E",
    "1E111E1010","0E1111150F","1E111E1211","0F100E011E","1F04040404",
    "111111110E","1111110A04","1111151B11","110A040A11","110A040404",
    "1F0204081F","0E1315110E","040C04040E","1E010E101F","1E0106011E",
    "12121F0202","1F101E011E","0E101E110E","1F02040808","0E110E110E",
    "0E110F010E","0102040810","00001F0000"]))

def lettering(draw, x, y, words, ink, sx=1, sy=1, slant=False):
    for ch in words:
        if ch in GLYPHS:
            for row, bits in enumerate(bytes.fromhex(GLYPHS[ch])):
                skew = (4-row) if slant else 0
                for col in range(5):
                    if bits & (16 >> col):
                        a, b = x + col*sx + skew, y + row*sy
                        draw.rectangle((a,b,a+sx-1,b+sy-1), fill=ink)
        x += 6*sx

def stripes(d, x, y, w, h):
    for row in range(h):
        for col in range(w):
            if (row+col) % 8 < 4:
                d.point((x+col,y+row),fill=2)

def billet(d,x,y):
    d.rectangle((x,y,x+6,y+6),fill=1)
    d.rectangle((x,y,x+5,y+4),fill=2)
    d.line((x+1,y,x+4,y),fill=3)

title = Image.new('P',(160,144))
d = ImageDraw.Draw(title)
d.line((6,11,153,11),fill=1)
lettering(d,7,3,"KILN WORKS",3)
stripes(d,126,3,27,5)
# Falling hot metal at the right of the wordmark.
for x,y in [(134,14),(141,14),(141,21),(148,21),(120,38),(127,38),(134,38),(120,45)]:
    billet(d,x,y)
for x,y in [(137,28),(147,32),(126,51)]:
    d.line((x,y,x,y+4),fill=1)
for word,x,y,sx,sy in [('SLAG',7,18,5,4),('SHIFT',7,43,5,5)]:
    lettering(d,x+2,y+3,word,1,sx,sy,True)
    lettering(d,x,y,word,3 if word=='SLAG' else 2,sx,sy,True)
d.line((6,75,153,75),fill=1)
lettering(d,10,82,'A/B',2); lettering(d,62,82,'ROTATE',3)
lettering(d,10,92,'DOWN',2); lettering(d,62,92,'SOFT DROP',3)
lettering(d,10,102,'UP',2); lettering(d,62,102,'HARD DROP',3)
d.polygon([(7,118),(148,118),(153,123),(153,135),(12,135),(7,130)],fill=2)
d.polygon([(19,122),(19,131),(25,126)],fill=0)
lettering(d,46,124,'PRESS START',0)
# GB Studio's indexed monochrome import, darkest first in this source.
title.putpalette([7,24,33,48,104,80,134,192,108,224,248,207]+[0]*756)
title.save(GAME/'assets/backgrounds/slag_shift.png')

panel = Image.new('P',(160,144))
d = ImageDraw.Draw(panel)
for x in (0,88):
    d.rectangle((x,0,x+7,143),fill=1)
    d.line((x+4,0,x+4,143),fill=0)
    for y in range(3,144,16):
        d.rectangle((x+2,y,x+4,y+1),fill=2)
d.line((97,18,157,18),fill=1)
for y,word in [(24,'NEXT'),(66,'SCORE'),(90,'LINES'),(114,'LEVEL')]:
    lettering(d,99,y,word,3)
    end = 99+6*len(word)+2
    d.line((end,y+2,156,y+2),fill=1)
d.rectangle((98,32,156,59),outline=1)
stripes(d,100,137,56,3)

tiles,indices=[],{}
def register(im):
    key=im.tobytes()
    if key not in indices:
        indices[key]=len(tiles)
        packed=[]
        for y in range(8):
            row=key[y*8:y*8+8]
            for bit in range(2):
                packed.append(sum(((c>>bit)&1)<<(7-i) for i,c in enumerate(row)))
        tiles.append(packed)
    return indices[key]

def pattern(rows):
    im=Image.new('P',(8,8)); im.putdata([int(c) for row in rows for c in row]); return register(im)

empty=pattern(['00000000']*7+['00000001'])
metal=pattern(['03333320','32222221','32222221','32221221','32222221','32222221','22222211','01111110'])
guide=pattern(['00000000','01100110','01000010','00000000','00000000','01000010','01100110','00000000'])
flash=pattern(['33333333']*8)
blank=register(Image.new('P',(8,8)))
digits=[]
for ch in '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    im=Image.new('P',(8,8)); lettering(ImageDraw.Draw(im),1,1,ch,3); digits.append(register(im))
shared_count=len(tiles)
tilemap=[register(panel.crop((x,y,x+8,y+8))) for y in range(0,144,8) for x in range(0,160,8)]
assert len(tiles)<128

def array(name,seq,width=16):
    lines=[', '.join(str(x) for x in seq[i:i+width]) for i in range(0,len(seq),width)]
    return 'static const uint8_t '+name+'[] = {\n    '+',\n    '.join(lines)+'\n};\n'
header='/* Generated from art/tiles.py. */\n'
for name,val in [('BED',empty),('METAL',metal),('GUIDE',guide),('FLASH',flash),('BLANK',blank),('ART_COUNT',len(tiles))]:
    header+=f'#define {name} {val}u\n'
header+=array('tile_pixels',sum(tiles,[]))+array('panel_map',tilemap,20)+array('glyphs',digits)

# Both layouts start with the same glyph indices so the ROM can draw numbers
# on either screen. Only one layout needs to occupy tile memory at a time.
tiles=tiles[:shared_count]
indices={key:value for key,value in indices.items() if value<shared_count}
picker=Image.new('P',(160,144))
d=ImageDraw.Draw(picker)
lettering(d,7,6,'LEVEL SELECT',2,2,2)
d.line((7,21,152,21),fill=1)
lettering(d,8,27,'START WITH',3)
for y in (37,53,69,85):
    d.line((8,y,151,y),fill=1)
lettering(d,95,89,'UP/DOWN',2)
lettering(d,8,105,'SCORE GOAL',2)
d.line((8,116,151,116),fill=1)
lettering(d,8,122,'LEFT/RIGHT',3); lettering(d,98,122,'LEVEL',2)
lettering(d,8,132,'A/START',3); lettering(d,98,132,'PLAY',2)
picker_map=[register(picker.crop((x,y,x+8,y+8))) for y in range(0,144,8) for x in range(0,160,8)]
assert len(tiles)<128
header+=f'#define PICKER_COUNT {len(tiles)}u\n'
header+=array('picker_pixels',sum(tiles,[]))+array('picker_map',picker_map,20)

# The award plaque uses the second CGB tile bank so its animation can share
# the screen with the well, the landing guide, and the complete sidebar.
tiles,indices=[],{}
def award_frame(unlock, step):
    plaque=Image.new('P',(80,40))
    d=ImageDraw.Draw(plaque)
    d.polygon([(0,5),(5,0),(74,0),(79,5),(79,34),(74,39),(5,39),(0,34)],
              fill=0,outline=1)
    d.line((6,1,73,1),fill=2)
    d.line((6,38,73,38),fill=2)
    for x in (5,72):
        for y in (6,32):
            d.rectangle((x,y,x+1,y+1),fill=1)
    if unlock:
        lift=(0,1,4,6,6,6)[step]
        # The right leg retracts while the left stays anchored in the body.
        d.line([(34,17),(34,12-lift),(37,9-lift),(42,9-lift),
                (45,12-lift),(45,17-lift)],fill=3,width=2)
        d.rectangle((31,16,48,24),fill=2)
        d.line((32,16,47,16),fill=3)
        d.rectangle((39,18,41,20),fill=0)
        d.point((40,21),fill=0)
    else:
        check=[(31,15),(37,21),(48,10)]
        d.line(check[:2],fill=2,width=3)
        if step:
            reach=min(11,step*4)
            d.line([(37,21),(37+reach,21-reach)],fill=3,width=3)
    # Stagger the embers so the burst travels outward and then settles.
    if 1 <= step <= 4:
        for side in (-1,1):
            for dx,y in [(9,7),(14,15),(12,23)]:
                x=39+side*(dx+step*3)
                yy=y+(step-1)*((y-15)//6)
                d.line((x,yy,x+side*(4-step),yy),fill=3 if step<3 else 2)
                if step==2: d.line((x,yy-1,x,yy+1),fill=2)
    lettering(d,17,29,'UNLOCKED' if unlock and step>=2 else 'GOAL MET',3)
    return [register(plaque.crop((x,y,x+8,y+8))) for y in range(0,40,8) for x in range(0,80,8)]

for name,unlock in [('unlock_frames',True),('goal_frames',False)]:
    frames=[award_frame(unlock,step) for step in range(6)]
    header+=array(name,[tile for frame in frames for tile in frame],10)
assert len(tiles)<128
header+=f'#define AWARD_TILE_COUNT {len(tiles)}u\n'
header+=array('award_pixels',sum(tiles,[]))
(ROOT/'engine/include/states/slag_art.h').write_text(header)
