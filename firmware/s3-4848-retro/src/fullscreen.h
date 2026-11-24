#pragma once
#include <stdint.h>
#include <string.h>
// Native RGB565 -> existing RGB scanout; no additional full-size framebuffer.
namespace Fullscreen {
struct Control { int16_t x,y,w,h; uint8_t key; const char *label; };
static constexpr Control controls[]={
 {8,8,76,38,0,"EXIT"}, {396,8,76,38,0,"FIT"},
 {62,302,60,52,64,"UP"}, {8,358,60,52,32,"LT"},
 {62,414,60,52,128,"DN"}, {116,358,60,52,16,"RT"},
 {187,432,86,38,4,"SEL"}, {278,432,96,38,8,"START"},
 {332,356,62,62,2,"B"}, {406,310,66,66,1,"A"}
};
static uint16_t xmap[480], ymap[480];
static uint16_t row[480];
static int srcw=0,srch=0,viewH=480,viewY=0;
static bool fit=false;
static void configure(bool nes,bool aspect){
    srcw=nes?256:160; srch=nes?240:144; fit=aspect;
    viewH=aspect?(nes?450:432):480; viewY=(480-viewH)/2;
    for(int x=0;x<480;x++)xmap[x]=x*srcw/480;
    for(int y=0;y<viewH;y++)ymap[y]=y*srch/viewH;
}
static bool contains(int x,int y,const Control& c){
    return x>=c.x&&x<c.x+c.w&&y>=c.y&&y<c.y+c.h;
}
// Five-by-seven glyphs; 5 columns per row, most-significant bit on left.
static const uint8_t* glyph(char c){
    static const uint8_t A[]={14,17,17,31,17,17,17}, B[]={30,17,17,30,17,17,30},
      D[]={30,17,17,17,17,17,30}, E[]={31,16,16,30,16,16,31},
      F[]={31,16,16,30,16,16,16}, I[]={31,4,4,4,4,4,31},
      L[]={16,16,16,16,16,16,31}, N[]={17,25,25,21,19,19,17},
      P[]={30,17,17,30,16,16,16}, R[]={30,17,17,30,20,18,17},
      S[]={15,16,16,14,1,1,30}, T[]={31,4,4,4,4,4,4},
      U[]={17,17,17,17,17,17,14}, X[]={17,17,10,4,10,17,17};
    switch(c){case 'A':return A;case 'B':return B;case 'D':return D;
      case 'E':return E;case 'F':return F;case 'I':return I;case 'L':return L;
      case 'N':return N;case 'P':return P;case 'R':return R;case 'S':return S;
      case 'T':return T;case 'U':return U;case 'X':return X;default:return nullptr;}
}
static void control(uint16_t *fb,const Control &c,bool pressed,const char *label){
    // 50% dark overlay, 75% dark when pressed; bright outline and lettering.
    for(int j=0;j<c.h;j++){
      uint16_t *d=fb+(c.y+j)*480+c.x;
      for(int i=0;i<c.w;i++){
        if(j<2||j>=c.h-2||i<2||i>=c.w-2)d[i]=pressed?0x07e0:0xbdf7;
        else {uint16_t v=(d[i]&0xf7de)>>1; d[i]=pressed?((v&0xf7de)>>1):v;}
      }
    }
    int scale=strlen(label)==1?3:2;
    int tx=c.x+(c.w-(int(strlen(label))*6-1)*scale)/2;
    int ty=c.y+(c.h-7*scale)/2;
    for(int k=0;label[k];k++){
      const uint8_t *g=glyph(label[k]);if(!g)continue;
      for(int j=0;j<7;j++)for(int i=0;i<5;i++)if(g[j]&(16>>i))
        for(int yy=0;yy<scale;yy++)for(int xx=0;xx<scale;xx++)
          fb[(ty+j*scale+yy)*480+tx+(k*6+i)*scale+xx]=0xffff;
    }
}
static void render(uint16_t *fb,const uint16_t *src,uint8_t keys){
    if(!fb||!src||!srcw)return;
    if(viewY)memset(fb,0,viewY*480*sizeof(uint16_t));
    int prev=-1;
    for(int y=0;y<viewH;y++){
      int sy=ymap[y];
      if(sy!=prev){
        const uint16_t *s=src+sy*srcw;
        for(int x=0;x<480;x++)row[x]=s[xmap[x]];
        prev=sy;
      }
      memcpy(fb+(y+viewY)*480,row,sizeof(row));
    }
    if(viewY)memset(fb+(viewY+viewH)*480,0,(480-viewY-viewH)*480*sizeof(uint16_t));
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);i++)
      control(fb,controls[i],(keys&controls[i].key)!=0,i==1?(fit?"FULL":"FIT"):controls[i].label);
}
}
