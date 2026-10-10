// Pinned ares 9408cb43d4948fc3ea6e152a307a34348df3fe04 CIC model.
// ISC notice: third_party/ares/LICENSE.txt. This reproduces model vectors, not hardware measurements.
// c++ -std=c++20 tools/cic_vectors.cpp -o out/cic_vectors && out/cic_vectors > tools/cic_vectors.h
#include <cstdint>
#include <ranges>
#include <cstdio>
using u32 = uint32_t;
template<unsigned N> struct UInt { u32 value; UInt(u32 v=0): value(v & ((1u<<N)-1)) {} operator u32() const {return value;} UInt& operator=(u32 v) {value=v&((1u<<N)-1);return *this;} };
using n4=UInt<4>; using n3=UInt<3>; using n1=UInt<1>;
auto range(u32 n) { return std::views::iota(0u,n); }
enum { DummyChallenge, RealChallenge }; int challengeAlgo=RealChallenge;
auto challenge(n4 mem[30]) -> void {
  if(challengeAlgo == DummyChallenge) {
    for(u32 address : range(30))
      mem[address] = ~mem[address];
    return;
  }

  //CIC-NUS-6105 anti-piracy challenge
  if(challengeAlgo == RealChallenge) {
    static n4 lut[32] = {
      0x4, 0x7, 0xa, 0x7, 0xe, 0x5, 0xe, 0x1,
      0xc, 0xf, 0x8, 0xf, 0x6, 0x3, 0x6, 0x9,
      0x4, 0x1, 0xa, 0x7, 0xe, 0x5, 0xe, 0x1,
      0xc, 0x9, 0x8, 0x5, 0x6, 0x3, 0xc, 0x9,
    };

    n4 key = 0xb;
    n1 sel = 0;
    for(u32 address : range(30)) {
      n4 data = key + 5 * mem[address];
      mem[address] = data;
      key = lut[sel << 4 | data];
      n1 mod = data >> 3;
      n3 mag = data >> 0;
      if(mod) mag = ~mag;
      if(mag % 3 != 1) mod = !mod;
      if(sel) {
        if(data == 0x1 || data == 0x9) mod = 1;
        if(data == 0xb || data == 0xe) mod = 0;
      }
      sel = mod;
    }
    return;
  }
}


int main() { puts("// Generated from the pinned ares CIC transition with masked n4/n3/n1 types.\nstatic const struct { u8 input[15], response[15]; } cic_vectors[] = {");
for(unsigned s=0;s<16;s++) { unsigned rng=0x12345678+s; n4 mem[30]; unsigned input[15]={}; for(unsigned i=0;i<30;i++){rng=rng*1664525+1013904223; unsigned x=s==0?0:s==1?15:s==2?(i&15):rng>>28; mem[i]=x; input[i/2]|=x<<((i&1)?0:4);} challenge(mem); printf("{{"); for(unsigned i=0;i<15;i++)printf("%s0x%02x",i?",":"",input[i]); printf("},{");for(unsigned i=0;i<15;i++)printf("%s0x%02x",i?",":"",(unsigned(mem[2*i])<<4)|unsigned(mem[2*i+1])); puts("}},"); } puts("};"); }
