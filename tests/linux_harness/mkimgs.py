import struct, os, random, sys
d = sys.argv[1]; os.makedirs(d, exist_ok=True)
random.seed(1)
def rnd(n): return bytes(random.getrandbits(8) for _ in range(n))
# sparse super.img: blk 4096: raw 3 blocks, fill 2 blocks (0xAABBCCDD), dontcare 5 blocks, raw 1 block, crc chunk
B=4096
raw1=rnd(3*B); raw2=rnd(B)
chunks=[]
chunks.append(struct.pack('<HHII',0xCAC1,0,3,12+len(raw1))+raw1)
chunks.append(struct.pack('<HHII',0xCAC2,0,2,16)+struct.pack('<I',0xAABBCCDD))
chunks.append(struct.pack('<HHII',0xCAC3,0,5,12))
chunks.append(struct.pack('<HHII',0xCAC1,0,1,12+len(raw2))+raw2)
chunks.append(struct.pack('<HHII',0xCAC4,0,0,16)+b'\0'*4)
hdr=struct.pack('<IHHHHIIII',0xED26FF3A,1,0,28,12,B,11,len(chunks),0)
open(f'{d}/super.img','wb').write(hdr+b''.join(chunks))
open(f'{d}/super.expected','wb').write(raw1+struct.pack('<I',0xAABBCCDD)*(2*B//4)+b'\0'*5*B+raw2)
open(f'{d}/vbmeta.img','wb').write(rnd(12288))
open(f'{d}/odd.img','wb').write(rnd(5000))  # not sector multiple
# boot images
kernel=rnd(10000); grd=rnd(3000); vrd1=rnd(2000); vrd2=rnd(1500); vrec=rnd(700)
def pad(b,a=4096): return b+b'\0'*((-len(b))%a)
def bootimg(k,r,cmd):
    h=b'ANDROID!'+struct.pack('<IIII',len(k),len(r),0,1580+4)+b'\0'*16+struct.pack('<I',4)+cmd.ljust(1536,b'\0')+struct.pack('<I',0)
    return pad(h)+pad(k)+pad(r)
open(f'{d}/boot.img','wb').write(bootimg(kernel,b'',b'console=ttynull bootconfig'))
open(f'{d}/init_boot.img','wb').write(bootimg(b'',grd,b''))
page=4096
rds=vrd1+vrd2+vrec
bc=b'androidboot.hardware = cutf_cvm\nandroidboot.foo = "bar"\n'
entries=b''
off=0
for data,typ,name in [(vrd1,1,b'base'),(vrd2,3,b'dlkm'),(vrec,2,b'recovery')]:
    entries+=struct.pack('<III',len(data),off,typ)+name.ljust(32,b'\0')+b'\0'*64; off+=len(data)
hsize=2128
h=b'VNDRBOOT'+struct.pack('<IIIII',4,page,0,0,len(rds))+b'loop.max_part=7'.ljust(2048,b'\0')+struct.pack('<I',0)+b'\0'*16+struct.pack('<IIQ',hsize,0,0)+struct.pack('<IIII',len(entries),3,108,len(bc))
assert len(h)==hsize
open(f'{d}/vendor_boot.img','wb').write(pad(h)+pad(rds)+pad(entries)+pad(bc))
open(f'{d}/expect_rd','wb').write(vrd1+vrd2)
open(f'{d}/expect_grd','wb').write(grd)
open(f'{d}/expect_kernel','wb').write(kernel)
# vbmeta with a real AVB header (flags at offset 120, big endian)
avb=bytearray(b'AVB0'+rnd(4092)); avb[120:124]=b'\0\0\0\0'
open(f'{d}/vbmeta_avb.img','wb').write(avb)
# ramdisk: two LZ4-legacy frames, each a newc cpio archive
def cpio(files):
    o=b''
    for name,data in files+[('TRAILER!!!',b'')]:
        n=name.encode()+b'\0'
        h=b'070701'+b''.join(b'%08X'%v for v in [1,0o100644,0,0,1,0,len(data),0,0,0,0,len(n),0])
        o+=h+n; o+=b'\0'*((-len(o))%4); o+=data; o+=b'\0'*((-len(o))%4)
    return o+b'\0'*((-len(o))%512)
def lz4_block(s):
    out=bytearray(); i=0; anchor=0; table={}; n=len(s)
    def emit(lit,off,ml):
        t=(min(len(lit),15)<<4)|(min(ml-4,15) if off else 0); out.append(t)
        if len(lit)>=15:
            r=len(lit)-15
            while r>=255: out.append(255); r-=255
            out.append(r)
        out.extend(lit)
        if off:
            out.extend(struct.pack('<H',off))
            if ml-4>=15:
                r=ml-4-15
                while r>=255: out.append(255); r-=255
                out.append(r)
    while i<n-12:
        k=s[i:i+4]; c=table.get(k); table[k]=i
        if c is not None and i-c<=65535:
            m=4
            while i+m<n-5 and s[c+m]==s[i+m]: m+=1
            emit(s[anchor:i],i-c,m); i+=m; anchor=i
        else: i+=1
    emit(s[anchor:],0,0)
    return bytes(out)
def lz4_legacy(s,blk=8<<20):
    o=struct.pack('<I',0x184C2102)
    for p in range(0,len(s),blk):
        b=lz4_block(s[p:p+blk]); o+=struct.pack('<I',len(b))+b
    return o
a1=cpio([('first_stage_ramdisk/fstab.cf.ext4.hctr2',b'x'*40),('first_stage_ramdisk/fstab.cf.f2fs.cts',b'y'*33),
         ('lib/modules/big.ko',(b'abcdefgh'*5000)+rnd(3000)+b'abcdefgh'*100)])
a2=cpio([('first_stage_ramdisk/fstab.cf.f2fs.hctr2',b'z'*77),('init',rnd(999))])
open(f'{d}/ramdisk.lz4','wb').write(lz4_legacy(a1,blk=30000)+lz4_legacy(a2))
open(f'{d}/ramdisk.cpio','wb').write(a2)
sb=bytearray(4096); sb[1024:1028]=struct.pack('<I',0xF2F52010); open(f'{d}/fs_f2fs.raw','wb').write(sb)
sb=bytearray(4096); sb[1024+56:1024+58]=b'\x53\xEF'; open(f'{d}/fs_ext4.raw','wb').write(sb)

# .xapk bundle: stored and deflated entries, incl. data longer than the 32 KiB deflate window
import zipfile
zb=rnd(5000); zt=(b'The quick brown fox jumps over the lazy dog. '*4000)+b'end'
zm=b''.join((rnd(700)+bytes([i%251])*900+b'aetheron'*60) for i in range(90)); zr=rnd(70000)
for n,v in [('zip_base',zb),('zip_text',zt),('zip_mixed',zm),('zip_random',zr)]: open(f'{d}/{n}','wb').write(v)
with zipfile.ZipFile(f'{d}/bundle.xapk','w') as z:
    z.writestr(zipfile.ZipInfo('base.apk'),zb,zipfile.ZIP_STORED)
    z.writestr(zipfile.ZipInfo('config.x86_64.apk'),zt,zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo('config.arm64_v8a.apk'),zm,zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo('Android/obb/com.example.game/main.1.com.example.game.obb'),zr,zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo('empty.apk'),b'',zipfile.ZIP_DEFLATED)
    z.writestr(zipfile.ZipInfo('manifest.json'),b'{}',zipfile.ZIP_STORED)
b=bytearray(open(f'{d}/bundle.xapk','rb').read()); p=b.find(b'config.x86_64.apk')+len('config.x86_64.apk')+40; b[p]^=0x55
open(f'{d}/bundle_bad.xapk','wb').write(b)
