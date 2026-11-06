#include "subtitles.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

enum { MAX_CUES=20000, MAX_BYTES=8*1024*1024, SLOTS=4 };
typedef struct { char *tag; uint16_t pitch,perm; uint32_t begin,end; uint16_t *text; } Cue;
typedef struct { int active,priority; int32_t id; uint32_t start,end; size_t first,last; } Voice;
struct subtitles { Cue *cues; size_t count; Voice voices[SLOTS]; uint32_t last; int clock_set; };
static void release(Cue *c,size_t n){size_t i;for(i=0;i<n;i++){free(c[i].tag);free(c[i].text);}free(c);}
subtitles *subtitles_create(void){return (subtitles *)calloc(1,sizeof(subtitles));}
void subtitles_reset(subtitles *s){if(s){memset(s->voices,0,sizeof(s->voices));s->clock_set=0;}}
void subtitles_destroy(subtitles *s){if(s){release(s->cues,s->count);free(s);}}
static int number(const char *p,uint32_t limit,uint32_t *v){uint32_t n=0;if(!*p)return 0;while(*p){unsigned d=(unsigned char)*p++-'0';if(d>9||n>limit/10||(n==limit/10&&d>limit%10))return 0;n=n*10+d;}*v=n;return 1;}
/* Strict Unicode scalar decoding, including UTF-16 surrogate pairs. */
static uint16_t *decode(const char *p){size_t n=strlen(p),i=0,k=0;uint16_t *out=(uint16_t *)malloc((n+1)*sizeof(*out));if(!out)return NULL;
 while(i<n){uint32_t c=(unsigned char)p[i++],min=0;unsigned more=0,j;
  if(c=='\\'){if(i==n)goto bad;c=(unsigned char)p[i++];if(c=='n')c=10;else if(c=='r')c=13;else if(c=='t')c=9;else if(c!='\\')goto bad;}
  else if(c<0x80){}
  else if(c>=0xc2&&c<=0xdf){c&=31;more=1;min=0x80;}
  else if(c>=0xe0&&c<=0xef){c&=15;more=2;min=0x800;}
  else if(c>=0xf0&&c<=0xf4){c&=7;more=3;min=0x10000;}
  else goto bad;
  if(more>n-i)goto bad;
  for(j=0;j<more;j++){uint32_t b=(unsigned char)p[i++];if((b&0xc0)!=0x80)goto bad;c=(c<<6)|(b&63);}
  if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))goto bad;
  if(c>0xffff){c-=0x10000;out[k++]=(uint16_t)(0xd800+(c>>10));out[k++]=(uint16_t)(0xdc00+(c&1023));}else out[k++]=(uint16_t)c;
 }out[k]=0;return out;
 bad:free(out);return NULL;
}
static int keycmp(const Cue *a,const Cue *b){int x=strcmp(a->tag,b->tag);if(x)return x;if(a->pitch!=b->pitch)return a->pitch<b->pitch?-1:1;return a->perm<b->perm?-1:a->perm>b->perm;}
static int compare(const void *a,const void *b){const Cue *x=a,*y=b;int k=keycmp(x,y);if(k)return k;return x->begin<y->begin?-1:x->begin>y->begin;}
int subtitles_load(subtitles *s,const char *data,size_t bytes){char *buf,*p;Cue *c=NULL;size_t n=0,capacity=0;
 if(!s||bytes>MAX_BYTES||(!data&&bytes))return 0;
 if(!bytes){release(s->cues,s->count);s->cues=NULL;s->count=0;subtitles_reset(s);return 1;}
 if(memchr(data,0,bytes))return 0;
 buf=(char *)malloc(bytes+1);if(!buf)return 0;memcpy(buf,data,bytes);buf[bytes]=0;p=buf;
 if(bytes>=3&&(unsigned char)p[0]==0xef&&(unsigned char)p[1]==0xbb&&(unsigned char)p[2]==0xbf)p+=3;
 while(*p){char *row=p,*end,*v[6];uint32_t nums[4];size_t i;Cue item;
  while(*p&&*p!='\n')p++;
  if(*p)*p++=0;
  end=row+strlen(row);if(end>row&&end[-1]=='\r')end[-1]=0;
  if(!*row||*row=='#')continue;
  v[0]=row;
  for(i=1;i<6;i++){char *tab=strchr(v[i-1],'\t');if(!tab)goto bad;*tab=0;v[i]=tab+1;}
  if(!*v[0]||!*v[5]||strchr(v[5],'\t')||n==MAX_CUES)goto bad;
  for(i=0;i<4;i++)if(!number(v[i+1],i<2?65535:3600000,&nums[i]))goto bad;
  if(nums[3]<=nums[2])goto bad;
  if(n==capacity){size_t cap=capacity?capacity*2:64;Cue *next;if(cap>MAX_CUES)cap=MAX_CUES;next=(Cue *)realloc(c,cap*sizeof(*c));if(!next)goto bad;c=next;capacity=cap;}
  memset(&item,0,sizeof(item));item.tag=(char *)malloc(strlen(v[0])+1);item.text=decode(v[5]);
  if(!item.tag||!item.text){free(item.tag);free(item.text);goto bad;}
  strcpy(item.tag,v[0]);item.pitch=(uint16_t)nums[0];item.perm=(uint16_t)nums[1];item.begin=nums[2];item.end=nums[3];c[n++]=item;
 }
 if(n)qsort(c,n,sizeof(*c),compare);
 /* Reject overlapping cues for a sound key, avoiding ambiguous text choice. */
 {size_t i;for(i=1;i<n;i++)if(!keycmp(c+i-1,c+i)&&c[i].begin<c[i-1].end)goto bad;}
 free(buf);release(s->cues,s->count);s->cues=c;s->count=n;subtitles_reset(s);return 1;
 bad:free(buf);release(c,n);return 0;
}
static void clock_update(subtitles *s,uint32_t now){if(s->clock_set&&(int32_t)(now-s->last)<0)subtitles_reset(s);s->last=now;s->clock_set=1;}
static void expire(subtitles *s,uint32_t now){size_t i;for(i=0;i<SLOTS;i++)if(s->voices[i].active&&(uint32_t)(now-s->voices[i].start)>=s->voices[i].end)s->voices[i].active=0;}
void subtitles_sound_stop(subtitles *s,int32_t id){size_t i;if(s)for(i=0;i<SLOTS;i++)if(s->voices[i].active&&s->voices[i].id==id)s->voices[i].active=0;}
void subtitles_sound_start(subtitles *s,int32_t id,const char *tag,uint16_t pitch,uint16_t perm,uint32_t now,int priority){Cue key;size_t lo=0,hi,first,last,i;uint32_t end=0;Voice *slot=NULL;
 if(!s||!tag||id==-1)return;
 clock_update(s,now);expire(s,now);subtitles_sound_stop(s,id);
 memset(&key,0,sizeof(key));key.tag=(char *)tag;key.pitch=pitch;key.perm=perm;hi=s->count;
 while(lo<hi){size_t mid=lo+(hi-lo)/2;if(keycmp(s->cues+mid,&key)<0)lo=mid+1;else hi=mid;}
 first=lo;last=lo;while(last<s->count&&!keycmp(s->cues+last,&key)){if(s->cues[last].end>end)end=s->cues[last].end;last++;}if(first==last)return;
 for(i=0;i<SLOTS;i++)if(!s->voices[i].active){slot=s->voices+i;break;}
 if(!slot){slot=s->voices;for(i=1;i<SLOTS;i++){Voice *v=s->voices+i;if(v->priority<slot->priority||(v->priority==slot->priority&&(uint32_t)(now-v->start)>(uint32_t)(now-slot->start)))slot=v;}if(slot->priority>priority)return;}
 slot->active=1;slot->id=id;slot->start=now;slot->end=end;slot->priority=priority;slot->first=first;slot->last=last;
}
void subtitles_render(subtitles *s,uint32_t now,int visible,subtitles_draw_fn draw,void *user){size_t i,j;Voice *best=NULL;const uint16_t *text=NULL;
 if(!s)return;
 clock_update(s,now);expire(s,now);if(!visible||!draw)return;
 for(i=0;i<SLOTS;i++){Voice *v=s->voices+i;uint32_t age;if(!v->active)continue;age=now-v->start;
  for(j=v->first;j<v->last;j++){Cue *c=s->cues+j;if(age>=c->begin&&age<c->end){if(!best||v->priority>best->priority||(v->priority==best->priority&&age<=(uint32_t)(now-best->start))){best=v;text=c->text;}break;}}
 }if(text)draw(user,text);
}
