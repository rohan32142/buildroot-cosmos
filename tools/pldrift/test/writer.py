import mmap, struct, time
n=3276
f=open('ring.bin','r+b'); m=mmap.mmap(f.fileno(),65536)
i=0; ts=(1<<32)-50_000_000
t_end=time.time()+4
while time.time()<t_end:
    for _ in range(200):
        struct.pack_into('<Q',m,i*20,ts & 0xffffffffffffffff)
        i=(i+1)%n; ts+=10000
    time.sleep(0.01)
