"""Contact sheet of a flicker scan: the worst frames (flickscan.py's
worst_*.bmp, the flickering pixels red) and the heat map, 4 a row.
    montage.py <scan dir> <out.bmp> [count, default 6]"""
import sys, glob, os, numpy as np
sys.argv += []
def load(p):
    b=np.fromfile(p,dtype=np.uint8); off=int.from_bytes(b[10:14].tobytes(),'little'); w=int.from_bytes(b[18:22].tobytes(),'little',signed=True); h=int.from_bytes(b[22:26].tobytes(),'little',signed=True)
    st=(w*3+3)&~3; img=b[off:off+st*abs(h)].reshape(abs(h),st)[:,:w*3].reshape(abs(h),w,3)
    return img[::-1] if h>0 else img
def write(p,img):
    h,w,_=img.shape; st=(w*3+3)&~3; d=np.zeros((h,st),np.uint8); d[:,:w*3]=img[::-1].reshape(h,w*3)
    hdr=b'BM'+(54+d.size).to_bytes(4,'little')+b'\0'*4+(54).to_bytes(4,'little')+(40).to_bytes(4,'little')+w.to_bytes(4,'little')+h.to_bytes(4,'little')+(1).to_bytes(2,'little')+(24).to_bytes(2,'little')+b'\0'*4+d.size.to_bytes(4,'little')+b'\0'*16
    open(p,'wb').write(hdr+d.tobytes())
def main():
  d=sys.argv[1]; files=sorted(glob.glob(d+'/worst_*.bmp'), key=lambda p:-int(p.rsplit('_',1)[1][:-4]))[:int(sys.argv[3]) if len(sys.argv)>3 else 6]
  tiles=[]
  for p in files:
      im=load(p); im=im[::2,::2]; tiles.append(im)
  hp=d+'/heat.bmp'
  if os.path.exists(hp): tiles.append(load(hp))
  s=max(t.shape[0] for t in tiles); tiles=[np.pad(t,((0,s-t.shape[0]),(0,s-t.shape[1]),(0,0))) for t in tiles]
  cols=4; rows=(len(tiles)+cols-1)//cols
  grid=np.full((rows*(s+6),cols*(s+6),3),255,np.uint8)
  for k,t in enumerate(tiles):
      r,c=divmod(k,cols); grid[r*(s+6):r*(s+6)+s, c*(s+6):c*(s+6)+s]=t
  write(sys.argv[2],grid)
  print([os.path.basename(f) for f in files])
if __name__=="__main__": main()
