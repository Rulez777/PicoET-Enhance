from pathlib import Path
import numpy as np,json
from PIL import Image
r=Path(__file__).parent
fs=sorted((r/'Capture03Final').glob('*.pgm'))
a=np.stack([np.asarray(Image.open(f),dtype=np.float32)/255 for f in fs])
labels=[f.with_suffix('.meta').read_text().split()[0].split('=')[1].rsplit('_',1)[0] for f in fs]
full=np.load(r/'personal-template-baseline.npz'); mask=full['mask']
y,x=np.where(mask); take=(y%4==0)&(x%4==0); y=y[take]+150;x=x[take]+60
z=np.sqrt(a[:,y,x]);z-=np.median(z,axis=1)[:,None]
classes=sorted(set(labels)); t=np.stack([z[np.array(labels)==k].mean(0) for k in classes])
scores=((z[:,None,:]-t[None,:,:])**2).mean(2)
print('sampled features',len(y),'training-only correct',sum(classes[i]==truth for i,truth in zip(scores.argmin(1),labels)))
print('training mse',scores.min(1).max(),'margins',np.min((np.sort(scores,axis=1)[:,1]-scores.min(1))/np.sort(scores,axis=1)[:,1]))
# Check temporally withheld frames with only first half of each burst as prototypes.
testt=np.stack([z[[i for i,k in enumerate(labels) if k==c][:4]].mean(0) for c in classes])
testids=[i for c in classes for i in [j for j,k in enumerate(labels) if k==c][4:]]
pred=((z[testids,None,:]-testt[None,:,:])**2).mean(2).argmin(1)
print('same-burst4+4 correct',sum(classes[k]==labels[i] for k,i in zip(pred,testids)),'of',len(testids))
np.savez(r/'live-model.npz',x=x,y=y,templates=t,labels=np.array(classes))
with (r.parents[1]/'hook/src/tongue_model.h').open('w') as f:
 f.write('#pragma once\nnamespace tongue_model {\nconstexpr unsigned N=%d;\n'%len(y))
 f.write('constexpr unsigned offsets[N]={'+','.join(str(int(yy*400+xx)) for yy,xx in zip(y,x))+'};\n')
 f.write('constexpr float templates[4][N]={\n'+',\n'.join('{'+','.join(format(float(v),'.9f')+'f' for v in row)+'}' for row in t)+'};\n}\n')
(r/'live-model-provenance.json').write_text(json.dumps(dict(classes=classes,feature_count=len(y),training_frames=32,scope='personal experimental template model; no independent verification yet'),indent=2))
