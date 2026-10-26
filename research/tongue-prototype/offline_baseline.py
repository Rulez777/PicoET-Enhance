"""Offline feasibility baseline only; same-burst split does not measure generalization."""
from pathlib import Path
import json
import numpy as np
from PIL import Image

root=Path(__file__).parent
folder=root/'Capture03Final'
records=[]
for f in sorted(folder.glob('*.pgm')):
    label=f.with_suffix('.meta').read_text().split()[0].split('=')[1].rsplit('_',1)[0]
    a=np.asarray(Image.open(f),dtype=np.float32)/255
    # Fixed lower-face crop. Mask the saturated nose and immediate surroundings.
    crop=a[150:340,60:340]
    records.append((f.name,label,crop))
bright=np.maximum.reduce([a for _,_,a in records])>0.75
# Conservative 9-pixel box dilation without an additional dependency.
pad=np.pad(bright,4)
mask=~np.logical_or.reduce([pad[y:y+bright.shape[0],x:x+bright.shape[1]] for y in range(9) for x in range(9)])
def feature(a):
    # Relative contrast, not absolute exposure, within the unoccluded crop.
    z=np.sqrt(a[mask]); z-=np.median(z)
    return z
labels=sorted(set(label for _,label,_ in records))
train={label:[r for r in records if r[1]==label][:4] for label in labels}
templates={label:np.mean([feature(a) for _,_,a in group],axis=0) for label,group in train.items()}
results=[]
for label in labels:
    for name,truth,a in [r for r in records if r[1]==label][4:]:
        v=feature(a)
        scores={k:float(np.mean((v-t)**2)) for k,t in templates.items()}
        results.append(dict(file=name,truth=truth,predicted=min(scores,key=scores.get),mse=scores))
report=dict(purpose='same-burst feasibility only; NOT independent accuracy',train_frames=16,test_frames=len(results),correct=sum(r['truth']==r['predicted'] for r in results),labels=labels,mask_pixels=int(mask.sum()),results=results,limitations=['One held pose burst per class','No held-out repeat or headset re-donning','No head-motion/speech/smile negatives','No live inference or OSC integration'])
(root/'baseline-report.json').write_text(json.dumps(report,indent=2))
np.savez(root/'personal-template-baseline.npz',mask=mask,labels=np.array(labels),templates=np.stack([templates[k] for k in labels]),crop=np.array([150,340,60,340]))
print(json.dumps({k:v for k,v in report.items() if k!='results'},indent=2))
