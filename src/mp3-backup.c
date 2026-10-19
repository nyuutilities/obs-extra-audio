// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mp3-backup.h"
#include <util/platform.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/intreadwrite.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

static bool add(char *out,size_t size,const char *text)
{
    size_t used=strlen(out),length=strlen(text);if(used+length>=size)return false;
    memcpy(out+used,text,length+1);return true;
}
static bool date_pattern(char *out,size_t size,const char *pattern,time_t started)
{
    struct tm local;if(localtime_s(&local,&started))return false;out[0]=0;
    static const char *tokens[][2]={
        {"%CCYY","%Y"},{"%YY","%y"},{"%MM","%m"},{"%DD","%d"},{"%hh","%H"},{"%mm","%M"},{"%ss","%S"},{"%%","%%"},
        {"%a","%a"},{"%A","%A"},{"%b","%b"},{"%B","%B"},{"%d","%d"},{"%H","%H"},{"%I","%I"},{"%m","%m"},
        {"%M","%M"},{"%p","%p"},{"%S","%S"},{"%y","%y"},{"%Y","%Y"},{"%z","%z"},{"%Z","%Z"}};
    for(const char *p=pattern;*p;) {
        bool found=false;char value[128];
        for(unsigned i=0;i<sizeof(tokens)/sizeof(tokens[0]);++i)if(!strncmp(p,tokens[i][0],strlen(tokens[i][0]))) {
            if(!strftime(value,sizeof(value),tokens[i][1],&local) || !add(out,size,value))return false;
            p+=strlen(tokens[i][0]);found=true;break;
        }
        if(found)continue;
        if(!strncmp(p,"%s",2)) {snprintf(value,sizeof(value),"%lld",(long long)started);if(!add(out,size,value))return false;p+=2;continue;}
        // Let OBS expand its non-date video tokens; date tokens above use one start-time snapshot.
        const char *video[]={"%FPS","%CRES","%ORES","%VF"};
        for(unsigned i=0;i<4;++i)if(!strncmp(p,video[i],strlen(video[i]))) {
            char *expanded=os_generate_formatted_filename("",true,video[i]);
            bool ok=expanded && add(out,size,expanded);bfree(expanded);if(!ok)return false;
            p+=strlen(video[i]);found=true;break;
        }
        if(found)continue;
        if(*p=='%') {++p;continue;}
        char one[2]={*p++,0};if(!add(out,size,one))return false;
    }
    return true;
}
// A blank track name leaves no dangling separator: "{recording}_Track {track} - {name}" gives "<recording>_Track 7".
static void drop_blank_name(char *pattern)
{
    static const char *parts[]={" - {name}","{name} - "};
    for(unsigned i=0;i<2;++i)for(char *at;(at=strstr(pattern,parts[i]))!=NULL;)memmove(at,at+strlen(parts[i]),strlen(at+strlen(parts[i]))+1);
}
bool ea_backup_name(char *out,size_t size,const char *pattern,const char *recording,const struct ea_extra_track *track,time_t started)
{
    if(!pattern || !*pattern || strpbrk(pattern,"/\\") || strlen(pattern)>=1024)return false;
    char own[1024];snprintf(own,sizeof(own),"%s",pattern);if(!*track->name)drop_blank_name(own);
    char dated[2048];if(!date_pattern(dated,sizeof(dated),own,started))return false;
    size_t len;
    const char *base=recording,*a=strrchr(recording,'/'),*b=strrchr(recording,'\\');
    if(a || b)base=(a && (!b || a>b)?a:b)+1;
    char stem[1024];snprintf(stem,sizeof(stem),"%s",base);char *dot=strrchr(stem,'.');if(dot)*dot=0;
    char name[2048]="",number[16];snprintf(number,sizeof(number),"%u",track->number);
    bool ok=true;
    for(const char *p=dated;*p && ok;) {
        if(!strncmp(p,"{track}",7)){ok=add(name,sizeof(name),number);p+=7;}
        else if(!strncmp(p,"{name}",6)){ok=add(name,sizeof(name),track->name);p+=6;}
        else if(!strncmp(p,"{recording}",11)){ok=add(name,sizeof(name),stem);p+=11;}
        else {char one[2]={*p++,0};ok=add(name,sizeof(name),one);}
    }
    if(!ok)return false;
    for(char *p=name;*p;++p)if((unsigned char)*p<32 || strchr("<>:\"/\\|?*",*p))*p='_';
    len=strlen(name);while(len && (name[len-1]=='.' || name[len-1]==' '))name[--len]=0;
    if(!len || len>220)return false;
    if(len>4 && !strcmp(name+len-4,".mp3"))name[len-4]=0;
    char device[16];size_t base_length=strcspn(name,".");
    if(base_length<sizeof(device)) {
        for(size_t i=0;i<base_length;++i)device[i]=(char)toupper((unsigned char)name[i]);device[base_length]=0;
        if(!strcmp(device,"CON") || !strcmp(device,"PRN") || !strcmp(device,"AUX") || !strcmp(device,"NUL") ||
            (base_length==4 && (!strncmp(device,"COM",3) || !strncmp(device,"LPT",3)) && device[3]>='1' && device[3]<='9')) {
            memmove(name+1,name,strlen(name)+1);name[0]='_';
        }
    }
    if(strlen(name)>=size)return false;
    memcpy(out,name,strlen(name)+1);return true;
}
bool ea_backup_filename_at(char *path,size_t size,const char *directory,const char *pattern,
    const char *recording,const struct ea_extra_track *track,time_t started)
{
    char name[2048];
    if(!directory || !*directory || !ea_backup_name(name,sizeof(name),pattern,recording,track,started))return false;
    for(unsigned collision=0;collision<10000;++collision) {
        char suffix[32]="";if(collision)snprintf(suffix,sizeof(suffix)," (%u)",collision+1);
        int n=snprintf(path,size,"%s/%s%s.mp3",directory,name,suffix);
        if(n<0 || (size_t)n>=size)return false;
        if(!os_file_exists(path))return true;
    }
    return false;
}
bool ea_backup_filename(char *path,size_t size,const char *directory,const char *pattern,
    const char *recording,const struct ea_extra_track *track)
{
    return ea_backup_filename_at(path,size,directory,pattern,recording,track,time(NULL));
}

