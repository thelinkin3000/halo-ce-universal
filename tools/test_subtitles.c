#include "subtitles.h"
#include "subtitle_layout.h"
#include <assert.h>
#include <string.h>
static int draws; static unsigned first;
static void draw(void *u,const uint16_t *s){(void)u;draws++;first=s[0];}
int main(void){
 subtitles *s=subtitles_create();subtitle_rect p,t;
 const char *data="story\t0\t0\t0\t2000\tStory\ncombat\t0\t0\t0\t2000\tCombat\n";
 assert(s);assert(subtitles_load(s,data,strlen(data)));
 subtitles_sound_start(s,1,"story",0,0,100,10);subtitles_sound_start(s,2,"combat",0,0,110,1);
 subtitles_render(s,120,1,draw,0);assert(draws==1&&first=='S');
 subtitles_render(s,130,0,draw,0);assert(draws==1);
 subtitles_sound_stop(s,1);subtitles_render(s,140,1,draw,0);assert(draws==2&&first=='C');
 assert(!subtitles_load(s,"invalid",7));subtitles_render(s,150,1,draw,0);assert(draws==3);
 subtitles_reset(s);subtitles_render(s,160,1,draw,0);assert(draws==3);
 assert(subtitle_footer_layout(853,&p,&t));assert(p.y0==419&&p.y1==478&&t.y0==444&&t.x1==809);
 assert(!subtitle_footer_layout(10,&p,&t));subtitles_destroy(s);return 0;
}
