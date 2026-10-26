from pathlib import Path
import numpy as np,json
from PIL import Image
r=Path(__file__).parent;m=np.load(r/'live-model.npz');x=m['x'];y=m['y'];old=m['templates'];protos=list(old);classes=[0,1,2,3]
extra=[]
for f in sorted((r/'Capture04Extra').glob('*.pgm')):
 label=f.with_suffix('.meta').read_text().split()[0].split('=')[1]
 a=np.asarray(Image.open(f),dtype=np.float32)/255;z=np.sqrt(a[y,x]);z-=np.median(z)
 extra.append((f.name,3 if label.startswith('right') else 1,z))
checks=[]
for c in (3,1):
 group=[z for _,k,z in extra if k==c];protos.append(np.mean(group[:4],axis=0));classes.append(c)
protos=np.stack(protos)
for c in (3,1):
 for name,k,z in [v for v in extra if v[1]==c][4:]:
  raw=((protos-z)**2).mean(1);scores=[float(min(raw[i] for i,k in enumerate(classes) if k==cc)) for cc in range(4)]
  checks.append(dict(file=name,truth=c,pred=int(np.argmin(scores)),scores=scores))
(r/'live-model-v2-check.json').write_text(json.dumps(dict(scope='within-burst withheld extra frames; no new independent test',checks=checks),indent=2))
np.savez(r/'live-model-v2.npz',x=x,y=y,templates=protos,classes=classes)
with (r.parents[1]/'hook/src/tongue_model.h').open('w') as f:
 f.write('#pragma once\nnamespace tongue_model {\nconstexpr unsigned N=%d,K=%d;\n'%(len(y),len(protos)))
 f.write('constexpr unsigned classes[K]={'+','.join(map(str,classes))+'};\n')
 f.write('constexpr unsigned offsets[N]={'+','.join(str(int(yy*400+xx)) for yy,xx in zip(y,x))+'};\n')
 f.write('constexpr float templates[K][N]={\n'+',\n'.join('{'+','.join(format(float(v),'.9f')+'f' for v in row)+'}' for row in protos)+'};\n}\n')
print('extra withheld',sum(k['truth']==k['pred'] for k in checks),'of',len(checks),'prototypes',len(protos))