#define MP3_TAIL_PACKETS 128u
struct mp3_track { AVFormatContext *output; AVCodecContext *codec; AVFrame *frame; AVStream *stream; HANDLE file;
    unsigned buffered,tail_read,tail_count;bool header;int64_t written_end;AVPacket *tail[MP3_TAIL_PACKETS]; };
struct ea_mp3_recorder { struct mp3_track tracks[EA_MAX_EXTRA_TRACKS]; unsigned count; int64_t position,final_frames; bool failed,finishing; AVPacket *packet; };
static int file_write(void *data,const uint8_t *buffer,int size)
{
    struct mp3_track *t=data;DWORD done;
    return WriteFile(t->file,buffer,(DWORD)size,&done,NULL) && done==(DWORD)size ? size : AVERROR(EIO);
}
static int64_t file_seek(void *data,int64_t offset,int whence)
{
    struct mp3_track *t=data;LARGE_INTEGER at={.QuadPart=offset},result;
    if(whence==AVSEEK_SIZE)return GetFileSizeEx(t->file,&result)?result.QuadPart:AVERROR(EIO);
    whence&=~AVSEEK_FORCE;
    DWORD origin=whence==SEEK_SET?FILE_BEGIN:whence==SEEK_CUR?FILE_CURRENT:whence==SEEK_END?FILE_END:MAXDWORD;
    if(origin==MAXDWORD)return AVERROR(EINVAL);
    return SetFilePointerEx(t->file,at,&result,origin)?result.QuadPart:AVERROR(EIO);
}
static time_t recording_start(const char *recording)
{
    wchar_t wide[8192];WIN32_FILE_ATTRIBUTE_DATA info;
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,recording,-1,wide,8192) && GetFileAttributesExW(wide,GetFileExInfoStandard,&info)) {
        uint64_t ticks=((uint64_t)info.ftCreationTime.dwHighDateTime<<32)|info.ftCreationTime.dwLowDateTime;
        return (time_t)(ticks/10000000ULL-11644473600ULL);
    }
    return time(NULL);
}
static bool open_new(struct mp3_track *t,char *path,size_t size,const char *directory,const char *pattern,
    const char *recording,const struct ea_extra_track *track,time_t started)
{
    for(unsigned attempt=0;attempt<10000;++attempt) {
        if(!ea_backup_filename_at(path,size,directory,pattern,recording,track,started))return false;
        wchar_t wide[8192];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,8192))return false;
        // CREATE_NEW owns the name and handle atomically, even with concurrent recorders.
        t->file=CreateFileW(wide,GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(t->file!=INVALID_HANDLE_VALUE)return true;
        t->file=NULL;DWORD error=GetLastError();if(error!=ERROR_FILE_EXISTS && error!=ERROR_ALREADY_EXISTS)return false;
    }
    return false;
}
static bool check(struct ea_mp3_recorder *w,int result,const char *step)
{
    if(result>=0)return true;char error[AV_ERROR_MAX_STRING_SIZE];av_strerror(result,error,sizeof(error));
    blog(LOG_ERROR,"[extra-audio] MP3 contingency %s: %s",step,error);w->failed=true;return false;
}
static bool flush_tail(struct ea_mp3_recorder *w,struct mp3_track *t,int64_t before,bool final)
{
    if(final && t->written_end>w->final_frames)return check(w,AVERROR(EINVAL),"native end precedes committed audio");
    while(t->tail_count) {
        AVPacket *p=t->tail[t->tail_read];
        if(!final && p->pts+p->duration>before)break;
        if(p->pts<w->final_frames || !final) {
            if(final && p->pts+p->duration>=w->final_frames) {
                int64_t cut=p->pts+p->duration-w->final_frames;size_t size;
                uint8_t *skip=av_packet_get_side_data(p,AV_PKT_DATA_SKIP_SAMPLES,&size);
                if(!skip) {skip=av_packet_new_side_data(p,AV_PKT_DATA_SKIP_SAMPLES,10);if(skip)memset(skip,0,10);}
                if(!skip)return check(w,AVERROR(ENOMEM),"end padding");
                AV_WL32(skip+4,AV_RL32(skip+4)+(uint32_t)cut);p->duration-=cut;
            }
            t->written_end=p->pts+p->duration;
            av_packet_rescale_ts(p,t->codec->time_base,t->stream->time_base);p->stream_index=0;
            if(!check(w,av_interleaved_write_frame(t->output,p),"write"))return false;
        }
        av_packet_free(&p);t->tail[t->tail_read]=NULL;t->tail_read=(t->tail_read+1)%MP3_TAIL_PACKETS;--t->tail_count;
    }
    return true;
}
static bool drain(struct ea_mp3_recorder *w,struct mp3_track *t)
{
    for(;;) {
        int result=avcodec_receive_packet(t->codec,w->packet);if(result==AVERROR(EAGAIN) || result==AVERROR_EOF)return true;
        if(!check(w,result,"receive"))return false;
        if(t->tail_count==MP3_TAIL_PACKETS)return check(w,AVERROR(ENOMEM),"packet tail");
        AVPacket *p=av_packet_clone(w->packet);av_packet_unref(w->packet);if(!p)return check(w,AVERROR(ENOMEM),"packet");
        t->tail[(t->tail_read+t->tail_count)%MP3_TAIL_PACKETS]=p;++t->tail_count;
        if(!flush_tail(w,t,p->pts-t->codec->sample_rate,false))return false;
    }
}
static bool send(struct ea_mp3_recorder *w,struct mp3_track *t,int64_t end)
{
    if(!t->buffered)return true;t->frame->pts=end-t->buffered;t->frame->nb_samples=(int)t->buffered;
    if(!check(w,avcodec_send_frame(t->codec,t->frame),"encode") || !drain(w,t) || !check(w,av_frame_make_writable(t->frame),"buffer"))return false;
    t->buffered=0;return true;
}
struct ea_mp3_recorder *ea_mp3_open(const char *directory,const char *pattern,const char *recording,
    unsigned rate,const struct ea_extra_track *tracks,unsigned count)
{
    const AVCodec *codec=avcodec_find_encoder_by_name("libmp3lame");if(!codec || !count || count>EA_MAX_EXTRA_TRACKS)return NULL;
    if(os_mkdirs(directory)<0 && !os_file_exists(directory))return NULL;
    struct ea_mp3_recorder *w=av_mallocz(sizeof(*w));if(!w)return NULL;
    w->packet=av_packet_alloc();if(!w->packet)goto fail;
    time_t started=recording_start(recording);
    for(unsigned i=0;i<count;++i) {
        struct mp3_track *t=&w->tracks[w->count++];char path[8192];
        t->codec=avcodec_alloc_context3(codec);t->frame=av_frame_alloc();if(!t->codec || !t->frame)goto fail;
        t->codec->sample_rate=(int)rate;t->codec->sample_fmt=AV_SAMPLE_FMT_FLTP;t->codec->bit_rate=tracks[i].bitrate*1000;
        t->codec->time_base=(AVRational){1,(int)rate};t->codec->thread_count=1;av_channel_layout_default(&t->codec->ch_layout,2);
        if(!check(w,avcodec_open2(t->codec,codec,NULL),"open encoder") || !check(w,avformat_alloc_output_context2(&t->output,NULL,"mp3",NULL),"container"))goto fail;
        t->stream=avformat_new_stream(t->output,NULL);if(!t->stream)goto fail;t->stream->time_base=t->codec->time_base;
        if(!check(w,avcodec_parameters_from_context(t->stream->codecpar,t->codec),"stream"))goto fail;
        char title[320];snprintf(title,sizeof(title),*tracks[i].name?"Track %u - %s":"Track %u",tracks[i].number,tracks[i].name);
        av_dict_set(&t->output->metadata,"title",title,0);
        if(!open_new(t,path,sizeof(path),directory,pattern,recording,&tracks[i],started))goto fail;
        uint8_t *io_buffer=av_malloc(32768);if(!io_buffer)goto fail;
        t->output->pb=avio_alloc_context(io_buffer,32768,1,t,NULL,file_write,file_seek);
        if(!t->output->pb) {av_free(io_buffer);goto fail;}
        t->output->flags|=AVFMT_FLAG_CUSTOM_IO;
        if(!check(w,avformat_write_header(t->output,NULL),"header"))goto fail;
        t->header=true;t->frame->format=t->codec->sample_fmt;t->frame->sample_rate=(int)rate;t->frame->nb_samples=t->codec->frame_size;
        if(!check(w,av_channel_layout_copy(&t->frame->ch_layout,&t->codec->ch_layout),"layout") || !check(w,av_frame_get_buffer(t->frame,0),"frame"))goto fail;
        blog(LOG_INFO,"[extra-audio] MP3 contingency: %s",path);
    }
    return w;
fail:
    w->failed=true;ea_mp3_close(w);return NULL;
}
// OBS limits audio to -1.0..1.0 before its encoders take it, so the embedded
// AAC tracks are limited too; do the same here so backups match them.
static void copy_limited(float *target,const float *source,unsigned count)
{
    for(unsigned n=0;n<count;++n) {
        float value=source[n];
        target[n]=value>1.0f?1.0f:value<-1.0f?-1.0f:value;
    }
}
bool ea_mp3_append(struct ea_mp3_recorder *w,const struct ea_stem_block *block,unsigned start,unsigned count)
{
    if(w->failed || start>AUDIO_OUTPUT_FRAMES || count>AUDIO_OUTPUT_FRAMES-start)return false;
    unsigned original=count;
    for(unsigned i=0;i<w->count;++i) {
        struct mp3_track *t=&w->tracks[i];unsigned offset=start,left=count;int64_t position=w->position;
        while(left) {
            unsigned take=(unsigned)t->codec->frame_size-t->buffered;if(take>left)take=left;
            for(unsigned c=0;c<2;++c) {
                float *target=(float *)t->frame->data[c]+t->buffered;
                if(block)copy_limited(target,block->pcm[i][c]+offset,take);else memset(target,0,take*sizeof(float));
            }
            t->buffered+=take;offset+=take;left-=take;position+=take;
            if(t->buffered==(unsigned)t->codec->frame_size && !send(w,t,position))return false;
        }
    }
    w->position+=original;return true;
}
bool ea_mp3_finish(struct ea_mp3_recorder *w,int64_t frames)
{
    if(!w)return false;
    w->finishing=true;w->final_frames=frames;
    for(unsigned i=0;i<w->count;++i) {
        struct mp3_track *t=&w->tracks[i];
        if(t->header && !w->failed) {
            if(send(w,t,w->position) && check(w,avcodec_send_frame(t->codec,NULL),"flush"))drain(w,t);
            if(!w->failed && flush_tail(w,t,0,true))check(w,av_write_trailer(t->output),"finish");
        }
        if(t->output && t->output->pb) {
            avio_flush(t->output->pb);check(w,t->output->pb->error,"close");
            av_freep(&t->output->pb->buffer);avio_context_free(&t->output->pb);
        }
        if(t->file && !CloseHandle(t->file))w->failed=true;
        for(unsigned n=0;n<MP3_TAIL_PACKETS;++n)av_packet_free(&t->tail[n]);
        av_frame_free(&t->frame);avcodec_free_context(&t->codec);avformat_free_context(t->output);
    }
    bool ok=!w->failed;av_packet_free(&w->packet);av_free(w);return ok;
}
bool ea_mp3_close(struct ea_mp3_recorder *w) {return w && ea_mp3_finish(w,w->position);}
static bool decoded_frames(struct ea_mp3_recorder *writer,AVCodecContext *decoder,AVFrame *frame,
    struct ea_stem_block *block,AVRational timebase,unsigned rate,int64_t limit,int64_t *position)
{
    for(;;) {
        int result=avcodec_receive_frame(decoder,frame);
        if(result==AVERROR(EAGAIN) || result==AVERROR_EOF)return true;
        if(result<0 || frame->format!=AV_SAMPLE_FMT_FLTP || frame->sample_rate!=(int)rate || frame->ch_layout.nb_channels!=2)return false;
        int64_t at=frame->pts==AV_NOPTS_VALUE?*position:av_rescale_q(frame->pts,timebase,(AVRational){1,(int)rate});
        unsigned skip=at<*position?(unsigned)(*position-at):0;
        if(at>limit)at=limit;
        while(*position<at) {
            unsigned take=at-*position>AUDIO_OUTPUT_FRAMES?AUDIO_OUTPUT_FRAMES:(unsigned)(at-*position);
            if(!ea_mp3_append(writer,NULL,0,take))return false;*position+=take;
        }
        while(skip<(unsigned)frame->nb_samples && *position<limit) {
            unsigned take=(unsigned)frame->nb_samples-skip;if(take>AUDIO_OUTPUT_FRAMES)take=AUDIO_OUTPUT_FRAMES;
            if((int64_t)take>limit-*position)take=(unsigned)(limit-*position);
            for(unsigned c=0;c<2;++c)memcpy(block->pcm[0][c],(float *)frame->extended_data[c]+skip,take*sizeof(float));
            if(!ea_mp3_append(writer,block,0,take))return false;skip+=take;*position+=take;
        }
        av_frame_unref(frame);
    }
}
bool ea_mp3_from_aac(const char *audio,const char *directory,const char *pattern,const char *recording,
    unsigned rate,const struct ea_extra_track *tracks,unsigned count,int64_t frames)
{
    bool all_ok=true;struct ea_stem_block *block=bmalloc(sizeof(*block));
    for(unsigned i=0;i<count;++i) {
        AVFormatContext *in=NULL;AVCodecContext *decoder=NULL;AVPacket *packet=av_packet_alloc();AVFrame *frame=av_frame_alloc();
        struct ea_mp3_recorder *writer=NULL;bool ok=false;int64_t position=0;
        if(!packet || !frame || avformat_open_input(&in,audio,NULL,NULL)<0 || i>=in->nb_streams)goto cleanup;
        AVStream *stream=in->streams[i];const AVCodec *codec=avcodec_find_decoder(stream->codecpar->codec_id);
        decoder=avcodec_alloc_context3(codec);if(!decoder || avcodec_parameters_to_context(decoder,stream->codecpar)<0)goto cleanup;
        decoder->pkt_timebase=stream->time_base;decoder->thread_count=1;if(avcodec_open2(decoder,codec,NULL)<0)goto cleanup;
        for(unsigned s=0;s<in->nb_streams;++s)if(s!=i)in->streams[s]->discard=AVDISCARD_ALL;
        writer=ea_mp3_open(directory,pattern,recording,rate,&tracks[i],1);if(!writer)goto cleanup;
        int result;
        while((result=av_read_frame(in,packet))>=0) {
            if(packet->stream_index==(int)i && (avcodec_send_packet(decoder,packet)<0 ||
                !decoded_frames(writer,decoder,frame,block,stream->time_base,rate,frames,&position))) {av_packet_unref(packet);goto cleanup;}
            av_packet_unref(packet);
        }
        if(result!=AVERROR_EOF || avcodec_send_packet(decoder,NULL)<0 || !decoded_frames(writer,decoder,frame,block,stream->time_base,rate,frames,&position))goto cleanup;
        while(position<frames) {
            unsigned take=frames-position>AUDIO_OUTPUT_FRAMES?AUDIO_OUTPUT_FRAMES:(unsigned)(frames-position);
            if(!ea_mp3_append(writer,NULL,0,take))goto cleanup;position+=take;
        }
        ok=ea_mp3_finish(writer,frames);writer=NULL;
cleanup:
        if(writer)ea_mp3_close(writer);av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&decoder);avformat_close_input(&in);
        if(!ok) {blog(LOG_ERROR,"[extra-audio] Replay MP3 contingency failed for Track %u: %s",tracks[i].number,recording);all_ok=false;}
    }
    bfree(block);return all_ok;
}
