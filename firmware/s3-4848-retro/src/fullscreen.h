#pragma once
#include <stdint.h>
#include <string.h>
// Native RGB565 -> existing RGB scanout; no additional full-size framebuffer.
namespace Fullscreen {
struct Control { int16_t x,y,w,h; uint8_t key; const char *label; };
static constexpr Control controls[]={
 {8,8,76,38,0,"EXIT"}, {402,8,70,38,0,"SAFE"},
 {62,302,60,52,64,"UP"}, {8,358,60,52,32,"LT"},
 {62,414,60,52,128,"DN"}, {116,358,60,52,16,"RT"},
 {187,432,86,38,4,"SEL"}, {278,432,96,38,8,"START"},
 {332,356,62,62,2,"B"}, {406,310,66,66,1,"A"}
};
static constexpr int STRIP=16;
alignas(16) static uint16_t strip[STRIP*480];
alignas(16) static uint16_t row[480];
static uint16_t ymap[480];
static void configure(){for(int y=0;y<480;y++)ymap[y]=y*288/480;}
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
// Compose only into internal SRAM; open-center outlines, no alpha or readback.
static void overlayLine(uint16_t *dst,int y,uint8_t keys){
  for(const auto &c:controls){
    int j=y-c.y;if(j<0||j>=c.h)continue;
    const uint16_t color=(keys&c.key)?0x07e0:0xffff;
    if(j<2||j>=c.h-2){
      for(int x=0;x<c.w;x++)dst[c.x+x]=color;
    }else{
      dst[c.x]=dst[c.x+1]=dst[c.x+c.w-2]=dst[c.x+c.w-1]=color;
    }
    int scale=strlen(c.label)==1?3:2;
    int tx=c.x+(c.w-(int(strlen(c.label))*6-1)*scale)/2;
    int ty=(c.h-7*scale)/2;
    if(j>=ty&&j<ty+7*scale){
      for(int k=0;c.label[k];k++){
        const uint8_t *g=glyph(c.label[k]);if(!g)continue;
        for(int i=0;i<5;i++)if(g[(j-ty)/scale]&(16>>i))
          for(int dx=0;dx<scale;dx++)dst[tx+(k*6+i)*scale+dx]=color;
      }
    }
  }
}
template<class Display> static void render(Display& lcd,const uint16_t *src,uint8_t keys){
    int prev=-1;
    for(int y=0;y<480;y+=STRIP){
      for(int j=0;j<STRIP;j++){
        int sy=ymap[y+j];
        if(sy!=prev){
          const uint16_t *s=src+sy*320;
          for(int x=0,z=0;x<320;x+=2,z+=3){
            row[z]=row[z+1]=s[x];row[z+2]=s[x+1];
          }
          prev=sy;
        }
        uint16_t *dst=strip+j*480;
        memcpy(dst,row,sizeof(row));
        overlayLine(dst,y+j,keys);
      }
      lcd.draw16bitRGBBitmap(0,y,strip,480,STRIP);
    }
}
}
