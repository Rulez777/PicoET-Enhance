"""Train the shadow segmentation model on the exact 200x200 headset stream format.

  --cv     whole-capture-folder exclusion (each capture held out entirely), report per burst
  --final  train on all 168 frames, save shadow-final.pt (the model the PC runner loads)

Recipe is finetune_spatial_cv.py (revised masks v2, 500 steps, encoder lr 3e-5, head 1e-3,
affine/flip/gamma/noise augmentation, pretrained BN statistics frozen)."""
from pathlib import Path
import argparse, hashlib, json, sys, time
import numpy as np
import torch
from torch.nn import functional as F

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))
from shadow_model import Segment, to_input, geometry, downsample400, OUT  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--cv', action='store_true')
ap.add_argument('--final', action='store_true')
ap.add_argument('--steps', type=int, default=500)
args = ap.parse_args()
assert args.cv != args.final, 'choose --cv or --final'

r = here.parent
torch.hub.set_dir(str(r / 'torch-cache' / 'hub'))
assert torch.cuda.is_available()
data = np.load(r / 'segmentation-data-v2.npz')
rows = json.loads((r / 'dataset-index.json').read_text())
assert list(data['names']) == [v['path'] for v in rows]
for v in rows:
    assert hashlib.sha256((r.parent / v['path']).read_bytes()).hexdigest() == v['sha256']
labels = data['labels']
groups = np.array([Path(v['path']).parent.name for v in rows])
positive = np.array([g.startswith(('left', 'right', 'center')) for g in labels])

u8 = np.rint(data['images'] * 255).astype(np.uint8)
frames = np.stack([downsample400(a) for a in u8])                       # (N,200,200) uint8
X = to_input(torch.tensor(frames, device='cuda'))                       # (N,1,224,224)
Y = F.interpolate(torch.tensor(data['masks'], device='cuda')[:, None], X.shape[-2:], mode='area')


def augment(x, y):
    n = len(x)
    a = (torch.rand(n, device='cuda') - .5) * .35
    scale = .65 + torch.rand(n, device='cuda') * .8
    t = torch.zeros(n, 2, 3, device='cuda')
    t[:, 0, 0] = a.cos() * scale; t[:, 1, 1] = a.cos() * scale
    t[:, 0, 1] = -a.sin() * scale; t[:, 1, 0] = a.sin() * scale
    t[:, :, 2] = (torch.rand(n, 2, device='cuda') - .5) * .18
    flip = torch.where(torch.rand(n, device='cuda') > .5, 1., -1.)
    t[:, 0, :2] *= flip[:, None]
    grid = F.affine_grid(t, x.shape, align_corners=False)
    x = F.grid_sample(x, grid, padding_mode='border', align_corners=False)
    y = F.grid_sample(y, grid, align_corners=False)
    x = (x.clamp_min(1e-6) ** (.7 + torch.rand(n, 1, 1, 1, device='cuda') * .7)) * (.75 + torch.rand(n, 1, 1, 1, device='cuda') * .5)
    x = (x + torch.randn_like(x) * .003).clamp(0, 1)
    return x, F.interpolate(y, (OUT, OUT), mode='area')


def train(idx, seed):
    torch.manual_seed(seed)
    tr = torch.tensor(idx, device='cuda')
    model = Segment().cuda()
    opt = torch.optim.AdamW([dict(params=model.encoder.parameters(), lr=3e-5),
                             dict(params=model.head.parameters(), lr=.001)], weight_decay=.01)
    for _ in range(args.steps):
        model.train(); model.encoder.eval()
        ids = tr[torch.randint(len(tr), (8,), device='cuda')]
        x, y = augment(X[ids], Y[ids])
        p = model(x)
        bce = F.binary_cross_entropy_with_logits(p, y, pos_weight=p.new_tensor(3.))
        s = p.sigmoid()
        dice = 1 - (2 * (s * y).sum((1, 2, 3)) + 1) / (s.sum((1, 2, 3)) + y.sum((1, 2, 3)) + 1)
        loss = bce + dice.mean()
        opt.zero_grad(); loss.backward(); opt.step()
    return model.eval()


out = here / 'runs'
out.mkdir(exist_ok=True)
start = time.time()
if args.final:
    model = train(np.arange(len(rows)), 2600)
    torch.save(model.cpu().state_dict(), here / 'shadow-final.pt')
    print('saved', here / 'shadow-final.pt', hashlib.sha256((here / 'shadow-final.pt').read_bytes()).hexdigest(), f'{time.time()-start:.0f}s')
    sys.exit()

prob = np.zeros((len(rows), OUT, OUT), np.float32)
for fold, held in enumerate(sorted(set(groups))):
    va = np.flatnonzero(groups == held)
    model = train(np.flatnonzero(groups != held), 2600 + fold)
    with torch.inference_mode():
        prob[va] = model(X[torch.tensor(va, device='cuda')]).sigmoid()[:, 0].cpu().numpy()
    print('fold', fold, held, f'{time.time()-start:.0f}s', flush=True)
    del model

report = []
for g in sorted(set(labels), key=lambda s: (s.split('_')[0], s)):
    ids = np.flatnonzero(labels == g)
    geo = [geometry(frames[i], prob[i]) for i in ids]
    report.append(dict(burst=str(g), capture=str(groups[ids[0]]), positive=bool(positive[ids[0]]),
                       present=[x['present'] for x in geo], area=[x['area'] for x in geo],
                       angle=[x['angle'] for x in geo], position=[x['position'] for x in geo]))
    ok = sum(x['present'] == positive[ids[0]] for x in geo)
    ang = [round(x['angle'], 2) for x in geo if x['angle'] is not None]
    print(f"{g:20s} presence {ok}/{len(ids)}  angles {ang}")
np.savez_compressed(out / 'cv-excluded-predictions.npz', probability=prob, labels=labels, groups=groups)
(out / 'cv-report.json').write_text(json.dumps(dict(
    scope='Whole-capture-folder exclusion on 200x200 headset stream format; weak revised masks.',
    steps=args.steps, seconds=time.time() - start, bursts=report), indent=2))
print('CV done', f'{time.time()-start:.0f}s')
