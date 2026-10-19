"""Apply approved browser-rendered Aurora branding without changing control pixels.

branding-export.html is the self-contained HTML source; branding-shell.png is
its lossless1280x720 render. This step follows the two-row file filter patch.
"""
from pathlib import Path
import numpy as np
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'main/ui_shell.rgb565'
REF=ROOT/'tools/aurora-reference'
def main():
    raw=np.frombuffer(OUT.read_bytes(),dtype='<u2').reshape(720,1280).copy()
    image=Image.open(REF/'branding-shell.png').convert('RGB')
    assert image.size==(1280,720)
    rgb=np.array(image,dtype=np.uint16)
    pixels=((rgb[:,:,0]>>3)<<11)|((rgb[:,:,1]>>2)<<5)|(rgb[:,:,2]>>3)
    # Top brand/vent, formerDEMOmark, centered footer, FXsecondary label and
    # moved filter's secondary label. Never replace cover/control/image regions.
    for x0,y0,x1,y1 in [(40,18,440,78),(735,18,877,78),(40,685,1070,704),
                       (657,563,742,583),(962,550,1042,568)]:
        raw[y0:y1,x0:x1]=pixels[y0:y1,x0:x1]
    OUT.write_bytes(raw.astype('<u2').tobytes())
if __name__=='__main__':main()
