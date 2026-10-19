// SPDX-License-Identifier: GPL-2.0-or-later
// Append AAC media and a replacement index. Existing video/audio bytes stay put.
#include "mp4-inplace.h"
#include <libavutil/error.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#include <libavformat/avio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define INDEX_LIMIT (512u * 1024u * 1024u)
struct box { uint64_t at, size; unsigned header; char type[4]; };
struct bytes { unsigned char *data; size_t size, capacity; };
struct journal {
	char magic[8];
	uint64_t original_size, moov, moov_size;
	DWORD volume, index_high, index_low;
	unsigned char sha[32];
};
static const unsigned char marker[16]={0x5c,0xfa,0xc8,0x21,0x47,0x34,0x4e,0x89,0xb4,0x73,0xea,0x08,0x00,0x00,0x00,0x01};

static uint32_t u32(const unsigned char *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t u64(const unsigned char *p) { return (uint64_t)u32(p)<<32 | u32(p+4); }
static void put32(unsigned char *p, uint32_t n) { p[0]=(unsigned char)(n>>24); p[1]=(unsigned char)(n>>16); p[2]=(unsigned char)(n>>8); p[3]=(unsigned char)n; }
static void put64(unsigned char *p, uint64_t n) { put32(p,(uint32_t)(n>>32)); put32(p+4,(uint32_t)n); }
static bool fail(const char *text) { fprintf(stderr,"Extra Audio in-place MP4: %s (Windows error %lu)\n",text,GetLastError()); return false; }

static bool io(HANDLE file, uint64_t at, void *data, DWORD count, bool write)
{
	if (at > INT64_MAX) return false;
	LARGE_INTEGER offset; offset.QuadPart=(LONGLONG)at;
	DWORD done=0;
	return SetFilePointerEx(file,offset,NULL,FILE_BEGIN) &&
		(write ? WriteFile(file,data,count,&done,NULL) : ReadFile(file,data,count,&done,NULL)) && done==count;
}

static bool file_box(HANDLE file, uint64_t at, uint64_t end, struct box *b)
{
	unsigned char h[16];
	if (at>end || end-at<8 || !io(file,at,h,8,false)) return false;
	b->at=at; b->size=u32(h); b->header=8; memcpy(b->type,h+4,4);
	if (b->size==1) { if(end-at<16 || !io(file,at+8,h+8,8,false)) return false; b->size=u64(h+8); b->header=16; }
	if (!b->size) b->size=end-at;
	return b->size>=b->header && b->size<=end-at;
}

static bool memory_box(const unsigned char *data, size_t at, size_t end, struct box *b)
{
	if (at>end || end-at<8) return false;
	b->at=at; b->size=u32(data+at); b->header=8; memcpy(b->type,data+at+4,4);
	if(b->size==1) { if(end-at<16) return false; b->size=u64(data+at+8); b->header=16; }
	if (!b->size) b->size=end-at;
	return b->size>=b->header && b->size<=end-at;
}

static bool push(struct bytes *b, const void *data, size_t count)
{
	if (count>INDEX_LIMIT || b->size>INDEX_LIMIT-count) return false;
	if (b->size+count>b->capacity) {
		size_t capacity=b->capacity?b->capacity:4096;
		while(capacity<b->size+count) capacity=capacity>INDEX_LIMIT/2?INDEX_LIMIT:capacity*2;
		unsigned char *next=realloc(b->data,capacity); if(!next) return false;
		b->data=next; b->capacity=capacity;
	}
	memcpy(b->data+b->size,data,count); b->size+=count; return true;
}

static void journal_hash(struct journal *j, unsigned char result[32])
{
	struct AVSHA *sha=av_sha_alloc(); av_sha_init(sha,256);
	av_sha_update(sha,(const unsigned char *)j,offsetof(struct journal,sha)); av_sha_final(sha,result); av_free(sha);
}

static bool restore(struct ea_mp4_edit *e, struct journal *j)
{
	unsigned char h[16], hash[32]; BY_HANDLE_FILE_INFORMATION info; LARGE_INTEGER length;
	journal_hash(j,hash);
	if (memcmp(j->magic,"EAMP4J01",8) || memcmp(hash,j->sha,32) ||
		!GetFileInformationByHandle(e->file,&info) || !GetFileSizeEx(e->file,&length) ||
		info.dwVolumeSerialNumber!=j->volume || info.nFileIndexHigh!=j->index_high || info.nFileIndexLow!=j->index_low ||
		j->original_size>(uint64_t)length.QuadPart || j->moov_size<8 || j->moov_size>j->original_size || j->moov>j->original_size-j->moov_size ||
		!io(e->file,j->moov,h,8,false) || (memcmp(h+4,"moov",4) && memcmp(h+4,"free",4)))
		return fail("Recovery journal does not match this file; no changes made");
	uint64_t size=u32(h);
	if(size==1) { if(!io(e->file,j->moov+8,h+8,8,false)) return false; size=u64(h+8); }
	if(size!=j->moov_size) return fail("Original index size changed; no recovery changes made");
	if(!io(e->file,j->moov+4,"moov",4,true) || !FlushFileBuffers(e->file)) return false;
	LARGE_INTEGER end; end.QuadPart=(LONGLONG)j->original_size;
	if(!SetFilePointerEx(e->file,end,NULL,FILE_BEGIN) || !SetEndOfFile(e->file) || !FlushFileBuffers(e->file)) return false;
	return DeleteFileW(e->journal)!=0;
}

bool ea_mp4_begin(struct ea_mp4_edit *e, HANDLE file, const char *path)
{
	memset(e,0,sizeof(*e)); e->file=file;
	wchar_t native[32768];
	if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,native,32768) || wcslen(native)>32720) return false;
	swprintf(e->journal,32768,L"%ls.extra-audio-transaction",native);
	HANDLE prior=CreateFileW(e->journal,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
	if(prior!=INVALID_HANDLE_VALUE) {
		struct journal j; LARGE_INTEGER n;
		bool valid=GetFileSizeEx(prior,&n) && n.QuadPart==sizeof(j) && io(prior,0,&j,sizeof(j),false); CloseHandle(prior);
		if(!valid || !restore(e,&j)) return fail("Could not restore interrupted append");
		fprintf(stderr,"Restored original MP4 after interrupted append.\n");
	} else if(GetLastError()!=ERROR_FILE_NOT_FOUND) return fail("Cannot inspect recovery journal");
	LARGE_INTEGER length; if(!GetFileSizeEx(file,&length) || length.QuadPart<8) return false;
	e->original_size=e->size=(uint64_t)length.QuadPart;
	for(uint64_t at=0;at<e->original_size;) {
		struct box b; if(!file_box(file,at,e->original_size,&b)) return fail("Invalid original MP4 box");
		if(!memcmp(b.type,"moof",4)) return fail("Recording is still fragmented; wait for OBS to finalize it");
		if(!memcmp(b.type,"moov",4)) {
			if(e->moov || b.size>INDEX_LIMIT) return fail("Unexpected or oversized original MP4 index");
			e->old_moov=at; e->old_moov_size=b.size; e->moov=malloc((size_t)b.size);
			if(!e->moov || !io(file,at,e->moov,(DWORD)b.size,false)) return false;
			if(!u32(e->moov)) return fail("Original index needs an explicit size for safe recovery");
			for(size_t p=b.header;p<b.size;) {
				struct box child; if(!memory_box(e->moov,p,(size_t)b.size,&child)) return false;
				if(!memcmp(child.type,"mvex",4)) return fail("Unfinished fragmented MP4 index");
				if(!memcmp(child.type,"trak",4)) ++e->track_count;
				if(!memcmp(child.type,"uuid",4) && child.size-child.header>=16 && !memcmp(e->moov+p+child.header,marker,16)) e->completed=true;
				if(!memcmp(child.type,"mvhd",4)) {
					if(child.size-child.header<4 || e->moov[p+child.header]>1) return false;
					size_t offset=p+child.header+(e->moov[p+child.header]==1 ? 20 : 12);
					if(offset+4>p+child.size) return false; e->timescale=u32(e->moov+offset);
				}
				p+=(size_t)child.size;
			}
		}
		at+=b.size;
	}
	return e->moov && e->timescale && e->track_count;
}

