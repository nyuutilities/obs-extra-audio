// SPDX-License-Identifier: GPL-2.0-or-later
// Read/write only MP4 clock metadata. Never touch encoded media or native files when setting durations.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mp4-clock.h"
#include <string.h>
struct box {uint64_t at,size,payload;char type[4];};
static uint32_t be32(const unsigned char *p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t be64(const unsigned char *p) {return ((uint64_t)be32(p)<<32)|be32(p+4);}
static bool io(HANDLE f,uint64_t at,void *bytes,DWORD size,bool write)
{
    LARGE_INTEGER offset={.QuadPart=(LONGLONG)at};DWORD done;
    return SetFilePointerEx(f,offset,NULL,FILE_BEGIN) && (write?WriteFile(f,bytes,size,&done,NULL):ReadFile(f,bytes,size,&done,NULL)) && done==size;
}
static bool box_at(HANDLE f,uint64_t at,uint64_t end,struct box *b)
{
    unsigned char h[16];if(end-at<8 || !io(f,at,h,8,false))return false;
    b->at=at;b->size=be32(h);b->payload=at+8;memcpy(b->type,h+4,4);
    if(b->size==1) {if(end-at<16 || !io(f,at+8,h,8,false))return false;b->size=be64(h);b->payload+=8;}
    else if(!b->size)b->size=end-at;
    return b->size>=b->payload-at && b->size<=end-at;
}
static bool child(HANDLE f,const struct box *parent,const char *type,struct box *found)
{
    for(uint64_t at=parent->payload;at<parent->at+parent->size;) {
        struct box b;if(!box_at(f,at,parent->at+parent->size,&b))return false;
        if(!memcmp(b.type,type,4)) {*found=b;return true;}at+=b.size;
    }
    return false;
}
static HANDLE open_movie(const char *path,bool write,struct box *moov,uint32_t *scale)
{
    wchar_t wide[8192];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,8192))return INVALID_HANDLE_VALUE;
    HANDLE f=CreateFileW(wide,GENERIC_READ|(write?GENERIC_WRITE:0),FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return f;
    LARGE_INTEGER length;bool found=false;
    if(GetFileSizeEx(f,&length))for(uint64_t at=0;at<(uint64_t)length.QuadPart;) {
        struct box b;if(!box_at(f,at,(uint64_t)length.QuadPart,&b))break;
        if(!memcmp(b.type,"moov",4)) {*moov=b;found=true;}at+=b.size;
    }
    struct box mvhd;unsigned char data[32];
    if(!found || !child(f,moov,"mvhd",&mvhd) || mvhd.size-(mvhd.payload-mvhd.at)<24 || !io(f,mvhd.payload,data,24,false) || data[0]>1)goto fail;
    *scale=be32(data+(data[0]?20:12));if(!*scale)goto fail;return f;
fail:CloseHandle(f);return INVALID_HANDLE_VALUE;
}
static bool audio_track(HANDLE f,const struct box *track)
{
    struct box mdia,hdlr;unsigned char handler[4];
    return child(f,track,"mdia",&mdia) && child(f,&mdia,"hdlr",&hdlr) && hdlr.size-(hdlr.payload-hdlr.at)>=12 &&
        io(f,hdlr.payload+8,handler,4,false) && !memcmp(handler,"soun",4);
}
static bool edit_duration(HANDLE f,const struct box *track,uint64_t *duration,uint64_t *position,bool *wide)
{
    struct box edts,elst;unsigned char data[28];
    if(!child(f,track,"edts",&edts) || !child(f,&edts,"elst",&elst) || !io(f,elst.payload,data,8,false) || data[0]>1 || be32(data+4)!=1)return false;
    *wide=data[0]!=0;DWORD size=*wide?20:12;
    if(elst.size-(elst.payload-elst.at)<8+size || !io(f,elst.payload+8,data,size,false))return false;
    int64_t start=*wide?(int64_t)be64(data+8):(int32_t)be32(data+4);if(start<0)return false;
    *duration=*wide?be64(data):be32(data);*position=elst.payload+8;return *duration>0;
}
static bool put_duration(HANDLE f,uint64_t at,uint64_t value,bool wide)
{
    unsigned char data[8];if(!wide && value>UINT32_MAX)return false;
    unsigned size=wide?8:4;for(unsigned i=0;i<size;++i)data[size-i-1]=(unsigned char)(value>>(8*i));return io(f,at,data,size,true);
}
bool ea_mp4_audio_frames(const char *path,unsigned rate,int64_t *frames)
{
    struct box moov;uint32_t scale;HANDLE f=open_movie(path,false,&moov,&scale);if(f==INVALID_HANDLE_VALUE)return false;bool ok=false;
    for(uint64_t at=moov.payload;at<moov.at+moov.size;) {
        struct box b;if(!box_at(f,at,moov.at+moov.size,&b))break;
        if(!memcmp(b.type,"trak",4) && audio_track(f,&b)) {
            uint64_t duration,pos;bool wide;
            if(edit_duration(f,&b,&duration,&pos,&wide) && duration<=(uint64_t)INT64_MAX/rate) {
                *frames=(int64_t)((duration*rate+scale/2)/scale);ok=true;
            }
            break;
        }
        at+=b.size;
    }
    CloseHandle(f);return ok;
}
bool ea_mp4_set_audio_duration(const char *path,unsigned rate,int64_t frames)
{
    if(frames<=0 || !rate)return false;struct box moov;uint32_t scale;
    HANDLE f=open_movie(path,true,&moov,&scale);if(f==INVALID_HANDLE_VALUE)return false;
    bool ok=true;unsigned count=0;uint64_t duration=((uint64_t)frames*scale+rate/2)/rate;
    for(uint64_t at=moov.payload;ok && at<moov.at+moov.size;) {
        struct box b;if(!box_at(f,at,moov.at+moov.size,&b)) {ok=false;break;}
        if(!memcmp(b.type,"trak",4) && audio_track(f,&b)) {
            uint64_t old,pos;bool wide;struct box tkhd;unsigned char version;
            ok=edit_duration(f,&b,&old,&pos,&wide) && put_duration(f,pos,duration,wide) && child(f,&b,"tkhd",&tkhd) && io(f,tkhd.payload,&version,1,false) && version<=1;
            if(ok)ok=put_duration(f,tkhd.payload+(version?28:20),duration,version!=0);++count;
        }else if(!memcmp(b.type,"mvhd",4)) {
            unsigned char version;ok=io(f,b.payload,&version,1,false) && version<=1 && put_duration(f,b.payload+(version?24:16),duration,version!=0);
        }
        at+=b.size;
    }
    CloseHandle(f);return ok && count>0;
}
