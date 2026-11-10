#include "net/user_net.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
using namespace Aetheron;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fflush(stdout); _exit(1);} }while(0)
typedef std::vector<uint8_t> V;
static uint8_t GMAC[6]={0x52,0x54,0,0x12,0x34,0x56}, GW[6]={0x52,0x55,0x0a,0,2,2};
static uint16_t be16(const uint8_t*p){return p[0]<<8|p[1];} static uint32_t be32(const uint8_t*p){return uint32_t(p[0])<<24|p[1]<<16|p[2]<<8|p[3];}
static void p16(uint8_t*p,uint16_t v){p[0]=v>>8;p[1]=v;} static void p32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint32_t sw(const uint8_t*p,size_t n,uint32_t s=0){for(size_t i=0;i+1<n;i+=2)s+=p[i]<<8|p[i+1]; if(n&1)s+=p[n-1]<<8; return s;}
static uint16_t fold(uint32_t s){while(s>>16)s=(s&0xffff)+(s>>16);return ~s;}
static uint16_t l4(uint32_t a,uint32_t b,uint8_t pr,const uint8_t*s,size_t n){uint8_t ph[12];p32(ph,a);p32(ph+4,b);ph[8]=0;ph[9]=pr;p16(ph+10,n);return fold(sw(s,n,sw(ph,12)));}
static V ip(uint32_t src,uint32_t dst,uint8_t pr,const V& pl){ V f(34+pl.size()); memcpy(&f[0],GW,6);memcpy(&f[6],GMAC,6);p16(&f[12],0x800);
  uint8_t*i=&f[14]; i[0]=0x45;p16(i+2,20+pl.size());p16(i+6,0x4000);i[8]=64;i[9]=pr;p32(i+12,src);p32(i+16,dst);p16(i+10,fold(sw(i,20))); memcpy(i+20,pl.data(),pl.size()); return f;}
static V udp(uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,const V&d){V u(8+d.size());p16(&u[0],sp);p16(&u[2],dp);p16(&u[4],u.size());memcpy(&u[8],d.data(),d.size());p16(&u[6],l4(src,dst,17,u.data(),u.size()));return ip(src,dst,17,u);}
const uint32_t G=0x0A00020F, GWIP=0x0A000202, DNS=0x0A000203;
// verify checksums of every frame we receive
static void verify(const V& f){ CHECK(f.size()>=14); CHECK(memcmp(&f[6],GW,6)==0); if(be16(&f[12])!=0x800) return; const uint8_t*i=&f[14];
  CHECK(fold(sw(i,20))==0); size_t tl=be16(i+2); CHECK(14+tl==f.size()); if(i[9]==6||i[9]==17) CHECK(l4(be32(i+12),be32(i+16),i[9],i+20,tl-20)==0); CHECK(f.size()<=1514);}
