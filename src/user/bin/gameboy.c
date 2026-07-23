#include "appui.h"
#include "guiapp.h"
#include "libc.h"
#include "../third_party/binjgb/src/emulator.h"

#define ROM_DEFAULT "/fs/games/gameboy/game.gb"
#define AUDIO_RATE 44100
#define AUDIO_FRAMES 768

static struct guiapp_ctx gui;
static Emulator *emulator;
static volatile int ready, closed, want_w=640, want_h=576;
static volatile uint8_t held_buttons;
static JoypadButtons buttons;
#ifndef BINJGB_BUZZOS_INDEXED_COLOR
static uint8_t native_frame[SCREEN_WIDTH*SCREEN_HEIGHT];
#endif
static uint8_t mono_audio[AUDIO_FRAMES];
#ifndef BINJGB_BUZZOS_INDEXED_COLOR
static uint8_t color_r[256],color_g[256],color_b[256];
#endif
static char rom_path[GUIAPP_PATH_MAX], save_path[GUIAPP_PATH_MAX];

enum { BTN_RIGHT=1, BTN_LEFT=2, BTN_UP=4, BTN_DOWN=8,
       BTN_A=16, BTN_B=32, BTN_START=64, BTN_SELECT=128 };

static void copy_text(char*d,const char*s,size_t cap){size_t i=0;if(!cap)return;while(s&&s[i]&&i+1<cap){d[i]=s[i];i++;}d[i]=0;}
static int rom_extension(const char*s){size_t n=strlen(s);if(n<3)return 0;if(s[n-3]=='.'&&(s[n-2]=='g'||s[n-2]=='G')&&(s[n-1]=='b'||s[n-1]=='B'))return 1;return n>=4&&s[n-4]=='.'&&(s[n-3]=='g'||s[n-3]=='G')&&(s[n-2]=='b'||s[n-2]=='B')&&(s[n-1]=='c'||s[n-1]=='C');}
static int choose_rom(const char*arg){
 struct stat st;if(arg&&arg[0]&&stat(arg,&st)==0&&st.st_type==DT_REG){copy_text(rom_path,arg,sizeof(rom_path));return 0;}
 if(stat(ROM_DEFAULT,&st)==0&&st.st_type==DT_REG){copy_text(rom_path,ROM_DEFAULT,sizeof(rom_path));return 0;}
 int fd=open("/fs/games/gameboy",O_RDONLY);if(fd<0)return-1;struct dirent e[16];int bytes;
 while((bytes=getdents(fd,e,sizeof(e)))>0)for(int i=0;i<bytes/(int)sizeof(e[0]);i++)if(e[i].d_type==DT_REG&&rom_extension(e[i].d_name)){snprintf(rom_path,sizeof(rom_path),"/fs/games/gameboy/%s",e[i].d_name);close(fd);return 0;}
 close(fd);return-1;
}
static int load_rom(FileData*out){
 struct stat st;if(stat(rom_path,&st)<0||st.st_type!=DT_REG||st.st_size<MINIMUM_ROM_SIZE||st.st_size>MAXIMUM_ROM_SIZE||(st.st_size&(MINIMUM_ROM_SIZE-1)))return-1;
 out->data=malloc(st.st_size);if(!out->data)return-1;out->size=st.st_size;int fd=open(rom_path,O_RDONLY);if(fd<0)return-1;size_t p=0;
 while(p<out->size){int n=read(fd,out->data+p,out->size-p);if(n<=0){close(fd);return-1;}p+=(size_t)n;}close(fd);return 0;
}
static void make_save_path(void){const char*n=strrchr(rom_path,'/');n=n?n+1:rom_path;char base[32];size_t i=0;while(n[i]&&n[i]!='.'&&i+1<sizeof(base)){base[i]=n[i];i++;}base[i]=0;if(!base[0])copy_text(base,"game",sizeof(base));snprintf(save_path,sizeof(save_path),"/fs/games/gameboy/%s.sav",base);}

static uint8_t key_button(int k){switch(k){
 case GUIAPP_KEY_RIGHT:return BTN_RIGHT;case GUIAPP_KEY_LEFT:return BTN_LEFT;
 case GUIAPP_KEY_UP:return BTN_UP;case GUIAPP_KEY_DOWN:return BTN_DOWN;
 case 'z':case 'Z':return BTN_B;case 'x':case 'X':return BTN_A;
 case '\r':case '\n':return BTN_START;case GUIAPP_KEY_BACKSPACE:case 'c':case 'C':return BTN_SELECT;default:return 0;}}
