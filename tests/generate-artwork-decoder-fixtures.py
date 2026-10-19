"""Synthetic containers only; no user music or artwork is packaged."""
from pathlib import Path
from PIL import Image
import io, struct, sys

root = Path(sys.argv[1]); root.mkdir(parents=True, exist_ok=True)
manifest = []
def ss(n): return bytes((n >> s) & 127 for s in (21,14,7,0))
def pic(fmt='JPEG', color=(240,32,16), size=(640,640), **kw):
    b=io.BytesIO(); Image.new('RGB',size,color).save(b,fmt,**kw); return b.getvalue()
red=pic(quality=94); green=pic('PNG',(16,240,32),(400,320))
def frame(payload,version=3,name=None,flags=0):
    return ((name or b'PIC')+len(payload).to_bytes(3,'big')+payload if version==2 else
            (name or b'APIC')+(ss(len(payload)) if version==4 else struct.pack('>I',len(payload)))+bytes((0,flags))+payload)
def apic(image=red,kind=3,version=3,encoding=0,desc=None,flags=0):
    png=image.startswith(b'\x89PNG'); desc=(b'\0\0' if encoding in (1,2) else b'\0') if desc is None else desc
    p=bytes((encoding,))+(b'PNG' if png else b'JPG') if version==2 else bytes((encoding,))+b'image/'+(b'png' if png else b'jpeg')+b'\0'
    p+=bytes((kind,))+desc+image
    if flags&0x40 and version==4:p=b'\x01'+p
    if flags&1 and version==4:p=(p[:1]+ss(len(p)-1)+p[1:]) if flags&0x40 else ss(len(p))+p
    if flags&2 and version==4:p=p.replace(b'\xff',b'\xff\0')
    return frame(p,version,flags=flags)
def tag(frames,version=3,flags=0,padding=0):
    p=frames+b'\0'*padding
    if flags&0x80 and version<4:p=p.replace(b'\xff',b'\xff\0')
    return b'ID3'+bytes((version,0,flags))+ss(len(p))+p
def case(name,data,color='red',ext='mp3',bound=1000000,folder=None,source=None):
    p=root/name;p.mkdir(exist_ok=True);(p/('track.'+ext)).write_bytes(data)
    if folder is not None:(p/'cover.png').write_bytes(folder)
    if source is not None:(root/(name+'.source')).write_bytes(source)
    manifest.append(f'{name}\t{ext}\t{color}\t{bound}\n')
case('apic-other',tag(apic(kind=0)),bound=20000)
case('apic-front',tag(apic(green,1)+apic(green,0)+apic(red,3)),bound=20000)
case('other-over-icon',tag(apic(red,1)+apic(green,0)),'green',bound=20000)
case('back-over-icon',tag(apic(green,2)+apic(red,4)))
case('bad-front-valid-other',tag(apic(b'\x89PNG\r\n\x1a\n'+b'bad'*30)+apic(green,0)),'green')
case('v22-other',tag(apic(green,0,2),2),'green')
case('v22-global-unsync',tag(apic(red,0,2),2,0x80))
case('v23-global-unsync',tag(apic(green,0)+apic(red,3),3,0x80))
case('utf16-description',tag(apic(red,0,3,1,b'\xff\xfeA\0B\0\0\0')))
case('v24-unsync',tag(apic(red,0,4,flags=2),4))
case('v24-prefix-unsync',tag(apic(red,0,4,1,b'\xff\xfeA\0\0\0',0x43),4))
case('v24-global-unsync',tag(apic(red,0,4,flags=2),4,0x80))
case('v23-extended',tag(struct.pack('>IHI',6,0,0)+apic(red,0),3,0x40))
case('v24-extended',tag(ss(6)+b'\1\0'+apic(red,0,4),4,0x40))
case('large-tag',tag(frame(b'x'*(4*1024*1024),name=b'PRIV')+apic(red,0),padding=4*1024*1024),bound=20000)
case('large-unrelated-picture',tag(apic(pic('PNG',(0,0,255),(1024,1024)),1)+apic(red,3)),bound=25000)
case('compressed-skipped',tag(frame(b'\0'*64,flags=0x80)+apic(red,0)))
case('missing-description-end',tag(frame(b'\0image/jpeg\0\3unterminated')),'none')
case('truncated-tag',tag(apic())[:-200],'none')
case('invalid-syncsafe',b'ID3\4\0\0\xff\xff\xff\xff'+b'junk'*100,'none')
case('oversized-frame',tag(b'APIC\x7f\xff\xff\xff\0\0'+b'a'*20),'none')
case('linked-picture',tag(frame(b'\0-->\0\3\0https://example.invalid/cover.png')),'none')
case('missing',b'no embedded image','none')
case('folder',b'no embedded image','green',folder=green)
case('aac-id3',tag(apic(green,0))+b'\xff\xf1\x50\x80\x00\xff\xfc','green',ext='aac')
def chunk(name,data):return name+struct.pack('<I',len(data))+data+(b'\0' if len(data)&1 else b'')
def wav(data):return b'RIFF'+struct.pack('<I',len(data)+4)+b'WAVE'+data
case('wav-trailing',wav(chunk(b'data',b'\0'*(3*1024*1024))+chunk(b'JUNK',b'odd')+chunk(b'id3 ',tag(apic(red,0)))),ext='wav',bound=20000)
case('wav-bounds',wav(chunk(b'id3 ',b'ID3\3\0\0'+ss(20000)))+tag(apic()),'none',ext='wav')
def flacblock(kind,data,last=False):return bytes((kind|(128 if last else 0),))+len(data).to_bytes(3,'big')+data
def fp(image,kind):
    mime=b'image/png' if image.startswith(b'\x89PNG') else b'image/jpeg'
    return struct.pack('>II',kind,len(mime))+mime+struct.pack('>I',0)+struct.pack('>IIIII',640,640,24,0,len(image))+image
