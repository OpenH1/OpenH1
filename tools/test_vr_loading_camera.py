"""Production first-picture handoff with motion larger than the culling margin."""
from pathlib import Path
import subprocess
import shutil
import pytest
ROOT = Path(__file__).resolve().parents[1]

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

def test_preparation_refreshes_culling_without_advancing_visuals_twice(tmp_path):
    refresh = function((ROOT/'port/linux/game/vr_render.c').read_text(), 'void vr_render_camera_refresh(')
    frame = function((ROOT/'source/render/render.c').read_text(), 'static void render_vr_player_frame(')
    source = r'''
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
static int halo_load_trace_enabled(void) {return 0;}
static void halo_load_trace(const char*p,const char*d) {(void)p;(void)d;}
typedef int boolean;
typedef float real;
#define TRUE 1
#define FALSE 0
#define HALO_VR_PASS_SCOPE 1
#define HALO_VR_PASS_EYES 2
#define HALO_VR_PASS_HUD 3
typedef struct { float i,j,k; } real_vector3d;
typedef struct { float x,y,z; } point;
struct render_camera { point position; real_vector3d forward,up; float z_near; };
struct render_window { struct render_camera render_camera,rasterizer_camera; };
struct halo_vr_view { float head_forward[3],head_up[3],head_position[3],eye_offset[2][3],fov[2][4],world_units_per_metre,seconds; };
static struct { struct halo_vr_view view; float base_yaw,listener_pitch,crouch_blended,crouch_head_drop; point head_position; } vr_render;
static int render_preparing_frame,render_vr_eye_pass,render_vr_scope_pass;
static float pose,culled,eye_pose,submitted;
static int pending,visual_updates,real_pictures,refreshes,ready,wall_updates;
static float death_offset;
static int refresh_failed;
static struct { int pending,playing; point eye; struct render_camera camera; } preparation_headwall;
static void vr_headwall_update(int playing,const point*eye,const point*head,const real_vector3d*f,const real_vector3d*u,float near,float units,float seconds) {
 (void)playing;(void)eye;(void)head;(void)u;(void)near;(void)units;(void)seconds;
 wall_updates++;assert(fabsf(atan2f(f->j,f->i)-pose)<0.00001f);assert(fabsf(head->x-pose*3)<0.00001f);
}

static void view_rotate(float yaw,float pitch,const float *p,real_vector3d *v) {
 (void)yaw;(void)pitch;v->i=p[0];v->j=p[1];v->k=p[2];
}
static float dot_product3d(const real_vector3d*a,const real_vector3d*b) {return a->i*b->i+a->j*b->j+a->k*b->k;}
static void cross_product3d(const real_vector3d*a,const real_vector3d*b,real_vector3d*c) {
 c->i=a->j*b->k-a->k*b->j;c->j=a->k*b->i-a->i*b->k;c->k=a->i*b->j-a->j*b->i;
}
static void view_pose(struct halo_vr_view*v) {
 v->head_forward[0]=cosf(pose);v->head_forward[1]=sinf(pose);v->head_forward[2]=0;
 v->head_up[0]=v->head_up[1]=0;v->head_up[2]=1;v->world_units_per_metre=1;v->head_position[0]=pose*3;v->head_position[2]=pose*2;
}
static int halo_vr_refresh_tracking(struct halo_vr_view*v) {refreshes++;if(refresh_failed)return 0;view_pose(v);return 1;}
static void camera_frusta(struct render_camera*c,struct render_camera*r) {
 *r=*c;culled=atan2f(c->forward.j,c->forward.i);eye_pose=culled;
}
''' + refresh + r'''
static int vr_render_camera(struct render_camera*c,struct render_camera*r,int preparing) {
 preparation_headwall.pending=preparing;
 visual_updates++;view_pose(&vr_render.view);
 c->position.x=pose*3;c->position.z=pose*2;vr_render.head_position=c->position;
 c->forward.i=cosf(pose);c->forward.j=sinf(pose);c->forward.k=0;
 c->up.i=c->up.j=0;c->up.k=1;preparation_headwall.camera=*c;c->position.x+=death_offset;camera_frusta(c,r);if(!preparing)wall_updates++;return 1;
}
static int render_enumerate_pending(int scope) {(void)scope;return pending;}
static int halo_vr_scope_size(void) {return 0;}
static void vr_cabin_display_frame(void) {}
static void vr_scope_enumerate_camera(struct render_camera*a,struct render_camera*b) {(void)a;(void)b;}
static void vr_render_scope_begin(void) {}
static void vr_render_scope_end(void) {}
static void halo_vr_pass(int p) {(void)p;}
static void render_player_frame(struct render_window*w,void*p) {
 (void)p;assert(render_vr_eye_pass || render_vr_scope_pass);
 if(render_preparing_frame) {pose=0.34906585f;pending=0;return;}
 assert(ready);assert(fabsf(w->render_camera.position.x-pose*3-death_offset)<0.00001f);real_pictures++;culled=atan2f(w->render_camera.forward.j,w->render_camera.forward.i);
 /* The host late-latch submits the same new pose; its residual is zero. */
 submitted=pose;assert(fabsf(culled-submitted)<0.00001f);assert(fabsf(eye_pose-submitted)<0.00001f);
}
static void vr_render_prepare_hud(int p) {(void)p;}
static void render_vr_hud(struct render_window*w,int p) {(void)w;(void)p;}
static void halo_loading_ready(void) {ready=1;}
static int vr_scope_camera(struct render_camera*a,struct render_camera*b) {(void)a;(void)b;return 0;}
static int vr_headwall_faded(void) {return 0;}
static void vr_render_end(void) {}
''' + frame + r'''
int main(void) {
 struct render_window window;
 struct render_camera before;
 memset(&window,0,sizeof(window));pending=1;death_offset=10;vr_render.crouch_blended=.5f;
 render_vr_player_frame(&window);
 assert(visual_updates==1 && refreshes==1 && real_pictures==1 && wall_updates==1);
 assert(fabsf(vr_render.head_position.x-pose*3)<1e-6f);
 assert(fabsf(vr_render.crouch_head_drop-pose)<1e-6f);
 assert(culled>0.034906585f); /* motion exceeds the old 2-degree margin */
 ready=0;render_vr_player_frame(&window);
 assert(visual_updates==2 && refreshes==1 && real_pictures==2 && wall_updates==2);
 /* Identical input does not add a floating-point round trip. */
 before=window.render_camera;vr_render_camera_refresh(&window.render_camera,&window.rasterizer_camera);
 assert(!memcmp(&before,&window.render_camera,sizeof(before)));
 /* A failed refresh still consumes exactly one pending collision update. */
 preparation_headwall.pending=1;refresh_failed=1;
 vr_render_camera_refresh(&window.render_camera,&window.rasterizer_camera);
 assert(wall_updates==3 && !preparation_headwall.pending);
 vr_render_camera_refresh(&window.render_camera,&window.rasterizer_camera);
 assert(wall_updates==3);
 return 0;
}
'''
    compiler=shutil.which('clang') or shutil.which('cc')
    if not compiler: pytest.skip('C compiler required')
    path=tmp_path/'probe.c';path.write_text(source)
    executable=tmp_path/'probe'
    subprocess.run([compiler,'-std=gnu89','-Wall','-Wextra','-Werror',str(path),'-lm','-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True,timeout=5)


def test_tracking_refresh_keeps_actions_clock_and_filters(tmp_path):
    text=(ROOT/'port/linux/src/vr.c').read_text()
    move=function(text,'static void tracking_direction_move(')
    refresh=function(text,'int halo_vr_refresh_tracking(')
    source=r'''
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
static int halo_load_trace_enabled(void) {return 0;}
static void halo_load_trace(const char*p,const char*d) {(void)p;(void)d;}
#include "vr.h"
struct vr_host_pose {float position[3],orientation[4];int valid;};
struct vr_host_views {struct vr_host_pose head,eye[2],aim[2],grip[2],gaze;float fov[2][4],display_elapsed,display_period;};
static struct {
 struct {struct vr_host_pose located_eye[2];unsigned int located_high,located_low;} latch;
 struct vr_host_views views;int views_unassigned;float origin_yaw,origin_position[3],two_handed_weight,trigger_touch;
 int throw_state;struct {int held;} reload;
 struct {float right_trigger,left_grip;} input;
 struct {float handle_forward[3],handle_up[3],handles[2][3];} seat;int bars_held[2];
} vr;
static struct {int controller_aim,floating_hands,arms;float weapon_offset[3];} settings;
static struct vr_host_views fresh;
static int located,partial;
#define THROW_IDLE 0
int halo_vr_frame_active(void) {return 1;}
static int host_vr_locate(struct vr_host_views*v) {if(partial) {memcpy(v->eye,fresh.eye,sizeof(v->eye));return 1;} *v=fresh;return 1;}
static void latch_located(void) {located++;}
static void test_head_apply(void) {}
static void test_fov_apply(void) {}
static void jitter_apply(void) {}
static void test_hands_apply(int input) {assert(!input);}
static void hands_assign_views(void) {vr.views_unassigned=0;}
static void test_point_apply(void) {}
static void shake_apply(void) {}
static void quaternion_rotate(const float*q,const float*v,float*out) {(void)q;memcpy(out,v,12);}
static void game_axes(const float*v,float yaw,float*out) {(void)yaw;memcpy(out,v,12);}
static void body_place(const float*h,const float*f,const float*u,float*n,float*e) {(void)f;(void)u;memcpy(n,h,12);memcpy(e,h,12);}
static void weapon_pointing(float*f,float*u) {f[0]=!located;f[1]=!!located;f[2]=0;u[0]=u[1]=0;u[2]=1;}
static int hand_controller(int hand) {return hand;}
''' + move + refresh + r'''
int main(void) {
 struct halo_vr_view view;
 memset(&view,0,sizeof(view));
 settings.controller_aim=settings.floating_hands=1;
 view.floating_hands=1;view.controller_aim=1;view.hand_forward[0]=1;view.hand_up[2]=1;
 view.aim_forward[0]=.8f;view.aim_forward[1]=.6f;
 view.seconds=.014f;view.hud_visor=1;view.hud_forward[0]=.123f;
 vr.views.display_elapsed=.014f;vr.views.display_period=.014f;
 vr.input.right_trigger=.7f;vr.input.left_grip=.3f;
 fresh.eye[0].valid=fresh.eye[1].valid=fresh.aim[0].valid=fresh.aim[1].valid=1;
 fresh.grip[0].valid=fresh.grip[1].valid=1;
 fresh.eye[0].position[0]=.97f;fresh.eye[1].position[0]=1.03f;
 fresh.aim[1].position[0]=2;fresh.grip[1].position[0]=3;fresh.grip[0].position[0]=4;
 fresh.gaze.valid=1;fresh.gaze.position[0]=.25f;
 fresh.display_elapsed=20;fresh.display_period=30;
 assert(halo_vr_refresh_tracking(&view));
 assert(vr.views.gaze.valid && vr.views.gaze.position[0]==.25f);
 assert(located==1 && view.head_position[0]==1 && view.hand_position[0]==2);
 assert(view.right_grip_position[0]==3 && view.left_grip_position[0]==4);
 assert(fabsf(view.aim_forward[0]+.6f)<1e-6f && fabsf(view.aim_forward[1]-.8f)<1e-6f);
 assert(view.seconds==.014f && vr.views.display_elapsed==.014f && vr.views.display_period==.014f);
 assert(view.hud_forward[0]==.123f && vr.input.right_trigger==.7f && vr.input.left_grip==.3f);
 view.controller_aim=0;assert(halo_vr_refresh_tracking(&view));
 assert(view.aim_forward[0]==0 && view.aim_forward[1]==1);
 /* A failed head/controller locate leaves those fields untouched. */
 vr.views.head.position[0]=7;vr.views.aim[1].position[0]=8;vr.views.grip[1].position[0]=9;partial=1;
 assert(halo_vr_refresh_tracking(&view));
 assert(vr.views.head.position[0]==7 && view.hand_position[0]==8 && view.right_grip_position[0]==9);
 fresh.eye[0].valid=0;assert(!halo_vr_refresh_tracking(&view));assert(located==3);
 return 0;
}
'''
    compiler=shutil.which('clang') or shutil.which('cc')
    if not compiler: pytest.skip('C compiler required')
    path=tmp_path/'tracking.c';path.write_text(source)
    executable=tmp_path/'tracking'
    subprocess.run([compiler,'-std=gnu89','-Wall','-Wextra','-Werror','-I',str(ROOT/'port/linux/src'),str(path),'-lm','-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True,timeout=5)