static void read_events(void){
 struct guiapp_event e;while(guiapp_read_event(&gui,&e)==0){
  if(e.type==GUIAPP_EVT_CLOSE){closed=1;break;}
  if(e.type==GUIAPP_EVT_INIT||e.type==GUIAPP_EVT_RESIZE){if(e.width>0)want_w=e.width;if(e.height>0)want_h=e.height;ready=1;}
  else if(e.type==GUIAPP_EVT_KEY){uint8_t m=key_button(e.key);if(m){if(e.buttons)held_buttons|=m;else held_buttons&=(uint8_t)~m;}}
 }closed=1;
}
static void update_buttons(void){
 uint8_t held=held_buttons;
 memset(&buttons,0,sizeof(buttons));buttons.right=!!(held&BTN_RIGHT);buttons.left=!!(held&BTN_LEFT);buttons.up=!!(held&BTN_UP);buttons.down=!!(held&BTN_DOWN);
 buttons.A=!!(held&BTN_A);buttons.B=!!(held&BTN_B);buttons.start=!!(held&BTN_START);buttons.select=!!(held&BTN_SELECT);emulator_set_joypad_buttons(emulator,&buttons);
}
static void load_save(void){struct stat st;if(stat(save_path,&st)<0||st.st_type!=DT_REG||!st.st_size)return;FileData d={0};d.data=malloc(st.st_size);if(!d.data)return;d.size=st.st_size;int fd=open(save_path,O_RDONLY);size_t p=0;if(fd>=0){while(p<d.size){int n=read(fd,d.data+p,d.size-p);if(n<=0)break;p+=(size_t)n;}close(fd);if(p==d.size)(void)emulator_read_ext_ram(emulator,&d);}free(d.data);}
static void save_game(void){FileData d={0};emulator_init_ext_ram_file_data(emulator,&d);if(!d.data||!d.size)return;if(SUCCESS(emulator_write_ext_ram(emulator,&d))){int fd=open(save_path,O_WRONLY|O_CREAT|O_TRUNC);size_t p=0;if(fd>=0){while(p<d.size){int n=write(fd,d.data+p,d.size-p);if(n<=0)break;p+=(size_t)n;}close(fd);}}free(d.data);}
static void send_audio(void){
 AudioBuffer*a=emulator_get_audio_buffer(emulator);u32 count=audio_buffer_get_frames(a);if(count>AUDIO_FRAMES)count=AUDIO_FRAMES;
 for(u32 i=0;i<count;i++)mono_audio[i]=(uint8_t)(((unsigned)a->data[i*2]+(unsigned)a->data[i*2+1])/2u);
 /* audio_write() is deliberately non-blocking and may accept only the free
  * part of the kernel FIFO.  Dropping that unwritten tail produces audible
  * gaps whenever rendering briefly outruns AC97.  Treat the hardware stream
  * as the master clock and retain every emulated sample. */
 u32 offset=0;while(offset<count&&!closed){int n=audio_write(mono_audio+offset,count-offset);if(n<0)break;if(n==0){sleep_ms(1);continue;}offset+=(u32)n;}
}
#ifndef BINJGB_BUZZOS_INDEXED_COLOR
static void init_color_tables(void){for(unsigned i=0;i<256;i++){unsigned q=i*5u/255u;color_r[i]=(uint8_t)(40u+q*36u);color_g[i]=(uint8_t)(q*6u);color_b[i]=(uint8_t)q;}}
static uint8_t rgba_index(RGBA p){return(uint8_t)(color_r[p&255u]+color_g[(p>>8)&255u]+color_b[(p>>16)&255u]);}
#endif
static void render(const char*title){
 int w=want_w,h=want_h;if(w<320)w=320;if(w>GUIAPP_MAX_W)w=GUIAPP_MAX_W;if(h<288)h=288;if(h>GUIAPP_MAX_H)h=GUIAPP_MAX_H;int need=w*h;
 (void)need;
#ifdef BINJGB_BUZZOS_INDEXED_COLOR
 const uint8_t*native_frame=*emulator_get_frame_buffer(emulator);
#else
 RGBA*fb=*emulator_get_frame_buffer(emulator);
 for(int i=0;i<SCREEN_WIDTH*SCREEN_HEIGHT;i++)native_frame[i]=rgba_index(fb[i]);
#endif
 if(guiapp_send_scaled_frame(&gui,title,w,h,native_frame,SCREEN_WIDTH,SCREEN_HEIGHT)<0)closed=1;
}

