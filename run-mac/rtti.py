import mmap,struct,sys
f=open('g1/files/guest_dump.bin','rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
def u32(a): o=a-0x40000000; return struct.unpack('>I',m[o:o+4])[0]
def cstr(a):
    o=a-0x40000000; e=m.find(b'\0',o); return m[o:e].decode('latin1')
for v in sys.argv[1:]:
    vt=int(v,16); col=u32(vt-4)
    try:
        td=u32(col+12); print(hex(vt), cstr(td+8))
    except Exception as e: print(hex(vt),'?',hex(col))
