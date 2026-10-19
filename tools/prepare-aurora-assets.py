"""Compile the approved HTML skin and dragon deformation into native assets.
Inputs are lossless browser renders at the physical 1280x720 resolution.
No browser, audio or network dependency exists in the resulting firmware.
"""
from pathlib import Path
import json, math
import numpy as np
from PIL import Image, ImageFont
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tools/aurora-reference'
OUT=ROOT/'main'
def rgb565(a):
    a=np.asarray(a,dtype=np.uint16)
    return ((a[...,0]>>3)<<11)|((a[...,1]>>2)<<5)|(a[...,2]>>3)
shell=Image.open(WORK/'native-shell.png').convert('RGB')
assert shell.size==(1280,720)
rgb565(shell).astype('<u2').tofile(OUT/'ui_shell.rgb565')
rgb565(shell.crop((36,101,343,408)).resize((56,56),Image.Resampling.LANCZOS)).astype('<u2').tofile(OUT/'ui_dragon_thumb.rgb565')
geometry=json.loads((WORK/'native-geometry.json').read_text())
atlas=bytearray(); definitions=[]
shots=[Image.open(WORK/f'native-{s}.png').convert('RGB') for s in ['off','on','pressed','pressed-on']]
for r in geometry:
    x,y,w,h=[r[k] for k in ['x','y','w','h']]
    if r['id']>=8: x+=3;y+=3;w-=6;h-=6
    stride=(w*2+3)&~3
    definitions.append(f'{{{x},{y},{w},{h},{stride},{len(atlas)}}}')
    for shot in shots:
        data=rgb565(shot.crop((x,y,x+w,y+h))).astype('<u2')
        for row in data: atlas.extend(row.tobytes()+b'\0'*(stride-w*2))
(OUT/'aurora_controls.bin').write_bytes(atlas)
(OUT/'aurora_geometry.h').write_text('#pragma once\nstruct AuroraRect{int x,y,w,h,stride,offset;};\nstatic constexpr AuroraRect aurora_controls[]={\n'+',\n'.join(definitions)+'\n};\nstatic constexpr int AURORA_CONTROLS_BYTES='+str(len(atlas))+';\n')

import sys
import runpy
runpy.run_path(str(ROOT/'tools/prepare-file-filter-assets.py'), run_name='__main__')
runpy.run_path(str(ROOT/'tools/prepare-branding-assets.py'), run_name='__main__')
if '--skin-only' in sys.argv: sys.exit(0)

# Rasterize the same continuous deformation as dragon-renderer.js. Solve the
# inverse map at 3x resolution; subdivision-free sampling avoids moving seams.
src=np.asarray(Image.open(WORK/'dragon.png').convert('RGBA'),dtype=np.float32)
H,W=125,132; SH,SW=src.shape[:2]
yy,xx=np.mgrid[:H*3,:W*3]; fit=min(W/(SW/SH),H)*1.00
dest=np.stack(((xx+.5)/3-W/2,(yy+.5)/3-H/2),-1)/np.array([fit*SW/SH,fit])+.5
def ease(a,b,x):
    t=np.clip((x-a)/(b-a),0,1);return t*t*(3-2*t)
def rotate(p,a):
    return np.stack((p[...,0]*np.cos(a)-p[...,1]*np.sin(a),p[...,0]*np.sin(a)+p[...,1]*np.cos(a)),-1)
def deform(uv,t,flight=False):
    assert not flight, "Flight uses a rigid surface, not the Living deformation"
    x,y=uv[...,0],uv[...,1];q=uv.copy();breath=math.sin(t*math.pi/2)
    wing=ease(.56,.72,x)*(1-ease(.61,.73,y))*ease(.14,.22,y);phase=t*math.pi/2;fold=.5-.5*math.cos(phase)
    local=uv-[.605,.462];scaled=local.copy();scaled[...,0]*=1-.49*fold*.14
    flap=rotate(scaled,math.sin(phase)*.15*.14);flap[...,1]-=.035*math.sin(phase)*.14
    q+=wing[...,None]*(flap-local)
    chest=np.exp(-np.sum(((uv-[.47,.52])*[4,5])**2,-1))
    q+=np.stack(((x-.49)*.045,(y-.57)*.017),-1)*(chest*breath)[...,None]
    head=(1-ease(.30,.48,y))*(1-ease(.62,.80,x));local=uv-[.49,.44]
    q+=head[...,None]*(rotate(local,.028*breath)-local)
    tail=ease(.64,.94,y);q[...,0]+=tail*.028*np.sin(t*math.pi/2-y*6);q[...,1]+=tail*.006*np.cos(t*math.pi/2-y*4)
    return q

# Treat the wing as a separate articulated surface. Its lower boundary is
# exactly the user's red seam. The other boundary crosses transparent space
# between the neck/horns and wing, never a broad bending mask through the wing.
syy,sxx=np.mgrid[:SH,:SW];source_uv=np.stack(((sxx+.5)/SW,(syy+.5)/SH),-1)
root=np.array([.515,.445]);metric=np.array([SW/SH,1.])
axis=np.array([.290,.205])*metric;axis/=np.linalg.norm(axis);normal=np.array([axis[1],-axis[0]])
seam_dist=np.sum((source_uv-root)*metric*normal,-1)
cut=np.interp(source_uv[...,1],[0,.17,.32,.405,.445,1],[.98,.82,.615,.615,.515,.515])
wing_mask=(source_uv[...,0]>cut)&(seam_dist>0)
wing_source=src.copy();wing_source[...,3]*=wing_mask
body_source=src.copy();body_source[...,3]*=~wing_mask
for a in (wing_source,body_source):a[...,:3]*=a[...,3:4]/255

