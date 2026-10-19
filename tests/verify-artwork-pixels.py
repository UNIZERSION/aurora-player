"""Compare actual RGB565 output against Pillow's independent JPEG decoder."""
from pathlib import Path
from PIL import Image
import io, math, struct, sys
root=Path(sys.argv[1])
def compare(name,source,scaled=False):
    image=Image.open(io.BytesIO(source)).convert('RGB');w,h=image.size
    if scaled:
        scale=0
        while scale<3 and min(w,h)>>(scale+1)>=312:scale+=1
        factor=1<<scale
        image=image.crop((0,0,w//factor*factor,h//factor*factor)).resize((w//factor,h//factor),Image.Resampling.BOX)
        w,h=image.size
    side=min(w,h);left=(w-side)//2;top=(h-side)//2
    raw=(root/(name+'.rgb565')).read_bytes();squared=0;maximum=0;count=0
    for y in range(312):
        for x in range(312):
            color=struct.unpack_from('<H',raw,(y*312+x)*2)[0]
            got=((color>>11)*255/31,((color>>5)&63)*255/63,(color&31)*255/31)
            want=image.getpixel((left+x*side//312,top+y*side//312))
            for a,b in zip(got,want):squared+=(a-b)**2;maximum=max(maximum,abs(a-b));count+=1
    rms=math.sqrt(squared/count)
    assert rms<5, (name,rms,maximum)
    print(f'PASS independent pixel oracle {name}: RGB RMS={rms:.3f}/255 max={maximum:.1f}/255 (includes RGB565 quantization).')
for name in ('sampling12','scaled2','scaled4','scaled8'):compare(name,(root/(name+'.source')).read_bytes(),name.startswith('scaled'))
if len(sys.argv)>2 and sys.argv[2]:
    b=Path(sys.argv[2]).read_bytes();size=sum(v<<s for v,s in zip(b[6:10],(21,14,7,0)));pos=10
    while pos+10<=10+size:
        h=b[pos:pos+10];n=int.from_bytes(h[4:8],'big')
        if h[:4]==b'APIC':
            p=b[pos+10:pos+10+n];mime=p.index(0,1);start=p.index(0,mime+2)+1
            compare('real',p[start:]);break
        pos+=10+n