static bool pop(UserNet& n, V* f, int ms=3000){ for(int t=0;t<ms/10;++t){ if(n.PopToGuest(f)){verify(*f);return true;} n.Poll(10);} return false; }
static V tcp(uint16_t sp,uint32_t dst,uint16_t dp,uint32_t seq,uint32_t ack,uint8_t fl,const V& d,uint16_t win=65535){ V t(20+d.size());p16(&t[0],sp);p16(&t[2],dp);p32(&t[4],seq);p32(&t[8],ack);t[12]=5<<4;t[13]=fl;p16(&t[14],win);memcpy(&t[20],d.data(),d.size());p16(&t[16],l4(G,dst,6,t.data(),t.size()));return ip(G,dst,6,t);}
int main(){
  UserNet n(GMAC, nullptr); V f;
  // ARP
  { V a(42); memset(&a[0],0xff,6); memcpy(&a[6],GMAC,6); p16(&a[12],0x806); uint8_t*r=&a[14]; p16(r,1);p16(r+2,0x800);r[4]=6;r[5]=4;p16(r+6,1);memcpy(r+8,GMAC,6);p32(r+14,G);p32(r+24,GWIP);
    n.FromGuest(a.data(),a.size()); CHECK(pop(n,&f)); CHECK(be16(&f[12])==0x806 && be16(&f[20])==2 && memcmp(&f[22],GW,6)==0 && be32(&f[28])==GWIP); printf("ARP ok\n"); }
  // DHCP discover
  { V d(300,0); d[0]=1;d[1]=1;d[2]=6; p32(&d[4],0x1234); memcpy(&d[28],GMAC,6); p32(&d[236],0x63825363); d[240]=53;d[241]=1;d[242]=1;d[243]=255;
    n.FromGuest(udp(0,68,0xffffffff,67,d).data(),udp(0,68,0xffffffff,67,d).size()); CHECK(pop(n,&f)); const uint8_t*b=&f[42];
    CHECK(be32(b+4)==0x1234 && be32(b+16)==G && b[240]==53 && b[242]==2); printf("DHCP OFFER ok\n"); }
  // DNS localhost
  { V q={0xab,0xcd,1,0,0,1,0,0,0,0,0,0, 9,'l','o','c','a','l','h','o','s','t',0, 0,1,0,1};
    auto fr=udp(G,40000,DNS,53,q); n.FromGuest(fr.data(),fr.size()); CHECK(pop(n,&f)); const uint8_t*d=&f[42];
    CHECK(be16(d)==0xabcd && (d[2]&0x80) && be16(d+6)>=1); CHECK(be32(&f[f.size()-4])==0x7F000001); printf("DNS localhost -> 127.0.0.1 ok\n");
    V q2={0,7,1,0,0,1,0,0,0,0,0,0, 7,'n','o','p','e','x','y','z',7,'i','n','v','a','l','i','d',0,0,1,0,1};
    fr=udp(G,40001,DNS,53,q2); n.FromGuest(fr.data(),fr.size()); CHECK(pop(n,&f,8000)); printf("DNS invalid name rcode=%d answers=%d\n", f[42+3]&15, be16(&f[42+6])); CHECK(be16(&f[42+6])==0); }
  // ICMP ping gateway
  { V e={8,0,0,0,0,1,0,1,'h','i'}; p16(&e[2],fold(sw(e.data(),e.size()))); auto fr=ip(G,GWIP,1,e); n.FromGuest(fr.data(),fr.size()); CHECK(pop(n,&f)); CHECK(f[34]==0); printf("ping gateway ok\n"); }
  // UDP echo via 10.0.2.2 -> host 127.0.0.1
  { V d={'h','e','y'}; auto fr=udp(G,5555,GWIP,18081,d); n.FromGuest(fr.data(),fr.size()); CHECK(pop(n,&f)); CHECK(be16(&f[36])==5555 && std::string((char*)&f[42],f.size()-42)=="echo:hey"); printf("UDP NAT ok\n"); }
  // TCP: HTTP GET of 1 MiB, guest window 16 KiB, simulate one dropped segment
  { uint32_t gs=1000; auto syn=tcp(33000,GWIP,18080,gs,0,0x02,{}); 
    { V o=syn; uint8_t*t=&o[34]; } n.FromGuest(syn.data(),syn.size()); CHECK(pop(n,&f)); const uint8_t*t=&f[34];
    CHECK((t[13]&0x12)==0x12 && be32(t+8)==gs+1); uint32_t ss=be32(t+4)+1, rn=ss; gs++;
    std::string req="GET /payload.bin HTTP/1.0\r\nHost: x\r\n\r\n"; V rq(req.begin(),req.end());
    auto a=tcp(33000,GWIP,18080,gs,rn,0x18,rq,16384); n.FromGuest(a.data(),a.size()); gs+=rq.size();
    std::string body; bool fin=false, dropped=false; int segs=0;
    for(int it=0; it<4000 && !fin; ++it){ if(!pop(n,&f,3000)) break; t=&f[34]; CHECK(be16(t+2)==33000);
      size_t hl=(t[12]>>4)*4; size_t dl=f.size()-34-hl; uint32_t sq=be32(t+4);
      if(dl && !dropped && segs==5){ dropped=true; continue; } // lose one segment
      if(sq==rn && dl){ body.append((char*)t+hl,dl); rn+=dl; segs++; }
      if((t[13]&1) && sq+dl==rn){ rn++; fin=true; }
      auto ack=tcp(33000,GWIP,18080,gs,rn,0x10,{},16384); n.FromGuest(ack.data(),ack.size()); }
    CHECK(fin); size_t p=body.find("\r\n\r\n"); CHECK(p!=std::string::npos); std::string b=body.substr(p+4);
    std::ifstream pf("payload.bin",std::ios::binary); std::string want((std::istreambuf_iterator<char>(pf)),{});
    CHECK(b==want); printf("TCP HTTP 1 MiB download ok (%d segments, 1 dropped + recovered)\n",segs);
    auto fn=tcp(33000,GWIP,18080,gs,rn,0x11,{}); n.FromGuest(fn.data(),fn.size()); CHECK(pop(n,&f)); }
  // TCP to closed port -> RST
  { auto syn=tcp(33001,GWIP,1,5,0,0x02,{}); n.FromGuest(syn.data(),syn.size()); CHECK(pop(n,&f)); CHECK(f[34+13]&0x04); printf("TCP refused -> RST ok\n"); }
  printf("ALL NET TESTS PASSED\n"); fflush(stdout); _exit(0); }