static bool rewrite_track(struct bytes *out, const unsigned char *data, size_t size,
	uint64_t audio_start, uint64_t audio_size, uint64_t native_start, uint32_t id, unsigned depth)
{
	if(depth>5) return false;
	for(size_t at=0;at<size;) {
		struct box b; if(!memory_box(data,at,size,&b)) return false;
		const unsigned char *payload=data+at+b.header; size_t length=(size_t)b.size-b.header;
		if(!memcmp(b.type,"trak",4) || !memcmp(b.type,"mdia",4) || !memcmp(b.type,"minf",4) || !memcmp(b.type,"stbl",4)) {
			size_t start=out->size; unsigned char h[8]={0}; memcpy(h+4,b.type,4);
			if(!push(out,h,8) || !rewrite_track(out,payload,length,audio_start,audio_size,native_start,id,depth+1)) return false;
			put32(out->data+start,(uint32_t)(out->size-start));
		} else if(!memcmp(b.type,"stco",4) || !memcmp(b.type,"co64",4)) {
			bool wide=!memcmp(b.type,"co64",4); if(length<8) return false;
			uint32_t count=u32(payload+4); if((uint64_t)count*(wide?8:4)+8!=length || count>(INDEX_LIMIT-16)/8) return false;
			unsigned char h[16]={0}; put32(h,16+8*count); memcpy(h+4,"co64",4); memcpy(h+8,payload,8);
			if(!push(out,h,16)) return false;
			for(uint32_t i=0;i<count;++i) {
				uint64_t offset=wide?u64(payload+8+8*i):u32(payload+8+4*i);
				if(offset<audio_start || offset-audio_start>=audio_size || native_start>INT64_MAX-(offset-audio_start)) return false;
				unsigned char chunk[8]; put64(chunk,native_start+offset-audio_start); if(!push(out,chunk,8)) return false;
			}
		} else {
			size_t start=out->size; if(!push(out,data+at,(size_t)b.size)) return false;
			if(!memcmp(b.type,"tkhd",4)) {
				if(length<4 || payload[0]>1) return false;
				size_t offset=b.header+(payload[0]==1?20:12); if(offset+4>b.size) return false;
				put32(out->data+start+offset,id);
			}
		}
		at+=(size_t)b.size;
	}
	return true;
}

