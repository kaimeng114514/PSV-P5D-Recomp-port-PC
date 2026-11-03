#include <cstdio>
#include "sce_gxm.h"
#include "vulkan_backend.h"
#include "common/log.h"
#include "texture_format.h"
#include "../mem/address_space.h"
#include <cstdint>
#include <cstring>

using namespace p5d::vulkan;

// GXM draw 状态机（M1）：记录当前 context 绑定的 program / 顶点流
// 游戏代码将来按 SetVertexProgram -> SetVertexStream -> SetUniformData -> Draw 顺序调用，
// 这里保存绑定状态，由 sceGxmDraw 统一翻译为 Vulkan 绘制。
#define P5D_GXM_MAX_STREAMS 16
#define P5D_GXM_MAX_TEX_UNITS 16
struct GxmDrawState {
    SceGxmVertexProgram vertex_program = nullptr;
    SceGxmFragmentProgram fragment_program = nullptr;
    const void* vertex_streams[P5D_GXM_MAX_STREAMS] = {};
    SceUInt32 active_streams = 0;
    // SetFragmentTexture / SetVertexTexture 绑定的 16B SceGxmTexture 控制块指针
    const void* frag_textures[P5D_GXM_MAX_TEX_UNITS] = {};
    const void* vert_textures[P5D_GXM_MAX_TEX_UNITS] = {};
};
static GxmDrawState g_gxm_draw;

// 按位域打包 16B SceGxmTexture 控制块（对照 Vita3K SceGxmTexture 位域）
// type_index: 0 SWIZZLED / 2 CUBE / 3 LINEAR / 4 TILED；format=base<<24|swizzle<<12；data 为 PSV 虚拟字节地址
void p5d_gxm_pack_texture(SceUInt8* out, SceUInt32 type_index, const void* data,
                          SceUInt32 format, SceUInt32 width, SceUInt32 height, SceUInt32 mipCount) {
    auto wr=[&](int i,SceUInt32 v){ std::memcpy(out+i*4,&v,4); };
    SceUInt32 base5=(format>>24)&0x1F, swz=(format>>12)&0x7, fmt0=(format>>31)&0x1;
    // w0：默认 wrap=repeat(0)、linear filter、gamma=0；mip_count 字段=(mipCount-1)&15
    SceUInt32 mc = mipCount ? ((mipCount-1)&0xF) : 0;
    SceUInt32 w0 = (0<<3)|(0<<6)|(0<<9)|(1<<10)|(1<<12)|(mc<<17)|(0<<27)|(fmt0<<31);
    wr(0,w0);
    // w1：宽高编码随类型；base_format bits24:28；type bits29:31
    SceUInt32 wh;
    if (type_index==0 || type_index==2) { // SWIZZLED/CUBE：log2
        SceUInt32 lw=0,lh=0; SceUInt32 tw=width,th=height;
        while((1u<<lw)<tw && lw<15)lw++; while((1u<<lh)<th && lh<15)lh++;
        wh=(lh&0xF)|((lw&0xF)<<12);
    } else {
        wh=((height-1)&0xFFF)|(((width-1)&0xFFF)<<12);
    }
    SceUInt32 w1 = wh | (base5<<24) | ((type_index&0x7)<<29);
    wr(1,w1);
    // w2：data_addr 字段在 bits31:2（字地址），需 (byteAddr>>2) 再左移 2 对齐回 bit2
    uintptr_t byte=(uintptr_t)data;
    wr(2,(SceUInt32)(((byte>>2)&0x3FFFFFFFu)<<2));
    // w3：swizzle bits30:28
    wr(3,(swz&0x7)<<28);
}

const void* p5d_gxm_get_fragment_texture(SceUInt32 unit) {
    return unit<P5D_GXM_MAX_TEX_UNITS ? g_gxm_draw.frag_textures[unit] : nullptr;
}

void p5d_gxm_fill_fragment_controls(const void* out[16]) {
    for(int i=0;i<16;i++) out[i]=g_gxm_draw.frag_textures[i];
}

// GXM翻译层实现
// 将PSV GXM调用映射到Vulkan
// 参考Vita3K的renderer模块实现

extern "C" SceInt32 _sceGxmBeginScene(void) {
    STUB_LOG("_sceGxmBeginScene");
    return 0;
}

extern "C" SceInt32 _sceGxmMidSceneFlush(void) {
    STUB_LOG("_sceGxmMidSceneFlush");
    return 0;
}

extern "C" SceInt32 _sceGxmProgramFindParameterBySemantic(void) {
    STUB_LOG("_sceGxmProgramFindParameterBySemantic");
    return 0;
}

extern "C" SceInt32 _sceGxmProgramParameterGetSemantic(void) {
    STUB_LOG("_sceGxmProgramParameterGetSemantic");
    return 0;
}

extern "C" SceInt32 _sceGxmSetVertexTexture(void) {
    STUB_LOG("_sceGxmSetVertexTexture");
    return 0;
}

extern "C" SceInt32 _sceGxmTextureSetHeight(void) {
    STUB_LOG("_sceGxmTextureSetHeight");
    return 0;
}

extern "C" SceInt32 _sceGxmTextureSetWidth(void) {
    STUB_LOG("_sceGxmTextureSetWidth");
    return 0;
}

