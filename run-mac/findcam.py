import mmap, struct, sys
f=open(sys.argv[1],'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ); base=int(sys.argv[2],16)
# the camera object: a view matrix 0xF0 before (-x, y, 0, w) where the matrix row at +0x40 has translation ~ |pos|
i=0; out=[]
while True:
    i=m.find(b'\x3f\x35\x04', i+1)
    if i==-1 or len(out)>5: break
for off in range(0, len(m)-0x200, 16):
    pass
