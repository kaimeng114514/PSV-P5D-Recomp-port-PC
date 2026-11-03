// Vulkan后端核心
// 管理Vulkan实例、设备、队列、命令缓冲区、渲染通道、管线、交换链
#pragma once
#include "common/types.h"
#include <vector>
#include <string>

#ifdef P5D_HAS_VULKAN
// 真实Vulkan模式：包含官方头文件
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#else
// 前置声明（避免在头文件中包含vulkan.h）
typedef void* VkInstance_T;
typedef void* VkPhysicalDevice_T;
typedef void* VkDevice_T;
typedef void* VkQueue_T;
typedef void* VkCommandPool_T;
typedef void* VkCommandBuffer_T;
typedef void* VkRenderPass_T;
typedef void* VkFramebuffer_T;
typedef void* VkImageView_T;
typedef void* VkImage_T;
typedef void* VkDeviceMemory_T;
typedef void* VkBuffer_T;
typedef void* VkShaderModule_T;
typedef void* VkPipeline_T;
typedef void* VkPipelineLayout_T;
typedef void* VkDescriptorSetLayout_T;
typedef void* VkDescriptorPool_T;
typedef void* VkDescriptorSet_T;
typedef void* VkSampler_T;
typedef void* VkFence_T;
typedef void* VkSemaphore_T;
typedef void* VkSurfaceKHR_T;
typedef void* VkSwapchainKHR_T;

typedef VkInstance_T* VkInstance;
typedef VkPhysicalDevice_T* VkPhysicalDevice;
typedef VkDevice_T* VkDevice;
typedef VkQueue_T* VkQueue;
typedef VkCommandPool_T* VkCommandPool;
typedef VkCommandBuffer_T* VkCommandBuffer;
typedef VkRenderPass_T* VkRenderPass;
typedef VkFramebuffer_T* VkFramebuffer;
typedef VkImageView_T* VkImageView;
typedef VkImage_T* VkImage;
typedef VkDeviceMemory_T* VkDeviceMemory;
typedef VkBuffer_T* VkBuffer;
typedef VkShaderModule_T* VkShaderModule;
typedef VkPipeline_T* VkPipeline;
typedef VkPipelineLayout_T* VkPipelineLayout;
typedef VkDescriptorSetLayout_T* VkDescriptorSetLayout;
typedef VkDescriptorPool_T* VkDescriptorPool;
typedef VkDescriptorSet_T* VkDescriptorSet;
typedef VkSampler_T* VkSampler;
typedef VkFence_T* VkFence;
typedef VkSemaphore_T* VkSemaphore;
typedef VkSurfaceKHR_T* VkSurfaceKHR;
typedef VkSwapchainKHR_T* VkSwapchainKHR;

// 结果码
typedef enum VkResult {
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_TIMEOUT = 2,
    VK_EVENT_SET = 3,
    VK_EVENT_RESET = 4,
    VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4,
    VK_ERROR_MEMORY_MAP_FAILED = -5,
    VK_ERROR_LAYER_NOT_PRESENT = -6,
    VK_ERROR_EXTENSION_NOT_PRESENT = -7,
    VK_ERROR_FEATURE_NOT_PRESENT = -8,
    VK_ERROR_INCOMPATIBLE_DRIVER = -9,
    VK_ERROR_TOO_MANY_OBJECTS = -10,
    VK_ERROR_FORMAT_NOT_SUPPORTED = -11,
    VK_ERROR_SURFACE_LOST_KHR = -1000000000,
    VK_ERROR_NATIVE_WINDOW_IN_USE_KHR = -1000000001,
    VK_SUBOPTIMAL_KHR = 1000001003,
    VK_ERROR_OUT_OF_DATE_KHR = -1000001004,
} VkResult;
#endif // P5D_HAS_VULKAN

// 清除值
struct ClearColor {
    float r, g, b, a;
};

struct ClearDepthStencil {
    float depth;
    SceUInt32 stencil;
};

// 顶点属性
struct Vertex {
    float x, y, z, w;
    float r, g, b;
};

