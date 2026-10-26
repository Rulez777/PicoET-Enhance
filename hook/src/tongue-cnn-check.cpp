#include <cstdio>
#include <cmath>
#include "tongue_cnn.h"
#include "tongue_model.h"
int main(int argc,char**argv){
 for(int i=1;i<argc;i++){
  FILE*f=fopen(argv[i],"rb");if(!f)return 2;int w,h,max;char magic[3];if(fscanf(f,"%2s%d%d%d",magic,&w,&h,&max)!=4||w!=400||h!=400||max!=255)return 3;fgetc(f);unsigned char pixels[160000];if(fread(pixels,1,sizeof(pixels),f)!=sizeof(pixels))return 4;fclose(f);
  double sx=0,sy=0,n=0;for(int y=0;y<250;y++)for(int x=0;x<300;x++)if(pixels[y*400+x]>=240){sx+=x;sy+=y;n++;}
  float p[4];int dx=int(lround(sx/n-tongue_model::ref_x)),dy=int(lround(sy/n-tongue_model::ref_y));if(!tongue_cnn::infer(pixels,dx,dy,p))return 5;
  printf("%s %.9f %.9f %.9f %.9f\n",argv[i],p[0],p[1],p[2],p[3]);
 }
}
