// SPDX-License-Identifier: GPL-2.0-or-later
// Attach AAC already encoded during recording. No PCM spool or second encode.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <obs.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#include "mp4-inplace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct finalizer { unsigned native_count, stem_count; unsigned char *expected_hash; };
static struct ea_mp4_edit edit;
static void rollback_on_exit(void) { if(edit.pending && !ea_mp4_rollback(&edit)) fprintf(stderr,"Rollback needs the retained job file.\n"); }
static void check(int result,const char *step) {
    if(result>=0) return; char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result,error,sizeof(error));
    fprintf(stderr,"Extra Audio finalizer %s: %s\n",step,error); exit(1);
}
static void hash_packets(AVFormatContext *context,unsigned char *hash) {
    struct AVSHA **sha=av_calloc(context->nb_streams,sizeof(*sha));
    if(!sha) check(AVERROR(ENOMEM),"hash allocation");
    for(unsigned i=0;i<context->nb_streams;++i) { sha[i]=av_sha_alloc(); av_sha_init(sha[i],256); }
    AVPacket *packet=av_packet_alloc(); int result;
    while((result=av_read_frame(context,packet))>=0) { av_sha_update(sha[packet->stream_index],packet->data,packet->size); av_packet_unref(packet); }
    if(result!=AVERROR_EOF) check(result,"read payloads");
    for(unsigned i=0;i<context->nb_streams;++i) { av_sha_final(sha[i],hash+32*i); av_free(sha[i]); }
    av_free(sha); av_packet_free(&packet);
}

static void validate(struct finalizer *f)
{
	AVFormatContext *context = avformat_alloc_context();
	unsigned char *buffer=av_malloc(65536);
	AVIOContext *io=avio_alloc_context(buffer,65536,0,&edit,ea_mp4_read,NULL,ea_mp4_seek);
	if(!context || !buffer || !io) check(AVERROR(ENOMEM),"allocating verification view");
	context->pb=io; context->flags |= AVFMT_FLAG_CUSTOM_IO;
	check(avformat_open_input(&context, NULL, NULL, NULL), "verifying appended MP4 before activating its index");
	for(unsigned i=0;i<f->native_count && i<context->nb_streams;++i) context->streams[i]->discard=AVDISCARD_ALL;
	check(avformat_find_stream_info(context, NULL), "reading completed MP4 stream information");
	if (context->nb_streams != f->native_count + f->stem_count) {
		fprintf(stderr, "Extra Audio finalizer: unexpected track count in completed file\n"); exit(1);
	}
	unsigned count=f->stem_count;
	struct AVSHA **sha = av_calloc(count, sizeof(*sha));
	for (unsigned i = 0; i < count; ++i) { sha[i] = av_sha_alloc(); av_sha_init(sha[i], 256); }
	AVPacket *packet = av_packet_alloc();
	int result;
	while ((result = av_read_frame(context, packet)) >= 0) {
		if ((unsigned)packet->stream_index >= f->native_count && (unsigned)packet->stream_index < f->native_count+count)
			av_sha_update(sha[packet->stream_index-f->native_count], packet->data, packet->size);
		av_packet_unref(packet);
	}
	if (result != AVERROR_EOF) check(result, "verifying media payloads");
	for (unsigned i = 0; i < count; ++i) {
		unsigned char hash[32]; av_sha_final(sha[i], hash); av_free(sha[i]);
		if (memcmp(hash, f->expected_hash + 32 * i, 32)) {
			fprintf(stderr, "Extra Audio finalizer: media payload verification failed for stream %u\n", i); exit(1);
		}
	}
	av_free(sha); av_packet_free(&packet); avformat_close_input(&context); av_freep(&io->buffer); avio_context_free(&io);
}


int main(int argc,char **argv)
{
    bool recover=argc==3 && !strcmp(argv[1],"--recover");
    if(argc!=2 && !recover) { fprintf(stderr,"Usage: obs-extra-audio-mux.exe [--recover] recording-job.json\n"); return 2; }
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS); atexit(rollback_on_exit);
    obs_data_t *job=obs_data_create_from_json_file(argv[recover?2:1]); if(!job) return 2;
    if(!recover && obs_data_get_bool(job,"capture_failed")) return 2;
    const char *native=obs_data_get_string(job,"native_path"), *audio=obs_data_get_string(job,"audio_path");
    if(!*native || (!recover && !*audio)) { fprintf(stderr,"The job names no recording, or no captured audio to add.\n"); return 2; }
    wchar_t path[32768]; if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,native,-1,path,32768)) return 2;
    HANDLE file=INVALID_HANDLE_VALUE;
    for(unsigned retry=0;retry<600;++retry) {
        file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(file!=INVALID_HANDLE_VALUE || GetLastError()!=ERROR_SHARING_VIOLATION) break; Sleep(100);
    }
    if(file==INVALID_HANDLE_VALUE || !ea_mp4_begin(&edit,file,native)) return 1;
    if(recover || edit.completed) { CloseHandle(file); obs_data_release(job); return 0; }
    AVFormatContext *input=NULL,*aac=NULL;
    check(avformat_open_input(&input,native,NULL,NULL),"open native index");
    check(avformat_open_input(&aac,audio,NULL,NULL),"open compressed audio"); check(avformat_find_stream_info(aac,NULL),"audio info");
    struct finalizer f={.native_count=input->nb_streams,.stem_count=aac->nb_streams};
    if(!f.stem_count || f.stem_count>10 || f.native_count!=edit.track_count) return 2;
    unsigned native_audio=0;
    for(unsigned i=0;i<input->nb_streams;++i) if(input->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) ++native_audio;
    for(unsigned i=0;i<aac->nb_streams;++i) if(aac->streams[i]->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO || aac->streams[i]->codecpar->codec_id!=AV_CODEC_ID_AAC) return 2;
    f.expected_hash=av_calloc(f.stem_count,32); if(!f.expected_hash) return 1;
    hash_packets(aac,f.expected_hash);
    avformat_close_input(&input); avformat_close_input(&aac);
    if(!ea_mp4_append(&edit,audio)) return 1;
    validate(&f);
    if(!ea_mp4_commit(&edit)) return 1;
    CloseHandle(file);
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,audio,-1,path,32768)) DeleteFileW(path);
    printf("PASS: %u native audio + %u live-encoded AAC tracks embedded in place; original index preserved, AAC SHA-256 verified; native view read %llu bytes; no PCM spool or second encode\n",native_audio,f.stem_count,(unsigned long long)edit.native_view_read_bytes);
    if(obs_data_get_bool(job,"replay_clip") && MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[1],-1,path,32768))DeleteFileW(path);
    av_free(f.expected_hash); free(edit.moov); obs_data_release(job); return 0;
}
