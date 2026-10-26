#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include "root_file.h"
#include "tongue_model.h"
// Personal experimental model. No images saved or transmitted. Explicit root-owned LAN IP opts in.
static void tongue_live(const void* mat) {
    static std::atomic_flag busy=ATOMIC_FLAG_INIT;
    static uint64_t last=0, last_good=0; static unsigned stable_class=1; static unsigned sequence=0;
    static int sock=-1, client=-1; static float last_presence=0; static float left=0,right=0,out=0;
    timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t now=uint64_t(ts.tv_sec)*1000000000ULL+ts.tv_nsec;
    if(busy.test_and_set())return;
    struct Unlock {std::atomic_flag& f; ~Unlock(){f.clear();}} unlock{busy};
    if(now-last<50000000ULL)return; last=now;
    int ctl=picoet_open_root_file("/data/local/tmp/tongue-live.ip",O_RDONLY,0);
    if(ctl<0){left=right=out=0;return;}
    char ip[48]={}; ssize_t n=read(ctl,ip,sizeof(ip)-1);close(ctl);
    if(n<=0)return; ip[strcspn(ip,"\r\n ")]=0;
    sockaddr_in dest{};dest.sin_family=AF_INET;dest.sin_port=htons(9031);
    if(inet_pton(AF_INET,ip,&dest.sin_addr)!=1)return;
    uint32_t addr=ntohl(dest.sin_addr.s_addr);
    if((addr>>24)!=10 && (addr>>16)!=0xc0a8 && (addr>>20)!=0xac1)return;
    unsigned char h[96];iovec dh{h,sizeof(h)},sh{const_cast<void*>(mat),sizeof(h)};
    if(process_vm_readv(getpid(),&dh,1,&sh,1,0)!=sizeof(h))return;
    int flags,rows,cols;unsigned char* data;size_t stride;
    memcpy(&flags,h,4);memcpy(&rows,h+8,4);memcpy(&cols,h+12,4);memcpy(&data,h+16,8);memcpy(&stride,h+80,8);
    if((unsigned(flags)&0xffff0fffU)!=0x42ff0000U || rows!=400 || cols!=400 || stride!=400 || !data)return;
    static unsigned char pixels[160000];iovec dp{pixels,sizeof(pixels)},sp{data,sizeof(pixels)};
    if(process_vm_readv(getpid(),&dp,1,&sp,1,0)!=sizeof(pixels))return;
    uint64_t sum_x=0,sum_y=0,count_bright=0;
    for(unsigned yy=0;yy<250;yy++)for(unsigned xx=0;xx<300;xx++)if(pixels[yy*400+xx]>=240){sum_x+=xx;sum_y+=yy;++count_bright;}
    if(count_bright<1000)return;
    int dx=int(lroundf(float(sum_x)/count_bright-tongue_model::ref_x));
    int dy=int(lroundf(float(sum_y)/count_bright-tongue_model::ref_y));
    if(abs(dx)>60 || abs(dy)>60)return;
    float v[tongue_model::N],sorted[tongue_model::N];
    for(unsigned i=0;i<tongue_model::N;i++){
        int yy=int(tongue_model::offsets[i]/400)+dy,xx=int(tongue_model::offsets[i]%400)+dx;
        if(xx<0||xx>=400||yy<0||yy>=400)return;
        sorted[i]=v[i]=sqrtf(pixels[yy*400+xx]/255.f);
    }
    std::nth_element(sorted,sorted+tongue_model::N/2,sorted+tongue_model::N);
    float median=sorted[tongue_model::N/2]; // N odd, matches numpy median.
    float raw[tongue_model::K]={};
    for(unsigned i=0;i<tongue_model::N;i++)for(unsigned k=0;k<tongue_model::K;k++){
        float d=v[i]-median-tongue_model::templates[k][i];raw[k]+=d*d/tongue_model::N;
    }
    float score[4]={1,1,1,1};
    for(unsigned k=0;k<tongue_model::K;k++)score[tongue_model::classes[k]]=std::min(score[tongue_model::classes[k]],raw[k]);
    unsigned best=0;for(unsigned k=1;k<4;k++)if(score[k]<score[best])best=k;
    float second=1.f;for(unsigned k=0;k<4;k++)if(k!=best&&score[k]<second)second=score[k];
    float confidence=(second-score[best])/(second+1.e-9f);
    float visible=std::min(score[0],std::min(score[2],score[3]));
    float visibility=(score[1]-visible)/(score[1]+visible+1.e-9f);
    bool same=best==stable_class && now-last_good<120000000ULL;
    bool accepted=score[best]<(same?0.003f:0.002f) && (best==1?confidence>0.2f:visibility>(same?0.04f:0.08f));
    bool held=!accepted && stable_class!=1 && now-last_good<120000000ULL;
    if(accepted){stable_class=best;last_good=now;last_presence=best==1?0:std::max(0.f,visibility);}
    // Class order center,hidden,left,right. Unknown and hidden immediately neutralize.
    // Continuous signed position from relative similarities to left/center/right.
    // Hidden-vs-visible confidence is separate: center transitions must not be rejected as ambiguous direction.
    float wl=expf(-(score[2]-visible)/0.0007f);
    float wc=expf(-(score[0]-visible)/0.0007f);
    float wr=expf(-(score[3]-visible)/0.0007f);
    float position=std::max(-1.f,std::min(1.f,1.15f*(wr-wl)/(wl+wc+wr+1.e-9f)));
    if(fabsf(position)<.03f)position=0;
    float lo=std::max(0.f,-position),ro=std::max(0.f,position);
    float oo=accepted&&best!=1?1.f:0.f;
    if(held){}else if(!oo)left=right=out=0;else{
        // 20Hz, ~80ms time constant. Preserve fractional output; no binary directional channels.
        constexpr float alpha=.465f;
        left+=(lo-left)*alpha;right+=(ro-right)*alpha;out+=(oo-out)*alpha;
    }
    struct Packet{char magic[4];unsigned seq;float l,r,o,c;} packet{{'T','N','G','2'},((++sequence)&0x3fffffffU)|(accepted?(best==1?0x80000000U:0U):0x40000000U),left,right,out,accepted?last_presence:(held?last_presence:0.f)};
    static_assert(sizeof(Packet)==24,"TNG1 packet ABI");
    if(sock<0){
        sock=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if(sock>=0){sockaddr_un local{};local.sun_family=AF_UNIX;
            const char name[]="pico_tongue_live";memcpy(local.sun_path+1,name,sizeof(name)-1);
            if(bind(sock,(sockaddr*)&local,offsetof(sockaddr_un,sun_path)+sizeof(name))<0 || listen(sock,1)<0){close(sock);sock=-1;}}
    }
    if(client<0 && sock>=0)client=accept4(sock,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
    int sent=client<0?-1:send(client,&packet,sizeof(packet),MSG_DONTWAIT|MSG_NOSIGNAL);
    if(client>=0 && sent!=sizeof(packet)){close(client);client=-1;}
    if(sequence%10==0){
        int f=picoet_open_root_file("/data/local/tmp/tongue-live.status",O_WRONLY|O_CREAT,0600);
        if(f>=0){ftruncate(f,0);dprintf(f,"seq=%u class=%u accepted=%d mse=%.8f confidence=%.4f left=%.3f right=%.3f out=%.3f sent=%d\n",sequence,best,accepted,score[best],packet.c,left,right,out,sent);close(f);}
    }
}
