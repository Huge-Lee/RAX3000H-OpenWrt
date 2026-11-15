/* SPDX-License-Identifier: MIT
 * Validate the single static SquashFS UBI format accepted by this installer.
 * UBI CRCs detect damaged uploads; no signature/vendor/version restriction.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static uint32_t crc(const unsigned char *p, size_t n) {
    uint32_t c=0xffffffff;
    while(n--) { c^=*p++; for(int i=0;i<8;i++) c=(c>>1)^((c&1)?0xedb88320:0); }
    return c;
}
static void fail(const char *s) { fprintf(stderr,"Invalid UBI image: %s\n",s); exit(1); }
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    FILE *f=fopen(argv[1],"rb"); struct stat st;
    if(!f || fstat(fileno(f),&st)) {perror(argv[1]);return 1;}
    if(st.st_size<3*131072 || st.st_size>27*1024*1024 || st.st_size%131072) fail("size");
    unsigned char b[131072]; unsigned next=0,used=0,layouts=0; uint64_t bytes=0;
    while(fread(b,1,sizeof(b),f)==sizeof(b)) {
        if(be32(b)!=0x55424923 || b[4]!=1 || be32(b+16)!=2048 || be32(b+20)!=4096 || be32(b+60)!=crc(b,60)) fail("eraseblock header/geometry/CRC");
        unsigned char *v=b+2048;
        if(be32(v)!=0x55424921 || v[4]!=1 || be32(v+60)!=crc(v,60)) fail("volume header/CRC");
        unsigned id=be32(v+8),lnum=be32(v+12),len=be32(v+20);
        if(id==0x7fffefff) {
            if(v[5]!=1 || lnum>1 || len || layouts&(1u<<lnum)) fail("layout volume");
            layouts|=1u<<lnum;
            unsigned char *r=b+4096;
            if(be32(r+168)!=crc(r,168) || r[12]!=2 || r[14]!=0 || r[15]!=8 || memcmp(r+16,"squashfs",8)) fail("volume table");
            for(int i=1;i<128;i++) if(be32(r+i*172)) fail("additional volume");
            continue;
        }
        if(id || v[5]!=2 || lnum!=next++ || len>126976 || be32(v+32)!=crc(b+4096,len)) fail("static volume/data CRC");
        if(!used) used=be32(v+24);
        if(!used || be32(v+24)!=used || used>220) fail("volume length");
        if(lnum==0 && (memcmp(b+4096,"hsqs",4) || b[4096+28]!=4 || b[4096+20]!=4)) fail("expected SquashFS v4/XZ");
        bytes+=len;
    }
    if(ferror(f) || !feof(f) || layouts!=3 || next!=used) fail("incomplete image");
    printf("UBI_OK blocks=%u squashfs_bytes=%llu\n",next,(unsigned long long)bytes);
    fclose(f); return 0;
}