case('flac-front',b'fLaC'+flacblock(1,b'\0'*(3*1024*1024))+flacblock(6,fp(green,0))+flacblock(6,fp(red,3),True),ext='flac',bound=12000)
case('flac-other',b'fLaC'+flacblock(6,fp(red,1))+flacblock(6,fp(green,0),True),'green',ext='flac',bound=12000)
case('flac-bounds',b'fLaC'+flacblock(6,struct.pack('>II',3,0xffffffff)+b'\0'*24,True),'none',ext='flac')
case('progressive',tag(apic(pic(quality=94,progressive=True),0)),bound=20000)
case('progressive-1024',tag(apic(pic(size=(1024,1024),quality=94,progressive=True),0)))
case('pixel-bomb',tag(apic(pic(size=(4096,4096),quality=75,progressive=True),0)),'none')
case('scratch-budget',tag(apic(pic(size=(2048,2048),quality=75,progressive=True),0)),'none')
case('thin-jpeg',tag(apic(pic(size=(1,4096),quality=95),0)))
case('thin-png',tag(apic(pic('PNG',(16,240,32),(2048,1)),0)),'green')
# Legal SOF0 with Y/Cb/Cr all h=1,v=2. Each block has DC=0 and EOB;
# identical one-bit Huffman tables make a compact independent fixture.
def seg(marker,data):return b'\xff'+bytes((marker,))+struct.pack('>H',len(data)+2)+data
jpeg12=b'\xff\xd8'+seg(0xdb,b'\0'+bytes([1])*64)
jpeg12+=seg(0xc0,b'\x08'+struct.pack('>HHB',640,640,3)+b'\1\x12\0\2\x12\0\3\x12\0')
jpeg12+=seg(0xc4,b'\0\1'+bytes(15)+b'\0'+b'\x10\1'+bytes(15)+b'\0')
jpeg12+=seg(0xda,b'\3\1\0\2\0\3\0\0\x3f\0')+bytes(4800)+b'\xff\xd9'
case('sampling12',tag(apic(jpeg12,0)),'oracle',source=jpeg12)
for name,size in [('scaled2',(800,640)),('scaled4',(1281,1501)),('scaled8',(2601,2504))]:
    w,h=size;im=Image.new('RGB',size);im.putdata([(int(x*230/w),int(y*230/h),int((x+y)*200/(w+h))) for y in range(h) for x in range(w)])
    out=io.BytesIO();im.save(out,'JPEG',quality=97);src=out.getvalue()
    case(name,tag(apic(src,0)),'oracle',source=src)
(root/'manifest.tsv').write_text(''.join(manifest),encoding='utf8')
print(f'Generated {len(manifest)} artwork decoder cases.')