extern "C" SceInt32 sceGxmAddRazorGpuCaptureBuffer(void) {
    STUB_LOG("sceGxmAddRazorGpuCaptureBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmBeginCommandList(void) {
    STUB_LOG("sceGxmBeginCommandList");
    return 0;
}

extern "C" SceInt32 sceGxmBeginScene(SceGxmContext context, SceUInt32 flags,
                                     const SceGxmRenderTarget* renderTarget,
                                     const SceGxmColorSurface* colorSurface,
                                     const SceGxmDepthStencilSurface* depthStencilSurface,
                                     SceGxmSyncObject* vertexSyncObject,
                                     SceGxmSyncObject* fragmentSyncObject) {
    if (g_backend.initialized) {
        // begin_frame内部会调用cmd_begin_render_pass（含清除值、视口、裁剪）
        begin_frame();
        LOGD("sceGxmBeginScene: render pass started (frame=%llu)",
             (unsigned long long)g_backend.frame_count);
    }
    return 0;
}

extern "C" SceInt32 sceGxmBeginSceneEx(void) {
    STUB_LOG("sceGxmBeginSceneEx");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetClip(void) {
    STUB_LOG("sceGxmColorSurfaceGetClip");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetData(void) {
    STUB_LOG("sceGxmColorSurfaceGetData");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetDitherMode(void) {
    STUB_LOG("sceGxmColorSurfaceGetDitherMode");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetFormat(void) {
    STUB_LOG("sceGxmColorSurfaceGetFormat");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetGammaMode(void) {
    STUB_LOG("sceGxmColorSurfaceGetGammaMode");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetScaleMode(void) {
    STUB_LOG("sceGxmColorSurfaceGetScaleMode");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetStrideInPixels(void) {
    STUB_LOG("sceGxmColorSurfaceGetStrideInPixels");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceGetType(void) {
    STUB_LOG("sceGxmColorSurfaceGetType");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceInit(void) {
    STUB_LOG("sceGxmColorSurfaceInit");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceInitDisabled(void) {
    STUB_LOG("sceGxmColorSurfaceInitDisabled");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceIsEnabled(void) {
    STUB_LOG("sceGxmColorSurfaceIsEnabled");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceSetClip(void) {
    STUB_LOG("sceGxmColorSurfaceSetClip");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceSetData(void) {
    STUB_LOG("sceGxmColorSurfaceSetData");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceSetDitherMode(void) {
    STUB_LOG("sceGxmColorSurfaceSetDitherMode");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceSetFormat(void) {
    STUB_LOG("sceGxmColorSurfaceSetFormat");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceSetGammaMode(void) {
    STUB_LOG("sceGxmColorSurfaceSetGammaMode");
    return 0;
}

extern "C" SceInt32 sceGxmColorSurfaceSetScaleMode(void) {
    STUB_LOG("sceGxmColorSurfaceSetScaleMode");
    return 0;
}

extern "C" SceInt32 sceGxmCreateContext(const SceGxmContextParams* params, SceGxmContext* context) {
    STUB_LOG("sceGxmCreateContext");
    return 0;
}

extern "C" SceInt32 sceGxmCreateDeferredContext(void) {
    STUB_LOG("sceGxmCreateDeferredContext");
    return 0;
}

extern "C" SceInt32 sceGxmCreateRenderTarget(const SceGxmRenderTargetParams* params, SceGxmRenderTarget* renderTarget) {
    STUB_LOG("sceGxmCreateRenderTarget");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetBackgroundDepth(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetBackgroundDepth");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetBackgroundMask(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetBackgroundMask");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetBackgroundStencil(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetBackgroundStencil");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetForceLoadMode(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetForceLoadMode");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetForceStoreMode(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetForceStoreMode");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetFormat(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetFormat");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceGetStrideInSamples(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceGetStrideInSamples");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceInit(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceInit");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceInitDisabled(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceInitDisabled");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceIsEnabled(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceIsEnabled");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceSetBackgroundDepth(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceSetBackgroundDepth");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceSetBackgroundMask(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceSetBackgroundMask");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceSetBackgroundStencil(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceSetBackgroundStencil");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceSetForceLoadMode(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceSetForceLoadMode");
    return 0;
}

extern "C" SceInt32 sceGxmDepthStencilSurfaceSetForceStoreMode(void) {
    STUB_LOG("sceGxmDepthStencilSurfaceSetForceStoreMode");
    return 0;
}

extern "C" SceInt32 sceGxmDestroyContext(SceGxmContext context) {
    STUB_LOG("sceGxmDestroyContext");
    return 0;
}

extern "C" SceInt32 sceGxmDestroyDeferredContext(void) {
    STUB_LOG("sceGxmDestroyDeferredContext");
    return 0;
}

extern "C" SceInt32 sceGxmDestroyRenderTarget(SceGxmRenderTarget renderTarget) {
    STUB_LOG("sceGxmDestroyRenderTarget");
    return 0;
}

extern "C" SceInt32 sceGxmDisplayQueueAddEntry(SceGxmSyncObject* oldSyncObject, SceGxmSyncObject* newSyncObject, const SceGxmColorSurface* surface) {
    STUB_LOG("sceGxmDisplayQueueAddEntry");
    return 0;
}

extern "C" SceInt32 sceGxmDisplayQueueFinish(void) {
    STUB_LOG("sceGxmDisplayQueueFinish");
    return 0;
}

// ★ ARCH_525：查询"逐 draw 日志当前是否开启"（实现见 vulkan_backend.cpp），
//   让本文件的 `[DRAWVA]` 与那边的 `[DRAWLOG]` 共用同一个按内容触发的窗口。
//   `extern "C"` 必须放在全局作用域（函数体内会报 C2598）。
extern "C" int p5d_drawlog_active(void);

extern "C" SceInt32 sceGxmDraw(SceGxmContext context, SceUInt32 mode, SceUInt32 indexType,
                               SceUInt32 count, const void* indices) {
    if (g_backend.initialized && g_backend.current_cmd_buffer) {
        // draw 前：把当前绑定的 fragment 纹理控制块按 vaddr 自动 materialize 到 Vulkan unit（带脏缓存）
        const void* ctrls[16]; p5d_gxm_fill_fragment_controls(ctrls);
        p5d::vulkan::MaterializeStats ms = p5d::vulkan::materialize_fragment_textures(ctrls);
        if(ms.uploaded||ms.missing_addr||ms.failed)
            LOGD("sceGxmDraw materialize: uploaded=%u cached=%u missing=%u failed=%u",ms.uploaded,ms.cached,ms.missing_addr,ms.failed);
        // 真实Vulkan绘制调用
        // ★ ARCH_525：与 `[DRAWLOG]` 共用**同一个按内容触发的窗口**（见 vulkan_backend.cpp 的
        //   `p5d_drawlog_active()`）⇒ 两边严格 1:1 对齐，配对不必靠猜顺序。
        //   ARCH_524 之所以 0 条，是因为用帧号 `P5D_DRAWVA=<fc>` 指定窗口，而**帧号↔场景会漂移**。
        //   （声明放在全局作用域 —— 函数体内不允许 `extern "C"`，编译器报 C2598。）
        { static int _dvbegin=0;
          if(p5d_drawlog_active()){
            if(!_dvbegin){ _dvbegin=1;
              printf("[DRAWVA-BEGIN] fc=%lld\n",(long long)g_backend.frame_count); fflush(stdout); }
            printf("[DRAWVA] fc=%lld count=%u s0=%p s1=%p s2=%p active=%u\n",
                   (long long)g_backend.frame_count,(unsigned)count,
                   g_gxm_draw.vertex_streams[0],g_gxm_draw.vertex_streams[1],g_gxm_draw.vertex_streams[2],
                   (unsigned)g_gxm_draw.active_streams);
            fflush(stdout); } }
        gxm_draw_arrays(count);  // 状态机驱动：当前 program/pipeline + 顶点流
        LOGD("sceGxmDraw: vkCmdDrawIndexed count=%d", count);
    }
    return 0;
}

extern "C" SceInt32 sceGxmDrawInstanced(void) {
    STUB_LOG("sceGxmDrawInstanced");
    return 0;
}

extern "C" SceInt32 sceGxmDrawPrecomputed(void) {
    STUB_LOG("sceGxmDrawPrecomputed");
    return 0;
}

extern "C" SceInt32 sceGxmEndCommandList(void) {
    STUB_LOG("sceGxmEndCommandList");
    return 0;
}

extern "C" SceInt32 sceGxmEndScene(SceGxmContext context, SceGxmSyncObject* vertexSyncObject,
                                   SceGxmSyncObject* fragmentSyncObject) {
    if (g_backend.initialized) {
        // end_frame内部会调用cmd_end_render_pass + 提交命令缓冲区
        end_frame();
        LOGD("sceGxmEndScene: frame submitted");
    }
    return 0;
}

extern "C" SceInt32 sceGxmExecuteCommandList(void) {
    STUB_LOG("sceGxmExecuteCommandList");
    return 0;
}

extern "C" SceInt32 sceGxmFinish(void) {
    STUB_LOG("sceGxmFinish");
    return 0;
}

extern "C" SceInt32 sceGxmFragmentProgramGetPassType(void) {
    STUB_LOG("sceGxmFragmentProgramGetPassType");
    return 0;
}

extern "C" SceInt32 sceGxmFragmentProgramGetProgram(void) {
    STUB_LOG("sceGxmFragmentProgramGetProgram");
    return 0;
}

extern "C" SceInt32 sceGxmFragmentProgramIsEnabled(void) {
    STUB_LOG("sceGxmFragmentProgramIsEnabled");
    return 0;
}

extern "C" SceInt32 sceGxmGetContextType(void) {
    STUB_LOG("sceGxmGetContextType");
    return 0;
}

extern "C" SceInt32 sceGxmGetDeferredContextFragmentBuffer(void) {
    STUB_LOG("sceGxmGetDeferredContextFragmentBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmGetDeferredContextVdmBuffer(void) {
    STUB_LOG("sceGxmGetDeferredContextVdmBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmGetDeferredContextVertexBuffer(void) {
    STUB_LOG("sceGxmGetDeferredContextVertexBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmGetNotificationRegion(void) {
    STUB_LOG("sceGxmGetNotificationRegion");
    return 0;
}

extern "C" SceInt32 sceGxmGetParameterBufferThreshold(void) {
    STUB_LOG("sceGxmGetParameterBufferThreshold");
    return 0;
}

extern "C" SceInt32 sceGxmGetPrecomputedDrawSize(void) {
    STUB_LOG("sceGxmGetPrecomputedDrawSize");
    return 0;
}

extern "C" SceInt32 sceGxmGetPrecomputedFragmentStateSize(void) {
    STUB_LOG("sceGxmGetPrecomputedFragmentStateSize");
    return 0;
}

extern "C" SceInt32 sceGxmGetPrecomputedVertexStateSize(void) {
    STUB_LOG("sceGxmGetPrecomputedVertexStateSize");
    return 0;
}

extern "C" SceInt32 sceGxmGetRenderTargetMemSize(void) {
    STUB_LOG("sceGxmGetRenderTargetMemSize");
    return 0;
}

extern "C" SceInt32 sceGxmInitialize(const SceGxmInitializeParams* params) {
    LOGI("sceGxmInitialize called");
    if (params) {
        LOGI("  frontendBufferSize=%d, globalVertexBufferSize=%d, globalFragmentBufferSize=%d",
             params->frontendBufferSize, params->globalVertexBufferSize, params->globalFragmentBufferSize);
    }
    // 初始化Vulkan后端
    SceInt32 ret = backend_init();
    if (ret != 0) {
        LOGE("Vulkan backend init failed, falling back to stub mode");
    }
    return 0;
}

extern "C" SceInt32 sceGxmIsDebugVersion(void) {
    STUB_LOG("sceGxmIsDebugVersion");
    return 0;
}

extern "C" void* sceGxmMapFragmentUsseMemory(void* base, SceUInt32 size, SceUInt32* offset) {
    STUB_LOG("sceGxmMapFragmentUsseMemory");
    return 0;
}

extern "C" SceInt32 sceGxmMapMemory(void* addr, SceUInt32 size, SceInt32 flags) {
    SceUInt32 vaddr=(SceUInt32)(uintptr_t)addr;
    bool known = p5d::mem::translate(vaddr)!=nullptr;
    LOGI("sceGxmMapMemory vaddr=0x%08X size=%u flags=%d registered=%d",vaddr,size,(int)flags,(int)known);
    return 0; // 显存块由 sceKernel 分配时已 register_region，这里只做 GXM 可见性标记
}

extern "C" void* sceGxmMapVertexUsseMemory(void* base, SceUInt32 size, SceUInt32* offset) {
    STUB_LOG("sceGxmMapVertexUsseMemory");
    return 0;
}

extern "C" SceInt32 sceGxmMidSceneFlush(void) {
    STUB_LOG("sceGxmMidSceneFlush");
    return 0;
}

extern "C" SceInt32 sceGxmNotificationWait(void) {
    STUB_LOG("sceGxmNotificationWait");
    return 0;
}

extern "C" SceInt32 sceGxmPadHeartbeat(void) {
    STUB_LOG("sceGxmPadHeartbeat");
    return 0;
}

extern "C" SceInt32 sceGxmPadTriggerGpuPaTrace(void) {
    STUB_LOG("sceGxmPadTriggerGpuPaTrace");
    return 0;
}

extern "C" SceInt32 sceGxmPopUserMarker(void) {
    STUB_LOG("sceGxmPopUserMarker");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedDrawInit(void) {
    STUB_LOG("sceGxmPrecomputedDrawInit");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedDrawSetAllVertexStreams(void) {
    STUB_LOG("sceGxmPrecomputedDrawSetAllVertexStreams");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedDrawSetParams(void) {
    STUB_LOG("sceGxmPrecomputedDrawSetParams");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedDrawSetParamsInstanced(void) {
    STUB_LOG("sceGxmPrecomputedDrawSetParamsInstanced");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedDrawSetVertexStream(void) {
    STUB_LOG("sceGxmPrecomputedDrawSetVertexStream");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateInit(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateInit");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateSetAllTextures(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateSetAllTextures");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateSetAllUniformBuffers(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateSetAllUniformBuffers");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateSetTexture(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateSetTexture");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedFragmentStateSetUniformBuffer(void) {
    STUB_LOG("sceGxmPrecomputedFragmentStateSetUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateGetDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateGetDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateInit(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateInit");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateSetAllTextures(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateSetAllTextures");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateSetAllUniformBuffers(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateSetAllUniformBuffers");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateSetDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateSetTexture(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateSetTexture");
    return 0;
}

extern "C" SceInt32 sceGxmPrecomputedVertexStateSetUniformBuffer(void) {
    STUB_LOG("sceGxmPrecomputedVertexStateSetUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmProgramCheck(void) {
    STUB_LOG("sceGxmProgramCheck");
    return 0;
}

extern "C" SceInt32 sceGxmProgramFindParameterByName(void) {
    STUB_LOG("sceGxmProgramFindParameterByName");
    return 0;
}

extern "C" SceInt32 sceGxmProgramFindParameterBySemantic(void) {
    STUB_LOG("sceGxmProgramFindParameterBySemantic");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetDefaultUniformBufferSize(void) {
    STUB_LOG("sceGxmProgramGetDefaultUniformBufferSize");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetFragmentProgramInputs(void) {
    STUB_LOG("sceGxmProgramGetFragmentProgramInputs");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetOutputRegisterFormat(void) {
    STUB_LOG("sceGxmProgramGetOutputRegisterFormat");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetParameter(void) {
    STUB_LOG("sceGxmProgramGetParameter");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetParameterCount(void) {
    STUB_LOG("sceGxmProgramGetParameterCount");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetSize(void) {
    STUB_LOG("sceGxmProgramGetSize");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetType(void) {
    STUB_LOG("sceGxmProgramGetType");
    return 0;
}

extern "C" SceInt32 sceGxmProgramGetVertexProgramOutputs(void) {
    STUB_LOG("sceGxmProgramGetVertexProgramOutputs");
    return 0;
}

extern "C" SceInt32 sceGxmProgramIsDepthReplaceUsed(void) {
    STUB_LOG("sceGxmProgramIsDepthReplaceUsed");
    return 0;
}

extern "C" SceInt32 sceGxmProgramIsDiscardUsed(void) {
    STUB_LOG("sceGxmProgramIsDiscardUsed");
    return 0;
}

extern "C" SceInt32 sceGxmProgramIsEquivalent(void) {
    STUB_LOG("sceGxmProgramIsEquivalent");
    return 0;
}

extern "C" SceInt32 sceGxmProgramIsFragColorUsed(void) {
    STUB_LOG("sceGxmProgramIsFragColorUsed");
    return 0;
}

extern "C" SceInt32 sceGxmProgramIsNativeColorUsed(void) {
    STUB_LOG("sceGxmProgramIsNativeColorUsed");
    return 0;
}

extern "C" SceInt32 sceGxmProgramIsSpriteCoordUsed(void) {
    STUB_LOG("sceGxmProgramIsSpriteCoordUsed");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetArraySize(void) {
    STUB_LOG("sceGxmProgramParameterGetArraySize");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetCategory(void) {
    STUB_LOG("sceGxmProgramParameterGetCategory");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetComponentCount(void) {
    STUB_LOG("sceGxmProgramParameterGetComponentCount");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetContainerIndex(void) {
    STUB_LOG("sceGxmProgramParameterGetContainerIndex");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetIndex(void) {
    STUB_LOG("sceGxmProgramParameterGetIndex");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetName(void) {
    STUB_LOG("sceGxmProgramParameterGetName");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetResourceIndex(void) {
    STUB_LOG("sceGxmProgramParameterGetResourceIndex");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetSemantic(void) {
    STUB_LOG("sceGxmProgramParameterGetSemantic");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetSemanticIndex(void) {
    STUB_LOG("sceGxmProgramParameterGetSemanticIndex");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterGetType(void) {
    STUB_LOG("sceGxmProgramParameterGetType");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterIsRegFormat(void) {
    STUB_LOG("sceGxmProgramParameterIsRegFormat");
    return 0;
}

extern "C" SceInt32 sceGxmProgramParameterIsSamplerCube(void) {
    STUB_LOG("sceGxmProgramParameterIsSamplerCube");
    return 0;
}

extern "C" SceInt32 sceGxmPushUserMarker(void) {
    STUB_LOG("sceGxmPushUserMarker");
    return 0;
}

extern "C" SceInt32 sceGxmRemoveRazorGpuCaptureBuffer(void) {
    STUB_LOG("sceGxmRemoveRazorGpuCaptureBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmRenderTargetGetDriverMemBlock(void) {
    STUB_LOG("sceGxmRenderTargetGetDriverMemBlock");
    return 0;
}

extern "C" SceInt32 sceGxmReserveFragmentDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmReserveFragmentDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmReserveVertexDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmReserveVertexDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackDepthBias(void) {
    STUB_LOG("sceGxmSetBackDepthBias");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackDepthFunc(void) {
    STUB_LOG("sceGxmSetBackDepthFunc");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackDepthWriteEnable(void) {
    STUB_LOG("sceGxmSetBackDepthWriteEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackFragmentProgramEnable(void) {
    STUB_LOG("sceGxmSetBackFragmentProgramEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackLineFillLastPixelEnable(void) {
    STUB_LOG("sceGxmSetBackLineFillLastPixelEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackPointLineWidth(void) {
    STUB_LOG("sceGxmSetBackPointLineWidth");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackPolygonMode(void) {
    STUB_LOG("sceGxmSetBackPolygonMode");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackStencilFunc(void) {
    STUB_LOG("sceGxmSetBackStencilFunc");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackStencilRef(void) {
    STUB_LOG("sceGxmSetBackStencilRef");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackVisibilityTestEnable(void) {
    STUB_LOG("sceGxmSetBackVisibilityTestEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackVisibilityTestIndex(void) {
    STUB_LOG("sceGxmSetBackVisibilityTestIndex");
    return 0;
}

extern "C" SceInt32 sceGxmSetBackVisibilityTestOp(void) {
    STUB_LOG("sceGxmSetBackVisibilityTestOp");
    return 0;
}

extern "C" SceInt32 sceGxmSetCullMode(void) {
    STUB_LOG("sceGxmSetCullMode");
    return 0;
}

extern "C" SceInt32 sceGxmSetDefaultRegionClipAndViewport(void) {
    STUB_LOG("sceGxmSetDefaultRegionClipAndViewport");
    return 0;
}

extern "C" SceInt32 sceGxmSetDeferredContextFragmentBuffer(void) {
    STUB_LOG("sceGxmSetDeferredContextFragmentBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetDeferredContextVdmBuffer(void) {
    STUB_LOG("sceGxmSetDeferredContextVdmBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetDeferredContextVertexBuffer(void) {
    STUB_LOG("sceGxmSetDeferredContextVertexBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetFragmentDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmSetFragmentDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetFragmentProgram(SceGxmContext context, SceGxmFragmentProgram program) {
    g_gxm_draw.fragment_program = program;  // 记录当前片元 GXP program
    LOGD("sceGxmSetFragmentProgram: %p", (void*)program);
    return 0;
}

extern "C" SceInt32 sceGxmSetFragmentTexture(SceGxmContext context, SceUInt32 textureUnit, const void* texture) {
    (void)context;
    if (textureUnit >= P5D_GXM_MAX_TEX_UNITS) return (SceInt32)0x80000000;
    g_gxm_draw.frag_textures[textureUnit] = texture;
    if (texture) {
        p5d::tex::ResolvedGxmTexture rt;
        if (p5d::tex::resolve_gxm_texture((const SceUInt8*)texture, rt))
            LOGI("SetFragmentTexture unit=%u %ux%u base=0x%08X vkfmt=%u comp=%u swz=(%u,%u,%u,%u) type=%u cube=%d vaddr=0x%08X",
                 textureUnit, rt.info.width, rt.info.height, rt.info.base_format, rt.vk_format, rt.component_count,
                 rt.swizzle_map.r, rt.swizzle_map.g, rt.swizzle_map.b, rt.swizzle_map.a,
                 rt.info.texture_type, (int)rt.info.is_cube, rt.info.data_byte_addr);
    }
    return 0;
}

extern "C" SceInt32 sceGxmSetFragmentUniformBuffer(void) {
    STUB_LOG("sceGxmSetFragmentUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontDepthBias(void) {
    STUB_LOG("sceGxmSetFrontDepthBias");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontDepthFunc(void) {
    STUB_LOG("sceGxmSetFrontDepthFunc");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontDepthWriteEnable(void) {
    STUB_LOG("sceGxmSetFrontDepthWriteEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontFragmentProgramEnable(void) {
    STUB_LOG("sceGxmSetFrontFragmentProgramEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontLineFillLastPixelEnable(void) {
    STUB_LOG("sceGxmSetFrontLineFillLastPixelEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontPointLineWidth(void) {
    STUB_LOG("sceGxmSetFrontPointLineWidth");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontPolygonMode(void) {
    STUB_LOG("sceGxmSetFrontPolygonMode");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontStencilFunc(void) {
    STUB_LOG("sceGxmSetFrontStencilFunc");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontStencilRef(void) {
    STUB_LOG("sceGxmSetFrontStencilRef");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontVisibilityTestEnable(void) {
    STUB_LOG("sceGxmSetFrontVisibilityTestEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontVisibilityTestIndex(void) {
    STUB_LOG("sceGxmSetFrontVisibilityTestIndex");
    return 0;
}

extern "C" SceInt32 sceGxmSetFrontVisibilityTestOp(void) {
    STUB_LOG("sceGxmSetFrontVisibilityTestOp");
    return 0;
}

extern "C" SceInt32 sceGxmSetPrecomputedFragmentState(void) {
    STUB_LOG("sceGxmSetPrecomputedFragmentState");
    return 0;
}

extern "C" SceInt32 sceGxmSetPrecomputedVertexState(void) {
    STUB_LOG("sceGxmSetPrecomputedVertexState");
    return 0;
}

extern "C" SceInt32 sceGxmSetRegionClip(void) {
    STUB_LOG("sceGxmSetRegionClip");
    return 0;
}

extern "C" SceInt32 sceGxmSetTwoSidedEnable(void) {
    STUB_LOG("sceGxmSetTwoSidedEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetUniformDataF(void) {
    STUB_LOG("sceGxmSetUniformDataF");
    return 0;
}

extern "C" SceInt32 sceGxmSetUserMarker(void) {
    STUB_LOG("sceGxmSetUserMarker");
    return 0;
}

extern "C" SceInt32 sceGxmSetValidationEnable(void) {
    STUB_LOG("sceGxmSetValidationEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetVertexDefaultUniformBuffer(void) {
    STUB_LOG("sceGxmSetVertexDefaultUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetVertexProgram(SceGxmContext context, SceGxmVertexProgram program) {
    g_gxm_draw.vertex_program = program;  // 记录当前顶点 GXP program（pipeline 由其翻译结果驱动）
    LOGD("sceGxmSetVertexProgram: %p", (void*)program);
    return 0;
}

extern "C" SceInt32 sceGxmSetVertexStream(SceGxmContext context, SceUInt32 stream, const void* data) {
    if (stream < P5D_GXM_MAX_STREAMS) {
        g_gxm_draw.vertex_streams[stream] = data;
        if (data && stream + 1 > g_gxm_draw.active_streams) g_gxm_draw.active_streams = stream + 1;
    }
    LOGD("sceGxmSetVertexStream: stream=%u data=%p", (unsigned)stream, data);
    return 0;
}

extern "C" SceInt32 sceGxmSetVertexTexture(SceGxmContext context, SceUInt32 textureUnit, const void* texture) {
    (void)context;
    if (textureUnit < P5D_GXM_MAX_TEX_UNITS) g_gxm_draw.vert_textures[textureUnit] = texture;
    return 0;
}

extern "C" SceInt32 sceGxmSetVertexUniformBuffer(void) {
    STUB_LOG("sceGxmSetVertexUniformBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetViewport(SceGxmContext context, SceFloat xOffset, SceFloat yOffset, SceFloat xScale, SceFloat yScale, SceFloat zScale, SceFloat zOffset) {
    STUB_LOG("sceGxmSetViewport");
    return 0;
}

extern "C" SceInt32 sceGxmSetViewportEnable(void) {
    STUB_LOG("sceGxmSetViewportEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetVisibilityBuffer(void) {
    STUB_LOG("sceGxmSetVisibilityBuffer");
    return 0;
}

extern "C" SceInt32 sceGxmSetWBufferEnable(void) {
    STUB_LOG("sceGxmSetWBufferEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetWClampEnable(void) {
    STUB_LOG("sceGxmSetWClampEnable");
    return 0;
}

extern "C" SceInt32 sceGxmSetWClampValue(void) {
    STUB_LOG("sceGxmSetWClampValue");
    return 0;
}

extern "C" SceInt32 sceGxmSetWarningEnabled(void) {
    STUB_LOG("sceGxmSetWarningEnabled");
    return 0;
}

extern "C" SceInt32 sceGxmSetYuvProfile(void) {
    STUB_LOG("sceGxmSetYuvProfile");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherAddRefFragmentProgram(void) {
    STUB_LOG("sceGxmShaderPatcherAddRefFragmentProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherAddRefVertexProgram(void) {
    STUB_LOG("sceGxmShaderPatcherAddRefVertexProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherCreate(const SceGxmShaderPatcherParams* params, SceGxmShaderPatcher* patcher) {
    STUB_LOG("sceGxmShaderPatcherCreate");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherCreateFragmentProgram(void) {
    STUB_LOG("sceGxmShaderPatcherCreateFragmentProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(void) {
    STUB_LOG("sceGxmShaderPatcherCreateMaskUpdateFragmentProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherCreateVertexProgram(void) {
    STUB_LOG("sceGxmShaderPatcherCreateVertexProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherDestroy(SceGxmShaderPatcher patcher) {
    STUB_LOG("sceGxmShaderPatcherDestroy");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherForceUnregisterProgram(void) {
    STUB_LOG("sceGxmShaderPatcherForceUnregisterProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetBufferMemAllocated(void) {
    STUB_LOG("sceGxmShaderPatcherGetBufferMemAllocated");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetFragmentProgramRefCount(void) {
    STUB_LOG("sceGxmShaderPatcherGetFragmentProgramRefCount");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetFragmentUsseMemAllocated(void) {
    STUB_LOG("sceGxmShaderPatcherGetFragmentUsseMemAllocated");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetHostMemAllocated(void) {
    STUB_LOG("sceGxmShaderPatcherGetHostMemAllocated");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetProgramFromId(void) {
    STUB_LOG("sceGxmShaderPatcherGetProgramFromId");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetUserData(void) {
    STUB_LOG("sceGxmShaderPatcherGetUserData");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetVertexProgramRefCount(void) {
    STUB_LOG("sceGxmShaderPatcherGetVertexProgramRefCount");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherGetVertexUsseMemAllocated(void) {
    STUB_LOG("sceGxmShaderPatcherGetVertexUsseMemAllocated");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherRegisterProgram(void) {
    STUB_LOG("sceGxmShaderPatcherRegisterProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherReleaseFragmentProgram(void) {
    STUB_LOG("sceGxmShaderPatcherReleaseFragmentProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherReleaseVertexProgram(void) {
    STUB_LOG("sceGxmShaderPatcherReleaseVertexProgram");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherSetUserData(void) {
    STUB_LOG("sceGxmShaderPatcherSetUserData");
    return 0;
}

extern "C" SceInt32 sceGxmShaderPatcherUnregisterProgram(void) {
    STUB_LOG("sceGxmShaderPatcherUnregisterProgram");
    return 0;
}

extern "C" SceInt32 sceGxmSyncObjectCreate(SceGxmSyncObject* syncObject) {
    STUB_LOG("sceGxmSyncObjectCreate");
    return 0;
}

extern "C" SceInt32 sceGxmSyncObjectDestroy(SceGxmSyncObject syncObject) {
    STUB_LOG("sceGxmSyncObjectDestroy");
    return 0;
}

extern "C" SceInt32 sceGxmTerminate(void) {
    LOGI("sceGxmTerminate");
    backend_shutdown();
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetData(void) {
    STUB_LOG("sceGxmTextureGetData");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetFormat(void) {
    STUB_LOG("sceGxmTextureGetFormat");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetGammaMode(void) {
    STUB_LOG("sceGxmTextureGetGammaMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetHeight(void) {
    STUB_LOG("sceGxmTextureGetHeight");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetLodBias(void) {
    STUB_LOG("sceGxmTextureGetLodBias");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetLodMin(void) {
    STUB_LOG("sceGxmTextureGetLodMin");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetMagFilter(void) {
    STUB_LOG("sceGxmTextureGetMagFilter");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetMinFilter(void) {
    STUB_LOG("sceGxmTextureGetMinFilter");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetMipFilter(void) {
    STUB_LOG("sceGxmTextureGetMipFilter");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetMipmapCount(void) {
    STUB_LOG("sceGxmTextureGetMipmapCount");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetMipmapCountUnsafe(void) {
    STUB_LOG("sceGxmTextureGetMipmapCountUnsafe");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetNormalizeMode(void) {
    STUB_LOG("sceGxmTextureGetNormalizeMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetPalette(void) {
    STUB_LOG("sceGxmTextureGetPalette");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetStride(void) {
    STUB_LOG("sceGxmTextureGetStride");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetType(void) {
    STUB_LOG("sceGxmTextureGetType");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetUAddrMode(void) {
    STUB_LOG("sceGxmTextureGetUAddrMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetUAddrModeSafe(void) {
    STUB_LOG("sceGxmTextureGetUAddrModeSafe");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetVAddrMode(void) {
    STUB_LOG("sceGxmTextureGetVAddrMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetVAddrModeSafe(void) {
    STUB_LOG("sceGxmTextureGetVAddrModeSafe");
    return 0;
}

extern "C" SceInt32 sceGxmTextureGetWidth(void) {
    STUB_LOG("sceGxmTextureGetWidth");
    return 0;
}

extern "C" SceInt32 sceGxmTextureInitCube(void) {
    STUB_LOG("sceGxmTextureInitCube");
    return 0;
}

extern "C" SceInt32 sceGxmTextureInitCubeArbitrary(void) {
    STUB_LOG("sceGxmTextureInitCubeArbitrary");
    return 0;
}

// h145k1091：逐次记录纹理创建（P5D_TEXINIT=1）
//   目的：570 次 sceGxmTextureInitLinear 里找出 w==960&&h==544
//   或 data 落在解码帧区间 [0xB1000000,0xB2000000) 的那一次。
static void p5d_texinit_log(const char* kind, SceUInt32 tex, const void* data,
                            SceUInt32 format, SceUInt32 width, SceUInt32 height, SceUInt32 mip) {
    static int _on = -1; static long long _n = 0;
    if (_on < 0) { _on = std::getenv("P5D_TEXINIT") ? 1 : 0; }
    if (!_on) return;
    ++_n;
    SceUInt32 d = (SceUInt32)(uintptr_t)data;
    const char* mark = "";
    if (width == 960u && height == 544u) mark = "  ★ 960x544";
    else if (d >= 0xB1000000u && d < 0xB2000000u) mark = "  ★ 解码帧区";
    if (_n <= 700 || mark[0])
        std::printf("[TEXINIT] #%lld %s tex=0x%08X data=0x%08X fmt=0x%X %ux%u mip=%u%s\n",
                    _n, kind, tex, d, format, width, height, mip, mark);
    std::fflush(stdout);
}

extern "C" SceInt32 sceGxmTextureInitLinear(SceGxmTexture texture, const void* data, SceUInt32 format, SceUInt32 width, SceUInt32 height, SceUInt32 mipCount) {
    if (!texture) return (SceInt32)0x80000000;
    p5d_texinit_log("Linear", (SceUInt32)(uintptr_t)texture, data, format, width, height, mipCount ? mipCount : 1u);
    p5d_gxm_pack_texture((SceUInt8*)texture, 3u, data, format, width, height, mipCount ? mipCount : 1u);
    return 0;
}

extern "C" SceInt32 sceGxmTextureInitLinearStrided(void) {
    STUB_LOG("sceGxmTextureInitLinearStrided");
    return 0;
}

extern "C" SceInt32 sceGxmTextureInitSwizzled(SceGxmTexture texture, const void* data, SceUInt32 format, SceUInt32 width, SceUInt32 height, SceUInt32 mipCount) {
    if (!texture) return (SceInt32)0x80000000;
    p5d_texinit_log("Swizzled", (SceUInt32)(uintptr_t)texture, data, format, width, height, mipCount ? mipCount : 1u);
    p5d_gxm_pack_texture((SceUInt8*)texture, 0u, data, format, width, height, mipCount ? mipCount : 1u);
    return 0;
}

extern "C" SceInt32 sceGxmTextureInitSwizzledArbitrary(void) {
    STUB_LOG("sceGxmTextureInitSwizzledArbitrary");
    return 0;
}

extern "C" SceInt32 sceGxmTextureInitTiled(SceGxmTexture texture, const void* data, SceUInt32 format, SceUInt32 width, SceUInt32 height, SceUInt32 mipCount) {
    if (!texture) return (SceInt32)0x80000000;
    p5d_gxm_pack_texture((SceUInt8*)texture, 4u, data, format, width, height, mipCount ? mipCount : 1u);
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetData(void) {
    STUB_LOG("sceGxmTextureSetData");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetFormat(void) {
    STUB_LOG("sceGxmTextureSetFormat");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetGammaMode(void) {
    STUB_LOG("sceGxmTextureSetGammaMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetHeight(void) {
    STUB_LOG("sceGxmTextureSetHeight");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetLodBias(void) {
    STUB_LOG("sceGxmTextureSetLodBias");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetLodMin(void) {
    STUB_LOG("sceGxmTextureSetLodMin");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetMagFilter(void) {
    STUB_LOG("sceGxmTextureSetMagFilter");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetMinFilter(void) {
    STUB_LOG("sceGxmTextureSetMinFilter");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetMipFilter(void) {
    STUB_LOG("sceGxmTextureSetMipFilter");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetMipmapCount(void) {
    STUB_LOG("sceGxmTextureSetMipmapCount");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetNormalizeMode(void) {
    STUB_LOG("sceGxmTextureSetNormalizeMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetPalette(void) {
    STUB_LOG("sceGxmTextureSetPalette");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetStride(void) {
    STUB_LOG("sceGxmTextureSetStride");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetUAddrMode(void) {
    STUB_LOG("sceGxmTextureSetUAddrMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetUAddrModeSafe(void) {
    STUB_LOG("sceGxmTextureSetUAddrModeSafe");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetVAddrMode(void) {
    STUB_LOG("sceGxmTextureSetVAddrMode");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetVAddrModeSafe(void) {
    STUB_LOG("sceGxmTextureSetVAddrModeSafe");
    return 0;
}

extern "C" SceInt32 sceGxmTextureSetWidth(void) {
    STUB_LOG("sceGxmTextureSetWidth");
    return 0;
}

extern "C" SceInt32 sceGxmTextureValidate(void) {
    STUB_LOG("sceGxmTextureValidate");
    return 0;
}

extern "C" SceInt32 sceGxmTransferCopy(void) {
    STUB_LOG("sceGxmTransferCopy");
    return 0;
}

extern "C" SceInt32 sceGxmTransferDownscale(void) {
    STUB_LOG("sceGxmTransferDownscale");
    return 0;
}

extern "C" SceInt32 sceGxmTransferFill(void) {
    STUB_LOG("sceGxmTransferFill");
    return 0;
}

extern "C" SceInt32 sceGxmTransferFinish(void) {
    STUB_LOG("sceGxmTransferFinish");
    return 0;
}

extern "C" SceInt32 sceGxmUnmapFragmentUsseMemory(void) {
    STUB_LOG("sceGxmUnmapFragmentUsseMemory");
    return 0;
}

extern "C" SceInt32 sceGxmUnmapMemory(void) {
    STUB_LOG("sceGxmUnmapMemory");
    return 0;
}

extern "C" SceInt32 sceGxmUnmapVertexUsseMemory(void) {
    STUB_LOG("sceGxmUnmapVertexUsseMemory");
    return 0;
}

extern "C" SceInt32 sceGxmVertexFence(void) {
    STUB_LOG("sceGxmVertexFence");
    return 0;
}

extern "C" SceInt32 sceGxmVertexProgramGetProgram(void) {
    STUB_LOG("sceGxmVertexProgramGetProgram");
    return 0;
}

extern "C" SceInt32 sceGxmWaitEvent(void) {
    STUB_LOG("sceGxmWaitEvent");
    return 0;
}

// === NID映射表（267个函数）===
extern "C" const SceGxmNidEntry g_scegxm_nid_map[] = {
    {0xDBA33160, (void*)_sceGxmBeginScene},
    {0x51FE0899, (void*)_sceGxmMidSceneFlush},
    {0x7FFFDD7A, (void*)_sceGxmProgramFindParameterBySemantic},
    {0xAAFD61D5, (void*)_sceGxmProgramParameterGetSemantic},
    {0x16C9D339, (void*)_sceGxmSetVertexTexture},
    {0x1B20D5DF, (void*)_sceGxmTextureSetHeight},
    {0x5A690B60, (void*)_sceGxmTextureSetWidth},
    {0xE9E81073, (void*)sceGxmAddRazorGpuCaptureBuffer},
    {0x944D3F83, (void*)sceGxmBeginCommandList},
    {0x8734FF4E, (void*)sceGxmBeginScene},
    {0x4709CF5A, (void*)sceGxmBeginSceneEx},
    {0x07DFEE4B, (void*)sceGxmColorSurfaceGetClip},
    {0x2DB6026C, (void*)sceGxmColorSurfaceGetData},
    {0x200A96E1, (void*)sceGxmColorSurfaceGetDitherMode},
    {0xF3C1C6C6, (void*)sceGxmColorSurfaceGetFormat},
    {0xEE0B4DF0, (void*)sceGxmColorSurfaceGetGammaMode},
    {0x6E3FA74D, (void*)sceGxmColorSurfaceGetScaleMode},
    {0xF33D9980, (void*)sceGxmColorSurfaceGetStrideInPixels},
    {0x52FDE962, (void*)sceGxmColorSurfaceGetType},
    {0xED0F6E25, (void*)sceGxmColorSurfaceInit},
    {0x613639FA, (void*)sceGxmColorSurfaceInitDisabled},
    {0x0E0EBB57, (void*)sceGxmColorSurfaceIsEnabled},
    {0x86456F7B, (void*)sceGxmColorSurfaceSetClip},
    {0x537CA400, (void*)sceGxmColorSurfaceSetData},
    {0x45027BAB, (void*)sceGxmColorSurfaceSetDitherMode},
    {0x5F9A3A16, (void*)sceGxmColorSurfaceSetFormat},
    {0xF5C89643, (void*)sceGxmColorSurfaceSetGammaMode},
    {0x6B96EDF7, (void*)sceGxmColorSurfaceSetScaleMode},
    {0xE84CE5B4, (void*)sceGxmCreateContext},
    {0x249D5B00, (void*)sceGxmCreateDeferredContext},
    {0x207AF96B, (void*)sceGxmCreateRenderTarget},
    {0x269B56BE, (void*)sceGxmDepthStencilSurfaceGetBackgroundDepth},
    {0x85D4DEFE, (void*)sceGxmDepthStencilSurfaceGetBackgroundMask},
    {0xAAFC062B, (void*)sceGxmDepthStencilSurfaceGetBackgroundStencil},
    {0x2F5CC20C, (void*)sceGxmDepthStencilSurfaceGetForceLoadMode},
    {0x544AA05A, (void*)sceGxmDepthStencilSurfaceGetForceStoreMode},
    {0x8504038D, (void*)sceGxmDepthStencilSurfaceGetFormat},
    {0x11628789, (void*)sceGxmDepthStencilSurfaceGetStrideInSamples},
    {0xCA9D41D1, (void*)sceGxmDepthStencilSurfaceInit},
    {0xA41DB0D6, (void*)sceGxmDepthStencilSurfaceInitDisabled},
    {0x082200E1, (void*)sceGxmDepthStencilSurfaceIsEnabled},
    {0x32F280F0, (void*)sceGxmDepthStencilSurfaceSetBackgroundDepth},
    {0xF28A688A, (void*)sceGxmDepthStencilSurfaceSetBackgroundMask},
    {0xF5D3F3E8, (void*)sceGxmDepthStencilSurfaceSetBackgroundStencil},
    {0x0C44ACD7, (void*)sceGxmDepthStencilSurfaceSetForceLoadMode},
    {0x12AAA7AF, (void*)sceGxmDepthStencilSurfaceSetForceStoreMode},
    {0xEDDC5FB2, (void*)sceGxmDestroyContext},
    {0xD6A2FF2F, (void*)sceGxmDestroyDeferredContext},
    {0x0B94C50A, (void*)sceGxmDestroyRenderTarget},
    {0xEC5C26B5, (void*)sceGxmDisplayQueueAddEntry},
    {0xB98C5B0D, (void*)sceGxmDisplayQueueFinish},
    {0xBC059AFC, (void*)sceGxmDraw},
    {0x14C4E7D3, (void*)sceGxmDrawInstanced},
    {0xED3F78B8, (void*)sceGxmDrawPrecomputed},
    {0x36D85916, (void*)sceGxmEndCommandList},
    {0xFE300E2F, (void*)sceGxmEndScene},
    {0xE8E139DD, (void*)sceGxmExecuteCommandList},
    {0x0733D8AE, (void*)sceGxmFinish},
    {0xCE0B0A76, (void*)sceGxmFragmentProgramGetPassType},
    {0xE0E3B3F8, (void*)sceGxmFragmentProgramGetProgram},
    {0x5748367E, (void*)sceGxmFragmentProgramIsEnabled},
    {0x2A1BCDDB, (void*)sceGxmGetContextType},
    {0xFA081D05, (void*)sceGxmGetDeferredContextFragmentBuffer},
    {0xAD8C2EBA, (void*)sceGxmGetDeferredContextVdmBuffer},
    {0x87632B9C, (void*)sceGxmGetDeferredContextVertexBuffer},
    {0x8BDE825A, (void*)sceGxmGetNotificationRegion},
    {0x6ABF3F76, (void*)sceGxmGetParameterBufferThreshold},
    {0x41BBD792, (void*)sceGxmGetPrecomputedDrawSize},
    {0x85DE8506, (void*)sceGxmGetPrecomputedFragmentStateSize},
    {0x9D83CA3B, (void*)sceGxmGetPrecomputedVertexStateSize},
    {0xB291C959, (void*)sceGxmGetRenderTargetMemSize},
    {0xB0F1E4EC, (void*)sceGxmInitialize},
    {0x873B07C0, (void*)sceGxmIsDebugVersion},
    {0x008402C6, (void*)sceGxmMapFragmentUsseMemory},
    {0xC61E34FC, (void*)sceGxmMapMemory},
    {0xFA437510, (void*)sceGxmMapVertexUsseMemory},
    {0x2B5C0444, (void*)sceGxmMidSceneFlush},
    {0x9F448E79, (void*)sceGxmNotificationWait},
    {0x3D25FCE9, (void*)sceGxmPadHeartbeat},
    {0x47E06984, (void*)sceGxmPadTriggerGpuPaTrace},
    {0x4FA073A6, (void*)sceGxmPopUserMarker},
    {0xA197F096, (void*)sceGxmPrecomputedDrawInit},
    {0xB6C6F571, (void*)sceGxmPrecomputedDrawSetAllVertexStreams},
    {0x884D0D08, (void*)sceGxmPrecomputedDrawSetParams},
    {0x3A7B1633, (void*)sceGxmPrecomputedDrawSetParamsInstanced},
    {0x6C936214, (void*)sceGxmPrecomputedDrawSetVertexStream},
    {0xCECB584A, (void*)sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer},
    {0xE297D7AF, (void*)sceGxmPrecomputedFragmentStateInit},
    {0xC383DE39, (void*)sceGxmPrecomputedFragmentStateSetAllTextures},
    {0x5A783DC3, (void*)sceGxmPrecomputedFragmentStateSetAllUniformBuffers},
    {0x91236858, (void*)sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer},
    {0x29118BF1, (void*)sceGxmPrecomputedFragmentStateSetTexture},
    {0xB452F1FB, (void*)sceGxmPrecomputedFragmentStateSetUniformBuffer},
    {0xBE5A68EF, (void*)sceGxmPrecomputedVertexStateGetDefaultUniformBuffer},
    {0xBE937F8D, (void*)sceGxmPrecomputedVertexStateInit},
    {0xC40C9127, (void*)sceGxmPrecomputedVertexStateSetAllTextures},
    {0x0389861D, (void*)sceGxmPrecomputedVertexStateSetAllUniformBuffers},
    {0x34BF64E3, (void*)sceGxmPrecomputedVertexStateSetDefaultUniformBuffer},
    {0x6A29EB06, (void*)sceGxmPrecomputedVertexStateSetTexture},
    {0xDBF97ED6, (void*)sceGxmPrecomputedVertexStateSetUniformBuffer},
    {0xED8B6C69, (void*)sceGxmProgramCheck},
    {0x277794C4, (void*)sceGxmProgramFindParameterByName},
    {0x633CAA54, (void*)sceGxmProgramFindParameterBySemantic},
    {0x8FA3F9C3, (void*)sceGxmProgramGetDefaultUniformBufferSize},
    {0xC6924709, (void*)sceGxmProgramGetFragmentProgramInputs},
    {0xE11603B1, (void*)sceGxmProgramGetOutputRegisterFormat},
    {0x06FF9151, (void*)sceGxmProgramGetParameter},
    {0xD5D5FCCD, (void*)sceGxmProgramGetParameterCount},
    {0xBF5E2090, (void*)sceGxmProgramGetSize},
    {0x04BB3C59, (void*)sceGxmProgramGetType},
    {0xFB01C7E5, (void*)sceGxmProgramGetVertexProgramOutputs},
    {0x89613EF2, (void*)sceGxmProgramIsDepthReplaceUsed},
    {0x029B4F1C, (void*)sceGxmProgramIsDiscardUsed},
    {0xE23C838C, (void*)sceGxmProgramIsEquivalent},
    {0x104F23F4, (void*)sceGxmProgramIsFragColorUsed},
    {0xA4297E57, (void*)sceGxmProgramIsNativeColorUsed},
    {0xE63C53D8, (void*)sceGxmProgramIsSpriteCoordUsed},
    {0xDBA8D061, (void*)sceGxmProgramParameterGetArraySize},
    {0x1997DC17, (void*)sceGxmProgramParameterGetCategory},
    {0xBD2998D1, (void*)sceGxmProgramParameterGetComponentCount},
    {0xBB58267D, (void*)sceGxmProgramParameterGetContainerIndex},
    {0x6E61DDF5, (void*)sceGxmProgramParameterGetIndex},
    {0x6AF88A5D, (void*)sceGxmProgramParameterGetName},
    {0x5C79D59A, (void*)sceGxmProgramParameterGetResourceIndex},
    {0xE6D9C4CE, (void*)sceGxmProgramParameterGetSemantic},
    {0xB85CC13E, (void*)sceGxmProgramParameterGetSemanticIndex},
    {0x7B9023C3, (void*)sceGxmProgramParameterGetType},
    {0x871E5009, (void*)sceGxmProgramParameterIsRegFormat},
    {0xF7AA978B, (void*)sceGxmProgramParameterIsSamplerCube},
    {0x3276C475, (void*)sceGxmPushUserMarker},
    {0x0468E3F2, (void*)sceGxmRemoveRazorGpuCaptureBuffer},
    {0x49553737, (void*)sceGxmRenderTargetGetDriverMemBlock},
    {0x7B1FABB6, (void*)sceGxmReserveFragmentDefaultUniformBuffer},
    {0x97118913, (void*)sceGxmReserveVertexDefaultUniformBuffer},
    {0x17B3BF86, (void*)sceGxmSetBackDepthBias},
    {0xB042A4D2, (void*)sceGxmSetBackDepthFunc},
    {0xC18B706B, (void*)sceGxmSetBackDepthWriteEnable},
    {0xE26B4834, (void*)sceGxmSetBackFragmentProgramEnable},
    {0xC88EB702, (void*)sceGxmSetBackLineFillLastPixelEnable},
    {0x8DCB0EDB, (void*)sceGxmSetBackPointLineWidth},
    {0xF66EC6FE, (void*)sceGxmSetBackPolygonMode},
    {0x1A68C8D2, (void*)sceGxmSetBackStencilFunc},
    {0x866A0517, (void*)sceGxmSetBackStencilRef},
    {0x17CF46B9, (void*)sceGxmSetBackVisibilityTestEnable},
    {0xAE7886FE, (void*)sceGxmSetBackVisibilityTestIndex},
    {0xC83F0AB3, (void*)sceGxmSetBackVisibilityTestOp},
    {0xE1CA72AE, (void*)sceGxmSetCullMode},
    {0x60CF708E, (void*)sceGxmSetDefaultRegionClipAndViewport},
    {0xC6B3FCD0, (void*)sceGxmSetDeferredContextFragmentBuffer},
    {0x0B9D13CE, (void*)sceGxmSetDeferredContextVdmBuffer},
    {0x27CAD127, (void*)sceGxmSetDeferredContextVertexBuffer},
    {0xA824EB24, (void*)sceGxmSetFragmentDefaultUniformBuffer},
    {0xAD2F48D9, (void*)sceGxmSetFragmentProgram},
    {0x29C34DF5, (void*)sceGxmSetFragmentTexture},
    {0xEA0FC310, (void*)sceGxmSetFragmentUniformBuffer},
    {0xAAA97F81, (void*)sceGxmSetFrontDepthBias},
    {0x14BD831F, (void*)sceGxmSetFrontDepthFunc},
    {0xF32CBF34, (void*)sceGxmSetFrontDepthWriteEnable},
    {0x575958A8, (void*)sceGxmSetFrontFragmentProgramEnable},
    {0x5765DE9F, (void*)sceGxmSetFrontLineFillLastPixelEnable},
    {0x06752183, (void*)sceGxmSetFrontPointLineWidth},
    {0xFD93209D, (void*)sceGxmSetFrontPolygonMode},
    {0xB8645A9A, (void*)sceGxmSetFrontStencilFunc},
    {0x8FA6FE44, (void*)sceGxmSetFrontStencilRef},
    {0x30459117, (void*)sceGxmSetFrontVisibilityTestEnable},
    {0x12625C34, (void*)sceGxmSetFrontVisibilityTestIndex},
    {0xD0E3CD9A, (void*)sceGxmSetFrontVisibilityTestOp},
    {0xF8952750, (void*)sceGxmSetPrecomputedFragmentState},
    {0xB7626A93, (void*)sceGxmSetPrecomputedVertexState},
    {0x70C86868, (void*)sceGxmSetRegionClip},
    {0x0DE9AEB7, (void*)sceGxmSetTwoSidedEnable},
    {0x65DD0C84, (void*)sceGxmSetUniformDataF},
    {0xC7A8CB77, (void*)sceGxmSetUserMarker},
    {0x8C6A24C9, (void*)sceGxmSetValidationEnable},
    {0xC697CAE5, (void*)sceGxmSetVertexDefaultUniformBuffer},
    {0x31FF8ABD, (void*)sceGxmSetVertexProgram},
    {0x895DF2E9, (void*)sceGxmSetVertexStream},
    {0x9EB4380F, (void*)sceGxmSetVertexTexture},
    {0xC68015E4, (void*)sceGxmSetVertexUniformBuffer},
    {0x3EB3380B, (void*)sceGxmSetViewport},
    {0x814F61EB, (void*)sceGxmSetViewportEnable},
    {0x7767EC49, (void*)sceGxmSetVisibilityBuffer},
    {0xEED86975, (void*)sceGxmSetWBufferEnable},
    {0x1BF8B853, (void*)sceGxmSetWClampEnable},
    {0xD096336E, (void*)sceGxmSetWClampValue},
    {0x12D18B3D, (void*)sceGxmSetWarningEnabled},
    {0xB32917F0, (void*)sceGxmSetYuvProfile},
    {0x4CD2D19F, (void*)sceGxmShaderPatcherAddRefFragmentProgram},
    {0x0FD1E589, (void*)sceGxmShaderPatcherAddRefVertexProgram},
    {0x05032658, (void*)sceGxmShaderPatcherCreate},
    {0x4ED2E49D, (void*)sceGxmShaderPatcherCreateFragmentProgram},
    {0xA4433427, (void*)sceGxmShaderPatcherCreateMaskUpdateFragmentProgram},
    {0xB7BBA6D5, (void*)sceGxmShaderPatcherCreateVertexProgram},
    {0xEAA5B100, (void*)sceGxmShaderPatcherDestroy},
    {0x630D4B2E, (void*)sceGxmShaderPatcherForceUnregisterProgram},
    {0xC694D039, (void*)sceGxmShaderPatcherGetBufferMemAllocated},
    {0x2C5550F0, (void*)sceGxmShaderPatcherGetFragmentProgramRefCount},
    {0x3C9DDB4A, (void*)sceGxmShaderPatcherGetFragmentUsseMemAllocated},
    {0x9DBBC71C, (void*)sceGxmShaderPatcherGetHostMemAllocated},
    {0xA949A803, (void*)sceGxmShaderPatcherGetProgramFromId},
    {0x96A7E6DD, (void*)sceGxmShaderPatcherGetUserData},
    {0xA1A16FF6, (void*)sceGxmShaderPatcherGetVertexProgramRefCount},
    {0x7D2F83C1, (void*)sceGxmShaderPatcherGetVertexUsseMemAllocated},
    {0x2B528462, (void*)sceGxmShaderPatcherRegisterProgram},
    {0xBE2743D1, (void*)sceGxmShaderPatcherReleaseFragmentProgram},
    {0xAC1FF2DA, (void*)sceGxmShaderPatcherReleaseVertexProgram},
    {0xF9B8FCFD, (void*)sceGxmShaderPatcherSetUserData},
    {0xF103AF8A, (void*)sceGxmShaderPatcherUnregisterProgram},
    {0x6A6013E1, (void*)sceGxmSyncObjectCreate},
    {0x889AE88C, (void*)sceGxmSyncObjectDestroy},
    {0xB627DE66, (void*)sceGxmTerminate},
    {0x5341BD46, (void*)sceGxmTextureGetData},
    {0xE868D2B3, (void*)sceGxmTextureGetFormat},
    {0xF23FCE81, (void*)sceGxmTextureGetGammaMode},
    {0x5420A086, (void*)sceGxmTextureGetHeight},
    {0x2DE55DA5, (void*)sceGxmTextureGetLodBias},
    {0xBE524A2C, (void*)sceGxmTextureGetLodMin},
    {0xAE7FBB51, (void*)sceGxmTextureGetMagFilter},
    {0x920666C6, (void*)sceGxmTextureGetMinFilter},
    {0xCE94CA15, (void*)sceGxmTextureGetMipFilter},
    {0xF7B7B1E4, (void*)sceGxmTextureGetMipmapCount},
    {0x4CC42929, (void*)sceGxmTextureGetMipmapCountUnsafe},
    {0x512BB86C, (void*)sceGxmTextureGetNormalizeMode},
    {0x0D189C30, (void*)sceGxmTextureGetPalette},
    {0xB0BD52F3, (void*)sceGxmTextureGetStride},
    {0xF65D4917, (void*)sceGxmTextureGetType},
    {0x2AE22788, (void*)sceGxmTextureGetUAddrMode},
    {0xC037DA83, (void*)sceGxmTextureGetUAddrModeSafe},
    {0x46136CA9, (void*)sceGxmTextureGetVAddrMode},
    {0xD2F0D9C1, (void*)sceGxmTextureGetVAddrModeSafe},
    {0x126A3EB3, (void*)sceGxmTextureGetWidth},
    {0x11DC8DC9, (void*)sceGxmTextureInitCube},
    {0xE3DF5E3B, (void*)sceGxmTextureInitCubeArbitrary},
    {0x4811AECB, (void*)sceGxmTextureInitLinear},
    {0x6679BEF0, (void*)sceGxmTextureInitLinearStrided},
    {0xD572D547, (void*)sceGxmTextureInitSwizzled},
    {0x5DBFBA2C, (void*)sceGxmTextureInitSwizzledArbitrary},
    {0xE6F0DB27, (void*)sceGxmTextureInitTiled},
    {0x855814C4, (void*)sceGxmTextureSetData},
    {0xFC943596, (void*)sceGxmTextureSetFormat},
    {0xA6D9F4DA, (void*)sceGxmTextureSetGammaMode},
    {0xAEE7FDD1, (void*)sceGxmTextureSetHeight},
    {0xB65EE6F7, (void*)sceGxmTextureSetLodBias},
    {0xB79E43DD, (void*)sceGxmTextureSetLodMin},
    {0xFA695FD7, (void*)sceGxmTextureSetMagFilter},
    {0x416764E3, (void*)sceGxmTextureSetMinFilter},
    {0x1CA9FE0B, (void*)sceGxmTextureSetMipFilter},
    {0xD2DC4643, (void*)sceGxmTextureSetMipmapCount},
    {0xCE8DDAD0, (void*)sceGxmTextureSetNormalizeMode},
    {0xDD6AABFA, (void*)sceGxmTextureSetPalette},
    {0x58D0EB0A, (void*)sceGxmTextureSetStride},
    {0x4281763E, (void*)sceGxmTextureSetUAddrMode},
    {0x8699ECF4, (void*)sceGxmTextureSetUAddrModeSafe},
    {0x126CDAA3, (void*)sceGxmTextureSetVAddrMode},
    {0xFA22F6CC, (void*)sceGxmTextureSetVAddrModeSafe},
    {0x2EA178BE, (void*)sceGxmTextureSetWidth},
    {0x5331BED3, (void*)sceGxmTextureValidate},
    {0x62312BF8, (void*)sceGxmTransferCopy},
    {0xD10F7EAD, (void*)sceGxmTransferDownscale},
    {0x69DDFF5E, (void*)sceGxmTransferFill},
    {0x45229C39, (void*)sceGxmTransferFinish},
    {0x80CCEDBB, (void*)sceGxmUnmapFragmentUsseMemory},
    {0x828C68E8, (void*)sceGxmUnmapMemory},
    {0x099134F5, (void*)sceGxmUnmapVertexUsseMemory},
    {0xE05277D6, (void*)sceGxmVertexFence},
    {0xBC52320E, (void*)sceGxmVertexProgramGetProgram},
    {0x8BD94593, (void*)sceGxmWaitEvent},
};

extern "C" const int g_scegxm_nid_count = sizeof(g_scegxm_nid_map) / sizeof(g_scegxm_nid_map[0]);