// 窗口配置
struct WindowConfig {
    SceUInt32 width = 960;   // PSV分辨率
    SceUInt32 height = 544;
    const char* title = "P5D Native Port";
    bool fullscreen = false;
};

namespace p5d {
namespace vulkan {

// Vulkan后端状态
struct BackendState {
    bool initialized = false;
    bool window_created = false;
    bool swapchain_created = false;

    // 窗口（平台相关）
    void* window_handle = nullptr;  // HWND on Windows, ANativeWindow* on Android
    void* instance_handle = nullptr; // HINSTANCE on Windows

    // 实例
    VkInstance instance = nullptr;

    // 设备
    VkPhysicalDevice physical_device = nullptr;
    VkDevice device = nullptr;
    VkQueue graphics_queue = nullptr;
    VkQueue present_queue = nullptr;
    SceUInt32 graphics_queue_family = 0;
    SceUInt32 present_queue_family = 0;

    // 命令池
    VkCommandPool command_pool = nullptr;

    // 当前命令缓冲区
    VkCommandBuffer current_cmd_buffer = nullptr;

    // 渲染通道
    VkRenderPass render_pass = nullptr;

    // 表面和交换链
    VkSurfaceKHR surface = nullptr;
    VkSwapchainKHR swapchain = nullptr;
    SceUInt32 swapchain_image_count = 0;
    VkImage* swapchain_images = nullptr;
    VkImageView* swapchain_image_views = nullptr;
    VkFramebuffer* swapchain_framebuffers = nullptr;
    SceUInt32 current_frame = 0;
    SceUInt32 current_image_index = 0;
    SceUInt32 swapchain_width = 960;
    SceUInt32 swapchain_height = 544;
    SceUInt32 swapchain_format = 0; // VkFormat

    // 同步
    VkFence frame_fence = nullptr;
    VkSemaphore image_available = nullptr;
    VkSemaphore render_finished = nullptr;

    // 管线
    VkPipelineLayout pipeline_layout = nullptr;
    VkDescriptorSetLayout descriptor_set_layout = nullptr;
    VkPipeline graphics_pipeline = nullptr;

    // 顶点缓冲
    VkBuffer vertex_buffer = nullptr;
    VkDeviceMemory vertex_buffer_memory = nullptr;
    void* vertex_buffer_mapped = nullptr;   // 持久映射指针
    SceUInt32 vertex_capacity = 0;          // 已分配容量（字节）
    SceUInt32 bound_vertex_count = 0;       // 当前顶点流中的顶点数
    // 索引缓冲（h137，持久映射）
    VkBuffer index_buffer = nullptr;
    VkDeviceMemory index_buffer_memory = nullptr;
    void* index_buffer_mapped = nullptr;
    SceUInt32 index_capacity = 0;

    // Uniform缓冲（MVP矩阵）
    VkBuffer uniform_buffer = nullptr;
    VkDeviceMemory uniform_buffer_memory = nullptr;
    void* uniform_buffer_mapped = nullptr;
    // h144k20【顶点/片元 UBO 拆分】binding0=顶点默认 UBO，binding1=片元默认 UBO。
    //   GXM 本来就是两个独立缓冲（SetVertexDefaultUniformBuffer / SetFragmentDefaultUniformBuffer），
    //   合成一个槽会让「一个 stage 读另一个 stage 的常量」——这正是整屏纯色与 bloom 常量找不到的根因。
    VkBuffer uniform_buffer_frag = nullptr;
    VkDeviceMemory uniform_buffer_frag_memory = nullptr;
    void* uniform_buffer_frag_mapped = nullptr;

    // 描述符池和描述符集
    VkDescriptorPool descriptor_pool = nullptr;
    VkDescriptorSet descriptor_set = nullptr;

    // test texture (combined image sampler, binding=1)
    VkImage test_texture_image = nullptr;
    VkDeviceMemory test_texture_memory = nullptr;
    VkImageView test_texture_view = nullptr;
    VkSampler test_texture_sampler = nullptr;
    SceUInt32 test_texture_width = 0;
    SceUInt32 test_texture_height = 0;
    bool test_texture_created = false;

