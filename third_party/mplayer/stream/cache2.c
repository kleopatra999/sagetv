#include "config.h"

// Initial draft of my new cache system...
// Note it runs in 2 processes (using fork()), but doesn't requires locking!!
// TODO: seeking, data consistency checking

#define READ_USLEEP_TIME 10000
#define FILL_USLEEP_TIME 50000
#define PREFILL_SLEEP_TIME 200

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "osdep/timer.h"
#ifndef WIN32
#include <sys/wait.h>
#include "osdep/shmem.h"
#include <pthread.h>
#else
#include <windows.h>
static DWORD WINAPI ThreadProc(void* s);
#endif

#include "mp_msg.h"
#include "help_mp.h"

#include "stream.h"
#include "input/input.h"

int stream_fill_buffer(stream_t *s);
int stream_seek_long(stream_t *s,off_t pos);

static int min_fill=0;

int cache_fill_status=0;

void cache_stats(cache_vars_t* s){
  int newb=s->max_filepos-s->read_filepos; // new bytes in the buffer
  mp_msg(MSGT_CACHE,MSGL_INFO,"0x%06X  [0x%06X]  0x%06X   ",(int)s->min_filepos,(int)s->read_filepos,(int)s->max_filepos);
  mp_msg(MSGT_CACHE,MSGL_INFO,"%3d %%  (%3d%%)\n",100*newb/s->buffer_size,100*min_fill/s->buffer_size);
}

int cache_read(cache_vars_t* s,unsigned char* buf,int size){
  int total=0;
  while(size>0){
    int pos,newb,len;

//printf("CACHE2_READ: 0x%X <= 0x%X <= 0x%X  \n",s->min_filepos,s->read_filepos,s->max_filepos);
    
    if(s->read_filepos>=s->max_filepos || s->read_filepos<s->min_filepos){
	// eof?
	if(s->eof) break;
	// waiting for buffer fill...
	usec_sleep(READ_USLEEP_TIME); // 10ms
	continue; // try again...
    }

    newb=s->max_filepos-s->read_filepos; // new bytes in the buffer
    if(newb<min_fill) min_fill=newb; // statistics...

//printf("*** newb: %d bytes ***\n",newb);

    pos=s->read_filepos - s->offset;
    if(pos<0) pos+=s->buffer_size; else
    if(pos>=s->buffer_size) pos-=s->buffer_size;

    if(newb>s->buffer_size-pos) newb=s->buffer_size-pos; // handle wrap...
    if(newb>size) newb=size;
    
    // check:
    if(s->read_filepos<s->min_filepos) mp_msg(MSGT_CACHE,MSGL_ERR,"Ehh. s->read_filepos<s->min_filepos !!! Report bug...\n");
    
    // len=write(mem,newb)
//printf("Buffer read: %d bytes\n",newb);
    memcpy(buf,&s->buffer[pos],newb);
    buf+=newb;
    len=newb;
    // ...
    
    s->read_filepos+=len;
    size-=len;
    total+=len;
    
  }
  cache_fill_status=(s->max_filepos-s->read_filepos)/(s->buffer_size / 100);
  return total;
}

