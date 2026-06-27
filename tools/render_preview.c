// 用固件「真身」渲染逻辑在主机上把各表情渲成 PNG（README 用图）。
// 这里的 SDF/分层合成与 fw/main/display.c 的 draw_face_frame 一字不差（拷贝自固件），
// 只把推屏换成写 PPM，保证图= 设备实际画面。  cc -O2 -o /tmp/rp render_preview.c -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#define LCD 466
#define CXC 233
#define EYEY 227
#define GAP 92

typedef enum { EYE_ROUND, EYE_WIDE, EYE_HAPPY, EYE_SLEEPY, EYE_STRAIN, EYE_SQUEEZE } eye_shape_t;
typedef struct {
    const char *name; eye_shape_t shape; float hw, hh, glow, breathe;
    bool blink, shake, blush, sweat, tear; const char *bubble;
} mood_t;

// —— 与 fw/main/mood.c 的 MOODS 一致 ——
static const mood_t MOODS[] = {
    {"happy",     EYE_ROUND,   38, 52, 26, 1.0f, true,  false, true,  false, false, ""},
    {"grin",      EYE_HAPPY,   46, 34, 24, 1.0f, false, false, true,  false, false, ""},
    {"busy",      EYE_STRAIN,  38, 28, 16, 1.9f, true,  false, false, true,  false, ""},
    {"surprised", EYE_WIDE,    48, 60, 34, 1.0f, true,  false, true,  false, false, ""},
    {"offline",   EYE_SQUEEZE, 40, 44, 14, 1.0f, false, true,  false, false, true,  ""},
    {"sleepy",    EYE_SLEEPY,  42, 30, 12, 0.5f, false, false, false, false, false, ""},
    {"wilt",      EYE_STRAIN,  38, 26, 14, 0.7f, true,  false, false, false, false, ""},
    {"pet",       EYE_HAPPY,   48, 30, 28, 1.0f, false, false, true,  false, false, ""},
};
#define N (int)(sizeof(MOODS)/sizeof(MOODS[0]))

// —— 以下 SDF/合成函数拷贝自 fw/main/display.c ——
static inline float rbox_dist(float px, float py, float ex, float ey, float hw, float hh, float cr) {
    float qx = fabsf(px-ex)-(hw-cr), qy = fabsf(py-ey)-(hh-cr);
    float mx = qx>0?qx:0, my = qy>0?qy:0;
    float outside = (mx>0&&my>0)?sqrtf(mx*mx+my*my):(mx+my);
    float inside = (qx>qy?qx:qy); if (inside>0) inside=0;
    return outside+inside-cr;
}
static inline float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx=bx-ax, vy=by-ay, wx=px-ax, wy=py-ay, c2=vx*vx+vy*vy;
    float t = c2>0?(vx*wx+vy*wy)/c2:0; if (t<0)t=0; else if (t>1)t=1;
    float dx=px-(ax+t*vx), dy=py-(ay+t*vy); return sqrtf(dx*dx+dy*dy);
}
static float eye_sdf(eye_shape_t shape, float px, float py, float ex, float ey, float hw, float hh) {
    switch (shape) {
        default: case EYE_ROUND: case EYE_WIDE: case EYE_STRAIN:
            return rbox_dist(px,py,ex,ey,hw,hh,fminf(hw,hh));
        case EYE_HAPPY: { const float th=7; float cxp=px<ex-hw?ex-hw:(px>ex+hw?ex+hw:px);
            float t=(cxp-ex)/hw, yc=ey-hh*0.7f*(1-t*t), dx=px-cxp, dy=py-yc; return sqrtf(dx*dx+dy*dy)-th; }
        case EYE_SLEEPY: { const float th=7; float cxp=px<ex-hw?ex-hw:(px>ex+hw?ex+hw:px);
            float t=(cxp-ex)/hw, yc=ey+hh*0.6f*(1-t*t), dx=px-cxp, dy=py-yc; return sqrtf(dx*dx+dy*dy)-th; }
        case EYE_SQUEEZE: { const float th=6;
            float d1=seg_dist(px,py,ex-hw,ey-0.4f*hh,ex-0.05f*hw,ey);
            float d2=seg_dist(px,py,ex-0.05f*hw,ey,ex-hw,ey+0.4f*hh);
            float d3=seg_dist(px,py,ex+hw,ey-0.4f*hh,ex+0.05f*hw,ey);
            float d4=seg_dist(px,py,ex+0.05f*hw,ey,ex+hw,ey+0.4f*hh);
            return fminf(fminf(d1,d2),fminf(d3,d4))-th; }
    }
}
static inline bool in_drop(float px, float py, float cx, float cy, float r) {
    float dx=px-cx, dy=py-cy; if (dx*dx+dy*dy<=r*r) return true;
    if (dy<0 && dy>-1.9f*r) { float tt=-dy/(1.9f*r); if (fabsf(dx)<=r*0.75f*(1-tt)) return true; }
    return false;
}

