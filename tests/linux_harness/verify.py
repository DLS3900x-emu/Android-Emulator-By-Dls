import struct, zlib
d=open('disk.bin','rb').read(); S=512
assert d[510:512]==b'\x55\xaa' and d[446+4]==0xEE
def hdr(lba):
    h=bytearray(d[lba*S:lba*S+92]); assert h[:8]==b'EFI PART'
    crc=struct.unpack_from('<I',h,16)[0]; h[16:20]=b'\0'*4; assert zlib.crc32(bytes(h))==crc, 'hdr crc'
    my,alt,fu,lu=struct.unpack_from('<QQQQ',h,24); el,n,es,ecrc=struct.unpack_from('<QIII',h,72)
    ents=d[el*S:el*S+n*es]; assert zlib.crc32(ents)==ecrc,'entries crc'
    return my,alt,fu,lu,el,ents
last=len(d)//S-1
p=hdr(1); b=hdr(last)
assert p[0]==1 and p[1]==last and b[0]==last and b[1]==1 and p[5]==b[5]
parts=[]
for i in range(128):
    e=p[5][i*128:(i+1)*128]
    if e[:16]==b'\0'*16: continue
    f,l=struct.unpack_from('<QQ',e,32); name=e[56:128].decode('utf-16-le').rstrip('\0')
    assert p[2]<=f<=l<=p[3]; parts.append((name,f,l))
for (n,f,l),(n2,f2,l2) in zip(parts,parts[1:]): assert l<f2
print('GPT valid:', [(n,f,(l-f+1)*S) for n,f,l in parts])
P={n:(f,l) for n,f,l in parts}
def part(n): f,l=P[n]; return d[f*S:(l+1)*S]
assert part('vbmeta_a')==open('imgs/vbmeta.img','rb').read()
o=open('imgs/odd.img','rb').read(); assert part('odd')[:5000]==o and part('odd')[5000:]==b'\0'*(len(part('odd'))-5000)
exp=open('imgs/super.expected','rb').read(); assert part('super')==exp, 'super mismatch'
assert len(part('missing_a'))==65536 and len(part('userdata'))==3<<20 and len(part('misc'))==1<<20
print('partition contents match sources (sparse expanded correctly)')
# bootconfig per linux init/main.c
r=open('initrd.bin','rb').read()
assert r.endswith(b'#BOOTCONFIG\n')
size,csum=struct.unpack_from('<II',r,len(r)-12-8)
data=r[len(r)-20-size:len(r)-20]
assert sum(data)&0xffffffff==csum and size%4==0
print('bootconfig ok:', data.rstrip(b'\0'))
rd=open('imgs/expect_rd','rb').read(); g=open('imgs/expect_grd','rb').read()
assert r.startswith(rd)
off=len(rd)+(-len(rd))%4; assert r[off:off+len(g)]==g
print('initrd = vendor ramdisks (recovery skipped) + generic ramdisk + bootconfig')
# MP table
m=open('mp.bin','rb').read()
assert m[:4]==b'_MP_' and sum(m[:16])%256==0
ct=m[16:]; assert ct[:4]==b'PCMP'; blen=struct.unpack_from('<H',ct,4)[0]; assert sum(ct[:blen])%256==0
cnt=struct.unpack_from('<H',ct,34)[0]; pos=44; kinds=[]
for i in range(cnt):
    t=ct[pos]; kinds.append(t); pos+= 20 if t==0 else 8
assert pos==blen, (pos,blen)
print('MP table ok:', {k:kinds.count(k) for k in set(kinds)}, 'entries', cnt)