bool ea_mp4_append(struct ea_mp4_edit *e, const char *audio_path)
{
	if(e->completed || e->pending) return fail("Recording already contains extra tracks or an unfinished append");
	wchar_t wide[32768]; if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,audio_path,-1,wide,32768)) return false;
	HANDLE audio=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
	if(audio==INVALID_HANDLE_VALUE) return false;
	struct bytes moov={0}; struct box audio_mdat={0}, audio_moov={0}; LARGE_INTEGER length;
	bool ok=GetFileSizeEx(audio,&length)!=0; unsigned char *audio_index=NULL; uint32_t max_id=0, extra=0;
	for(uint64_t at=0;ok && at<(uint64_t)length.QuadPart;) {
		struct box b; ok=file_box(audio,at,(uint64_t)length.QuadPart,&b); if(!ok) break;
		if(!memcmp(b.type,"mdat",4)) { if(audio_mdat.size) { ok=false; break; } audio_mdat=b; }
		if(!memcmp(b.type,"moov",4)) audio_moov=b;
		at+=b.size;
	}
	if(!audio_mdat.size || !audio_moov.size || audio_moov.size>INDEX_LIMIT) ok=false;
	if(ok) { audio_index=malloc((size_t)audio_moov.size); ok=audio_index && io(audio,audio_moov.at,audio_index,(DWORD)audio_moov.size,false); }
	unsigned native_header=u32(e->moov)==1?16:8;
	for(size_t p=native_header;ok && p<e->old_moov_size;) {
		struct box b; ok=memory_box(e->moov,p,(size_t)e->old_moov_size,&b); if(!ok) break;
		if(!memcmp(b.type,"trak",4)) for(size_t q=p+b.header;q<p+b.size;) {
			struct box c; if(!memory_box(e->moov,q,(size_t)(p+b.size),&c)) { ok=false; break; }
			if(!memcmp(c.type,"tkhd",4)) {
				if(c.size-c.header<4 || e->moov[q+c.header]>1) { ok=false; break; }
				size_t offset=q+c.header+(e->moov[q+c.header]==1?20:12);
				if(offset+4>q+c.size) { ok=false; break; } uint32_t id=u32(e->moov+offset); if(id>max_id) max_id=id;
			}
			q+=(size_t)c.size;
		}
		p+=(size_t)b.size;
	}
	unsigned char head[8]={0}; memcpy(head+4,"moov",4);
	if(ok) ok=push(&moov,head,8) && push(&moov,e->moov+native_header,(size_t)e->old_moov_size-native_header);
	for(size_t p=audio_moov.header;ok && p<audio_moov.size;) {
		struct box b; ok=memory_box(audio_index,p,(size_t)audio_moov.size,&b); if(!ok) break;
		if(!memcmp(b.type,"mvhd",4)) {
			if(b.size-b.header<4 || audio_index[p+b.header]>1) { ok=false; break; }
			size_t offset=p+b.header+(audio_index[p+b.header]==1?20:12);
			if(offset+4>p+b.size || u32(audio_index+offset)!=e->timescale) ok=false;
		}
		if(!memcmp(b.type,"trak",4)) {
			if(max_id==UINT32_MAX || extra==10) { ok=false; break; }
			ok=rewrite_track(&moov,audio_index+p,(size_t)b.size,audio_mdat.at+audio_mdat.header,
				audio_mdat.size-audio_mdat.header,e->original_size+16,++max_id,0); ++extra;
		}
		p+=(size_t)b.size;
	}
	if(!extra || max_id==UINT32_MAX) ok=false;
	if(ok) { unsigned char uuid[24]; put32(uuid,24); memcpy(uuid+4,"uuid",4); memcpy(uuid+8,marker,16); ok=push(&moov,uuid,24); }
	if(ok) {
		put32(moov.data,(uint32_t)moov.size);
		for(size_t p=8;p<moov.size;) { struct box b; if(!memory_box(moov.data,p,moov.size,&b)) { ok=false; break; }
			if(!memcmp(b.type,"mvhd",4)) put32(moov.data+p+b.size-4,max_id+1); p+=(size_t)b.size; }
	}
	// Native track/index boxes must be byte-identical. Only mvhd.next_track_id
	// changes. This proves preservation without re-reading a long recording's video.
	for(size_t p=native_header;ok && p<e->old_moov_size;) {
		struct box b; ok=memory_box(e->moov,p,(size_t)e->old_moov_size,&b); if(!ok) break;
		size_t length=(size_t)b.size; if(!memcmp(b.type,"mvhd",4)) length-=4;
		if(memcmp(e->moov+p,moov.data+8+p-native_header,length)) ok=false;
		p+=(size_t)b.size;
	}
	uint64_t payload=audio_mdat.size-audio_mdat.header;
	if(e->original_size>INT64_MAX-16 || payload>INT64_MAX-e->original_size-16 || moov.size>INT64_MAX-e->original_size-16-payload) ok=false;
	if(ok) {
		struct journal j={0}; memcpy(j.magic,"EAMP4J01",8); j.original_size=e->original_size; j.moov=e->old_moov; j.moov_size=e->old_moov_size;
		BY_HANDLE_FILE_INFORMATION info={0}; ok=GetFileInformationByHandle(e->file,&info)!=0;
		j.volume=info.dwVolumeSerialNumber; j.index_high=info.nFileIndexHigh; j.index_low=info.nFileIndexLow; journal_hash(&j,j.sha);
		HANDLE journal=ok?CreateFileW(e->journal,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL):INVALID_HANDLE_VALUE;
		if(journal==INVALID_HANDLE_VALUE) ok=false;
		else { ok=io(journal,0,&j,sizeof(j),true) && FlushFileBuffers(journal); CloseHandle(journal); e->pending=ok; if(!ok) DeleteFileW(e->journal); }
	}
	if(ok) {
		unsigned char mdat[16]; put32(mdat,1); memcpy(mdat+4,"mdat",4); put64(mdat+8,payload+16);
		ok=io(e->file,e->original_size,mdat,16,true);
		unsigned char *buffer=malloc(1024*1024); if(!buffer) ok=false;
		for(uint64_t copied=0;ok && copied<payload;) {
			DWORD count=(DWORD)((payload-copied)>1024*1024?1024*1024:payload-copied);
			ok=io(audio,audio_mdat.at+audio_mdat.header+copied,buffer,count,false) && io(e->file,e->original_size+16+copied,buffer,count,true); copied+=count;
		}
		free(buffer);
		if(ok) ok=io(e->file,e->original_size+16+payload,moov.data,(DWORD)moov.size,true) && FlushFileBuffers(e->file);
		if(ok) { e->size=e->original_size+16+payload+moov.size; e->position=0; e->appended=true; }
	}
	CloseHandle(audio); free(audio_index); free(moov.data);
	return ok || fail("Could not append audio/index; original index remains available");
}