def sample_rgba(picture,uv):
    sx=np.clip(uv[...,0]*SW-.5,0,SW-1);sy=np.clip(uv[...,1]*SH-.5,0,SH-1)
    x0=sx.astype(int);y0=sy.astype(int);x1=np.minimum(x0+1,SW-1);y1=np.minimum(y0+1,SH-1)
    fx=(sx-x0)[...,None];fy=(sy-y0)[...,None]
    rgba=(picture[y0,x0]*(1-fx)+picture[y0,x1]*fx)*(1-fy)+(picture[y1,x0]*(1-fx)+picture[y1,x1]*fx)*fy
    rgba*=np.all((uv>=0)&(uv<=1),axis=-1)[...,None]
    return rgba

body_fixed=sample_rgba(body_source,dest)
def flight_frame(t):
    cycle=(t/1.65)%1
    stroke=.5-.5*math.cos(math.pi*cycle/.42) if cycle<.42 else .5+.5*math.cos(math.pi*(cycle-.42)/.58)
    scale=1-.42*stroke;sweep=.055*stroke
    local=(dest-root)*metric;reach=np.sum(local*normal,-1)/scale
    along=np.sum(local*axis,-1)-reach*sweep
    uv=root+(along[...,None]*axis+reach[...,None]*normal)/metric
    wing=sample_rgba(wing_source,uv)
    # Premultiplied alpha compositing avoids bright seams at the fixed hinge.
    # The sampled cut edge partitions one original surface: preserve coverage
    # by addition there; use wing-over-body only where the folded layers overlap.
    body_factor=np.minimum(1,np.divide(255-wing[...,3:4],body_fixed[...,3:4],out=np.ones_like(wing[...,3:4]),where=body_fixed[...,3:4]>0))
    composed=wing+body_fixed*body_factor
    alpha=composed[...,3:4]
    composed[...,:3]=np.divide(composed[...,:3]*255,alpha,out=np.zeros_like(composed[...,:3]),where=alpha>0)
    return composed

reuse_living=None
if '--flight-only' in sys.argv:
    previous=(OUT/'aurora_dragon.bin').read_bytes()
    assert len(previous)==160*H*W*3, 'Cannot reuse incomplete Living frames'
    reuse_living=previous[60*H*W*3:]
with (OUT/'aurora_dragon.bin').open('wb') as f:
    for flight,frames,period in [(True,60,1.65),(False,100,4.)]:
        if not flight and reuse_living is not None:
            f.write(reuse_living);print('dragon living 100 frames reused byte-exact',flush=True);continue
        for frame in range(frames):
            uv=dest.copy();t=frame/frames*period
            if flight:
                rgba=flight_frame(t)
            else:
                # Newton solve avoids the non-convergent fixed-point seam from v1.3.2.
                for _ in range(32):
                    error=deform(uv,t,flight)-dest
                    if np.max(np.abs(error))<1e-7: break
                    dx=(deform(uv+[1e-5,0],t,flight)-deform(uv-[1e-5,0],t,flight))/2e-5
                    dy=(deform(uv+[0,1e-5],t,flight)-deform(uv-[0,1e-5],t,flight))/2e-5
                    determinant=dx[...,0]*dy[...,1]-dx[...,1]*dy[...,0]
                    inside=np.all((uv>=0)&(uv<=1),axis=-1)
                    assert np.min(determinant[inside])>.1, ('folded dragon mesh',frame,float(np.min(determinant[inside])))
                    safe=np.where(np.abs(determinant)<.02,.02,determinant)
                    delta=np.stack((dy[...,1]*error[...,0]-dy[...,0]*error[...,1],-dx[...,1]*error[...,0]+dx[...,0]*error[...,1]),-1)/safe[...,None]
                    uv-=np.clip(delta,-.06,.06)*.8
                inside=np.all((uv>=0)&(uv<=1),axis=-1)
                assert np.max(np.abs(deform(uv,t,flight)-dest)[inside])<2e-5, ('unresolved dragon pixel',frame)
                sx=np.clip(uv[...,0]*SW-.5,0,SW-1);sy=np.clip(uv[...,1]*SH-.5,0,SH-1)
                x0=sx.astype(int);y0=sy.astype(int);x1=np.minimum(x0+1,SW-1);y1=np.minimum(y0+1,SH-1)
                fx=(sx-x0)[...,None];fy=(sy-y0)[...,None]
                rgba=(src[y0,x0]*(1-fx)+src[y0,x1]*fx)*(1-fy)+(src[y1,x0]*(1-fx)+src[y1,x1]*fx)*fy
                rgba[...,3]*=np.all((uv>=0)&(uv<=1),axis=-1)
            im=Image.fromarray(rgba.clip(0,255).astype(np.uint8),'RGBA').resize((W,H),Image.Resampling.LANCZOS)
            a=np.asarray(im);rgb=rgb565(a).astype('<u2')
            # Interleaved RGB565 + alpha. Transparent texels are skipped at runtime.
            packed=np.empty((H,W,3),dtype=np.uint8);packed[...,:2]=rgb.view(np.uint8).reshape(H,W,2);packed[...,2]=a[...,3]
            f.write(packed.tobytes())
        print('dragon', 'flight' if flight else 'living',frames,'frames',flush=True)


import runpy
runpy.run_path(str(ROOT/"tools/generate-aurora-fonts.py"))
