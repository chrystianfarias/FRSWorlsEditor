from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
import race_routes as routes, colorsys
K=0.0917; CX=312.0; CY=298.5
UP=4; W=512*UP
races=[r for r in routes.races()]
def P(x,y): return (K*x+CX, -K*y+CY)
# base map: per-pixel minimum over all race maps removes every highlighted route
base=None
masks={}
for r in races:
    t=Image.open('tm/TRACKMAP%s.png'%r).convert('RGB')
    base=t if base is None else ImageChops.darker(base,t)
for r in races:
    t=Image.open('tm/TRACKMAP%s.png'%r).convert('L')
    b=base.convert('L')
    diff=ImageChops.subtract(t,b).point(lambda v:255 if v>40 else 0).filter(ImageFilter.MaxFilter(5))
    masks[r]=diff
groups=sorted(set(r[:3] for r in races))
PAL=[(230,25,75),(245,130,48),(255,225,25),(60,180,75),(70,240,240),(0,130,200),(145,30,180),(240,50,230),(210,245,60),(250,190,212),(255,255,255)]
def colour(r):
    return PAL[groups.index(r[:3])%len(PAL)]
out=base.resize((W,W),Image.LANCZOS)
out=Image.eval(out,lambda v:int(v*0.75))
overlay=Image.new('RGBA',(512,512),(0,0,0,0))
d=ImageDraw.Draw(out); od=ImageDraw.Draw(overlay)
try: font=ImageFont.truetype('arialbd.ttf',22); big=ImageFont.truetype('arialbd.ttf',30)
except: font=big=ImageFont.load_default()
labels=[]
stats=[]
for r in races:
    ns=routes.nodes(r); m=masks[r].load(); c=colour(r)
    def on(p):
        x,y=int(p[0]),int(p[1])
        return 0<=x<512 and 0<=y<512 and m[x,y]
    kept=0
    for i in range(1,len(ns)):
        a,b=ns[i-1],ns[i]
        if a['fl']!=b['fl']: continue
        pa,pb=P(a['x'],a['y']),P(b['x'],b['y'])
        mid=((pa[0]+pb[0])/2,(pa[1]+pb[1])/2)
        if not (on(pa) and on(pb) and on(mid)): continue
        kept+=1
        d.line([(pa[0]*UP,pa[1]*UP),(pb[0]*UP,pb[1]*UP)],fill=c,width=5)
        od.line([pa,pb],fill=c+(255,),width=2)
    start=min((n for n in ns if on(P(n['x'],n['y']))),key=lambda n:n['dist'])
    labels.append((r,P(start['x'],start['y']),c))
    stats.append((r,len(ns),kept))
boxes=[]
def hit(b): return any(not(b[2]<o[0] or b[0]>o[2] or b[3]<o[1] or b[1]>o[3]) for o in boxes)
for r,p,c in labels:
    x,y=p[0]*UP,p[1]*UP
    d.ellipse([x-7,y-7,x+7,y+7],fill=c,outline=(0,0,0),width=2)
    boxes.append((x-7,y-7,x+7,y+7))
for r,p,c in labels:
    x,y=p[0]*UP,p[1]*UP
    for k in range(40):
        dx,dy=[(12,-12),(12,14),(-68,-12),(-68,14)][k%4]
        ty=y+dy+(k//4)*26*(1 if k%2 else -1)
        b=(x+dx-2,ty-2,x+dx+58,ty+24)
        if not hit(b): break
    boxes.append(b)
    if abs(ty-(y+dy))>1: d.line([(x,y),(x+dx+(0 if dx>0 else 56),ty+11)],fill=c,width=2)
    d.rectangle(b,fill=(0,0,0),outline=c,width=2)
    d.text((x+dx+3,ty),r,font=font,fill=c)
# legend
lx,ly=40,W-60-34*len(groups)
d.rectangle([lx-20,ly-60,lx+470,W-30],fill=(0,0,0))
d.text((lx,ly-50),'NFSU2 L4RA - %d races'%len(races),font=big,fill=(255,255,255))
for i,g in enumerate(groups):
    ids=[r for r in races if r[:3]==g]
    c=colour(ids[0]); y=ly+i*34
    d.rectangle([lx,y+6,lx+40,y+22],fill=c)
    d.text((lx+52,y),'%sx: %s'%(g,', '.join(r[3] for r in ids)),font=font,fill=(255,255,255))
out.save('races-map-L4RA.png'); overlay.save('races-overlay-L4RA.png')
for s in stats: print(*s)