int ea_mp4_read(void *opaque, uint8_t *buffer, int count)
{
	struct ea_mp4_edit *e=opaque; if(e->position>=e->size) return AVERROR_EOF;
	if((uint64_t)count>e->size-e->position) count=(int)(e->size-e->position);
	if(!io(e->file,e->position,buffer,(DWORD)count,false)) return AVERROR(EIO);
	if(e->position<e->original_size) e->native_view_read_bytes+=(uint64_t)count<e->original_size-e->position?(uint64_t)count:e->original_size-e->position;
	for(unsigned i=0;i<4;++i) if(e->old_moov+4+i>=e->position && e->old_moov+4+i-e->position<(uint64_t)count)
		buffer[e->old_moov+4+i-e->position]=(uint8_t)"free"[i];
	e->position+=(unsigned)count; return count;
}

int64_t ea_mp4_seek(void *opaque, int64_t offset, int whence)
{
	struct ea_mp4_edit *e=opaque; if(whence==AVSEEK_SIZE) return (int64_t)e->size;
	whence &= ~AVSEEK_FORCE;
	int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)e->position:whence==SEEK_END?(int64_t)e->size:-1;
	if(base<0 || offset < -base || (offset>0 && base>INT64_MAX-offset)) return AVERROR(EINVAL);
	e->position=(uint64_t)(base+offset); return (int64_t)e->position;
}

bool ea_mp4_commit(struct ea_mp4_edit *e)
{
	if(!e->pending || !e->appended) return false;
	if(!io(e->file,e->old_moov+4,"free",4,true) || !FlushFileBuffers(e->file)) return fail("Could not activate verified index");
	if(!DeleteFileW(e->journal)) return fail("Could not clear recovery journal");
	e->pending=false; return true;
}

bool ea_mp4_rollback(struct ea_mp4_edit *e)
{
	if(!e->pending) return true;
	HANDLE file=CreateFileW(e->journal,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
	struct journal j; bool ok=file!=INVALID_HANDLE_VALUE && io(file,0,&j,sizeof(j),false);
	if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
	if(ok) ok=restore(e,&j);
	if(ok) e->pending=false;
	return ok;
}