static uint8_t img[LCD][LCD][3];

static void put565(int x, int y, uint16_t c) {
    int R=(c>>11)&31, G=(c>>5)&63, B=c&31;
    img[y][x][0]=(R<<3)|(R>>2); img[y][x][1]=(G<<2)|(G>>4); img[y][x][2]=(B<<3)|(B>>2);
}
static void stamp(float cx, float cy, float r, uint16_t color) {
    for (int y=(int)(cy-1.9f*r); y<=(int)(cy+r); y++) for (int x=(int)(cx-r); x<=(int)(cx+r); x++)
        if (x>=0&&x<LCD&&y>=0&&y<LCD&&in_drop(x,y,cx,cy,r)) put565(x,y,color);
}

static void render(const mood_t *m) {
    memset(img, 0, sizeof img);
    const float eyeYc=EYEY, hw=m->hw, glow=m->glow;
    float hh=m->hh;
    const float exL=CXC-GAP, exR=CXC+GAP, cxg=CXC;
    const float bbx=hw+glow+6, bby=hh+glow+6;
    const bool blush=m->blush; const float blush_y=eyeYc+hh+glow*0.5f+8, BR=26;
    const float bxL=exL-hw*0.15f, bxR=exR+hw*0.15f;
    for (int py=0; py<LCD; py++) {
        bool in_bl = blush && fabsf(py-blush_y)<=BR;
        if (fabsf(py-eyeYc)>bby && !in_bl) continue;
        for (int px=0; px<LCD; px++) {
            float ex = px<cxg?exL:exR;
            if (fabsf(px-ex)>bbx) continue;
            float d = eye_sdf(m->shape,px,py,ex,eyeYc,hw,hh);
            int bR=0,bG=0,bB=0;
            if (blush) { float bcx=px<cxg?bxL:bxR, ddx=px-bcx, ddy=py-blush_y, rr=ddx*ddx+ddy*ddy;
                if (rr<BR*BR) { float bi=1-sqrtf(rr)/BR; bi*=bi; bR=14*bi; bG=4*bi; bB=5*bi; } }
            uint16_t c;
            if (d<=0) c=0xFFFF;
            else if (d<glow) { float gi=1-d/glow; gi*=gi;
                int R=bR+(int)(31*gi*0.85f); if(R>31)R=31; int G=bG+(int)(63*gi*0.92f); if(G>63)G=63;
                int B=bB+(int)(31*gi); if(B>31)B=31; c=(R<<11)|(G<<5)|B; }
            else c=(bR<<11)|(bG<<5)|bB;
            put565(px,py,c);
        }
    }
    if (m->sweat) stamp(exR+hw+9, eyeYc-hh-4, 6, (20<<11)|(44<<5)|31);
    if (m->tear) { uint16_t blue=(9<<11)|(28<<5)|31; stamp(exL, eyeYc+hh*0.5f, 5.5f, blue); stamp(exR, eyeYc+hh*0.5f, 5.5f, blue); }
    // 圆屏遮罩
    for (int y=0;y<LCD;y++) for (int x=0;x<LCD;x++){ int dx=x-233,dy=y-233; if(dx*dx+dy*dy>233*233){img[y][x][0]=img[y][x][1]=img[y][x][2]=16;} }
}

int main(void) {
    for (int i=0;i<N;i++) {
        render(&MOODS[i]);
        char fn[64]; snprintf(fn,sizeof fn,"/tmp/face_%s.ppm",MOODS[i].name);
        FILE *f=fopen(fn,"wb"); fprintf(f,"P6\n%d %d\n255\n",LCD,LCD); fwrite(img,1,sizeof img,f); fclose(f);
        printf("%s\n",fn);
    }
    return 0;
}