int cache_fill(cache_vars_t* s){
int back,back2,newb,space,len,pos;
  off_t read=s->read_filepos;
  
  if(read<s->min_filepos || read>s->max_filepos){
      // seek...
      mp_msg(MSGT_CACHE,MSGL_DBG2,"Out of boundaries... seeking to 0x%"PRIX64"  \n",(int64_t)read);
      // streaming: drop cache contents only if seeking backward or too much fwd:
      if(s->stream->type!=STREAMTYPE_STREAM ||
          read<s->min_filepos || read>=s->max_filepos+s->seek_limit)
      {
        s->offset= // FIXME!?
        s->min_filepos=s->max_filepos=read; // drop cache content :(
		s->stream->activeFileFlag = s->streamOriginal->activeFileFlag;
        if(s->stream->eof) stream_reset(s->stream);
        stream_seek(s->stream,read);
        mp_msg(MSGT_CACHE,MSGL_DBG2,"Seek done. new pos: 0x%"PRIX64"  \n",(int64_t)stream_tell(s->stream));
      }
  }
  
  // calc number of back-bytes:
  back=read - s->min_filepos;
  if(back<0) back=0; // strange...
  if(back>s->back_size) back=s->back_size;
  
  // calc number of new bytes:
  newb=s->max_filepos - read;
  if(newb<0) newb=0; // strange...

  // calc free buffer space:
  space=s->buffer_size - (newb+back);
  
  // calc bufferpos:
  pos=s->max_filepos - s->offset;
  if(pos>=s->buffer_size) pos-=s->buffer_size; // wrap-around
  
  if(space<s->fill_limit){
//    printf("Buffer is full (%d bytes free, limit: %d)\n",space,s->fill_limit);
    return 0; // no fill...
  }

  //printf("### read=0x%X  back=%d  newb=%d  space=%d  pos=%d\n",read,back,newb,space,pos);
     
  // reduce space if needed:
  if(space>s->buffer_size-pos) space=s->buffer_size-pos;
  
//  if(space>32768) space=32768; // limit one-time block size
  if(space>4*s->sector_size) space=4*s->sector_size;
  
//  if(s->seek_lock) return 0; // FIXME

#if 1
  // back+newb+space <= buffer_size
  back2=s->buffer_size-(space+newb); // max back size
  if(s->min_filepos<(read-back2)) s->min_filepos=read-back2;
#else
  s->min_filepos=read-back; // avoid seeking-back to temp area...
#endif
  
  // ....
  //printf("Buffer fill: %d bytes of %d\n",space,s->buffer_size);
  //len=stream_fill_buffer(s->stream);
  //memcpy(&s->buffer[pos],s->stream->buffer,len); // avoid this extra copy!
  // ....
  s->stream->activeFileFlag = s->streamOriginal->activeFileFlag;
  len=stream_read(s->stream,&s->buffer[pos],space);
  if(!len) s->eof=1;
  
  s->max_filepos+=len;
  if(pos+len>=s->buffer_size){
      // wrap...
      s->offset+=s->buffer_size;
  }
  
  return len;
  
}

cache_vars_t* cache_init(int size,int sector){
  int num;
  cache_vars_t* s=malloc(sizeof(cache_vars_t));
  if(s==NULL) return NULL;
  
  memset(s,0,sizeof(cache_vars_t));
  num=size/sector;
  if(num < 16){
     num = 16;
  }//32kb min_size
  s->buffer_size=num*sector;
  s->sector_size=sector;
  s->buffer=malloc(s->buffer_size);

  if(s->buffer == NULL){
    free(s);
    return NULL;
  }

  s->fill_limit=8*sector;
  s->back_size=s->buffer_size/2;
  return s;
}

void cache_uninit(stream_t *s) {
  cache_vars_t* c = s->cache_data;
  if(!s->cache_pid) return;
#ifndef WIN32
  c->killCacheThread = 1;
  while(c->killCacheThread) usleep(100); // don't join or we could deadlock!
//  pthread_join(s->cache_pid, NULL);
#else
  TerminateThread((HANDLE)s->cache_pid,0);
#endif
  free(c->stream);
  if(!c) return;
  free(c->buffer);
  free(s->cache_data);
}

static void exit_sighandler(int x){
  // close stream
  exit(0);
}

void *threadfunc(void*s){
fprintf(stderr,"CACHE THREAD STARTING\n");fflush(stderr);
	// cache thread mainloop:
#ifdef SYS_DARWIN
  signal(SIGTERM,exit_sighandler); // kill
#endif
  while(!((cache_vars_t*)s)->killCacheThread){
//fprintf(stderr,"CACHE THREAD RUNNING\n");fflush(stderr);
    if(!cache_fill((cache_vars_t*)s)){
	 usec_sleep(FILL_USLEEP_TIME); // idle
    }
//	 cache_stats(s->cache_data);
  }
  ((cache_vars_t*)s)->killCacheThread = 0;
fprintf(stderr,"CACHE THREAD ENDING\n");fflush(stderr);
//  pthread_exit(s);
}