    // test cube map (combined image sampler, binding=2): 6 faces, CUBE view
    VkImage test_cube_image = nullptr;
    VkDeviceMemory test_cube_memory = nullptr;
    VkImageView test_cube_view = nullptr;
    VkSampler test_cube_sampler = nullptr;
    SceUInt32 test_cube_size = 0;
    bool test_cube_created = false;

    // === 按 SceGxm texture unit 的真实纹理池（descriptor binding = unit+1，binding0 为 UBO）===
    struct UnitTexture {
        VkImage image = nullptr;
        VkDeviceMemory memory = nullptr;
        VkImageView view = nullptr;
        VkSampler sampler = nullptr;
        SceUInt32 width = 0, height = 0, layers = 1;
        SceUInt32 vk_format = 0;
        bool is_cube = false;
        bool created = false;
        // 运行时 materialize 脏检测缓存：记录上次上传来源 vaddr 与 16B 控制块快照
        SceUInt32 last_vaddr = 0xFFFFFFFFu;
        SceUInt32 last_ctrl[4] = {0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};
        // ★★★ h145k133【内容指纹】修掉"地址+控制块不变就永不重传"的缺陷：
        //   影片帧缓冲这类纹理地址固定、内容逐帧被 CPU/解码器改写，旧逻辑会让它
        //   永远停在"首次上传（全黑）"的样子。这里记住上次上传时的跨步内容指纹。
        SceUInt32 content_hash = 0;
        // h144p RT 别名：该 unit 采样的是离屏 RT（view 属 RT 池，不得销毁）
        VkImageView alias_view = nullptr;
        SceUInt32 alias_color = 0;
        bool is_rt_alias = false;
    };
    UnitTexture unit_textures[16];
    // 1x1 dummy，填充未加载 unit 槽，避免 shader 静态引用 missing binding
    VkImage dummy_image = nullptr;
    VkDeviceMemory dummy_memory = nullptr;
    VkImageView dummy_view = nullptr;
    VkSampler dummy_sampler = nullptr;
    bool dummy_created = false;

    // 截图用linear image
    VkImage screenshot_image = nullptr;
    VkDeviceMemory screenshot_memory = nullptr;
    bool screenshot_ready = false;

    // 视口/裁剪
    SceFloat viewport_x = 0, viewport_y = 0;
    SceFloat viewport_w = 960, viewport_h = 544;
    SceUInt32 scissor_x = 0, scissor_y = 0;
    SceUInt32 scissor_w = 960, scissor_h = 544;

    // 清除值
    ClearColor clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    ClearDepthStencil clear_depth = {1.0f, 0};

    // 渲染通道状态
    bool in_render_pass = false;

