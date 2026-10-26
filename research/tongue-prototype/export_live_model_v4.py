from pathlib import Path
import numpy as np,json
from PIL import Image
r=Path(__file__).parent;records=[]
for folder in ('Capture03Final','Capture04Extra','Capture06Extremes'):
 for f in sorted((r/folder).glob('*.pgm')):
  lab=f.with_suffix('.meta').read_text().split()[0].split('=')[1];k=2 if lab.startswith('left') else 3 if lab.startswith('right') else 0 if lab.startswith('center') else 1
  a=np.asarray(Image.open(f)); yy,xx=np.where(a[:250,:300]>=240); records.append(dict(file=str(f.relative_to(r)),label=lab,k=k,a=a,cx=float(xx.mean()),cy=float(yy.mean())))
groups=sorted(set(rec['label'] for rec in records));train=[];test=[]
for g in groups:
 q=[v for v in records if v['label']==g];train+=q[:4];test+=q[4:]
refx=np.mean([rec['cx'] for rec in train if rec['file'].startswith('Capture03Final')]);refy=np.mean([rec['cy'] for rec in train if rec['file'].startswith('Capture03Final')])
for rec in records:
 dx=int(round(rec['cx']-refx));dy=int(round(rec['cy']-refy));yy=np.clip(np.arange(400)+dy,0,399);xx=np.clip(np.arange(400)+dx,0,399);rec['aligned']=rec['a'][yy[:,None],xx[None,:]].astype(np.float32)/255
bright=np.maximum.reduce([rec['aligned'][150:340,60:340] for rec in train])>.75
pad=np.pad(bright,4);mask=~np.logical_or.reduce([pad[y:y+190,x:x+280] for y in range(9) for x in range(9)]);y,x=np.where(mask);take=(y%4==0)&(x%4==0);y=y[take]+150;x=x[take]+60
for rec in records:
 v=np.sqrt(rec['aligned'][y,x]);v-=np.median(v);rec['v']=v
protos=[];classes=[]
for group in groups:
 q=[rec for rec in train if rec['label']==group];protos.append(np.mean([rec['v'] for rec in q],axis=0));classes.append(q[0]['k'])
protos=np.stack(protos);classes=np.array(classes);checks=[]
for rec in test:
 raw=((protos-rec['v'])**2).mean(1);sc=np.array([min(raw[classes==i]) for i in range(4)]);vis=min(sc[[0,2,3]]);checks.append(dict(file=rec['file'],truth=rec['k'],pred=int(sc.argmin()),mse=float(sc.min()),presence=float((sc[1]-vis)/(sc[1]+vis+1e-9))))
report=dict(scope='translation-aligned same-burst withheld only',reference=[float(refx),float(refy)],features=len(x),correct=sum(v['truth']==v['pred'] for v in checks),total=len(checks),checks=checks)
(r/'live-model-v4-check.json').write_text(json.dumps(report,indent=2));np.savez(r/'live-model-v4.npz',x=x,y=y,templates=protos,classes=classes,reference=[refx,refy])
with (r.parents[1]/'hook/src/tongue_model.h').open('w') as f:
 f.write('#pragma once\nnamespace tongue_model {\nconstexpr unsigned N=%d,K=%d;\nconstexpr float ref_x=%.8ff,ref_y=%.8ff;\n'%(len(y),len(protos),refx,refy));f.write('constexpr unsigned classes[K]={'+','.join(map(str,classes))+'};\n');f.write('constexpr unsigned offsets[N]={'+','.join(str(int(yy*400+xx)) for yy,xx in zip(y,x))+'};\n');f.write('constexpr float templates[K][N]={\n'+',\n'.join('{'+','.join(format(float(v),'.9f')+'f' for v in row)+'}' for row in protos)+'};\n}\n')
print(json.dumps({k:v for k,v in report.items() if k!='checks'},indent=2));print('errors',[v for v in checks if v['truth']!=v['pred']])