int stream_enable_cache(stream_t *stream,int size,int min,int seek_limit){
  int ss=(stream->type==STREAMTYPE_VCD)?VCD_SECTOR_DATA:stream_buffer_size;
  cache_vars_t* s;

  if (stream->type==STREAMTYPE_STREAM && stream->fd < 0) {
    // The stream has no 'fd' behind it, so is non-cacheable
    mp_msg(MSGT_CACHE,MSGL_STATUS,"\rThis stream is non-cacheable\n");
    return 1;
  }

  s=cache_init(size,ss);
  if(s == NULL) return 0;
  stream->cache_data=s;
  s->streamOriginal=s->stream=stream; // callback
  s->seek_limit=seek_limit;


  //make sure that we won't wait from cache_fill
  //more data than it is alowed to fill
  if (s->seek_limit > s->buffer_size - s->fill_limit ){
     s->seek_limit = s->buffer_size - s->fill_limit;
  }
  if (min > s->buffer_size - s->fill_limit) {
     min = s->buffer_size - s->fill_limit;
  }
  
  // since we're doing threads instead of forking we need different memory for each of them,
  // this is how the caching is designed to work
  stream_t* stream2=malloc(sizeof(stream_t));
  memcpy(stream2,s->stream,sizeof(stream_t));
  s->stream=stream2;
#ifndef WIN32  
  pthread_attr_t attr;
  int rc;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  if (pthread_create(&stream->cache_pid, &attr, threadfunc, s))
	  return 0;
  else
  {
#else
  {
    DWORD threadId;
    stream->cache_pid = CreateThread(NULL,0,ThreadProc,s,0,&threadId);
#endif

    // wait until cache is filled at least prefill_init %
    mp_msg(MSGT_CACHE,MSGL_V,"CACHE_PRE_INIT: %"PRId64" [%"PRId64"] %"PRId64"  pre:%d  eof:%d  \n",
	(int64_t)s->min_filepos,(int64_t)s->read_filepos,(int64_t)s->max_filepos,min,s->eof);
    while(s->read_filepos<s->min_filepos || s->max_filepos-s->read_filepos<min){
//	mp_msg(MSGT_CACHE,MSGL_STATUS,"\rCache fill: %5.2f%% (%d bytes)    ",
//	    100.0*(float)(s->max_filepos-s->read_filepos)/(float)(s->buffer_size),
//	    s->max_filepos-s->read_filepos
//	);
	usec_sleep(50000);
	if(s->eof) break; // file is smaller than prefill size
	if(mp_input_check_interrupt(PREFILL_SLEEP_TIME))
	  return 0;
    }
    mp_msg(MSGT_CACHE,MSGL_STATUS,"\n");
    return 1; // parent exits
  }
  
}
#ifdef WIN32
static DWORD WINAPI ThreadProc(void*s){
fprintf(stderr,"CACHE THREAD STARTING\n");fflush(stderr);
	// cache thread mainloop:
//  signal(SIGTERM,exit_sighandler); // kill
  while(!((cache_vars_t*)s)->killCacheThread){
//fprintf(stderr,"CACHE THREAD RUNNING\n");fflush(stderr);
    if(!cache_fill((cache_vars_t*)s)){
	 usec_sleep(FILL_USLEEP_TIME); // idle
    }
//	 cache_stats(s->cache_data);
  }
fprintf(stderr,"CACHE THREAD ENDING\n");fflush(stderr);
//  pthread_exit(s);
}

#endif

int cache_stream_fill_buffer(stream_t *s){
  int len;
  if(s->eof){ s->buf_pos=s->buf_len=0; return 0; }
  if(!s->cache_pid) return stream_fill_buffer(s);

  //cache_stats(s->cache_data);

  if(s->pos!=((cache_vars_t*)s->cache_data)->read_filepos) mp_msg(MSGT_CACHE,MSGL_ERR,"!!! read_filepos differs!!! report this bug...\n");

  len=cache_read(s->cache_data,s->buffer, ((cache_vars_t*)s->cache_data)->sector_size);
  //printf("cache_stream_fill_buffer->read -> %d\n",len);

  if(len<=0){ s->eof=1; s->buf_pos=s->buf_len=0; return 0; }
  s->buf_pos=0;
  s->buf_len=len;
  s->pos+=len;
//  printf("[%d]",len);fflush(stdout);
  return len;

}

int cache_stream_seek_long(stream_t *stream,off_t pos){
  cache_vars_t* s;
  off_t newpos;
  if(!stream->cache_pid) return stream_seek_long(stream,pos);
  
  s=stream->cache_data;
//  s->seek_lock=1;
  
  mp_msg(MSGT_CACHE,MSGL_DBG2,"CACHE2_SEEK: 0x%"PRIX64" <= 0x%"PRIX64" (0x%"PRIX64") <= 0x%"PRIX64"  \n",s->min_filepos,pos,s->read_filepos,s->max_filepos);

  newpos=pos/s->sector_size; newpos*=s->sector_size; // align
  stream->pos=s->read_filepos=newpos;
  s->eof=0; // !!!!!!!

  cache_stream_fill_buffer(stream);

  pos-=newpos;
  if(pos>=0 && pos<=stream->buf_len){
    stream->buf_pos=pos; // byte position in sector
    return 1;
  }

//  stream->buf_pos=stream->buf_len=0;
//  return 1;

  mp_msg(MSGT_CACHE,MSGL_V,"cache_stream_seek: WARNING! Can't seek to 0x%"PRIX64" !\n",(int64_t)(pos+newpos));
  return 0;
}
