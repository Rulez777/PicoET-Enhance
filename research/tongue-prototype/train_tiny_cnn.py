from pathlib import Path
import math,json,time
import numpy as np
from PIL import Image
import torch
from torch import nn
from torch.nn import functional as F
r=Path(__file__).parent;torch.manual_seed(20261026);np.random.seed(42);torch.set_num_threads(4)
device='cuda' if torch.cuda.is_available() else 'cpu'
if device=='cuda': torch.cuda.set_per_process_memory_fraction(.06)
ref=np.load(r/'live-model-v4.npz')['reference'];records=[]
for folder in ('Capture03Final','Capture04Extra','Capture06Extremes'):
 for f in sorted((r/folder).glob('*.pgm')):
  label=f.with_suffix('.meta').read_text().split()[0].split('=')[1];k=2 if label.startswith('left') else 3 if label.startswith('right') else 0 if label.startswith('center') else 1
  a=np.asarray(Image.open(f),dtype=np.float32)/255;yy,xx=np.where(a[:250,:300]>=240/255);dx=int(round(xx.mean()-ref[0]));dy=int(round(yy.mean()-ref[1]));ys=np.clip(np.arange(140,360)+dy,0,399);xs=np.clip(np.arange(60,340)+dx,0,399)
  im=F.interpolate(torch.tensor(a[ys[:,None],xs[None,:]])[None,None],size=(48,64),mode='bilinear',align_corners=False)[0,0].numpy();valid=im<.75;v=np.sqrt(im);med=np.partition(v[valid],valid.sum()//2)[valid.sum()//2];v=np.where(valid,np.clip((v-med)*3,-1,1),0)
  records.append((str(f.relative_to(r)),label,k,v))
groups=sorted(set(rec[1] for rec in records));train=[];val=[]
for g in groups:
 q=[i for i,v in enumerate(records) if v[1]==g];train+=q[:4];val+=q[4:]
X=torch.tensor(np.stack([rec[3] for rec in records]))[:,None].to(device);Y=torch.tensor([rec[2] for rec in records]).to(device);tr=torch.tensor(train,device=device);va=torch.tensor(val,device=device)
class Tiny(nn.Module):
 def __init__(self):
  super().__init__();self.c1=nn.Conv2d(1,8,5,2);self.c2=nn.Conv2d(8,12,3,2);self.c3=nn.Conv2d(12,16,3,2);self.f1=nn.Linear(384,32);self.f2=nn.Linear(32,4)
 def forward(self,x):
  x=F.relu(self.c1(x));x=F.relu(self.c2(x));x=F.relu(self.c3(x));x=F.relu(self.f1(x.flatten(1)));return self.f2(x)
def augment(x):
 n=x.shape[0];angle=(torch.rand(n,device=device)-.5)*.22;scale=.94+torch.rand(n,device=device)*.12
 theta=torch.zeros(n,2,3,device=device);theta[:,0,0]=angle.cos()*scale;theta[:,0,1]=-angle.sin()*scale;theta[:,1,0]=angle.sin()*scale;theta[:,1,1]=angle.cos()*scale;theta[:,:,2]=(torch.rand(n,2,device=device)-.5)*.1
 x=F.grid_sample(x,F.affine_grid(theta,x.shape,align_corners=False),align_corners=False,padding_mode='border');x=x*(.8+torch.rand(n,1,1,1,device=device)*.4)+(torch.rand(n,1,1,1,device=device)-.5)*.04+torch.randn_like(x)*.012
 return x.clamp(-1,1)
net=Tiny().to(device);opt=torch.optim.AdamW(net.parameters(),lr=.002,weight_decay=.004);start=time.time()
for step in range(700):
 ids=tr[torch.randint(len(tr),(64,),device=device)];net.train();out=net(augment(X[ids]));loss=F.cross_entropy(out,Y[ids]);opt.zero_grad();loss.backward();opt.step()
 if step%175==0:print('step',step,'loss',float(loss.detach()),flush=True)
net.eval()
with torch.no_grad():
 probs=net(X).softmax(1);pred=probs.argmax(1);correct=int((pred[va]==Y[va]).sum());augment_correct=sum(int((net(augment(X[va])).argmax(1)==Y[va]).sum()) for _ in range(10))
report=dict(model='TinyCNN 1x48x64;3convs+2FC',train_frames=len(train),validation_frames=len(val),correct=correct,augmented_validation_correct=augment_correct,augmented_validation_total=10*len(val),scope='same-burst held-out only; not independent/general accuracy',seconds=time.time()-start,reference=ref.tolist(),errors=[dict(file=records[i][0],truth=records[i][2],pred=int(pred[i]),prob=probs[i].cpu().tolist()) for i in val if pred[i]!=Y[i]])
(r/'tiny-cnn-report.json').write_text(json.dumps(report,indent=2));torch.save(net.cpu().state_dict(),r/'tiny-cnn.pt');np.savez(r/'tiny-cnn-validation.npz',input=X.cpu().numpy(),probabilities=probs.cpu().numpy())
with (r.parents[1]/'hook/src/tongue_cnn_weights.h').open('w') as f:
 f.write('#pragma once\nnamespace tongue_cnn_weights {\n')
 for name,v in net.state_dict().items():f.write('constexpr float '+name.replace('.','_')+'[] = {'+','.join(format(float(k),'.9f')+'f' for k in v.flatten())+'};\n')
 f.write('}\n')
print(json.dumps(report,indent=2),flush=True)
