import sys,re,collections,statistics as S
AN="Idle Walk Run Roll Atk1 Atk2 Atk3 Charged Cleave Halfmoon Death Backflip Kneel WalkBack HolyRaise PlungeLeap HeroDrop Lunge StrafeL StrafeR".split()
ST="INTRO CHASE COMBO ROLL CLEAVE HALFMOON MOONBLADE RECOVER STAGGER DEAD SPLITTER UPDOWN TRISLAM HOLYSLAM ROLLCUT PLUNGE LUNGE".split()
def load(fs):
    ev=collections.defaultdict(list); mv=collections.defaultdict(collections.Counter)
    for f in fs:
        for l in open(f,errors='ignore'):
            m=re.search(r'IS64: \[pop\] st (-?\d+)>(-?\d+) clip (-?\d+)>(-?\d+) peak (\d+) pt (-?\d+) yaw (\d+) body (\d+) at (\d+) ms late (\d+) (\d+)',l)
            if not m: continue
            fs_,ts,fa,ta,pk,pt,yw,bd,at,lb,ls=map(int,m.groups())
            if ta<0: continue
            k=(AN[fa] if fa>=0 else '-')+' > '+AN[ta]
            ev[k].append((bd,pk,lb,ls,at))
            if ts>=0: mv[k][ST[ts]]+=1
    return ev,mv
B,mv=load(sys.argv[1].split(',')); R,_=load(sys.argv[2].split(','))
rows=[]
for k,v in B.items():
    if len(v)<4 or k not in R or len(R[k])<3: continue
    bb=S.median(x[0] for x in v); bs=S.median(x[1] for x in v)
    rb=S.median(x[2] for x in R[k]); rs=S.median(x[3] for x in R[k])
    ff=sum(1 for x in v if x[4]<40)/len(v)
    rows.append((max(bb/max(rb,200), bs/max(rs,500)),k,len(v),bb,rb,bs,rs,ff,mv[k].most_common(2)))
rows.sort(reverse=True)
print(f"{'clip change':22s} {'n':>4s} {'body':>6s} {'own':>6s} {'sword':>6s} {'own':>6s} {'x':>5s} {'1st fr':>6s}  moves")
for r,k,n,bb,rb,bs,rs,ff,m in rows[:40]:
    print(f"{k:22s} {n:4d} {bb:6.0f} {rb:6.0f} {bs:6.0f} {rs:6.0f} {r:5.1f} {ff*100:5.0f}%  {' '.join(a for a,_ in m)}")