    // 帧计数
    SceUInt64 frame_count = 0;
};

// 全局后端状态
extern BackendState g_backend;

// === 初始化/终止 ===
SceInt32 backend_init();
SceInt32 backend_init_with_window(const WindowConfig& config);
void backend_shutdown();

// === 窗口管理（平台相关）===
SceInt32 create_window(const WindowConfig& config);
void destroy_window();
bool process_window_events();  // 处理窗口消息，返回false表示应退出

// === 设备/队列 ===
SceInt32 create_instance();
SceInt32 pick_physical_device();
SceInt32 create_logical_device();
SceInt32 create_command_pool();

// === 表面/交换链 ===
SceInt32 create_surface();
SceInt32 create_swapchain();
SceInt32 create_swapchain_image_views();
SceInt32 create_framebuffers();
void cleanup_swapchain();
SceInt32 recreate_swapchain(SceUInt32 width, SceUInt32 height);

// === 命令缓冲区 ===
VkCommandBuffer begin_command_buffer();
void end_command_buffer(VkCommandBuffer cmd);
void submit_command_buffer(VkCommandBuffer cmd);

// === 渲染通道 ===
SceInt32 create_render_pass();

// === 管线/缓冲 ===
SceInt32 create_graphics_pipeline();
SceInt32 create_graphics_pipeline_from_spirv(const SceUInt32* vertex_spv, SceUInt32 vertex_size,
                                              const SceUInt32* fragment_spv, SceUInt32 fragment_size);
SceInt32 create_shader_module(const SceUInt32* spv_code, SceUInt32 size, VkShaderModule* module);
SceInt32 load_spirv_file(const char* path, std::vector<SceUInt32>& code);
// 【h125】探测：从磁盘加载一对 vert/frag SPIR-V，用空顶点输入+正式描述符布局建临时管线，返回 VkResult(0=成功)
SceInt32 probe_spv_pipeline(const char* vpath, const char* fpath);
// 【h136】按 v/f SPIR-V 路径 get-or-create 真实游戏管线（缓存），并设为当前绑定；返回 VkResult(0=OK)
SceInt32 select_real_pipeline(const char* vpath, const char* fpath);
SceInt32 create_vertex_buffer();
SceInt32 upload_vertex_stream(const void* data, SceUInt32 bytes, SceUInt32 vertex_count);
void gxm_draw_arrays(SceUInt32 vertex_count);
SceInt32 create_index_buffer();  // h137
SceInt32 upload_index_stream(const void* data, SceUInt32 bytes);  // h137
// h137：上传真实顶点/索引并录制一次 indexed draw（index_16=1 为 UINT16，0 为 UINT32）
void gxm_draw_indexed_real(const void* vtx, SceUInt32 vbytes, const void* idx, SceUInt32 ibytes,
                           SceUInt32 index_count, SceUInt32 index_16);
SceInt32 create_uniform_buffer();
SceInt32 create_descriptor_pool_and_set();
SceInt32 create_test_texture();  // 创建棋盘格测试纹理+sampler
SceInt32 create_test_cube_texture();  // create 6-face cube map + sampler (binding=2)
SceInt32 create_dummy_texture();                                // 1x1 占位纹理
SceInt32 load_dds_to_unit(SceUInt32 unit, const char* dds_path, SceUInt32 swizzle_index=0xFFFFFFFFu); // 真实DDS上传到unit；swizzle_index 0..7 应用4通道重排，0xFFFFFFFF=IDENTITY
SceInt32 upload_rgba_swizzled(SceUInt32 unit, SceUInt32 w, SceUInt32 h, const SceUInt8* rgba, SceUInt32 swizzle_index); // RGBA8按PSV swizzle重排上传
SceInt32 upload_gxm_control_to_unit(SceUInt32 unit, const void* control16, const SceUInt8* hostPixels, SceUInt32 bytes); // 运行时：解析SceGxmTexture控制块并上传到unit
// 运行时：控制块 data_byte_addr 经全局地址空间翻译得到宿主像素后上传（无需外部传像素）
SceInt32 upload_gxm_control_resolved(SceUInt32 unit, const void* control16);
// ★ h145k281【P5D_TEXSRC=AUTO 用】每次解码产帧后由 eboot_probe 调用，
//   把"最新解码帧的 Y 平面地址"告诉后端；之后任何对 u0 的采样都会上传该帧。
void set_texsrc_addr(SceUInt32 addr);
// draw 前批量 materialize 当前绑定的 16 个 fragment 控制块（带脏检测缓存）
struct MaterializeStats { SceUInt32 uploaded=0, cached=0, missing_addr=0, failed=0; };
p5d::vulkan::MaterializeStats materialize_fragment_textures(const void* const controls[16]);
SceInt32 create_cube_unit_procedural(SceUInt32 unit, SceUInt32 size); // 程序化6面cube到unit（真实cube资源到位前的占位）
void update_uniform_buffer(const float* local_to_world, const float* view_proj);
// h143z：上传 guest GXM 默认 UBO 原始字节（screen/矩阵，vec4 槽布局与翻译器 ubo[phys-4] 对齐）；data=nullptr 清零
void update_uniform_buffer_raw(const void* data, SceUInt32 bytes);
// h144k20：片元默认 UBO 上传（binding1）。顶点用上面的 binding0。
void update_fragment_uniform_buffer_raw(const void* data, SceUInt32 bytes);
void cmd_bind_descriptor_set(VkCommandBuffer cmd);
SceInt32 create_sync_objects();

// === 截图 ===
SceInt32 save_screenshot(const char* filename);
// h145k442【池内 RT 亮度判决】回读指定离屏面并打印 [RTSTAT]（filename 传 "nul" 即只统计不落盘）
SceInt32 save_rt_screenshot(SceUInt32 cd, const char* filename);
// h145k525【整池亮度普查】遍历离屏 RT 池逐个回读并打印 [RTPOOL]/[RTSTAT]，不必再猜 cd。
//   动机：P5D_RTMEAN 原来硬编码 5 个 cd，实测一个都不在池里 ⇒ 早退 + [RTSTAT] 0 行。
SceInt32 rt_pool_probe(int maxn);
// h145k383【内容触发抓帧】draw 侧发现目标片元哈希时累加，present_frame 据此抓帧。
extern unsigned g_frame_content_draws;
// ★★★ h145k1110【当前 draw 的片元程序哈希】★★★
//   动机：用户实测 `P5D_TEXSRC=NV12` 有**重影**——一层正常影片 + 一层错误画面叠在一起。
//   根因：`upload_gxm_control_resolved` 里的 TEXSRC 分支**无条件**覆盖每一个纹理单元调用，
//   于是「影片四边形」以外的 draw（后处理全屏 pass、UI 等）采样到的也是影片帧 ⇒ 叠加。
//   本变量由 eboot_probe 的 `n_draw` 在每次 draw 前写入（它已算出 `gxp_hash_cached(g_cur_fgxp)`），
//   后端据此可用 `P5D_TEXSRC_FH=<hash,...>` 把覆盖**限制在影片自己的那一/几个 draw 上**。
extern unsigned long long g_cur_draw_fh;
// ★★★ h145k1135【当前 draw 的 primType（GXM 枚举，带高位）】★★★
//   动机：后端所有 pipeline 硬编码 `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`，而本作有大量
//   `0x0C000000`(TRIANGLE_STRIP) 的 draw。若影片四边形正是 STRIP，按 LIST 画只会画出
//   第 0 个三角形 ⇒ 画面沿**对角线**分成两半（用户实测："斜角左上半正常、右下半拉伸"）。
//   本变量由 eboot_probe 的 `n_draw` 在每次 draw 前写入 `g_cur_prim`，后端只用于**诊断打印**。
extern unsigned g_cur_prim;
// ★★★ h145k1110b【guest 是否已经请求播放影片】★★★
//   由 thumb_interpreter.cpp 的 `[MOVIEOPEN]` 探针（pc=0x811588F6）经
//   `p5d_movie_opened_notify()` 置 1。实测 guest 是在 **fc≈3015** 才打开影片，
//   而第一笔带纹理的 draw 在 **fc=2264**；因此"影片打开前"这段时间的画面
//   （ATLUS / P-STUDIO / CRIWARE 三个 LOGO，实测 fc 2800–3000）必须原样保留，
//   不能被 TEXSRC 的影片覆盖掉。
extern int g_movie_opened;
// h145k1110c：每次 `[MOVIEOPEN]` 让世代号 +1；后端据此把影片帧号/时钟归零
//   （用户实测：片头放完进标题界面待机，之后 attract 会再放一次动画 —— 那是新的一次 open）。
extern unsigned long long g_movie_open_epoch;
void request_capture(const char* name); // h143r4 下一帧 submit 后强制回读截图

// === 渲染通道执行 ===
void cmd_begin_render_pass(VkCommandBuffer cmd, SceUInt32 image_index);
void cmd_end_render_pass(VkCommandBuffer cmd);
void cmd_set_viewport(VkCommandBuffer cmd, SceFloat x, SceFloat y, SceFloat w, SceFloat h);
// h144k74【GXM viewport 状态机】由 eboot_probe 的 sceGxmSetViewport / SetViewportEnable HLE 调用。
// 权威：Vita3K renderer/src/vulkan/sync_state.cpp:174-198 与 renderer/src/state_set.cpp:145-148。
void set_gxm_viewport(SceFloat xo, SceFloat xs, SceFloat yo, SceFloat ys, SceFloat zo, SceFloat zs);
void set_gxm_viewport_enable(int mode);   // mode==0(DISABLED) => flat
// ★★★ h145k164【GXM 混合状态】把某个片元程序的打包 SceGxmBlendInfo 交给后端建管线时使用。
//   打包格式（见 eboot_probe.cpp:327）：cm | cf<<8 | af<<12 | cs<<16 | cd<<20 | as<<24 | ad<<28
//   （cm=SceGxmColorMask, cf/af=SceGxmBlendFunc color/alpha, cs/cd/as/ad=SceGxmBlendFactor）
//   0xFFFFFFFF = 未设置 ⇒ 关闭混合、colorMask=ALL（GXM 的 blendInfo==NULL 语义）。
void set_fragment_blend(SceUInt32 packed);
int  gxm_viewport_flat();
void cmd_set_scissor(VkCommandBuffer cmd, SceUInt32 x, SceUInt32 y, SceUInt32 w, SceUInt32 h);
void cmd_bind_pipeline(VkCommandBuffer cmd);
void cmd_bind_vertex_buffer(VkCommandBuffer cmd, VkBuffer buffer);
void cmd_draw(VkCommandBuffer cmd, SceUInt32 vertex_count, SceUInt32 instance_count,
              SceUInt32 first_vertex, SceUInt32 first_instance);
void cmd_draw_indexed(VkCommandBuffer cmd, SceUInt32 index_count, SceUInt32 instance_count,
                      SceUInt32 first_index, SceInt32 vertex_offset, SceUInt32 first_instance);

// === 帧管理（带交换链）===
SceInt32 begin_frame();  // 获取交换链图像 + 开始命令缓冲区 + 开始渲染通道
void end_frame();        // 结束渲染通道 + 提交 + 呈现
// h144n/o 多 pass 单帧：BeginScene 切到 colorData 离屏 RT；主合成 pass 后 copy->swapchain present
void begin_scene_pass(SceUInt32 colorData, SceUInt32 w, SceUInt32 h);
void end_scene_pass();
void present_frame(SceUInt32 colorData);

// === 工具函数 ===
const char* vk_result_string(VkResult result);
SceUInt32 find_memory_type(SceUInt32 type_filter, SceUInt32 properties);

// 调试计数器
extern int g_bind_pipeline_count;
extern int g_draw_call_count;

} // namespace vulkan
} // namespace p5d

