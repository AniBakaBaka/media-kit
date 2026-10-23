#pragma once

// One GPU group estimates an 8x8 block. Its lanes share current luma and
// evaluate all integer offsets in +/-8 pixels concurrently. Nonnegative
// partial errors reject candidates that cannot beat zero motion or a lane's
// winner. No CPU frame readback or additional motion buffers are required.
inline constexpr char kFlow[] = R"hlsl(
Texture2D<float4> cur:register(t0); Texture2D<float4> prev:register(t1);
RWTexture2D<float2> motion:register(u0);
cbuffer C:register(b0) { uint w,h,reset,pad; }
groupshared float samples[9];
groupshared float zeroCost;
groupshared float costs[64];
groupshared int2 vectors[64];
float lum(float3 c) { return dot(c,float3(.2126,.7152,.0722)); }
float costAt(int2 base,int2 delta,float limit) {
 float cost=.00005*dot(float2(delta),float2(delta));
 if(cost>=limit) return cost;
 [loop] for(int y=0;y<3;++y) [loop] for(int x=0;x<3;++x) {
  int2 p=clamp(base+int2(1+x*3,1+y*3),int2(0,0),int2(w-1,h-1));
  int2 q=clamp(p+delta,int2(0,0),int2(w-1,h-1));
  cost+=abs(samples[y*3+x]-lum(prev[q].rgb));
  if(cost>=limit) return cost;
 }
 return cost;
}
[numthreads(8,8,1)] void main(uint3 block:SV_GroupID,uint3 pixel:SV_GroupThreadID,uint lane:SV_GroupIndex) {
 int2 base=int2(block.xy)*8;
 int2 p=base+int2(pixel.xy);
 // All threads in a group take the same path around the barriers.
 if(reset!=0) {
  if(p.x<int(w) && p.y<int(h)) motion[p]=0;
  return;
 }
 if(lane<9) {
  int2 q=clamp(base+int2(1+(lane%3)*3,1+(lane/3)*3),int2(0,0),int2(w-1,h-1));
  samples[lane]=lum(cur[q].rgb);
 }
 GroupMemoryBarrierWithGroupSync();
 if(lane==0) zeroCost=costAt(base,int2(0,0),1e10);
 GroupMemoryBarrierWithGroupSync();
 float bestCost=zeroCost; int2 best=0;
 if(zeroCost>0) {
  [loop] for(uint candidate=lane;candidate<289;candidate+=64) {
   int2 delta=int2(int(candidate%17)-8,int(candidate/17)-8);
   float cost=costAt(base,delta,bestCost);
   if(cost<bestCost) { bestCost=cost; best=delta; }
  }
 }
 costs[lane]=bestCost; vectors[lane]=best;
 GroupMemoryBarrierWithGroupSync();
 [unroll] for(uint stride=32;stride>0;stride/=2) {
  if(lane<stride && costs[lane+stride]<costs[lane]) {
   costs[lane]=costs[lane+stride]; vectors[lane]=vectors[lane+stride];
  }
  GroupMemoryBarrierWithGroupSync();
 }
 if(p.x<int(w) && p.y<int(h)) motion[p]=vectors[0];
})hlsl";
