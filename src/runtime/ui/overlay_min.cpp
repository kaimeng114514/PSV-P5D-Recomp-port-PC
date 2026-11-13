/* Minimal build's stand-in for runtime/ui/overlay.cpp (RECOMP_MINIMAL): every overlay entry point
 * in overlay.h as a no-op, so the game's runtime carries no overlay menu, no Dear ImGui, no
 * settings UI, no toasts and no HUD -- only the code that runs the game. Link this instead of
 * overlay.cpp + third_party/imgui (runtime/build.py, MINIMAL=1). */
#include "overlay.h"

extern "C" {

void overlay_toast_titled(const char *title, const char *body, double seconds) { (void)title; (void)body; (void)seconds; }
void overlay_toast(const char *msg, double seconds) { (void)msg; (void)seconds; }
int overlay_dialog_active(void) { return 0; }
int overlay_init(SDL_GPUDevice *device, SDL_Window *window, SDL_GPUTextureFormat color_fmt) { (void)device; (void)window; (void)color_fmt; return 1; }
void overlay_shutdown(void) {}
int overlay_is_open(void) { return 0; }
int overlay_layout_ask(void) { return 0; }
void overlay_open(void) {}
void overlay_close(void) {}
void overlay_toggle(void) {}
void overlay_set_paused(int on) { (void)on; }
int overlay_game_paused(void) { return 0; }
int overlay_mouse_captured(void) { return 0; }
void overlay_bar_dismiss(void) {}
int overlay_wants_quit(void) { return 0; }
int overlay_quit_confirm(void) { return 0; }
int overlay_wants_capture(void) { return 0; }
void overlay_process_event(const SDL_Event *event) { (void)event; }
void overlay_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s) { (void)cmd; (void)target; (void)w; (void)h; (void)dt_s; }
void overlay_touch_cursor(int on) { (void)on; }
void overlay_marks(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s, const OverlayMark *marks, int n, const float rear_rect[4]) { (void)cmd; (void)target; (void)w; (void)h; (void)dt_s; (void)marks; (void)n; (void)rear_rect; }
int overlay_draws_hud(void) { return 0; }
void overlay_render_forced(SDL_GPUDevice *device, SDL_GPUTexture *target, uint32_t w, uint32_t h, const char *ui) { (void)device; (void)target; (void)w; (void)h; (void)ui; }

}
