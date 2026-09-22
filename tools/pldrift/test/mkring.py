import struct, sys
n=3276; head=int(sys.argv[1]); newest=int(sys.argv[2],0)
buf=bytearray(65536)
for k in range(n):
    i=(head-k)%n
    ts=newest-k*10000
    struct.pack_into('<Q6h',buf,i*20,ts,0,0,0,0,0,0)
open('ring.bin','wb').write(buf)
