#include "prisma_diag.h"
#ifdef PRISMA_COMBINED
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <string.h>

// Single producer (Arduino loop/setup); never called from LCD worker.
namespace {
constexpr uint32_t MAGIC=0x50444c49u,VERSION=1;
constexpr unsigned N=32,L=144;
struct Entry {uint32_t seq,ms;char text[L];};
struct History {uint32_t magic,version,next,count,seq;Entry entries[N];};
RTC_NOINIT_ATTR static History history;
static char lastCrash[192]="No retained crash phase";
static bool live=true,command=false,overflow=false,swallowLF=false;
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
   if(perf&&!strstr(e.text,"PERF")&&!strstr(e.text,"SLICE")&&!strstr(e.text,"SCALE")&&!strstr(e.text,"BENCH")&&!strstr(e.text,"LOAD"))continue;
   Serial.printf("%lu %lu %.*s\n",(unsigned long)e.seq,(unsigned long)e.ms,int(L-1),e.text);
   vTaskDelay(1);
 }
 Serial.println("PRISMA LOG END");
}
static void runCommand(){
 if(overflow){Serial.println("PRISMA ERR command too long");return;}
 line[used]=0;
 if(!strcmp(line,"help"))Serial.println("PRISMA :help :status :perf :mem :tasks :log :crash :debug on :debug off :bench ; :: sends literal colon");
 else if(!strcmp(line,"status"))prisma_diag_status();
 else if(!strcmp(line,"perf"))exportLog(true);
 else if(!strcmp(line,"log"))exportLog(false);
 else if(!strcmp(line,"crash"))Serial.printf("PRISMA CRASH reset=%d %s\n",(int)esp_reset_reason(),lastCrash);
 else if(!strcmp(line,"mem"))Serial.printf("PRISMA MEM heap=%u internal=%u largest=%u psram=%u psram_largest=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)ESP.getFreePsram(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 else if(!strcmp(line,"tasks"))Serial.printf("PRISMA TASKS count=%u loop_core=%d loop_stack=%u\n",(unsigned)uxTaskGetNumberOfTasks(),xPortGetCoreID(),(unsigned)uxTaskGetStackHighWaterMark(nullptr));
 else if(!strcmp(line,"debug on")){live=true;Serial.println("PRISMA DEBUG ON");}
 else if(!strcmp(line,"debug off")){live=false;Serial.println("PRISMA DEBUG OFF (history retained)");}
 else if(!strcmp(line,"bench")){if(!prisma_diag_bench())Serial.println("PRISMA BENCH unavailable: enter a DOS game or console first");}
 else Serial.println("PRISMA ERR unknown command; :help");
}
}
void diag_begin(){
 auto reason=esp_reset_reason();
 if(reason==ESP_RST_POWERON||reason==ESP_RST_BROWNOUT||history.magic!=MAGIC||history.version!=VERSION||history.next>=N||history.count>N){
   memset(&history,0,sizeof(history));history.version=VERSION;history.magic=MAGIC;
 }
 diag_logf(false,"PRISMA BOOT 050 reset=%d cpu_mhz=%u\n",(int)reason,getCpuFrequencyMhz());
}
void diag_logf(bool periodic,const char *fmt,...){
 char buf[256];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
 append(buf);if(!periodic||live)Serial.print(buf);
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
