from pathlib import Path
import numpy as np,json
from PIL import Image
r=Path(__file__).parent;m=np.load(r/'live-model-v2.npz');x,y=m['x'],m['y'];protos=list(m['templates']);classes=list(m['classes']);extra=[]
for f in sorted((r/'Capture06Extremes').glob('*.pgm')):
 label=f.with_suffix('.meta').read_text().split()[0].split('=')[1];k=1 if label.startswith('smile') else 3 if label.startswith('right') else 2
 a=np.asarray(Image.open(f),dtype=np.float32)/255;v=np.sqrt(a[y,x]);v-=np.median(v);extra.append((f.name,k,v))
for k in (1,2,3):protos.append(np.mean([v for _,c,v in extra if c==k][:4],axis=0));classes.append(k)
protos=np.stack(protos);classes=np.array(classes);checks=[]
for k in (1,2,3):
 for name,c,v in [rec for rec in extra if rec[1]==k][4:]:
  raw=((protos-v)**2).mean(1);sc=np.array([min(raw[classes==i]) for i in range(4)]);visible=min(sc[[0,2,3]]);presence=float((sc[1]-visible)/(sc[1]+visible+1e-9));checks.append(dict(file=name,truth=k,pred=int(sc.argmin()),mse=float(sc.min()),presence=presence))
(r/'live-model-v3-check.json').write_text(json.dumps(checks,indent=2));np.savez(r/'live-model-v3.npz',x=x,y=y,templates=protos,classes=classes)
with (r.parents[1]/'hook/src/tongue_model.h').open('w') as f:
 f.write('#pragma once\nnamespace tongue_model {\nconstexpr unsigned N=%d,K=%d;\n'%(len(y),len(protos)));f.write('constexpr unsigned classes[K]={'+','.join(map(str,classes))+'};\n');f.write('constexpr unsigned offsets[N]={'+','.join(str(int(yy*400+xx)) for yy,xx in zip(y,x))+'};\n');f.write('constexpr float templates[K][N]={\n'+',\n'.join('{'+','.join(format(float(v),'.9f')+'f' for v in row)+'}' for row in protos)+'};\n}\n')
print('within-burst withheld',sum(v['truth']==v['pred'] for v in checks),'of',len(checks));print(json.dumps(checks,indent=2))
