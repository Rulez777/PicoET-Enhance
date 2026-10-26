#pragma once
#include <atomic>
#include <cstdio>
#include <cstring>
#include <time.h>
#include <sys/uio.h>
#include "root_file.h"
// Explicit opt-in only. Maximum sixty frames per service lifetime; no background stream.
static void tongue_snapshot(const void* mat) {
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    static unsigned count = 0, pose_count = 0;
    static char previous_label[32] = {};
    static uint64_t last = 0;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = uint64_t(ts.tv_sec)*1000000000ULL + ts.tv_nsec;
    if (busy.test_and_set()) return;
    struct Unlock { std::atomic_flag& f; ~Unlock(){f.clear();} } unlock{busy};
    if (count >= 60 || now-last < 500000000ULL) return;
    int arm = picoet_open_root_file("/data/local/tmp/tongue-capture.arm", O_RDONLY, 0);
    if (arm < 0) return;
    char label[32] = {};
    ssize_t n = read(arm, label, sizeof(label)-1); close(arm);
    if(n<=0)return;
    for(int i=0;i<n;i++) if(label[i]=='\n' || label[i]=='\r'){label[i]=0;break;}
    for(unsigned i=0;label[i];i++) if(!((label[i]>='a'&&label[i]<='z') || label[i]=='_' || (label[i]>='0'&&label[i]<='9')))return;
    if(strcmp(previous_label,label)){strcpy(previous_label,label);pose_count=0;}
    if(pose_count>=8)return;
    last = now; ++pose_count;
    unsigned char header[96];
    iovec dst{header, sizeof(header)}, src{const_cast<void*>(mat), sizeof(header)};
    if (process_vm_readv(getpid(), &dst, 1, &src, 1, 0) != sizeof(header)) return;
    int flags, rows, cols; unsigned char* data; size_t stride;
    memcpy(&flags,header,4); memcpy(&rows,header+8,4); memcpy(&cols,header+12,4);
    memcpy(&data,header+16,8); memcpy(&stride,header+80,8);
    unsigned channels = ((unsigned(flags)>>3)&511)+1;
    char path[160]; snprintf(path,sizeof(path),"/data/local/tmp/tongue-capture03/%s-frame-%02u.meta",label,count);
    int mf=picoet_open_root_file(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(mf>=0){dprintf(mf,"label=%s flags=%08x rows=%d cols=%d channels=%u stride=%zu monotonic_ns=%llu\n",label,flags,rows,cols,channels,stride,(unsigned long long)now);close(mf);}
    unsigned index=count++;
    if ((unsigned(flags)&0xffff0000U)!=0x42ff0000U || (flags&7)!=0 ||
        (channels!=1 && channels!=3) || rows<8 || rows>2048 || cols<8 || cols>2048 ||
        stride<size_t(cols)*channels || stride>16384 || !data) return;
    snprintf(path,sizeof(path),"/data/local/tmp/tongue-capture03/%s-frame-%02u.%s",label,index,channels==1?"pgm":"ppm");
    int fd=picoet_open_root_file(path,O_WRONLY|O_CREAT|O_EXCL,0600); if(fd<0)return;
    dprintf(fd,"P%d\n%d %d\n255\n",channels==1?5:6,cols,rows);
    unsigned char line[6144];
    for(int y=0;y<rows;y++){
        iovec out{line,size_t(cols)*channels},in{data+size_t(y)*stride,size_t(cols)*channels};
        if(process_vm_readv(getpid(),&out,1,&in,1,0)!=ssize_t(out.iov_len))break;
        if(write(fd,line,out.iov_len)!=ssize_t(out.iov_len))break;
    }
    close(fd);
}