// ★ h145k435【影片全屏彩色输出】把一帧 BGRA 交给后端：present 时用 vkCmdBlitImage
//   把它**全屏缩放** blit 到交换链图像上（与交换链同格式 ⇒ 颜色通道正确）。
extern "C" void p5d_movie_set_frame(const void* bgra, int w, int h);
// ★ h145k445【停止影片全屏 blit】把 g_movie_ready 清 0 ⇒ present 不再覆盖画面（回到引擎自身内容）。
extern "C" void p5d_movie_clear();
// ★ h145k1142【宿主全屏影片层是否在场】由 p5d_movie_set_frame / p5d_movie_clear 维护。
//   用途：`P5D_TEXSRC` 覆盖与宿主全屏 blit **互斥** —— 两层同时上屏会叠成
//   "斜角左上半正常、右下半被拉伸"的错层（引擎后处理链的顶点数据在本后端残缺）。
extern "C" int p5d_movie_layer_active;
// ★ h145k445【guest 请求播放影片的登记口】——由 thumb_interpreter 的 [MOVIEOPEN] 探针
//   （0x811588F6，即 app 的 `openMovie(player, name, ...)`）调用，把**guest 自己要放的影片名**
//   （实测 'movie/c_p5d000.usm'）交给宿主侧，供 `P5D_MOVIE_FS=AUTO` 自动播放**同一部**影片。
extern "C" void p5d_movie_request(const char* name);
extern "C" const char* p5d_movie_requested();
extern "C" unsigned p5d_movie_request_seq();
// ★ h145k461【影片播完后武装"把引擎推过影片这一环"】——见 vulkan_backend.cpp 里的说明。
extern "C" void p5d_movend_arm();
extern "C" int  p5d_movend_armed();
