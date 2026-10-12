"""One flicker of a scan side by side: the three frames (raw_<frame>_a/b/c)
and, given its count, the marked one, cropped to x0,y0,x1,y1.
    triplet.py <scan dir> <frame> <out.bmp> [x0,y0,x1,y1 [count]]"""
import sys, numpy as np
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from montage import load, write
d, i, out = sys.argv[1], sys.argv[2], sys.argv[3]
x0,y0,x1,y1 = [int(v) for v in sys.argv[4].split(',')] if len(sys.argv)>4 else (0,0,10000,10000)
ims=[load('%s/raw_%s_%s.bmp'%(d,i,k))[y0:y1,x0:x1] for k in 'abc']
ims.append(load('%s/worst_%s_%s.bmp'%(d,i,sys.argv[5]))[y0:y1,x0:x1] if len(sys.argv)>5 else ims[1])
h,w,_=ims[0].shape; g=np.full((h,4*w+18,3),255,np.uint8)
for k,im in enumerate(ims): g[:, k*(w+6):k*(w+6)+w]=im
write(out,g)
