/* verify_scenes.c - render every recovered background through the port's own
 * renderer (atari_gfx.h) and dump each as a PPM.
 *
 * Paired with verify_scenes.py, which compares these against the reference model
 * recovered from the emulator captures. Both must agree to the pixel: that is what
 * makes "the port draws the original's backgrounds" a checked claim rather than a
 * visual impression.
 *
 * Build and run:  make verify-scenes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generated/scenes.h"

/* the logical screen is the Atari frame, as in the port */
#define W 384
#define H 240
static unsigned char fb[H][W][3];
static void setpx(int x,int y,int r,int g,int b){
    if(x<0||x>=W||y<0||y>=H) return;
    fb[y][x][0]=(unsigned char)r; fb[y][x][1]=(unsigned char)g; fb[y][x][2]=(unsigned char)b;
}
#include "atari_gfx.h"
int main(void){
    for(int i=0;i<BG_SCENE_COUNT;i++){
        memset(fb,0,sizeof fb);
        sceneDraw(&BG_SCENES[i], W, H, setpx);
        char p[64]; snprintf(p,sizeof p,"/tmp/cscene%d.ppm",i);
        FILE* f=fopen(p,"wb");
        fprintf(f,"P6\n%d %d\n255\n",W,H);
        fwrite(fb,1,sizeof fb,f);
        fclose(f);
    }
    printf("rendered %d scenes\n", BG_SCENE_COUNT);
    return 0;
}
