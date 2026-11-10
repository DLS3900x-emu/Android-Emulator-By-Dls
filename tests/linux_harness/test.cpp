#include <cassert>
#include <cstdio>
#include <vector>
#include <fstream>
#include "disk/disk_config.h"
#include "disk/composite_disk.h"
#include "boot/android_boot.h"
#include "boot/mptable.h"
#include "devices/pit.h"
#include "devices/virtio_blk.h"
#include "devices/virtio_console.h"
#include "devices/ioapic.h"
#include "common/host_clock.h"
#include "devices/virtio_net.h"
#include "devices/virtio_vsock.h"
#include "host/oemlock.h"
#include "boot/ramdisk_inspect.h"
#include <memory>
#include <string>
#include <cstring>
using namespace Aetheron;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); return 1;} }while(0)
static std::vector<uint8_t> rd(const char* p){ std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
struct Irq : IrqSink { std::vector<std::pair<uint32_t,bool>> ev; void SetIrqLevel(uint32_t g,bool l) override { ev.push_back({g,l}); } };
struct MemBackend : BlockBackend { std::vector<uint8_t> d; MemBackend(size_t n):d(n){ for(size_t i=0;i<n;++i) d[i]=uint8_t(i*7+3);} 
  uint64_t SizeBytes() const override {return d.size();} bool Read(uint64_t o,void*b,uint32_t l) override {memcpy(b,&d[o],l);return true;}
  bool Write(uint64_t o,const void*b,uint32_t l) override {memcpy(&d[o],b,l);return true;} bool Flush() override {return true;} };
int main(){
  // ---- disk ----
  { std::ofstream c("imgs/disk.cfg"); c << "misc blank:1M\nvbmeta_a vbmeta.img\nodd odd.img\nsuper super.img\nmissing_a nothere.img ifmissing=blank:64K\nuserdata blank:2M min=3M\n"; }
  std::vector<PartitionSpec> parts; std::string err;
  if(!PrepareDiskPartitions(L"imgs/disk.cfg", L"imgs", L"imgs/cache", &parts, &err)){ printf("err: %s\n", err.c_str()); return 1; }
  CompositeDisk disk; CHECK(disk.Build(parts));
  std::vector<uint8_t> all(disk.SizeBytes());
  for (uint64_t o=0;o<all.size();o+=65536){ uint32_t n=uint32_t(std::min<uint64_t>(65536,all.size()-o)); CHECK(disk.Read(o,&all[o],n)); }
  std::ofstream("disk.bin",std::ios::binary).write((char*)all.data(),all.size());
  // write to a writable partition and to a read-only one
  uint8_t pat[512]; memset(pat,0x5A,512);
  CHECK(!disk.Write(0x100000*2 + 0, pat, 512)); // vbmeta_a (raw source, ro) starts at 2MiB
  // ---- boot ----
  AndroidBootFiles bf;
  CHECK(LoadAndroidBootImages(L"imgs/boot.img", L"imgs/init_boot.img", L"imgs/vendor_boot.img", &bf, &err));
  CHECK(bf.kernel==rd("imgs/expect_kernel")); CHECK(bf.generic_ramdisk==rd("imgs/expect_grd")); CHECK(bf.vendor_ramdisk==rd("imgs/expect_rd"));
  CHECK(bf.boot_cmdline=="console=ttynull bootconfig"); CHECK(bf.vendor_cmdline=="loop.max_part=7");
  CHECK(bf.vendor_bootconfig=="androidboot.hardware = cutf_cvm\nandroidboot.foo = \"bar\"\n");
  auto initrd = BuildInitrdWithBootconfig(bf, "androidboot.a = 1\nandroidboot.bb = x");
  std::ofstream("initrd.bin",std::ios::binary).write((char*)initrd.data(),initrd.size());
  // ---- mptable ----
  std::vector<uint8_t> ram(0x100000);
  uint32_t n = WriteMpTable(ram.data(), 0xF0000, 1, 1, 0xFEC00000, {{1,16},{2,17}});
  std::ofstream("mp.bin",std::ios::binary).write((char*)&ram[0xF0000], n);
  // ---- PIT ----
  { Irq irq; Pit pit(&irq);
    pit.PortWrite(0x61,1,0x01); pit.PortWrite(0x43,1,0xB0); // ch2 mode0 lohi
    uint16_t cnt=11932; pit.PortWrite(0x42,1,cnt&0xff); pit.PortWrite(0x42,1,cnt>>8);
    uint64_t t0=HostNowNs(); while(!(pit.PortRead(0x61,1)&0x20)) {} uint64_t dt=HostNowNs()-t0;
    printf("PIT ch2 mode0 10ms count: OUT2 after %.3f ms\n", dt/1e6); CHECK(dt>9500000 && dt<11000000);
    // readback of live count decreasing
    pit.PortWrite(0x43,1,0xB0); pit.PortWrite(0x42,1,0xff); pit.PortWrite(0x42,1,0xff);
    pit.PortWrite(0x43,1,0x80); uint16_t a=pit.PortRead(0x42,1); a|=pit.PortRead(0x42,1)<<8;
    timespec ts{0,2000000}; nanosleep(&ts,nullptr);
    pit.PortWrite(0x43,1,0x80); uint16_t b=pit.PortRead(0x42,1); b|=pit.PortRead(0x42,1)<<8;
    printf("PIT latch: %u -> %u after ~2ms (expect ~2386 ticks)\n",a,b); CHECK(a>b && a-b>2000 && a-b<3200);
    // ch0 periodic 1ms -> IRQ count in 100ms
    pit.PortWrite(0x43,1,0x34); pit.PortWrite(0x40,1,1193&0xff); pit.PortWrite(0x40,1,1193>>8);
    uint64_t s=HostNowNs(); while(HostNowNs()-s<100000000){ pit.Poll(HostNowNs()); }
    size_t rises=0; for(auto&e:irq.ev) if(e.first==2&&e.second) rises++;
    printf("PIT ch0 1kHz: %zu IRQ0 pulses in 100ms\n", rises); CHECK(rises>=98 && rises<=101); }
  // ---- IOAPIC level + EOI ----
  { std::vector<ApicMessage> got; IoApic io(1,[&](const ApicMessage&m){got.push_back(m);});
    io.MmioWrite(0,4,0x10+2*17); io.MmioWrite(0x10,4,(1<<15)|(1<<13)|0x41); // pin17 level, vector 0x41, unmasked
    io.SetIrqLevel(17,true); CHECK(got.size()==1 && got[0].vector==0x41 && got[0].level_triggered);
    io.SetIrqLevel(17,true); CHECK(got.size()==1); // remote IRR blocks
    io.OnEoi(0x41); CHECK(got.size()==2);          // still asserted -> redelivered
    io.SetIrqLevel(17,false); io.OnEoi(0x41); CHECK(got.size()==2);
    io.MmioWrite(0,4,0x10+2*17); CHECK((io.MmioRead(0x10,4)&(1<<14))==0); }
  // ---- virtio-blk over modern transport ----
  { GuestMemory mem; CHECK(mem.Allocate(64<<20)); Irq irq;
    auto* be = new MemBackend(1<<20); std::unique_ptr<BlockBackend> bp(be);
    VirtioBlk blk(&mem, std::move(bp), "TESTSERIAL"); blk.SetIrq(&irq, 17);
    blk.ConfigWrite(4,2,0x6); // mem + bus master
    CHECK(blk.ConfigRead(0,4)==0x10421AF4);
    // walk capabilities
    int caps=0; for(uint8_t p=blk.ConfigRead(0x34,1); p; p=blk.ConfigRead(p+1,1)) { CHECK(blk.ConfigRead(p,1)==9); caps++; } CHECK(caps==4);
    auto W=[&](uint64_t o,uint8_t s,uint64_t v){blk.BarWrite(o,s,v);}; auto R=[&](uint64_t o,uint8_t s){return blk.BarRead(o,s);};
    W(0x14,1,0); W(0x14,1,1); W(0x14,1,3);
    W(0x00,4,1); CHECK(R(0x04,4)==1); // VERSION_1 in high word
    W(0x08,4,0); W(0x0C,4,0); W(0x08,4,1); W(0x0C,4,1); W(0x14,1,0xB); CHECK(R(0x14,1)&8);
    CHECK(R(0x2000,4)==2048); // capacity in sectors (1MiB/512)
    uint64_t desc=0x10000, avail=0x11000, used=0x12000; W(0x16,2,0); W(0x18,2,16);
    W(0x20,4,desc); W(0x24,4,0); W(0x28,4,avail); W(0x2C,4,0); W(0x30,4,used); W(0x34,4,0); W(0x1C,2,1); W(0x14,1,0xF);
    // request: read 2 sectors at sector 3 into 0x20000, status at 0x21000
    struct { uint32_t type, res; uint64_t sector; } hdr{0,0,3}; mem.Write(0x13000,&hdr,16);
    auto D=[&](int i,uint64_t a,uint32_t l,uint16_t f,uint16_t nx){ uint8_t b[16]; memcpy(b,&a,8);memcpy(b+8,&l,4);memcpy(b+12,&f,2);memcpy(b+14,&nx,2); mem.Write(desc+16*i,b,16);};
    D(0,0x13000,16,1,1); D(1,0x20000,1024,3,2); D(2,0x21000,1,2,0);
    uint16_t z=0, one=1; mem.Write(avail,&z,2); mem.Write(avail+4,&z,2); mem.Write(avail+2,&one,2);
    W(0x3000,2,0);
    uint8_t st=9; mem.Read(0x21000,&st,1); CHECK(st==0);
    CHECK(memcmp(mem.Translate(0x20000,1024), &be->d[3*512], 1024)==0);
    uint16_t uidx; mem.Read(used+2,&uidx,2); CHECK(uidx==1); uint32_t ulen; mem.Read(used+8,&ulen,4); CHECK(ulen==1025);
    CHECK(!irq.ev.empty() && irq.ev.back().second==true);
    CHECK(R(0x1000,1)==1); CHECK(irq.ev.back().second==false); // ISR read deasserts
    printf("virtio-blk transport OK\n"); }
  // ---- shared PCI INTx (wired-OR) ----
  { Irq irq; PciBus bus(&irq); GuestMemory mem; CHECK(mem.Allocate(1<<20));
    auto* a = new VirtioConsole(&mem, "/dev/null"); auto* b = new VirtioConsole(&mem, "/dev/null");
    bus.AddDevice(1, a, 16); bus.AddDevice(9, b, 16);
    struct Probe : VirtioConsole { using VirtioConsole::VirtioConsole; void Raise(bool l){ SetIntx(l);} };
    reinterpret_cast<Probe*>(a)->Raise(true); reinterpret_cast<Probe*>(b)->Raise(true);
    reinterpret_cast<Probe*>(a)->Raise(false); CHECK(irq.ev.back().second==true);   // b still asserting
    reinterpret_cast<Probe*>(b)->Raise(false); CHECK(irq.ev.back().second==false);
    printf("shared INTx OK\n"); }
  // ---- virtio-net: ARP through the virtqueues ----
  { GuestMemory mem; CHECK(mem.Allocate(16<<20)); Irq irq; uint8_t mac[6]={0x52,0x54,0,0x12,0x34,0x56};
    VirtioNet net(&mem, mac); net.SetIrq(&irq, 20); net.ConfigWrite(4,2,0x6);
    auto W=[&](uint64_t o,uint8_t s,uint64_t v){net.BarWrite(o,s,v);};
    W(0x14,1,1); W(0x14,1,3); W(0x08,4,0); W(0x0C,4,(1u<<5)|(1u<<16)); W(0x08,4,1); W(0x0C,4,1); W(0x14,1,0xB);
    CHECK(net.BarRead(0x2000,4)==0x12005452u); // MAC bytes via device config
    auto setq=[&](int q,uint64_t base){ W(0x16,2,q); W(0x18,2,8); W(0x20,4,base); W(0x24,4,0); W(0x28,4,base+0x1000); W(0x2C,4,0); W(0x30,4,base+0x2000); W(0x34,4,0); W(0x1C,2,1); };
    setq(0,0x10000); setq(1,0x20000); W(0x14,1,0xF);
    auto D=[&](uint64_t desc,int i,uint64_t a,uint32_t l,uint16_t f){ uint8_t b[16]={}; memcpy(b,&a,8);memcpy(b+8,&l,4);memcpy(b+12,&f,2); mem.Write(desc+16*i,b,16);};
    auto avail=[&](uint64_t base,uint16_t idx,uint16_t head){ mem.Write(base+0x1000+4+2*(idx%8),&head,2); uint16_t n=idx+1; mem.Write(base+0x1000+2,&n,2);};
    D(0x10000,0,0x30000,2048,2); avail(0x10000,0,0); W(0x3000,2,0);          // rx buffer
    uint8_t arp[12+42]={}; uint8_t* f=arp+12; memset(f,0xff,6); memcpy(f+6,mac,6); f[12]=8;f[13]=6;
    uint8_t* r=f+14; r[1]=1;r[2]=8;r[4]=6;r[5]=4;r[7]=1; memcpy(r+8,mac,6); r[14]=10;r[15]=0;r[16]=2;r[17]=15; r[24]=10;r[25]=0;r[26]=2;r[27]=2;
    mem.Write(0x40000,arp,sizeof(arp)); D(0x20000,0,0x40000,sizeof(arp),0); avail(0x20000,0,0); W(0x3004,2,1);
    uint16_t uidx; mem.Read(0x10000+0x2000+2,&uidx,2); CHECK(uidx==1);
    uint32_t len; mem.Read(0x10000+0x2000+8,&len,4); CHECK(len==12+42);
    uint8_t got[54]; mem.Read(0x30000,got,54); CHECK(got[10]==1 && got[12+12]==8 && got[12+13]==6 && got[12+21]==2);
    printf("virtio-net ARP round trip OK\n"); }
  // ---- vbmeta disable-verification ----
  { { std::ofstream c("imgs/disk2.cfg"); c << "vbmeta_a vbmeta_avb.img disable-verification\n"; }
    std::vector<PartitionSpec> p2; CHECK(PrepareDiskPartitions(L"imgs/disk2.cfg", L"imgs", L"imgs/cache", &p2, &err));
    CHECK(p2.size()==1 && !p2[0].writable);
    auto src=rd("imgs/vbmeta_avb.img"), dst=rd("imgs/cache/vbmeta_a.raw"); CHECK(src.size()==dst.size());
    CHECK(dst[120]==0 && dst[121]==0 && dst[122]==0 && dst[123]==3); dst[123]=0; CHECK(src==dst);
    { std::ofstream c("imgs/disk3.cfg"); c << "vbmeta_a vbmeta.img disable-verification\n"; }
    std::vector<PartitionSpec> p3; CHECK(!PrepareDiskPartitions(L"imgs/disk3.cfg", L"imgs", L"imgs/cache", &p3, &err));
    CHECK(err.find("AVB0")!=std::string::npos);
    printf("vbmeta disable-verification OK\n"); }
  // ---- ramdisk inspection / fstab suffix ----
  { std::string fmt; auto files=ListRamdiskFiles(rd("imgs/ramdisk.lz4"), &fmt);
    CHECK(fmt=="lz4-legacy"); CHECK(files.size()==5);
    CHECK(files[2]=="lib/modules/big.ko" && files[4]=="init");
    CHECK(DetectFilesystem(L"imgs/fs_f2fs.raw")=="f2fs"); CHECK(DetectFilesystem(L"imgs/fs_ext4.raw")=="ext4");
    CHECK(DetectFilesystem(L"imgs/odd.img")=="");
    CHECK(ChooseFstabSuffix(files,"f2fs")=="cf.f2fs.hctr2"); CHECK(ChooseFstabSuffix(files,"ext4")=="cf.ext4.hctr2");
    CHECK(ChooseFstabSuffix(files,"")=="cf.f2fs.hctr2");
    auto f2=ListRamdiskFiles(rd("imgs/ramdisk.cpio"), &fmt); CHECK(fmt=="cpio" && f2.size()==2);
    auto f3=ListRamdiskFiles(rd("imgs/expect_rd"), &fmt); CHECK(fmt=="unknown" && f3.empty());
    CHECK(ChooseFstabSuffix(f3,"f2fs")=="");
    printf("ramdisk fstab detection OK\n"); }
  // ---- oemlock responder (secure_env wire protocol) ----
  { remove("imgs/oemlock.bin");
    auto msg=[](uint32_t cmd,int set){ std::vector<uint8_t> m(8); uint32_t sz=set<0?0:1; memcpy(&m[0],&cmd,4); memcpy(&m[4],&sz,4); if(set>=0) m.push_back(uint8_t(set)); return m; };
    auto val=[](const std::vector<uint8_t>& r,uint32_t cmd){ uint32_t w,s; if(r.size()!=9) return -1; memcpy(&w,&r[0],4); memcpy(&s,&r[4],4); return (w==(cmd|0x80000000u) && s==1)? int(r[8]) : -1; };
    { OemLockResponder o("imgs/oemlock.bin");
      auto m=msg(2,-1); CHECK(val(o.Feed(m.data(),m.size()),2)==1);         // allowed by default
      m=msg(3,-1); CHECK(val(o.Feed(m.data(),m.size()),3)==0);              // unlocked
      m=msg(0,0);                                                           // carrier disallows, sent in two pieces
      CHECK(o.Feed(m.data(),5).empty()); CHECK(val(o.Feed(m.data()+5,4),0)==0);
      m=msg(2,-1); CHECK(val(o.Feed(m.data(),m.size()),2)==0);
      auto a=msg(1,-1), b=msg(3,1); a.insert(a.end(),b.begin(),b.end());    // two requests in one write
      auto r=o.Feed(a.data(),a.size()); CHECK(r.size()==18 && r[8]==1 && r[17]==1);
      uint32_t echo[2]={0x80000002u,1}; uint8_t e[9]; memcpy(e,echo,8); e[8]=1; CHECK(o.Feed(e,9).empty()); }
    { OemLockResponder o("imgs/oemlock.bin");                               // state persisted
      auto m=msg(0,-1); CHECK(val(o.Feed(m.data(),m.size()),0)==0);
      m=msg(3,-1); CHECK(val(o.Feed(m.data(),m.size()),3)==1); }
    printf("oemlock responder OK\n"); }
  // ---- virtio-vsock: connect, data + credit, shutdown ----
  { GuestMemory mem; CHECK(mem.Allocate(16<<20)); Irq irq; VirtioVsock vs(&mem); vs.SetIrq(&irq, 17); vs.ConfigWrite(4,2,0x6);
    auto W=[&](uint64_t o,uint8_t s,uint64_t v){vs.BarWrite(o,s,v);};
    W(0x14,1,1); W(0x14,1,3); W(0x08,4,0); W(0x0C,4,0); W(0x08,4,1); W(0x0C,4,1); W(0x14,1,0xB);
    CHECK(vs.BarRead(0x2000,4)==3);  // guest CID
    auto setq=[&](int q,uint64_t base){ W(0x16,2,q); W(0x18,2,8); W(0x20,4,base); W(0x24,4,0); W(0x28,4,base+0x1000); W(0x2C,4,0); W(0x30,4,base+0x2000); W(0x34,4,0); W(0x1C,2,1); };
    setq(0,0x10000); setq(1,0x20000); setq(2,0x50000); W(0x14,1,0xF);
    auto D=[&](uint64_t desc,int i,uint64_t a,uint32_t l,uint16_t f){ uint8_t b[16]={}; memcpy(b,&a,8);memcpy(b+8,&l,4);memcpy(b+12,&f,2); mem.Write(desc+16*i,b,16);};
    auto avail=[&](uint64_t base,uint16_t idx,uint16_t head){ mem.Write(base+0x1000+4+2*(idx%8),&head,2); uint16_t n=idx+1; mem.Write(base+0x1000+2,&n,2);};
    for(int i=0;i<4;++i){ D(0x10000,i,0x30000+0x100*i,64,2); avail(0x10000,i,i); } W(0x3000,2,0);
    uint16_t txn=0;
    auto send=[&](uint16_t op,uint32_t len,uint64_t dcid){ uint8_t h[44]={}; uint64_t s=3; uint32_t sp=1000,dp=6900; uint16_t ty=1;
      memcpy(h,&s,8); memcpy(h+8,&dcid,8); memcpy(h+16,&sp,4); memcpy(h+20,&dp,4); memcpy(h+24,&len,4); memcpy(h+28,&ty,2); memcpy(h+30,&op,2);
      mem.Write(0x40000,h,44); D(0x20000,0,0x40000,44,0); avail(0x20000,txn++,0); W(0x3004,2,1); };
    auto rx=[&](int i,size_t off){ uint32_t v; mem.Read(0x30000+0x100*i+off,&v,4); return v; };
    auto used=[&]{ uint16_t u; mem.Read(0x10000+0x2000+2,&u,2); return u; };
    send(1,0,2); CHECK(used()==1); CHECK((rx(0,28)>>16)==2); CHECK(rx(0,16)==6900 && rx(0,20)==1000 && rx(0,0)==2 && rx(0,8)==3);
    send(5,40000,2); CHECK(used()==1);                         // below the credit threshold: no reply
    send(5,40000,2); CHECK(used()==2); CHECK((rx(1,28)>>16)==6); CHECK(rx(1,40)==80000 && rx(1,36)==256*1024);
    send(4,0,2); CHECK(used()==3); CHECK((rx(2,28)>>16)==3);   // shutdown -> RST
    send(1,0,7); CHECK(used()==4); CHECK((rx(3,28)>>16)==3);   // unknown CID -> RST
    printf("virtio-vsock OK\n"); }
  // ---- virtio-vsock: host-initiated stream (the adb bridge path) ----
  { GuestMemory mem; CHECK(mem.Allocate(16<<20)); Irq irq; VirtioVsock vs(&mem); vs.SetIrq(&irq, 17); vs.ConfigWrite(4,2,0x6);
    auto W=[&](uint64_t o,uint8_t s,uint64_t v){vs.BarWrite(o,s,v);};
    W(0x14,1,1); W(0x14,1,3); W(0x08,4,0); W(0x0C,4,0); W(0x08,4,1); W(0x0C,4,1); W(0x14,1,0xB);
    auto setq=[&](int q,uint64_t base){ W(0x16,2,q); W(0x18,2,8); W(0x20,4,base); W(0x24,4,0); W(0x28,4,base+0x1000); W(0x2C,4,0); W(0x30,4,base+0x2000); W(0x34,4,0); W(0x1C,2,1); };
    setq(0,0x10000); setq(1,0x20000); setq(2,0x50000); W(0x14,1,0xF);
    auto D=[&](uint64_t desc,int i,uint64_t a,uint32_t l,uint16_t f){ uint8_t b[16]={}; memcpy(b,&a,8);memcpy(b+8,&l,4);memcpy(b+12,&f,2); mem.Write(desc+16*i,b,16);};
    auto avail=[&](uint64_t base,uint16_t idx,uint16_t head){ mem.Write(base+0x1000+4+2*(idx%8),&head,2); uint16_t n=idx+1; mem.Write(base+0x1000+2,&n,2);};
    for(int i=0;i<8;++i){ D(0x10000,i,0x30000+0x100*i,64,2); avail(0x10000,i,i); } W(0x3000,2,0);   // 64-byte rx buffers: 20 payload bytes each
    std::string got; int connected=-1;
    uint32_t h=vs.HostConnect(5555,[&](const uint8_t* d,size_t n){ got.append((const char*)d,n); },[&](bool c){ connected=c; });
    auto rx32=[&](int i,size_t off){ uint32_t v; mem.Read(0x30000+0x100*i+off,&v,4); return v; };
    auto used=[&]{ uint16_t u; mem.Read(0x10000+0x2000+2,&u,2); return u; };
    CHECK(used()==1); CHECK((rx32(0,28)>>16)==1 && rx32(0,16)==h && rx32(0,20)==5555);          // REQUEST to guest port 5555
    CHECK(vs.HostSend(h,(const uint8_t*)"early",5)); CHECK(used()==1);                           // not sent before the guest accepts
    uint16_t txn=0;
    auto send=[&](uint16_t op,const char* payload,uint32_t buf_alloc,uint32_t fwd){ uint8_t p[128]={}; uint64_t s=3,dc=2; uint32_t sp=5555,len=payload?uint32_t(strlen(payload)):0; uint16_t ty=1;
      memcpy(p,&s,8); memcpy(p+8,&dc,8); memcpy(p+16,&sp,4); memcpy(p+20,&h,4); memcpy(p+24,&len,4); memcpy(p+28,&ty,2); memcpy(p+30,&op,2); memcpy(p+36,&buf_alloc,4); memcpy(p+40,&fwd,4);
      if(len) memcpy(p+44,payload,len);
      mem.Write(0x40000,p,44+len); D(0x20000,0,0x40000,44+len,0); avail(0x20000,txn++,0); W(0x3004,2,1); };
    send(2,nullptr,30,0);                                                                        // RESPONSE, guest window 30 bytes
    CHECK(connected==1); CHECK(used()==2); CHECK((rx32(1,28)>>16)==5 && rx32(1,24)==5);
    char buf[32]={}; mem.Read(0x30000+0x100*1+44,buf,5); CHECK(std::string(buf)=="early");
    std::string big(40,'x'); CHECK(vs.HostSend(h,(const uint8_t*)big.data(),big.size()));
    // window 30, 5 used: 20 bytes (buffer size), then 5 bytes, then a credit request because the window is full
    CHECK(used()==5); CHECK(rx32(2,24)==20 && rx32(3,24)==5); CHECK((rx32(4,28)>>16)==7);
    send(6,nullptr,30,30);                                                                       // CREDIT_UPDATE: guest read everything
    CHECK(used()==6); CHECK(rx32(5,24)==15);
    send(5,"abc",30,45); CHECK(got=="abc");                                                      // guest -> host data
    send(4,nullptr,30,45); CHECK(connected==0); CHECK(used()==7); CHECK((rx32(6,28)>>16)==3);    // SHUTDOWN -> RST, closed
    CHECK(!vs.HostSend(h,(const uint8_t*)"x",1));
    printf("virtio-vsock host stream OK\n"); }
  // ---- all 30 PCI slots used by the emulator fit on the bus ----
  { Irq irq; PciBus bus(&irq); GuestMemory mem; CHECK(mem.Allocate(1<<20)); std::vector<std::unique_ptr<VirtioConsole>> cs;
    for (uint8_t s=1;s<=26;++s){ cs.emplace_back(new VirtioConsole(&mem, "")); bus.AddDevice(s, cs.back().get(), 16+(s%8)); }
    bus.PortWrite(0xCF8,4,0x80000000u|(26u<<11)); CHECK((bus.PortRead(0xCFC,4)&0xFFFF)==0x1AF4);
    printf("26 PCI devices OK\n"); }
  printf("ALL C++ CHECKS PASSED\n"); return 0; }