static int message(const char*title,const char*a,const char*b){
 struct guiapp_event e;uint8_t*p=malloc((size_t)GUIAPP_MAX_W*GUIAPP_MAX_H);if(!p)return 1;int w=640,h=400;
 for(;;){if(guiapp_read_event(&gui,&e)<0||e.type==GUIAPP_EVT_CLOSE)break;if(e.type==GUIAPP_EVT_INIT||e.type==GUIAPP_EVT_RESIZE){w=e.width;h=e.height;if(w<400)w=400;if(w>GUIAPP_MAX_W)w=GUIAPP_MAX_W;if(h<260)h=260;if(h>GUIAPP_MAX_H)h=GUIAPP_MAX_H;}
  appui_fill(p,w,h,(struct appui_rect){0,0,w,h},appui_gray(2));appui_text(p,w,h,28,30,title,15,-1,(struct appui_rect){20,20,w-40,32});
  appui_text(p,w,h,28,84,a,appui_rgb6(1,4,2),-1,(struct appui_rect){20,76,w-40,30});appui_text(p,w,h,28,126,b,15,-1,(struct appui_rect){20,118,w-40,60});
  appui_text(p,w,h,28,200,"Controls: arrows, Z=B, X=A, Enter=Start, Backspace=Select",appui_gray(5),-1,(struct appui_rect){20,192,w-40,34});if(guiapp_send_frame(&gui,title,w,h,p)<0)break;
 }free(p);return 0;
}
int main(int argc,char**argv){
 if(guiapp_parse_args(argc,argv,&gui)<0)return 1;mkdir("/fs/games");mkdir("/fs/games/gameboy");
#ifndef BINJGB_BUZZOS_INDEXED_COLOR
 init_color_tables();
#endif
 if(choose_rom(argc>4?argv[4]:0)<0)return message("Game Boy Color - ROM required","No .gb or .gbc ROM was found.","Put a legally obtained ROM in /fs/games/gameboy/ and reopen.");
 FileData rom={0};if(load_rom(&rom)<0)return message("Game Boy Color - ROM error","The selected ROM has an invalid size or could not be read.",rom_path);
 char game_name[17];memset(game_name,0,sizeof(game_name));for(int i=0;i<16&&rom.data[0x134+i];i++){uint8_t c=rom.data[0x134+i];game_name[i]=(c>=32&&c<127)?(char)c:' ';}
 if(audio_config_latency(AUDIO_RATE,60)<0)return message("Game Boy Color - audio error","No audio device could select 44100 Hz playback.",rom_path);
 EmulatorInit init;memset(&init,0,sizeof(init));init.rom=rom;init.audio_frequency=AUDIO_RATE;init.audio_frames=AUDIO_FRAMES;init.builtin_palette=0;init.cgb_color_curve=CGB_COLOR_CURVE_SAMEBOY_EMULATE_HARDWARE;
 emulator=emulator_new(&init);if(!emulator)return message("Game Boy Color - unsupported ROM","binjgb could not initialize this cartridge.",rom_path);
 make_save_path();load_save();char title[GUIAPP_TITLE_MAX];snprintf(title,sizeof(title),"Game Boy Color - %s",game_name[0]?game_name:"Game");
 if(spawn(read_events)<0)return 2;while(!ready&&!closed)sleep_ms(1);uint32_t frame_due=monotonic_ms(),frame_frac=0,last_save=frame_due;int dirty=0,invalid=0;
 while(!closed&&!invalid){
  update_buttons();uint32_t now=monotonic_ms();int frames=1;
  if((int32_t)(now-frame_due)>17){frames+=(int)((now-frame_due)/17u);if(frames>4)frames=4;}
  for(int f=0;f<frames&&!invalid;f++){
   int got_frame=0;
   while(!got_frame&&!invalid){
    Ticks target=emulator_get_ticks(emulator)+PPU_FRAME_TICKS*2u;EmulatorEvent event;
    do{event=emulator_run_until(emulator,target);if(event&EMULATOR_EVENT_AUDIO_BUFFER_FULL)send_audio();if(event&EMULATOR_EVENT_NEW_FRAME)got_frame=1;if(event&EMULATOR_EVENT_INVALID_OPCODE){invalid=1;break;}}while(!(event&EMULATOR_EVENT_UNTIL_TICKS)&&!got_frame);
   }
   frame_due+=16;frame_frac+=743;if(frame_frac>=1000){frame_due++;frame_frac-=1000;}
  }
  render(title);if(emulator_was_ext_ram_updated(emulator))dirty=1;now=monotonic_ms();if(dirty&&now-last_save>=5000){save_game();dirty=0;last_save=now;}
  if((int32_t)(frame_due-now)>0)sleep_ms(frame_due-now);else if((int32_t)(now-frame_due)>100)frame_due=now;
 }
 if(dirty)save_game();(void)audio_flush();emulator_delete(emulator);return invalid?3:0;
}
