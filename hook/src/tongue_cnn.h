#pragma once
#include <algorithm>
#include <cmath>
#include "tongue_cnn_weights.h"
namespace tongue_cnn {
inline void conv(const float* in,int ci,int h,int w,const float* weights,const float* bias,int co,int k,float* out){
 int oh=(h-k)/2+1,ow=(w-k)/2+1;
 for(int o=0;o<co;o++)for(int y=0;y<oh;y++)for(int x=0;x<ow;x++){
  float sum=bias[o];for(int c=0;c<ci;c++)for(int yy=0;yy<k;yy++)for(int xx=0;xx<k;xx++)sum+=in[(c*h+y*2+yy)*w+x*2+xx]*weights[((o*ci+c)*k+yy)*k+xx];out[(o*oh+y)*ow+x]=std::max(0.f,sum);
 }
}
inline bool infer(const unsigned char* pixels,int dx,int dy,float* probability){
 using namespace tongue_cnn_weights;
 static float input[48*64],samples[48*64],median_buffer[48*64],a[8*22*30],b[12*10*14],c[16*4*6],d[32];
 unsigned count=0;
 for(int y=0;y<48;y++)for(int x=0;x<64;x++){
  float sx=60+(x+.5f)*280.f/64-.5f+dx,sy=140+(y+.5f)*220.f/48-.5f+dy;
  int x0=int(floorf(sx)),y0=int(floorf(sy));float fx=sx-x0,fy=sy-y0;int x1=std::max(0,std::min(399,x0+1)),y1=std::max(0,std::min(399,y0+1));x0=std::max(0,std::min(399,x0));y0=std::max(0,std::min(399,y0));
  float value=((pixels[y0*400+x0]*(1-fx)+pixels[y0*400+x1]*fx)*(1-fy)+(pixels[y1*400+x0]*(1-fx)+pixels[y1*400+x1]*fx)*fy)/255;
  samples[y*64+x]=value<.75f?sqrtf(value):-1;
  if(value<.75f)median_buffer[count++]=samples[y*64+x];
 }
 if(count<500)return false;
 std::nth_element(median_buffer,median_buffer+count/2,median_buffer+count);float med=median_buffer[count/2];
 for(int i=0;i<48*64;i++)input[i]=samples[i]<0?0:std::max(-1.f,std::min(1.f,(samples[i]-med)*3));
 conv(input,1,48,64,c1_weight,c1_bias,8,5,a);conv(a,8,22,30,c2_weight,c2_bias,12,3,b);conv(b,12,10,14,c3_weight,c3_bias,16,3,c);
 for(int o=0;o<32;o++){float s=f1_bias[o];for(int i=0;i<384;i++)s+=c[i]*f1_weight[o*384+i];d[o]=std::max(0.f,s);}
 for(int o=0;o<4;o++){float s=f2_bias[o];for(int i=0;i<32;i++)s+=d[i]*f2_weight[o*32+i];probability[o]=s/2.f;}
 float top=*std::max_element(probability,probability+4),sum=0;for(int k=0;k<4;k++)sum+=probability[k]=expf(probability[k]-top);for(int k=0;k<4;k++)probability[k]/=sum;
 return true;
}
}
