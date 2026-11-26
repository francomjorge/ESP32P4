#include "prisma_diag.h"
#ifdef PRISMA_COMBINED
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <string.h>
void dos_cursor_scan(const char *arg);
void dos_key_scan(const char *arg);
bool dos_diag_scene_trace();
bool dos_diag_cpu_profile();

// Single producer (Arduino loop/setup); never called from LCD worker.
namespace {
constexpr uint32_t MAGIC=0x50444c49u,VERSION=1;
constexpr unsigned N=32,L=144;
struct Entry {uint32_t seq,ms;char text[L];};
struct History {uint32_t magic,version,next,count,seq;Entry entries[N];};
RTC_NOINIT_ATTR static History history;
static char lastCrash[192]="No retained crash phase";
enum LogMode : uint8_t { LOG_ALL,LOG_GFX,LOG_INPUT,LOG_MEM,LOG_OFF };
static LogMode live=LOG_ALL;
static bool command=false,overflow=false,swallowLF=false;
static char photo[14][64]={};
static const char *logNames[]={"all","gfx","input","mem","off"};
static bool prefix(const char *s,const char *p){return !strncmp(s,p,strlen(p));}
static bool matches(LogMode m,const char *s){
 if(m==LOG_ALL)return true;
 if(m==LOG_OFF)return false;
 if(m==LOG_GFX)return prefix(s,"DOS GFX")||prefix(s,"DOS PERF")||prefix(s,"DOS SLICE")||prefix(s,"DOS SCALE")||prefix(s,"DOS RASTER")||prefix(s,"DOS VGA")||prefix(s,"VIDEO PERF")||prefix(s,"PERF ");
 if(m==LOG_INPUT)return prefix(s,"DOS INPUT")||prefix(s,"DOS TEXT INPUT")||prefix(s,"DOS MOUSE")||prefix(s,"DOS RAM TOUCH")||prefix(s,"DOS OVERLAY")||prefix(s,"DOS ACTION")||prefix(s,"DOS TOUCH");
 return prefix(s,"DOS STACK")||prefix(s,"PRISMA MEM");
}
static void exportPhoto(unsigned first,unsigned end){
 live=LOG_OFF;
 if(!photo[first][0]){Serial.println("072 PHOTO no DOS sample yet; wait 3s and retry");return;}
 for(unsigned i=first;i<end;++i)Serial.println(photo[i]);
 Serial.println("PHOTO END (logs off)");
}
static char line[64];
static unsigned used=0;
static void append(const char *s){
 if(history.magic!=MAGIC||history.version!=VERSION||history.next>=N||history.count>N)return;
 Entry &e=history.entries[history.next];
 e.seq=++history.seq;e.ms=millis();
 snprintf(e.text,sizeof(e.text),"%s",s);
 unsigned n=strlen(e.text);while(n&&(e.text[n-1]=='\n'||e.text[n-1]=='\r'))e.text[--n]=0;
 history.next=(history.next+1)%N;if(history.count<N)history.count++;
}
static void exportLog(bool perf){
 Serial.printf("PRISMA LOG BEGIN entries=%u filter=%s\n",history.count,perf?"PERF":"ALL");
 for(unsigned i=0;i<history.count;i++){
   const Entry &e=history.entries[(history.next+N-history.count+i)%N];
   if(perf&&!strstr(e.text,"PERF")&&!strstr(e.text,"SLICE")&&!strstr(e.text,"SCALE")&&!strstr(e.text,"BENCH")&&!strstr(e.text,"LOAD")&&!strstr(e.text,"DOS GFX"))continue;
   Serial.printf("%lu %lu %.*s\n",(unsigned long)e.seq,(unsigned long)e.ms,int(L-1),e.text);
   vTaskDelay(1);
 }
 Serial.println("PRISMA LOG END");
}
static void runCommand(){
 if(overflow){Serial.println("PRISMA ERR command too long");return;}
 while(used&&line[used-1]==' ')--used;
 line[used]=0;
 if(!strcmp(line,"help"))Serial.println("PRISMA :help :status :gfx :input :memory :logs all/gfx/input/mem/off :perf :mem :tasks :log :crash :debug on :debug off :bench :trace :cpu :mark LABEL :cursor click/click mouse/touch on/status/watch/yfind/ylist/testy Y [hexaddr]/test X Y/testx X/auto/base/right/left/down/up/list/stop :clave ref DDDD/after/list/stop ; :: sends literal colon");
 else if(!strcmp(line,"status"))prisma_diag_status();
 else if(!strcmp(line,"perf"))exportLog(true);
 else if(!strcmp(line,"gfx"))exportPhoto(0,6);
 else if(!strcmp(line,"input"))exportPhoto(6,10);
 else if(!strcmp(line,"memory"))exportPhoto(10,14);
 else if(!strncmp(line,"logs ",5)){
   for(unsigned i=0;i<5;++i)if(!strcmp(line+5,logNames[i])){live=LogMode(i);Serial.printf("PRISMA LOGS %s\n",logNames[i]);return;}
   Serial.println("PRISMA logs all/gfx/input/mem/off");
 }
 else if(!strncmp(line,"mark ",5)&&line[5])diag_logf(false,"DOS GFX MARK %s\n",line+5);
 else if(!strcmp(line,"log"))exportLog(false);
 else if(!strcmp(line,"crash"))Serial.printf("PRISMA CRASH reset=%d %s\n",(int)esp_reset_reason(),lastCrash);
 else if(!strcmp(line,"mem"))Serial.printf("PRISMA MEM heap=%u internal=%u largest=%u psram=%u psram_largest=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)ESP.getFreePsram(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 else if(!strcmp(line,"tasks"))Serial.printf("PRISMA TASKS count=%u loop_core=%d loop_stack=%u\n",(unsigned)uxTaskGetNumberOfTasks(),xPortGetCoreID(),(unsigned)uxTaskGetStackHighWaterMark(nullptr));
 else if(!strcmp(line,"debug on")){live=LOG_ALL;Serial.println("PRISMA DEBUG ON");}
 else if(!strcmp(line,"debug off")){live=LOG_OFF;Serial.println("PRISMA DEBUG OFF (history retained)");}
 else if(!strcmp(line,"cpu")){live=LOG_OFF;if(!dos_diag_cpu_profile())Serial.println("072 CPU unavailable");}
 else if(!strcmp(line,"trace")){live=LOG_OFF;if(!dos_diag_scene_trace())Serial.println("072 TRACE unavailable");}
 else if(!strcmp(line,"bench")){if(!prisma_diag_bench())Serial.println("PRISMA BENCH unavailable: enter a DOS game or console first");}
 else if(!strncmp(line,"cursor ",7))dos_cursor_scan(line+7);
 else if(!strncmp(line,"clave ",6))dos_key_scan(line+6);
 else Serial.println("PRISMA ERR unknown command; :help");
}
}
void diag_begin(){
 auto reason=esp_reset_reason();
 if(reason==ESP_RST_POWERON||reason==ESP_RST_BROWNOUT||history.magic!=MAGIC||history.version!=VERSION||history.next>=N||history.count>N){
   memset(&history,0,sizeof(history));history.version=VERSION;history.magic=MAGIC;
 }
 diag_logf(false,"PRISMA BOOT 065 reset=%d cpu_mhz=%u\n",(int)reason,getCpuFrequencyMhz());
}
void diag_logf(bool periodic,const char *fmt,...){
 char buf[256];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
 append(buf);if(!periodic||matches(live,buf))Serial.print(buf);
}
void diag_photo_line(unsigned slot,const char *fmt,...){
 if(slot>=14)return;
 va_list ap;va_start(ap,fmt);vsnprintf(photo[slot],sizeof(photo[slot]),fmt,ap);va_end(ap);
}
void diag_crashf(const char *fmt,...){
 va_list ap;va_start(ap,fmt);vsnprintf(lastCrash,sizeof(lastCrash),fmt,ap);va_end(ap);
 diag_logf(false,"%s",lastCrash);
}
bool diag_command_pending(){return command||swallowLF;}
bool diag_consume(uint8_t c){
 if(swallowLF){swallowLF=false;if(c=='\n')return true;}
 if(!command){if(c!=':')return false;command=true;used=0;overflow=false;return true;}
 if(c==':'&&used==0&&!overflow){command=false;return false;}
 if(c=='\r'||c=='\n'){command=false;swallowLF=c=='\r';runCommand();used=0;overflow=false;return true;}
 if(c==27){command=false;used=0;overflow=false;return true;}
 if(c==8||c==127){if(used)--used;return true;}
 if(c>=32&&c<127){if(used+1<sizeof(line))line[used++]=char(c);else overflow=true;}
 return true;
}
void diag_poll(){for(int i=0;i<32&&Serial.available();i++)diag_consume((uint8_t)Serial.read());}
#endif
