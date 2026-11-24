#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <esp_partition.h>
struct __attribute__((packed)) PrivateEntry { char name[40]; uint32_t offset; uint32_t size; };
struct __attribute__((packed)) PrivateHeader { char magic[8]; uint32_t count; PrivateEntry entry[8]; };
static PrivateHeader privHeader{};
static bool privateMD[8]{};
static const esp_partition_t* privPart=nullptr;
static void private_rom_init(){
    privPart=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"storage");
    if(!privPart||esp_partition_read(privPart,0,&privHeader,sizeof(privHeader))!=ESP_OK||
       memcmp(privHeader.magic,"PRIVNES4",8)||privHeader.count>8){privHeader.count=0;return;}
    memset(privateMD,0,sizeof(privateMD));
    for(unsigned i=0;i<privHeader.count;i++){
        const auto &e=privHeader.entry[i];
        if(!memchr(e.name,0,40)||!e.name[0]||e.offset<4096||e.size<512||e.size>4*1024*1024||
           e.offset>privPart->size||e.size>privPart->size-e.offset){privHeader.count=0;return;}
        uint8_t hdr[260];
        if(esp_partition_read(privPart,e.offset,hdr,sizeof(hdr))!=ESP_OK){privHeader.count=0;return;}
        if(!memcmp(hdr,"NES\x1a",4) && e.size>=16400)privateMD[i]=false;
        else if(!memcmp(hdr+256,"SEGA",4))privateMD[i]=true;
        else {privHeader.count=0;return;}
    }
}
static bool private_rom_md(unsigned i){return i<privHeader.count&&privateMD[i];}
static unsigned private_rom_count(){return privHeader.count;}
static const char* private_rom_name(unsigned i){return privHeader.entry[i].name;}
static int private_rom_find(const char* name){for(unsigned i=0;i<privHeader.count;i++)if(!strcmp(name,private_rom_name(i)))return i;return -1;}
static size_t private_rom_size(unsigned i){return privHeader.entry[i].size;}
static bool private_rom_read(unsigned i,void *dst,size_t bytes){return i<privHeader.count&&bytes==private_rom_size(i)&&esp_partition_read(privPart,privHeader.entry[i].offset,dst,bytes)==ESP_OK;}
