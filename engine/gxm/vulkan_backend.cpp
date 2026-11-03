// Vulkan后端核心实现
// 完整的窗口、表面、交换链、渲染通道、绘制实�?
#include "vulkan_backend.h"
#include "common/log.h"
#include "texture_format.h"
#include "../mem/address_space.h"
#include <cstdio>
#include <chrono>

// ★ §18.188：计数变量前置声明
namespace p5d { namespace vulkan {
extern long g_n_present;
extern long g_n_begin_pass;
extern long g_n_draw_arrays;
} }

#ifdef P5D_HAS_VULKAN
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <mutex>
#include <thread>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace p5d {
namespace vulkan {

// h145k729【进�?t=0 时间戳】定义在 eboot_probe.cpp（静态初始化即起算）�?
//   用途：测出"窗口创建 / 设备就绪 / 首次 present"各自发生在进程启动后的第几秒�?
//   从而定位用户实�?~170s 黑屏到底花在哪一段�?
extern "C" double p5d_t_since_start(void);
// ★ ARCH_511：分配台账查询（实现在 src/arm/address_space.cpp）——回答"这个 guest 地址是哪个
//   guest PC 分配的"，用于把字形池/字形位图的归属钉到具体代码位置（配合 _srcmap.json 定位源文件）。
extern "C" unsigned p5d_mem_alloc_pc(unsigned a);
// ★ ARCH_513：返回**覆盖该地址的那一次分配**的起始 VA 与发起分配的 guest PC。
extern "C" unsigned p5d_mem_alloc_cover(unsigned a,unsigned len,unsigned* o_va,unsigned* o_pc);
// ★ ARCH_517：HLE 入口处的 guest 现场（实现在 src/arm/thumb_interpreter.cpp）。
//   用于定位"是谁在发这次 draw"——`lr` 就是引擎里那条 `bl` 的返回地址。
extern "C" unsigned p5d_guest_lr(void);
extern "C" unsigned p5d_guest_reg(int i);
// ★ ARCH_525：把"逐 draw 日志当前是否开启"暴露给 `sce_gxm.cpp`，
//   让 `[DRAWVA]`（顶点流的 guest VA，在 sceGxmDraw 里取）与 `[DRAWLOG]`/`[DRAWVTX]`
//   共用**同一个按内容触发的窗口**。这样两边严格 1:1 对齐，配对不需要靠猜顺序；
//   也避免了 ARCH_524 那次"帧窗口没命中 ⇒ DRAWVA 0 条"的问题（帧号↔场景会漂移）。
extern "C" int p5d_drawlog_active(void);
static int g_dl_active=0;
extern "C" int p5d_drawlog_active(void){ return g_dl_active; }
extern "C" void p5d_t_mark(const char* what);

BackendState g_backend;

// ★★�?h145k164【当前片元程序的 GXM 混合状态（打包）�?
//   0xFFFFFFFF = 未设�?�?关闭混合、colorMask 走原判据�?
//   �?eboot_probe 在绑定片元程序时写入（查 g_blend_by_gxp），建管线时读取�?
static SceUInt32 g_cur_frag_blend = 0xFFFFFFFFu;
void set_fragment_blend(SceUInt32 packed){ g_cur_frag_blend = packed; }

// 测试三角形顶点（中心在原点的小三角形，便于观察旋转）
static const Vertex test_triangle[] = {
    {-0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f},
    { 0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
    { 0.0f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
};

// Win32窗口过程
#ifdef _WIN32
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}
#endif

const char* vk_result_string(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VK_UNKNOWN_ERROR";
    }
}

SceUInt32 find_memory_type(SceUInt32 type_filter, SceUInt32 properties) {
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device), &mem_properties);
    for (SceUInt32 i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOGE("Failed to find suitable memory type");
    return 0xFFFFFFFF;
}

SceInt32 create_window(const WindowConfig& config) {
#ifdef _WIN32
    LOGI("Vulkan: Creating Win32 window (%dx%d)...", config.width, config.height);

    HINSTANCE hInstance = GetModuleHandle(nullptr);
    g_backend.instance_handle = hInstance;

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "P5DVulkanWindow";

    if (!RegisterClassEx(&wc)) {
        LOGE("RegisterClassEx failed");
        return -1;
    }

    RECT rect = {0, 0, (LONG)config.width, (LONG)config.height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowEx(
        0, "P5DVulkanWindow", config.title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        LOGE("CreateWindowEx failed");
        return -1;
    }

    g_backend.window_handle = hwnd;
    g_backend.window_created = true;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    LOGI("Win32 window created: HWND=%p", hwnd);
    p5d_t_mark("Win32 窗口创建并显示（ShowWindow 已调用）");
    return 0;
#else
    LOGW("Window creation not supported on this platform");
    return -1;
#endif
}

void destroy_window() {
#ifdef _WIN32
    if (g_backend.window_handle) {
        DestroyWindow((HWND)g_backend.window_handle);
        g_backend.window_handle = nullptr;
    }
    g_backend.window_created = false;
#endif
}

bool process_window_events() {
#ifdef _WIN32
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
#else
    return true;
#endif
}

static VKAPI_ATTR VkBool32 VKAPI_CALL p5d_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    const char* lvl = "?";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) lvl = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) lvl = "WARN";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) lvl = "INFO";
    else lvl = "VERB";
    fprintf(stderr, "[VAL %s] %s\n", lvl, (data && data->pMessage) ? data->pMessage : "(null)");
    fflush(stderr);
    return VK_FALSE;
}

static bool g_val_layer_on = false;
SceInt32 create_instance() {
    LOGI("Vulkan: Creating instance...");

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "P5D Native Port";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "P5DEngine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    // h144ak: 检测并启用 validation layer（layer 不可用时自动降级，不影响运行�?
    bool useValidation = false;
    {
        SceUInt32 n = 0; vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lps(n);
        if (n) vkEnumerateInstanceLayerProperties(&n, lps.data());
        for (SceUInt32 i = 0; i < n; i++)
            if (strstr(lps[i].layerName, "VK_LAYER_KHRONOS_validation")) useValidation = true;
    }
    bool hasDbg = false;
    {
        SceUInt32 n = 0; vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> eps(n);
        if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, eps.data());
        for (SceUInt32 i = 0; i < n; i++)
            if (strstr(eps[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) hasDbg = true;
    }
    fprintf(stderr, "[VAL] validation=%d debug_utils=%d\n", (int)useValidation, (int)hasDbg); fflush(stderr);
    g_val_layer_on = useValidation;

    std::vector<const char*> exts;
    exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
    exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(__ANDROID__)
    exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
    if (hasDbg) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkDebugUtilsMessengerCreateInfoEXT dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = p5d_debug_callback;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = (SceUInt32)exts.size();
    create_info.ppEnabledExtensionNames = exts.data();
    static const char* kLayerName[] = { "VK_LAYER_KHRONOS_validation" };
    if (useValidation) {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = kLayerName;
        create_info.pNext = &dci;
    }

    VkResult result = vkCreateInstance(&create_info, nullptr,
        reinterpret_cast<VkInstance*>(&g_backend.instance));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateInstance failed: %s", vk_result_string(result));
        return -1;
    }
    if (useValidation && hasDbg) {
        PFN_vkCreateDebugUtilsMessengerEXT pfn =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                *reinterpret_cast<VkInstance*>(&g_backend.instance),
                "vkCreateDebugUtilsMessengerEXT");
        if (pfn) {
            static VkDebugUtilsMessengerEXT s_dbgmsg = VK_NULL_HANDLE;
            pfn(*reinterpret_cast<VkInstance*>(&g_backend.instance), &dci, nullptr, &s_dbgmsg);
        }
    }
    LOGI("Vulkan instance created");
    return 0;
}

SceInt32 create_surface() {
#ifdef _WIN32
    LOGI("Vulkan: Creating Win32 surface...");

    VkWin32SurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.hinstance = (HINSTANCE)g_backend.instance_handle;
    surface_info.hwnd = (HWND)g_backend.window_handle;

    VkResult result = vkCreateWin32SurfaceKHR(
        *reinterpret_cast<VkInstance*>(&g_backend.instance),
        &surface_info, nullptr,
        reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateWin32SurfaceKHR failed: %s", vk_result_string(result));
        return -1;
    }
    LOGI("Win32 surface created");
    return 0;
#else
    LOGW("Surface creation not supported on this platform");
    return -1;
#endif
}

SceInt32 pick_physical_device() {
    LOGI("Vulkan: Picking physical device...");

    SceUInt32 device_count = 0;
    vkEnumeratePhysicalDevices(
        *reinterpret_cast<VkInstance*>(&g_backend.instance), &device_count, nullptr);
    if (device_count == 0) {
        LOGE("No Vulkan-capable GPUs found");
        return -1;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(
        *reinterpret_cast<VkInstance*>(&g_backend.instance), &device_count, devices.data());

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        LOGI("  GPU: %s (type=%d)", props.deviceName, props.deviceType);

        SceUInt32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

        for (SceUInt32 i = 0; i < queue_family_count; i++) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                // 检查是否支持呈�?
                VkBool32 present_support = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i,
                    *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), &present_support);

                if (present_support) {
                    g_backend.physical_device = device;
                    g_backend.graphics_queue_family = i;
                    g_backend.present_queue_family = i;
                    LOGI("  Selected GPU: %s, queue family=%d (graphics+present)", props.deviceName, i);
                    return 0;
                }
            }
        }
    }
    LOGE("No GPU with graphics+present queue found");
    return -1;
}

static void vulkan_layer_probe(VkDevice dev,const char* tag,const uint32_t* qvs=nullptr,size_t nvs=0,const uint32_t* qfs=nullptr,size_t nfs=0,VkShaderModule* outVs=nullptr,VkShaderModule* outFs=nullptr);
SceInt32 create_logical_device() {
    LOGI("Vulkan: Creating logical device...");

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info{};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = g_backend.graphics_queue_family;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures device_features{};
    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.pQueueCreateInfos = &queue_create_info;
    create_info.queueCreateInfoCount = 1;
    create_info.pEnabledFeatures = &device_features;

    const char* device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = device_extensions;

    VkResult result = vkCreateDevice(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device),
        &create_info, nullptr,
        reinterpret_cast<VkDevice*>(&g_backend.device));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateDevice failed: %s", vk_result_string(result));
        return -1;
    }
    fprintf(stderr,"[INITTID] vkCreateDevice tid=%lu\n",(unsigned long)GetCurrentThreadId()); fflush(stderr);

    vkGetDeviceQueue(*reinterpret_cast<VkDevice*>(&g_backend.device),
        g_backend.graphics_queue_family, 0,
        reinterpret_cast<VkQueue*>(&g_backend.graphics_queue));
    vkGetDeviceQueue(*reinterpret_cast<VkDevice*>(&g_backend.device),
        g_backend.present_queue_family, 0,
        reinterpret_cast<VkQueue*>(&g_backend.present_queue));
    vulkan_layer_probe(*reinterpret_cast<VkDevice*>(&g_backend.device),"early");

    LOGI("Logical device created");
    return 0;
}

SceInt32 create_command_pool() {
    LOGI("Vulkan: Creating command pool...");
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = g_backend.graphics_queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult result = vkCreateCommandPool(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &pool_info, nullptr,
        reinterpret_cast<VkCommandPool*>(&g_backend.command_pool));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateCommandPool failed: %s", vk_result_string(result));
        return -1;
    }
    LOGI("Command pool created");
    return 0;
}

SceInt32 create_swapchain() {
    LOGI("Vulkan: Creating swapchain...");

    // 查询表面能力
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device),
        *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), &capabilities);

    // 查询表面格式
    SceUInt32 format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device),
        *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device),
        *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), &format_count, formats.data());

    // 选择格式（优先B8G8R8A8_SRGB�?
    VkSurfaceFormatKHR selected_format = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected_format = fmt;
            break;
        }
    }

    // 查询呈现模式
    SceUInt32 present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device),
        *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        *reinterpret_cast<VkPhysicalDevice*>(&g_backend.physical_device),
        *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), &present_mode_count, present_modes.data());

    VkPresentModeKHR selected_present_mode = VK_PRESENT_MODE_FIFO_KHR; // 必须支持
    for (const auto& mode : present_modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            selected_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }

    // h145k727【实测】把实际选中�?present mode 打出�?—�?决定 19.5ms/�?的送显是否 vsync 阻塞
    printf("[PRESMODE] 可用=%u 选中=%d (%s)  minImageCount=%u\n",
        (unsigned)present_mode_count,(int)selected_present_mode,
        selected_present_mode==VK_PRESENT_MODE_MAILBOX_KHR?"MAILBOX(不阻�?":
        selected_present_mode==VK_PRESENT_MODE_IMMEDIATE_KHR?"IMMEDIATE(不阻�?":
        selected_present_mode==VK_PRESENT_MODE_FIFO_KHR?"FIFO(vsync阻塞)":"其他",
        (unsigned)capabilities.minImageCount);
    fflush(stdout);

    // 选择交换链分辨率
    VkExtent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = 960;
        extent.height = 544;
    }

    SceUInt32 image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface);
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = selected_format.format;
    swapchain_info.imageColorSpace = selected_format.colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = selected_present_mode;
    swapchain_info.clipped = VK_TRUE;
    swapchain_info.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &swapchain_info, nullptr,
        reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed: %s", vk_result_string(result));
        return -1;
    }

    g_backend.swapchain_width = extent.width;
    g_backend.swapchain_height = extent.height;
    g_backend.swapchain_format = selected_format.format;

    // 获取交换链图�?
    vkGetSwapchainImagesKHR(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain),
        &g_backend.swapchain_image_count, nullptr);
    g_backend.swapchain_images = new VkImage[g_backend.swapchain_image_count];
    vkGetSwapchainImagesKHR(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain),
        &g_backend.swapchain_image_count,
        reinterpret_cast<VkImage*>(g_backend.swapchain_images));

    g_backend.swapchain_created = true;
    LOGI("Swapchain created: %dx%d, %d images, format=%d",
         extent.width, extent.height, g_backend.swapchain_image_count, selected_format.format);
    return 0;
}

SceInt32 create_swapchain_image_views() {
    LOGI("Vulkan: Creating swapchain image views...");

    g_backend.swapchain_image_views = new VkImageView[g_backend.swapchain_image_count];

    for (SceUInt32 i = 0; i < g_backend.swapchain_image_count; i++) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = g_backend.swapchain_images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = (VkFormat)g_backend.swapchain_format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        VkResult result = vkCreateImageView(
            *reinterpret_cast<VkDevice*>(&g_backend.device),
            &view_info, nullptr,
            reinterpret_cast<VkImageView*>(&g_backend.swapchain_image_views[i]));
        if (result != VK_SUCCESS) {
            LOGE("vkCreateImageView [%d] failed: %s", i, vk_result_string(result));
            return -1;
        }
    }
    LOGI("Created %d swapchain image views", g_backend.swapchain_image_count);
    return 0;
}

SceInt32 create_render_pass() {
    LOGI("Vulkan: Creating render pass...");

    VkAttachmentDescription color_attachment{};
    color_attachment.format = (VkFormat)g_backend.swapchain_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment_ref{};
    color_attachment_ref.attachment = 0;
    color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    VkResult result = vkCreateRenderPass(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &render_pass_info, nullptr,
        reinterpret_cast<VkRenderPass*>(&g_backend.render_pass));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed: %s", vk_result_string(result));
        return -1;
    }
    LOGI("Render pass created");
    return 0;
}

SceInt32 create_framebuffers() {
    LOGI("Vulkan: Creating framebuffers...");

    g_backend.swapchain_framebuffers = new VkFramebuffer[g_backend.swapchain_image_count];

    for (SceUInt32 i = 0; i < g_backend.swapchain_image_count; i++) {
        VkImageView attachments[] = { g_backend.swapchain_image_views[i] };

        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = *reinterpret_cast<VkRenderPass*>(&g_backend.render_pass);
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = attachments;
        framebuffer_info.width = g_backend.swapchain_width;
        framebuffer_info.height = g_backend.swapchain_height;
        framebuffer_info.layers = 1;

        VkResult result = vkCreateFramebuffer(
            *reinterpret_cast<VkDevice*>(&g_backend.device),
            &framebuffer_info, nullptr,
            reinterpret_cast<VkFramebuffer*>(&g_backend.swapchain_framebuffers[i]));
        if (result != VK_SUCCESS) {
            LOGE("vkCreateFramebuffer [%d] failed: %s", i, vk_result_string(result));
            return -1;
        }
    }
    LOGI("Created %d framebuffers", g_backend.swapchain_image_count);
    return 0;
}

void cleanup_swapchain() {
    if (!g_backend.device) return;
    vkDeviceWaitIdle(*reinterpret_cast<VkDevice*>(&g_backend.device));

    if (g_backend.swapchain_framebuffers) {
        for (SceUInt32 i = 0; i < g_backend.swapchain_image_count; i++) {
            if (g_backend.swapchain_framebuffers[i]) {
                vkDestroyFramebuffer(*reinterpret_cast<VkDevice*>(&g_backend.device),
                    g_backend.swapchain_framebuffers[i], nullptr);
            }
        }
        delete[] g_backend.swapchain_framebuffers;
        g_backend.swapchain_framebuffers = nullptr;
    }
    if (g_backend.swapchain_image_views) {
        for (SceUInt32 i = 0; i < g_backend.swapchain_image_count; i++) {
            if (g_backend.swapchain_image_views[i]) {
                vkDestroyImageView(*reinterpret_cast<VkDevice*>(&g_backend.device),
                    g_backend.swapchain_image_views[i], nullptr);
            }
        }
        delete[] g_backend.swapchain_image_views;
        g_backend.swapchain_image_views = nullptr;
    }
    if (g_backend.swapchain_images) {
        delete[] g_backend.swapchain_images;
        g_backend.swapchain_images = nullptr;
    }
    if (g_backend.swapchain) {
        vkDestroySwapchainKHR(*reinterpret_cast<VkDevice*>(&g_backend.device),
            *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain), nullptr);
        g_backend.swapchain = nullptr;
    }
    g_backend.swapchain_created = false;
}

SceInt32 recreate_swapchain(SceUInt32 width, SceUInt32 height) {
    (void)width; (void)height;
    LOGI("Vulkan: Recreating swapchain...");
    cleanup_swapchain();
    if (create_swapchain() != 0) return -1;
    if (create_swapchain_image_views() != 0) return -1;
    if (create_framebuffers() != 0) return -1;
    return 0;
}

SceInt32 create_shader_module(const SceUInt32* spv_code, SceUInt32 size, VkShaderModule* module) {
    if (!spv_code || size == 0 || !module) return -1;
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = size * 4;
    create_info.pCode = spv_code;
    VkResult result = vkCreateShaderModule(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &create_info, nullptr,
        reinterpret_cast<VkShaderModule*>(module));
    if (result != VK_SUCCESS) {
        { static int _smf=0; if(_smf<8){ fprintf(stderr,"[SHADERMOD] vkCreateShaderModule FAIL result=%d (%s)\n",(int)result,vk_result_string(result)); fflush(stderr); _smf++; } }
        LOGE("vkCreateShaderModule failed: %s", vk_result_string(result));
        *module = VK_NULL_HANDLE;
        return -1;
    }
    { static int _sms=0; if(_sms<8){ fprintf(stderr,"[SHADERMOD] ok handle=%p codeWords=%u\n",(void*)*module,size); fflush(stderr); _sms++; } }
    return 0;
}
static void vulkan_layer_probe(VkDevice dev,const char* tag,const uint32_t* qvs,size_t nvs,const uint32_t* qfs,size_t nfs,VkShaderModule* outVs,VkShaderModule* outFs){
  static const uint32_t pv[]={0x07230203,0x00010000,0x00000000,0x0000000D,0x00000000,0x00020011,0x00000001,0x0003000E,0x00000000,0x00000001,0x0007000F,0x00000000,0x00000001,0x6E69616D,0x00000000,0x00000002,0x00000003,0x00040047,0x00000002,0x0000000B,0x00000000,0x00040047,0x00000003,0x0000001E,0x00000000,0x00020013,0x00000004,0x00030021,0x00000005,0x00000004,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,0x00040020,0x00000009,0x00000001,0x00000007,0x0004003B,0x00000008,0x00000002,0x00000003,0x0004003B,0x00000009,0x00000003,0x00000001,0x00050036,0x00000004,0x00000001,0x00000000,0x00000005,0x000200F8,0x0000000A,0x0004003D,0x00000007,0x0000000B,0x00000003,0x0003003E,0x00000002,0x0000000B,0x000100FD,0x00010038,};
  static const uint32_t pf[]={0x07230203,0x00010000,0x00000000,0x0000000E,0x00000000,0x00020011,0x00000001,0x0003000E,0x00000000,0x00000001,0x0006000F,0x00000004,0x00000001,0x6E69616D,0x00000000,0x00000002,0x00030010,0x00000001,0x00000007,0x00040047,0x00000002,0x0000001E,0x00000000,0x00020013,0x00000003,0x00030021,0x00000004,0x00000003,0x00030016,0x00000005,0x00000020,0x00040017,0x00000006,0x00000005,0x00000004,0x00040020,0x00000007,0x00000003,0x00000006,0x0004003B,0x00000007,0x00000002,0x00000003,0x0004002B,0x00000005,0x00000008,0x3F800000,0x0004002B,0x00000005,0x00000009,0x00000000,0x0007002C,0x00000006,0x0000000A,0x00000008,0x00000009,0x00000009,0x00000008,0x00050036,0x00000003,0x00000001,0x00000000,0x00000004,0x000200F8,0x0000000B,0x0003003E,0x00000002,0x0000000A,0x000100FD,0x00010038,};
  VkShaderModule mvs=VK_NULL_HANDLE,mfs=VK_NULL_HANDLE;
  VkShaderModuleCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize=sizeof(pv); ci.pCode=pv; VkResult r1=vkCreateShaderModule(dev,&ci,nullptr,&mvs);
  ci.codeSize=sizeof(pf); ci.pCode=pf; VkResult r2=vkCreateShaderModule(dev,&ci,nullptr,&mfs);
  VkPipelineLayoutCreateInfo lc{}; lc.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; VkPipelineLayout pl=VK_NULL_HANDLE; vkCreatePipelineLayout(dev,&lc,nullptr,&pl);
  VkAttachmentDescription ad{}; ad.format=VK_FORMAT_B8G8R8A8_UNORM; ad.samples=VK_SAMPLE_COUNT_1_BIT; ad.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp=VK_ATTACHMENT_STORE_OP_STORE; ad.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ar{}; ar.attachment=0; ar.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription sd{}; sd.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sd.colorAttachmentCount=1; sd.pColorAttachments=&ar;
  VkRenderPassCreateInfo rpc{}; rpc.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpc.attachmentCount=1; rpc.pAttachments=&ad; rpc.subpassCount=1; rpc.pSubpasses=&sd;
  VkRenderPass rp=VK_NULL_HANDLE; VkResult r3=vkCreateRenderPass(dev,&rpc,nullptr,&rp);
  VkPipelineShaderStageCreateInfo st[2]{}; st[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage=VK_SHADER_STAGE_VERTEX_BIT; st[0].module=mvs; st[0].pName="main";
  st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module=mfs; st[1].pName="main";
  VkVertexInputBindingDescription bd{}; bd.binding=0; bd.stride=16; bd.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription at{}; at.binding=0; at.location=0; at.format=VK_FORMAT_R32G32B32A32_SFLOAT; at.offset=0;
  VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bd; vi.vertexAttributeDescriptionCount=1; vi.pVertexAttributeDescriptions=&at;
  VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp{}; vp.width=64; vp.height=64; vp.maxDepth=1.f; VkRect2D sc{}; sc.extent={64,64};
  VkPipelineViewportStateCreateInfo vs{}; vs.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount=1; vs.pViewports=&vp; vs.scissorCount=1; vs.pScissors=&sc;
  VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.f;
  VkPipelineMultisampleStateCreateInfo ms{}; ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cb{}; cb.colorWriteMask=0xF;
  VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount=1; cbs.pAttachments=&cb;
  VkGraphicsPipelineCreateInfo gp{}; gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount=2; gp.pStages=st; gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia; gp.pViewportState=&vs; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pColorBlendState=&cbs; gp.layout=pl; gp.renderPass=rp;
  VkPipeline pipe=VK_NULL_HANDLE; VkResult r4=vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gp,nullptr,&pipe);
  fprintf(stderr,"[LAYERPROBE %s] cmod=%d/%d mvs=%p mfs=%p rp=%d rpipe=%d pipe=%p\n",tag,(int)r1,(int)r2,(void*)mvs,(void*)mfs,(int)r3,(int)r4,(void*)pipe); fflush(stderr);
  if(qvs && nvs>=8){
    int sameVS=((nvs==sizeof(pv)/4)&&(memcmp(qvs,pv,nvs*4)==0));
    int sameFS=((qfs!=nullptr)&&(nfs==sizeof(pf)/4)&&(memcmp(qfs,pf,nfs*4)==0));
    fprintf(stderr,"[LAYERCMP %s] nvs=%zu pv=%zu sameVS=%d nfs=%zu pf=%zu sameFS=%d q0=%08X p0=%08X\n",tag,nvs,sizeof(pv)/4,sameVS?1:0,nfs,sizeof(pf)/4,sameFS?1:0,qvs[0],pv[0]); fflush(stderr);
    VkShaderModule qmvs=VK_NULL_HANDLE,qmfs=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo qci=ci; qci.codeSize=nvs*4; qci.pCode=qvs; VkResult q1=vkCreateShaderModule(dev,&qci,nullptr,&qmvs);
    qci.codeSize=nfs*4; qci.pCode=qfs; VkResult q2=vkCreateShaderModule(dev,&qci,nullptr,&qmfs);
    st[0].module=qmvs; st[1].module=qmfs;
    VkPipeline qpipe=VK_NULL_HANDLE; VkResult q4=vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gp,nullptr,&qpipe);
    fprintf(stderr,"[LAYERCMP %s] project cmod=%d/%d qmvs=%p qmfs=%p qpipe=%d qh=%p\n",tag,(int)q1,(int)q2,(void*)qmvs,(void*)qmfs,(int)q4,(void*)qpipe); fflush(stderr);
    st[0].module=mvs; st[1].module=mfs;
    if(qpipe)vkDestroyPipeline(dev,qpipe,nullptr);
    if(outVs){*outVs=qmvs; fprintf(stderr,"[LAYERCMP %s] hand out VS=%p\n",tag,(void*)qmvs);fflush(stderr);} else if(qmvs)vkDestroyShaderModule(dev,qmvs,nullptr);
    if(outFs){*outFs=qmfs; fprintf(stderr,"[LAYERCMP %s] hand out FS=%p\n",tag,(void*)qmfs);fflush(stderr);} else if(qmfs)vkDestroyShaderModule(dev,qmfs,nullptr);
  }
  if(pipe)vkDestroyPipeline(dev,pipe,nullptr); if(rp)vkDestroyRenderPass(dev,rp,nullptr); vkDestroyPipelineLayout(dev,pl,nullptr); if(mfs)vkDestroyShaderModule(dev,mfs,nullptr); if(mvs)vkDestroyShaderModule(dev,mvs,nullptr);
}

SceInt32 load_spirv_file(const char* path, std::vector<SceUInt32>& code) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOGE("Cannot open SPIR-V file: %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 20 || size % 4 != 0) {
        fclose(f);
        LOGE("Invalid SPIR-V file size: %ld", size);
        return -1;
    }
    code.resize(size / 4);
    fread(code.data(), 4, size / 4, f);
    fclose(f);
    // 验证magic
    if (code[0] != 0x07230203) {
        LOGE("Invalid SPIR-V magic: 0x%08x", code[0]);
        return -1;
    }
    return 0;
}

SceInt32 create_graphics_pipeline_from_spirv(const SceUInt32* vertex_spv, SceUInt32 vertex_size,
                                              const SceUInt32* fragment_spv, SceUInt32 fragment_size) {
    LOGI("Vulkan: Creating graphics pipeline from SPIR-V...");
    // 销毁旧的管线对�?
    if (g_backend.graphics_pipeline) {
        vkDestroyPipeline(*reinterpret_cast<VkDevice*>(&g_backend.device),
            *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline), nullptr);
        g_backend.graphics_pipeline = nullptr;
    }
    if (g_backend.pipeline_layout) {
        vkDestroyPipelineLayout(*reinterpret_cast<VkDevice*>(&g_backend.device),
            *reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout), nullptr);
        g_backend.pipeline_layout = nullptr;
    }
    if (g_backend.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(*reinterpret_cast<VkDevice*>(&g_backend.device),
            *reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout), nullptr);
        g_backend.descriptor_set_layout = nullptr;
    }
    fprintf(stderr, "[DEBUG] create_graphics_pipeline_from_spirv: vert=%u frag=%u\n", vertex_size, fragment_size);
    fprintf(stderr, "[DEBUG] sizeof(VkGraphicsPipelineCreateInfo)=%zu\n", sizeof(VkGraphicsPipelineCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineShaderStageCreateInfo)=%zu\n", sizeof(VkPipelineShaderStageCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineVertexInputStateCreateInfo)=%zu\n", sizeof(VkPipelineVertexInputStateCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineInputAssemblyStateCreateInfo)=%zu\n", sizeof(VkPipelineInputAssemblyStateCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineViewportStateCreateInfo)=%zu\n", sizeof(VkPipelineViewportStateCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineRasterizationStateCreateInfo)=%zu\n", sizeof(VkPipelineRasterizationStateCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineMultisampleStateCreateInfo)=%zu\n", sizeof(VkPipelineMultisampleStateCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineColorBlendStateCreateInfo)=%zu\n", sizeof(VkPipelineColorBlendStateCreateInfo));
    fprintf(stderr, "[DEBUG] sizeof(VkPipelineDynamicStateCreateInfo)=%zu\n", sizeof(VkPipelineDynamicStateCreateInfo));
    fflush(stderr);
    fprintf(stderr, "[DEBUG] vertex_spv=%p frag_spv=%p\n", (void*)vertex_spv, (void*)fragment_spv);
    fprintf(stderr, "[DEBUG] vertex magic=0x%08X frag magic=0x%08X\n", vertex_spv[0], fragment_spv[0]);
    fflush(stderr);
    
    // 创建着色器模块
    VkShaderModule vert_module = nullptr, frag_module = nullptr;
    if (create_shader_module(vertex_spv, vertex_size, &vert_module) != 0) return -1;
    if (create_shader_module(fragment_spv, fragment_size, &frag_module) != 0) {
        printf("  [DEBUG] frag module FAILED\n");
        vkDestroyShaderModule(*reinterpret_cast<VkDevice*>(&g_backend.device), vert_module, nullptr);
        return -1;
    }
    fprintf(stderr, "[DEBUG] both shader modules created\n"); fflush(stderr);
    
    // 着色器阶段
    VkPipelineShaderStageCreateInfo shader_stages[2]{};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert_module;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag_module;
    shader_stages[1].pName = "main";
    
    // 顶点输入
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = 16;
    
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;
    
    // 输入组装
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;
    
    // 视口（动态状态）
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    
    // 光栅�?
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // 临时禁用面剔除调�?
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    
    // 多重采样
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // 颜色混合
    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable = VK_FALSE;
    
    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &color_blend_attachment;
    
    // 动态状�?
    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;
    
    // 描述符集布局（包含一个uniform buffer binding�?
    fprintf(stderr, "[DEBUG] creating descriptor set layout...\n"); fflush(stderr);
    // h144k20 binding0=顶点默认 UBO；binding1=片元默认 UBO；binding(2+unit)=texture unit 0..15 �?combined image sampler
    VkDescriptorSetLayoutBinding ds_bindings[18]{};
    ds_bindings[0].binding = 0;
    ds_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_bindings[0].descriptorCount = 1;
    ds_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[0].pImmutableSamplers = nullptr;
    ds_bindings[1].binding = 1;
    ds_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_bindings[1].descriptorCount = 1;
    ds_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ds_bindings[1].pImmutableSamplers = nullptr;
    for (SceUInt32 u = 0; u < 16; u++) {
        ds_bindings[2+u].binding = 2+u;
        ds_bindings[2+u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ds_bindings[2+u].descriptorCount = 1;
        ds_bindings[2+u].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        ds_bindings[2+u].pImmutableSamplers = nullptr;
    }

    VkDescriptorSetLayoutCreateInfo ds_layout_info{};
    ds_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds_layout_info.bindingCount = 18;
    ds_layout_info.pBindings = ds_bindings;
    VkResult result = vkCreateDescriptorSetLayout(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &ds_layout_info, nullptr,
        reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout));
    fprintf(stderr, "[DEBUG] vkCreateDescriptorSetLayout result=%d, layout=%p\n", 
            result, (void*)g_backend.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        LOGE("vkCreateDescriptorSetLayout failed: %s", vk_result_string(result));
        return -1;
    }
    
    // 管线布局
    fprintf(stderr, "[DEBUG] creating pipeline layout...\n"); fflush(stderr);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout);
    result = vkCreatePipelineLayout(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &pipeline_layout_info, nullptr,
        reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout));
    fprintf(stderr, "[DEBUG] vkCreatePipelineLayout result=%d, layout=%p\n", result, (void*)g_backend.pipeline_layout);
    fprintf(stderr, "[DEBUG] device=%p, renderPass=%p\n", 
            (void*)*reinterpret_cast<VkDevice*>(&g_backend.device),
            (void*)*reinterpret_cast<VkRenderPass*>(&g_backend.render_pass));
    fflush(stderr);
    if (result != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed: %s", vk_result_string(result));
        return -1;
    }
    
    // 创建图形管线
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout);
    pipeline_info.renderPass = *reinterpret_cast<VkRenderPass*>(&g_backend.render_pass);
    pipeline_info.subpass = 0;
    
    fprintf(stderr, "[DEBUG] pipeline_info: sType=%d pNext=%p stages=%d\n",
            pipeline_info.sType, pipeline_info.pNext, pipeline_info.stageCount);
    fprintf(stderr, "[DEBUG]   layout=%p renderPass=%p subpass=%d\n",
            (void*)pipeline_info.layout, (void*)pipeline_info.renderPass, pipeline_info.subpass);
    fprintf(stderr, "[DEBUG]   vertInput=%p inputAsm=%p viewport=%p raster=%p\n",
            (void*)pipeline_info.pVertexInputState, (void*)pipeline_info.pInputAssemblyState,
            (void*)pipeline_info.pViewportState, (void*)pipeline_info.pRasterizationState);
    fprintf(stderr, "[DEBUG]   multisample=%p colorBlend=%p dynamic=%p\n",
            (void*)pipeline_info.pMultisampleState, (void*)pipeline_info.pColorBlendState,
            (void*)pipeline_info.pDynamicState);
    fprintf(stderr, "[DEBUG] shader[0]: stage=%d module=%p pName=%s\n",
            shader_stages[0].stage, (void*)shader_stages[0].module, shader_stages[0].pName);
    fprintf(stderr, "[DEBUG] shader[1]: stage=%d module=%p pName=%s\n",
            shader_stages[1].stage, (void*)shader_stages[1].module, shader_stages[1].pName);
    fprintf(stderr, "[DEBUG] calling vkCreateGraphicsPipelines directly (MSVC)...\n"); fflush(stderr);
    result = vkCreateGraphicsPipelines(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline));
    fprintf(stderr, "[DEBUG] vkCreateGraphicsPipelines returned %d\n", result); fflush(stderr);
    
    // 销毁着色器模块（管线创建后不再需要）
    vkDestroyShaderModule(*reinterpret_cast<VkDevice*>(&g_backend.device), vert_module, nullptr);
    vkDestroyShaderModule(*reinterpret_cast<VkDevice*>(&g_backend.device), frag_module, nullptr);
    
    if (result != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed: %s", vk_result_string(result));
        return -1;
    }
    
    LOGI("Graphics pipeline created from SPIR-V (vertex=%d words, fragment=%d words)",
         vertex_size, fragment_size);
    return 0;
}

SceInt32 probe_spv_pipeline(const char* vpath, const char* fpath) {
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    std::vector<SceUInt32> vs, fs;
    if (load_spirv_file(vpath, vs) != 0) { printf("[PIPE-PROBE] load FAIL v %s\n", vpath); return -1; }
    if (load_spirv_file(fpath, fs) != 0) { printf("[PIPE-PROBE] load FAIL f %s\n", fpath); return -2; }
    VkShaderModule vm = nullptr, fm = nullptr;
    if (create_shader_module(vs.data(), (SceUInt32)vs.size(), &vm) != 0) { printf("[PIPE-PROBE] vmodule FAIL %s\n", vpath); return -3; }
    if (create_shader_module(fs.data(), (SceUInt32)fs.size(), &fm) != 0) {
        vkDestroyShaderModule(dev, vm, nullptr); printf("[PIPE-PROBE] fmodule FAIL %s\n", fpath); return -4; }
    static VkDescriptorSetLayout sDsl = nullptr;
    static VkPipelineLayout sPl = nullptr;
    if (!sDsl) {
        VkDescriptorSetLayoutBinding b[18]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[1].descriptorCount = 1;   // h144k20 片元 UBO
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (SceUInt32 u = 0; u < 16; u++) {
            b[2+u].binding = 2+u; b[2+u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2+u].descriptorCount = 1; b[2+u].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
        VkDescriptorSetLayoutCreateInfo di{}; di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        di.bindingCount = 18; di.pBindings = b;
        vkCreateDescriptorSetLayout(dev, &di, nullptr, &sDsl);
        VkPipelineLayoutCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pi.setLayoutCount = 1; pi.pSetLayouts = &sDsl;
        vkCreatePipelineLayout(dev, &pi, nullptr, &sPl);
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vm; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fm; st[1].pName = "main";
    VkVertexInputBindingDescription vib{}; vib.binding=0; vib.stride=16; vib.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[8]{};
    for (SceUInt32 li=0; li<8; li++){ via[li].binding=0; via[li].location=li; via[li].format=VK_FORMAT_R32G32B32A32_SFLOAT; via[li].offset=li*16; }
    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&vib;
    vi.vertexAttributeDescriptionCount=8; vi.pVertexAttributeDescriptions=via;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ca{}; ca.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ca;
    VkDynamicState dyns[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
    VkGraphicsPipelineCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2; info.pStages = st; info.pVertexInputState = &vi; info.pInputAssemblyState = &ia;
    info.pViewportState = &vp; info.pRasterizationState = &rs; info.pMultisampleState = &ms;
    info.pColorBlendState = &cb; info.pDynamicState = &dyn; info.layout = sPl;
    info.renderPass = *reinterpret_cast<VkRenderPass*>(&g_backend.render_pass); info.subpass = 0;
    VkPipeline pipe = nullptr;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipe);
    printf("[PIPE-PROBE] v=%s f=%s -> %d (%s)%s\n", vpath, fpath, (int)r, vk_result_string(r),
        pipe ? " [PIPELINE-OK]" : " [NO-PIPELINE]");
    fflush(stdout);
    if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyShaderModule(dev, vm, nullptr);
    vkDestroyShaderModule(dev, fm, nullptr);
    return (r == VK_SUCCESS) ? 0 : (SceInt32)r;
}

// 【h136】真实多管线缓存：按 "vpath|fpath" get-or-create VkPipeline，长期保留（module 建后销毁）�?
// 复用后端 descriptor/pipeline layout �?renderpass；顶点输入先�?8x vec4 通用声明（后续接真实 vertex stream 再细化）�?
static VkRenderPass g_rp_clear = VK_NULL_HANDLE, g_rp_load = VK_NULL_HANDLE;  // h144bs moved up for real pipeline
static const SceUInt32 s_diag_red_fs[] = {
    0x07230203,0x00010000,0x00000000,0x0000000E,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0006000F,0x00000004,0x00000001,0x6E69616D,0x00000000,0x00000002,
    0x00030010,0x00000001,0x00000007,
    0x00040047,0x00000002,0x0000001E,0x00000000,
    0x00020013,0x00000003,
    0x00030021,0x00000004,0x00000003,
    0x00030016,0x00000005,0x00000020,
    0x00040017,0x00000006,0x00000005,0x00000004,
    0x00040020,0x00000007,0x00000003,0x00000006,
    0x0004003B,0x00000007,0x00000002,0x00000003,
    0x0004002B,0x00000005,0x00000008,0x3F800000,
    0x0004002B,0x00000005,0x00000009,0x00000000,
    0x0007002C,0x00000006,0x0000000A,0x00000008,0x00000009,0x00000009,0x00000008,
    0x00050036,0x00000003,0x00000001,0x00000000,0x00000004,
    0x000200F8,0x0000000B,
    0x0003003E,0x00000002,0x0000000A,
    0x000100FD,
    0x00010038,
};
// h145k30【判�?varying 交接是否失效】把片元输出改成"直接输出 Input Location 0"�?
//   用途：两个模块静态都被逐指令核对为正确（顶点把颜色写到 Output loc0、片元把 Input loc0
//   写到颜色输出），但画面全�?�?只剩两种可能�?A) 运行�?varying 没传过来�?
//   (B) 片元侧其它运算把颜色弄没了。本模块只做 Load(Input loc0) -> Store(Output loc0)�?
//   把两者一刀切开：出现顶点颜�?�?(B)；仍是黑 �?(A)�?
static const SceUInt32 s_diag_show_in_fs[] = {
    0x07230203,0x00010000,0x00000000,0x0000000C,0x00000000,
    0x00020011,0x00000001,                                   // OpCapability Shader
    0x0003000E,0x00000000,0x00000001,                        // OpMemoryModel Logical GLSL450
    0x0007000F,0x00000004,0x00000001,0x6E69616D,0x00000000,0x00000002,0x00000003, // OpEntryPoint Fragment %1 "main" %2 %3
    0x00030010,0x00000001,0x00000007,                        // OpExecutionMode %1 OriginUpperLeft
    0x00040047,0x00000002,0x0000001E,0x00000000,            // OpDecorate %2 Location 0
    0x00040047,0x00000003,0x0000001E,0x00000000,            // OpDecorate %3 Location 0
    0x00020013,0x00000004,                                   // OpTypeVoid %4
    0x00030021,0x00000005,0x00000004,                        // OpTypeFunction %5 = void()
    0x00030016,0x00000006,0x00000020,                        // OpTypeFloat %6 = 32
    0x00040017,0x00000007,0x00000006,0x00000004,            // OpTypeVector %7 = v4float
    0x00040020,0x00000008,0x00000003,0x00000007,            // OpTypePointer %8 = Output v4float
    0x00040020,0x00000009,0x00000001,0x00000007,            // OpTypePointer %9 = Input  v4float
    0x0004003B,0x00000008,0x00000002,0x00000003,            // OpVariable %8 %2 Output
    0x0004003B,0x00000009,0x00000003,0x00000001,            // OpVariable %9 %3 Input
    0x00050036,0x00000004,0x00000001,0x00000000,0x00000005, // OpFunction %4 %1 None %5
    0x000200F8,0x0000000A,                                   // OpLabel %10
    0x0004003D,0x00000007,0x0000000B,0x00000003,            // OpLoad %7 %11 %3
    0x0003003E,0x00000002,0x0000000B,                        // OpStore %2 %11
    0x000100FD,                                              // OpReturn
    0x00010038,                                              // OpFunctionEnd
};
// h145k31【判�?alpha 是否把内容压黑】输�?vec4(InputLoc0.xyz, 1.0)�?
//   动机：顶点颜色紧凑值为 0x0100FFFF，按本项目的解包顺序 = (1.0, 1.0, 0.0, 1/255)
//   �?alpha�?.0039。若链上任何一级乘 alpha，内容就会被压成黑�?
//   本模块把 alpha 强置 1：出现内�?�?病因�?alpha；仍全黑 �?病因�?varying 本身�?
static const SceUInt32 s_diag_show_in_a1_fs[] = {
    0x07230203,0x00010000,0x00000000,0x0000000E,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0007000F,0x00000004,0x00000001,0x6E69616D,0x00000000,0x00000002,0x00000003,
    0x00030010,0x00000001,0x00000007,
    0x00040047,0x00000002,0x0000001E,0x00000000,
    0x00040047,0x00000003,0x0000001E,0x00000000,
    0x00020013,0x00000004,
    0x00030021,0x00000005,0x00000004,
    0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000004,
    0x00040020,0x00000008,0x00000003,0x00000007,
    0x00040020,0x00000009,0x00000001,0x00000007,
    0x0004003B,0x00000008,0x00000002,0x00000003,
    0x0004003B,0x00000009,0x00000003,0x00000001,
    0x0004002B,0x00000006,0x0000000C,0x3F800000,            // OpConstant %6 %12 = 1.0
    0x00050036,0x00000004,0x00000001,0x00000000,0x00000005,
    0x000200F8,0x0000000A,
    0x0004003D,0x00000007,0x0000000B,0x00000003,            // OpLoad %7 %11 %3
    0x00060051,0x00000007,0x0000000D,0x0000000C,0x0000000B,0x00000003, // OpCompositeInsert %7 %13 1.0 %11 3
    0x0003003E,0x00000002,0x0000000D,                        // OpStore %2 %13
    0x000100FD,
    0x00010038,
};
// ★★�?h145k139【判�?varying �?还是"采样读成�?】—�?本轮最重要的两个探针�?
//   背景：已�?red-FS 整屏纯红（几�?混合/深度/呈现全对），真实纹理确实绑定且已上传
//   （[TEXBIND] real=1 / [TEXMAT] 源数据有内容），app 自己的片元着色器也确�?
//   OpImageSampleImplicitLod；但输出恒黑。于是只剩两种可能：
//     (A) 顶点 OUT -> 片元 IN �?varying 没传过来（片元拿�?(0,0) �?永远采纹�?(0,0)）；
//     (B) 采样链本身把真实纹理读成黑（描述�?采样�?格式/通道）�?
//   mode 4 = 输出 vec4(InputLoc1.xyz, 1)：出现渐�?�?(A) 排除�?
//   mode 5 = texture(u0, InputLoc1.xy).rgba �?alpha=1：出现纹理内�?�?整条采样链正�?
//            �?病在 app 着色器自身的运算；仍黑 �?采样链有问题�?
//   注意描述符约定（�?usse_translator 一致）：sampler �?DescriptorSet 0 / Binding 1+纹理单元�?
static const SceUInt32 s_diag_show_in1_fs[] = {
    0x07230203,0x00010000,0x00000000,0x00000020,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0007000F,0x00000004,0x0000000F,0x6E69616D,0x00000000,0x0000000D,0x0000000C, // %13 in(loc1) %12 out(loc0)
    0x00030010,0x0000000F,0x00000007,
    0x00040047,0x0000000D,0x0000001E,0x00000001,            // %13 Location 1
    0x00040047,0x0000000C,0x0000001E,0x00000000,            // %12 Location 0
    0x00020013,0x00000001,0x00030021,0x00000002,0x00000001,0x00030016,0x00000003,0x00000020,
    0x00040017,0x00000005,0x00000003,0x00000004,
    0x00040020,0x00000006,0x00000003,0x00000005,            // Output v4
    0x00040020,0x00000007,0x00000001,0x00000005,            // Input v4
    0x0004002B,0x00000003,0x0000000B,0x3F800000,            // 1.0
    0x0004003B,0x00000006,0x0000000C,0x00000003,
    0x0004003B,0x00000007,0x0000000D,0x00000001,
    0x00050036,0x00000001,0x0000000F,0x00000000,0x00000002,
    0x000200F8,0x00000010,
    0x0004003D,0x00000005,0x00000011,0x0000000D,            // %17 = Load in
    0x00060052,0x00000005,0x00000014,0x0000000B,0x00000011,0x00000003, // insert alpha=1
    0x0003003E,0x0000000C,0x00000014,                        // Store out
    0x000100FD,0x00010038,
};
static const SceUInt32 s_diag_tex0_fs[] = {
    0x07230203,0x00010000,0x00000000,0x00000020,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0007000F,0x00000004,0x0000000F,0x6E69616D,0x00000000,0x0000000D,0x0000000C,
    0x00030010,0x0000000F,0x00000007,
    0x00040047,0x0000000D,0x0000001E,0x00000001,            // in Location 1
    0x00040047,0x0000000C,0x0000001E,0x00000000,            // out Location 0
    0x00040047,0x0000000E,0x00000022,0x00000000,            // tex DescriptorSet 0
    0x00040047,0x0000000E,0x00000021,0x00000002,            // �?tex Binding 2  (= texture unit 0)
                                                            //   约定�?=顶点UBO 1=片元UBO 2+unit=sampler
                                                            //   （usse_translator.cpp:1360-1366 �?vulkan_backend.cpp:2186 一致；
                                                            //    此前误写 Binding 1 �?采到的是片元 UBO �?必然全黑，是一次假阴性）
    0x00020013,0x00000001,0x00030021,0x00000002,0x00000001,0x00030016,0x00000003,0x00000020,
    0x00040017,0x00000004,0x00000003,0x00000002,            // v2float
    0x00040017,0x00000005,0x00000003,0x00000004,            // v4float
    0x00040020,0x00000006,0x00000003,0x00000005,
    0x00040020,0x00000007,0x00000001,0x00000005,
    0x00090019,0x00000008,0x00000003,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000, // OpTypeImage f32 2D
    0x0003001B,0x00000009,0x00000008,                        // OpTypeSampledImage
    0x00040020,0x0000000A,0x00000000,0x00000009,            // UniformConstant
    0x0004002B,0x00000003,0x0000000B,0x3F800000,
    0x0004003B,0x00000006,0x0000000C,0x00000003,
    0x0004003B,0x00000007,0x0000000D,0x00000001,
    0x0004003B,0x0000000A,0x0000000E,0x00000000,
    0x00050036,0x00000001,0x0000000F,0x00000000,0x00000002,
    0x000200F8,0x00000010,
    0x0004003D,0x00000005,0x00000011,0x0000000D,            // uv4 = Load in
    0x0004003D,0x00000009,0x00000012,0x0000000E,            // sm  = Load tex
    0x0007004F,0x00000004,0x00000015,0x00000011,0x00000011,0x00000000,0x00000001, // uv2 = shuffle(uv4,uv4,0,1)
    0x00050057,0x00000005,0x00000013,0x00000012,0x00000015, // col = ImageSampleImplicitLod
    0x00060052,0x00000005,0x00000014,0x0000000B,0x00000013,0x00000003,             // alpha=1
    0x0003003E,0x0000000C,0x00000014,
    0x000100FD,0x00010038,
};
// h144bs minimal passthrough VS: in vec4 loc0 -> gl_Position=vec4(pos.xy,0,1); no UBO/varyings
// h145k233【mode 6】不依赖任何 varying：UV = gl_FragCoord.xy * (1/512)�?
//   动机：mode 5（`texture(u0, InputLoc1.xy)`）实�?画面变纯�?被当�?采样链�?的证据，
//   但纯白恰恰说�?**UV 恒定**（一�?mean�?86 的纹理不可能采出 1.0 纯白）—�?
//   �?varying 交接坏掉�?mode 5 也会打印"纯白"，那是个假阳性�?
//   本模块用 FragCoord 自带 UV 绕开 varying，唯一地判定：
//     mode 6 出纹理图�?�?纹理数据/采样�?描述�?vkFormat 全对，病�?varying 交接�?
//     mode 6 仍纯�?    �?采样�?描述�?vkFormat 有问题（�?varying 无关）；
//     mode 6 全黑       �?绑定的那张纹理在该区域本来就是黑的�?
//   1/512 = 0x3B000000 是精�?2^-9，避免手算任意常量的舍入错误�?
static const SceUInt32 s_diag_fcoord_tex0_fs[] = {
    0x07230203,0x00010000,0x00000000,0x00000019,0x00000000,
    0x00020011,0x00000001,                                   // OpCapability Shader
    0x0003000E,0x00000000,0x00000001,                        // OpMemoryModel Logical GLSL450
    0x0007000F,0x00000004,0x0000000E,0x6E69616D,0x00000000,0x00000012,0x00000017, // OpEntryPoint Fragment %14 "main" %18 %23
    0x00030010,0x0000000E,0x00000007,                        // OpExecutionMode %14 OriginUpperLeft
    0x00040047,0x00000012,0x0000000B,0x0000000F,            // Decorate %18 BuiltIn FragCoord
    0x00040047,0x00000017,0x0000001E,0x00000000,            // Decorate %23 Location 0
    0x00040047,0x00000013,0x00000022,0x00000000,            // Decorate %19 DescriptorSet 0
    0x00040047,0x00000013,0x00000021,0x00000002,            // Decorate %19 Binding 2 (= texture unit 0)
    0x00020013,0x00000001,                                   // %1 void
    0x00030021,0x00000002,0x00000001,                        // %2 fn void()
    0x00030016,0x00000003,0x00000020,                        // %3 float
    0x00040017,0x00000004,0x00000003,0x00000002,            // %4 v2float
    0x00040017,0x00000005,0x00000003,0x00000004,            // %5 v4float
    0x00040020,0x00000006,0x00000003,0x00000005,            // %6 ptr Output v4float
    0x00040020,0x00000007,0x00000001,0x00000005,            // %7 ptr Input  v4float
    0x00090019,0x00000008,0x00000003,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000, // %8 image f32 2D
    0x0003001B,0x00000009,0x00000008,                        // %9 sampledImage
    0x00040020,0x0000000A,0x00000000,0x00000009,            // %10 ptr UniformConstant sampledImage
    0x0004002B,0x00000003,0x0000000B,0x3F800000,            // %11 = 1.0
    0x0004002B,0x00000003,0x0000000C,0x3B000000,            // %12 = 1/512  (2^-9, 精确)
    0x0005002C,0x00000004,0x0000000D,0x0000000C,0x0000000C,  // %13 = vec2(1/512, 1/512)
    0x0004003B,0x00000007,0x00000012,0x00000001,            // %18 = Variable Input v4 (FragCoord)
    0x0004003B,0x00000006,0x00000017,0x00000003,            // %23 = Variable Output v4
    0x0004003B,0x0000000A,0x00000013,0x00000000,            // %19 = Variable UniformConstant sampledImage
    0x00050036,0x00000001,0x0000000E,0x00000000,0x00000002,  // OpFunction
    0x000200F8,0x0000000F,                                   // OpLabel
    0x0004003D,0x00000005,0x00000010,0x00000012,            // %16 = Load %18
    0x0007004F,0x00000004,0x00000011,0x00000010,0x00000010,0x00000000,0x00000001, // %17 = shuffle(%16,%16,0,1)
    0x00050085,0x00000004,0x00000014,0x00000011,0x0000000D,  // %20 = FMul %17 * %13
    0x0004003D,0x00000009,0x00000015,0x00000013,            // %21 = Load %19
    0x00050057,0x00000005,0x00000016,0x00000015,0x00000014,  // %22 = ImageSampleImplicitLod %21 %20
    0x00060052,0x00000005,0x00000018,0x0000000B,0x00000016,0x00000003, // %24 = CompositeInsert 1.0 @3 of %22
    0x0003003E,0x00000017,0x00000018,                       // Store %23 %24
    0x000100FD,0x00010038,
};
static const SceUInt32 s_diag_passthru_vs[] = // h144bs: reuse proven s_fs_vert body (Load loc0 -> Store Position; includes OpMemoryModel)
{
    0x07230203,0x00010000,0x00000000,0x0000000D,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0007000F,0x00000000,0x00000001,0x6E69616D,0x00000000,0x00000002,0x00000003,
    0x00040047,0x00000002,0x0000000B,0x00000000,
    0x00040047,0x00000003,0x0000001E,0x00000000,
    0x00020013,0x00000004,
    0x00030021,0x00000005,0x00000004,
    0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000004,
    0x00040020,0x00000008,0x00000003,0x00000007,
    0x00040020,0x00000009,0x00000001,0x00000007,
    0x0004003B,0x00000008,0x00000002,0x00000003,
    0x0004003B,0x00000009,0x00000003,0x00000001,
    0x00050036,0x00000004,0x00000001,0x00000000,0x00000005,
    0x000200F8,0x0000000A,
    0x0004003D,0x00000007,0x0000000B,0x00000003,
    0x0003003E,0x00000002,0x0000000B,
    0x000100FD,
    0x00010038,
};
static std::unordered_map<std::string, VkPipeline> g_real_pipe_cache;
static VkPipeline g_current_real_pipe = VK_NULL_HANDLE;
// ★★�?h145k269【管线创�?RP vs 绘制 RP】★★★
//   动机：Vulkan 要求"管线创建时的 renderPass"�?绘制�?vkCmdBeginRenderPass �?renderPass"
//   **兼容**，否则该 draw 是未定义行为（实测常见表现就是整�?draw 一个像素都不写）�?
//   此前 `[RPCHK] match=1` 比较的是我们自己的两个变量（g_dbg_pass_rp / g_cur_pass_rp），
//   **从未比较�?管线对象里固化的那个 RP"**。这里在创建时把 RP 记下来，
//   绘制时把「绑定管线的创建 RP」与「当�?RP」一起打印，去重汇总�?
static std::unordered_map<VkPipeline, std::string> g_pipe_info;
static std::unordered_map<std::string,long> g_pipe_mismatch;
// h144k24【当前正在使用的 render pass】begin_scene_pass 每次�?doClear �?g_rp_clear / g_rp_load 之间切换�?
//   Vulkan 规定：管线创建时�?renderPass 与绘制时 vkCmdBeginRenderPass �?renderPass 必须**兼容**�?
//   否则该绘制是未定义行为（实测常见表现就是整条 draw 一个像素都不写）�?
//   此前管线缓存 key 只有 (vpath,fpath)，即「第一次创建时用的是哪�?RP」被永久固定�?
//   于是后续在另一�?RP 里的绘制全部作废 —�?这与「顶�?索引/管线/命令缓冲全对却零像素」的现象吻合�?
static VkRenderPass g_cur_pass_rp = VK_NULL_HANDLE;
// h144k34【动态状态记录】排查「顶�?索引/管线/命令缓冲全对却零像素」时，viewport/scissor 是最后两�?
//   从未被证实过的环节：它们�?*动态状�?*，由 cmd_set_viewport/cmd_set_scissor 在绘制前下发�?
//   一旦某处下发了 w=0/h=0 或把 scissor 推成空矩形，所�?draw 都会静默变成零像素�?
//   所以这里把「最后一次真�?vkCmdSet* 的值」记下来，在 draw 时打印出来对照�?
static float g_last_vp[4] = {0,0,0,0};
static int   g_last_sc[4] = {0,0,0,0};
static int   g_vp_sets = 0, g_sc_sets = 0;
SceInt32 get_or_create_real_pipeline(const char* vpath, const char* fpath) {
    VkRenderPass _rp = g_cur_pass_rp ? g_cur_pass_rp
                    : (g_rp_clear ? g_rp_clear : *reinterpret_cast<VkRenderPass*>(&g_backend.render_pass));
    std::string key = std::string(vpath) + "|" + std::string(fpath) + "|rp" + std::to_string((unsigned long long)(uintptr_t)_rp);
    auto it = g_real_pipe_cache.find(key);
    if (it != g_real_pipe_cache.end()) { g_current_real_pipe = it->second; return 0; }
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    std::vector<SceUInt32> vs, fs;
    if (load_spirv_file(vpath, vs) != 0) { printf("[REAL-PIPE] load FAIL v %s\n", vpath); return -1; }
    if (load_spirv_file(fpath, fs) != 0) { printf("[REAL-PIPE] load FAIL f %s\n", fpath); return -2; }
    VkShaderModule vm = nullptr, fm = nullptr;
    if (create_shader_module(vs.data(), (SceUInt32)vs.size(), &vm) != 0) return -3;
    if (create_shader_module(fs.data(), (SceUInt32)fs.size(), &fm) != 0) { vkDestroyShaderModule(dev, vm, nullptr); return -4; }
    // h144qw2-h: 扫描 frag SPV 是否声明 Output 变量（storage class=3）。无 => 该片元不产生颜色输出�?
    // 管线�?colorWriteMask=0，避免把 PA0 初�?0,0,0,0)当颜色写出而涂黑覆盖其�?pass�?
    // 依据 Vita3K pipeline_cache.cpp:870-876「lack of a fragment shader results in undefined values」�?
    bool fs_writes_color = true;
    {
        size_t _ni = 5; bool _seen_var = false, _any_out = false;
        while (_ni < fs.size()) {
            SceUInt32 _x = fs[_ni]; SceUInt32 _wc = _x >> 16, _op = _x & 0xFFFFu;
            if (_wc == 0 || _ni + _wc > fs.size()) break;
            if (_op == 59 && _wc >= 4) { _seen_var = true; if (fs[_ni + 3] == 3u) _any_out = true; } // OpVariable, storage class @+3
            _ni += _wc;
        }
        if (_seen_var && !_any_out) fs_writes_color = false;
    }
    bool _fm_owned=true; // h144bp diagnostic
    bool _vm_owned=true; // h144bs expC
    { const char* _rv=getenv("P5D_DBG_VS_BUILTIN");
      if(_rv && _rv[0]=='1'){
        static VkShaderModule s_pvm=VK_NULL_HANDLE;
        if(s_pvm==VK_NULL_HANDLE){ if(create_shader_module(s_diag_passthru_vs,(SceUInt32)(sizeof(s_diag_passthru_vs)/4),&s_pvm)==0) fprintf(stderr,"[REALVS-BUILTIN] passthrough vertex module ready\n"); fflush(stderr); }
        if(s_pvm!=VK_NULL_HANDLE){ vkDestroyShaderModule(dev,vm,nullptr); vm=s_pvm; _vm_owned=false; }
      } }
    { const char* _rf=getenv("P5D_REALFS");
      if(_rf && _rf[0]=='r'){
        static VkShaderModule s_redfm=VK_NULL_HANDLE;
        if(s_redfm==VK_NULL_HANDLE){ if(create_shader_module(s_diag_red_fs,(SceUInt32)(sizeof(s_diag_red_fs)/4),&s_redfm)==0) fprintf(stderr,"[REALFS-RED] red diagnostic fragment module ready\n"); fflush(stderr); }
        if(s_redfm!=VK_NULL_HANDLE){ vkDestroyShaderModule(dev,fm,nullptr); fm=s_redfm; _fm_owned=false; }
      } }
    // h145k30【判别开关】P5D_DIAG_FS_IN=1：把片元模块换成"直接输出 Input Location 0"�?
    //   一次判�?varying 交接是否失效（见 s_diag_show_in_fs 说明）�?
    { const char* _di=getenv("P5D_DIAG_FS_IN");
      // ★★�?h145k136：原来只�?frag_0084 生效。本机实测『red-FS 整屏纯红 �?几何/混合/深度/呈现全对�?
      //   �?app 自己的片元着色器输出全黑』，而绑定的真实纹理�?guest 内存里确实有内容（mean=182.9）�?
      //   SPIR-V 里也确实�?1 �?OpImageSampleImplicitLod�?> 头号嫌疑�?**varying(location) 语义接错**�?
      //   以致每个片元都采到同一个（黑的）纹素。把它放开到所有片元着色器，就能一次性判定：
      //     mode 1 原样输出 Input loc0 �?有渐�?交接正常；纯�?交接坏了
      //     mode 2 同上�?alpha 强置 1（排�?alpha 混合把画面吃掉）
      //     mode 3 常量红（替换机制自身的对照）
      if(_di && _di[0] && fpath && (std::strstr(fpath,"frag_0084") || std::getenv("P5D_DIAG_FS_ALL"))){
        // h145k34【判�?A/B】mode 1=原样输出 Input loc0�?=�?1 �?alpha 强置 1�?
        //   3=常量红（s_diag_red_fs�?*完全不读 Input**）—�?用作"替换机制本身有效"的对照�?
        // �?h145k139 新增�?=输出 Input loc1(alpha=1)�?=采样 u0 输出(alpha=1)�?
        const char _m=_di[0];
        static VkShaderModule s_din=VK_NULL_HANDLE, s_din_a1=VK_NULL_HANDLE, s_din_red=VK_NULL_HANDLE,
                              s_din_l1=VK_NULL_HANDLE, s_din_t0=VK_NULL_HANDLE, s_din_fc0=VK_NULL_HANDLE;
        VkShaderModule* _slot = (_m=='3') ? &s_din_red : ((_m=='2') ? &s_din_a1
                              : ((_m=='4') ? &s_din_l1  : ((_m=='5') ? &s_din_t0
                              : ((_m=='6') ? &s_din_fc0 : &s_din))));
        const SceUInt32* _src = (_m=='3') ? s_diag_red_fs : ((_m=='2') ? s_diag_show_in_a1_fs
                              : ((_m=='4') ? s_diag_show_in1_fs : ((_m=='5') ? s_diag_tex0_fs
                              : ((_m=='6') ? s_diag_fcoord_tex0_fs : s_diag_show_in_fs))));
        const SceUInt32 _wc = (_m=='3') ? (SceUInt32)(sizeof(s_diag_red_fs)/4)
                            : ((_m=='2') ? (SceUInt32)(sizeof(s_diag_show_in_a1_fs)/4)
                            : ((_m=='4') ? (SceUInt32)(sizeof(s_diag_show_in1_fs)/4)
                            : ((_m=='5') ? (SceUInt32)(sizeof(s_diag_tex0_fs)/4)
                            : ((_m=='6') ? (SceUInt32)(sizeof(s_diag_fcoord_tex0_fs)/4)
                                         : (SceUInt32)(sizeof(s_diag_show_in_fs)/4)))));
        if(*_slot==VK_NULL_HANDLE){
          if(create_shader_module(_src,_wc,_slot)==0)
            fprintf(stderr,"[DIAGFS-IN] fragment module ready (mode=%c: %s)\n",_m,
                    _m=='3'?"constant red":(_m=='2'?"Input loc0, alpha=1":
                    (_m=='4'?"Input loc1, alpha=1":(_m=='5'?"sample u0 with Input loc1":
                    (_m=='6'?"sample u0 with gl_FragCoord*1/512 (NO varying)":"Input loc0, raw")))));
          fflush(stderr); }
        if(*_slot!=VK_NULL_HANDLE){ vkDestroyShaderModule(dev,fm,nullptr); fm=*_slot; _fm_owned=false; } } }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fm; st[1].pName = "main";
    // 【h141】真实顶点流：单 binding stride=128(8*vec4)，location0..7 统一 R32G32B32A32�?
    // n_draw 按每程序属性表把顶�?CPU 解包�?per-location vec4(loc0=position,loc1=color,loc2=texcoord...)，缺省补0�?
    VkVertexInputBindingDescription vib{}; vib.binding = 0; vib.stride = 128; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[8]{};
    for (SceUInt32 _li = 0; _li < 8; _li++){ via[_li].binding = 0; via[_li].location = _li; via[_li].format = VK_FORMAT_R32G32B32A32_SFLOAT; via[_li].offset = _li*16; }
    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vib;
    vi.vertexAttributeDescriptionCount = 8; vi.pVertexAttributeDescriptions = via;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ca{}; ca.colorWriteMask = fs_writes_color ? 0xF : 0;  // h144qw2-h: 无颜色输出则不写
    // ★★�?h145k164【GXM 混合状�?�?Vulkan】★★★
    //   背景：实�?窗口恒为纯黑"的真凶是 `vert_0014`+`frag_0079` 画的**一整屏不透明纯黑覆盖�?*
    //   （`P5D_SKIP_VH=ABE1E100963724B0` 跳过它后背景�?(0,0,0) 变成引擎真实背景�?(31,22,18)）�?
    //   �?`frag_0079` 的常量块实测�?(0,0,0,1)——若它本意是"淡入淡出叠层"，就必须�?alpha 混合�?
    //   但后端建管线�?*从未设置混合**（blendEnable 恒为 FALSE）⇒ 任何半透明叠层都变成实心色块�?
    //   HLE 侧早已捕获每个片元程序的 SceGxmBlendInfo（g_blend_by_gxp），这里把它映射�?Vulkan 状态�?
    //   packed = cm | cf<<8 | af<<12 | cs<<16 | cd<<20 | as<<24 | ad<<28（见 eboot_probe.cpp:327�?
    {
        auto _gxmFactor=[](SceUInt32 f)->VkBlendFactor{
            switch(f){
                case 0:  return VK_BLEND_FACTOR_ZERO;
                case 1:  return VK_BLEND_FACTOR_ONE;
                case 2:  return VK_BLEND_FACTOR_SRC_COLOR;
                case 3:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case 4:  return VK_BLEND_FACTOR_SRC_ALPHA;
                case 5:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case 6:  return VK_BLEND_FACTOR_DST_COLOR;
                case 7:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case 8:  return VK_BLEND_FACTOR_DST_ALPHA;
                case 9:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
                case 11: return VK_BLEND_FACTOR_DST_ALPHA;   // Vulkan �?DST_ALPHA_SATURATE，取最近语�?
                default: return VK_BLEND_FACTOR_ONE; }
        };
        auto _gxmOp=[](SceUInt32 f)->VkBlendOp{
            switch(f){
                case 1:  return VK_BLEND_OP_ADD;
                case 2:  return VK_BLEND_OP_SUBTRACT;
                case 3:  return VK_BLEND_OP_REVERSE_SUBTRACT;
                case 4:  return VK_BLEND_OP_MIN;
                case 5:  return VK_BLEND_OP_MAX;
                default: return VK_BLEND_OP_ADD; }
        };
        static int _blend_off=-1; if(_blend_off<0) _blend_off=std::getenv("P5D_NO_BLEND")?1:0;
        if(!_blend_off && g_cur_frag_blend!=0xFFFFFFFFu && fs_writes_color){
            SceUInt32 _p=g_cur_frag_blend;
            SceUInt32 _cm=_p&0xFFu, _cf=(_p>>8)&0xFu, _af=(_p>>12)&0xFu;
            SceUInt32 _cs=(_p>>16)&0xFu, _cd=(_p>>20)&0xFu, _as=(_p>>24)&0xFu, _ad=(_p>>28)&0xFu;
            if(_cf!=0u){                          // 0 = SCE_GXM_BLEND_FUNC_NONE �?关闭
                ca.blendEnable=VK_TRUE;
                ca.srcColorBlendFactor=_gxmFactor(_cs);
                ca.dstColorBlendFactor=_gxmFactor(_cd);
                ca.colorBlendOp=_gxmOp(_cf);
                ca.srcAlphaBlendFactor=_gxmFactor(_as);
                ca.dstAlphaBlendFactor=_gxmFactor(_ad);
                ca.alphaBlendOp=_gxmOp(_af?_af:_cf); }
            // SceGxmColorMask 位序 A=1,R=2,G=4,B=8 �?Vulkan R=1,G=2,B=4,A=8
            SceUInt32 _vm=0;
            if(_cm&2u)_vm|=1u; if(_cm&4u)_vm|=2u; if(_cm&8u)_vm|=4u; if(_cm&1u)_vm|=8u;
            if(_cm) ca.colorWriteMask=_vm;        // mask==0 时保持原判据，避免把正常 draw 全关�?
            // �?h145k731【逐管线混合判决改�?stdout】原实现只进 stderr（默认被丢进 NUL），
            //   导致"哪条管线拿到了什么混合状�?完全不可见。动机：`g_cur_frag_blend` �?
            //   **全局单�?*，由 CreateFragmentProgram（加载期）写入、建管线（首次绘制）时读取，
            //   中间会被别的片元程序覆盖 �?管线很可能拿到别的程序的混合状态�?
            //   工程既知真凶�?vert_0014+frag_0079 的全�?(0,0,0,1) 填充，故必须可见�?
            static int _bn=0; if(_bn<64){ _bn++;
                printf("[BLENDMAP] f=%s packed=%08X mask=%X cf=%u af=%u cs=%u cd=%u as=%u ad=%u -> enable=%d vkMask=%X\n",
                    fpath,_p,_cm,_cf,_af,_cs,_cd,_as,_ad,(int)ca.blendEnable,_vm); fflush(stdout); } } }
    // ★★�?h145k146【直�?最后写屏者把画面盖成�?】★★★
    //   `[DRAWSEQ]` 实测每张�?RT �?*最后两�?draw 都是 `gxp_frag_0079`**—�?
    //   一支零纹理采样、只读片元常量块的全屏填充（`[FUBOALL]` 显示其常量是 (0,0,0,1)）�?
    //   把它�?colorWriteMask �?0 就等�?跳过这一发填�?，用来判定它底下是否已经有真实画面�?
    //   P5D_SKIP_FRAG79=1 启用；命中时打一�?[SKIP79]�?
    { static int _sk=-1; if(_sk<0) _sk=std::getenv("P5D_SKIP_FRAG79")?1:0;
      if(_sk && fpath && std::strstr(fpath,"frag_0079")){
        ca.colorWriteMask = 0;
        static int _s1=0; if(_s1<4){ _s1++; fprintf(stderr,"[SKIP79] f=%s colorWriteMask=0 (实验：跳过全屏黑填充)\n",fpath); fflush(stderr); } } }
    if (!fs_writes_color) { static int _ncw = 0; if (_ncw < 12) { _ncw++; fprintf(stderr, "[NOCOLOROUT] f=%s colorWriteMask=0 (frag �?Output 变量)\n", fpath); fflush(stderr); } }
    VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ca;
    VkDynamicState dyns[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{}; dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
    VkGraphicsPipelineCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2; info.pStages = st; info.pVertexInputState = &vi; info.pInputAssemblyState = &ia;
    info.pViewportState = &vp; info.pRasterizationState = &rs; info.pMultisampleState = &ms;
    info.pColorBlendState = &cb; info.pDynamicState = &dyn;
    info.layout = *reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout);
    info.renderPass = _rp; info.subpass = 0;  // h144k24: 用「当前实际使用的 RP」建管线，与缓存 key 一�?
    VkPipeline pipe = nullptr;
    fprintf(stderr,"[REALPIPE-CREATE] before vkCreateGraphicsPipelines v=%s f=%s\n",vpath,fpath); fflush(stderr);
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipe);
    fprintf(stderr,"[REALPIPE-CREATE] after r=%d pipe=%p\n",(int)r,(void*)pipe); fflush(stderr);
    if(_vm_owned) vkDestroyShaderModule(dev, vm, nullptr);  // h144bs: keep shared builtin VS
    if(_fm_owned) vkDestroyShaderModule(dev, fm, nullptr);
    if (r != VK_SUCCESS || !pipe) { printf("[REAL-PIPE] FAIL v=%s f=%s -> %d (%s)\n", vpath, fpath, (int)r, vk_result_string(r)); return (SceInt32)r; }
    g_real_pipe_cache[key] = pipe;
    g_current_real_pipe = pipe;
    { char _ib[220]; std::snprintf(_ib,sizeof(_ib),"v=%s f=%s rp=%p",vpath,fpath,(void*)_rp); g_pipe_info[pipe]=_ib; }
    printf("[REAL-PIPE] #%zu OK v=%s f=%s rp=%p pipe=%p\n", g_real_pipe_cache.size(), vpath, fpath, (void*)_rp, (void*)pipe);
    fflush(stdout);
    return 0;
}
SceInt32 select_real_pipeline(const char* vpath, const char* fpath) { return get_or_create_real_pipeline(vpath, fpath); }

SceInt32 create_graphics_pipeline() {
    // 尝试从文件加载SPIR-V，如果失败则创建空管线布局
    LOGI("Vulkan: Creating graphics pipeline...");
    
    std::vector<SceUInt32> vert_spv, frag_spv;
    if (load_spirv_file("vertex.spv", vert_spv) == 0 &&
        load_spirv_file("fragment.spv", frag_spv) == 0) {
        LOGI("Loaded SPIR-V from files, creating full pipeline");
        return create_graphics_pipeline_from_spirv(
            vert_spv.data(), vert_spv.size(),
            frag_spv.data(), frag_spv.size());
    }
    
    // 【h144k20】无内置测试 shader：建�?18-binding DSL（binding0=顶点UBO，binding1=片元UBO�?..17=combined sampler�?
    LOGW("No SPIR-V files found, creating 18-binding DSL + pipeline layout");
    {
        VkDescriptorSetLayoutBinding db[18]{};
        db[0].binding = 0; db[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; db[0].descriptorCount = 1;
        db[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        db[1].binding = 1; db[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; db[1].descriptorCount = 1;
        db[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (SceUInt32 u = 0; u < 16; u++) {
            db[2+u].binding = 2+u; db[2+u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            db[2+u].descriptorCount = 1; db[2+u].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
        VkDescriptorSetLayoutCreateInfo di{}; di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        di.bindingCount = 18; di.pBindings = db;
        VkResult dr = vkCreateDescriptorSetLayout(*reinterpret_cast<VkDevice*>(&g_backend.device), &di, nullptr,
            reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout));
        if (dr != VK_SUCCESS) { LOGE("fallback vkCreateDescriptorSetLayout failed: %s", vk_result_string(dr)); return -1; }
    }
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout);
    VkResult result = vkCreatePipelineLayout(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &pipeline_layout_info, nullptr,
        reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout));
    fprintf(stderr, "[DEBUG] vkCreatePipelineLayout result=%d, layout=%p\n", result, (void*)g_backend.pipeline_layout);
    fprintf(stderr, "[DEBUG] device=%p, renderPass=%p\n", 
            (void*)*reinterpret_cast<VkDevice*>(&g_backend.device),
            (void*)*reinterpret_cast<VkRenderPass*>(&g_backend.render_pass));
    fflush(stderr);
    if (result != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed: %s", vk_result_string(result));
        return -1;
    }
    LOGI("Pipeline layout created (no shaders)");
    return 0;
}

SceInt32 create_vertex_buffer() {
    LOGI("Vulkan: Creating vertex buffer...");
    VkDeviceSize buffer_size = 1048576;  // h141: stride128 多属性顶点，1MB  // 固定容量，持久映射供顶点流动态上�?
    g_backend.vertex_capacity = (SceUInt32)buffer_size;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &buffer_info, nullptr,
        reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateBuffer failed: %s", vk_result_string(result));
        return -1;
    }

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer), &mem_requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        mem_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    result = vkAllocateMemory(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &alloc_info, nullptr,
        reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory));
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateMemory failed: %s", vk_result_string(result));
        return -1;
    }

    // 【h137n】标准顺序：�?bind（检查返回值），再 map（检查返回值）
    VkResult _brc = vkBindBufferMemory(*reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer),
        *reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory), 0);
    if (_brc != VK_SUCCESS) { LOGE("vertex vkBindBufferMemory failed: %s", vk_result_string(_brc)); return -1; }
    void* data = nullptr;
    VkResult _mrc = vkMapMemory(*reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory),
        0, buffer_size, 0, &data);
    if (_mrc != VK_SUCCESS || !data) { LOGE("vertex vkMapMemory failed: %s data=%p", vk_result_string(_mrc), data); return -1; }
    memcpy(data, test_triangle, sizeof(test_triangle));
    g_backend.vertex_buffer_mapped = data;  // 持久映射，供 upload_vertex_stream 复用
    g_backend.bound_vertex_count = 3;
    fprintf(stderr,"[H137N] vertex buffer bound+map OK buf=%p mem=%p mapped=%p\n",
        (void*)*reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer),
        (void*)*reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory), data);

    LOGI("Vertex buffer created (%d bytes)", (int)buffer_size);
    return 0;
}

// h144k20：把单个 host-visible UBO 的创建抽出来，顶�?片元各一份（binding0 / binding1�?
static SceInt32 create_one_ubo(VkBuffer* pbuf, VkDeviceMemory* pmem, void** pmapped,
                               VkDeviceSize buffer_size, const char* tag) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = buffer_size;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(*reinterpret_cast<VkDevice*>(&g_backend.device), &bi, nullptr, pbuf) != VK_SUCCESS) {
        LOGE("vkCreateBuffer (%s) failed", tag); return -1; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(*reinterpret_cast<VkDevice*>(&g_backend.device), *pbuf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    SceUInt32 mt = find_memory_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == 0xFFFFFFFFu) mt = find_memory_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(*reinterpret_cast<VkDevice*>(&g_backend.device), &ai, nullptr, pmem) != VK_SUCCESS) {
        LOGE("vkAllocateMemory (%s) failed", tag); return -1; }
    if (vkBindBufferMemory(*reinterpret_cast<VkDevice*>(&g_backend.device), *pbuf, *pmem, 0) != VK_SUCCESS) {
        LOGE("vkBindBufferMemory (%s) failed", tag); return -1; }
    if (vkMapMemory(*reinterpret_cast<VkDevice*>(&g_backend.device), *pmem, 0, buffer_size, 0, pmapped) != VK_SUCCESS) {
        LOGE("vkMapMemory (%s) failed", tag); return -1; }
    fprintf(stderr, "[DEBUG] %s memtype=%u buf=%p mem=%p mapped=%p size=%llu\n", tag, mt,
            (void*)*pbuf, (void*)*pmem, *pmapped, (unsigned long long)buffer_size);
    return 0;
}

SceInt32 create_uniform_buffer() {
    LOGI("Vulkan: Creating uniform buffers (vertex@0 / fragment@1)...");
    VkDeviceSize buffer_size = 8192; // 【h137d诊断】扩大覆�?shader array[128] UBO 访问
    if (create_one_ubo(reinterpret_cast<VkBuffer*>(&g_backend.uniform_buffer),
                       reinterpret_cast<VkDeviceMemory*>(&g_backend.uniform_buffer_memory),
                       &g_backend.uniform_buffer_mapped, buffer_size, "UBO-vert(binding0)") != 0) return -1;
    if (create_one_ubo(reinterpret_cast<VkBuffer*>(&g_backend.uniform_buffer_frag),
                       reinterpret_cast<VkDeviceMemory*>(&g_backend.uniform_buffer_frag_memory),
                       &g_backend.uniform_buffer_frag_mapped, buffer_size, "UBO-frag(binding1)") != 0) return -1;
    // h144k20：此前这里把整块 UBO 填成绿色 vec4(0,1,0,1)（h137d 诊断遗留）�?
    //   它会在任何「没�?SetUniform 覆盖到」的 draw 上把常量块变成绿/黄色，是「整屏纯色」的帮凶�?
    //   改为清零，让未初始化的常量是中性值�?
    { float* _p=(float*)g_backend.uniform_buffer_mapped; for(SceUInt32 _i=0;_i+4<=(SceUInt32)buffer_size;_i+=4){ _p[_i]=0.f;_p[_i+1]=0.f;_p[_i+2]=0.f;_p[_i+3]=0.f; } }
    { float* _p=(float*)g_backend.uniform_buffer_frag_mapped; for(SceUInt32 _i=0;_i+4<=(SceUInt32)buffer_size;_i+=4){ _p[_i]=0.f;_p[_i+1]=0.f;_p[_i+2]=0.f;_p[_i+3]=0.f; } }
    LOGI("Uniform buffers created (%llu bytes each)", (unsigned long long)buffer_size);
    return 0;
}

SceInt32 create_test_texture() {
    LOGI("Vulkan: Creating test checkerboard texture...");
    const SceUInt32 TEX_W = 64, TEX_H = 64, CH = 4;
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkDeviceSize image_size = (VkDeviceSize)TEX_W * TEX_H * CH;

    // 1. checkerboard RGBA8 pixels
    std::vector<SceUInt8> pixels((size_t)image_size);
    for (SceUInt32 y = 0; y < TEX_H; y++) {
        for (SceUInt32 x = 0; x < TEX_W; x++) {
            SceUInt32 idx = (y * TEX_W + x) * CH;
            bool ck = ((x / 8) + (y / 8)) % 2 == 0;
            // horizontal red->green gradient multiplied by checker for easy UV inspection
            SceUInt8 r = (SceUInt8)(x * 255 / TEX_W);
            SceUInt8 g = (SceUInt8)(y * 255 / TEX_H);
            pixels[idx+0] = ck ? r : (SceUInt8)(255 - r);
            pixels[idx+1] = ck ? g : (SceUInt8)(255 - g);
            pixels[idx+2] = ck ? 240 : 40;
            pixels[idx+3] = 255;
        }
    }

    // 2. staging buffer (host visible)
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkBufferCreateInfo sbinfo{};
    sbinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbinfo.size = image_size;
    sbinfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    sbinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &sbinfo, nullptr, &staging) != VK_SUCCESS) { LOGE("tex: vkCreateBuffer staging fail"); return -1; }
    VkMemoryRequirements sreq; vkGetBufferMemoryRequirements(dev, staging, &sreq);
    VkMemoryAllocateInfo salloc{};
    salloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    salloc.allocationSize = sreq.size;
    salloc.memoryTypeIndex = find_memory_type(sreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &salloc, nullptr, &staging_mem) != VK_SUCCESS) { LOGE("tex: staging alloc fail"); return -1; }
    vkBindBufferMemory(dev, staging, staging_mem, 0);
    void* mapped = nullptr;
    vkMapMemory(dev, staging_mem, 0, image_size, 0, &mapped);
    memcpy(mapped, pixels.data(), (size_t)image_size);
    vkUnmapMemory(dev, staging_mem);

    // 3. VkImage (optimal, sampled + transfer dst)
    VkImageCreateInfo iinfo{};
    iinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iinfo.imageType = VK_IMAGE_TYPE_2D;
    iinfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    iinfo.extent = { TEX_W, TEX_H, 1 };
    iinfo.mipLevels = 1; iinfo.arrayLayers = 1;
    iinfo.samples = VK_SAMPLE_COUNT_1_BIT;
    iinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    iinfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    iinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    iinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult tr = vkCreateImage(dev, &iinfo, nullptr, reinterpret_cast<VkImage*>(&g_backend.test_texture_image));
    if (tr != VK_SUCCESS) { LOGE("tex: vkCreateImage fail %d", tr); return -1; }
    VkMemoryRequirements ireq;
    vkGetImageMemoryRequirements(dev, *reinterpret_cast<VkImage*>(&g_backend.test_texture_image), &ireq);
    VkMemoryAllocateInfo ialloc{};
    ialloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ialloc.allocationSize = ireq.size;
    ialloc.memoryTypeIndex = find_memory_type(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    tr = vkAllocateMemory(dev, &ialloc, nullptr, reinterpret_cast<VkDeviceMemory*>(&g_backend.test_texture_memory));
    if (tr != VK_SUCCESS) {
        // fallback: any visible memory
        ialloc.memoryTypeIndex = find_memory_type(ireq.memoryTypeBits, 0);
        vkAllocateMemory(dev, &ialloc, nullptr, reinterpret_cast<VkDeviceMemory*>(&g_backend.test_texture_memory));
    }
    vkBindImageMemory(dev, *reinterpret_cast<VkImage*>(&g_backend.test_texture_image),
        *reinterpret_cast<VkDeviceMemory*>(&g_backend.test_texture_memory), 0);

    // 4. one-time cmd: layout transitions + copy
    VkCommandBuffer cmd = begin_command_buffer();
    VkImage img = *reinterpret_cast<VkImage*>(&g_backend.test_texture_image);
    auto do_barrier = [&](VkImageLayout oldl, VkImageLayout newl, VkAccessFlags sa, VkAccessFlags da,
                          VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldl; b.newLayout = newl;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.srcAccessMask = sa; b.dstAccessMask = da;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    do_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { TEX_W, TEX_H, 1 };
    vkCmdCopyBufferToImage(cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    do_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    end_command_buffer(cmd); submit_command_buffer(cmd);

    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, staging_mem, nullptr);

    // 5. image view
    VkImageViewCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image = img;
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    vinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vinfo.subresourceRange.levelCount = 1; vinfo.subresourceRange.layerCount = 1;
    tr = vkCreateImageView(dev, &vinfo, nullptr, reinterpret_cast<VkImageView*>(&g_backend.test_texture_view));
    if (tr != VK_SUCCESS) { LOGE("tex: vkCreateImageView fail %d", tr); return -1; }

    // 6. sampler
    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samp.anisotropyEnable = VK_FALSE;
    samp.maxAnisotropy = 1.0f;
    samp.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samp.unnormalizedCoordinates = VK_FALSE;
    samp.compareEnable = VK_FALSE;
    samp.compareOp = VK_COMPARE_OP_ALWAYS;
    samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samp.minLod = 0.0f; samp.maxLod = 0.0f;
    tr = vkCreateSampler(dev, &samp, nullptr, reinterpret_cast<VkSampler*>(&g_backend.test_texture_sampler));
    if (tr != VK_SUCCESS) { LOGE("tex: vkCreateSampler fail %d", tr); return -1; }

    g_backend.test_texture_width = TEX_W;
    g_backend.test_texture_height = TEX_H;
    g_backend.test_texture_created = true;
    LOGI("Test texture created %ux%u (combined sampler ready, binding=1)", TEX_W, TEX_H);
    return 0;
}

// 6-face cube map test resource (binding=2). Each face gets a distinct base tint
// plus a checker so cube directional sampling is visually identifiable.
SceInt32 create_test_cube_texture() {
    LOGI("Vulkan: Creating test cube map (6 faces)...");
    const SceUInt32 S = 64, CH = 4, FACES = 6;
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    const SceUInt8 base[FACES][3] = {
        {220,70,70},{70,200,200},{70,200,70},{200,70,200},{80,115,230},{230,210,70}
    }; // +X -X +Y -Y +Z -Z
    const VkDeviceSize face_size = (VkDeviceSize)S*S*CH;
    const VkDeviceSize total = face_size*FACES;
    std::vector<SceUInt8> pixels((size_t)total);
    for (SceUInt32 f = 0; f < FACES; f++)
        for (SceUInt32 y = 0; y < S; y++)
            for (SceUInt32 x = 0; x < S; x++) {
                bool ck = ((x/8)+(y/8))%2==0;
                SceUInt32 idx = (f*S*S + y*S + x)*CH;
                pixels[idx+0] = ck ? base[f][0] : (SceUInt8)(base[f][0]/2);
                pixels[idx+1] = ck ? base[f][1] : (SceUInt8)(base[f][1]/2);
                pixels[idx+2] = ck ? base[f][2] : (SceUInt8)(base[f][2]/2);
                pixels[idx+3] = 255;
            }

    VkBuffer staging=VK_NULL_HANDLE; VkDeviceMemory staging_mem=VK_NULL_HANDLE;
    VkBufferCreateInfo sbinfo{}; sbinfo.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbinfo.size=total; sbinfo.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT; sbinfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(dev,&sbinfo,nullptr,&staging)!=VK_SUCCESS){LOGE("cube: staging buf fail");return -1;}
    VkMemoryRequirements sreq; vkGetBufferMemoryRequirements(dev,staging,&sreq);
    VkMemoryAllocateInfo salloc{}; salloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    salloc.allocationSize=sreq.size;
    salloc.memoryTypeIndex=find_memory_type(sreq.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(dev,&salloc,nullptr,&staging_mem)!=VK_SUCCESS){LOGE("cube: staging mem fail");return -1;}
    vkBindBufferMemory(dev,staging,staging_mem,0);
    void* mapped=nullptr; vkMapMemory(dev,staging_mem,0,total,0,&mapped);
    memcpy(mapped,pixels.data(),(size_t)total); vkUnmapMemory(dev,staging_mem);

    VkImageCreateInfo iinfo{}; iinfo.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iinfo.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    iinfo.imageType=VK_IMAGE_TYPE_2D; iinfo.format=VK_FORMAT_R8G8B8A8_UNORM;
    iinfo.extent={S,S,1}; iinfo.mipLevels=1; iinfo.arrayLayers=FACES;
    iinfo.samples=VK_SAMPLE_COUNT_1_BIT; iinfo.tiling=VK_IMAGE_TILING_OPTIMAL;
    iinfo.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    iinfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE; iinfo.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult tr=vkCreateImage(dev,&iinfo,nullptr,reinterpret_cast<VkImage*>(&g_backend.test_cube_image));
    if(tr!=VK_SUCCESS){LOGE("cube: vkCreateImage fail %d",tr);return -1;}
    VkMemoryRequirements ireq; vkGetImageMemoryRequirements(dev,*reinterpret_cast<VkImage*>(&g_backend.test_cube_image),&ireq);
    VkMemoryAllocateInfo ialloc{}; ialloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ialloc.allocationSize=ireq.size; ialloc.memoryTypeIndex=find_memory_type(ireq.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    tr=vkAllocateMemory(dev,&ialloc,nullptr,reinterpret_cast<VkDeviceMemory*>(&g_backend.test_cube_memory));
    if(tr!=VK_SUCCESS){ialloc.memoryTypeIndex=find_memory_type(ireq.memoryTypeBits,0);vkAllocateMemory(dev,&ialloc,nullptr,reinterpret_cast<VkDeviceMemory*>(&g_backend.test_cube_memory));}
    vkBindImageMemory(dev,*reinterpret_cast<VkImage*>(&g_backend.test_cube_image),*reinterpret_cast<VkDeviceMemory*>(&g_backend.test_cube_memory),0);

    VkCommandBuffer cmd=begin_command_buffer();
    VkImage cimg=*reinterpret_cast<VkImage*>(&g_backend.test_cube_image);
    VkImageMemoryBarrier to_dst{}; to_dst.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; to_dst.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; to_dst.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    to_dst.image=cimg; to_dst.srcAccessMask=0; to_dst.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount=1; to_dst.subresourceRange.layerCount=FACES;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&to_dst);
    VkBufferImageCopy regions[FACES]{};
    for(SceUInt32 f=0;f<FACES;f++){
        regions[f].bufferOffset=f*face_size;
        regions[f].bufferRowLength=S; regions[f].bufferImageHeight=S;
        regions[f].imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        regions[f].imageSubresource.mipLevel=0;
        regions[f].imageSubresource.baseArrayLayer=f; regions[f].imageSubresource.layerCount=1;
        regions[f].imageExtent={S,S,1};
    }
    vkCmdCopyBufferToImage(cmd,staging,cimg,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,FACES,regions);
    VkImageMemoryBarrier to_ro=to_dst;
    to_ro.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_ro.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_ro.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; to_ro.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&to_ro);
    end_command_buffer(cmd); submit_command_buffer(cmd);
    vkDestroyBuffer(dev,staging,nullptr); vkFreeMemory(dev,staging_mem,nullptr);

    VkImageViewCreateInfo vinfo{}; vinfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image=cimg; vinfo.viewType=VK_IMAGE_VIEW_TYPE_CUBE; vinfo.format=VK_FORMAT_R8G8B8A8_UNORM;
    vinfo.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    vinfo.subresourceRange.levelCount=1; vinfo.subresourceRange.layerCount=FACES;
    tr=vkCreateImageView(dev,&vinfo,nullptr,reinterpret_cast<VkImageView*>(&g_backend.test_cube_view));
    if(tr!=VK_SUCCESS){LOGE("cube: vkCreateImageView fail %d",tr);return -1;}

    VkSamplerCreateInfo samp{}; samp.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter=VK_FILTER_LINEAR; samp.minFilter=VK_FILTER_LINEAR;
    samp.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.anisotropyEnable=VK_FALSE; samp.maxAnisotropy=1.0f;
    samp.borderColor=VK_BORDER_COLOR_INT_OPAQUE_BLACK; samp.unnormalizedCoordinates=VK_FALSE;
    samp.compareEnable=VK_FALSE; samp.compareOp=VK_COMPARE_OP_ALWAYS;
    samp.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR; samp.minLod=0.0f; samp.maxLod=0.0f;
    tr=vkCreateSampler(dev,&samp,nullptr,reinterpret_cast<VkSampler*>(&g_backend.test_cube_sampler));
    if(tr!=VK_SUCCESS){LOGE("cube: vkCreateSampler fail %d",tr);return -1;}

    g_backend.test_cube_size=S; g_backend.test_cube_created=true;
    LOGI("Test cube map created %ux%u x6 (combined sampler ready, binding=2)",S,S);
    return 0;
}

// ============================================================================
// �?texture unit 的通用纹理上传（支持未压缩�?BC 块压缩�?D / Cube�?
// data �?mip0 顶层；cube �?layers 个面紧密连续�?
// ============================================================================
// ★ ARCH_462：字形图集监视的全局状态。捕获发生在 upload_texture_to_unit（上传路径），
//   扫描必须发生在 present_frame（每帧路径）—— 上一版把扫描也放在上传路径里，
//   结果 fc=3827 捕获之后 30 帧内没有新的纹理上传 ⇒ 条件永不成立 ⇒ 一次采样都没有。
// ★ ARCH_463 撤回声明：ARCH_459/460 认定的"字形图集"是**误认**。
//   fmt=98 的那个 1088x1088 对象走的是 render-target 路径（`[NATIVE] sceGxmRenderTargetMemSize
//   1088x1088` 与之同尺寸），内容恒为 IEEE-754 1.0f —— 那是**深度清屏值**，不是字形覆盖度；
//   而且它只被上传过一次。下面的 ATLAS-WATCH 已**解除武装**（三处判据改成 `false && ...`）：
//     · 它监视的是错对象；
//     · 更严重的是，捕获时存下的是**主机侧临时缓冲的裸指针**，30 帧后再读 = use-after-free。
//       这正是此前多次"日志没有 stop 行、进程直接消失"的真凶（不是 guest 崩溃）。
//   ⇒ 待整体删除；纹理侧的新取证手段见下面的精确内容指纹。
static const SceUInt8* g_atlasHost=nullptr;
static SceUInt64 g_atlasFc=0, g_atlasLastAt=0;
static SceUInt32 g_atlasW=0, g_atlasH=0;
static int g_atlasRep=0;

// ★ ARCH_468 / ARCH_481【精确内容指纹】按**来源地址**做键的全长哈希表。
//   动机：materialize 路径里的指纹缓存原先只按 `bytes/256` 抽样（4.7MB 纹理 stride≈18496，
//   即只看 256 个字节）。字形图集、影片帧这类**局部更新**的纹理几乎必然漏检 ⇒ 旧副本被
//   永久复用 ⇒ 贴图/文字可能显示成旧内容。一个内容缓存必须按**精确内容**做键。
//
//   ⚠ ARCH_481 自我纠错：初版这张表是**按 unit 索引**的（`g_texfull[unit]`），
//   而同一个 unit 上会轮流绑定不同纹理 ⇒ 彼此覆盖缓存 ⇒ **每次复用都误报一次
//   [HASHMISS]**。所以 ARCH_469 那批"某纹理被丢弃 100+ 次"的**计数不可信**
//   （计数器被我自己的键冲突放大了）。现在改成**按地址**线性探测的表。
//   注意：架构性缺陷本身（抽样指纹按构造就可能漏检）与这个计数问题是两件事。
static SceUInt32 g_full_va[64]={0};
static SceUInt64 g_full_h [64]={0};
static int p5d_full_slot(SceUInt32 va){
    SceUInt32 key=va?va:1u; int free_i=-1;
    for(int i=0;i<64;++i){
        if(g_full_va[i]==key) return i;
        if(free_i<0 && g_full_va[i]==0) free_i=i;
    }
    if(free_i<0) free_i=(int)(key&63u);     // 表满：覆盖（只影响命中率，不影响正确性）
    g_full_va[free_i]=key; g_full_h[free_i]=0;
    return free_i;
}

// 全长内容哈希：按 64 位字推进（8 字节/次），比逐字节快约 8 倍；尾字节单独处理。
static SceUInt64 p5d_tex_full_hash(const SceUInt8* p, SceUInt32 n){
    SceUInt64 h=1469598103934665603ull;
    SceUInt32 i=0;
    for(; i+8u<=n; i+=8u){ SceUInt64 w; std::memcpy(&w,p+i,8); h^=w; h*=1099511628211ull; }
    for(; i<n; ++i){ h^=(SceUInt64)p[i]; h*=1099511628211ull; }
    return h;
}

// ★ ARCH_523【主机指针 → guest VA 反查】`address_space.h` 没有反向翻译，但有
//   `region_count()` / `region_bounds()` / `translate()`，三者一组合就能反查。
//   动机（ARCH_522）：`[DRAWVTX]` 打出来的 `vtx` 是**64 位主机地址**
//   （如 `0x00000246EDEBE190`），不是 guest VA ⇒ 想对顶点缓冲做 guest 写监控，
//   必须先把它换回 guest VA，否则监控的是空气。
static SceUInt32 p5d_host_to_gva(const void* p){
    if(!p) return 0;
    SceUInt32 n=p5d::mem::region_count();
    for(SceUInt32 i=0;i<n;++i){
        SceUInt32 va=0,sz=0;
        if(!p5d::mem::region_bounds(i,&va,&sz) || !sz) continue;
        SceUInt8* h=p5d::mem::translate(va);
        if(!h) continue;
        const SceUInt8* q=(const SceUInt8*)p;
        if(q>=h && q<h+(size_t)sz) return va + (SceUInt32)(q-h);
    }
    return 0;
}

static SceInt32 upload_texture_to_unit(BackendState::UnitTexture& ut, SceUInt32 vkfmt,
                                       SceUInt32 w, SceUInt32 h, SceUInt32 layers, bool isCube,
                                       const SceUInt8* data, SceUInt32 data_size,
                                       SceUInt32 sw_r=0, SceUInt32 sw_g=0,
                                       SceUInt32 sw_b=0, SceUInt32 sw_a=0) {
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkDeviceSize total = data_size;
    // h144k34【纹理内容统计】red-FS 已证明几�?管线/RP 全对，dummy 归因实验又排除了"采到 dummy"�?
    //   于是只剩「真实纹理本身是黑的」。这里在上传入口直接统计**源数据的亮度分布**�?
    //   �?mean=0 �?上传进来的就是全黑数据，问题在上游（guest 纹理内存/解码/格式）；
    //   �?mean>0 �?上传内容正常，问题在采样侧（swizzle/通道映射/采样�?着色器常量）�?
    // �?h145k506【纹理上传时间线】原 cap=24 且打印到 stderr（可能被日志优化丢掉）�?
    //   现在：cap=2000、带 frame_count、并�?*只在 cap 内做统计循环**（统�?1MB 纹理要遍�?100 万次�?
    //   无条件执行会成为负担）。判据：`mean=0` �?上传的就是全黑数据（上游问题：解�?格式/内存）；
    //   `mean>0` �?上传内容正常（问题在采样侧）。帧号能告诉我们"title 阶段有没有新纹理"�?
    { static int _tc=0;
      if(_tc<2000){ _tc++;
        unsigned long long _sum=0; unsigned _mx=0; SceUInt32 _nz=0;
        for(SceUInt32 _i=0;_i<data_size;_i++){ unsigned _v=data[_i]; _sum+=_v; if(_v>_mx)_mx=_v; if(_v)_nz++; }
        double _mean = data_size? (double)_sum/(double)data_size : 0.0;
        // �?h145k524：同时打 stdout（原因同 [RTSTAT]——stderr 默认�?freopen �?NUL�?
        //   而开 P5D_VERBOSE 会拖慢。这条是"纹理链到底有没有真数�?的关键判据，必须常备。）
        printf("[TEXUPLOAD #%d] fc=%llu fmt=%u %ux%u L%u size=%u mean=%.2f max=%u nonzero=%u/%u sw=(%u,%u,%u,%u)\n",
          _tc,(unsigned long long)g_backend.frame_count,vkfmt,w,h,layers,data_size,_mean,_mx,_nz,data_size,sw_r,sw_g,sw_b,sw_a);
        fprintf(stderr,"[TEXUPLOAD #%d] fc=%llu fmt=%u %ux%u L%u size=%u mean=%.2f max=%u nonzero=%u/%u sw=(%u,%u,%u,%u)\n",
          _tc,(unsigned long long)g_backend.frame_count,vkfmt,w,h,layers,data_size,_mean,_mx,_nz,data_size,sw_r,sw_g,sw_b,sw_a);
        fflush(stdout); fflush(stderr); } }
    // �?ARCH_102【真实纹理解析】把 guest 纹理内存原样落盘，用于离线看"游戏真正的贴图长什么样"�?
    //   已知到达此处�?vkfmt=37 �?VK_FORMAT_R8G8B8A8_UNORM，故 .bin 可直接按 RGBA8 解释�?
    //   cap=40、只�?>=64KB，避免刷爆磁盘。判据：文件�?> 0 �?mean>0 �?角色/UI 贴图确已解析出来�?
    // �?ARCH_458：用户报告「主界面贴图显示不完全」（文字缺字）�?
    //   离线解码已有 dump 证明�?12x256 �?UI 图集（SAVE/LOADING/ONLINE）与 512x512 �?
    //   星形背景**数据都是完整可读�?* �?数据层没问题，嫌疑在字体/字形图集�?
    //   �?`[TEXUPLOAD]` 尺寸分布显示 **`1024x512` 上传�?244 �?*（最可能就是字体图集），
    //   但旧筛�?`data_size>=65536` + cap40 被前面的图集用光�?�?一个字体图集都没落盘�?
    //   改为**只落大纹�?*�?=1MB，即 1024x512 RGBA8 或更大），并放宽 cap�?
    // �?ARCH_459：定位字�?字形图集。`[TEXUPLOAD #487] fc=3834 fmt=98 1088x1088 L1
    //   size=4734976 sw=(3,1,1,2)` 是唯一一�?1088x1088�? sceGxmGetRenderTargetMemSize
    //   �?1088x1088 渲染目标 tiles=34x34），格式与其它纹理不同（vkfmt=98 而非 37）�?
    //   旧筛选（>=1MB + cap60）被 512x512/1024x512 用光�?*它一张都没落�?*�?
    //   现在只落 >=4.5MB 的（�?1088x1088 这一档），cap 放宽�?
    { static int _td=0;
      if(_td<8 && data_size>=4734976u){
        char _fn[260];
        snprintf(_fn,sizeof(_fn),"C:\\dev\\p5d_build\\texdump\\t%02d_%ux%u_L%u_f%u.bin",_td,w,h,layers,vkfmt);
        if(FILE* _f=fopen(_fn,"wb")){ fwrite(data,1,data_size,_f); fclose(_f);
          printf("[TEXDUMP] %s size=%u sw=(%u,%u,%u,%u)\n",_fn,data_size,sw_r,sw_g,sw_b,sw_a);
          fflush(stdout); _td++; } } }
    // ★★�?ARCH_460【字形图集是否被后续渲染填充？—�?(a1) vs (a2) 的判据】★★★
    //   事实�?088x1088 的图集全运行只上传过一次（fc=3612），内容�?1,183,744 个连�?
    //   `1.0f`（IEEE-754 0x3F800000，字节熵 1.5 bit）；`sw=(3,1,1,2)`=(R,ZERO,ZERO,ONE)
    //   �?采样�?R 通道当覆盖度、a 恒为 1 �?**�?1.0 意味着没有任何"墨迹"**�?
    //   且它**不在 `g_rt_pool`**（池里只�?960x544 �?224x68）⇒ 探针对它的唯一视图就是那次上传�?
    //   剩下两个子情形，修法完全不同�?
    //     (a1) 字形�?*上传之后**才被渲染进这块内存的 �?探针副本永久过期（探�?bug�?
    //     (a2) 字形渲染根本没发�?�?图集真的始终�?1.0（guest 路径 / HLE 问题�?
    //   判据：记住上传时的缓冲指针，之后�?500 帧扫�?�?1.0f 的像素数"�?
    //     该数**增长** �?(a1)�?*始终�?0** �?(a2)�?
    { const SceUInt8*& _atlas=g_atlasHost; SceUInt64& _lastAt=g_atlasLastAt; int& _aRep=g_atlasRep;
      SceUInt32& _aW=g_atlasW; SceUInt32& _aH=g_atlasH;   // �?自查修的 BUG：扫描必须用**捕获�?*�?w/h�?
                                       //   而不是当前这次上传的 w/h（上一版因此把分母写成 65536�?
      if(false && data_size>=4734976u && _atlas==nullptr){   // ARCH_463 解除武装：错对象 + 跨帧裸指针 UAF
        _atlas=data; _lastAt=g_backend.frame_count; _aW=w; _aH=h;
        printf("[ATLAS-WATCH] 捕获字形图集缓冲 size=%u %ux%u fc=%llu\n",data_size,w,h,(unsigned long long)_lastAt);
        fflush(stdout); }
      if(false && _atlas && _aRep<200 && g_backend.frame_count>=_lastAt+30){   // ARCH_463 解除武装
        ++_aRep; _lastAt=g_backend.frame_count;
        SceUInt64 _npx=(SceUInt64)_aW*(SceUInt64)_aH, _notOne=0; SceUInt32 _firstOff=0xFFFFFFFFu, _sum=0;
        for(SceUInt64 _i=0;_i<_npx;++_i){ SceUInt32 _v; std::memcpy(&_v,_atlas+_i*4,4);
          _sum+=_v&0xFF;
          if(_v!=0x3F800000u){ if(_firstOff==0xFFFFFFFFu) _firstOff=(SceUInt32)_i; ++_notOne; } }
        printf("[ATLAS-WATCH] fc=%llu �?.0像素=%llu/%llu 首个偏移=%u 低字节和=%u\n",
               (unsigned long long)g_backend.frame_count,(unsigned long long)_notOne,
               (unsigned long long)_npx,_firstOff,_sum);
        fflush(stdout); } }
    // 1. staging
    VkBuffer staging=VK_NULL_HANDLE; VkDeviceMemory smem=VK_NULL_HANDLE;
    VkBufferCreateInfo sbi{}; sbi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; sbi.size=total;
    sbi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT; sbi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(dev,&sbi,nullptr,&staging)!=VK_SUCCESS){LOGE("unit tex: staging buf fail");return -1;}
    VkMemoryRequirements sr; vkGetBufferMemoryRequirements(dev,staging,&sr);
    VkMemoryAllocateInfo sa{}; sa.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; sa.allocationSize=sr.size;
    sa.memoryTypeIndex=find_memory_type(sr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(dev,&sa,nullptr,&smem)!=VK_SUCCESS){LOGE("unit tex: staging mem fail");return -1;}
    vkBindBufferMemory(dev,staging,smem,0);
    void* mp=nullptr; vkMapMemory(dev,smem,0,total,0,&mp); memcpy(mp,data,(size_t)total); vkUnmapMemory(dev,smem);
    // 2. image
    VkImageCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType=VK_IMAGE_TYPE_2D; ii.format=(VkFormat)vkfmt; ii.extent={w,h,1};
    ii.mipLevels=1; ii.arrayLayers=layers; ii.samples=VK_SAMPLE_COUNT_1_BIT;
    ii.tiling=VK_IMAGE_TILING_OPTIMAL; ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(isCube) ii.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkResult tr=vkCreateImage(dev,&ii,nullptr,&ut.image);
    if(tr!=VK_SUCCESS){LOGE("unit tex: vkCreateImage fail %d fmt=%u %ux%u L%u",tr,vkfmt,w,h,layers);return -1;}
    VkMemoryRequirements ir; vkGetImageMemoryRequirements(dev,ut.image,&ir);
    VkMemoryAllocateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ia.allocationSize=ir.size;
    ia.memoryTypeIndex=find_memory_type(ir.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    tr=vkAllocateMemory(dev,&ia,nullptr,&ut.memory);
    if(tr!=VK_SUCCESS){ia.memoryTypeIndex=find_memory_type(ir.memoryTypeBits,0);vkAllocateMemory(dev,&ia,nullptr,&ut.memory);}
    vkBindImageMemory(dev,ut.image,ut.memory,0);
    // 3. barriers + per-layer copy
    VkCommandBuffer cmd=begin_command_buffer();
    VkImageMemoryBarrier b{}; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=ut.image; b.srcAccessMask=0; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount=1; b.subresourceRange.layerCount=layers;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkDeviceSize face = total/layers;
    std::vector<VkBufferImageCopy> regions(layers);
    for(SceUInt32 f=0;f<layers;f++){
        regions[f]={};
        regions[f].bufferOffset=f*face;
        regions[f].imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        regions[f].imageSubresource.mipLevel=0;
        regions[f].imageSubresource.baseArrayLayer=f; regions[f].imageSubresource.layerCount=1;
        regions[f].imageExtent={w,h,1};
    }
    vkCmdCopyBufferToImage(cmd,staging,ut.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,(SceUInt32)regions.size(),regions.data());
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    end_command_buffer(cmd); submit_command_buffer(cmd);
    vkDestroyBuffer(dev,staging,nullptr); vkFreeMemory(dev,smem,nullptr);
    // 4. view
    VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image=ut.image;
    vi.viewType=isCube?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D; vi.format=(VkFormat)vkfmt;
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount=1; vi.subresourceRange.layerCount=layers;
    vi.components.r=(VkComponentSwizzle)sw_r; vi.components.g=(VkComponentSwizzle)sw_g;
    vi.components.b=(VkComponentSwizzle)sw_b; vi.components.a=(VkComponentSwizzle)sw_a;
    tr=vkCreateImageView(dev,&vi,nullptr,&ut.view);
    if(tr!=VK_SUCCESS){LOGE("unit tex: view fail %d",tr);return -1;}
    // 5. sampler
    VkSamplerCreateInfo si{}; si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter=VK_FILTER_LINEAR; si.minFilter=VK_FILTER_LINEAR;
    VkSamplerAddressMode am=isCube?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeU=am; si.addressModeV=am; si.addressModeW=am;
    si.anisotropyEnable=VK_FALSE; si.maxAnisotropy=1.0f; si.borderColor=VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    si.unnormalizedCoordinates=VK_FALSE; si.compareEnable=VK_FALSE; si.compareOp=VK_COMPARE_OP_ALWAYS;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR; si.minLod=0; si.maxLod=0;
    tr=vkCreateSampler(dev,&si,nullptr,&ut.sampler);
    if(tr!=VK_SUCCESS){LOGE("unit tex: sampler fail %d",tr);return -1;}
    ut.width=w; ut.height=h; ut.layers=layers; ut.vk_format=vkfmt; ut.is_cube=isCube; ut.created=true;
    LOGI("unit texture uploaded: unit-slot fmt=%u %ux%u layers=%u cube=%d bytes=%u",vkfmt,w,h,layers,isCube,data_size);
    return 0;
}

// 1x1 不透明�?dummy，填充未使用 unit �?
//   h144k34【归因实验】P5D_DUMMY_RGB=r,g,b：把 dummy 改成醒目颜色（默�?255,255,255 不透明白）�?
//   用途：几何已被证明 100% 正确（red-FS 实验整屏变红），剩下的可能只有两类：
//     (A) 片元着色器采到的其实是 **dummy**（说明真�?guest 纹理压根没绑�?/ 没上传）�?
//     (B) 片元着色器采到的是**真实但内容为�?*的纹理（说明问题在纹理解�?格式/通道）�?
//   �?dummy 换成洋红后：整屏变洋�?�?属于 (A)；仍是黑�?�?属于 (B)。一刀切开两类原因�?
SceInt32 create_dummy_texture() {
    if(g_backend.dummy_created) return 0;
    SceUInt8 px[4]={255,255,255,255};
    { const char* _dr=getenv("P5D_DUMMY_RGB");
      if(_dr&&*_dr){ unsigned _r=255,_g=255,_b=255; if(sscanf(_dr,"%u,%u,%u",&_r,&_g,&_b)==3){
        px[0]=(SceUInt8)_r; px[1]=(SceUInt8)_g; px[2]=(SceUInt8)_b; px[3]=255;
        fprintf(stderr,"[DUMMY-RGB] dummy texture = (%u,%u,%u,255)\n",_r,_g,_b); fflush(stderr); } } }
    BackendState::UnitTexture ut;
    if(upload_texture_to_unit(ut,VK_FORMAT_R8G8B8A8_UNORM,1,1,1,false,px,4)!=0) return -1;
    g_backend.dummy_image=ut.image; g_backend.dummy_memory=ut.memory;
    g_backend.dummy_view=ut.view; g_backend.dummy_sampler=ut.sampler; g_backend.dummy_created=true;
    return 0;
}

// 真实 DDS（BC1/BC3 等）上传到指�?texture unit
SceInt32 load_dds_to_unit(SceUInt32 unit, const char* dds_path, SceUInt32 swizzle_index) {
    if(unit>=16){LOGE("load_dds_to_unit: bad unit %u",unit);return -1;}
    p5d::tex::DdsImage im;
    if(!p5d::tex::load_dds_file(dds_path,im)){LOGE("load_dds_to_unit: DDS load fail %s",dds_path);return -1;}
    SceUInt32 sr=0,sg=0,sb=0,sa=0; // 默认 IDENTITY
    if(swizzle_index<=7){ p5d::tex::TexComponentMap m=p5d::tex::texture_swizzle4(swizzle_index);
        sr=m.r;sg=m.g;sb=m.b;sa=m.a;
        LOGI("load_dds_to_unit unit=%u swizzle_index=%u -> map(%u,%u,%u,%u)",unit,swizzle_index,sr,sg,sb,sa);}
    return upload_texture_to_unit(g_backend.unit_textures[unit],im.vk_format,im.width,im.height,
                                  im.array_layers,im.is_cube,im.pixels.data(),(SceUInt32)im.pixels.size(),
                                  sr,sg,sb,sa);
}

// 原始 RGBA8 位图�?PSV swizzle_index(0..7) 做通道重排后上传到 unit（通道映射验证/运行时未压缩纹理路径�?
SceInt32 upload_rgba_swizzled(SceUInt32 unit, SceUInt32 w, SceUInt32 h,
                              const SceUInt8* rgba, SceUInt32 swizzle_index) {
    if(unit>=16 || !rgba) return -1;
    p5d::tex::TexComponentMap m=p5d::tex::texture_swizzle4(swizzle_index);
    return upload_texture_to_unit(g_backend.unit_textures[unit],VK_FORMAT_R8G8B8A8_UNORM,w,h,1,false,
                                  rgba,w*h*4,m.r,m.g,m.b,m.a);
}

// 运行�?materialize：解�?16B SceGxmTexture 控制块，用宿主像素缓冲上传到对应 texture unit�?
// hostPixels 为控制块 data_byte_addr 经虚拟地址层解析后的宿主指针（游戏真跑时由 self_loader 地址翻译提供）�?
SceInt32 upload_gxm_control_to_unit(SceUInt32 unit, const void* control16,
                                    const SceUInt8* hostPixels, SceUInt32 bytes) {
    if(unit>=16 || !control16 || !hostPixels) return -1;
    p5d::tex::ResolvedGxmTexture rt;
    if(!p5d::tex::resolve_gxm_texture((const SceUInt8*)control16, rt)){LOGE("gxm materialize: resolve fail unit=%u",unit);return -1;}
    if(rt.vk_format==p5d::tex::FMT_UNDEFINED){LOGE("gxm materialize: base=0x%08X needs transcode (PVRTC/palette), unit=%u",rt.info.base_format,unit);return -2;}
    SceUInt32 layers = rt.info.is_cube?6u:1u;
    LOGI("gxm materialize unit=%u vkfmt=%u %ux%u layers=%u cube=%d swz=(%u,%u,%u,%u) bytes=%u vaddr=0x%08X",
        unit,rt.vk_format,rt.info.width,rt.info.height,layers,(int)rt.info.is_cube,
        rt.swizzle_map.r,rt.swizzle_map.g,rt.swizzle_map.b,rt.swizzle_map.a,bytes,rt.info.data_byte_addr);
    return upload_texture_to_unit(g_backend.unit_textures[unit],rt.vk_format,rt.info.width,rt.info.height,
                                  layers,rt.info.is_cube,hostPixels,bytes,
                                  rt.swizzle_map.r,rt.swizzle_map.g,rt.swizzle_map.b,rt.swizzle_map.a);
}

// h144bp: GPU resources replaced during frame recording are queued for deferred destruction;
// they are only released after the fence wait in frame_acquire() proves the GPU finished the
// frame whose command buffer references them (avoids 'object destroyed while bound' VVL errors).
struct P5dGpuZombies {
    std::vector<VkSampler> samplers;
    std::vector<VkImageView> views;
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memories;
};
static P5dGpuZombies g_gpu_zombies;
static void p5d_flush_zombies(VkDevice dev) {
    for (size_t i=0;i<g_gpu_zombies.samplers.size();++i) if(g_gpu_zombies.samplers[i]) vkDestroySampler(dev,g_gpu_zombies.samplers[i],nullptr);
    for (size_t i=0;i<g_gpu_zombies.views.size();++i)    if(g_gpu_zombies.views[i])    vkDestroyImageView(dev,g_gpu_zombies.views[i],nullptr);
    for (size_t i=0;i<g_gpu_zombies.images.size();++i)   if(g_gpu_zombies.images[i])   vkDestroyImage(dev,g_gpu_zombies.images[i],nullptr);
    for (size_t i=0;i<g_gpu_zombies.memories.size();++i) if(g_gpu_zombies.memories[i]) vkFreeMemory(dev,g_gpu_zombies.memories[i],nullptr);
    g_gpu_zombies.samplers.clear(); g_gpu_zombies.views.clear(); g_gpu_zombies.images.clear(); g_gpu_zombies.memories.clear();
}
static void destroy_unit_texture(BackendState::UnitTexture& ut) {
    if(ut.is_rt_alias){ ut.is_rt_alias=false; ut.alias_view=VK_NULL_HANDLE; ut.alias_color=0; ut.created=false; return; }
    if (ut.sampler){ g_gpu_zombies.samplers.push_back(ut.sampler); ut.sampler=nullptr; }
    if (ut.view){ g_gpu_zombies.views.push_back(ut.view); ut.view=nullptr; }
    if (ut.image){ g_gpu_zombies.images.push_back(ut.image); ut.image=nullptr; }
    if (ut.memory){ g_gpu_zombies.memories.push_back(ut.memory); ut.memory=nullptr; }
    ut.created=false;
}

static VkImageView acquire_rt_alias_view(SceUInt32 cd);  // h144p 前向
// 控制�?data_byte_addr 经全局地址空间翻译得到宿主像素后上传（游戏真跑路径�?
static SceUInt32 g_texsrc_auto_f=0;   // h145k281: 最新解码帧 Y 平面地址（set_texsrc_addr 更新�?
void set_texsrc_addr(SceUInt32 addr){ g_texsrc_auto_f=addr; }
// h145k1133：guest 最近一�?openMovie 的片名与序号（定义在本文件后面）�?
//   TEXSRC=NV12 用它自动切换 open/title 两个预解 NV12 源�?
extern "C" const char* p5d_movie_requested(void);
extern "C" unsigned p5d_movie_request_seq(void);
// �?h145k1110c：这两个状态必须在 `upload_gxm_control_resolved` 之前可见（它用它们决定放完就停）�?
static int g_texsrc_done = 0;    // 影片已放�?�?停止覆盖
static int g_texsrc_reset = 0;   // guest 重新 openMovie �?帧号/时钟归零
SceInt32 upload_gxm_control_resolved(SceUInt32 unit, const void* control16) {
    // ★★★ ARCH_744【主纹理上传路径是否被调用？】★★★
    //   动机（ARCH_743）：全运行 `[TEXUPLOAD]` 只有 1 次（来自 create_dummy_texture）
    //   ⇒ 需要判定**主路径 `upload_gxm_control_resolved` 是否被调用 / 走到上传点**。
    //   本仪器在入口与"真正上传前"各打一次（按 unit 计数，前若干次打印，之后只累计）。
    //   标签 [TEXENT] / [TEXAT] 全局唯一。
    { static long _te=0; ++_te;
      if(_te<=12) { printf("[TEXENT] #%ld unit=%u ctrl=%p fc=%llu\n",
                           _te,unit,control16,(unsigned long long)g_backend.frame_count); fflush(stdout); } }
    if(unit>=16 || !control16) return -1;
    // �?h145k281【P5D_TEXSRC=AUTO】最新解码帧地址（由 set_texsrc_addr 更新�?
    SceUInt32& g_texsrc_auto = g_texsrc_auto_f;
    // ★★�?h145k280【P5D_TEXSRC=<hex>：直接显示一�?guest 内存】★★★
    //   动机：h145k277 实测我们已经解出�?*真实影片�?*——每�?783360 B�?60x544 NV12�?
    //   落在 0xB1300000 / 0xB13BF400 / …（[REALVDEC] got=783360）�?
    //   而第 19 轮已证明「全屏四边形（P5D_V15_FULLSCREEN=4�? FragCoord 采样（P5D_DIAG_FS_IN=6）�?
    //   能把任意纹理显示到窗口里。两者结�?�?**把解码帧直接显示出来**�?
    //   本开关忽略纹理控制块，改�?`P5D_TEXSRC` 指定�?guest 地址�?
    //   `P5D_TEXSRC_WH`（默�?960x544）个字节�?**Y 平面**，复制成 RGBA 后上传�?
    //   用法：P5D_TEXSRC=B1300000 [P5D_TEXSRC_WH=960x544]
    //         + P5D_V15_FULLSCREEN=4 + P5D_ONLY_FH=6952EF6C507D515E + P5D_DIAG_FS_IN=6
    { static int _ts=-1; static SceUInt32 _ta=0; static SceUInt32 _tw=0,_th=0;
      if(_ts<0){ _ts=0; const char* _e=std::getenv("P5D_TEXSRC");
        if(_e&&*_e){ if(std::strcmp(_e,"AUTO")==0||std::strcmp(_e,"auto")==0){ _ta=1u; _ts=2; }
          else if(std::strcmp(_e,"NV12")==0||std::strcmp(_e,"nv12")==0){ _ts=3; }
          else { _ta=(SceUInt32)std::strtoul(_e,nullptr,16); _ts=1; }
          _tw=960; _th=544;
          const char* _w=std::getenv("P5D_TEXSRC_WH");
          if(_w&&*_w){ unsigned a=0,b=0; if(std::sscanf(_w,"%ux%u",&a,&b)==2 && a&&b){ _tw=a; _th=b; } }
          printf("[TEXSRC] mode=%d src=0x%X %ux%u\n",_ts,_ta,_tw,_th); fflush(stdout); } }
      if(_ts==2) _ta=g_texsrc_auto;          // AUTO：用最新解码帧地址
      // ★★�?h145k1110【把 TEXSRC 覆盖范围收窄到「影片自己那一�?draw」】★★★
      //   动机（用户实�?NV12 出画后）：有**重影**——一层正常影�?+ 一层错误画面叠着�?
      //   画面**被拉�?*；且**出现时机不对**。三者同一个根因：
      //   下面两个分支�?*每一�?*纹理单元上传时都无条件替换内容。于是「影片四边形」以外的
      //   draw（后处理整屏 pass / UI / 过场底图）采样到的也是影片帧 �?叠加成重影、被各自�?
      //   几何拉伸、并在影片本不该出现的帧上出现�?
      //   用法：`P5D_TEXSRC_FH=<16hex>[,<16hex>…]` 只在这些片元程序哈希�?draw 上替换；
      //         `P5D_TEXSRC_UNIT=<n>` 只在�?n 号纹理单元上替换�?
      //         `P5D_TEXSRC_LOG=1` 打印每次被替换的 (片元哈希, 单元�?�?
      //   副作用（正是我们要的）：`_t0ms` �?`_ts==3` 分支内初始化，加闸门后它变成
      //   **"影片那一�?draw 第一次出�?的时�?*——而不再是"开机后第一次任意纹理上�?�?
      //   于是帧号从影片真正的�?0 帧开始推进（此前从中间开始播 = 时机不对 / 看不到片�?LOGO）�?
      bool _allow=true;
      { static std::vector<unsigned long long> _fhv; static int _teu=-2; static int _tlog=-1; static int _gini=-1;
        if(_gini<0){ _gini=0;
          const char* _ef=std::getenv("P5D_TEXSRC_FH");
          if(_ef&&*_ef){ const char* _p=_ef;
            while(*_p){ char* _e2=nullptr; unsigned long long _x=std::strtoull(_p,&_e2,16);
              if(_e2==_p) break; _fhv.push_back(_x); _p=_e2; while(*_p==','||*_p==' '||*_p=='\t')++_p; } }
          _teu=-1; const char* _eu=std::getenv("P5D_TEXSRC_UNIT"); if(_eu&&*_eu) _teu=std::atoi(_eu);
          _tlog=(std::getenv("P5D_TEXSRC_LOG")!=nullptr)?1:0;
          if(!_fhv.empty()||_teu>=0||_tlog){
            printf("[TEXSRC-GATE] fh_n=%zu unit=%d log=%d\n",_fhv.size(),_teu,_tlog); fflush(stdout); } }
        if(_teu>=0 && (int)unit!=_teu) _allow=false;
        if(_allow && !_fhv.empty()){ bool _h=false;
          for(size_t _i=0;_i<_fhv.size();_i++) if(_fhv[_i]==p5d::vulkan::g_cur_draw_fh){ _h=true; break; }
          if(!_h) _allow=false; }
        // ★★�?h145k1110b【默认排�?整屏 present pass"】★★★
        //   动机（用户实测）：画�?*被拉�?*。实测该 draw=`fh=57ABAA38F7202FA8`
        //   （顶点程�?`172A9E736C7DC35E`，rt=A0F00000/A1300000/A1700000，即 1024x544 的三�?
        //   backbuffer 槽，每帧 n�?04/1117 次）。它�?*整屏拷贝 pass**：把上一步的 RT 铺满�?
        //   backbuffer。我们若把影片帧塞进它采样的单元，就会得�?整屏被拉伸的影片"叠在正确画面�?
        //   ——这正是用户描述�?画面被拉�?+ 重影"里的那一层错误层�?
        //   `P5D_TEXSRC_EXCL_FH=<16hex>[,…]` 可再追加要排除的哈希；设 `EXCL_FH=none` 关闭默认排除�?
        { static std::vector<unsigned long long> _exv; static int _exi=-1;
          if(_exi<0){ _exi=0; _exv.push_back(0x57ABAA38F7202FA8ull);
            const char* _e=std::getenv("P5D_TEXSRC_EXCL_FH");
            if(_e&&*_e){ if(std::strcmp(_e,"none")==0) _exv.clear();
              else { const char* _p=_e;
                while(*_p){ char* _e2=nullptr; unsigned long long _x=std::strtoull(_p,&_e2,16);
                  if(_e2==_p) break; _exv.push_back(_x); _p=_e2; while(*_p==','||*_p==' '||*_p=='\t')++_p; } } }
            printf("[TEXSRC-EXCL] n=%zu\n",_exv.size()); fflush(stdout); }
          for(size_t _i=0;_i<_exv.size();_i++) if(_exv[_i]==p5d::vulkan::g_cur_draw_fh){ _allow=false; break; } }
        // ★★�?h145k1110b【影片打开前不覆盖 —�?保住片头 ATLUS / P-STUDIO / CRIWARE LOGO】★★★
        //   动机（用户实�?我没有看见公司LOGO"）：纯引擎基线实�?**fc=2800�?000 引擎自己画出�?
        //   ATLUS(2800/2840/2880) / P-STUDIO(2840/2920/3000) / CRIWARE(2880/2920/2960)** 三个 LOGO�?
        //   �?guest 是在 **fc�?015** �?`[MOVIEOPEN]` 打开影片的。我们的覆盖�?**fc=2264**
        //   （第一笔有纹理�?draw）就开始，把整�?LOGO 画面上采样到的内容全换成了影片帧
        //   �?"没有 LOGO" + "出现时机不对"�?
        //   修法：由 thumb_interpreter �?`[MOVIEOPEN]` 探针�?`g_movie_opened`�?
        //   覆盖只在 guest **确实请求播放影片之后**才生效。`P5D_TEXSRC_MOVIEONLY=0` 可关闭�?
        { static int _mo=-1;
          if(_mo<0){ _mo=1; const char* _e=std::getenv("P5D_TEXSRC_MOVIEONLY");
            if(_e&&*_e&&_e[0]=='0') _mo=0;
            printf("[TEXSRC-MOVIEONLY] %d\n",_mo); fflush(stdout); }
          if(_mo && !p5d::vulkan::g_movie_opened) _allow=false; }
        if(_allow && _tlog){ static std::map<unsigned long long,long long> _m; static long long _n=0;
          _m[((unsigned long long)p5d::vulkan::g_cur_draw_fh<<8)|(unsigned long long)unit]++; _n++;
          static long long _rep=0;
          if(_m.size()!=_rep){ _rep=(long long)_m.size();
            printf("[TEXSRC-DRAW] new site fh=%016llX unit=%u prim=%08X fc=%llu distinct=%zu tot=%lld\n",
                   (unsigned long long)p5d::vulkan::g_cur_draw_fh,unit,p5d::vulkan::g_cur_prim,
                   (unsigned long long)g_backend.frame_count,_m.size(),_n); fflush(stdout); } } }
      // \u2605\u2605\u2605 h145k1107\u3010P5D_TEXSRC=NV12\u3011\u771f NV12\u2192RGBA + \u6309\u6e32\u67d3\u5e27\u53f7\u9010\u5e27\u63a8\u8fdb \u2605\u2605\u2605
      //   \u7528\u6237\u5b9e\u6d4b TEXSRC=AUTO \u80fd\u770b\u5230\u5f71\u7247\u7b2c\u4e00\u5e27\uff08\u8bf4\u660e\u5f15\u64ce\u81ea\u5df1\u7684\u56db\u8fb9\u5f62\u786e\u5b9e\u5728\u753b\uff09\uff0c\u4f46
      //     (a) \u4ec5\u4e0a\u4f20 Y \u5e73\u9762 \u21d2 \u7070\u5ea6\u3001\u201c\u753b\u9762\u7279\u522b\u201d\uff1b
      //     (b) \u6e90\u5730\u5740\u53ea\u5728 sceAvcdecDecode \u65f6\u66f4\u65b0\uff08\u5168\u8f6e 7 \u6b21\uff09\u21d2 \u5e27\u53f7\u4e0d\u63a8\u8fdb\u3001\u52a8\u753b\u5361\u6b7b\u3002
      //   \u672c\u6a21\u5f0f\u76f4\u63a5\u8bfb\u5bbf\u4e3b\u9884\u89e3\u7684 NV12 \u5e8f\u5217\uff08\u9ed8\u8ba4 open_c_p5d000_nv12.raw\uff0c3084 \u5e27\uff09\uff0c
      //   \u7528 g_backend.frame_count \u63a8\u8fdb\u5e27\u53f7\uff0c\u505a BT.601 limited-range \u7684 NV12\u2192RGBA\u3002
      // h145k1110c：放完就�?—�?不再往画面上盖影片，把画面还给引擎（标题界面）�?
      if(g_texsrc_done && _ts==3) _allow=false;
      // ★★�?h145k1142【宿主全屏影片层在场时，禁用 TEXSRC 覆盖】★★★
      //   动机：`P5D_MOVIE_FS`（宿主把 NV12 �?BGRA 后全�?blit 到交换链）与 `P5D_TEXSRC`
      //   （把影片帧塞进引擎的后处理链纹理槽）�?*两条互相打架**的通路。同时开着时，
      //   MOVIE_FS 画出的正确影片会�?TEXSRC 那条错层盖住 —�?实测形状正是用户报的
      //   「斜角左上半正常、右下半被拉伸」（引擎后处理链的顶点数据在本后端残缺，
      //   `[GEOMSEE]` 实测 `cnt=6 vb=512` 却只有顶�?0 有效）�?
      //   �?影片层一旦上过屏，就�?TEXSRC 彻底关掉，只留正确的那一层�?
      if(_ts && p5d_movie_layer_active) _allow=false;
      if(_ts==3 && _allow){
        static std::FILE* _fp=nullptr; static long long _nf=0; static long long _shown=-1;
        static std::vector<SceUInt8> _nv; static std::vector<SceUInt8> _rgba3;
        static long long _rawseq=-1; static bool _auto_switch=true;
        if(!_fp || _rawseq!=(long long)p5d_movie_request_seq()){
          // ★★�?h145k1133【NV12 源跟着 guest 实际打开的影片切换】★★★
          //   动机（用户反馈）：开场影片放完后引擎进入标题影片相位，画面却是一片清屏色 —�?
          //   因为本模式此前只在第一次打开时固定读 `open_c_p5d000_nv12.raw`�?
          //   �?guest 每次 openMovie 都会�?`p5d_movie_request(name)` 把片名登记下�?
          //   （并有单调递增的序号）。这里按片名选源，并在序号变化时**重新打开**�?
          //   于是开场影片与标题影片都能显示，不再有"纯色空窗"�?
          //   仍可�?`P5D_TEXSRC_RAW` 强制指定某个文件（此时不做自动切换）�?
          if(_fp){ std::fclose(_fp); _fp=nullptr; _shown=-1; }
          const char* _pp=std::getenv("P5D_TEXSRC_RAW");
          if(_pp&&_pp[0]){ _auto_switch=false; }
          else {
            const char* _nm=p5d_movie_requested();
            if(_nm && std::strstr(_nm,"c_p5d001")) _pp="C:\\dev\\p5d_movies\\title_c_p5d001_nv12.raw";
            else _pp="C:\\dev\\p5d_movies\\open_c_p5d000_nv12.raw";
            _auto_switch=true; }
          _fp=std::fopen(_pp,"rb");
          _rawseq=(long long)p5d_movie_request_seq();
          if(_fp){ _fseeki64(_fp,0,SEEK_END); long long _sz=_ftelli64(_fp);
                   _nf=_sz/(long long)((size_t)_tw*_th*3u/2u); _fseeki64(_fp,0,SEEK_SET); }
          printf("[TEXSRC-NV12] file=%s frames=%lld %ux%u seq=%lld auto=%d\n",
                 _pp,_nf,_tw,_th,_rawseq,(int)_auto_switch); fflush(stdout); }
        if(_fp && _nf>0){
          const size_t _nvsz=(size_t)_tw*_th*3u/2u;
          if(_nv.size()<_nvsz) _nv.resize(_nvsz);
          // ★★�?h145k1110【帧号推进模型：wall（墙钟）| draw（跟引擎自己的渲染进度）】★★★
          //   动机：墙钟模式在**本机**是错的——模拟器每渲染一帧耗掉的真实时间远多于 1/29.97 秒，
          //   于是"影片"按真实时间跑完了 103 秒，而游戏自己的逻辑只走了很少的�?�?用户看到
          //   「出现时机不对」「动画像卡死/跳帧」。真正该跟的�?*引擎自己的播放进�?*�?
          //   影片那一�?draw 每出现一次，影片就前进一帧（可用 P5D_TEXSRC_DIV 细分）�?
          //   这样影片只在引擎**确实在画影片**的时候推进：游戏一收起影片（进 LOGO/读盘/菜单），
          //   影片自动停住，回到画面时从原处继续——与 CRI 播放器被暂停的行为一致�?
          //   `P5D_TEXSRC_SYNC=draw` 启用；`=wall` 或缺省保持旧行为�?
          //   `P5D_TEXSRC_DIV=<n>`：每 n 个引擎帧推进 1 个影片帧（默�?1）�?
          //   `P5D_TEXSRC_START=<n>`：起始影片帧（默�?0）�?
          static int _sync=-1; static double _div=1.0; static long long _start=0;
          if(_sync<0){ _sync=0;
            const char* _s=std::getenv("P5D_TEXSRC_SYNC");
            if(_s&&(_s[0]=='d'||_s[0]=='D')) _sync=1;
            const char* _d=std::getenv("P5D_TEXSRC_DIV");
            if(_d&&*_d){ double _v=std::atof(_d); if(_v>0.0) _div=_v; }
            const char* _st=std::getenv("P5D_TEXSRC_START");
            if(_st&&*_st) _start=std::strtoll(_st,nullptr,10);
            printf("[TEXSRC-SYNC] mode=%s div=%.3f start=%lld\n",_sync?"draw":"wall",_div,_start); fflush(stdout); }
          static long long _f=-1;
          // h145k1110c：世代号一变（guest 重新 openMovie）就把帧�?时钟/累加器全部归零�?
          if(g_texsrc_reset){ g_texsrc_reset=0; _f=-1; _shown=-1;
            printf("[TEXSRC-RESET] new movie open, epoch=%llu\n",
                   (unsigned long long)p5d::vulkan::g_movie_open_epoch); fflush(stdout); }
          if(_sync==1){
            // draw 模式：用 frame_count 作为"引擎�?刻度；同一渲染帧内多次上传只推进一次�?
            static unsigned long long _lastfc=(unsigned long long)-1; static double _acc=0.0;
            if(_f<0) _f=_start;
            if(g_backend.frame_count!=_lastfc){ _lastfc=g_backend.frame_count;
              _acc+=1.0/_div;
              while(_acc>=1.0){ _acc-=1.0; _f++; }
              if(_f>=_nf){ _f=_nf-1; g_texsrc_done=1; }   // 放完即停，不循环
            }
          } else {
            static long long _t0ms=-1;
            const long long _nowms=(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if(_t0ms<0){ _t0ms=_nowms;
              printf("[TEXSRC-T0] movie clock started at render frame %llu\n",
                     (unsigned long long)g_backend.frame_count); fflush(stdout); }
            _f=_start+(long long)((double)(_nowms-_t0ms)*30000.0/1001.0/1000.0);
            // �?h145k1110c�?*不再取模**。用户实�?影片放完后又从第 0 帧循�?正是
            //   `_f %= _nf` 造成的；而游戏这时已经进标题界面了。改成走到头就停在最后一�?
            //   并置 `g_texsrc_done`，下一次上传起就不再覆盖画面�?
            if(_f>=_nf){ _f=_nf-1; g_texsrc_done=1; }
            if(_f<0) _f=0;
          }
          // h145k1110：周期打印「影片帧 �?渲染帧」对应关系——用它可以直接量�?
          //   引擎渲染帧率�? Δ影片�?/ Δ渲染�?× 29.97），从而定�?draw 模式�?DIV�?
          { static long long _pl=-1;
            if(_f/120!=_pl){ _pl=_f/120;
              printf("[TEXSRC-FMAP] movie=%lld render=%llu\n",
                     _f,(unsigned long long)g_backend.frame_count); fflush(stdout); } }
          if(_f!=_shown){ _shown=_f;
            _fseeki64(_fp,(long long)_f*(long long)_nvsz,SEEK_SET);
            if(std::fread(_nv.data(),1,_nvsz,_fp)!=_nvsz) std::memset(_nv.data(),16,_nvsz);
            static int _c=0; if(_c<4){ _c++; printf("[TEXSRC-NV12] shown frame=%lld\n",_f); fflush(stdout); } }
          const SceUInt32 _n=_tw*_th;
          const SceUInt8* _Y=_nv.data(); const SceUInt8* _UV=_nv.data()+_n;
          if(_rgba3.size()<(size_t)_n*4u) _rgba3.resize((size_t)_n*4u);
          for(SceUInt32 _yy=0;_yy<_th;_yy++){
            const SceUInt32 _uvrow=(_yy>>1)*_tw;
            for(SceUInt32 _xx=0;_xx<_tw;_xx++){
              const SceUInt32 _i=_yy*_tw+_xx;
              int _Yv=(int)_Y[_i]-16; if(_Yv<0)_Yv=0;
              const SceUInt32 _uv=_uvrow+(_xx&~1u);
              const int _U=(int)_UV[_uv]-128, _V=(int)_UV[_uv+1]-128;
              int _r=(298*_Yv + 409*_V + 128)>>8;
              int _g=(298*_Yv - 100*_U - 208*_V + 128)>>8;
              int _b=(298*_Yv + 516*_U + 128)>>8;
              if(_r<0)_r=0; if(_r>255)_r=255;
              if(_g<0)_g=0; if(_g>255)_g=255;
              if(_b<0)_b=0; if(_b>255)_b=255;
              _rgba3[_i*4]=(SceUInt8)_r; _rgba3[_i*4+1]=(SceUInt8)_g;
              _rgba3[_i*4+2]=(SceUInt8)_b; _rgba3[_i*4+3]=255; } }
          BackendState::UnitTexture& _ut3=g_backend.unit_textures[unit];
          if(_ut3.created) destroy_unit_texture(_ut3);
          if(upload_texture_to_unit(_ut3,VK_FORMAT_R8G8B8A8_UNORM,_tw,_th,1,false,
                                    _rgba3.data(),(SceUInt32)_rgba3.size())==0) return 0; } }
      if(_ts && _ta && _allow){
        SceUInt32 _n=_tw*_th;
        if(p5d::mem::contains(_ta,_n)){
          SceUInt8* _y=p5d::mem::translate(_ta);
          if(_y){ static std::vector<SceUInt8> _rgba;
            if(_rgba.size()<(size_t)_n*4u) _rgba.resize((size_t)_n*4u);
            unsigned long long _sum=0;
            for(SceUInt32 _i=0;_i<_n;_i++){ SceUInt8 _v=_y[_i]; _sum+=_v;
              _rgba[_i*4]=_v; _rgba[_i*4+1]=_v; _rgba[_i*4+2]=_v; _rgba[_i*4+3]=255; }
            BackendState::UnitTexture& _ut=g_backend.unit_textures[unit];
            if(_ut.created) destroy_unit_texture(_ut);
            if(upload_texture_to_unit(_ut,VK_FORMAT_R8G8B8A8_UNORM,_tw,_th,1,false,_rgba.data(),(SceUInt32)_rgba.size())==0){
              static int _c=0; if(_c<6){ _c++;
                printf("[TEXSRC] uploaded %ux%u from 0x%X  Y-mean=%.1f\n",_tw,_th,_ta,(double)_sum/(double)_n); fflush(stdout); }
              return 0; } } } } }
    p5d::tex::ResolvedGxmTexture rt;
    if(!p5d::tex::resolve_gxm_texture((const SceUInt8*)control16,rt)){
      { static int _mf=0; if(_mf<20){ _mf++; printf("[MOVTEX-FAIL] #%d unit=%u ctrl=%p\n",_mf,unit,control16); fflush(stdout); } }
      return -1; }
    // ★★�?h145k928【电影纹�?(960x544) 到底解析成什么】★★★
    //   h145k927 已证：把真实影片帧写�?**正确�?* 三条 960x544 纹理
    //   (TEXINIT #9/#10/#11 data=0xAEB00000/0xAEE00000/0xAF100000) 画面**仍然全黑**
    //   �?说明问题不在"guest 内存里有没有内容"，而在这一跳的解析/上传�?
    //   这里把每一�?960x544 控制块的解析结果打出来�?
    if(rt.info.width==960u && rt.info.height==544u){
      static int _mz=0; if(_mz<16){ _mz++;
        printf("[MOVTEX] #%d unit=%u ctrl=%p data=0x%X base=0x%X w=%u h=%u vkfmt=%d cube=%u\n",
          _mz,unit,control16,rt.info.data_byte_addr,rt.info.base_format,rt.info.width,rt.info.height,
          (int)rt.vk_format,(unsigned)rt.info.is_cube); fflush(stdout); } }
    bool isP8=(rt.info.base_format==0x95000000u), isP4=(rt.info.base_format==0x94000000u), isIndexed=isP8||isP4;
    if(rt.vk_format==p5d::tex::FMT_UNDEFINED && !isIndexed){LOGE("resolved: base=0x%08X needs transcode unit=%u",rt.info.base_format,unit);return -2;}
    SceUInt32 layers=rt.info.is_cube?6u:1u;
    SceUInt32 W=rt.info.width,H=rt.info.height;
    SceUInt32 bytes=isIndexed?(isP8?W*H:(W*H+1)/2):p5d::tex::texture_byte_size(rt.vk_format,W,H,layers);
    // ★★�?h145k229【判决性实�?P5D_TEXPAT=<hex字节>】把**每一张被采样的纹�?*�?guest 数据�?
    //   统一填成已知图案。动机：全量去重统计显示所有被采样的美术纹理内�?*都是 0**
    //   （A0F/A13/A17 均�?0、AE100000 均�?0、A6C00680/A6D01080/A6C80B80 均�?0），
    //   而几�?[UPLOAD] NDC+UV 正常)、管线、shader、UBO、纹理绑定、清屏、上�?
    //   （P5D_REALFS=r 能整屏变红）均已**单独**验证可用 �?必须做一次判决：
    //     填图案后画面出现图案  �?"纹理内容为空"就是唯一阻塞，充分条件成立；
    //     填图案后画面仍全�?   �?阻塞在更下游（采�?描述�?写屏次序），需要另找�?
    //   �?必须放在 RT-alias 分支**之前**：采样最多的�?backbuffer(A0F/A13/A17) 走的�?alias 路径�?
    { static int _pvp=-1;
      if(_pvp==-1){ const char* _e=std::getenv("P5D_TEXPAT"); _pvp = (_e&&*_e)?(int)std::strtoul(_e,nullptr,16):-2; }
      if(_pvp>=0 && bytes){ SceUInt8* _q=p5d::mem::translate(rt.info.data_byte_addr);
        if(_q && p5d::mem::contains(rt.info.data_byte_addr,bytes)){ std::memset(_q,(unsigned char)(_pvp&0xFF),bytes);
          static int _pn=0; if(_pn<12){ _pn++;
            printf("[TEXPAT #%d] va=%08X %ux%u bytes=%u -> 全部�?0x%02X\n",_pn,rt.info.data_byte_addr,W,H,bytes,_pvp&0xFF);
            fflush(stdout);} } } }
    // ★★�?h145k230【判决实�?P5D_TEXGRAY=<hex>】在 **Vulkan 图像层面**把所有被采样的纹�?
    //   替换为纯色。必须做在图像层面而不�?guest 内存：RT 当纹理采样走的是
    //   `acquire_rt_alias_view`（VkImage 的别名视图）�?*完全绕过 guest 线性缓�?*
    //   �?P5D_TEXPAT / P5D_TEXFILL 这类"往 guest 内存�?的实验对它们**必然无效**�?
    //     这正是历史上多轮"填了也没变化"的真正原因�?
    //   判据：黑屏后画面出现该纯�?�?渲染/采样/上屏链路完整，唯一缺的就是纹理内容�?
    { static int _tg=-1;
      if(_tg==-1){ const char* _e=std::getenv("P5D_TEXGRAY"); _tg=(_e&&*_e)?(int)std::strtoul(_e,nullptr,16):-2; }
      if(_tg>=0){ unsigned char _g=(unsigned char)(_tg&0xFF); unsigned char _p8[8*8*4];
        for(int _i=0;_i<8*8;_i++){ _p8[_i*4+0]=_g; _p8[_i*4+1]=_g; _p8[_i*4+2]=_g; _p8[_i*4+3]=0xFF; }
        BackendState::UnitTexture& _ut=g_backend.unit_textures[unit];
        if(_ut.created) destroy_unit_texture(_ut);
        if(upload_texture_to_unit(_ut,VK_FORMAT_R8G8B8A8_UNORM,8,8,1,false,_p8,(SceUInt32)sizeof(_p8))==0){
          static int _tn=0; if(_tn<8){ _tn++;
            printf("[TEXGRAY #%d] unit=%u va=%08X -> 8x8 �?0x%02X\n",_tn,unit,rt.info.data_byte_addr,_g); fflush(stdout);} }
        return 0; } }
    { VkImageView _av = acquire_rt_alias_view(rt.info.data_byte_addr);
      if(_av){ BackendState::UnitTexture& _au=g_backend.unit_textures[unit];
        if(_au.created) destroy_unit_texture(_au);
        _au.is_rt_alias=true; _au.alias_view=_av; _au.alias_color=rt.info.data_byte_addr;
        _au.created=true; _au.width=W; _au.height=H; _au.vk_format=rt.vk_format; _au.layers=layers; _au.is_cube=rt.info.is_cube;
        _au.last_vaddr=rt.info.data_byte_addr; std::memcpy(_au.last_ctrl,control16,16);
        // h144k89 备注：这里打印的 W/H �?*纹理控制块自己声明的尺寸**（不是池 RT 的）�?
        //   实测被当别名采样时声明为 1024x544，而对�?pass 的目标面只有 960x544（`[BSIMG] cd=... 960x544`），
        //   别名 copy �?extent 也是 960x544 �?采样面与目标面尺寸不一致，�?alias 右侧 64 列未初始化�?
        // �?h145k471【性能】`[RT-ALIAS]` �?stdout 侧第三热点（5376 �?5 分钟）⇒ 默认静音�?
        { static int _vb3=-1; if(_vb3<0) _vb3=(getenv("P5D_VERBOSE")?1:0);
          if(_vb3){ printf("[RT-ALIAS] unit=%u cd=%08X tex=%ux%u\n",unit,rt.info.data_byte_addr,W,H); fflush(stdout); } }
        return 0; } }
    { static int _am=0; if(_am<24){_am++; printf("[ALIASMISS] unit=%u data=%08X W=%u H=%u vkf=%u\n",unit,rt.info.data_byte_addr,W,H,(SceUInt32)rt.vk_format); fflush(stdout);} }
    SceUInt8* px=p5d::mem::translate(rt.info.data_byte_addr);
    if(!px){LOGE("resolved: vaddr=0x%08X not in address space unit=%u",rt.info.data_byte_addr,unit);return -3;}
    if(!p5d::mem::contains(rt.info.data_byte_addr,bytes)){LOGE("resolved: texture crosses region vaddr=0x%08X bytes=%u",rt.info.data_byte_addr,bytes);return -4;}
    // ★ ARCH_471【整帧纹理转储】`P5D_TEXDUMP_FRAME=<fc>[,<fc>...]`（最多 4 个）
    //   在该帧把**每一张被 materialize 的纹理**从 **guest 内存**（`px`，不是主机侧临时缓冲）
    //   原样转储。用途：一次运行就拿到"主界面这一帧用到的全部贴图"，字形图集可以用眼睛
    //   直接认出来 —— 不必再去猜地址，也不必依赖指纹判据（避免又一次误认对象）。
    //   注意必须放在指纹复用判断**之前**，否则内容没变的纹理会在 `return 1` 处被跳过。
    // ★ ARCH_476【按地址定向转储】`P5D_TEXDUMP_VA=<hex>[,<hex>...]`（最多 8 个）
    //   与帧号无关：第一次 materialize 到该地址就把 **guest 内容 + 调色板**落盘。
    //   动机：`P5D_TEXDUMP_FRAME` 的"整帧普查"路线已实测失败 —— materialize 每帧只被调用
    //   约 1 次，且**大部分纹理走 RT-alias 提前 return**，根本不经过这里（实测每帧仅 1 张）。
    //   用途：针对 HASHMISS 名单里的具体纹理（如主界面标签图 A6C5AC00）取到"带调色板的真图"，
    //   判定它到底是"素材本身就有缺陷"还是"被画进去的画布"。
    { static SceUInt32 _tva[8]={0}; static int _tvn=-2; static int _tvd[8]={0};
      if(_tvn==-2){ _tvn=0;
        const char* _e=std::getenv("P5D_TEXDUMP_VA");
        if(_e&&*_e){ char _b[256]; std::snprintf(_b,sizeof(_b),"%s",_e);
          char* _s=_b;
          while(_s&&*_s&&_tvn<8){ char* _c=std::strchr(_s,','); if(_c)*_c=0;
            _tva[_tvn++]=(SceUInt32)std::strtoul(_s,nullptr,16); _s=_c?(_c+1):nullptr; } }
        printf("[TEXVA-CFG] env=%s n=%d",_e?_e:"(null)",_tvn);
        for(int _i=0;_i<_tvn;++_i) printf(" %08X",_tva[_i]);
        printf("\n"); fflush(stdout); }
      for(int _i=0;_i<_tvn;++_i){
        // ★ ARCH_478/492：对每个目标地址连续转储，并新增 `P5D_TEXDUMP_VA_EVERY=<n>`：
        //   同一地址**至少间隔 n 帧**才再转储一次（默认 1 = 原来的"连续 40 次"行为）。
        //   动机：字形精灵缓存是**按地址复用**的 —— 同一块 32x32 内存会先后装不同字形。
        //   要判定"屏幕上的错字/缺字"是**槽位内容错**还是**槽位复用后我们没刷新**，
        //   必须看同一地址在**长周期**上的内容变化（默认连续 40 次只覆盖约 4 帧，看不出复用）。
        static long long _valast[8]={0}; static long long _vaever=-2;
        if(_vaever==-2){ _vaever=1; const char* _s=std::getenv("P5D_TEXDUMP_VA_EVERY");
          if(_s&&*_s){ _vaever=std::atoll(_s); if(_vaever<1) _vaever=1; } }
        if(_tvd[_i]>=40 || _tva[_i]!=rt.info.data_byte_addr || !bytes) continue;
        if(_tvd[_i]>0 && (long long)g_backend.frame_count < _valast[_i]+_vaever) continue;
        ++_tvd[_i]; _valast[_i]=(long long)g_backend.frame_count;
        char _p[256];
        std::snprintf(_p,sizeof(_p),"C:\\dev\\p5d_build\\texframe\\va_%08X_%ux%u_vf%u_n%02d_fc%llu.raw",
                      rt.info.data_byte_addr,W,H,(unsigned)rt.vk_format,_tvd[_i],(unsigned long long)g_backend.frame_count);
        if(std::FILE* _f=std::fopen(_p,"wb")){ std::fwrite(px,1,bytes,_f); std::fclose(_f);
          printf("[TEXVA] n=%d %s bytes=%u base=0x%08X unit=%u indexed=%d\n",_tvd[_i],_p,bytes,rt.info.base_format,unit,(int)isIndexed);
          fflush(stdout); }
        if(isIndexed){
          SceUInt32 _nent=isP8?256u:16u;
          SceUInt8* _pp=(rt.info.palette_byte_addr&&p5d::mem::contains(rt.info.palette_byte_addr,_nent*4u))
                        ? p5d::mem::translate(rt.info.palette_byte_addr):nullptr;
          if(_pp){ char _q[256];
            std::snprintf(_q,sizeof(_q),"C:\\dev\\p5d_build\\texframe\\vapal_%08X_%s_%u.raw",
                          rt.info.data_byte_addr,isP8?"P8":"P4",_nent);
            if(std::FILE* _g=std::fopen(_q,"wb")){ std::fwrite(_pp,1,_nent*4u,_g); std::fclose(_g);
              printf("[TEXVA-PAL] %s nent=%u palva=0x%08X\n",_q,_nent,rt.info.palette_byte_addr); fflush(stdout); } } } } }
    { static long long _tdf[4]={-2,-2,-2,-2};
      static int _tdfdone[4]={0,0,0,0};
      static SceUInt32 _seen[600]; static int _ns=0;
      if(_tdf[0]==-2){
        for(int _i=0;_i<4;++_i) _tdf[_i]=-1;
        const char* _e=std::getenv("P5D_TEXDUMP_FRAME");
        if(_e&&*_e){ char _b[128]; std::snprintf(_b,sizeof(_b),"%s",_e);
          char* _s=_b; int _i=0;
          while(_s&&*_s&&_i<4){ char* _c=std::strchr(_s,','); if(_c)*_c=0;
            _tdf[_i++]=std::atoll(_s); _s=_c?(_c+1):nullptr; } }
        // ★ ARCH_475 诊断：把解析结果与当前帧号打出来。上一版一条转储都没产出，
        //   而运行确实越过了目标帧 ⇒ 必须先确认"配置读到了什么"，不能再靠猜。
        printf("[TEXFRAME-CFG] env=%s tdf=[%lld,%lld,%lld,%lld] fc_now=%llu\n",
               _e?_e:"(null)",_tdf[0],_tdf[1],_tdf[2],_tdf[3],
               (unsigned long long)g_backend.frame_count);
        fflush(stdout); }
      for(int _i=0;_i<4;++_i){
        // ★ 用**窗口** [target, target+3] 而非精确相等：精确相等在"该帧恰好没有 materialize
        //   调用"时会永久错过（这是上一版 0 条转储的可能原因之一）。只触发一次。
        if(_tdf[_i]<0 || _tdfdone[_i] || !bytes) continue;
        if((long long)g_backend.frame_count<(long long)_tdf[_i]) continue;
        if((long long)g_backend.frame_count>(long long)_tdf[_i]+3) continue;
        _tdfdone[_i]=1; _ns=0;
        printf("[TEXFRAME-BEGIN] target=%lld actual_fc=%llu\n",_tdf[_i],
               (unsigned long long)g_backend.frame_count); fflush(stdout);
        int _have=0;
        for(int _j=0;_j<_ns;++_j) if(_seen[_j]==rt.info.data_byte_addr){ _have=1; break; }
        if(_have) continue;
        // 小纹理（<64KB，多为 32x32/256x256 UI 元素）限量 400 张；大纹理（可能是图集）
        // 单独放宽到 600 —— 免得被一大堆小图把名额占满、恰好漏掉要找的那张。
        if(bytes<65536u){ if(_ns>=400) continue; } else { if(_ns>=600) continue; }
        _seen[_ns++]=rt.info.data_byte_addr;
        char _p[256];
        std::snprintf(_p,sizeof(_p),"C:\\dev\\p5d_build\\texframe\\f%lld_%03d_%08X_%ux%u_vf%u.raw",
                      _tdf[_i],_ns-1,rt.info.data_byte_addr,W,H,(unsigned)rt.vk_format);
        if(std::FILE* _f=std::fopen(_p,"wb")){ std::fwrite(px,1,bytes,_f); std::fclose(_f);
          printf("[TEXFRAME] %s bytes=%u base=0x%08X unit=%u\n",_p,bytes,rt.info.base_format,unit); fflush(stdout); }
        // ★ ARCH_472 索引纹理（P8/P4）必须连**调色板**一起转储，否则离线把"索引值当灰度"看会失真
        //   （`O` 显示成方框之类），历史上已多次因此误判。注意 `[TEXPAL]` 走 stderr，而 stderr
        //   在探针里被 freopen 到 NUL ⇒ 那条日志永远看不到，所以这里必须落文件。
        if(isIndexed){
          SceUInt32 _nent=isP8?256u:16u;
          SceUInt8* _pp=(rt.info.palette_byte_addr&&p5d::mem::contains(rt.info.palette_byte_addr,_nent*4u))
                        ? p5d::mem::translate(rt.info.palette_byte_addr):nullptr;
          if(_pp){ char _q[256];
            std::snprintf(_q,sizeof(_q),"C:\\dev\\p5d_build\\texframe\\pal_%08X_%s_%u.raw",
                          rt.info.data_byte_addr,isP8?"P8":"P4",_nent);
            if(std::FILE* _g=std::fopen(_q,"wb")){ std::fwrite(_pp,1,_nent*4u,_g); std::fclose(_g);
              printf("[TEXFRAME-PAL] %s nent=%u palva=0x%08X\n",_q,_nent,rt.info.palette_byte_addr); fflush(stdout); } } }
      }
    }
    // ★★�?h145k133【内容指纹缓存】—�?修复"永不重传"缺陷�?
    //   旧逻辑（materialize_fragment_textures）只比较 vaddr + 16B 控制块，
    //   **完全不看纹理内容**。影片帧缓冲地址固定、内容逐帧改写 �?首次上传（黑）之�?
    //   永远不再上传 �?屏幕永远是黑的，尽管 guest 内存里已经是真实解码帧�?
    //   这里�?guest 数据�?256 点跨步指纹；指纹不变才复用，变了就重传�?
    {
        static int _nofp=-1; if(_nofp<0) _nofp=std::getenv("P5D_NO_TEXHASH")?1:0;
        BackendState::UnitTexture& _u2=g_backend.unit_textures[unit];
        if(!_nofp && bytes){
            SceUInt32 _step=bytes/256u; if(!_step)_step=1u;
            SceUInt32 _hsz=2166136261u;
            for(SceUInt32 _i=0;_i<bytes;_i+=_step){ _hsz^=(SceUInt32)px[_i]; _hsz*=16777619u; }
            if(_u2.created && _u2.last_vaddr==rt.info.data_byte_addr && _u2.content_hash==_hsz
               && std::memcmp(_u2.last_ctrl,control16,16)==0){
                // ★ ARCH_468：抽样指纹命中 **不等于** 内容未变 —— 必须补一次**精确**确认。
                //   `_step>1` 时上面只看了 256 个字节（stride≈18496 的 4.7MB 纹理 ⇒ 256/4.7M），
                //   20x20 的新字形被命中的概率约 2% ⇒ 图集更新几乎必然漏检 ⇒ 旧副本被永久复用。
                //   精确相同才复用；否则落到下面正常的上传路径，并打一条 [HASHMISS] 作为证据。
                if(_step>1u){
                    SceUInt64 _full=p5d_tex_full_hash(px,bytes);
                    int _sl=p5d_full_slot(rt.info.data_byte_addr);
                    if(g_full_h[_sl]==_full){ return 1; }   // 全长也相同 —— 真的可以复用
                    { static int _hm=0; if(_hm<24){ ++_hm;
                        printf("[HASHMISS] unit=%u va=%08X %ux%u bytes=%u 抽样命中但全长不同 => 重新上传\n",
                               unit,rt.info.data_byte_addr,W,H,bytes); fflush(stdout); } }
                    g_full_h[_sl]=_full;
                }
                return 1;   // 内容确实没变 �?真的可以复用
            }
            _u2.content_hash=_hsz;
        } else { _u2.content_hash=0; }
    }
    // ===== h144k98【场景纹理内容取证】按地址打印"分通道均�?，判定进入主面的那张纹理到底是什么颜色�?
    //   动机：`[DRAWSRC]` 显示每帧开头有两发 draw 把真实纹�?`A6D01080`(1024x512) 画进主面�?
    //   而主面最终是**纯洋�?* �?必须回答"是纹理本身就是洋红，还是采样/通道错了"�?
    //   �?4 个字节位置分别求均值（�?RGBA/BGRA 未压缩格式即 R/G/B/A 的近似；�?BC 压缩只是粗略纹理统计）�?
    // �?h145k505【放开上限 + 打印纹理上传�?时间�?】原�?cap=300，只能看到最早的 300 次上�?
    //   （历史上由此得出"全程只上�?5 张纹�?的结论，实为 cap 假象）�?
    //   现在改为：默认打印前 5000 次，�?*每次打印都带 frame_count**，于�?title 阶段有没有上传纹�?
    //   变成可直接读的事实（配合 `P5D_TEXCH_ALL` 可全量）�?
    { static int _tc2=0;
      { static int _lim=-1; if(_lim<0){ const char*_l=getenv("P5D_TEXCH_ALL"); _lim=(_l&&*_l)?1000000:5000; }
      if(_tc2<_lim){ _tc2++;
        unsigned long long _s0=0,_s1=0,_s2=0,_s3=0; SceUInt32 _n=bytes/4u;
        for(SceUInt32 _i=0;_i<_n;_i++){ const SceUInt8* q=px+_i*4u; _s0+=q[0]; _s1+=q[1]; _s2+=q[2]; _s3+=q[3]; }
        fprintf(stderr,"[TEXCH#%d] fc=%llu va=%08X fmt=%u %ux%u bytes=%u n=%u avg[b0..b3]=%.1f %.1f %.1f %.1f\n",
          _tc2,(unsigned long long)g_backend.frame_count,
          rt.info.data_byte_addr, rt.vk_format, W, H, bytes, _n,
          _n?(double)_s0/_n:0.0, _n?(double)_s1/_n:0.0, _n?(double)_s2/_n:0.0, _n?(double)_s3/_n:0.0);
        fflush(stderr); } } }
    // ===== h145k104【★ 索引纹理(P8/P4)到底是不�?内容"�?====
    //   动机：本轮实�?全程只有 5 张纹�?，其�?4 张是 **调色板索引纹�?*（base_format=0x95xxxxxx=P8），
    //   �?[TEXCH] 打印�?avg �?*索引字节**的均值（P8 �?bytes=W*H，n=bytes/4，取每第 4 个索引字节）�?
    //   所�?avg�?0 **不是颜色**、只说明索引值集中在 10 附近。到底画面是黑还是亮，取决于**调色�?*�?
    //   这里同时打印：base_format / texture_type / 调色板地址 / 调色板前 16 �?/ 索引 32 桶直方图�?
    //   用来一次判定三种可能：
    //     (1) 索引分布很窄 + 调色板正�? �?游戏给的就是近乎纯色（那黑屏不是纹理的错�?
    //     (2) 索引分布很宽 + 调色板全 0  �?调色板没被填�?*缺失的创建工�?*就在填调色板这一环）
    //     (3) 索引分布很窄 + 调色板全 0  �?纹理数据整体是垃圾（上游解码/读取失败�?
    { static int _pi=0;
      if(isIndexed && _pi<12){ _pi++;
        SceUInt32 _nent=isP8?256u:16u;
        SceUInt32 _pal=rt.info.palette_byte_addr;
        SceUInt8* _pp=(_pal&&p5d::mem::contains(_pal,_nent*4u))?p5d::mem::translate(_pal):nullptr;
        fprintf(stderr,"[TEXPAL] va=%08X base=0x%08X ttype=%u %ux%u P%d pal=0x%08X palok=%d:",rt.info.data_byte_addr,
          rt.info.base_format,rt.info.texture_type,W,H,isP8?8:4,_pal,_pp?1:0);
        if(_pp){ for(SceUInt32 _k=0;_k<16&&_k<_nent;_k++){ SceUInt32 _w=(SceUInt32)_pp[_k*4]|((SceUInt32)_pp[_k*4+1]<<8)|((SceUInt32)_pp[_k*4+2]<<16)|((SceUInt32)_pp[_k*4+3]<<24); fprintf(stderr," %08X",_w); } }
        else fprintf(stderr," <no palette buffer>");
        { SceUInt32 _h[32]={0}; SceUInt32 _lim=isP8?(W*H):((W*H+1)/2);
          for(SceUInt32 _k=0;_k<bytes&&_k<_lim;_k++){ SceUInt32 _e; if(isP8) _e=px[_k]; else _e=(_k<(_lim))?((_k&1)?(px[_k>>1]>>4):(px[_k>>1]&0xF)):0; _h[_e>>3]++; }
          fprintf(stderr," hist:");
          for(int _k=0;_k<32;_k++) if(_h[_k]) fprintf(stderr," %d:%u",_k,_h[_k]);
          fprintf(stderr,"\n"); }
        fflush(stderr); } }
    BackendState::UnitTexture& ut=g_backend.unit_textures[unit];
    if(ut.created) destroy_unit_texture(ut);
    p5d::tex::VkFmt outFmt=rt.vk_format;
    std::vector<SceUInt8> lin;
    if(isIndexed){
        SceUInt32 nent=isP8?256u:16u;
        SceUInt32 palAddr=rt.info.palette_byte_addr;
        if(!palAddr || !p5d::mem::contains(palAddr,nent*4u)){LOGE("resolved: P8/P4 palette missing unit=%u pal=0x%08X",unit,palAddr);return -5;}
        const SceUInt8* pal=p5d::mem::translate(palAddr);
        if(!pal){LOGE("resolved: palette translate fail pal=0x%08X",palAddr);return -5;}
        // 索引缓冲：P8 1B/像素（SWIZZLED �?Morton 反排）；P4 每字�?像素（先按线性读�?
        std::vector<SceUInt8> idx;
        if(isP8){
            if(rt.info.texture_type==3u||rt.info.texture_type==6u) idx.assign(px,px+(size_t)W*H);
            else idx=p5d::tex::linearize_psv_texture(px,W,H,VK_FORMAT_R8_UNORM,rt.info.texture_type);
        } else { idx.assign(px,px+((size_t)W*H+1)/2); }
        lin.assign((size_t)W*H*4,0);
        for(SceUInt32 y=0;y<H;y++)for(SceUInt32 x=0;x<W;x++){
            size_t pi=(size_t)y*W+x; SceUInt32 e;
            if(isP8){ e=idx[pi]&0xFF; } else { SceUInt8 b=idx[pi>>1]; e=(pi&1)?(b>>4):(b&0xF); }
            // ★★�?h145k313【修 P8 调色板的 R/B 通道顺序】★★★
            //   证据链（h145k313）：
            //   �?用户提供**原版 PSV 截图**作为靶子：ATLUS = 蓝字 + �?T；CRIWARE = 蓝色六边形�?
            //   �?本移植渲染出来的对应画面�?*金色**：实测主�?#FFE00E / #FFDF00（ATLUS）�?
            //      #D5A00D（CRIWARE）；�?#FFE00E �?R↔B 交换正好�?#0EE0FF（亮蓝）�?
            //      #4B00F4（紫）交换后�?#F4004B（绯红，即那个红色的 T）⇒ 逐色吻合�?
            //   �?`[TEXMAT] bf=` 新探针查�?*这张 logo 纹理�?base_format �?0x95000000（P8 调色板）**�?
            //      而不�?0x0C000000 —�?这解释了 h145k297 �?`0x0C000000` �?VkFormat 为何**毫无效果**�?
            //   �?于是范围缩到本函数：原代码按"内存 BGRA(A8B8G8R8 小端)"�?pe[0]↔pe[2] 换一次再写成 RGBA�?
            //      但按 �?的实测颜色，最终结果恰恰是**多换了一�?* �?调色板实际就�?RGBA 字节序�?
            //   修法：不再交换，直接照抄。默认启用；`P5D_P8_BGRA=1` 可切回旧行为�?A/B 对照�?
            const SceUInt8* pe=pal+(size_t)e*4;
            SceUInt8* o=&lin[pi*4];
            static int _p8old=-1; if(_p8old<0) _p8old=std::getenv("P5D_P8_BGRA")?1:0;
            if(_p8old){ o[0]=pe[2]; o[1]=pe[1]; o[2]=pe[0]; o[3]=pe[3]; }   // 旧：假设 BGRA
            else      { o[0]=pe[0]; o[1]=pe[1]; o[2]=pe[2]; o[3]=pe[3]; }   // 新：RGBA 直抄
        }
        outFmt=VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        // PSV 显存布局(SWIZZLED/TILED)反排�?Vulkan 线性；LINEAR 内部原样 copy
        lin=p5d::tex::linearize_psv_texture(px,W,H,rt.vk_format,rt.info.texture_type);
    }
    SceInt32 rc=upload_texture_to_unit(ut,outFmt,W,H,layers,rt.info.is_cube,
                                       lin.data(),(SceUInt32)lin.size(),rt.swizzle_map.r,rt.swizzle_map.g,rt.swizzle_map.b,rt.swizzle_map.a);
    // ★ ARCH_744：真正执行上传的次数（与入口计数对照 ⇒ 看是否在中途早退）
    { static long _ta=0; ++_ta;
      if(_ta<=12) { printf("[TEXAT] #%ld unit=%u W=%u H=%u lin=%u fc=%llu rc=%d\n",
                           _ta,unit,W,H,(unsigned)lin.size(),(unsigned long long)g_backend.frame_count,(int)rc); fflush(stdout); } }
    LOGI("resolved unit=%u type=%u indexed=%d linearized %u->%u bytes",unit,rt.info.texture_type,(int)isIndexed,bytes,(SceUInt32)lin.size());
    // h144l 诊断：dump unit0 线性化后纹素，判定纹理内容是否均匀（直通采样灰白的根因）�?
    if(unit==0){
        static int _tdn=0;
        if(_tdn<4){ _tdn++;
            printf("[TEXDIAG #%d] unit=0 W=%u H=%u outFmt=%d layers=%u cube=%d lin=%u bytes base=0x%08X type=%u indexed=%d\n",
                _tdn,W,H,(int)outFmt,layers,(int)rt.info.is_cube,(SceUInt32)lin.size(),rt.info.data_byte_addr,rt.info.texture_type,(int)isIndexed);
            if(W>0 && H>0 && lin.size()>=(size_t)W*H*4u){
                struct PP{SceUInt32 fx,fy;}; PP pts[6]={{0,0},{W-1,0},{0,H-1},{W-1,H-1},{W/2,H/2},{W/4,H/4}};
                for(int _p=0;_p<6;_p++){
                    size_t off=((size_t)pts[_p].fy*W+pts[_p].fx)*4u;
                    if(off+4<=lin.size()){
                        SceUInt8 b0=lin[off],b1=lin[off+1],b2=lin[off+2],b3=lin[off+3];
                        printf("   texel(%u,%u) bytes=%02X %02X %02X %02X  asRGBA=(%u,%u,%u,%u)\n",
                            pts[_p].fx,pts[_p].fy,b0,b1,b2,b3,b0,b1,b2,b3);
                    }
                }
                // 统计�?4096 像素的不同字节值数量，粗判是否均匀
                SceUInt32 _nz=0,_distinct[4]={0,0,0,0}; SceUInt32 _last[4]={0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};
                SceUInt32 _cnt=(W*H<4096u)?W*H:4096u;
                for(SceUInt32 _i=0;_i<_cnt;_i++){ for(int _c=0;_c<4;_c++){ SceUInt32 _v=lin[_i*4u+_c]; if(_v!=_last[_c]){_distinct[_c]++;_last[_c]=_v;} } }
                printf("   �?u像素各通道变化次数(distinct runs) R=%u G=%u B=%u A=%u  (若均=1 则该通道恒定)\n",_cnt,_distinct[0],_distinct[1],_distinct[2],_distinct[3]);
            } else {
                printf("   [TEXDIAG] lin �?4bpp 或尺寸异常，dump �?64 字节 hex: ");
                for(size_t _b=0;_b<64 && _b<lin.size();_b++) printf("%02X ",lin[_b]);
                printf("\n");
            }
            fflush(stdout);
        }
    }
    if(rc==0){ut.is_rt_alias=false; ut.alias_view=VK_NULL_HANDLE; ut.last_vaddr=rt.info.data_byte_addr; std::memcpy(ut.last_ctrl,control16,16);}
    return rc;
}

// draw 前批�?materialize：脏检测（vaddr+16B 控制块快照不变即缓存命中�?
MaterializeStats materialize_fragment_textures(const void* const controls[16]) {
    MaterializeStats st;
    // ★★★ ARCH_746【绘制路径何时停止？（materialize 的调用时间线）】★★★
    //   动机（ARCH_745）：菜单帧（fc≥4300）的 `P5D_TEXCENSUS` **没有任何输出**，
    //   而 ARCH_489 明确其触发条件是"target 之后的**第一次调用**，必然命中"。
    //   ⇒ 若 0 行输出，说明**帧 4300 之后该函数根本没被调用** ⇒ **绘制路径停了**。
    //   本仪器记录：调用计数、首次/末次帧号，并每 200 次打印一次（不截断关键信息）。
    //   标签 [MAT] 全局唯一。
    { static long long _mc=0; static long long _mfirst=-1,_mlast=-1,_mprev=-1;
      long long _mfc=(long long)g_backend.frame_count;
      if(_mfirst<0){ _mfirst=_mfc; printf("[MAT] 首次调用 fc=%lld\n",_mfc); fflush(stdout); }
      _mprev=_mlast; _mlast=_mfc; ++_mc;
      if((_mc%200)==0 || _mfc!=_mprev){
        static long long _rep=0;
        if((_mc%200)==0 || _rep<40){ ++_rep;
          printf("[MAT] n=%lld fc=%lld（本次）prev_fc=%lld\n",_mc,_mfc,_mprev); fflush(stdout); } } }
    bool _censusOn=false;
    // ★ ARCH_480【每帧纹理工作集普查】`P5D_TEXCENSUS=<fc0>[,<fc1>]`，窗口 [fc, fc+2]
    //   动机（实测教训）：`upload_gxm_control_resolved` 只在"该 unit 的绑定发生变化"时才被调用，
    //   而菜单画面上大多数 unit 命中下面那条 **RT-alias 复用分支并 `continue`**
    //   ⇒ 在那里插桩每帧只能看到极少数纹理（实测 fc=4300 只有 1 张）。
    //   所以"这一帧到底用了哪些贴图"这个问题，只能在**函数入口**回答。
    //   `alias` 列来自 `unit_textures[u].is_rt_alias`，可区分"线性内存纹理"与"渲染目标别名"。
    { static long long _cf[2]={-2,-2}; static int _cfseen[2]={0,0};
      static long long _censusUntil=-1;
      if(_cf[0]==-2){ for(int _i=0;_i<2;++_i) _cf[_i]=-1;
        const char* _e=std::getenv("P5D_TEXCENSUS");
        if(_e&&*_e){ char _b[128]; std::snprintf(_b,sizeof(_b),"%s",_e); char* _s=_b; int _i=0;
          while(_s&&*_s&&_i<2){ char* _cc=std::strchr(_s,','); if(_cc)*_cc=0;
            _cf[_i++]=std::atoll(_s); _s=_cc?(_cc+1):nullptr; } }
        printf("[TEXCENSUS-CFG] env=%s cf=[%lld,%lld] fc_now=%llu\n",_e?_e:"(null)",_cf[0],_cf[1],
               (unsigned long long)g_backend.frame_count); fflush(stdout); }
      long long _fc=(long long)g_backend.frame_count;
      if(_censusUntil>=0 && _fc>_censusUntil) _censusUntil=-1;
      for(int _i=0;_i<2;++_i)
        // ★ ARCH_489 修正：触发条件不能是"帧号落在 [target, target+2] 窗口内"。
        //   开机头几帧 `materialize_fragment_textures` 可能根本没被调用 ⇒ 窗口整个错过
        //   ⇒ 实测 `P5D_TEXCENSUS=1` 得到 **0 行输出**。（与 ARCH_475 那次 `[TEXFRAME]`
        //   精确相等失败是同一类错误。）改成"**target 之后的第一次调用**"，必然命中。
        if(_cf[_i]>=0 && !_cfseen[_i] && _fc>=_cf[_i]){
          _cfseen[_i]=1;
          long long _end=_cf[_i]+2;
          { static long long _span=-2;
            if(_span==-2){ _span=3; const char* _s=std::getenv("P5D_TEXCENSUS_SPAN");
              if(_s&&*_s) _span=std::atoll(_s);
              if(std::getenv("P5D_TEXCENSUS_ALL")) _span=200000;
              if(_span<1) _span=1; if(_span>200000) _span=200000; }
            if(_span>3) _end=_fc+_span-1; }
          _censusUntil=_end;
          printf("[TEXCENSUS-BEGIN] target=%lld actual=%lld until=%lld\n",_cf[_i],_fc,_end);
          fflush(stdout); }
      // ★ ARCH_484：窗口长度改为可配置 `P5D_TEXCENSUS_SPAN=<n>`（默认 3 帧）。
      //   动机：实测 fc=3900 的 3 帧窗口只收到约 40 张纹理，而"主界面那句细体长段落"
      //   所在的文字图集并不在其中。把窗口拉到几百帧、配合按地址去重，一次运行就能把
      //   **整套文字图集**都收下来，不必反复猜帧号、也不必多次运行。
      { static long long _span2=-2; (void)_span2; }
      _censusOn = (_censusUntil>=0 && _fc<=_censusUntil);
    }
    for(SceUInt32 u=0;u<16;u++){
        const void* c=controls?controls[u]:nullptr;
        if(!c) continue;
        p5d::tex::SceGxmTextureInfo inf;
        if(!p5d::tex::parse_sce_gxm_texture((const SceUInt8*)c,inf)){st.failed++;continue;}
        if(_censusOn){
          const SceUInt32 _va=inf.data_byte_addr;
          const bool _al=(bool)g_backend.unit_textures[u].is_rt_alias;
          static SceUInt32 _csv[2048]={0}; static int _csn=0; int _seen2=0;
          for(int _j=0;_j<_csn;++_j) if(_csv[_j]==_va){ _seen2=1; break; }
          if(!_seen2)
            printf("[TEXCENSUS] NEW unit=%u va=%08X %ux%u ttype=%u base=0x%08X alias=%d\n",
                   u,_va,inf.width,inf.height,inf.texture_type,inf.base_format,(int)_al);
          if(!_al && !_seen2 && _csn<2048 && inf.width && inf.height){
            p5d::tex::ResolvedGxmTexture _rt2;
            if(p5d::tex::resolve_gxm_texture((const SceUInt8*)c,_rt2)){
              const bool _p8=(inf.base_format==0x95000000u), _p4=(inf.base_format==0x94000000u);
              const bool _idx=_p8||_p4;
              SceUInt32 _by=_idx?(_p8?inf.width*inf.height:(inf.width*inf.height+1u)/2u)
                                :p5d::tex::texture_byte_size(_rt2.vk_format,inf.width,inf.height,1);
              SceUInt8* _q=p5d::mem::translate(_va);
              if(_q && _by && p5d::mem::contains(_va,_by)){
                _csv[_csn++]=_va;
                char _pp[256];
                std::snprintf(_pp,sizeof(_pp),"C:\\dev\\p5d_build\\texframe\\cen_%08X_%ux%u_bf%08X.raw",
                              _va,inf.width,inf.height,inf.base_format);
                if(std::FILE* _f=std::fopen(_pp,"wb")){ std::fwrite(_q,1,_by,_f); std::fclose(_f);
                  printf("[TEXCEN] %s bytes=%u base=0x%08X\n",_pp,_by,inf.base_format); }
                if(_idx){
                  SceUInt32 _nent=_p8?256u:16u;
                  SceUInt8* _pal=(inf.palette_byte_addr&&p5d::mem::contains(inf.palette_byte_addr,_nent*4u))
                                 ? p5d::mem::translate(inf.palette_byte_addr):nullptr;
                  if(_pal){ char _qq[256];
                    std::snprintf(_qq,sizeof(_qq),"C:\\dev\\p5d_build\\texframe\\cenpal_%08X_%s_%u.raw",
                                  _va,_p8?"P8":"P4",_nent);
                    if(std::FILE* _g=std::fopen(_qq,"wb")){ std::fwrite(_pal,1,_nent*4u,_g); std::fclose(_g);
                      printf("[TEXCEN-PAL] %s\n",_qq); } } }
              }
            }
          }
          fflush(stdout); }
        {   // RT 别名：内容在 GPU image 上，不需要内容指纹；地址+控制块不变就直接复用�?
            BackendState::UnitTexture& _u1=g_backend.unit_textures[u];
            const SceUInt32* _w1=(const SceUInt32*)c;
            if(_u1.created && _u1.is_rt_alias && _u1.last_vaddr==inf.data_byte_addr
               && std::memcmp(_u1.last_ctrl,_w1,16)==0){ st.cached++; continue; }
        }
        // ★★�?h145k133【缓存缺陷修复】原脏检测只比较 vaddr �?16B 控制块，
        //   **完全不看内容**。对于「地址固定、内容由 CPU/解码器逐帧改写」的纹理
        //   （影片帧缓冲就是一例），首次上传（往往是全黑）之后永远不会再上传，
        //   于是画面上永远是黑的——�?guest 内存里其实已经是真实帧�?
        SceInt32 rc=upload_gxm_control_resolved(u,c);
        if(rc==0)st.uploaded++; else if(rc==1)st.cached++;
        else if(rc==-3||rc==-4)st.missing_addr++; else st.failed++;
    }
    return st;
}

// 程序�?6 �?cube �?unit（真�?cube 资源解析到位前的占位，验�?cube 绑定/采样链路�?
SceInt32 create_cube_unit_procedural(SceUInt32 unit, SceUInt32 S) {
    if(unit>=16) return -1;
    const SceUInt32 FACES=6,CH=4;
    const SceUInt8 base[6][3]={{220,70,70},{70,200,200},{70,200,70},{200,70,200},{80,115,230},{230,210,70}};
    std::vector<SceUInt8> px((size_t)S*S*CH*FACES);
    for(SceUInt32 f=0;f<FACES;f++)for(SceUInt32 y=0;y<S;y++)for(SceUInt32 x=0;x<S;x++){
        bool ck=((x/8)+(y/8))%2==0; SceUInt32 idx=(f*S*S+y*S+x)*CH;
        px[idx+0]=ck?base[f][0]:(SceUInt8)(base[f][0]/2);
        px[idx+1]=ck?base[f][1]:(SceUInt8)(base[f][1]/2);
        px[idx+2]=ck?base[f][2]:(SceUInt8)(base[f][2]/2); px[idx+3]=255;}
    return upload_texture_to_unit(g_backend.unit_textures[unit],VK_FORMAT_R8G8B8A8_UNORM,S,S,FACES,true,px.data(),(SceUInt32)px.size());
}


SceInt32 create_descriptor_pool_and_set() {
    LOGI("Vulkan: Creating descriptor pool and set...");
    if (!g_backend.descriptor_set_layout) {
        LOGE("Descriptor set layout not created");
        return -1;
    }

    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 4096*2;   // h144k20: binding0+binding1 两个 UBO/set（fresh set per bind, recycled per frame�?
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 4096*16;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    pool_info.maxSets = 4096;  // h144bp

    VkResult result = vkCreateDescriptorPool(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &pool_info, nullptr,
        reinterpret_cast<VkDescriptorPool*>(&g_backend.descriptor_pool));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateDescriptorPool failed: %s", vk_result_string(result));
        return -1;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = *reinterpret_cast<VkDescriptorPool*>(&g_backend.descriptor_pool);
    alloc_info.descriptorSetCount = 1;
    VkDescriptorSetLayout layouts[] = { *reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout) };
    alloc_info.pSetLayouts = layouts;

    result = vkAllocateDescriptorSets(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &alloc_info,
        reinterpret_cast<VkDescriptorSet*>(&g_backend.descriptor_set));
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateDescriptorSets failed: %s", vk_result_string(result));
        return -1;
    }

    // 更新描述符集，绑定uniform buffer
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = *reinterpret_cast<VkBuffer*>(&g_backend.uniform_buffer);
    buffer_info.offset = 0;
    buffer_info.range = VK_WHOLE_SIZE;  // 【h137d】覆�?shader 大数组访�?

    fprintf(stderr, "[DEBUG] desc_set update: buffer=%p offset=%zu range=%zu\n",
            (void*)buffer_info.buffer, (size_t)buffer_info.offset, (size_t)buffer_info.range);
    fprintf(stderr, "[DEBUG] uniform_buffer=%p uniform_buffer_memory=%p mapped=%p\n",
            (void*)g_backend.uniform_buffer, (void*)g_backend.uniform_buffer_memory,
            g_backend.uniform_buffer_mapped);
    if (g_backend.uniform_buffer_mapped) {
        fprintf(stderr, "[DEBUG] mapped[0]=%f\n", ((float*)g_backend.uniform_buffer_mapped)[0]);
    }

    // h144k20 binding0=顶点 UBO；binding1=片元 UBO；binding(2+u)=texture unit u（已加载用真实纹理，否则 dummy�?
    VkDescriptorBufferInfo buffer_info2{};
    buffer_info2.buffer = *reinterpret_cast<VkBuffer*>(&g_backend.uniform_buffer_frag);
    buffer_info2.offset = 0;
    buffer_info2.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet descriptor_writes[18]{};
    descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[0].dstSet = *reinterpret_cast<VkDescriptorSet*>(&g_backend.descriptor_set);
    descriptor_writes[0].dstBinding = 0;
    descriptor_writes[0].dstArrayElement = 0;
    descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_writes[0].descriptorCount = 1;
    descriptor_writes[0].pBufferInfo = &buffer_info;
    descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[1].dstSet = *reinterpret_cast<VkDescriptorSet*>(&g_backend.descriptor_set);
    descriptor_writes[1].dstBinding = 1;
    descriptor_writes[1].dstArrayElement = 0;
    descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_writes[1].descriptorCount = 1;
    descriptor_writes[1].pBufferInfo = &buffer_info2;

    VkDescriptorImageInfo unit_infos[16]{};
    for (SceUInt32 u = 0; u < 16; u++) {
        BackendState::UnitTexture& ut = g_backend.unit_textures[u];
        if (ut.created) { unit_infos[u].sampler = ut.sampler; unit_infos[u].imageView = ut.view; }
        else { unit_infos[u].sampler = g_backend.dummy_sampler; unit_infos[u].imageView = g_backend.dummy_view; }
        unit_infos[u].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        descriptor_writes[2+u].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptor_writes[2+u].dstSet = *reinterpret_cast<VkDescriptorSet*>(&g_backend.descriptor_set);
        descriptor_writes[2+u].dstBinding = 2+u;
        descriptor_writes[2+u].dstArrayElement = 0;
        descriptor_writes[2+u].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptor_writes[2+u].descriptorCount = 1;
        descriptor_writes[2+u].pImageInfo = &unit_infos[u];
    }
    vkUpdateDescriptorSets(*reinterpret_cast<VkDevice*>(&g_backend.device),
        18, descriptor_writes, 0, nullptr);
    fprintf(stderr, "[DEBUG] vkUpdateDescriptorSets called, writes=18 (UBO-vert + UBO-frag + 16 units)\n");

    LOGI("Descriptor pool and set created");
    return 0;
}

void update_uniform_buffer(const float* local_to_world, const float* view_proj) {
    if (g_backend.uniform_buffer_mapped) {
        memcpy(g_backend.uniform_buffer_mapped, local_to_world, 64);
        memcpy((float*)g_backend.uniform_buffer_mapped + 16, view_proj, 64);
        // 强制flush确保GPU可见
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = *reinterpret_cast<VkDeviceMemory*>(&g_backend.uniform_buffer_memory);
        range.offset = 0;
        range.size = 128;
        vkFlushMappedMemoryRanges(*reinterpret_cast<VkDevice*>(&g_backend.device), 1, &range);
    }
}

// h143z：guest GXM UBO -> Vulkan binding0�?192B，vec4[512]，std140 每元�?6B�?
void update_uniform_buffer_raw(const void* data, SceUInt32 bytes) {
    if (!g_backend.uniform_buffer_mapped) return;
    const SceUInt32 cap = 8192u;
    SceUInt32 n = bytes; if (n > cap) n = cap;
    if (data && n) memcpy(g_backend.uniform_buffer_mapped, data, n);
    if (n < cap) memset((char*)g_backend.uniform_buffer_mapped + n, 0, cap - n);
    // ★★�?h145k622【强正对照：�?binding0 整块填成重复的正交系数模式】P5D_FORCE_UBO_PAT=1 ★★�?
    //   动机：h145k621 �?`P5D_FORCE_UBO=0,0,0,0` 清零 binding0，画面（save 徽标期）**毫无变化**�?
    //   但那段内容未必走 binding0，所以那不算"binding0 不被�?的证据�?
    //   本开关把整个顶点 UBO 填成**�?4 �?float 重复一�?*�?`(2/960, -1, 2/544, -1)`�?
    //     无论 vert_0015 �?`SA[0].y / SA[2].x / SA[2].y / SA[4].x` 落在哪个偏移�?
    //     都会读到正确的正交系�?�?只要 shader 确实�?binding0，LOGO 相位就该出图�?
    //   若仍全黑 �?**坐实 binding0 没有到达顶点着色器**，问题在绑定/管线一侧�?
    { static int _fp2=-1; if(_fp2<0) _fp2=std::getenv("P5D_FORCE_UBO_PAT")?1:0;
      if(_fp2){ float* _p=(float*)g_backend.uniform_buffer_mapped;
        const float sx=2.0f/960.0f, sy=2.0f/544.0f;
        for(SceUInt32 _i=0;_i+3<cap/4u;_i+=4u){ _p[_i]=sx; _p[_i+1]=-1.0f; _p[_i+2]=sy; _p[_i+3]=-1.0f; }
        static int _pn=0; if(_pn<4){ _pn++;
          fprintf(stderr,"[UBO-PAT] #%d 顶点 UBO 已填 (2/960,-1,2/544,-1) 重复; �?: %.6f %.3f %.6f %.3f %.6f %.3f %.6f %.3f\n",
            _pn,_p[0],_p[1],_p[2],_p[3],_p[4],_p[5],_p[6],_p[7]); fflush(stderr); } } }
    // h144k18【UBO 归因开关】P5D_FORCE_UBO=r,g,b,a：把 binding0 整块填成该颜色�?
    //   若整屏随之变�?�?片元读的确实�?binding0；若不变 �?读的是别的绑�?别的缓冲�?
    { static const char* _fu=getenv("P5D_FORCE_UBO");
      if(_fu&&*_fu){ static int _pi=0; float r=0,g=0,b=0,a=1;
        if(_pi<0) _pi=0;
        if(!_pi){ _pi=1; if(std::sscanf(_fu,"%f,%f,%f,%f",&r,&g,&b,&a)!=4){ r=1;g=0;b=1;a=1; } }
        float* _p=(float*)g_backend.uniform_buffer_mapped;
        for(SceUInt32 _i=0;_i+3<cap/4u;_i+=4u){ _p[_i]=r; _p[_i+1]=g; _p[_i+2]=b; _p[_i+3]=a; } } }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = *reinterpret_cast<VkDeviceMemory*>(&g_backend.uniform_buffer_memory);
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(*reinterpret_cast<VkDevice*>(&g_backend.device), 1, &range);
}

// h144k20：片元默�?UBO -> Vulkan binding1（与顶点完全独立，GXM 语义如此�?
void update_fragment_uniform_buffer_raw(const void* data, SceUInt32 bytes) {
    if (!g_backend.uniform_buffer_frag_mapped) return;
    const SceUInt32 cap = 8192u;
    SceUInt32 n = bytes; if (n > cap) n = cap;
    if (data && n) memcpy(g_backend.uniform_buffer_frag_mapped, data, n);
    if (n < cap) memset((char*)g_backend.uniform_buffer_frag_mapped + n, 0, cap - n);
    { static const char* _fu=getenv("P5D_FORCE_UBO_FRAG");
      if(_fu&&*_fu){ static int _pi=0; float r=1,g=0,b=1,a=1;
        if(!_pi){ _pi=1; if(std::sscanf(_fu,"%f,%f,%f,%f",&r,&g,&b,&a)!=4){ r=1;g=0;b=1;a=1; } }
        float* _p=(float*)g_backend.uniform_buffer_frag_mapped;
        for(SceUInt32 _i=0;_i+3<cap/4u;_i+=4u){ _p[_i]=r; _p[_i+1]=g; _p[_i+2]=b; _p[_i+3]=a; } } }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = *reinterpret_cast<VkDeviceMemory*>(&g_backend.uniform_buffer_frag_memory);
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(*reinterpret_cast<VkDevice*>(&g_backend.device), 1, &range);
}

void cmd_bind_descriptor_set(VkCommandBuffer cmd) {
    VkDevice _dv = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkDescriptorSet _ds = VK_NULL_HANDLE;
    VkDescriptorSetLayout _lay = *reinterpret_cast<VkDescriptorSetLayout*>(&g_backend.descriptor_set_layout);
    VkDescriptorPool _pool = *reinterpret_cast<VkDescriptorPool*>(&g_backend.descriptor_pool);
    VkDescriptorSetAllocateInfo _ai{}; _ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    _ai.descriptorPool=_pool; _ai.descriptorSetCount=1; _ai.pSetLayouts=&_lay;
    VkResult _ar = vkAllocateDescriptorSets(_dv,&_ai,&_ds);
    // h144k33：描述符�?*从不回收**，每�?draw 都新分配一�?set。池一旦耗尽 �?后续全部回退到持�?set�?
    //   而该 set 若与当前 18-binding 布局不匹配，绘制就是未定义行为（常见表现=整条 draw 零像素）�?
    { static SceUInt64 _dfail=0,_dok=0; if(_ar!=VK_SUCCESS) ++_dfail; else ++_dok;
      if(_ar!=VK_SUCCESS && (_dfail<=4 || (_dfail%5000)==0)) fprintf(stderr,"[DESCPOOL] alloc FAIL n=%llu ok=%llu\n",(unsigned long long)_dfail,(unsigned long long)_dok); }
    if(_ar!=VK_SUCCESS || !_ds){ static int _af=0; if(_af<8){_af++; fprintf(stderr,"[DESCSET] alloc fail %d, fallback persistent\n",(int)_ar);} _ds=*reinterpret_cast<VkDescriptorSet*>(&g_backend.descriptor_set); }
    VkDescriptorBufferInfo _bi{};
    _bi.buffer=*reinterpret_cast<VkBuffer*>(&g_backend.uniform_buffer); _bi.offset=0; _bi.range=VK_WHOLE_SIZE;
    VkDescriptorBufferInfo _bi2{};   // h144k20 binding1 = 片元默认 UBO
    _bi2.buffer=*reinterpret_cast<VkBuffer*>(&g_backend.uniform_buffer_frag); _bi2.offset=0; _bi2.range=VK_WHOLE_SIZE;
    // ★★�?h145k148【决定性取�?+ 让强制真正生效】★★★
    //   已证：所�?RT 回读全是纯黑；内容管�?frag_0084 = 1 采样 + 6 OpLoad；最后有效写入�?
    //   frag_0090 = 1 采样 + 4 OpLoad �?输出很可能是「采样�?× 片元常量」。而强制白 dummy �?*仍黑**
    //   �?只能是那个常量乘数是 0�?
    //   �?`P5D_FORCE_UBO_FRAG` 此前只在**引擎调用 setter �?*填缓冲；若那�?draw 没有重新 set�?
    //   覆盖就永远不会落�?GPU 看到的缓冲上 �?之前�?无效"结论是假阴性�?
    //   这里改成**每次 bind 描述符集�?*都填，并打印宿主侧缓冲前 8 �?float 作为铁证�?
    { float* _fp=(float*)g_backend.uniform_buffer_frag_mapped;
      static const char* _fu=getenv("P5D_FORCE_UBO_FRAG");
      if(_fp && _fu && *_fu){
        static int _pi=0; static float r=1,g=0,b=1,a=1;
        if(!_pi){ _pi=1; if(std::sscanf(_fu,"%f,%f,%f,%f",&r,&g,&b,&a)!=4){ r=1;g=0;b=1;a=1; } }
        for(SceUInt32 _i=0;_i+3<8192u/4u;_i+=4u){ _fp[_i]=r; _fp[_i+1]=g; _fp[_i+2]=b; _fp[_i+3]=a; }
        VkMappedMemoryRange _r{}; _r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        _r.memory=*reinterpret_cast<VkDeviceMemory*>(&g_backend.uniform_buffer_frag_memory);
        _r.offset=0; _r.size=VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(_dv,1,&_r); }
      static long _fh=0;
      if(_fp && _fh<24){ _fh++;
        fprintf(stderr,"[FUBOHOST] #%ld 宿主侧片元UBO�?float: %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f\n",
          _fh,_fp[0],_fp[1],_fp[2],_fp[3],_fp[4],_fp[5],_fp[6],_fp[7]); fflush(stderr); } }
    VkDescriptorImageInfo _ii[16]{}; VkWriteDescriptorSet _ws[18]{};
    _ws[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; _ws[0].dstSet=_ds; _ws[0].dstBinding=0;
    _ws[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; _ws[0].descriptorCount=1; _ws[0].pBufferInfo=&_bi;
    _ws[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; _ws[1].dstSet=_ds; _ws[1].dstBinding=1;
    _ws[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; _ws[1].descriptorCount=1; _ws[1].pBufferInfo=&_bi2;
    for(SceUInt32 u=0;u<16;u++){
        BackendState::UnitTexture& _ut=g_backend.unit_textures[u];
        // ★★�?h145k142【一击判定：采样到的到底是不是这张真实纹理】P5D_FORCE_DUMMY_TEX=1
        //   把所�?unit 都换�?1x1 白色 dummy�?
        //     · 画面变白 �?片元确实在采这些 unit �?那么"�?就是**真实纹理�?GPU 侧解码成�?*
        //       （guest 那张�?vkfmt=133=BC1_RGBA�?56x512/65536B；若实际不是 BC1 而是 PSV swizzled 4bpp�?
        //        线性化后按 BC1 解码�?0，�?BC1 �?0 = 不透明黑，完全对得上）
        //     · 画面仍黑 �?采到的根本不是这�?unit（描述符落点/绑定编号另有问题�?
        static int _fd=-1; if(_fd<0) _fd=std::getenv("P5D_FORCE_DUMMY_TEX")?1:0;
        if(_fd){ _ii[u].imageView=g_backend.dummy_view; _ii[u].sampler=g_backend.dummy_sampler; }
        else if(_ut.is_rt_alias && _ut.alias_view){ _ii[u].imageView=_ut.alias_view; _ii[u].sampler=g_backend.dummy_sampler; }
        else if(_ut.created && _ut.view){ _ii[u].imageView=_ut.view; _ii[u].sampler=_ut.sampler?_ut.sampler:g_backend.dummy_sampler; }
        else { _ii[u].imageView=g_backend.dummy_view; _ii[u].sampler=g_backend.dummy_sampler; }
        _ii[u].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        _ws[2+u].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; _ws[2+u].dstSet=_ds;
        _ws[2+u].dstBinding=2+u; _ws[2+u].dstArrayElement=0;
        _ws[2+u].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; _ws[2+u].descriptorCount=1; _ws[2+u].pImageInfo=&_ii[u]; }
    // h144k34【纹理绑定普查】统计本�?draw �?16 �?unit 里：真实纹理 / RT 别名 / dummy 各多少个�?
    //   配合 P5D_DUMMY_RGB 归因实验使用：若真实纹理数为 0，则片元着色器采到的全�?dummy�?
    { static long _tb=0; static int _tb_printed=0;
      int _nreal=0,_nalias=0,_ndummy=0;
      for(SceUInt32 u=0;u<16;u++){ BackendState::UnitTexture& _ut=g_backend.unit_textures[u];
        if(_ut.is_rt_alias && _ut.alias_view) _nalias++; else if(_ut.created && _ut.view) _nreal++; else _ndummy++; }
      // 只在"真实/别名组合发生变化"或前若干次时打印，避免刷�?
      static int _last_r=-1,_last_a=-1;
      bool _chg = (_nreal!=_last_r || _nalias!=_last_a);
      if(_chg || _tb_printed<6){ _last_r=_nreal; _last_a=_nalias; _tb_printed++;
        fprintf(stderr,"[TEXBIND] #%ld real=%d alias=%d dummy=%d",_tb,_nreal,_nalias,_ndummy);
        for(SceUInt32 u=0;u<16;u++){ BackendState::UnitTexture& _ut=g_backend.unit_textures[u];
          if((_ut.is_rt_alias&&_ut.alias_view)||(_ut.created&&_ut.view))
            fprintf(stderr," u%u=%s%ux%u",u,_ut.is_rt_alias?"alias":"tex",(unsigned)_ut.width,(unsigned)_ut.height); }
        fprintf(stderr,"\n"); fflush(stderr); }
      _tb++; }
    vkUpdateDescriptorSets(_dv,18,_ws,0,nullptr);
    *reinterpret_cast<VkDescriptorSet*>(&g_backend.descriptor_set)=_ds;
    if (_ds && g_backend.pipeline_layout) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            *reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout),
            0, 1, &_ds, 0, nullptr);
    } else {
        fprintf(stderr, "[DEBUG] cmd_bind_descriptor_set: SKIPPED! ds=%p layout=%p\n",(void*)_ds,(void*)g_backend.pipeline_layout);
    }
}

static SceInt32 create_screenshot_image() {
    // h144k0d：失败自�?+ 永久禁用。原实现的两处缺陷会让一次失败拖垮整轮运行：
    //   �?分配内存失败�?*�?return，不销毁已创建�?image**，而下一行入口判据是
    //      `if (g_backend.screenshot_image) return 0;` �?之后每次 save_screenshot 都以�?
    //      「已就绪」，实际上对着一�?*未绑定内存的 image** �?vkCmdCopyImageToBuffer�?
    //   �?没有任何失败记忆，反复重试�?
    //   实测后果：err.log 出现 `vkAllocateMemory (screenshot) failed: VK_ERROR_OUT_OF_DEVICE_MEMORY`�?
    //   随后进程在约百帧处提前结束（RTDUMP 永远不会到达 frame 1400）�?
    static int s_shot_disabled = 0;
    if (s_shot_disabled) return -1;
    if (g_backend.screenshot_ready) return 0;                 // �?ready 为准，而非 image 非空
    if (g_backend.screenshot_image) {                         // 半成品残留（上一轮失败）�?先清�?
        VkDevice _d0 = *reinterpret_cast<VkDevice*>(&g_backend.device);
        vkDestroyImage(_d0, *reinterpret_cast<VkImage*>(&g_backend.screenshot_image), nullptr);
        g_backend.screenshot_image = nullptr;
    }
    LOGI("Vulkan: Creating screenshot image...");
    
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = (VkFormat)g_backend.swapchain_format;
    image_info.extent = { g_backend.swapchain_width, g_backend.swapchain_height, 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    VkResult result = vkCreateImage(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &image_info, nullptr,
        reinterpret_cast<VkImage*>(&g_backend.screenshot_image));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateImage (screenshot) failed: %s (screenshot permanently disabled)", vk_result_string(result));
        g_backend.screenshot_image = nullptr;
        s_shot_disabled = 1;
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkImage*>(&g_backend.screenshot_image), &mem_req);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(
        mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    result = vkAllocateMemory(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &alloc_info, nullptr,
        reinterpret_cast<VkDeviceMemory*>(&g_backend.screenshot_memory));
    if (result != VK_SUCCESS) {
        LOGE("vkAllocateMemory (screenshot) failed: %s（截图功能将永久禁用，不再重试）", vk_result_string(result));
        vkDestroyImage(*reinterpret_cast<VkDevice*>(&g_backend.device),
            *reinterpret_cast<VkImage*>(&g_backend.screenshot_image), nullptr);
        g_backend.screenshot_image = nullptr;
        s_shot_disabled = 1;
        return -1;
    }

    vkBindImageMemory(*reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkImage*>(&g_backend.screenshot_image),
        *reinterpret_cast<VkDeviceMemory*>(&g_backend.screenshot_memory), 0);

    g_backend.screenshot_ready = true;
    LOGI("Screenshot image created (%ux%u)", g_backend.swapchain_width, g_backend.swapchain_height);
    return 0;
}

// ★★�?h145k383【内容触发抓帧】★★★
//   动机（本轮查实的**取证方法论缺�?*）：内容 pass �?draw 只在**个别帧爆�?*
//   （实测：�?2252�?265�?763�?778 �?frag_0084，其余帧一个都没有），
//   而本项目历来�?固定帧号 / 固定步长"采样（STEP=100，取�?2000/2100/�?3000�?
//   �?**必然完全错过爆发�?*，于是反复得�?全黑 / 该机制无�?的假结论�?
//   修法：由 draw 侧（eboot_probe.cpp �?draw 过滤处）在发现目标片元哈希时累加本计数，
//   present_frame �?只要本帧出现过该哈希�?draw 就抓一�?——与帧号无关，配置无关�?
unsigned g_frame_content_draws = 0;
unsigned long long g_cur_draw_fh = 0;   // h145k1110：当�?draw 的片元程序哈希（draw 侧每笔更新）
unsigned g_cur_prim = 0;                // h145k1135：当�?draw �?GXM primType（仅诊断用）
int g_movie_opened = 0;                 // h145k1110b：guest 已请求播放影片（[MOVIEOPEN] 置位�?
// ★★�?h145k1110c【影片打开世代号】★★★
//   动机（用户实测）：影片放完后**又从�?0 帧循�?*，而游戏其实这时已经进标题界面�?
//   要到标题界面待机一段时间后，游戏才会再放一次片头（attract 模式）�?
//   因此"重置影片时钟"必须只由 **guest 真正�?openMovie 一�?*触发，不能靠墙钟取模�?
//   每次 `[MOVIEOPEN]` 都让本世代号 +1；后端发现世代号变化才归零帧�?时钟/结束标志�?
unsigned long long g_movie_open_epoch = 0;
// h145k1110c：`g_texsrc_done` / `g_texsrc_reset` 定义在本文件上方（upload_gxm_control_resolved 之前）�?
// h145k1110b：给 thumb_interpreter.cpp 用的 C 接口（它�?include 本头文件，沿�?p5d_movie_request 的写法）
extern "C" void p5d_movie_opened_notify(void){
  g_movie_opened = 1; g_movie_open_epoch++; g_texsrc_done = 0; g_texsrc_reset = 1; }

static char g_force_capture[64]={0};
void request_capture(const char* name){
  if(name){ size_t _i=0; for(;_i<63&&name[_i];_i++)g_force_capture[_i]=name[_i]; g_force_capture[_i]=0; }
  else g_force_capture[0]=0; }
SceInt32 save_screenshot(const char* filename) {
    if (!g_backend.screenshot_ready) {
        if (create_screenshot_image() != 0) return -1;
    }

    VkDevice device = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkQueue queue = *reinterpret_cast<VkQueue*>(&g_backend.graphics_queue);
    // 等前�?render-pass/present 命令全部落地，避免拷贝到尚未 clear 的交换链图像（首帧全黑即此竞争）
    vkQueueWaitIdle(queue);
    VkCommandPool cmd_pool = *reinterpret_cast<VkCommandPool*>(&g_backend.command_pool);
    VkImage src_image = g_backend.swapchain_images[g_backend.current_image_index];
    VkImage dst_image = *reinterpret_cast<VkImage*>(&g_backend.screenshot_image);
    SceUInt32 w = g_backend.swapchain_width;
    SceUInt32 h = g_backend.swapchain_height;

    // 分配命令缓冲�?
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(device, &alloc_info, &cmd) != VK_SUCCESS) return -1;

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // 源图像barrier: PRESENT -> TRANSFER_SRC
    VkImageMemoryBarrier barrier1{};
    barrier1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier1.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier1.image = src_image;
    barrier1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier1.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier1);

    // 目标图像barrier: PREINITIALIZED -> TRANSFER_DST
    VkImageMemoryBarrier barrier2{};
    barrier2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier2.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier2.image = dst_image;
    barrier2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier2.srcAccessMask = 0;
    barrier2.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier2);

    // 复制图像（源已被手动清成黄色�?
    VkImageCopy copy_region{};
    copy_region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy_region.srcOffset = { 0, 0, 0 };
    copy_region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy_region.dstOffset = { 0, 0, 0 };
    copy_region.extent = { w, h, 1 };
    vkCmdCopyImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

    // 目标图像barrier: TRANSFER_DST -> GENERAL（便于读取）
    VkImageMemoryBarrier barrier3{};
    barrier3.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier3.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier3.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier3.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier3.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier3.image = dst_image;
    barrier3.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier3.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier3);

    vkEndCommandBuffer(cmd);

    // 提交并等�?
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    VkFence fence;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device, &fence_info, nullptr, &fence);
    { FILE* f=fopen("shot_internal.log","a"); if(f){fprintf(f,"before submit\n");fclose(f);} }
    VkResult ss_sub = vkQueueSubmit(queue, 1, &submit_info, fence);
    { FILE* f=fopen("shot_internal.log","a"); if(f){fprintf(f,"after submit=%d before wait\n",ss_sub);fclose(f);} }
    VkResult ss_wait = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL);
    { FILE* f=fopen("shot_internal.log","a"); if(f){fprintf(f,"after wait=%d\n",ss_wait);fclose(f);} }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);

    // 映射内存读取像素
    VkImageSubresource subres{};
    subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subres.mipLevel = 0;
    subres.arrayLayer = 0;
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, dst_image, &subres, &layout);

    void* data;
    vkMapMemory(device, *reinterpret_cast<VkDeviceMemory*>(&g_backend.screenshot_memory),
        0, VK_WHOLE_SIZE, 0, &data);

    // 保存为BMP（假设格式是B8G8R8A8_UNORM�?
    SceUInt32 row_pitch = (SceUInt32)layout.rowPitch;
    SceUInt32 bmp_row_pitch = ((w * 3 + 3) / 4) * 4; // 24位BMP行对�?
    SceUInt32 pixel_data_size = bmp_row_pitch * h;

    // ★★�?h145k377【内容触发抓�?/ 逐帧亮度统计】★★★
    //   动机：固定时�?/ 固定步长采样反复漏掉"用户肉眼可见但只在个别帧出现"的真实内�?
    //   （SAVE 小图标动画、启动瞬间的 ATLUS logo），导致"全黑"结论反复出错�?
    //   这里�?*写文件之�?*先对回读到的整幅图算 max/mean�?
    //     - 无条件打�?[SHOTSTAT]（每帧一行）�?"哪一帧有内容"�?*测量**出来的，不再靠猜�?
    //     - 若设�?P5D_SEQ_MIN=<阈�?，则 max < 阈值时**直接丢弃、不写文�?*（返�?1）�?
    //       于是可以 STEP=1 全程逐帧扫描，却只留下真正有内容的帧（磁�?后续分析都不爆）�?
    unsigned _ss_max = 0; double _ss_sum = 0.0; unsigned long long _ss_n = 0;
    for (SceUInt32 _y = 0; _y < h; _y += 2) {
        unsigned char* _sr = (unsigned char*)data + (size_t)_y * row_pitch;
        for (SceUInt32 _x = 0; _x < w; _x += 2) {
            unsigned _b0 = _sr[_x * 4 + 0], _g0 = _sr[_x * 4 + 1], _r0 = _sr[_x * 4 + 2];
            unsigned _m0 = _b0; if (_g0 > _m0) _m0 = _g0; if (_r0 > _m0) _m0 = _r0;
            if (_m0 > _ss_max) _ss_max = _m0; _ss_sum += _m0; _ss_n++;
        }
    }
    { static long _ssc = 0; long _lim = 200000;
      const char* _l = getenv("P5D_SHOTSTAT_MAX"); if (_l && *_l) _lim = strtol(_l, nullptr, 10);
      if (_ssc < _lim) { _ssc++;
        fprintf(stderr, "[SHOTSTAT] frame=%llu max=%u mean=%.2f file=%s\n",
            (unsigned long long)g_backend.frame_count, _ss_max,
            _ss_n ? (_ss_sum / (double)_ss_n) : 0.0, filename ? filename : "-");
        fflush(stderr); } }
    { static int _smin = -1;
      if (_smin < 0) { const char* _s = getenv("P5D_SEQ_MIN"); _smin = (_s && *_s) ? (int)strtol(_s, nullptr, 10) : 0; }
      if (_smin > 0 && (int)_ss_max < _smin) {
        vkUnmapMemory(device, *reinterpret_cast<VkDeviceMemory*>(&g_backend.screenshot_memory));
        return 1; } }

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        vkUnmapMemory(device, *reinterpret_cast<VkDeviceMemory*>(&g_backend.screenshot_memory));
        LOGE("Failed to open %s for writing", filename);
        return -1;
    }

    // BMP文件�?
    SceUInt16 sig = 0x4D42; // "BM"
    SceUInt32 file_size = 54 + pixel_data_size;
    SceUInt32 reserved = 0;
    SceUInt32 data_offset = 54;
    fwrite(&sig, 2, 1, fp);
    fwrite(&file_size, 4, 1, fp);
    fwrite(&reserved, 4, 1, fp);
    fwrite(&data_offset, 4, 1, fp);

    // BMP信息�?
    SceUInt32 header_size = 40;
    SceInt32 bmp_width = w;
    SceInt32 bmp_height = h; // 正数=底向�?
    SceUInt16 planes = 1;
    SceUInt16 bpp = 24;
    SceUInt32 compression = 0;
    SceUInt32 image_size = pixel_data_size;
    SceInt32 x_ppm = 2835;
    SceInt32 y_ppm = 2835;
    SceUInt32 colors_used = 0;
    SceUInt32 colors_important = 0;
    fwrite(&header_size, 4, 1, fp);
    fwrite(&bmp_width, 4, 1, fp);
    fwrite(&bmp_height, 4, 1, fp);
    fwrite(&planes, 2, 1, fp);
    fwrite(&bpp, 2, 1, fp);
    fwrite(&compression, 4, 1, fp);
    fwrite(&image_size, 4, 1, fp);
    fwrite(&x_ppm, 4, 1, fp);
    fwrite(&y_ppm, 4, 1, fp);
    fwrite(&colors_used, 4, 1, fp);
    fwrite(&colors_important, 4, 1, fp);

    // 像素数据（BGR -> BGR，Vulkan B8G8R8A8 -> BMP BGR�?
    unsigned char* row_buf = new unsigned char[bmp_row_pitch];
    for (SceUInt32 y = 0; y < h; y++) {
        // Vulkan图像是顶向下，BMP是底向上，所以从最后一行开�?
        SceUInt32 src_y = h - 1 - y;
        unsigned char* src_row = (unsigned char*)data + src_y * row_pitch;
        for (SceUInt32 x = 0; x < w; x++) {
            row_buf[x * 3 + 0] = src_row[x * 4 + 0]; // B
            row_buf[x * 3 + 1] = src_row[x * 4 + 1]; // G
            row_buf[x * 3 + 2] = src_row[x * 4 + 2]; // R
        }
        fwrite(row_buf, 1, bmp_row_pitch, fp);
    }
    delete[] row_buf;
    fclose(fp);
    vkUnmapMemory(device, *reinterpret_cast<VkDeviceMemory*>(&g_backend.screenshot_memory));

    LOGI("Screenshot saved: %s (%ux%u)", filename, w, h);
    return 0;
}

SceInt32 create_sync_objects() {
    LOGI("Vulkan: Creating sync objects...");
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkResult result = vkCreateFence(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        &fence_info, nullptr,
        reinterpret_cast<VkFence*>(&g_backend.frame_fence));
    if (result != VK_SUCCESS) {
        LOGE("vkCreateFence failed: %s", vk_result_string(result));
        return -1;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(*reinterpret_cast<VkDevice*>(&g_backend.device),
        &semaphore_info, nullptr,
        reinterpret_cast<VkSemaphore*>(&g_backend.image_available));
    vkCreateSemaphore(*reinterpret_cast<VkDevice*>(&g_backend.device),
        &semaphore_info, nullptr,
        reinterpret_cast<VkSemaphore*>(&g_backend.render_finished));
    LOGI("Sync objects created");
    return 0;
}

VkCommandBuffer begin_command_buffer() {
    if (!g_backend.command_pool) return nullptr;
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = *reinterpret_cast<VkCommandPool*>(&g_backend.command_pool);
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(*reinterpret_cast<VkDevice*>(&g_backend.device),
            &alloc_info, &cmd) != VK_SUCCESS) return nullptr;
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        vkFreeCommandBuffers(*reinterpret_cast<VkDevice*>(&g_backend.device),
            *reinterpret_cast<VkCommandPool*>(&g_backend.command_pool), 1, &cmd);
        return nullptr;
    }
    fprintf(stderr,"[CBEG] cmd=%p tid=%lu\n",(void*)cmd,(unsigned long)std::hash<std::thread::id>{}(std::this_thread::get_id())); fflush(stderr);
    return cmd;
}

void end_command_buffer(VkCommandBuffer cmd) {
    if (cmd){ fprintf(stderr,"[CEND] cmd=%p tid=%lu\n",(void*)cmd,(unsigned long)std::hash<std::thread::id>{}(std::this_thread::get_id())); fflush(stderr); vkEndCommandBuffer(cmd); }
}

void submit_command_buffer(VkCommandBuffer cmd) {
    if (!cmd || !g_backend.graphics_queue) return;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    vkQueueSubmit(*reinterpret_cast<VkQueue*>(&g_backend.graphics_queue),
        1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(*reinterpret_cast<VkQueue*>(&g_backend.graphics_queue));
    vkFreeCommandBuffers(*reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkCommandPool*>(&g_backend.command_pool), 1, &cmd);
}

// 【h137z】init 阶段创建的持�?VB/IB 在本机驱动上 GPU 读不到（CPU mapped 正常），
// 帧内（device/swapchain 完全就绪、首�?renderpass 内）重建一次即稳定可读�?
static auto ensure_geometry_buffers_frame = [](){
    static int _done=0; if(_done) return; _done=1;
    VkDevice dev=*reinterpret_cast<VkDevice*>(&g_backend.device);
    vkDeviceWaitIdle(dev);
    if(g_backend.vertex_buffer_mapped){ vkUnmapMemory(dev,*reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory)); g_backend.vertex_buffer_mapped=nullptr; }
    if(g_backend.vertex_buffer_memory){ vkFreeMemory(dev,*reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory),nullptr); g_backend.vertex_buffer_memory=nullptr; }
    if(g_backend.vertex_buffer){ vkDestroyBuffer(dev,*reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer),nullptr); g_backend.vertex_buffer=nullptr; }
    if(g_backend.index_buffer_mapped){ vkUnmapMemory(dev,*reinterpret_cast<VkDeviceMemory*>(&g_backend.index_buffer_memory)); g_backend.index_buffer_mapped=nullptr; }
    if(g_backend.index_buffer_memory){ vkFreeMemory(dev,*reinterpret_cast<VkDeviceMemory*>(&g_backend.index_buffer_memory),nullptr); g_backend.index_buffer_memory=nullptr; }
    if(g_backend.index_buffer){ vkDestroyBuffer(dev,*reinterpret_cast<VkBuffer*>(&g_backend.index_buffer),nullptr); g_backend.index_buffer=nullptr; }
    int a=create_vertex_buffer();
    int b=create_index_buffer();
    fprintf(stderr,"[H137Z] in-frame geometry buffers rebuilt vb=%d ib=%d vbuf=%p ibuf=%p vmap=%p imap=%p\n",
        a,b,(void*)*reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer),(void*)*reinterpret_cast<VkBuffer*>(&g_backend.index_buffer),
        (void*)g_backend.vertex_buffer_mapped,(void*)g_backend.index_buffer_mapped);
};
void cmd_begin_render_pass(VkCommandBuffer cmd, SceUInt32 image_index) {
    if (!cmd || !g_backend.render_pass || !g_backend.swapchain_framebuffers) return;

    VkClearValue clear_values[1];
    clear_values[0].color.float32[0] = g_backend.clear_color.r;
    clear_values[0].color.float32[1] = g_backend.clear_color.g;
    clear_values[0].color.float32[2] = g_backend.clear_color.b;
    clear_values[0].color.float32[3] = g_backend.clear_color.a;

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = *reinterpret_cast<VkRenderPass*>(&g_backend.render_pass);
    render_pass_info.framebuffer = g_backend.swapchain_framebuffers[image_index];
    render_pass_info.renderArea.offset = {0, 0};
    render_pass_info.renderArea.extent = {g_backend.swapchain_width, g_backend.swapchain_height};
    render_pass_info.clearValueCount = 1;
    render_pass_info.pClearValues = clear_values;

    vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    g_backend.in_render_pass = true;
    // h143h: Vita GXM 帧缓冲左上原点、Y 朝下（viewport yScale=-h/2）；Vulkan �?negative-height �?Y 翻转
    cmd_set_viewport(cmd, 0, (SceFloat)g_backend.swapchain_height, (SceFloat)g_backend.swapchain_width, -(SceFloat)g_backend.swapchain_height);
    cmd_set_scissor(cmd, 0, 0, g_backend.swapchain_width, g_backend.swapchain_height);
    ensure_geometry_buffers_frame();
}

void cmd_end_render_pass(VkCommandBuffer cmd) {
    if (!cmd || !g_backend.in_render_pass) return;
    vkCmdEndRenderPass(cmd);
    g_backend.in_render_pass = false;
}

// ===== h144k74【GXM viewport 状态机 �?Vulkan 视口】权威：Vita3K renderer/src/vulkan/sync_state.cpp:174-198 =====
//   on gxm:    x_f = xOffset + xScale * (x / w)
//   on vulkan: x_f = (x + width/2) + width/2 * (x / w)
//   �?w = |2*xScale| ; h = 2*yScale ; x = xOffset - |xScale| ; y = yOffset - yScale
//   而什么时候用这个视口、什么时候用"全屏扁平"视口，由 viewport_flat 决定
//   （renderer/src/state_set.cpp:145-148：sceGxmSetViewport �?flat=false；SetViewportEnable(DISABLED) �?flat=true）�?
//   旧实�?*永远只设全屏正高度视�?*，把引擎给的正交投影整体丢掉（docs/progress.md h144k28 第一步要求修的点）�?
static SceFloat g_gxm_vp[6]={0.f,0.f,0.f,0.f,0.f,0.f};
static int      g_gxm_vp_valid=0, g_gxm_vp_flat=1;
void set_gxm_viewport(SceFloat xo,SceFloat xs,SceFloat yo,SceFloat ys,SceFloat zo,SceFloat zs){
    g_gxm_vp[0]=xo; g_gxm_vp[1]=xs; g_gxm_vp[2]=yo; g_gxm_vp[3]=ys; g_gxm_vp[4]=zo; g_gxm_vp[5]=zs;
    g_gxm_vp_valid=1; g_gxm_vp_flat=0;   // 权威：SetViewport 隐含 "viewport enabled with offset/scale"
}
void set_gxm_viewport_enable(int mode){ g_gxm_vp_flat=(mode==0)?1:0; }   // DISABLED=0 => flat
int  gxm_viewport_flat(){ return g_gxm_vp_flat; }

void cmd_set_viewport(VkCommandBuffer cmd, SceFloat x, SceFloat y, SceFloat w, SceFloat h) {
    if (!cmd) return;
    // h144k34：把"最后一次真正下发的视口"记下来，�?draw 时打印（排查"零像�?时最容易被忽略的一环）
    g_last_vp[0]=x; g_last_vp[1]=y; g_last_vp[2]=w; g_last_vp[3]=h; g_vp_sets++;
    VkViewport viewport{};
    viewport.x = x; viewport.y = y;
    viewport.width = w; viewport.height = h;
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    // h144k22【坐标系归因实验】P5D_DBG_PXVIEWPORT=1：把视口设成「identity」变�?
    //   (x=-1,y=-1,w=2,h=2) �?x_win = ndc.x*0.5*2 + (-1) + 0.5*2 = ndc.x，即 clip.xy 直接当窗口坐标�?
    //   实测 vert_0015 的顶点位置是**像素坐标**(288~1020, 165~572)，而它�?SPIR-V �?gl_Position.xy=属�?xy 直�?
    //   �?�?GL/GXM �?[-1,1] NDC 约定会被整体裁掉（主 RT 全黑的直接原因）。本开关用于验证该判断�?
    { static const char* _pv=getenv("P5D_DBG_PXVIEWPORT");
      if(_pv&&*_pv){ viewport.x=-1.0f; viewport.y=-1.0f; viewport.width=2.0f; viewport.height=2.0f; } }
    vkCmdSetViewport(cmd, 0, 1, &viewport);
}

void cmd_set_scissor(VkCommandBuffer cmd, SceUInt32 x, SceUInt32 y, SceUInt32 w, SceUInt32 h) {
    if (!cmd) return;
    g_last_sc[0]=(int)x; g_last_sc[1]=(int)y; g_last_sc[2]=(int)w; g_last_sc[3]=(int)h; g_sc_sets++;
    VkRect2D scissor{};
    scissor.offset = {(int32_t)x, (int32_t)y};
    scissor.extent = {w, h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

int g_bind_pipeline_count = 0;
void cmd_bind_pipeline(VkCommandBuffer cmd) {
    VkPipeline _real = g_current_real_pipe;
    VkPipeline _fallback = *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline);
    VkPipeline _use = _real ? _real : _fallback;
    if (!cmd || !_use) return;
    g_bind_pipeline_count++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _use);
}

void cmd_bind_vertex_buffer(VkCommandBuffer cmd, VkBuffer buffer) {
    if (!cmd || !buffer) return;
    VkDeviceSize offsets[] = {0};
    VkBuffer buf = *reinterpret_cast<VkBuffer*>(&buffer);
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);
}

int g_draw_call_count = 0;
void cmd_draw(VkCommandBuffer cmd, SceUInt32 vertex_count, SceUInt32 instance_count,
              SceUInt32 first_vertex, SceUInt32 first_instance) {
    if (!cmd) return;
    g_draw_call_count++;
    // 18.3597 per-draw state diagnostic (first 40 draws): what is actually submitted and to which RT/pipeline.
    if (g_draw_call_count <= 40) {
        std::printf("[DRAWST] #%d vc=%u inst=%u first=%u pipe=%p inrp=%d vtxcnt=%u vbuf=%p vp=%.1f,%.1f,%.1f,%.1f sc=%d,%d,%d,%d rp=%p\n",
            g_draw_call_count, (unsigned)vertex_count, (unsigned)instance_count, (unsigned)first_vertex,
            (void*)g_current_real_pipe, (int)g_backend.in_render_pass, (unsigned)g_backend.bound_vertex_count,
            (void*)g_backend.vertex_buffer, g_last_vp[0], g_last_vp[1], g_last_vp[2], g_last_vp[3],
            g_last_sc[0], g_last_sc[1], g_last_sc[2], g_last_sc[3], (void*)g_cur_pass_rp);
        std::fflush(stdout);
    }
    vkCmdDraw(cmd, vertex_count, instance_count, first_vertex, first_instance);
}

void cmd_draw_indexed(VkCommandBuffer cmd, SceUInt32 index_count, SceUInt32 instance_count,
                      SceUInt32 first_index, SceInt32 vertex_offset, SceUInt32 first_instance) {
    if (!cmd) return;
    static int _dc=0;
    if (++_dc <= 40) {
        std::printf("[DRAWSTI] #%d ic=%u inst=%u first=%u vofs=%d pipe=%p inrp=%d vbuf=%p rp=%p\n",
            _dc, (unsigned)index_count, (unsigned)instance_count, (unsigned)first_index, (int)vertex_offset,
            (void*)g_current_real_pipe, (int)g_backend.in_render_pass, (void*)g_backend.vertex_buffer, (void*)g_cur_pass_rp);
        std::fflush(stdout);
    }
    vkCmdDrawIndexed(cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
}

// 上传 GXM 顶点流数据到持久映射的顶点缓冲（替代写死�?test_triangle�?
SceInt32 upload_vertex_stream(const void* data, SceUInt32 bytes, SceUInt32 vertex_count) {
    if (!data || bytes == 0) { LOGE("upload_vertex_stream: empty"); return -1; }
    if (!g_backend.vertex_buffer_mapped) { LOGE("upload_vertex_stream: VB not mapped"); return -1; }
    if (bytes > g_backend.vertex_capacity) {
        LOGE("upload_vertex_stream: %u bytes > capacity %u", (int)bytes, (int)g_backend.vertex_capacity);
        return -1;
    }
    memcpy(g_backend.vertex_buffer_mapped, data, bytes);
    g_backend.bound_vertex_count = vertex_count;
    LOGI("upload_vertex_stream: %u bytes, %u verts", (int)bytes, (int)vertex_count);
    return 0;
}

// GXM draw 状态机：用当前绑定�?pipeline / 描述�?/ 顶点缓冲按数组绘�?
void gxm_draw_arrays(SceUInt32 vertex_count) {
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    if (!cmd) { LOGE("gxm_draw_arrays: no current command buffer"); return; }
    VkPipeline _draw_pipe = g_current_real_pipe ? g_current_real_pipe
                          : *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline);
    if (!_draw_pipe) { LOGE("gxm_draw_arrays: no graphics pipeline bound"); return; }
    cmd_bind_pipeline(cmd);
    cmd_bind_descriptor_set(cmd);
    cmd_bind_vertex_buffer(cmd, g_backend.vertex_buffer);
    cmd_draw(cmd, vertex_count, 1, 0, 0);
    g_draw_call_count++;
}

// 【h137】索引缓冲（持久映射，HOST_COHERENT�?
SceInt32 create_index_buffer() {
    VkDeviceSize buffer_size = 1048576;  // h141: stride128 多属性顶点，1MB
    g_backend.index_capacity = (SceUInt32)buffer_size;
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = buffer_size; bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(dev, &bi, nullptr, reinterpret_cast<VkBuffer*>(&g_backend.index_buffer));
    if (r != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *reinterpret_cast<VkBuffer*>(&g_backend.index_buffer), &mr);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = vkAllocateMemory(dev, &ai, nullptr, reinterpret_cast<VkDeviceMemory*>(&g_backend.index_buffer_memory));
    if (r != VK_SUCCESS) return -1;
    vkBindBufferMemory(dev, *reinterpret_cast<VkBuffer*>(&g_backend.index_buffer),
        *reinterpret_cast<VkDeviceMemory*>(&g_backend.index_buffer_memory), 0);
    void* d = nullptr;
    vkMapMemory(dev, *reinterpret_cast<VkDeviceMemory*>(&g_backend.index_buffer_memory), 0, buffer_size, 0, &d);
    g_backend.index_buffer_mapped = d;
    return d ? 0 : -1;
}
SceInt32 upload_index_stream(const void* data, SceUInt32 bytes) {
    if (!data || bytes == 0 || !g_backend.index_buffer_mapped) return -1;
    if (bytes > g_backend.index_capacity) return -1;
    memcpy(g_backend.index_buffer_mapped, data, bytes);
    return 0;
}

// 【h137】上传真实顶�?索引并录�?vkCmdDrawIndexed
static SceUInt32 g_dbg_pass_w=0, g_dbg_pass_h=0;  // h144p diag
static SceUInt32 g_dbg_pass_color=0;  // h144t
static VkCommandBuffer g_rp_cmd_dbg=nullptr; static VkFramebuffer g_rp_fb_dbg=nullptr; static SceUInt32 g_rp_cd_dbg=0;
// h144k32：记录本 pass 实际使用�?VkRenderPass 对象，用于与管线**创建�?*�?RP 对比
//   （Vulkan 要求两者兼容，否则�?draw 是未定义行为，常见表现就是整�?draw 不产生像素）�?
static VkRenderPass g_rp_rp_dbg=nullptr;   // h144bs cmd match
static std::mutex g_vk_record_mtx;  // h144aa: 串行�?Vulkan 命令录制
static VkDeviceSize g_frame_vb_off=0, g_frame_ib_off=0;  // h144bs X17 per-draw geometry arena (reset in frame_acquire)
void gxm_draw_indexed_real(const void* vtx, SceUInt32 vbytes, const void* idx, SceUInt32 ibytes,
                           SceUInt32 index_count, SceUInt32 index_16) {
    // 18.3609: reliable backend-level frag-hash skip (P5D_SKIP_FH_BE=<hex64>[,<hex64>...]).
    // Probe-level P5D_DBG_SKIP_FH/P5D_SKIP_VH turned out ineffective (inside manifest gating); skipping here,
    // right before submission, always takes effect. Useful to remove full-screen constant-colour overwriters
    // and reveal what content draws actually produce.
    { static int _ini=-1; static SceUInt64 _hs[8]; static int _hn=0;
      if(_ini<0){ _ini=0; const char* _e=std::getenv("P5D_SKIP_FH_BE");
        if(_e&&*_e){ char _b2[256]; std::snprintf(_b2,sizeof(_b2),"%s",_e); char* _s=_b2;
          while(_s&&*_s&&_hn<8){ char* _c=std::strchr(_s,','); if(_c)*_c=0;
            SceUInt64 _v=std::strtoull(_s,nullptr,16); if(_v) _hs[_hn++]=_v; _s=_c?(_c+1):nullptr; } } }
      if(_hn){ for(int _i=0;_i<_hn;_i++) if(g_cur_draw_fh==_hs[_i]){ static long _sk=0; ++_sk;
          if(_sk<=4){ std::printf("[SKIPFH-BE] skip fh=%016llX count=%ld\n",(unsigned long long)g_cur_draw_fh,_sk); std::fflush(stdout); }
          return; } } }
    std::lock_guard<std::mutex> _vklk(g_vk_record_mtx);
    VkDeviceSize _vbo=0, _ibo=0;  // h144bs X17 per-draw geometry offsets
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    VkPipeline pipe = g_current_real_pipe ? g_current_real_pipe
                       : *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline);
    // 18.3607: AE-only post-processing diagnostics (cd AE100000/AE300000).
    if(g_dbg_pass_color==0xAE100000u || g_dbg_pass_color==0xAE300000u){ static int _ae=0;
      if(_ae<60){ ++_ae; std::printf("[AEDRAW #%d] cd=%08X vbytes=%u ibytes=%u ic=%u prim=%08X fh=%016llX pipe=%p\n",
          _ae,(unsigned)g_dbg_pass_color,(unsigned)vbytes,(unsigned)ibytes,(unsigned)index_count,(unsigned)g_cur_prim,
          (unsigned long long)g_cur_draw_fh,(void*)g_current_real_pipe); std::fflush(stdout); } }
    // 18.3598: stdout diagnostic for the first calls: geometry sizes, render target, primitive type, in-pass.
    { static int _di2=0; if(_di2<80){ ++_di2;
        std::printf("[DRAWST2] #%d vbytes=%u ibytes=%u ic=%u i16=%u inRP=%d passW=%u passCD=%08X pipe=%p cmd=%p prim=%08X vtx=%p idx=%p passCmd=%p cmdMatch=%d\n",
          _di2,(unsigned)vbytes,(unsigned)ibytes,(unsigned)index_count,(unsigned)index_16,
          (int)g_backend.in_render_pass,(unsigned)g_dbg_pass_w,(unsigned)g_dbg_pass_color,
          (void*)pipe,(void*)cmd,(unsigned)g_cur_prim,(const void*)vtx,(const void*)idx,
          (void*)g_rp_cmd_dbg,(int)(cmd==g_rp_cmd_dbg));
        std::fflush(stdout); } }
    // h145k35【候�?1/2/3 判别】大几何（内容精�?vbytes>=1024）在真正提交时打印：
    //   inRP = 是否处于 render pass 内（0 �?�?draw 会被丢弃）；
    //   passCD = 本次 pass 的目�?RT 地址；pipe = 实际使用的管线指针�?
    if(vbytes>=1024u){ static long long _dl=0; _dl++;
      if(_dl<=12 || (_dl%200)==0)
        fprintf(stderr,"[DRAWIDX n=%lld] vbytes=%u ibytes=%u inRP=%d passCD=%08X pipe=%p cur=%p\n",
          (unsigned long long)_dl,vbytes,ibytes,(int)g_backend.in_render_pass,g_dbg_pass_color,
          (void*)pipe,(void*)g_current_real_pipe); fflush(stderr); }
    if (!cmd || !pipe) { LOGE("gxm_draw_indexed_real: no cmd/pipe"); return; }
    // ★★�?h145k1138【诊�?+ 非索�?draw 的恒等索引构造】★★★
    //   动机：h145k1136 加了 STRIP/FAN→LIST 的索引展开后，实测**采样影片纹理的那三笔
    //   `prim=0C000000`(TRIANGLE_STRIP) 完全没有出现�?`[STRIPFIX]` �?*，只有一�?
    //   `5D73281E76A107B2`(FAN) 被展开。最可能的原因是它们走的�?*非索引路�?*
    //   （eboot_probe �?`gxm_draw_arrays` 分支：`s_pd_idx=0, s_pd_isz=0` �?这里 `ibytes==0`）�?
    //   �?`ibytes==0` 会让下面�?`vkCmdBindIndexBuffer` 整段被跳�?�?`vkCmdDrawIndexed` 用的�?
    //   **上一�?draw 残留的索引缓�?*：几何来源完全错误，正是用户看到�?半边被拉�?�?
    //   修法：索引缺失时**构造恒等索�?*�?,1,2,…），再交给 h145k1136 的拓扑展开�?
    //   为降低风险，只在 `count �?[3,8]`（四边形/整屏三角形一类后处理几何）时启用�?
    //   诊断：`[PRIMSEE]` 对每 (primType,片元哈希) 首现打印一次实参�?
    std::vector<SceUInt8> _idbuf;
    { static std::map<unsigned long long,long> _psm;
      const unsigned long long _pk=((unsigned long long)g_cur_draw_fh<<8)|(unsigned long long)g_cur_prim;
      if((g_cur_prim==0x0C000000u||g_cur_prim==0x10000000u) && _psm[_pk]<2){ _psm[_pk]++;
        fprintf(stderr,"[PRIMSEE] prim=%08X fh=%016llX idx=%s ibytes=%u count=%u i16=%u\n",
          g_cur_prim,(unsigned long long)g_cur_draw_fh,(idx?"yes":"NULL"),
          (unsigned)ibytes,(unsigned)index_count,(unsigned)index_16); fflush(stderr); } }
    if((!idx || ibytes==0u) && index_count>=3u && index_count<=8u){
      _idbuf.assign((size_t)index_count*2u,0);
      for(SceUInt32 _k=0;_k<index_count;_k++){
        _idbuf[_k*2u]=(SceUInt8)(_k&0xFFu); _idbuf[_k*2u+1u]=(SceUInt8)((_k>>8)&0xFFu); }
      idx=_idbuf.data(); ibytes=(SceUInt32)_idbuf.size(); index_16=1u;
      static std::map<unsigned long long,long> _idm;
      long& _idc=_idm[((unsigned long long)g_cur_draw_fh<<8)|(unsigned long long)g_cur_prim];
      if(_idc<3){ _idc++;
        fprintf(stderr,"[IDXFIX] prim=%08X fh=%016llX 非索�?count=%u -> 恒等索引\n",
          g_cur_prim,(unsigned long long)g_cur_draw_fh,(unsigned)index_count); fflush(stderr); } }
    if(false && g_dbg_pass_w>=900u && index_count==3u){  /* h144bs X10: X8 blue disabled */
        VkClearAttachment _ia{}; _ia.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; _ia.colorAttachment=0;
        _ia.clearValue.color.float32[0]=0.f; _ia.clearValue.color.float32[1]=0.f; _ia.clearValue.color.float32[2]=1.f; _ia.clearValue.color.float32[3]=1.f;
        VkRect2D _isr{}; _isr.offset={0,0}; _isr.extent={g_dbg_pass_w,g_dbg_pass_h}; vkCmdSetScissor(cmd,0,1,&_isr);
        VkClearRect _icr{}; _icr.rect.offset={0,0}; _icr.rect.extent={g_dbg_pass_w,g_dbg_pass_h}; _icr.baseArrayLayer=0; _icr.layerCount=1;
        vkCmdClearAttachments(cmd,1,&_ia,1,&_icr);
        static int s_x8n=0; if(s_x8n<8){ fprintf(stderr,"[X8ENTRY] blue clear at gxm_draw entry cmd=%p inRP=%d n=%d\n",(void*)cmd,(int)g_backend.in_render_pass,s_x8n); fflush(stderr); s_x8n++; }
    }
    if(false && g_dbg_pass_w>=900u && index_count!=3u){ return; }  // h144bs X12: T2 disabled, all real draws run
    { static SceUInt64 _lf=(SceUInt64)~0; static int _tn=0;
      if(g_backend.frame_count!=_lf){_lf=g_backend.frame_count;_tn=0;} _tn++;
      int _lim=1<<(int)(g_backend.frame_count-1); if(_lim>64)_lim=64;
      (void)_lim;(void)_tn; /* h139c draw-gate disabled to verify full first-frame geometry */ }
    if (vtx && vbytes && vbytes <= g_backend.vertex_capacity && g_backend.vertex_buffer_mapped){
        g_frame_vb_off=(g_frame_vb_off+15)&~(VkDeviceSize)15;  // h144bs X17 16B align
        if(g_frame_vb_off+vbytes > (VkDeviceSize)g_backend.vertex_capacity) g_frame_vb_off=0;  // wrap (fence waited at acquire)
        _vbo=g_frame_vb_off;
        memcpy((char*)g_backend.vertex_buffer_mapped+_vbo, vtx, vbytes);
        g_frame_vb_off+=vbytes;
        // 像素->NDC 已在 eboot_probe n_draw �?vertex program hash 解包(U16/F16)，后端不再二次变�?
    }
    // h144k34【深度裁剪归因实�?—�?本轮找到�?零像�?根因验证�?
    //   [VREAD] 已证明顶点数据、索引、绑定偏移、管线描述符全部正确，[DYNSTATE] 证明 viewport/scissor 正常�?
    //   但把绑定的顶点按管线描述符反解出来后�?*每个顶点�?z 都大�?w**�?
    //     · vert_0015(主力 pass): loc0=(288.9,165.7, 2.125, 1.0)  �?z_ndc=2.125
    //     · vert_0014(背景 quad): loc0=(-1,-1, 2.0, 1.0)..(1,1,2.0) �?z_ndc=2.0
    //   Vulkan 规范 27.3：无论是否存在深度附件，只要 rasterization �?depthClampEnable=VK_FALSE
    //   （本后端 840 行即�?FALSE），图元都会被裁剪到裁剪�?**0 �?Zc �?Wc** —�?z 超界 �?整个三角形被丢弃�?
    //   这正�?所有链路都对却一个像素都不写"的原因，也解释了为何 vkCmdClearAttachments �?
    //   绿色标记能画�?10000 像素（ClearAttachments 不经过裁剪体）�?
    //   本开关用�?*决定性验�?*：把每个顶点�?z 强制成合法值�?
    //     P5D_ZFORCE=w  �?z=w（即 z_ndc=1.0，远平面）；P5D_ZFORCE=<float> �?z=该常数�?
    { static int _zf_mode=-3; static float _zf=0.5f;
      if(_zf_mode==-3){ _zf_mode=-1; const char* _s=getenv("P5D_ZFORCE");
        if(_s&&*_s){ if(_s[0]=='w'||_s[0]=='W') _zf_mode=1; else { _zf=(float)atof(_s); _zf_mode=0; }
          fprintf(stderr,"[ZFORCE] enabled mode=%s z=%s\n",(_zf_mode==1)?"w":"const",(_zf_mode==1)?"w":_s); fflush(stderr); } }
      if(_zf_mode>=0 && g_backend.vertex_buffer_mapped && vbytes>=128u){
        unsigned char* _b=(unsigned char*)g_backend.vertex_buffer_mapped+_vbo;
        for(unsigned _o=0;_o+128u<=(unsigned)vbytes;_o+=128u){ float* _p=(float*)(_b+_o); _p[2]= (_zf_mode==1)? _p[3] : _zf; }
        static int _zc=0; if(_zc<3){ _zc++; const float* _p0=(const float*)_b;
          fprintf(stderr,"[ZFORCE] draw idx=%u vbytes=%u _vbo=%llu -> v0=(%.4f,%.4f,%.4f,%.4f)\n",
            index_count,(unsigned)vbytes,(unsigned long long)_vbo,_p0[0],_p0[1],_p0[2],_p0[3]); fflush(stderr); } } }
    // ★★�?h145k412【默�?z 夹取 —�?对齐 POWERVR �?深度钳位"行为（本目标�?背景纯黑"根因）】★★★
    //   证据链（本轮实测）：
    //     · 全屏合成程序 `frag_0079`（占全部 draw 45%，vert_0014 �?quad）上传顶�?**z=2.0**�?
    //     · 它其实是**整屏全覆�?*：`ONLY_FH=5D73281E76A107B2 + ZFORCE=0.5 + REALFS=r`
    //       实测 `[SHOTSTAT] max=255 mean=255.00`�?00% 红）；用真着色器时整�?*纯黑**（`颜色变化次数=1`）；
    //     · 而不夹取时它**一个像素都不写**（同配置 `max=0`）：Vulkan 规范 27.3 —�?depthClampEnable=FALSE �?
    //       图元被裁到裁剪体 `0 �?Zc �?Wc`，z=2.0 超界 �?整个三角形被丢弃�?
    //     · 真机�?POWERVR SGX�?*深度钳位而非裁剪** �?同一份数据在 Vita 上完全正常�?
    //   �?修法：上传后�?*越界**�?z 夹进 [0,1]（范围内�?z 原样保留，不破坏引擎自己的层�?深度意图）�?
    //     效果：引擎想要非黑背景的那些阶段（例如原版红色主视觉）不再被整块裁掉�?
    //   可回退：P5D_NO_ZCLAMP=1。与 P5D_ZFORCE（无条件强制某个 z）不同，本项只处理越界值�?
    { static int _zcl=-1;
      if(_zcl<0){ _zcl = std::getenv("P5D_NO_ZCLAMP")?0:1;
        if(_zcl) fprintf(stderr,"[ZCLAMP] 默认开启：越界 z 夹进 [0,1]（对�?POWERVR 深度钳位）\n"); fflush(stderr); }
      if(_zcl && g_backend.vertex_buffer_mapped && vbytes>=128u){
        unsigned char* _b=(unsigned char*)g_backend.vertex_buffer_mapped+_vbo;
        int _n=0; float _ex=0.0f;
        for(unsigned _o=0;_o+128u<=(unsigned)vbytes;_o+=128u){ float* _p=(float*)(_b+_o);
          float _z=_p[2];
          if(!(_z>=0.0f && _z<=1.0f)){ _ex=_z; _p[2]=(_z<0.0f)?0.0f:1.0f; _n++; } }
        static int _zc2=0; if(_n && _zc2<6){ _zc2++;
          fprintf(stderr,"[ZCLAMP] draw idx=%u vbytes=%u -> 夹取 %d 个越�?z（示�?%.3f）\n",
            index_count,(unsigned)vbytes,_n,(double)_ex); fflush(stderr); } } }
    // ★★�?h145k1136【拓扑修正：TRIANGLE_STRIP / TRIANGLE_FAN �?TRIANGLE_LIST】★★★
    //   动机（用户实测）：过场动画画面「沿对角线分成两半：左上一半正常，右下一半被拉伸」�?
    //   取证：本轮给 `[TEXSRC-DRAW]` 补打�?primType，实�?*采样影片纹理的那几笔 draw 全是**
    //     `prim=0C000000` —�?�?GXM �?**TRIANGLE_STRIP**（枚举见 gxm/types.h�?
    //     TRIANGLES=0 / TRIANGLE_STRIP=0x0C000000 / TRIANGLE_FAN=0x10000000）�?
    //   而本后端**所�?* pipeline 都硬编码 `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` �?一�?4 顶点�?
    //   STRIP 只会画出�?0 个三角形�? 四边形沿对角线的一半），另一半保留上一�?draw 的内容�?
    //   这正�?斜角 + 半边被拉�?。eboot_probe �?h145k664 �?PRIMFIX 曾试图修，但它默认关�?
    //   （当年实测变黑），原因是它与 h143t �?`cnt==4 �?_quad6` 预展开**叠加**造成二次展开�?
    //   修法：在后端统一�?CPU 侧索引展开（语义等价，无需改管线与描述符）�?
    //     · STRIP：三角形 i = (v[i],v[i+1],v[i+2])，i 为奇数时交换前两个以保持绕序�?
    //     · FAN  ：三角形 i = (v[0],v[i+1],v[i+2])�?
    //   安全性：本后端所�?pipeline �?`cullMode` 均为 `VK_CULL_MODE_NONE`，即使绕序判反也不会丢面�?
    //   配套：h143t 预展开处已�?`p5d::vulkan::g_cur_prim` 归零，避免二次展开�?
    //   回退：`P5D_NO_STRIPFIX=1`�?
    std::vector<SceUInt8> _sfix;
    { static int _sfon=-1;
      if(_sfon<0){ _sfon=std::getenv("P5D_NO_STRIPFIX")?0:1;
        if(_sfon) fprintf(stderr,"[STRIPFIX] 拓扑修正开启：STRIP/FAN 索引将展开为三角形列表\n"); fflush(stderr); }
      if(_sfon && idx && ibytes>=2u && index_count>=3u &&
         (g_cur_prim==0x0C000000u || g_cur_prim==0x10000000u) &&
         (VkDeviceSize)ibytes <= g_backend.index_capacity){
        const bool _i16=(index_16!=0);
        const SceUInt32 _avail=(SceUInt32)(ibytes/(_i16?2u:4u));
        const SceUInt32 _cnt=(index_count<_avail)?index_count:_avail;
        if(_cnt>=3u && _cnt<65536u){
          const SceUInt8* _pb=(const SceUInt8*)idx;
          std::vector<SceUInt32> _ix(_cnt);
          for(SceUInt32 _k=0;_k<_cnt;_k++){
            if(_i16) _ix[_k]=(SceUInt32)_pb[_k*2u]|((SceUInt32)_pb[_k*2u+1u]<<8);
            else _ix[_k]=(SceUInt32)_pb[_k*4u]|((SceUInt32)_pb[_k*4u+1u]<<8)
                        |((SceUInt32)_pb[_k*4u+2u]<<16)|((SceUInt32)_pb[_k*4u+3u]<<24); }
          const SceUInt32 _oc=(_cnt-2u)*3u;
          _sfix.assign((size_t)_oc*2u,0);
          SceUInt32 _w=0;
          if(g_cur_prim==0x0C000000u){
            for(SceUInt32 _k=0;_k+2u<_cnt;_k++){
              SceUInt32 _a,_b,_c;
              if((_k&1u)==0u){ _a=_ix[_k];   _b=_ix[_k+1u]; _c=_ix[_k+2u]; }
              else           { _a=_ix[_k+1u]; _b=_ix[_k];   _c=_ix[_k+2u]; }
              _sfix[_w*2u]=(SceUInt8)(_a&0xFFu); _sfix[_w*2u+1u]=(SceUInt8)((_a>>8)&0xFFu); _w++;
              _sfix[_w*2u]=(SceUInt8)(_b&0xFFu); _sfix[_w*2u+1u]=(SceUInt8)((_b>>8)&0xFFu); _w++;
              _sfix[_w*2u]=(SceUInt8)(_c&0xFFu); _sfix[_w*2u+1u]=(SceUInt8)((_c>>8)&0xFFu); _w++; } }
          else {
            for(SceUInt32 _k=1;_k+1u<_cnt;_k++){
              SceUInt32 _t3[3]; _t3[0]=_ix[0]; _t3[1]=_ix[_k]; _t3[2]=_ix[_k+1u];
              for(int _j=0;_j<3;_j++){ _sfix[_w*2u]=(SceUInt8)(_t3[_j]&0xFFu);
                _sfix[_w*2u+1u]=(SceUInt8)((_t3[_j]>>8)&0xFFu); _w++; } } }
          static std::map<unsigned long long,long> _sfmap;
          long& _sfc=_sfmap[((unsigned long long)g_cur_draw_fh<<8)|(unsigned long long)g_cur_prim];
          if(_sfc<4){ _sfc++;
            fprintf(stderr,"[STRIPFIX] prim=%08X fh=%016llX idx=%u(i%d) -> %u list indices\n",
              g_cur_prim,(unsigned long long)g_cur_draw_fh,_cnt,(int)_i16,_oc); fflush(stderr); }
          idx=_sfix.data(); ibytes=(SceUInt32)_sfix.size(); index_count=_oc; index_16=1u; } } }
    if (idx && ibytes && ibytes <= g_backend.index_capacity && g_backend.index_buffer_mapped){
        g_frame_ib_off=(g_frame_ib_off+3)&~(VkDeviceSize)3;  // h144bs X17 4B align
        if(g_frame_ib_off+ibytes > (VkDeviceSize)g_backend.index_capacity) g_frame_ib_off=0;
        _ibo=g_frame_ib_off;
        memcpy((char*)g_backend.index_buffer_mapped+_ibo, idx, ibytes);
        g_frame_ib_off+=ibytes;
    }
    { {
        if(vtx && vbytes>=48){ const float* _vp=(const float*)vtx;
          fprintf(stderr,"[H137AC] REAL first draw vbytes=%u ibytes=%u idx=%u i16=%u pipe=%p\n  vtx= %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
            vbytes,ibytes,index_count,index_16,(void*)pipe,
            _vp[0],_vp[1],_vp[2],_vp[3],_vp[4],_vp[5],_vp[6],_vp[7],_vp[8],_vp[9],_vp[10],_vp[11]); }
        else fprintf(stderr,"[H137AC] first draw no vtx vbytes=%u ibytes=%u idx=%u\n",vbytes,ibytes,index_count);
    } }
    cmd_bind_pipeline(cmd); cmd_bind_descriptor_set(cmd);  // h144bs X11 restored for idx3
    // ===== h144k92【�?draw 采样源取证】把"�?draw 的目标面"�?unit0 实际绑到的源"�?*同一�?*打出来，
    //   彻底消除此前靠日志先后顺序推断的歧义。unit0 三类：alias:<RT cd>（匹配到 RT 池的别名快照）�?
    //   tex:<guest 地址>（真实纹理）、dummy（既非别名也非真实）�?
    //   动机：三�?blit（frag_0090/0070/0026）疑似都在采�?自己正在写的那张�?，而快照取�?clear 之前
    //   �?RT_new = RT_old 的帧间自反馈。必须先无歧义地确认这件事，才能决定修法�?
    { if(g_backend.frame_count>=295ull && g_backend.frame_count<=305ull){
        static int _srcn=0;
        if(_srcn<80){ _srcn++;
          BackendState::UnitTexture& _u0=g_backend.unit_textures[0];
          const char* _k = _u0.is_rt_alias ? "alias" : (_u0.created ? "tex" : "dummy");
          SceUInt32 _a = _u0.is_rt_alias ? _u0.alias_color : _u0.last_vaddr;
          fprintf(stderr,"[DRAWSRC] fc=%llu cd=%08X idx=%u u0=%s:%08X %ux%u self=%d\n",
            (unsigned long long)g_backend.frame_count, g_dbg_pass_color, index_count, _k, _a,
            _u0.width, _u0.height, (int)(_u0.is_rt_alias && _u0.alias_color==g_dbg_pass_color));
          fflush(stderr); } } }
    // h144k33【管线绑定校验】g_current_real_pipe �?*全局变量**；若宿主多线程并发记�?draw�?
    //   后一个线程会覆盖它，导致本线程实际绑到别的管线（表现�?命令都正常却零像�?）�?
    //   这里�?本次 draw 绑定的管�?�?线程 id"一起打印，�?[REAL-PIPE] �?pipe 句柄对照即可判定�?
    { static long _dp=0; static long _dp30=0;
      bool _want = (index_count==30u) ? (_dp30++<8) : (_dp++<16);
      if(_want){
        // �?h145k340：补�?cmd 句柄 —�?用于�?[SUBMIT] cmd=%p 对照�?
        //   判定"内容 draw 录进的命令缓�?是否就是"�?vkQueueSubmit 提交的那�?�?
        //   （[SUBMIT] �?[DRAWPIPE] �?tid 实测相同 �?已排�?跨线程未提交"�?
        //     但两者此前都不打�?cmd，无法判断是否录进了另一个缓冲。）
        fprintf(stderr,"[DRAWPIPE] idx=%u boundPipe=%p cmd=%p tid=%lu passCD=%08X vbytes=%u vbo=%llu\n",
            index_count,(void*)g_current_real_pipe,(void*)cmd,
            (unsigned long)std::hash<std::thread::id>{}(std::this_thread::get_id()),
            g_dbg_pass_color,(unsigned)vbytes,(unsigned long long)_vbo); fflush(stderr);
        // �?h145k269【管线创�?RP vs 绘制 RP】按 (管线信息, 当前RP) 去重，每种组合最多打 3 �?
        { auto _pi=g_pipe_info.find(g_current_real_pipe);
          const char* _inf=(_pi!=g_pipe_info.end())?_pi->second.c_str():"(unknown)";
          char _kb[260]; std::snprintf(_kb,sizeof(_kb),"%s||cur=%p",_inf,(void*)g_cur_pass_rp);
          long& _c=g_pipe_mismatch[std::string(_kb)];
          if(_c<3){ _c++;
            fprintf(stderr,"[PIPE-RP] #%ld bound[%s] current g_cur_pass_rp=%p | idx=%u cw=%u CD=%08X\n",
                    _c,_inf,(void*)g_cur_pass_rp,index_count,(unsigned)g_dbg_pass_w,g_dbg_pass_color);
            fflush(stderr); } }
        // h144k34【动态状态取证】viewport/scissor 是绘制前最后两�?未证�?的环节：
        //   它们是动态状态，任一处下�?w=0/h=0 或空 scissor，都会让所�?draw 静默变成零像素�?
        //   这里打印"最后一�?vkCmdSet* 的实际�?与调用计数，用于排除该可能�?
        fprintf(stderr,"[DYNSTATE] sets vp=%d sc=%d | viewport(x=%.1f y=%.1f w=%.1f h=%.1f) scissor(x=%d y=%d w=%d h=%d) sw=%ux%u\n",
            g_vp_sets,g_sc_sets,g_last_vp[0],g_last_vp[1],g_last_vp[2],g_last_vp[3],
            g_last_sc[0],g_last_sc[1],g_last_sc[2],g_last_sc[3],
            (unsigned)g_backend.swapchain_width,(unsigned)g_backend.swapchain_height); fflush(stderr);
    } }
    // ★ ARCH_490【逐 draw 绑定日志（stdout）】`P5D_DRAWLOG=<fc0>[,<fc1>]`
    //   动机：既有的 `[DRAWSRC]` 走 **stderr** —— 而探针把 stderr `freopen` 到 NUL
    //   ⇒ 历史上这条日志一律看不到（同一个坑已踩多次）；而且它硬编码在帧 295~305。
    //   而"主界面那句细体长段落由哪些 draw 画出、它们绑了什么纹理"正是当前唯一未闭合的
    //   纹理问题（已证明 84 张线性纹理全部转储、无一遗漏）。这里在**每次 draw 绑定之后**
    //   把 目标面 + scissor + viewport + unit0..3 的绑定来源全部打到 stdout，
    //   供离线按**屏幕区域**筛出真正画那句话的那些 draw。
    { static long long _dl[2]={-2,-2}; static int _dlseen[2]={0,0};
      if(_dl[0]==-2){ for(int _i=0;_i<2;++_i) _dl[_i]=-1;
        const char* _e=std::getenv("P5D_DRAWLOG");
        if(_e&&*_e){ char _b[128]; std::snprintf(_b,sizeof(_b),"%s",_e); char* _s=_b; int _i=0;
          while(_s&&*_s&&_i<2){ char* _cc=std::strchr(_s,','); if(_cc)*_cc=0;
            _dl[_i++]=std::atoll(_s); _s=_cc?(_cc+1):nullptr; } }
        printf("[DRAWLOG-CFG] env=%s dl=[%lld,%lld]\n",_e?_e:"(null)",_dl[0],_dl[1]); fflush(stdout); }
      long long _fc=(long long)g_backend.frame_count;
      for(int _i=0;_i<2;++_i)
        if(_dl[_i]>=0 && !_dlseen[_i] && _fc>=_dl[_i]){
          _dlseen[_i]=1; printf("[DRAWLOG-BEGIN] target=%lld actual=%lld\n",_dl[_i],_fc); fflush(stdout); }
      bool _on=false;
      for(int _i=0;_i<2;++_i) if(_dl[_i]>=0 && _dlseen[_i] && _fc<=(long long)_dl[_i]+2) _on=true;
      // ★ ARCH_518【按内容触发，替代按帧号触发】`P5D_DRAWLOG_ATGLYPHS=<n>`
      //   动机（ARCH_517 实测教训）：**帧号↔场景的对应在运行之间会漂移** ——
      //   同一份二进制、同样 `P5D_DRAWLOG=3900`，a490/a501/a507 三次在 fc=3900 捕到的是
      //   **主菜单（约 120 次 draw、大量 32x32 字形）**，而 a517 那次捕到的却是
      //   **只有 4 次 draw 的图标画面**。所以"用帧号指定画面"是不可靠的。
      //   改为**按内容触发**：某一帧内"unit0 是 32x32 纹理（= 逐字形绘制）"的 draw 数
      //   达到阈值时，从该帧起连续记录 3 帧 ⇒ 必定落在真正的文字绘制上。
      { static long long _glLastFc=-1; static int _glCount=0; static int _glLeft=0;
        if(_fc!=_glLastFc){ _glLastFc=_fc; _glCount=0; if(_glLeft>0) --_glLeft; }
        static long long _gthr=-2;
        if(_gthr==-2){ _gthr=-1; const char* _s=std::getenv("P5D_DRAWLOG_ATGLYPHS"); if(_s&&*_s) _gthr=std::atoll(_s); }
        if(_gthr>=0){
          BackendState::UnitTexture& _u0=g_backend.unit_textures[0];
          if(!_u0.is_rt_alias && _u0.width==32u && _u0.height==32u){
            ++_glCount;
            if(_glCount==(int)_gthr){ _glLeft=3;
              printf("[DRAWLOG-ARM] fc=%lld 字形 draw 数达到 %d，开始记录\n",(long long)_fc,(int)_gthr); fflush(stdout); }
          }
          if(_glLeft>0) _on=true; } }
      g_dl_active = _on ? 1 : 0;      // ★ ARCH_525：供 sce_gxm.cpp 的 [DRAWVA] 共用同一窗口
      if(_on){
        char _ub[260]; int _o=0;
        for(int _u=0;_u<4;++_u){
          BackendState::UnitTexture& _ut=g_backend.unit_textures[_u];
          const char* _k=_ut.is_rt_alias?"al":(_ut.created?"tx":"dm");
          SceUInt32 _a=_ut.is_rt_alias?_ut.alias_color:_ut.last_vaddr;
          _o+=std::snprintf(_ub+_o,sizeof(_ub)-_o," u%d=%s:%08X:%ux%u",_u,_k,_a,_ut.width,_ut.height);
          if(_o>(int)sizeof(_ub)-64) break;
        }
        printf("[DRAWLOG] fc=%llu cd=%08X idx=%u sc=%d,%d,%d,%d vp=%.0f,%.0f,%.0f,%.0f%s\n",
               (unsigned long long)_fc,g_dbg_pass_color,index_count,
               g_last_sc[0],g_last_sc[1],g_last_sc[2],g_last_sc[3],
               g_last_vp[0],g_last_vp[1],g_last_vp[2],g_last_vp[3],_ub);
        // ★ ARCH_501：补上**每个 draw 的顶点坐标**，替代"取 guest 寄存器"那条走不通的路。
        //   动机：直方图已把异常定量（每帧 75 个字形 draw，`幾` 0 次、`：` 多 4 次、`列` 多 2 次、
        //   `為` 多 1 次，三帧一致 ⇒ 确定性），但还不知道多出的 `：` 是画在**可见文本区内**
        //   （⇒ 真的插了字）还是**区外**（⇒ 另一个不可见文本面板/第二层）。
        //   顶点坐标直接回答这个问题，而且 `vtx` 就在本函数作用域内，无需任何新头文件。
        // ★ ARCH_507：为了判定"+24px 错位"发生在**顶点数据里**还是**同一顶点被不同变换**，
        //   把每次字形 draw 的**前 32 个 float**（至少覆盖 2 个顶点）全部打出来，
        //   这样可以把一个"正常字形"与一个"错位字形"的完整顶点流逐字段对比。
        { if(vtx && vbytes>=128u){
            const float* _pv=(const float*)vtx;
            // ★ ARCH_522：把 `vtx` 指针本身也打出来 —— 上一轮我用 draw HLE 的 `r3` 当顶点地址去
            //   做写监控，结果监控到的是引擎分配器（`A2D50220` 处全是分配器记账写入，
            //   浮点样写入为 0）⇒ `r3` 并不是顶点指针。这里直接打真正传进 draw 的那个指针。
            printf("[DRAWVTX] fc=%llu idx=%u vbytes=%u vtx=%p gva=%08X u0=%s",(unsigned long long)_fc,index_count,(unsigned)vbytes,
                   vtx, p5d_host_to_gva(vtx), g_backend.unit_textures[0].is_rt_alias?"al":"tx");
            for(int _q=0;_q<32;++_q) printf(" %.2f",_pv[_q]);
            printf("\n"); }
          else if(vtx){
            const float* _pv=(const float*)vtx;
            int _n=(int)(vbytes/4u); if(_n>32) _n=32;
            printf("[DRAWVTX] fc=%llu idx=%u vbytes=%u vtx=%p u0=%s",(unsigned long long)_fc,index_count,(unsigned)vbytes,
                   vtx, g_backend.unit_textures[0].is_rt_alias?"al":"tx");
            for(int _q=0;_q<_n;++_q) printf(" %.2f",_pv[_q]);
            printf("\n"); }
          else
            printf("[DRAWVTX] fc=%llu idx=%u vbytes=%u (no vtx)\n",
                   (unsigned long long)_fc,index_count,(unsigned)vbytes);
          // ★ ARCH_517：把"发出这次 draw 的 guest 代码地址"一并打出来（lr = 那条 bl 的返回地址），
          //   这样就能用 `_dis/_arch512_pc2src.py` 经 `_srcmap.json` 定位到引擎的排版源文件。
          printf("[DRAWPC] fc=%llu idx=%u lr=%08X r0=%08X r1=%08X r2=%08X r3=%08X\n",
                 (unsigned long long)_fc,index_count,p5d_guest_lr(),
                 p5d_guest_reg(0),p5d_guest_reg(1),p5d_guest_reg(2),p5d_guest_reg(3));
          fflush(stdout); }
        fflush(stdout); } }
    { static const char* _vd=std::getenv("P5D_VTXDIAG"); static int _vdn=0;
      if(_vd && *_vd && _vdn<8 && vtx && vbytes>=128u){ ++_vdn;
        const float* _pv=(const float*)vtx; const int _nv=(int)(vbytes/128u);
        std::printf("[VTXDIAG] #%d vbytes=%u ic=%u cd=%08X nv=%d", _vdn,(unsigned)vbytes,(unsigned)index_count,(unsigned)g_dbg_pass_color,_nv);
        for(int _k=0;_k<6 && _k<_nv;_k++){ const float* _q=_pv+(size_t)_k*32;
            std::printf(" v%d=(%.2f,%.2f,%.2f,%.2f|%.2f,%.2f,%.2f,%.2f)",_k,_q[0],_q[1],_q[2],_q[3],_q[4],_q[5],_q[6],_q[7]); }
        std::printf("\n"); std::fflush(stdout); } }
    VkBuffer vbuf = *reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer);
    VkDeviceSize voff[1] = { _vbo };  // h144bs X17 per-draw offset
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, voff);
    vkCmdBindIndexBuffer(cmd, *reinterpret_cast<VkBuffer*>(&g_backend.index_buffer), _ibo,
        index_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);  // h144bs X17 per-draw offset
    { static long _rr=0; if(_rr<16){ fprintf(stderr,"[REALREC] frame=%llu img=%u cmd=%p inRP=%d idx=%u pipe=%p\n",(unsigned long long)g_backend.frame_count,g_backend.current_image_index,(void*)cmd,(int)g_backend.in_render_pass,index_count,(void*)pipe); _rr++; } }
    if(g_dbg_pass_w>=900u){ static long _md=0; _md++;
      const float* _mv=(const float*)vtx;
      const float* _mm=(const float*)g_backend.vertex_buffer_mapped;
      bool _copyok = (vtx && vbytes && vbytes<=g_backend.vertex_capacity && g_backend.vertex_buffer_mapped);
      // ★★�?h145k257【无上限失败计数】★★★
      //   动机：`[MAINDRAW]` 有上限（`_md<=12 || _md%1000==0 || vbytes>=1024`），
      //   无法回答"是否**有部�?*内容 draw 上传失败/越界，从而用了陈旧顶�?�?
      //   这里对每一次宽�?draw 累加，每 500 次汇总一次，给出三个失败计数�?
      { static long _wc=0,_cfail=0,_cover=0,_czero=0;
        _wc++;
        if(!_copyok) _cfail++;
        if(g_backend.vertex_buffer_mapped && (size_t)_vbo+(size_t)vbytes > (size_t)g_backend.vertex_capacity) _cover++;
        if(!vtx || !vbytes) _czero++;
        if((_wc%500)==0 || _wc<=2)
          fprintf(stderr,"[WCHK] wide#%ld copyok_fail=%ld vbo_overflow=%ld null=%ld (vb=%u vbo=%llu cap=%u)\n",
            _wc,_cfail,_cover,_czero,(unsigned)vbytes,(unsigned long long)_vbo,(unsigned)g_backend.vertex_capacity);
        fflush(stderr); }
      // h145k36【门控修正】原 `_md<=12 || (_md%1000)==0` 会把内容绘制（帧 484，_md 已数千）滤掉�?
      //   实测 22 条全�?vb=384/512 的全屏绘制。加�?vbytes>=1024 以覆盖内容几何（1536B）�?
      if(_md<=12 || (_md%1000)==0 || vbytes>=1024u){
        // h145k38【甲/乙判别】同时读"绑定偏移 _vbo �?的内容：
        //   atoff == host �?数据其实在（问题在别处）；atoff == (-1,-1,1,1) �?�?offset 未写入�?
        const float* _mo = (g_backend.vertex_buffer_mapped)
            ? (const float*)((const SceUInt8*)g_backend.vertex_buffer_mapped + (size_t)_vbo) : nullptr;
        fprintf(stderr,"[MAINDRAW] #%ld idx=%u inRP=%d passW=%u passCD=%08X copyok=%d cap=%u vb=%u vbo=%llu map=%p\n  host=(%.2f,%.2f,%.2f,%.2f) gpu0=(%.2f,%.2f,%.2f,%.2f) gpu@vbo=(%.2f,%.2f,%.2f,%.2f) pipe=%p\n",
        _md,index_count,(int)g_backend.in_render_pass,g_dbg_pass_w,g_dbg_pass_color,(int)_copyok,(unsigned)g_backend.vertex_capacity,(unsigned)vbytes,(unsigned long long)_vbo,(void*)g_backend.vertex_buffer_mapped,
        (vtx?_mv[0]:0.f),(vtx?_mv[1]:0.f),(vtx?_mv[2]:0.f),(vtx?_mv[3]:0.f),
        (_mm?_mm[0]:0.f),(_mm?_mm[1]:0.f),(_mm?_mm[2]:0.f),(_mm?_mm[3]:0.f),
        (_mo?_mo[0]:0.f),(_mo?_mo[1]:0.f),(_mo?_mo[2]:0.f),(_mo?_mo[3]:0.f),(void*)pipe); }
        if(vtx && vbytes>=128 && (_md<=24 || (_md%2000)<3)){ int _nv=(int)(vbytes/128); if(_nv>10)_nv=10;
          for(int _vi=0;_vi<_nv;_vi++){ const float*_vv=(const float*)vtx+(size_t)_vi*32;
            fprintf(stderr,"  V%d pos=(%.3f,%.3f,%.3f,%.3f) l1=(%.2f,%.2f,%.2f,%.2f) l2=(%.2f,%.2f,%.2f,%.2f)\n",_vi,_vv[0],_vv[1],_vv[2],_vv[3],_vv[4],_vv[5],_vv[6],_vv[7],_vv[8],_vv[9],_vv[10],_vv[11]); } fflush(stderr); }
        // h144k33【索引缓冲精确校验】从**实际绑定的偏�?`_ibo`** 处读回并�?host 数据比对�?
        //   旧诊断从缓冲开头读�?host≠gpu"只能说明读错位置，无法证明绑定是否正确�?
        if((index_count==6u || index_count==30u) && vtx){ static int _ck6=0,_ck30=0; int& _ck=(index_count==30u)?_ck30:_ck6; if(_ck++<4){
            const unsigned short* _mh=(const unsigned short*)idx;
            const unsigned short* _mi=(const unsigned short*)((const unsigned char*)g_backend.index_buffer_mapped + _ibo);
            unsigned _h[6]={0,0,0,0,0,0}, _g[6]={0,0,0,0,0,0}; int _eq=1;
            for(int _k=0;_k<6 && _k<(int)index_count;_k++){ _h[_k]=_mh?_mh[_k]:0; _g[_k]=_mi?_mi[_k]:0; if(_h[_k]!=_g[_k])_eq=0; }
            fprintf(stderr,"[IDXVERIFY] idx_count=%u ibytes=%u ibo=%llu eq=%d host=(%u,%u,%u,%u,%u,%u) gpu=(%u,%u,%u,%u,%u,%u)\n",
                index_count,(unsigned)ibytes,(unsigned long long)_ibo,_eq,
                _h[0],_h[1],_h[2],_h[3],_h[4],_h[5], _g[0],_g[1],_g[2],_g[3],_g[4],_g[5]);
            fflush(stderr); } }
        // h144k34【GPU 侧顶点反解】用**管线实际声明的顶点描述符**(binding0 stride=128�?
        //   location0..7 = R32G32B32A32 @ offset k*16，见本文�?1160-1162 �?，从
        //   **实际绑定的内存基址 + 实际绑定的偏�?`_vbo`** 读回顶点，与 host 打包数据对照�?
        //   此前 [MAINDRAW] �?gpu= 是从映射内存**偏移 0**读的，而每�?draw �?_vbo �?0�?
        //   所以那组对比其实一直是错的读数 —�?本诊断是第一�?按正确偏移、按管线描述�?的读数�?
        if(vtx && vbytes>=128u && g_backend.vertex_buffer_mapped){ static int _rd30=0,_rd6=0; int& _rd=(index_count==30u)?_rd30:_rd6;
          if((index_count==6u||index_count==30u) && _rd++<3){
            const unsigned char* _base=(const unsigned char*)g_backend.vertex_buffer_mapped + _vbo;
            const unsigned short* _ii=(const unsigned short*)((const unsigned char*)g_backend.index_buffer_mapped + _ibo);
            unsigned _bmax=(vbytes>2048u)?2048u:(unsigned)vbytes; int _beq=1, _bat=-1;
            for(unsigned _b=0;_b<_bmax;_b++){ if(_base[_b]!=((const unsigned char*)vtx)[_b]){_beq=0;_bat=(int)_b;break;} }
            fprintf(stderr,"[VREAD] idx_count=%u vbytes=%u ibo=%llu _vbo=%llu memcmp(host, mapped+_vbo)=%d firstdiff=%d (checked %u B)\n",
                index_count,(unsigned)vbytes,(unsigned long long)_ibo,(unsigned long long)_vbo,_beq,_bat,_bmax);
            int _nv=(int)(vbytes/128u); if(_nv>6)_nv=6;
            for(int _k=0;_k<_nv;_k++){
              unsigned _vi = _ii ? _ii[_k] : (unsigned)_k;
              const float* _hp=(const float*)((const unsigned char*)vtx + (size_t)_k*128u);
              if(((size_t)_vi*128u+16u) <= (size_t)vbytes){
                const float* _gp=(const float*)(_base + (size_t)_vi*128u);
                fprintf(stderr,"   k=%d -> index=%u  GPU loc0=(%.4f,%.4f,%.4f,%.4f) loc1/col=(%.4f,%.4f,%.4f,%.4f) loc2/uv=(%.4f,%.4f)  HOST v%d loc0=(%.4f,%.4f,%.4f,%.4f)\n",
                    _k,_vi,_gp[0],_gp[1],_gp[2],_gp[3],_gp[4],_gp[5],_gp[6],_gp[7],_gp[8],_gp[9],_k,_hp[0],_hp[1],_hp[2],_hp[3]);
              } else {
                fprintf(stderr,"   k=%d -> index=%u  **越界(需%uB, �?uB)**  HOST v%d loc0=(%.4f,%.4f,%.4f,%.4f)\n",
                    _k,_vi,(unsigned)((size_t)_vi*128u+16u),(unsigned)vbytes,_k,_hp[0],_hp[1],_hp[2],_hp[3]);
              }
            }
            fflush(stderr); } }
        // ★★�?h145k256【全量索引扫描】★★★
        //   动机：`[VREAD]` �?dump �?k=0..5 **�?6 �?*索引，�?`index_count=30`�?
        //   �?24 个索引从未被检查过。若它们越界（≥ vbytes/128 = 12），GPU 会从上传范围�?
        //   读顶�?�?位置未定�?被裁 �?**零片�?*，�?CPU 侧看到的一切（�?6 个索引、VBO�?
        //   管线、视口、scissor）全部正常。这正是"所有取证都过、却一个像素都不写"的典型形态�?
        if((index_count==30u || index_count>=6u) && idx && g_backend.index_buffer_mapped){ static int _ix=0;
          if(_ix<6){ _ix++;
            const unsigned short* _all=(const unsigned short*)((const unsigned char*)g_backend.index_buffer_mapped + _ibo);
            unsigned _nvtx=(unsigned)(vbytes/128u);
            SceUInt32 _imax=0; int _oob=0, _firstoob=-1;
            for(SceUInt32 _q=0;_q<index_count;_q++){
              unsigned _vi=_all[_q];
              if(_vi>_imax)_imax=_vi;
              if(_vi>=_nvtx){ _oob++; if(_firstoob<0)_firstoob=(int)_q; } }
            fprintf(stderr,"[IDXSCAN] idx_count=%u nvtx=%u imax=%u oob=%d firstOOB=%d | first8=",
                index_count,_nvtx,_imax,_oob,_firstoob);
            for(SceUInt32 _q=0;_q<index_count && _q<8;_q++) fprintf(stderr," %u",_all[_q]);
            fprintf(stderr,"\n"); fflush(stderr); } }
        if(index_count==6 && _md<=40){ const unsigned short* _mi=(const unsigned short*)g_backend.index_buffer_mapped;
          const unsigned short* _mh=(const unsigned short*)idx;
          fprintf(stderr,"[IDXCHK] #%ld ib=%u host=(%u,%u,%u,%u,%u,%u) gpu=(%u,%u,%u,%u,%u,%u)\n",_md,(unsigned)ibytes,
            _mh[0],_mh[1],_mh[2],_mh[3],_mh[4],_mh[5],_mi[0],_mi[1],_mi[2],_mi[3],_mi[4],_mi[5]); fflush(stderr);} }
    fprintf(stderr,"[RPCHK] draw cmd=%p rpCmd=%p match=%d passCD=%08X rpCD=%08X inRP=%d w=%u passRP=%p curRP=%p\n",(void*)cmd,(void*)g_rp_cmd_dbg,(int)(cmd==g_rp_cmd_dbg),g_dbg_pass_color,g_rp_cd_dbg,(int)g_backend.in_render_pass,(unsigned)g_dbg_pass_w,(void*)g_rp_rp_dbg,(void*)g_cur_pass_rp); fflush(stderr);
    if(index_count==3u){ const unsigned short* _i3=(const unsigned short*)g_backend.index_buffer_mapped; const unsigned short* _h3=(const unsigned short*)idx;
      fprintf(stderr,"[IDX3] ib=%u i16=%u host=(%u,%u,%u) gpu=(%u,%u,%u)\n",(unsigned)ibytes,(unsigned)index_16,
        _h3?_h3[0]:0,_h3?_h3[1]:0,_h3?_h3[2]:0, _i3?_i3[0]:0,_i3?_i3[1]:0,_i3?_i3[2]:0); fflush(stderr); }
    // h144k23【主�?pass 专项】打�?index_count==30（vert_0015）这一路实际上传的顶点�?
    //   确认「展开后的 NDC 顶点」真的进�?GPU 顶点缓冲（此�?host 侧探针只看过 guest 原始数据）�?
    if(index_count==30u && vtx){ static int _k30=0; if(_k30++<4){
        int _nv=(int)(vbytes/128u); if(_nv>10)_nv=10;
        for(int _vi=0;_vi<_nv;_vi++){ const float*_vv=(const float*)vtx+(size_t)_vi*32;
            fprintf(stderr,"[V15GPU] V%d ndc=(%.4f,%.4f,%.4f,%.4f)\n",_vi,_vv[0],_vv[1],_vv[2],_vv[3]); }
        fflush(stderr); } }
    // h144k25【链路标记实验】P5D_DBG_MARK=1：在 index_count==30（vert_0015 主力 pass）这�?draw 处，
    //   插一�?100x100 的纯�?vkCmdClearAttachments。它**不依赖顶�?索引/管线/着色器**�?
    //   只验证「命令缓�?�?render pass �?framebuffer �?那张 image �?回读」这条链路本身是否通�?
    //   · 回读�?passCD 能看到绿�?�?链路通，零像素的原因在几�?管线侧；
    //   · 看不到绿�?         �?链路本身有问题（RP/framebuffer/image 绑定）�?
    { static const char* _mk=getenv("P5D_DBG_MARK");
      // h144k30：P5D_DBG_MARK=all 时对**每一�?draw** 都插绿块（而不是只 idx==30），
      //   用来一次判定「命令到底有没有写进 image」——若连每�?draw 都插了绿块、画面仍全黑�?
      //   则问题在 framebuffer/render pass 绑定，而非某一路几何�?
      bool _doMark = false;
      if(_mk&&*_mk){ if(std::strcmp(_mk,"all")==0) _doMark=true; else if(index_count==30u) _doMark=true; }
      if(_doMark){
        VkClearAttachment _ca{}; _ca.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; _ca.colorAttachment=0;
        _ca.clearValue.color.float32[0]=0.f; _ca.clearValue.color.float32[1]=1.f;
        _ca.clearValue.color.float32[2]=0.f; _ca.clearValue.color.float32[3]=1.f;
        VkClearRect _cr{}; _cr.rect.offset={0,0}; _cr.rect.extent={100,100}; _cr.baseArrayLayer=0; _cr.layerCount=1;
        vkCmdClearAttachments(cmd,1,&_ca,1,&_cr);
        static int _mkn=0; if(_mkn++<4){ fprintf(stderr,"[MARK] green 100x100 clear inserted before idx=%u draw (passCD=%08X)\n",index_count,g_dbg_pass_color); fflush(stderr); } } }
    // 18.3599 ROOT-CAUSE FIX: this path never bound the pipeline or descriptor set before drawing!
    //   gxm_draw_arrays() calls cmd_bind_pipeline + cmd_bind_descriptor_set, but gxm_draw_indexed_real only
    //   bound vertex/index buffers and issued vkCmdDrawIndexed -> undefined/no-pixel draws (all params looked
    //   valid, RT never received pixels). Bind them here.
    cmd_bind_pipeline(cmd);
    cmd_bind_descriptor_set(cmd);
    vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);  // h144bs X11 restored for idx3
    g_draw_call_count++;
    // h144bs expE: control red draw at the real draw point using g_backend.render_pass
    if(g_dbg_pass_w>=900u){
        static VkPipeline s_ep=nullptr; static VkPipelineLayout s_epl=nullptr;
        static VkBuffer s_evb=VK_NULL_HANDLE; static VkDeviceMemory s_emem=VK_NULL_HANDLE;
        static int s_edone=0;
        VkDevice devd=*reinterpret_cast<VkDevice*>(&g_backend.device);
        VkRenderPass _rpE=g_rp_clear?g_rp_clear:*reinterpret_cast<VkRenderPass*>(&g_backend.render_pass);  // h144bs
        if(!s_ep){
            VkShaderModule evm=0,efm=0;
            create_shader_module(s_diag_passthru_vs,(SceUInt32)(sizeof(s_diag_passthru_vs)/4),&evm);
            create_shader_module(s_diag_red_fs,(SceUInt32)(sizeof(s_diag_red_fs)/4),&efm);
            VkPipelineLayoutCreateInfo pl{}; pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            vkCreatePipelineLayout(devd,&pl,nullptr,&s_epl);
            VkPipelineShaderStageCreateInfo st2[2]{};
            st2[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st2[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st2[0].module=evm;st2[0].pName="main";
            st2[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st2[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st2[1].module=efm;st2[1].pName="main";
            VkVertexInputBindingDescription vb0{}; vb0.binding=0;vb0.stride=128;vb0.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;  // h144bs H stride128
            VkVertexInputAttributeDescription va0[8]{}; for(int _ai=0;_ai<8;_ai++){va0[_ai].binding=0;va0[_ai].location=(SceUInt32)_ai;va0[_ai].format=VK_FORMAT_R32G32B32A32_SFLOAT;va0[_ai].offset=(SceUInt32)(_ai*16);}
            VkPipelineVertexInputStateCreateInfo vi2{};vi2.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;vi2.vertexBindingDescriptionCount=1;vi2.pVertexBindingDescriptions=&vb0;vi2.vertexAttributeDescriptionCount=8;vi2.pVertexAttributeDescriptions=va0;  // h144bs H 8 attrs
            VkPipelineInputAssemblyStateCreateInfo ia2{};ia2.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia2.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp2{};vp2.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp2.viewportCount=1;vp2.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo rs2{};rs2.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;rs2.polygonMode=VK_POLYGON_MODE_FILL;rs2.cullMode=VK_CULL_MODE_NONE;rs2.frontFace=VK_FRONT_FACE_CLOCKWISE;rs2.lineWidth=1.f;
            VkPipelineMultisampleStateCreateInfo ms2{};ms2.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms2.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState ca2{};ca2.colorWriteMask=0xF;
            VkPipelineColorBlendStateCreateInfo cb2{};cb2.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb2.attachmentCount=1;cb2.pAttachments=&ca2;
            VkDynamicState dn2[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy2{};dy2.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dy2.dynamicStateCount=2;dy2.pDynamicStates=dn2;
            VkGraphicsPipelineCreateInfo info2{};info2.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info2.stageCount=2;info2.pStages=st2;info2.pVertexInputState=&vi2;info2.pInputAssemblyState=&ia2;
            info2.pViewportState=&vp2;info2.pRasterizationState=&rs2;info2.pMultisampleState=&ms2;info2.pColorBlendState=&cb2;info2.pDynamicState=&dy2;
            info2.layout=s_epl;info2.renderPass=_rpE;info2.subpass=0;
            VkResult rr=vkCreateGraphicsPipelines(devd,VK_NULL_HANDLE,1,&info2,nullptr,&s_ep);
            fprintf(stderr,"[EXPEH-PIPE] rr=%d ep=%p rp=%p\n",(int)rr,(void*)s_ep,(void*)_rpE);fflush(stderr);
            if(evm)vkDestroyShaderModule(devd,evm,nullptr); if(efm)vkDestroyShaderModule(devd,efm,nullptr);
            VkBufferCreateInfo tbi{};tbi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;tbi.size=384;tbi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;tbi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;  // h144bs H 3*128
            vkCreateBuffer(devd,&tbi,nullptr,&s_evb);
            VkMemoryRequirements tmr;vkGetBufferMemoryRequirements(devd,s_evb,&tmr);
            VkMemoryAllocateInfo tai{};tai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;tai.allocationSize=tmr.size;tai.memoryTypeIndex=find_memory_type(tmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(devd,&tai,nullptr,&s_emem);vkBindBufferMemory(devd,s_evb,s_emem,0);
            void* tmp=nullptr;vkMapMemory(devd,s_emem,0,384,0,&tmp);  // h144bs H
            float tri0[96]; for(int _i=0;_i<96;_i++) tri0[_i]=0.f;
            const float posH[12]={-1.f,-1.f,0.f,1.f, 3.f,-1.f,0.f,1.f, -1.f,3.f,0.f,1.f};
            for(int _v=0;_v<3;_v++) for(int _c=0;_c<4;_c++) tri0[_v*32+_c]=posH[_v*4+_c];
            if(tmp)memcpy(tmp,tri0,sizeof(tri0));
        }
        if(s_ep && s_evb && index_count==3){  // h144bs W: FSDRAW + s_ep after idx3
            /* h144bs X3: FSDRAW-identical static-viewport stride16 pipeline at draw-HLE site */
            static VkPipeline s_xp=nullptr; static VkPipelineLayout s_xpl=nullptr;
            static VkBuffer s_xvb=VK_NULL_HANDLE; static VkDeviceMemory s_xmem=VK_NULL_HANDLE;
            if(!s_xp){
                VkShaderModule xvm=0,xfm=0;
                create_shader_module(s_diag_passthru_vs,(SceUInt32)(sizeof(s_diag_passthru_vs)/4),&xvm);
                create_shader_module(s_diag_red_fs,(SceUInt32)(sizeof(s_diag_red_fs)/4),&xfm);
                VkPipelineLayoutCreateInfo xpl0{}; xpl0.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                vkCreatePipelineLayout(devd,&xpl0,nullptr,&s_xpl);
                VkPipelineShaderStageCreateInfo xs[2]{};
                xs[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;xs[0].stage=VK_SHADER_STAGE_VERTEX_BIT;xs[0].module=xvm;xs[0].pName="main";
                xs[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;xs[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;xs[1].module=xfm;xs[1].pName="main";
                VkVertexInputBindingDescription xb0{}; xb0.binding=0;xb0.stride=16;xb0.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
                VkVertexInputAttributeDescription xa0{}; xa0.binding=0;xa0.location=0;xa0.format=VK_FORMAT_R32G32B32A32_SFLOAT;xa0.offset=0;
                VkPipelineVertexInputStateCreateInfo xvi{};xvi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;xvi.vertexBindingDescriptionCount=1;xvi.pVertexBindingDescriptions=&xb0;xvi.vertexAttributeDescriptionCount=1;xvi.pVertexAttributeDescriptions=&xa0;
                VkPipelineInputAssemblyStateCreateInfo xia{};xia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;xia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkViewport xvp{0.f,0.f,(float)g_dbg_pass_w,(float)g_dbg_pass_h,0.f,1.f}; VkRect2D xsc{{0,0},{g_dbg_pass_w,g_dbg_pass_h}};
                VkPipelineViewportStateCreateInfo xvpst{};xvpst.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;xvpst.viewportCount=1;xvpst.scissorCount=1;xvpst.pViewports=&xvp;xvpst.pScissors=&xsc;
                VkPipelineRasterizationStateCreateInfo xrs{};xrs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;xrs.polygonMode=VK_POLYGON_MODE_FILL;xrs.cullMode=VK_CULL_MODE_NONE;xrs.frontFace=VK_FRONT_FACE_CLOCKWISE;xrs.lineWidth=1.f;
                VkPipelineMultisampleStateCreateInfo xms{};xms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;xms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
                VkPipelineColorBlendAttachmentState xca{};xca.colorWriteMask=0xF;
                VkPipelineColorBlendStateCreateInfo xcb{};xcb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;xcb.attachmentCount=1;xcb.pAttachments=&xca;
                VkGraphicsPipelineCreateInfo xinfo{};xinfo.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                xinfo.stageCount=2;xinfo.pStages=xs;xinfo.pVertexInputState=&xvi;xinfo.pInputAssemblyState=&xia;xinfo.pViewportState=&xvpst;xinfo.pRasterizationState=&xrs;xinfo.pMultisampleState=&xms;xinfo.pColorBlendState=&xcb;xinfo.pDynamicState=nullptr;
                xinfo.layout=s_xpl;xinfo.renderPass=_rpE;xinfo.subpass=0;
                VkResult xrr=vkCreateGraphicsPipelines(devd,VK_NULL_HANDLE,1,&xinfo,nullptr,&s_xp);
                fprintf(stderr,"[X3PIPE] rr=%d xp=%p rp=%p w=%u\n",(int)xrr,(void*)s_xp,(void*)_rpE,(unsigned)g_dbg_pass_w);fflush(stderr);
                if(xvm)vkDestroyShaderModule(devd,xvm,nullptr); if(xfm)vkDestroyShaderModule(devd,xfm,nullptr);
                VkBufferCreateInfo xtbi{};xtbi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;xtbi.size=48;xtbi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;xtbi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
                vkCreateBuffer(devd,&xtbi,nullptr,&s_xvb);
                VkMemoryRequirements xtmr;vkGetBufferMemoryRequirements(devd,s_xvb,&xtmr);
                VkMemoryAllocateInfo xtai{};xtai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;xtai.allocationSize=xtmr.size;xtai.memoryTypeIndex=find_memory_type(xtmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(devd,&xtai,nullptr,&s_xmem);vkBindBufferMemory(devd,s_xvb,s_xmem,0);
                void* xtmp=nullptr;vkMapMemory(devd,s_xmem,0,48,0,&xtmp);
                float xtri[12]={-1.f,-1.f,0.f,1.f, 3.f,-1.f,0.f,1.f, -1.f,3.f,0.f,1.f};
                if(xtmp)memcpy(xtmp,xtri,sizeof(xtri));
            }
            if(false && s_xp && s_xvb){  /* h144bs X11: X10 diagnostic red draw disabled */
                VkDeviceSize _xo=0;
                vkCmdBindVertexBuffers(cmd,0,1,&s_xvb,&_xo);
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,s_xp);
                vkCmdDraw(cmd,3,1,0,0);
                if(s_edone<8){ fprintf(stderr,"[X10DRAW] static red vkCmdDraw at idx3 cmd=%p w=%u n=%d inRP=%d xp=%p\n",(void*)cmd,g_dbg_pass_w,s_edone,(int)g_backend.in_render_pass,(void*)s_xp); fflush(stderr); }
                s_edone++;
            }
            cmd_bind_pipeline(cmd);
            VkBuffer rvbuf=*reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer); VkDeviceSize roff[1]={0};
            vkCmdBindVertexBuffers(cmd,0,1,&rvbuf,roff);
        }
    }
}


// ===================== h144n/o 离屏 RT �?+ �?pass 单帧 =====================
struct OffscreenRT {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fbClear = VK_NULL_HANDLE, fbLoad = VK_NULL_HANDLE;
    SceUInt32 w = 0, h = 0;
    SceUInt32 layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // h144k26【采样快照副本】同一张面既要被渲染、又要被当纹理采样时（frag_0003 �?out=PA0+PA1+PA2 自反馈求和�?
    //   vert_0006 系全屏合成），Vulkan **不允�?*采样处于 COLOR_ATTACHMENT_OPTIMAL �?image�?
    //   原实现遇到这种情况只�?[ALIAS-SKIP] 然后什么都不做 �?采到未定义内�?�?满屏随机小方块噪声（实测）�?
    //   方案：每�?RT 额外持有一份只读副�?aliasImg；begin_scene_pass �?*开始渲染之�?*（layout 还是
    //   SHADER_READ_ONLY，内容有效）把它 copy �?alias，之后所�?RT→纹理采样一律走 alias�?
    //   alias 永远停在 SHADER_READ_ONLY，layout 恒定合法�?
    VkImage aliasImg = VK_NULL_HANDLE;
    VkDeviceMemory aliasMem = VK_NULL_HANDLE;
    VkImageView aliasView = VK_NULL_HANDLE;
    SceUInt32 aliasLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};
static std::map<SceUInt32, OffscreenRT> g_rt_pool;
static bool g_rt_rp_ready = false;  // g_rp_clear/g_rp_load moved up (h144bs)
static bool g_frame_open = false;       // 本帧�?acquire + begin command buffer
static SceUInt32 g_pass_color = 0;      // 当前 pass 输出 colorData
static SceUInt32 g_frame_cleared[32]; static int g_frame_cleared_n = 0;

static SceInt32 create_offscreen_render_passes() {
    if (g_rt_rp_ready) return 0;
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkAttachmentDescription a{};
    a.format = (VkFormat)g_backend.swapchain_format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference aref{}; aref.attachment = 0; aref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &aref;
    VkSubpassDependency dep{}; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1; rp.pAttachments = &a; rp.subpassCount = 1; rp.pSubpasses = &sub;
    rp.dependencyCount = 1; rp.pDependencies = &dep;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (vkCreateRenderPass(dev, &rp, nullptr, &g_rp_clear) != VK_SUCCESS) { LOGE("rt rp_clear fail"); return -1; }
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (vkCreateRenderPass(dev, &rp, nullptr, &g_rp_load) != VK_SUCCESS) { LOGE("rt rp_load fail"); return -1; }
    g_rt_rp_ready = true;
    return 0;
}

static OffscreenRT* get_rt(SceUInt32 cd, SceUInt32 w, SceUInt32 h) {
    if (!cd || !w || !h) return nullptr;
    auto it = g_rt_pool.find(cd);
    if (it != g_rt_pool.end()) {
        if (it->second.w == w && it->second.h == h) return &it->second;
        VkDevice dev0 = *reinterpret_cast<VkDevice*>(&g_backend.device);
        if (it->second.fbClear) vkDestroyFramebuffer(dev0, it->second.fbClear, nullptr);
        if (it->second.fbLoad) vkDestroyFramebuffer(dev0, it->second.fbLoad, nullptr);
        if (it->second.view) vkDestroyImageView(dev0, it->second.view, nullptr);
        if (it->second.img) vkDestroyImage(dev0, it->second.img, nullptr);
        if (it->second.mem) vkFreeMemory(dev0, it->second.mem, nullptr);
        if (it->second.aliasView) vkDestroyImageView(dev0, it->second.aliasView, nullptr);
        if (it->second.aliasImg) vkDestroyImage(dev0, it->second.aliasImg, nullptr);
        if (it->second.aliasMem) vkFreeMemory(dev0, it->second.aliasMem, nullptr);
        g_rt_pool.erase(it);
    }
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    OffscreenRT r; r.w = w; r.h = h;
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = (VkFormat)g_backend.swapchain_format;
    ii.extent = { w, h, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &r.img) != VK_SUCCESS) { LOGE("rt image fail cd=%08X", cd); return nullptr; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.img, &mr);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &r.mem) != VK_SUCCESS) {
        ai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, 0);
        if (vkAllocateMemory(dev, &ai, nullptr, &r.mem) != VK_SUCCESS) return nullptr;
    }
    vkBindImageMemory(dev, r.img, r.mem, 0);
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = r.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = (VkFormat)g_backend.swapchain_format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(dev, &vi, nullptr, &r.view) != VK_SUCCESS) return nullptr;
    VkFramebufferCreateInfo fb{}; fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.attachmentCount = 1; fb.pAttachments = &r.view; fb.width = w; fb.height = h; fb.layers = 1;
    fb.renderPass = g_rp_clear; if (vkCreateFramebuffer(dev, &fb, nullptr, &r.fbClear) != VK_SUCCESS) return nullptr;
    fb.renderPass = g_rp_load;  if (vkCreateFramebuffer(dev, &fb, nullptr, &r.fbLoad) != VK_SUCCESS) return nullptr;
    // h144k26：为采样快照�?alias image（只读用途：SAMPLED + TRANSFER_DST，不做颜色附件）
    { VkImageCreateInfo xi = ii; xi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      if (vkCreateImage(dev, &xi, nullptr, &r.aliasImg) == VK_SUCCESS) {
        VkMemoryRequirements xr; vkGetImageMemoryRequirements(dev, r.aliasImg, &xr);
        VkMemoryAllocateInfo xa{}; xa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; xa.allocationSize = xr.size;
        xa.memoryTypeIndex = find_memory_type(xr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev, &xa, nullptr, &r.aliasMem) != VK_SUCCESS) {
            xa.memoryTypeIndex = find_memory_type(xr.memoryTypeBits, 0);
            vkAllocateMemory(dev, &xa, nullptr, &r.aliasMem);
        }
        if (r.aliasMem) vkBindImageMemory(dev, r.aliasImg, r.aliasMem, 0);
        VkImageViewCreateInfo xv{}; xv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        xv.image = r.aliasImg; xv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        xv.format = (VkFormat)g_backend.swapchain_format;
        xv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &xv, nullptr, &r.aliasView) != VK_SUCCESS) r.aliasView = VK_NULL_HANDLE;
        fprintf(stderr, "[RTALIAS] cd=%08X alias img=%p view=%p\n", cd, (void*)r.aliasImg, (void*)r.aliasView);
      } }
    auto ins = g_rt_pool.emplace(cd, r);
    // �?h145k525：同时打 stdout。这条是"哪个地址被建�?RT"的唯一直接证据——过去只�?stderr�?
    //   于是"引擎渲染的面"�?我们 present 的面"是不是同一张，一直无法直接比对�?
    printf("[RTPOOL] create cd=%08X %ux%u pool=%zu\n", cd, w, h, g_rt_pool.size());
    fprintf(stderr, "[RTPOOL] create cd=%08X %ux%u pool=%zu\n", cd, w, h, g_rt_pool.size());
    return &ins.first->second;
}

// ★★�?h145k525【整池亮度普查】★★★
//   动机：P5D_RTMEAN 原来硬编�?5 �?cd（A0F/A13/A17/A0E1D/AE10），实测**一个都不在池里**
//   �?save_rt_screenshot 早退、[RTSTAT] 一行都拿不到，白跑一轮�?
//   改为直接遍历池子：引擎到底把画面画进了哪张面，池子里有什么就回读什么，不再靠猜�?
SceInt32 rt_pool_probe(int maxn){
    int n=0;
    for(std::map<SceUInt32,OffscreenRT>::iterator _it=g_rt_pool.begin(); _it!=g_rt_pool.end(); ++_it){
        if(n>=maxn) break;
        printf("[RTPOOL] #%d cd=%08X %ux%u\n", n, (unsigned)_it->first,
               (unsigned)_it->second.w, (unsigned)_it->second.h);
        if(_it->second.img && _it->second.w && _it->second.h) save_rt_screenshot(_it->first, "nul");
        n++;
    }
    if(!n) printf("[RTPOOL] 池为空\n");
    fflush(stdout);
    return n;
}

// ===== h144qw2-j 诊断：按 colorData 回读离屏 RT �?BMP =====
// RT image �?DEVICE_LOCAL，需 host-visible staging buffer + vkCmdCopyImageToBuffer�?
// 调用点须�?present_frame �?submit 之后（内�?vkQueueWaitIdle 保证渲染已落地）�?
SceInt32 save_rt_screenshot(SceUInt32 cd, const char* filename) {
    auto it = g_rt_pool.find(cd);
    if (it == g_rt_pool.end()) {
        // �?h145k525：早退提示同时�?stdout。原来只�?stderr（默�?NUL）⇒ P5D_RTMEAN 硬编码的
        //   那几�?cd 若不在池里，�?*一行证据都看不�?*（表现为 [RTSTAT] 0 行），白跑一轮�?
        printf("[RTSHOT] cd=%08X 不在 RT 池（池内 %d 张）\n", cd, (int)g_rt_pool.size());
        fprintf(stderr, "[RTSHOT] cd=%08X 不在 RT 池\n", cd); fflush(stdout); fflush(stderr); return -1; }
    OffscreenRT& rt = it->second;
    if (!rt.img || !rt.w || !rt.h) {
        printf("[RTSHOT] cd=%08X img/w/h 无效\n", cd);
        fprintf(stderr, "[RTSHOT] cd=%08X img/w/h 无效\n", cd); fflush(stdout); fflush(stderr); return -2; }
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkQueue queue = *reinterpret_cast<VkQueue*>(&g_backend.graphics_queue);
    VkCommandPool cpool = *reinterpret_cast<VkCommandPool*>(&g_backend.command_pool);
    vkQueueWaitIdle(queue);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) return -3;
    VkCommandBufferBeginInfo cbi{}; cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &cbi) != VK_SUCCESS) { vkFreeCommandBuffers(dev, cpool, 1, &cmd); return -4; }

    // RT 当前 layout -> TRANSFER_SRC（layout �?begin_scene_pass/present_frame 跟踪�?
    VkImageMemoryBarrier b1{}; b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = (VkImageLayout)rt.layout;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = rt.img; b1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);
    rt.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // host-visible staging buffer
    VkDeviceSize sz = (VkDeviceSize)rt.w * (VkDeviceSize)rt.h * 4u;
    VkBuffer sb = VK_NULL_HANDLE; VkDeviceMemory sm = VK_NULL_HANDLE;
    VkBufferCreateInfo sbi{}; sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbi.size = sz; sbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &sbi, nullptr, &sb) != VK_SUCCESS) {
        vkFreeCommandBuffers(dev, cpool, 1, &cmd); return -5; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, sb, &mr);
    VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &mai, nullptr, &sm) != VK_SUCCESS) {
        vkDestroyBuffer(dev, sb, nullptr); vkFreeCommandBuffers(dev, cpool, 1, &cmd); return -6; }
    vkBindBufferMemory(dev, sb, sm, 0);

    VkBufferImageCopy rgn{}; rgn.bufferOffset = 0; rgn.bufferRowLength = 0; rgn.bufferImageHeight = 0;
    rgn.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    rgn.imageOffset = { 0, 0, 0 }; rgn.imageExtent = { rt.w, rt.h, 1 };
    vkCmdCopyImageToBuffer(cmd, rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &rgn);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFence f = VK_NULL_HANDLE; VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev, &fi, nullptr, &f);
    vkQueueSubmit(queue, 1, &si, f);
    vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ULL);
    vkDestroyFence(dev, f, nullptr);
    vkFreeCommandBuffers(dev, cpool, 1, &cmd);

    void* data = nullptr;
    if (vkMapMemory(dev, sm, 0, sz, 0, &data) != VK_SUCCESS || !data) {
        vkDestroyBuffer(dev, sb, nullptr); vkFreeMemory(dev, sm, nullptr); return -7; }

    // �?24bit BMP（B8G8R8A8 -> BGR，底向上），�?save_screenshot 同规�?
    SceUInt32 w = rt.w, h = rt.h;
    SceUInt32 bmp_pitch = ((w * 3 + 3) / 4) * 4;
    SceUInt32 pix_size = bmp_pitch * h;
    // h145k377【同�?A/B 需要的 RT 亮度统计】与 [SHOTSTAT] 同一口径（staging buffer 紧密打包，行�?= w*4）�?
    //   用途：在同一帧里同时看到 swapchain 回读�?引擎自己�?RT"回读�?max/mean�?
    //   一次判�?内容到底有没有画�?RT / 有没有进 swapchain"，不必再�?run 猜�?
    { unsigned _rxs = 0; double _rss = 0.0; unsigned long long _rsn = 0;
      for (SceUInt32 _y = 0; _y < h; _y += 2) {
          unsigned char* _sr = (unsigned char*)data + (size_t)_y * w * 4u;
          for (SceUInt32 _x = 0; _x < w; _x += 2) {
              unsigned _b0 = _sr[_x * 4 + 0], _g0 = _sr[_x * 4 + 1], _r0 = _sr[_x * 4 + 2];
              unsigned _m0 = _b0; if (_g0 > _m0) _m0 = _g0; if (_r0 > _m0) _m0 = _r0;
              if (_m0 > _rxs) _rxs = _m0; _rss += _m0; _rsn++; } }
      static long _rsc = 0; if (_rsc < 200000) { _rsc++;
        // �?h145k524【RTSTAT 同时�?stdout】原实现只打 stderr，�?stderr 默认�?freopen �?NUL
        //   �?想看 RT 亮度就必须开 P5D_VERBOSE=1，�?VERBOSE 会放出大�?fprintf(stderr) 探针�?
        //   运行拖慢（过去因此无法在"序列/logo 阶段"�?RT 回读判决）。解释器提�?7.4× 后仍有预算，
        //   但没必要付这个代价：这里同时打到 stdout，stdout 恒有�?
        // �?h145k564：补打帧�?`fc=`。此�?`[RTSTAT]` 只有 cd/max/mean�?
        //   �?`ONLY_FH=frag_0084` �?174 次采样里只有 2 �?mean=3.02 �?无法知道它落在哪一帧，
        //   也就无法回看那一帧的窗口/RT 图。加帧号后即可精确定�?内容一闪而过"的时刻�?
        printf("[RTSTAT] fc=%llu cd=%08X %ux%u max=%u mean=%.2f file=%s\n",
            (unsigned long long)g_backend.frame_count, cd, w, h, _rxs,
            _rsn ? (_rss / (double)_rsn) : 0.0, filename ? filename : "-");
        fprintf(stderr, "[RTSTAT] cd=%08X %ux%u max=%u mean=%.2f file=%s\n", cd, w, h, _rxs,
            _rsn ? (_rss / (double)_rsn) : 0.0, filename ? filename : "-"); fflush(stdout); fflush(stderr); } }
    FILE* fp = fopen(filename, "wb");
    if (fp) {
        SceUInt16 sig = 0x4D42; SceUInt32 fsize = 54 + pix_size, resv = 0, doff = 54;
        fwrite(&sig, 2, 1, fp); fwrite(&fsize, 4, 1, fp); fwrite(&resv, 4, 1, fp); fwrite(&doff, 4, 1, fp);
        SceUInt32 hs = 40; SceInt32 bw = (SceInt32)w, bh = (SceInt32)h; SceUInt16 pl = 1, bpp = 24;
        SceUInt32 comp = 0, isz = pix_size; SceInt32 xp = 2835, yp = 2835; SceUInt32 cu = 0, ci = 0;
        fwrite(&hs, 4, 1, fp); fwrite(&bw, 4, 1, fp); fwrite(&bh, 4, 1, fp); fwrite(&pl, 2, 1, fp);
        fwrite(&bpp, 2, 1, fp); fwrite(&comp, 4, 1, fp); fwrite(&isz, 4, 1, fp);
        fwrite(&xp, 4, 1, fp); fwrite(&yp, 4, 1, fp); fwrite(&cu, 4, 1, fp); fwrite(&ci, 4, 1, fp);
        unsigned char* row = new unsigned char[bmp_pitch];
        const unsigned char* src = (const unsigned char*)data;
        for (SceUInt32 y = 0; y < h; y++) {
            const unsigned char* srow = src + (size_t)(h - 1 - y) * (size_t)w * 4u;
            for (SceUInt32 x = 0; x < w; x++) {
                row[x * 3 + 0] = srow[x * 4 + 0];  // B
                row[x * 3 + 1] = srow[x * 4 + 1];  // G
                row[x * 3 + 2] = srow[x * 4 + 2];  // R
            }
            fwrite(row, 1, bmp_pitch, fp);
        }
        delete[] row;
        fclose(fp);
        // 统计�?4 �?+ 中心纹素，便于日志直接判读（无需外部脚本�?
        auto px = [&](SceUInt32 x, SceUInt32 y, unsigned& r, unsigned& g, unsigned& b) {
            const unsigned char* p = src + ((size_t)y * w + x) * 4u;
            b = p[0]; g = p[1]; r = p[2];
        };
        unsigned r, g, b; long distinct = 0; unsigned lastr = 0xFFFFFFFFu, lastg = 0xFFFFFFFFu, lastb = 0xFFFFFFFFu;
        for (SceUInt32 y = 0; y < h; y += 2) for (SceUInt32 x = 0; x < w; x += 2) {
            px(x, y, r, g, b);
            if (r != lastr || g != lastg || b != lastb) { distinct++; lastr = r; lastg = g; lastb = b; }
        }
        fprintf(stderr, "[RTSHOT] cd=%08X img=%p view=%p %ux%u -> %s", cd, (void*)rt.img, (void*)rt.view, w, h, filename);
        px(0, 0, r, g, b);                    fprintf(stderr, "  tl=(%u,%u,%u)", r, g, b);
        px(w - 1, 0, r, g, b);                fprintf(stderr, " tr=(%u,%u,%u)", r, g, b);
        px(0, h - 1, r, g, b);                fprintf(stderr, " bl=(%u,%u,%u)", r, g, b);
        px(w - 1, h - 1, r, g, b);            fprintf(stderr, " br=(%u,%u,%u)", r, g, b);
        px(w / 2, h / 2, r, g, b);            fprintf(stderr, " center=(%u,%u,%u)", r, g, b);
        fprintf(stderr, " 颜色变化次数=%ld\n", distinct);
        fflush(stderr);
    }
    vkUnmapMemory(dev, sm);
    vkDestroyBuffer(dev, sb, nullptr);
    vkFreeMemory(dev, sm, nullptr);
    return fp ? 0 : -8;
}

static const SceUInt32 s_fs_vert[] = {
    0x07230203,0x00010000,0x00000000,0x0000000D,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0007000F,0x00000000,0x00000001,0x6E69616D,0x00000000,0x00000002,0x00000003,
    0x00040047,0x00000002,0x0000000B,0x00000000,
    0x00040047,0x00000003,0x0000001E,0x00000000,
    0x00020013,0x00000004,
    0x00030021,0x00000005,0x00000004,
    0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000004,
    0x00040020,0x00000008,0x00000003,0x00000007,
    0x00040020,0x00000009,0x00000001,0x00000007,
    0x0004003B,0x00000008,0x00000002,0x00000003,
    0x0004003B,0x00000009,0x00000003,0x00000001,
    0x00050036,0x00000004,0x00000001,0x00000000,0x00000005,
    0x000200F8,0x0000000A,
    0x0004003D,0x00000007,0x0000000B,0x00000003,
    0x0003003E,0x00000002,0x0000000B,
    0x000100FD,
    0x00010038,
};
static const SceUInt32 s_fs_frag[] = {
    0x07230203,0x00010000,0x00000000,0x0000000E,0x00000000,
    0x00020011,0x00000001,
    0x0003000E,0x00000000,0x00000001,
    0x0006000F,0x00000004,0x00000001,0x6E69616D,0x00000000,0x00000002,
    0x00030010,0x00000001,0x00000007,  // h144au OpExecutionMode OriginUpperLeft
    0x00040047,0x00000002,0x0000001E,0x00000000,
    0x00020013,0x00000003,
    0x00030021,0x00000004,0x00000003,
    0x00030016,0x00000005,0x00000020,
    0x00040017,0x00000006,0x00000005,0x00000004,
    0x00040020,0x00000007,0x00000003,0x00000006,
    0x0004003B,0x00000007,0x00000002,0x00000003,
    0x0004002B,0x00000005,0x00000008,0x3F800000,
    0x0004002B,0x00000005,0x00000009,0x00000000,
    0x0007002C,0x00000006,0x0000000A,0x00000008,0x00000009,0x00000009,0x00000008,
    0x00050036,0x00000003,0x00000001,0x00000000,0x00000004,
    0x000200F8,0x0000000B,
    0x0003003E,0x00000002,0x0000000A,
    0x000100FD,
    0x00010038,
};

// h144ag 单线程离�?draw 自测（首次帧内、device/swapchain 就绪后跑一次，独立 submit+waitIdle+回读�?
static void offscreen_draw_selftest(){
    static int _ran=0; if(_ran) return;
    if(!g_backend.initialized) return;
    _ran=1;
    VkDevice dev=*reinterpret_cast<VkDevice*>(&g_backend.device);
    fprintf(stderr,"[SELFTEST] tid=%lu dev=%p\n",(unsigned long)GetCurrentThreadId(),(void*)dev); fflush(stderr);
    vulkan_layer_probe(dev,"frame",(const uint32_t*)s_fs_vert,sizeof(s_fs_vert)/4,(const uint32_t*)s_fs_frag,sizeof(s_fs_frag)/4);
    if(create_offscreen_render_passes()!=0){ fprintf(stderr,"[SELFTEST] rp create fail\n");fflush(stderr); return; }
    const SceUInt32 W=64,H=64;
    VkImage img=VK_NULL_HANDLE; VkDeviceMemory imem=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; VkFramebuffer fb=VK_NULL_HANDLE;
    VkImageCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType=VK_IMAGE_TYPE_2D;
    ii.format=(VkFormat)g_backend.swapchain_format; ii.extent={W,H,1}; ii.mipLevels=1; ii.arrayLayers=1;
    ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(dev,&ii,nullptr,&img)!=VK_SUCCESS){ fprintf(stderr,"[SELFTEST] img fail\n");fflush(stderr); return; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
    VkMemoryAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize=mr.size;
    ai.memoryTypeIndex=find_memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(dev,&ai,nullptr,&imem)!=VK_SUCCESS){ ai.memoryTypeIndex=find_memory_type(mr.memoryTypeBits,0); vkAllocateMemory(dev,&ai,nullptr,&imem); }
    vkBindImageMemory(dev,img,imem,0);
    VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image=img;
    vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=(VkFormat)g_backend.swapchain_format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCreateImageView(dev,&vi,nullptr,&view);
    VkFramebufferCreateInfo fbi{}; fbi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.attachmentCount=1; fbi.pAttachments=&view; fbi.width=W; fbi.height=H; fbi.layers=1; fbi.renderPass=g_rp_clear;
    if(vkCreateFramebuffer(dev,&fbi,nullptr,&fb)!=VK_SUCCESS){ fprintf(stderr,"[SELFTEST] fb fail\n");fflush(stderr); return; }

    VkBuffer vb=VK_NULL_HANDLE; VkDeviceMemory vmem=VK_NULL_HANDLE; void* vmap=nullptr;
    VkBufferCreateInfo bci{}; bci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size=48;
    bci.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(dev,&bci,nullptr,&vb);
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev,vb,&bmr);
    VkMemoryAllocateInfo bai{}; bai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; bai.allocationSize=bmr.size;
    bai.memoryTypeIndex=find_memory_type(bmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev,&bai,nullptr,&vmem); vkBindBufferMemory(dev,vb,vmem,0);
    vkMapMemory(dev,vmem,0,48,0,&vmap);
    { float tri[12]={-1.f,-1.f,0.f,1.f, 3.f,-1.f,0.f,1.f, -1.f,3.f,0.f,1.f}; if(vmap) memcpy(vmap,tri,sizeof(tri)); }

    VkShaderModule vm=VK_NULL_HANDLE,fm=VK_NULL_HANDLE;
    int rvm=create_shader_module(s_fs_vert,(SceUInt32)(sizeof(s_fs_vert)/4),&vm);
    int rfm=create_shader_module(s_fs_frag,(SceUInt32)(sizeof(s_fs_frag)/4),&fm);
    fprintf(stderr,"[SELFTEST] module create rvm=%d vm=%p rfm=%d fm=%p sizeof(vsh)=%zu fsh=%zu\n",rvm,(void*)vm,rfm,(void*)fm,sizeof(s_fs_vert),sizeof(s_fs_frag)); fflush(stderr);
    VkShaderModule dvm=VK_NULL_HANDLE, dfm=VK_NULL_HANDLE;
    VkShaderModule dvm2=VK_NULL_HANDLE, dfm2=VK_NULL_HANDLE;
    {
      VkShaderModuleCreateInfo qv{}; qv.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; qv.codeSize=sizeof(s_fs_vert); qv.pCode=s_fs_vert;
      VkResult rv2=vkCreateShaderModule(dev,&qv,nullptr,&dvm);
      VkShaderModuleCreateInfo qf{}; qf.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; qf.codeSize=sizeof(s_fs_frag); qf.pCode=s_fs_frag;
      VkResult rf2=vkCreateShaderModule(dev,&qf,nullptr,&dfm);
      fprintf(stderr,"[SELFTEST] DIRECT VS rr=%d h=%p  FS rr=%d h=%p (dev=%p)\n",(int)rv2,(void*)dvm,(int)rf2,(void*)dfm,(void*)dev); fflush(stderr);
    vulkan_layer_probe(dev,"mid",(const uint32_t*)s_fs_vert,sizeof(s_fs_vert)/4,(const uint32_t*)s_fs_frag,sizeof(s_fs_frag)/4,&dvm2,&dfm2);
      fprintf(stderr,"[SELFTEST] compare image-class handles: img=%p vb=%p fb=%p rp=%p\n",(void*)img,(void*)vb,(void*)fb,(void*)g_rp_clear); fflush(stderr); fflush(stderr);
    }
    VkPipelineLayout pl=VK_NULL_HANDLE; VkPipelineLayoutCreateInfo plc{}; plc.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    vkCreatePipelineLayout(dev,&plc,nullptr,&pl);
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[0].stage=VK_SHADER_STAGE_VERTEX_BIT; st[0].module=dvm2?dvm2:dvm; st[0].pName="main";
    st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module=dfm2?dfm2:dfm; st[1].pName="main";
    VkVertexInputBindingDescription vbd{}; vbd.binding=0; vbd.stride=16; vbd.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vad{}; vad.binding=0; vad.location=0; vad.format=VK_FORMAT_R32G32B32A32_SFLOAT; vad.offset=0;
    VkPipelineVertexInputStateCreateInfo vis{}; vis.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount=1; vis.pVertexBindingDescriptions=&vbd; vis.vertexAttributeDescriptionCount=1; vis.pVertexAttributeDescriptions=&vad;
    VkPipelineInputAssemblyStateCreateInfo ias{}; ias.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ias.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{}; vps.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount=1; vps.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_CLOCKWISE; rs.lineWidth=1.f;
    VkPipelineMultisampleStateCreateInfo mss{}; mss.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; mss.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask=0xF;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount=1; cbs.pAttachments=&cba;
    VkDynamicState dn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dys{}; dys.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dys.dynamicStateCount=2; dys.pDynamicStates=dn;
    VkGraphicsPipelineCreateInfo gp{}; gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount=2; gp.pStages=st; gp.pVertexInputState=&vis; gp.pInputAssemblyState=&ias; gp.pViewportState=&vps;
    gp.pRasterizationState=&rs; gp.pMultisampleState=&mss; gp.pColorBlendState=&cbs; gp.pDynamicState=&dys;
    gp.layout=pl; gp.renderPass=g_rp_clear; gp.subpass=0;
    VkPipeline pipe=VK_NULL_HANDLE; VkResult prr=vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gp,nullptr,&pipe);
    if(vm)vkDestroyShaderModule(dev,vm,nullptr); if(fm)vkDestroyShaderModule(dev,fm,nullptr);
    if(dvm)vkDestroyShaderModule(dev,dvm,nullptr); if(dfm)vkDestroyShaderModule(dev,dfm,nullptr);
    if(prr!=VK_SUCCESS||!pipe){ fprintf(stderr,"[SELFTEST] pipeline fail rr=%d\n",(int)prr);fflush(stderr); return; }

    VkBuffer stb=VK_NULL_HANDLE; VkDeviceMemory smem=VK_NULL_HANDLE; void* smap=nullptr;
    VkDeviceSize sz=(VkDeviceSize)W*H*4;
    VkBufferCreateInfo sci{}; sci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; sci.size=sz;
    sci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; sci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(dev,&sci,nullptr,&stb);
    VkMemoryRequirements smr; vkGetBufferMemoryRequirements(dev,stb,&smr);
    VkMemoryAllocateInfo sai{}; sai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; sai.allocationSize=smr.size;
    sai.memoryTypeIndex=find_memory_type(smr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev,&sai,nullptr,&smem); vkBindBufferMemory(dev,stb,smem,0);

    VkCommandBuffer cmd=begin_command_buffer();
    if(!cmd){ fprintf(stderr,"[SELFTEST] no cmd\n");fflush(stderr); return; }
    VkImageMemoryBarrier b1{}; b1.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b1.image=img;
    b1.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b1.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b1.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b1.srcAccessMask=0; b1.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&b1);
    VkClearValue cv{}; cv.color.float32[0]=0.f; cv.color.float32[1]=1.f; cv.color.float32[2]=0.f; cv.color.float32[3]=1.f;
    VkRenderPassBeginInfo rpi{}; rpi.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass=g_rp_clear; rpi.framebuffer=fb; rpi.renderArea={{0,0},{W,H}}; rpi.clearValueCount=1; rpi.pClearValues=&cv;
    vkCmdBeginRenderPass(cmd,&rpi,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    VkDeviceSize voff=0; vkCmdBindVertexBuffers(cmd,0,1,&vb,&voff);
    cmd_set_viewport(cmd,0,0.f,(SceFloat)W,(SceFloat)H); cmd_set_scissor(cmd,0,0,W,H);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRenderPass(cmd);
    VkImageMemoryBarrier b2{}; b2.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b2.image=img;
    b2.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b2.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b2.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b2.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    b2.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b2);
    VkBufferImageCopy br{}; br.bufferRowLength=0; br.bufferImageHeight=0;
    br.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; br.imageOffset={0,0,0}; br.imageExtent={W,H,1};
    vkCmdCopyImageToBuffer(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stb,1,&br);
    end_command_buffer(cmd); submit_command_buffer(cmd);

    vkMapMemory(dev,smem,0,smr.size,0,&smap);
    long nred=0,ngreen=0,nother=0;
    if(smap){
        const unsigned char* px=(const unsigned char*)smap;
        for(SceUInt32 i=0;i<W*H;i++){
            unsigned char b0=px[i*4+0],b1=px[i*4+1],b2=px[i*4+2];
            bool g=(b1>200 && b0<80 && b2<80);
            bool r=(b1<80 && (b0>200 || b2>200));
            if(g) ngreen++; else if(r) nred++; else nother++;
        }
        vkUnmapMemory(dev,smem);
    }
    fprintf(stderr,"[SELFTEST] total=%u red=%ld green=%ld other=%ld (red>0 means offscreen vkCmdDraw WORKS)\n",
        (unsigned)(W*H),nred,ngreen,nother); fflush(stderr);

    // ===== B 组：全新标准离屏 RP（initial UNDEFINED / final TRANSFER_SRC / 双向 dependency�?====
    {
        VkImage imgB=VK_NULL_HANDLE; VkDeviceMemory memB=VK_NULL_HANDLE; VkImageView viewB=VK_NULL_HANDLE; VkFramebuffer fbB=VK_NULL_HANDLE;
        VkImageCreateInfo ib{}; ib.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ib.imageType=VK_IMAGE_TYPE_2D;
        ib.format=(VkFormat)g_backend.swapchain_format; ib.extent={W,H,1}; ib.mipLevels=1; ib.arrayLayers=1;
        ib.samples=VK_SAMPLE_COUNT_1_BIT; ib.tiling=VK_IMAGE_TILING_OPTIMAL;
        ib.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ib.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ib.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(dev,&ib,nullptr,&imgB);
        VkMemoryRequirements mrb; vkGetImageMemoryRequirements(dev,imgB,&mrb);
        VkMemoryAllocateInfo aib{}; aib.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; aib.allocationSize=mrb.size;
        aib.memoryTypeIndex=find_memory_type(mrb.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev,&aib,nullptr,&memB); vkBindImageMemory(dev,imgB,memB,0);
        VkImageViewCreateInfo vb2{}; vb2.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vb2.image=imgB;
        vb2.viewType=VK_IMAGE_VIEW_TYPE_2D; vb2.format=(VkFormat)g_backend.swapchain_format;
        vb2.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCreateImageView(dev,&vb2,nullptr,&viewB);

        VkAttachmentDescription ab{}; ab.format=(VkFormat)g_backend.swapchain_format; ab.samples=VK_SAMPLE_COUNT_1_BIT;
        ab.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ab.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        ab.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; ab.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ab.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; ab.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference rb{}; rb.attachment=0; rb.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sb{}; sb.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sb.colorAttachmentCount=1; sb.pColorAttachments=&rb;
        VkSubpassDependency dpb[2]{};
        dpb[0].srcSubpass=VK_SUBPASS_EXTERNAL; dpb[0].dstSubpass=0;
        dpb[0].srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; dpb[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dpb[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dpb[1].srcSubpass=0; dpb[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dpb[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dpb[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dpb[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT; dpb[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rpb{}; rpb.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpb.attachmentCount=1; rpb.pAttachments=&ab; rpb.subpassCount=1; rpb.pSubpasses=&sb;
        rpb.dependencyCount=2; rpb.pDependencies=dpb;
        VkRenderPass rpB=VK_NULL_HANDLE; vkCreateRenderPass(dev,&rpb,nullptr,&rpB);
        VkFramebufferCreateInfo fbb{}; fbb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbb.attachmentCount=1; fbb.pAttachments=&viewB; fbb.width=W; fbb.height=H; fbb.layers=1; fbb.renderPass=rpB;
        vkCreateFramebuffer(dev,&fbb,nullptr,&fbB);

        VkGraphicsPipelineCreateInfo gpB=gp; gpB.renderPass=rpB;
        VkPipeline pipeB=VK_NULL_HANDLE; vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpB,nullptr,&pipeB);

        VkBuffer stbB=VK_NULL_HANDLE; VkDeviceMemory smemB=VK_NULL_HANDLE; void* smapB=nullptr;
        vkCreateBuffer(dev,&sci,nullptr,&stbB);
        VkMemoryRequirements smrb; vkGetBufferMemoryRequirements(dev,stbB,&smrb);
        VkMemoryAllocateInfo saib{}; saib.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; saib.allocationSize=smrb.size;
        saib.memoryTypeIndex=find_memory_type(smrb.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev,&saib,nullptr,&smemB); vkBindBufferMemory(dev,stbB,smemB,0);

        VkCommandBuffer cmdB=begin_command_buffer();
        VkClearValue cvB{}; cvB.color.float32[1]=1.f; cvB.color.float32[3]=1.f;  // green clear
        VkRenderPassBeginInfo rpiB{}; rpiB.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpiB.renderPass=rpB; rpiB.framebuffer=fbB; rpiB.renderArea={{0,0},{W,H}}; rpiB.clearValueCount=1; rpiB.pClearValues=&cvB;
        vkCmdBeginRenderPass(cmdB,&rpiB,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmdB,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeB);
        VkDeviceSize voffB=0; vkCmdBindVertexBuffers(cmdB,0,1,&vb,&voffB);
        cmd_set_viewport(cmdB,0,0.f,(SceFloat)W,(SceFloat)H); cmd_set_scissor(cmdB,0,0,W,H);
        vkCmdDraw(cmdB,3,1,0,0);
        vkCmdEndRenderPass(cmdB);  // finalLayout 直接�?TRANSFER_SRC
        VkBufferImageCopy brB{}; brB.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; brB.imageExtent={W,H,1};
        vkCmdCopyImageToBuffer(cmdB,imgB,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stbB,1,&brB);
        end_command_buffer(cmdB); submit_command_buffer(cmdB);

        vkMapMemory(dev,smemB,0,smrb.size,0,&smapB);
        long rB=0,gB=0,oB=0;
        if(smapB){ const unsigned char* px=(const unsigned char*)smapB;
            for(SceUInt32 i=0;i<W*H;i++){ unsigned char c0=px[i*4],c1=px[i*4+1],c2=px[i*4+2];
                bool gg=(c1>200&&c0<80&&c2<80); bool rr=(c1<80&&(c0>200||c2>200));
                if(gg)gB++; else if(rr)rB++; else oB++; }
            vkUnmapMemory(dev,smemB); }
        fprintf(stderr,"[SELFTEST-B stdRP] red=%ld green=%ld other=%ld\n",rB,gB,oB); fflush(stderr);
    }
    // ===== G 组：GPU �?copy 回读 vertex buffer，验�?GPU 是否读到 tri 数据 =====
    {
        VkBuffer cb=VK_NULL_HANDLE; VkDeviceMemory cm=VK_NULL_HANDLE; void* cmapped=nullptr;
        VkBufferCreateInfo cci{}; cci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; cci.size=48;
        cci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; cci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(dev,&cci,nullptr,&cb);
        VkMemoryRequirements cmr; vkGetBufferMemoryRequirements(dev,cb,&cmr);
        VkMemoryAllocateInfo cai{}; cai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; cai.allocationSize=cmr.size;
        cai.memoryTypeIndex=find_memory_type(cmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev,&cai,nullptr,&cm); vkBindBufferMemory(dev,cb,cm,0);
        VkCommandBuffer cmdG=begin_command_buffer();
        VkBufferMemoryBarrier vbb{}; vbb.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        vbb.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; vbb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vbb.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; vbb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        vbb.buffer=vb; vbb.offset=0; vbb.size=48;
        vbb.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        vkCmdPipelineBarrier(cmdG,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&vbb,0,nullptr);
        VkBufferCopy rc{}; rc.srcOffset=0; rc.dstOffset=0; rc.size=48;
        vkCmdCopyBuffer(cmdG,vb,cb,1,&rc);
        end_command_buffer(cmdG); submit_command_buffer(cmdG);
        vkMapMemory(dev,cm,0,cmr.size,0,&cmapped);
        if(cmapped){ const float* fp=(const float*)cmapped;
            fprintf(stderr,"[SELFTEST-G gpu-vb] %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f\n",
                fp[0],fp[1],fp[2],fp[3],fp[4],fp[5],fp[6],fp[7],fp[8],fp[9],fp[10],fp[11]); fflush(stderr);
            vkUnmapMemory(dev,cm); }
    }
}

static void frame_acquire() {
    if (g_frame_open) return;
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    vkWaitForFences(dev, 1, reinterpret_cast<VkFence*>(&g_backend.frame_fence), VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, reinterpret_cast<VkFence*>(&g_backend.frame_fence));
    // h144bp: previous frame finished on GPU -> release deferred GPU resources and recycle all
    // per-frame descriptor sets (each bind allocated a fresh set that the prior frame referenced).
    p5d_flush_zombies(dev);
    if(g_backend.descriptor_pool) vkResetDescriptorPool(dev, *reinterpret_cast<VkDescriptorPool*>(&g_backend.descriptor_pool), 0);
    VkResult result = vkAcquireNextImageKHR(dev, *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain),
        UINT64_MAX, *reinterpret_cast<VkSemaphore*>(&g_backend.image_available), VK_NULL_HANDLE,
        &g_backend.current_image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(g_backend.swapchain_width, g_backend.swapchain_height); return; }
    g_backend.current_cmd_buffer = begin_command_buffer();
    g_frame_open = true; g_frame_cleared_n = 0; g_backend.frame_count++;
    g_frame_vb_off=0; g_frame_ib_off=0;  // h144bs X17 reset per-frame geometry arena
    fprintf(stderr,"[FACQ] fc=%llu cmd=%p imgIdx=%u\n",(unsigned long long)g_backend.frame_count,(void*)g_backend.current_cmd_buffer,(unsigned)g_backend.current_image_index); fflush(stderr);
}

// 一�?GXM scene 开始：切换�?colorData 对应离屏 RT �?render pass（本帧首�?CLEAR，否�?LOAD�?
static OffscreenRT* g_pass_rt=nullptr; // h144bp: RT bound by current scene pass
// h144k74：按 GXM viewport 状态机设置�?pass �?Vulkan 视口（flat �?全屏扁平；否则按权威换算�?
static void apply_gxm_viewport(VkCommandBuffer cmd, float dw, float dh){
    static int _pn=0;
    if (g_gxm_vp_valid && !g_gxm_vp_flat) {
        const float xs=g_gxm_vp[1], ys=g_gxm_vp[3];
        const float vw=std::fabs(2.0f*xs), vh=2.0f*ys;
        const float vx=g_gxm_vp[0]-std::fabs(xs), vy=g_gxm_vp[2]-ys;
        cmd_set_viewport(cmd, vx, vy, (vw!=0.f)?vw:dw, (vh!=0.f)?vh:dh);
        if(_pn<8){ _pn++; fprintf(stderr,"[GXMVIEWPORT] real x=%.1f y=%.1f w=%.1f h=%.1f (xo=%.1f xs=%.1f yo=%.1f ys=%.1f) rt=%.0fx%.0f\n",
            vx,vy,(vw!=0.f)?vw:dw,(vh!=0.f)?vh:dh,g_gxm_vp[0],g_gxm_vp[1],g_gxm_vp[2],g_gxm_vp[3],dw,dh); fflush(stderr); }
    } else {
        cmd_set_viewport(cmd, 0.f, 0.f, dw, dh);
        if(_pn<8){ _pn++; fprintf(stderr,"[GXMVIEWPORT] flat x=0 y=0 w=%.0f h=%.0f\n",dw,dh); fflush(stderr); }
    }
}
void begin_scene_pass(SceUInt32 cd, SceUInt32 w, SceUInt32 h) {
    ++p5d::vulkan::g_n_begin_pass;
    std::lock_guard<std::mutex> _vklk(g_vk_record_mtx);
    g_dbg_pass_w=w; g_dbg_pass_h=h;
    if (!g_backend.initialized) return;
    // offscreen_draw_selftest(); // h144bp disabled: draw path proven; conflicts with per-frame pool/fence timing
    if (!cd) cd = 0x0A000001u;
    if (!w || !h) { w = g_backend.swapchain_width; h = g_backend.swapchain_height; }
    if (create_offscreen_render_passes() != 0) return;
    frame_acquire();
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    if (!cmd) return;
    if (g_backend.in_render_pass) { vkCmdEndRenderPass(cmd); g_backend.in_render_pass = false; }
    OffscreenRT* rt = get_rt(cd, w, h);
    g_pass_rt = rt;
    if (!rt) { LOGE("begin_scene_pass no rt cd=%08X", cd); return; }
    if(cd==0xAE100000u || cd==0xAE300000u){ static int _aep=0;
      if(_aep<60){ ++_aep; std::printf("[AEPASS #%d] cd=%08X %ux%u img=%p fbClear=%p\n",
          _aep,(unsigned)cd,(unsigned)w,(unsigned)h,(void*)rt->img,(void*)rt->fbClear); std::fflush(stdout); } }
    int doClear = 1;
    for (int i = 0; i < g_frame_cleared_n; i++) if (g_frame_cleared[i] == cd) { doClear = 0; break; }
    if (doClear && g_frame_cleared_n < 32) g_frame_cleared[g_frame_cleared_n++] = cd;
    VkClearValue cv{};
    cv.color.float32[0] = g_backend.clear_color.r; cv.color.float32[1] = g_backend.clear_color.g;
    cv.color.float32[2] = g_backend.clear_color.b; cv.color.float32[3] = g_backend.clear_color.a;
    // h144k0c：h144p 遗留�?*诊断�?*在此无条件覆盖了上面�?clear_color 填好的值，
    //   使主 RT 每帧被刷成纯�?(0,255,0)。它既是「画面唯一可见�?绿」的来源，又�?frag_0003 �?
    //   自反馈求�?(out = PA0 + PA1 + PA2) 叠加成「绿/黄交替闪烁�?
    //   —�?�?= 真实背景�?(255,72,0) + 诊断绿，相加后截断到�?(255,188,0)�?
    //   �?黄色本身证明**真实内容已经流进�?RT**，只是被诊断色污染�?
    //   改为中性黑，让真实内容不再被诊断色掩盖；P5D_DBG_GREENCLEAR=1 可恢复旧诊断行为做对照�?
    { static const char* _gc=getenv("P5D_DBG_GREENCLEAR");
      if(_gc && *_gc){ cv.color.float32[0]=0.0f; cv.color.float32[1]=1.0f; cv.color.float32[2]=0.0f; cv.color.float32[3]=1.0f; }
      else           { cv.color.float32[0]=0.0f; cv.color.float32[1]=0.0f; cv.color.float32[2]=0.0f; cv.color.float32[3]=1.0f; } }
    VkRenderPassBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = doClear ? g_rp_clear : g_rp_load;
    g_cur_pass_rp = bi.renderPass;   // h144k24：供 get_or_create_real_pipeline 建「与�?RP 匹配」的管线
    bi.framebuffer = doClear ? rt->fbClear : rt->fbLoad;
    bi.renderArea.offset = { 0, 0 }; bi.renderArea.extent = { w, h };
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    if(rt->layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL){
        // h144k26【采样快照】在把这�?RT 转为颜色附件**之前**（此刻它还是 SHADER_READ_ONLY、内容有效）�?
        //   先把内容 copy �?alias 副本。这样本 pass 内所有对该面�?RT→纹理采样都能拿到合�?layout 的快照，
        //   既解�?[ALIAS-SKIP] 的未定义采样（满屏噪声），也符合 Vita 的延迟渲染语义�?
        if (rt->aliasImg && rt->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && cmd) {
            VkImageMemoryBarrier sb[2]{};
            sb[0].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; sb[0].image=rt->img;
            sb[0].oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; sb[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sb[0].srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; sb[0].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            sb[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT; sb[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            sb[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            sb[1].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; sb[1].image=rt->aliasImg;
            sb[1].oldLayout=(VkImageLayout)rt->aliasLayout; sb[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            sb[1].srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; sb[1].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            sb[1].srcAccessMask=(rt->aliasLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)?VK_ACCESS_SHADER_READ_BIT:0;
            sb[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            sb[1].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,nullptr,0,nullptr,2,sb);
            VkImageCopy _cp{}; _cp.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            _cp.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; _cp.extent={w,h,1};
            vkCmdCopyImage(cmd, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rt->aliasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &_cp);
            VkImageMemoryBarrier ab{}; ab.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ab.image=rt->aliasImg;
            ab.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ab.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ab.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; ab.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            ab.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; ab.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            ab.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,0,nullptr,0,nullptr,1,&ab);
            rt->aliasLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            { static long _sn=0; if(_sn<12){ _sn++; fprintf(stderr,"[ALIAS-SNAP] cd=%08X %ux%u copied to alias\n",cd,w,h); fflush(stderr);} }
            // 注意：此�?rt 已被转到 TRANSFER_SRC，下面的 barrier �?rt->layout 作为 old 会不匹配�?
            //   故把 rt->layout 直接置为 TRANSFER_SRC，让后面的转换逻辑从正确起点出发�?
            rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else if (rt->aliasImg && rt->aliasLayout == VK_IMAGE_LAYOUT_UNDEFINED && cmd) {
            // 首次：alias 还没有内�?�?清成黑色并置�?SHADER_READ_ONLY（避免采样未初始�?image�?
            VkImageMemoryBarrier b1{}; b1.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b1.image=rt->aliasImg;
            b1.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b1.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b1.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b1.srcAccessMask=0; b1.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            b1.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b1);
            VkClearColorValue _cc{}; _cc.float32[0]=0.f;_cc.float32[1]=0.f;_cc.float32[2]=0.f;_cc.float32[3]=1.f;
            VkImageSubresourceRange _rr{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(cmd, rt->aliasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &_cc, 1, &_rr);
            VkImageMemoryBarrier b2{}; b2.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b2.image=rt->aliasImg;
            b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b2.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b2.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            b2.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b2);
            rt->aliasLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkImageMemoryBarrier gb{}; gb.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; gb.image=rt->img;
        gb.oldLayout=(VkImageLayout)rt->layout; gb.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        gb.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; gb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        gb.srcAccessMask=(rt->layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)?VK_ACCESS_SHADER_READ_BIT:0;
        gb.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        gb.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkPipelineStageFlags gs=(rt->layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        vkCmdPipelineBarrier(cmd,gs,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&gb);
        rt->layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    { static int _bg=0; if(_bg<40){ ++_bg; std::printf("[BEGRP2] cd=%08X %ux%u cmd=%p doClear=%d rp=%p\n",(unsigned)cd,(unsigned)w,(unsigned)h,(void*)cmd,(int)doClear,(void*)bi.renderPass); std::fflush(stdout); } }
    g_backend.in_render_pass = true; g_pass_color = cd; g_dbg_pass_color=cd;
    g_rp_cmd_dbg=cmd; g_rp_fb_dbg=bi.framebuffer; g_rp_cd_dbg=cd; g_rp_rp_dbg=bi.renderPass;
    if(w>=900u) fprintf(stderr,"[BSIMG] cd=%08X %ux%u doClear=%d img=%p fb=%p view=%p\n",
        cd,w,h,(int)doClear,(void*)rt->img,(void*)(doClear?rt->fbClear:rt->fbLoad),(void*)rt->view);
    apply_gxm_viewport(cmd, (float)w, (float)h);  // h144k74: �?GXM viewport 状态机（原来恒为全屏正高度视口�?
    cmd_set_scissor(cmd, 0, 0, w, h);
    ensure_geometry_buffers_frame();
    if(false && w>=900u){ VkClearAttachment _qa{}; _qa.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; _qa.colorAttachment=0;  /* h144bs X10 disabled */
      _qa.clearValue.color.float32[0]=1.f; _qa.clearValue.color.float32[1]=0.f; _qa.clearValue.color.float32[2]=0.f; _qa.clearValue.color.float32[3]=1.f;
      VkClearRect _qr{}; _qr.rect.offset={0,0}; _qr.rect.extent={w,h}; _qr.baseArrayLayer=0; _qr.layerCount=1;
      vkCmdClearAttachments(cmd,1,&_qa,1,&_qr); fprintf(stderr,"[X6BEG] red clear at begin_scene_pass cmd=%p w=%u\n",(void*)cmd,w); fflush(stderr); }
    // h144y DIAG (after ensure, buffer alive) built-in fullscreen triangle (verified simple_vert loc0 vec4, red frag)
    if(false && w>=900u && g_rp_clear){  // h144bs X1: FSDRAW disabled to isolate s_ep
        static VkPipeline s_fsp=nullptr; static VkPipelineLayout s_fsl=nullptr; static int s_fsrr=-99;
        VkDevice devd=*reinterpret_cast<VkDevice*>(&g_backend.device);
        if(!s_fsp){
            VkShaderModule vm2=nullptr,fm2=nullptr;
            int rm1=create_shader_module(s_fs_vert,(SceUInt32)(sizeof(s_fs_vert)/4),&vm2);
            int rm2=create_shader_module(s_fs_frag,(SceUInt32)(sizeof(s_fs_frag)/4),&fm2);
            VkPipelineLayoutCreateInfo pl{}; pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            vkCreatePipelineLayout(devd,&pl,nullptr,&s_fsl);
            VkPipelineShaderStageCreateInfo st2[2]{};
            st2[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st2[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st2[0].module=vm2;st2[0].pName="main";
            st2[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st2[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st2[1].module=fm2;st2[1].pName="main";
            VkVertexInputBindingDescription vb0{}; vb0.binding=0; vb0.stride=16; vb0.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
            VkVertexInputAttributeDescription va0{}; va0.binding=0; va0.location=0; va0.format=VK_FORMAT_R32G32B32A32_SFLOAT; va0.offset=0;
            VkPipelineVertexInputStateCreateInfo vi2{};vi2.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;vi2.vertexBindingDescriptionCount=1;vi2.pVertexBindingDescriptions=&vb0;vi2.vertexAttributeDescriptionCount=1;vi2.pVertexAttributeDescriptions=&va0;
            VkPipelineInputAssemblyStateCreateInfo ia2{};ia2.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia2.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport _svp{0.f,0.f,(float)w,(float)h,0.f,1.f}; VkRect2D _ssc{{0,0},{w,h}};
            VkPipelineViewportStateCreateInfo vp2{};vp2.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp2.viewportCount=1;vp2.scissorCount=1;vp2.pViewports=&_svp;vp2.pScissors=&_ssc;
            VkPipelineRasterizationStateCreateInfo rs2{};rs2.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;rs2.polygonMode=VK_POLYGON_MODE_FILL;rs2.cullMode=VK_CULL_MODE_NONE;rs2.frontFace=VK_FRONT_FACE_CLOCKWISE;rs2.lineWidth=1.0f;
            VkPipelineMultisampleStateCreateInfo ms2{};ms2.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms2.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState ca2{};ca2.colorWriteMask=0xF;
            VkPipelineColorBlendStateCreateInfo cb2{};cb2.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cb2.attachmentCount=1;cb2.pAttachments=&ca2;
            VkDynamicState dn2[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy2{};dy2.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dy2.dynamicStateCount=2;dy2.pDynamicStates=dn2;
            VkGraphicsPipelineCreateInfo info2{};info2.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info2.stageCount=2;info2.pStages=st2;info2.pVertexInputState=&vi2;info2.pInputAssemblyState=&ia2;
            info2.pViewportState=&vp2;info2.pRasterizationState=&rs2;info2.pMultisampleState=&ms2;info2.pColorBlendState=&cb2;info2.pDynamicState=nullptr;  // h144ae static viewport
            info2.layout=s_fsl;info2.renderPass=g_rp_clear;info2.subpass=0;
            VkResult rr=vkCreateGraphicsPipelines(devd,VK_NULL_HANDLE,1,&info2,nullptr,&s_fsp); s_fsrr=(int)rr;
            fprintf(stderr,"[FSPIPE] rm1=%d rm2=%d rr=%d pipe=%p\n",(int)rm1,(int)rm2,(int)rr,(void*)s_fsp);fflush(stderr);
            if(vm2)vkDestroyShaderModule(devd,vm2,nullptr); if(fm2)vkDestroyShaderModule(devd,fm2,nullptr);
        }
        {
            static VkBuffer s_tvb=VK_NULL_HANDLE; static VkDeviceMemory s_tmem=VK_NULL_HANDLE;
            if(s_fsp && s_tvb==VK_NULL_HANDLE){
                VkBufferCreateInfo tbi{}; tbi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                tbi.size=48; tbi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; tbi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
                VkResult tr0=vkCreateBuffer(devd,&tbi,nullptr,&s_tvb);
                VkMemoryRequirements tmr; vkGetBufferMemoryRequirements(devd,s_tvb,&tmr);
                VkMemoryAllocateInfo tai{}; tai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                tai.allocationSize=tmr.size; tai.memoryTypeIndex=find_memory_type(tmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                VkResult tr1=vkAllocateMemory(devd,&tai,nullptr,&s_tmem);
                VkResult tr2=vkBindBufferMemory(devd,s_tvb,s_tmem,0);
                void* tmp=nullptr; vkMapMemory(devd,s_tmem,0,48,0,&tmp);
                float tri0[12]={-1.f,-1.f,0.f,1.f, 3.f,-1.f,0.f,1.f, -1.f,3.f,0.f,1.f};
                if(tmp) memcpy(tmp,tri0,sizeof(tri0));
                fprintf(stderr,"[TVBUF] r=%d/%d/%d vb=%p mem=%p mt=%u\n",(int)tr0,(int)tr1,(int)tr2,(void*)s_tvb,(void*)s_tmem,(unsigned)tai.memoryTypeIndex);fflush(stderr);
            }
            if(s_fsp && s_tvb!=VK_NULL_HANDLE){
                VkDeviceSize _off=0;
                vkCmdBindVertexBuffers(cmd,0,1,&s_tvb,&_off);
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,s_fsp);
                cmd_set_viewport(cmd,0,0.0f,(SceFloat)w,(SceFloat)h);
                cmd_set_scissor(cmd,0,0,w,h);
                vkCmdDraw(cmd,3,1,0,0);
                fprintf(stderr,"[FSDRAW] cd=%08X tvb=%p\n",cd,(void*)s_tvb);fflush(stderr);
            }
        }
    }
}

void end_scene_pass() {
    std::lock_guard<std::mutex> _vklk(g_vk_record_mtx);
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    fprintf(stderr,"[ENDPASS] cd=%08X cmd=%p inRP=%d\n",g_pass_color,(void*)cmd,(int)g_backend.in_render_pass); fflush(stderr);
    if (cmd && g_backend.in_render_pass) {
        vkCmdEndRenderPass(cmd); g_backend.in_render_pass = false;
        // h144bp: rendering finished -> move RT to SHADER_READ_ONLY so a later composite scene
        // can sample it via an RT->texture alias. This barrier is OUTSIDE the render pass (legal).
        if (g_pass_rt && g_pass_rt->img) {
            VkImageMemoryBarrier rb{}; rb.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; rb.image=g_pass_rt->img;
            rb.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; rb.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rb.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; rb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            rb.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; rb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            rb.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,0,nullptr,0,nullptr,1,&rb);
            g_pass_rt->layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
}

// ★★�?h145k435【影片全屏输出：位置正确 + 颜色正确（不是灰色）】★★★
//   需求（用户）：影片必须**全屏**、位置正确�?*颜色正确**�?
//   做法：HLE �?NV12 帧转�?**BGRA** 后调用本函数上传成一�?VkImage（B8G8R8A8_UNORM，与交换链同格式），
//   �?present_frame 里用 `vkCmdBlitImage` 把它**全屏缩放** blit 到交换链图像�?—�?
//   同格�?�?颜色通道一致、无需着色器/管线；全屏缩�?�?位置/尺寸恒等于屏幕�?
static VkImage g_movie_img=VK_NULL_HANDLE; static VkDeviceMemory g_movie_mem=VK_NULL_HANDLE;
static int g_movie_w=0,g_movie_h=0,g_movie_ready=0;
// h145k1142：宿主全屏影片层是否正在放片（由 p5d_movie_set_frame / p5d_movie_clear 维护）�?
//   用途：TEXSRC 覆盖�?MOVIE_FS 全屏 blit 互斥，避免两层影片叠加成"斜角拉伸"错层�?
extern "C" int p5d_movie_layer_active=0;

extern "C" void p5d_movie_set_frame(const void* bgra,int w,int h){
    if(!g_backend.initialized||!bgra||w<=0||h<=0) return;
    VkDevice dev=*reinterpret_cast<VkDevice*>(&g_backend.device);
    if(!g_movie_img||g_movie_w!=w||g_movie_h!=h){
        if(g_movie_img){ vkDestroyImage(dev,g_movie_img,nullptr); g_movie_img=VK_NULL_HANDLE; }
        if(g_movie_mem){ vkFreeMemory(dev,g_movie_mem,nullptr); g_movie_mem=VK_NULL_HANDLE; }
        g_movie_ready=0;
        VkImageCreateInfo ii{}; ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType=VK_IMAGE_TYPE_2D; ii.format=VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent={(SceUInt32)w,(SceUInt32)h,1}; ii.mipLevels=1; ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT;
        ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev,&ii,nullptr,&g_movie_img)!=VK_SUCCESS){ g_movie_img=VK_NULL_HANDLE; return; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,g_movie_img,&mr);
        VkMemoryAllocateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ia.allocationSize=mr.size;
        ia.memoryTypeIndex=find_memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(vkAllocateMemory(dev,&ia,nullptr,&g_movie_mem)!=VK_SUCCESS){ g_movie_mem=VK_NULL_HANDLE; return; }
        vkBindImageMemory(dev,g_movie_img,g_movie_mem,0);
        g_movie_w=w; g_movie_h=h;
    }
    VkDeviceSize sz=(VkDeviceSize)w*(VkDeviceSize)h*4u;
    VkBuffer sb=VK_NULL_HANDLE; VkDeviceMemory sm=VK_NULL_HANDLE;
    VkBufferCreateInfo sbci{}; sbci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; sbci.size=sz;
    sbci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT; sbci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(dev,&sbci,nullptr,&sb)!=VK_SUCCESS) return;
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev,sb,&bmr);
    VkMemoryAllocateInfo bmai{}; bmai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; bmai.allocationSize=bmr.size;
    bmai.memoryTypeIndex=find_memory_type(bmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if(vkAllocateMemory(dev,&bmai,nullptr,&sm)!=VK_SUCCESS){ vkDestroyBuffer(dev,sb,nullptr); return; }
    vkBindBufferMemory(dev,sb,sm,0);
    void* mp=nullptr;
    if(vkMapMemory(dev,sm,0,sz,0,&mp)!=VK_SUCCESS||!mp){ vkDestroyBuffer(dev,sb,nullptr); vkFreeMemory(dev,sm,nullptr); return; }
    std::memcpy(mp,bgra,(size_t)sz); vkUnmapMemory(dev,sm);
    VkCommandBuffer mcmd=begin_command_buffer();
    if(!mcmd){ vkDestroyBuffer(dev,sb,nullptr); vkFreeMemory(dev,sm,nullptr); return; }
    VkImageMemoryBarrier mb{}; mb.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    mb.oldLayout=g_movie_ready?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; mb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    mb.image=g_movie_img; mb.srcAccessMask=g_movie_ready?VK_ACCESS_TRANSFER_READ_BIT:0;
    mb.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; mb.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(mcmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&mb);
    VkBufferImageCopy rgn{}; rgn.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    rgn.imageExtent={(SceUInt32)w,(SceUInt32)h,1};
    vkCmdCopyBufferToImage(mcmd,sb,g_movie_img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rgn);
    mb.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; mb.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(mcmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&mb);
    end_command_buffer(mcmd); submit_command_buffer(mcmd);
    vkQueueWaitIdle(*reinterpret_cast<VkQueue*>(&g_backend.graphics_queue));
    vkDestroyBuffer(dev,sb,nullptr); vkFreeMemory(dev,sm,nullptr);
    g_movie_ready=1;
    p5d_movie_layer_active=1;   // h145k1142：影片层已上�?�?关闭 TEXSRC 覆盖（两层会叠加成错层）
}

// �?h145k445：关闭影片全�?blit（影片放�?/ 换片时用）�?
extern "C" void p5d_movie_clear(){ g_movie_ready=0; p5d_movie_layer_active=0; }

// �?h145k445：guest 影片请求登记（见头文件注释）�?
//   只保�?*影片�?*（如 'movie/c_p5d000.usm'）与一个递增序号�?
//   探针侧用序号变化判定"guest 又开始放新一部影片了"，据此重新起表（时钟归零）�?
static char g_movie_req_name[96]={0};
static unsigned g_movie_req_seq=0;
extern "C" void p5d_movie_request(const char* name){
    if(!name||!*name) return;
    size_t i=0; for(;name[i]&&i<sizeof(g_movie_req_name)-1;i++) g_movie_req_name[i]=name[i];
    g_movie_req_name[i]=0; ++g_movie_req_seq;
}
extern "C" const char* p5d_movie_requested(){ return g_movie_req_name; }
extern "C" unsigned p5d_movie_request_seq(){ return g_movie_req_seq; }

// �?h145k461【影片放完后"把引擎推过影片这一�?的武装标志】★
//   动机（实测）：引擎自己的影片播放器卡�?`cri[+4]=0x1`（streamer 未建）⇒ 影片永不结束 �?
//   title sequence 永远到不�?title 场景。`P5D_FORCE_MOVEND` 证明"推过去之后引擎会继续"
//   （打开第二部影�?c_p5d001 = 标题 LOGO 影片、[TSEQ] stage 开始推进）�?
//   这里�?�?的时机改�?*我们自己的全屏影片播完之�?*（而不是一上来就谎报）�?
//   于是引擎侧经历的�?影片正常播了 103 秒然后结�?，比一开始就跳过干净得多�?
static int g_movend_armed=0;
extern "C" void p5d_movend_arm(){ g_movend_armed=1; }
extern "C" int  p5d_movend_armed(){ return g_movend_armed; }

// 主合�?pass 结束：把�?RT copy �?swapchain，submit + present（一帧一次）
void present_frame(SceUInt32 cd) {
    ++p5d::vulkan::g_n_present;
    // 18.3605: P5D_RT_ALL=1 -> dump every known offscreen/presented RT a few times at present time, so we can
    // see whether content geometry produced pixels in the offscreen post-processing RTs (AE100000/AE300000)
    // while the presented RT only holds constant-colour fills.
    { static int _ra=-1; if(_ra<0){ _ra=getenv("P5D_RT_ALL")?1:0; } static int _ran=0;
      if(_ra && _ran<8){ ++_ran;
        static const SceUInt32 _cds[5]={0xA0F00000u,0xA1300000u,0xA1700000u,0xAE100000u,0xAE300000u};
        for(int _i=0;_i<5;_i++){ char _fn[80]; std::snprintf(_fn,sizeof(_fn),"rt_all_%d_%08X.bmp",_ran,_cds[_i]);
          save_rt_screenshot(_cds[_i],_fn); } } }
    // ★★★ ARCH_462【字形图集(a1) vs (a2) 的逐帧判据】★★★
    //   捕获在上传路径（见 upload_texture_to_unit 里的 ATLAS-WATCH），扫描在这里 —— 每帧一次。
    //   判据：图集内存里"非 1.0f 的像素数"。该数**增长** ⇒ (a1) 上传副本过期；
    //   始终为 0 ⇒ (a2) 字形从未被渲染。
    // ★ ARCH_496/498【原始字符串取证】`P5D_STRSCAN=<fc>`
    //   动机：屏幕上那句话到底**应该是**什么，是当前诊断的关键。靠放大截图读像素已经到极限
    //   （空隙里只有很淡的痕迹）。必须从 guest 内存直接读出原始字符串。
    //   ARCH_496 只扫了 0xA0000000~0xB0000000 的 ASCII "P5D"，只命中文件名列表
    //   （`c_staff_P5D.bin` 之类）⇒ ARCH_498 扩大范围到 0x80000000~0xC0000000，
    //   并同时尝试 UTF-8 与 UTF-16LE/BE 的 `當`（U+7576）与 `P5D`，覆盖三种可能编码。
    { static long long _ss=-2; static int _ssdone=0;
      if(_ss==-2){ _ss=-1; const char* _e=std::getenv("P5D_STRSCAN"); if(_e&&*_e) _ss=std::atoll(_e); }
      if(!_ssdone && _ss>=0 && (long long)g_backend.frame_count>=_ss){
        _ssdone=1;
        struct Needle { const char* name; SceUInt8 b[8]; int n; };
        Needle nd[6]={
          {"ascii-P5D",   {0x50,0x35,0x44,0,0,0,0,0}, 3},
          {"u16le-P5D",   {0x50,0x00,0x35,0x00,0x44,0x00,0,0}, 6},
          {"utf8-dang",   {0xE7,0x95,0xB6,0,0,0,0,0}, 3},
          {"u16le-dang",  {0x76,0x75,0,0,0,0,0,0}, 2},
          {"big5-dang",   {0xB7,0x43,0,0,0,0,0,0}, 2},
          {"utf8-comma",  {0xEF,0xBC,0x8C,0,0,0,0,0}, 3},
        };
        printf("[STRSCAN] begin fc=%llu\n",(unsigned long long)g_backend.frame_count); fflush(stdout);
        for(int _k=0;_k<6;++_k){
          int _hits=0;
          for(SceUInt32 _pg=0x80000000u; _pg<0xC0000000u && _hits<6; _pg+=0x1000u){
            if(!p5d::mem::contains(_pg,0x1000u)) continue;
            SceUInt8* _b=p5d::mem::translate(_pg);
            if(!_b) continue;
            for(SceUInt32 _o=0;_o+8<0x1000u;++_o){
              int _m=1; for(int _t=0;_t<nd[_k].n;++_t) if(_b[_o+_t]!=nd[_k].b[_t]){ _m=0; break; }
              if(!_m) continue;
              SceUInt32 _s=(_o>=80u)?(_o-80u):0u, _e2=_o+80u; if(_e2>0x1000u) _e2=0x1000u;
              printf("[STRSCAN %s] hit#%d va=%08X pre=%u\n",nd[_k].name,_hits,_pg+_s,_o-_s);
              printf("   hex:");
              for(SceUInt32 _i=_s;_i<_e2;++_i) printf("%02X",_b[_i]);
              printf("\n   txt:");
              for(SceUInt32 _i=_s;_i<_e2;++_i){ unsigned char _c=_b[_i]; putchar((_c>=32&&_c<127)?(int)_c:'.'); }
              printf("\n"); fflush(stdout);
              ++_hits; if(_hits>=6) break;
            }
          }
          if(_hits==0) printf("[STRSCAN %s] no hit\n",nd[_k].name);
          fflush(stdout);
        }
        printf("[STRSCAN] end\n"); fflush(stdout); }
    }
    // ★ ARCH_504【任意 guest 区域转储】`P5D_DUMPREGION=<hexva>:<hexlen>[:<fc>]`
    //   动机：字形槽位是**等距定长数组**（ARCH_494：步长 0x1100 = 4096 + 256），
    //   而异常恰好是"某些字形被画在 +24px"（ARCH_503）。那多出的 **256 字节**很可能是
    //   **每个字形的度量**（advance / bearing / 基线）。先把它原样转储下来看清楚，
    //   再决定要不要往"度量表被写错"这条线上投。
    { static int _dr=-2; static int _drdone=0; static SceUInt32 _drva=0; static SceUInt32 _drlen=0; static long long _drfc=-1;
      if(_dr==-2){ _dr=0; const char* _e=std::getenv("P5D_DUMPREGION");
        if(_e&&*_e){ char _b[128]; std::snprintf(_b,sizeof(_b),"%s",_e); char* _s=_b;
          char* _c1=std::strchr(_s,':');
          if(_c1){ *_c1=0; _drva=(SceUInt32)std::strtoul(_s,nullptr,16);
            char* _s2=_c1+1; char* _c2=std::strchr(_s2,':');
            if(_c2){ *_c2=0; _drfc=std::atoll(_c2+1); }
            _drlen=(SceUInt32)std::strtoul(_s2,nullptr,16); _dr=1; } }
        printf("[DUMPREGION-CFG] env=%s va=%08X len=%u fc=%lld\n",_e?_e:"(null)",_drva,_drlen,_drfc);
        fflush(stdout); }
      if(_dr==1 && !_drdone && (_drfc<0 || (long long)g_backend.frame_count>=_drfc)){
        _drdone=1;
        if(_drlen && p5d::mem::contains(_drva,_drlen)){
          SceUInt8* _p=p5d::mem::translate(_drva);
          char _f[256]; std::snprintf(_f,sizeof(_f),"C:\\dev\\p5d_build\\texframe\\region_%08X_%u.raw",_drva,_drlen);
          if(_p){ if(std::FILE* _fp=std::fopen(_f,"wb")){ std::fwrite(_p,1,_drlen,_fp); std::fclose(_fp);
              printf("[DUMPREGION] %s\n",_f); } }
          else printf("[DUMPREGION] translate failed va=%08X\n",_drva);
        } else printf("[DUMPREGION] not contained va=%08X len=%u\n",_drva,_drlen);
        fflush(stdout); } }
    // ★ ARCH_511【分配归属取证】`P5D_ALLOCPC=<hexva>[,<hexva>...]`（最多 8 个）+ `P5D_ALLOCPC_FC=<fc>`
    //   动机（ARCH_510）：异常槽位是**交错**出现的（槽 19/22/24 的位图满宽 `lft=0,rgt=0` ⇒ 被裁切，
    //   而槽 21/23 正常），说明不是"某个池坏了"，而是**同一字符的某一次光栅化失败**。
    //   要知道"这块 4096 字节的槽是谁分配的"，最直接的办法就是问探针自己的分配台账：
    //   `p5d_mem_alloc_pc(va)`（address_space.cpp 导出，记的是真正的 guest 调用方）。
    //   拿到 PC 后就能用 `_srcmap.json`（源码↔地址全图）离线定位到具体源文件，再反汇编。
    { static int _ap=-2; static int _apdone=0; static SceUInt32 _apva[8]={0}; static int _apn=0; static long long _apfc=-1;
      if(_ap==-2){ _ap=0;
        const char* _e=std::getenv("P5D_ALLOCPC");
        if(_e&&*_e){ char _b[256]; std::snprintf(_b,sizeof(_b),"%s",_e); char* _s=_b;
          while(_s&&*_s&&_apn<8){ char* _c=std::strchr(_s,','); if(_c)*_c=0;
            _apva[_apn++]=(SceUInt32)std::strtoul(_s,nullptr,16); _s=_c?(_c+1):nullptr; } }
        const char* _f=std::getenv("P5D_ALLOCPC_FC"); if(_f&&*_f) _apfc=std::atoll(_f);
        printf("[ALLOCPC-CFG] n=%d fc=%lld",_apn,_apfc);
        for(int _i=0;_i<_apn;++_i) printf(" %08X",_apva[_i]);
        printf("\n"); fflush(stdout); }
      if(_ap==0 && !_apdone && (_apfc<0 || (long long)g_backend.frame_count>=_apfc)){
        _apdone=1;
        for(int _i=0;_i<_apn;++_i){
          SceUInt32 _pc=p5d_mem_alloc_pc(_apva[_i]);
          // ★ ARCH_513：`p5d_mem_alloc_pc` 只对"分配起点"有记录，槽位是池内部的地址 ⇒ 返回 0。
          //   正解是用 `p5d_mem_alloc_cover(a,len,&va,&pc)`：它返回**覆盖该地址的那一次分配**
          //   的起始 VA 与发起分配的 guest PC ⇒ 正好回答"这些 4096 字节槽位属于哪个池、谁分配的"。
          SceUInt32 _cva=0, _cpc=0;
          unsigned _cov=p5d_mem_alloc_cover(_apva[_i],4096u,&_cva,&_cpc);
          printf("[ALLOCPC] va=%08X start_pc=%08X cover=%u pool_va=%08X pool_pc=%08X\n",
                 _apva[_i],_pc,_cov,_cva,_cpc);
        }
        fflush(stdout); } }
    if(false && g_atlasHost && g_atlasRep<400 && g_backend.frame_count>=g_atlasLastAt+1){   // ARCH_463 解除武装（见全局状态处说明）
      ++g_atlasRep; g_atlasLastAt=g_backend.frame_count;
      SceUInt64 _npx=(SceUInt64)g_atlasW*(SceUInt64)g_atlasH, _notOne=0; SceUInt32 _first=0xFFFFFFFFu;
      for(SceUInt64 _i=0;_i<_npx;++_i){ SceUInt32 _v; std::memcpy(&_v,g_atlasHost+_i*4,4);
        if(_v!=0x3F800000u){ if(_first==0xFFFFFFFFu) _first=(SceUInt32)_i; ++_notOne; } }
      printf("[ATLAS-WATCH] fc=%llu 非1.0像素=%llu/%llu 首个偏移=%u\n",
             (unsigned long long)g_backend.frame_count,(unsigned long long)_notOne,
             (unsigned long long)_npx,_first);
      fflush(stdout); }
    // h145k727【分段计时】送显 19.5ms/�?的归属：RT 拷贝�?/ submit / present
    auto _pf_t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> _vklk(g_vk_record_mtx);
    if (!g_backend.initialized) return;
    VkDevice dev = *reinterpret_cast<VkDevice*>(&g_backend.device);
    VkCommandBuffer cmd = g_backend.current_cmd_buffer;
    if (!cmd) { g_frame_open = false; return; }
    if (g_backend.in_render_pass) { vkCmdEndRenderPass(cmd); g_backend.in_render_pass = false; }
    OffscreenRT* rt = get_rt(cd, g_backend.swapchain_width, g_backend.swapchain_height);
    VkImage dst = g_backend.swapchain_images[g_backend.current_image_index];
    // ★★�?h145k567【swapchain image �?layout 跟踪】★★★
    //   问题：下面的 barrier �?swapchain image 恒用 `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED`
    //   （Vulkan 规范：UNDEFINED 表示"之前的内容可丢弃"）且 `srcAccessMask=0`�?
    //   而除第一帧外，该 image 的真�?layout �?`PRESENT_SRC_KHR`（上�?present 之后的状态）
    //   �?长期使用**不合规的 oldLayout**，驱动可以合法地丢弃内容�?
    //   实测症状：主 RT 确有内容（`[RTSTAT] fc=2253�?289 mean=2.48�?.01`�?10 条非零）�?
    //   submit/present 全部 `VK_SUCCESS`，但**同帧 swapchain 回读**（`seq_same_*.bmp`�?
    //   与窗口客户区仍全�?�?内容没有进入 swapchain image�?
    //   修法：为每张 swapchain image 维护真实 layout，初�?UNDEFINED，present 成功后置 PRESENT_SRC_KHR�?
    static VkImageLayout s_sc_layout[8] = { VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED };
    static int s_sc_ini = 0;
    if (!s_sc_ini) { s_sc_ini = 1;
        SceUInt32 _n = g_backend.swapchain_image_count; if (_n > 8u) _n = 8u;
        for (SceUInt32 _i = 0; _i < _n; ++_i) s_sc_layout[_i] = VK_IMAGE_LAYOUT_UNDEFINED;
        printf("[SCLAYOUT] 初始�?swapchain layout 跟踪，image_count=%u\n", (unsigned)_n); fflush(stdout); }
    const SceUInt32 _sci = (SceUInt32)g_backend.current_image_index;
    VkImageLayout _sc_old = (_sci < 8u) ? s_sc_layout[_sci] : VK_IMAGE_LAYOUT_UNDEFINED;
    // �?h145k525：同时打 stdout（present �?cd = 真正上屏的那张面，必须与引擎绑定�?RT 直接比对�?
    printf("[PRES] cd=%08X imgIdx=%u rtimg=%p pass_rt=%p\n",cd,(unsigned)g_backend.current_image_index,
        (void*)(rt?rt->img:nullptr),(void*)(g_pass_rt?g_pass_rt->img:nullptr));
    fprintf(stderr,"[PRES] cd=%08X cmd=%p imgIdx=%u inRP=%d img=%p view=%p\n",cd,(void*)cmd,(unsigned)g_backend.current_image_index,(int)g_backend.in_render_pass,
        (void*)(rt?rt->img:nullptr),(void*)(rt?rt->view:nullptr)); fflush(stdout); fflush(stderr);
    if (rt && dst) {
        VkImageMemoryBarrier bs[2]{};
        bs[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bs[0].image = rt->img;
        bs[0].oldLayout = (VkImageLayout)rt->layout; // h144bp: COLOR_ATTACHMENT or SHADER_READ after end_scene_pass
        bs[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bs[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bs[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bs[0].srcAccessMask = (rt->layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; bs[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bs[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        bs[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bs[1].image = dst;
        bs[1].oldLayout = _sc_old;   // �?h145k567：用**真实 layout**（此前恒�?UNDEFINED�?
        bs[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bs[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bs[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bs[1].srcAccessMask = 0; bs[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bs[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bs);
        rt->layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkImageCopy cp{};
        cp.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        cp.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        SceUInt32 cw = rt->w < g_backend.swapchain_width ? rt->w : g_backend.swapchain_width;
        SceUInt32 ch = rt->h < g_backend.swapchain_height ? rt->h : g_backend.swapchain_height;
        cp.extent = { cw, ch, 1 };
        fprintf(stderr,"[CPYIMG] cd=%08X src=%p %ux%u dst=%p idx=%u\n",
            cd,(void*)rt->img,rt->w,rt->h,(void*)dst,g_backend.current_image_index);
        vkCmdCopyImage(cmd, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        // �?h145k435【影片全�?blit】把影片�?*全屏缩放**覆到交换链图像上（同格式 �?颜色正确）�?
        //   放在 RT→swapchain 拷贝之后、转 PRESENT_SRC 之前，此�?dst 正处�?TRANSFER_DST_OPTIMAL�?
        if(g_movie_ready && g_movie_img && g_movie_w>0 && g_movie_h>0){
            VkImageBlit bl{}; bl.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            bl.srcOffsets[0]={0,0,0}; bl.srcOffsets[1]={g_movie_w,g_movie_h,1};
            bl.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            bl.dstOffsets[0]={0,0,0};
            bl.dstOffsets[1]={(SceInt32)g_backend.swapchain_width,(SceInt32)g_backend.swapchain_height,1};
            vkCmdBlitImage(cmd,g_movie_img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&bl,VK_FILTER_LINEAR);
            static int _mb=0; if(_mb<4){ _mb++;
                fprintf(stderr,"[MOVIEBLIT] %dx%d -> %ux%u 全屏 blit\n",g_movie_w,g_movie_h,
                    (unsigned)g_backend.swapchain_width,(unsigned)g_backend.swapchain_height); fflush(stderr); } }
        VkImageMemoryBarrier pd{}; pd.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        pd.image = dst; pd.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pd.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        pd.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; pd.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pd.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; pd.dstAccessMask = 0;
        pd.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &pd);
        // h144bp: leave main RT SHADER_READ_ONLY after copy so feedback/alias sampling is valid;
        // begin_scene_pass moves it back to COLOR_ATTACHMENT (outside the RP) before re-rendering.
        VkImageMemoryBarrier rb{}; rb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rb.image = rt->img; rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; rb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; rb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &rb);
        rt->layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    end_command_buffer(cmd);

    VkSemaphore wait_semaphores[] = { *reinterpret_cast<VkSemaphore*>(&g_backend.image_available) };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT };
    VkSemaphore signal_semaphores[] = { *reinterpret_cast<VkSemaphore*>(&g_backend.render_finished) };
    VkSubmitInfo submit_info{}; submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1; submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages; submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd; submit_info.signalSemaphoreCount = 1; submit_info.pSignalSemaphores = signal_semaphores;
    VkResult result = vkQueueSubmit(*reinterpret_cast<VkQueue*>(&g_backend.graphics_queue), 1, &submit_info,
        *reinterpret_cast<VkFence*>(&g_backend.frame_fence));
    auto _pf_t1 = std::chrono::steady_clock::now();   // h145k727 分段计时：submit 完成�?
    if (result != VK_SUCCESS) LOGE("present_frame submit failed: %s", vk_result_string(result));
    // �?h145k566【submit 结果常备可见】原实现只在失败�?`LOGE`（走 stderr，默认被 NUL）⇒
    //   "submit 是否失败"在日志里**从未可见**。这里同时打 stdout，并显式带出 result�?
    { static int _sn=0; _sn++;
      if(result!=VK_SUCCESS || _sn<=8)
        printf("[SUBMIT] #%d cmd=%p imgIdx=%u result=%d(%s)\n",_sn,(void*)cmd,
          (unsigned)g_backend.current_image_index,(int)result,vk_result_string(result));
      fflush(stdout); }
    // ★★�?h145k367【RenderDoc 应用内触发（不依�?F12 键盘钩子）】★★★
    //   动机：h145k366 �?`renderdoccmd capture` 启动 + 向窗口发 F12 **未能产出 .rdc**
    //   �?LOCALAPPDATA%\Temp\RenderDoc 下只有一个空�?dumps 目录，无日志、无捕获）�?
    //   改用官方 API：被注入�?renderdoc.dll 已在本进程里，导�?`RENDERDOC_GetAPI`�?
    //   本块�?*指定�?*调用 `TriggerCapture()`，帧号由 `P5D_RDOC_FRAME` 精确给定�?
    //   （RENDERDOC_API_1_0_0 结构：索�?11 = SetCaptureFilePathTemplate，索�?15 = TriggerCapture。）
    {
      static void* _rdapi=(void*)0; static int _rdtry=0, _rddone=0, _rdwant_init=0; static unsigned long _rdwant=0;
      if(!_rdwant_init){ _rdwant_init=1; const char* _e=std::getenv("P5D_RDOC_FRAME");
        _rdwant=(_e&&*_e)?strtoul(_e,nullptr,10):0; }
      if(_rdwant && !_rddone){
        if(!_rdtry){ _rdtry=1;
          HMODULE _h=LoadLibraryA("renderdoc.dll");
          if(!_h) fprintf(stderr,"[RDOC] renderdoc.dll 未加载（未在 RenderDoc 下运行）\n");
          else {
            typedef int (*GetAPIFn)(int,void**);
            GetAPIFn _f=(GetAPIFn)(void*)GetProcAddress(_h,"RENDERDOC_GetAPI");
            void* _api=nullptr;
            if(_f && _f(10000,&_api)==1 && _api){ _rdapi=_api; fprintf(stderr,"[RDOC] API ready (1.0.0)\n"); }
            else fprintf(stderr,"[RDOC] RENDERDOC_GetAPI(10000) 失败\n"); }
          fflush(stderr); }
        if(_rdapi && g_backend.frame_count==_rdwant){
          _rddone=1; void** _vt=(void**)_rdapi;
          typedef void (*SetTmplFn)(void*,const char*); typedef void (*TrigFn)(void*);
          SetTmplFn _st=(SetTmplFn)_vt[11]; TrigFn _tg=(TrigFn)_vt[15];
          if(_st&&_tg){ _st(_rdapi,"C:\\dev\\p5d_build\\p5dcap"); _tg(_rdapi);
            fprintf(stderr,"[RDOC] TriggerCapture 已在 frame=%llu 调用\n",(unsigned long long)g_backend.frame_count); }
          else fprintf(stderr,"[RDOC] 函数指针为空 st=%p tg=%p\n",(void*)_st,(void*)_tg);
          fflush(stderr); } }
    }

    // 18.3579: P5D_CAP_FIRST=N -> save the first N presented frames as p5d_first_%04ld.bmp (lightweight;
    // does NOT keep stderr open like P5D_DBG_FRAMES). Used to obtain visual proof of AOT-rendered frames.
    { static int _cf_init = 0; static long _cf = 0, _cfmax = 0, _cfafter = 0;
      if (!_cf_init) { _cf_init = 1; const char* _e = std::getenv("P5D_CAP_FIRST");
                       _cfmax = (_e && *_e) ? strtol(_e, nullptr, 10) : 0;
                       const char* _a = std::getenv("P5D_CAP_AFTER"); _cfafter = (_a && *_a) ? strtol(_a, nullptr, 10) : 0; }
      if (_cfmax > 0 && _cf < _cfmax && g_backend.frame_count > (SceUInt64)_cfafter) {
          char _fn[48]; std::snprintf(_fn, sizeof(_fn), "p5d_first_%04ld.bmp", _cf);
          save_screenshot(_fn);
          { char _fn2[64]; std::snprintf(_fn2, sizeof(_fn2), "rt_first_%04ld.bmp", _cf);
            save_rt_screenshot(cd, _fn2); }
          ++_cf;
          fprintf(stderr, "[CAPFIRST] #%ld frame=%llu file=%s\n", _cf, (unsigned long long)g_backend.frame_count, _fn);
          fflush(stderr);
      } }
    { static int __cap = 0; static const SceUInt64 __mk[4] = { 1400ull,1450ull,1500ull,1550ull };  // h144bs X15 steady frames
      // h145k415【修仪表 bug】原写法 `if (save_screenshot(__fn) == 0) __cap++;`�?
      //   �?`P5D_SEQ_MIN`（全黑帧返回 1，不写文件）�?__cap **永远不推�?* �?从帧 1400 �?*每帧**都做一�?
      //   全屏回读（实�?`[SHOTSTAT] frame=1726/1727/1728�?file=p5d_frame_1.bmp`），既把运行拖慢数倍，
      //   又把 `P5D_SHOTSTAT_MAX` 的打印额度耗光，导致后续真正的抓帧没有统计行�?
      //   修法：无论是否写出文件都推进（跳过＝该时刻无内容，不需要重试）�?
      if (__cap < 4 && g_backend.frame_count >= __mk[__cap]) { char __fn[48]; std::snprintf(__fn, sizeof(__fn), "p5d_frame_%d.bmp", __cap + 1);
        save_screenshot(__fn); __cap++; } }
    // h144k25【高频连续抓帧】P5D_DBG_FRAMES=1：每 100 帧抓一�?p5d_seq_NNNN.bmp（到 2400 帧）�?
    //   动机：用户肉眼在窗口里看到「白点和线」，而固�?4 个时刻（1400/1450/1500/1550）抓下来的全是纯�?
    //   �?画面内容很可能是**瞬时**的，固定时刻采样会漏掉。连续采样才能看出内容随时间怎么变�?
    { static const char* _fs=getenv("P5D_DBG_FRAMES");
      if(_fs&&*_fs){ static SceUInt64 _last=0;
        // h144k72【逐帧扫描】步�?区间可配：P5D_SEQ_STEP（默�?100�? P5D_SEQ_FROM / P5D_SEQ_TO�?
        //   动机：每 100 帧采样会漏掉"只在个别帧出�?的瞬时内容（本项目已因此误判过多次）�?
        //   需要能连续逐帧抓取来判定内容是"一次�?/ 周期�?/ 常驻"�?
        static SceUInt64 _step=0,_from=0,_to=0;
        if(!_step){ const char* _s=getenv("P5D_SEQ_STEP"); _step=(_s&&*_s)?strtoull(_s,nullptr,10):100ull; if(!_step)_step=100ull;
          const char* _f=getenv("P5D_SEQ_FROM"); _from=(_f&&*_f)?strtoull(_f,nullptr,10):_step;
          const char* _t=getenv("P5D_SEQ_TO");   _to=(_t&&*_t)?strtoull(_t,nullptr,10):2400ull; }
        SceUInt64 _b=g_backend.frame_count/_step;
        if(g_backend.frame_count>=_from && _b!=_last && g_backend.frame_count<=_to){
          _last=_b; char _fn[48];
          std::snprintf(_fn,sizeof(_fn),"p5d_seq_%04llu.bmp",(unsigned long long)g_backend.frame_count);
          // h145k377：save_screenshot 现在返回 **0 = 确实写出了文�?*�?*1 = �?P5D_SEQ_MIN 判为全黑而丢�?*�?
          //   P5D_SEQ_RT=1 时，只对"有内�?的那些帧追加同帧 RT 回读（[RTSTAT] + rt_same_*.bmp），
          //   于是能一眼看出内容是�?RT 里、还是只�?rt→swapchain 这一步�?
          SceInt32 _sr2=save_screenshot(_fn);
          { const char* _rr=getenv("P5D_SEQ_RT");
            if(_sr2==0 && _rr && *_rr){ char _fn2[64];
              std::snprintf(_fn2,sizeof(_fn2),"rt_same_%04llu.bmp",(unsigned long long)g_backend.frame_count);
              save_rt_screenshot(cd,_fn2); } } } } }
    // ★★�?h145k383【内容触发抓帧】只要本帧出现过目标片元哈希�?draw，就抓一帧（与帧号无关）★★�?
    { static long _cdcap=0; long _lim=40; const char* _l=getenv("P5D_CAPH_MAX"); if(_l&&*_l) _lim=strtol(_l,nullptr,10);
      if(g_frame_content_draws && _cdcap<_lim){ _cdcap++;
        char _fn[64]; std::snprintf(_fn,sizeof(_fn),"p5d_cd_%04ld.bmp",_cdcap);
        fprintf(stderr,"[CDCAP] #%ld frame=%llu content_draws=%u file=%s\n",_cdcap,
            (unsigned long long)g_backend.frame_count,(unsigned)g_frame_content_draws,_fn); fflush(stderr);
        save_screenshot(_fn);
        { const char* _rr=getenv("P5D_SEQ_RT");
          if(_rr && *_rr){ char _fn2[64];
            std::snprintf(_fn2,sizeof(_fn2),"rt_cd_%04ld.bmp",_cdcap);
            save_rt_screenshot(cd,_fn2); } } }
      g_frame_content_draws = 0; }
    if (g_force_capture[0]) { save_screenshot(g_force_capture); g_force_capture[0] = 0; }
    // h144qw2-j 诊断：P5D_DBG_RTDUMP=1 时，稳态帧回读离屏 RT 内容，判定真实背景是否被渲染出来�?
    //   AA9DC000 = frag_0002（背�?tone-map）产物；ADD04000 = frag_0005 产物；cd = �?RT�?
    { const char* _rtd = getenv("P5D_DBG_RTDUMP");
      // ★★�?ARCH_441【每 N 帧自动抓图：一次运�?= 一�?胶片"】★★★
      //   动机（效率）：`P5D_RTDUMP_AT` 只在**首次越过**某个帧时�?3 �?—�?
      //   想比�?5 个帧就要�?5 次（每次 3~8 分钟）；而且目标帧没到达�?*一个文件都不产生�?
      //   日志里也毫无提示**（本会话因此白烧过两次运行）�?
      //   现在：`P5D_RTDUMP_EVERY=<N>`（配�?`P5D_RTDUMP_FROM=<�?`，默�?0�?
      //   �?N 帧抓一张送显面与一�?swapchain，文件名带帧号：
      //     film_<fc>_main.bmp / film_<fc>_swap.bmp
      //   �?一次运行就能看到整条时间线�?画面何时变化"，再对变化点做定点分析�?
      { static const char* _re=getenv("P5D_RTDUMP_EVERY");
        if(_re&&*_re){ long _every=strtol(_re,nullptr,10);
          if(_every>0){
            static int _rf0=-1; static SceUInt64 _from=0;
            if(_rf0<0){ _rf0=0; const char* _r=getenv("P5D_RTDUMP_FROM");
              if(_r&&*_r) _from=(SceUInt64)strtoull(_r,nullptr,10);
              printf("[FILM] P5D_RTDUMP_EVERY=%ld from=%llu（一次运行输出整条胶片）\n",_every,(unsigned long long)_from);
              fflush(stdout); }
            SceUInt64 _fc4=g_backend.frame_count;
            if(_fc4>=_from && ((_fc4-_from)%(SceUInt64)_every)==0){
              char _fn[80];
              std::snprintf(_fn,sizeof(_fn),"film_%06llu_main.bmp",(unsigned long long)_fc4);
              save_rt_screenshot(cd,_fn);
              std::snprintf(_fn,sizeof(_fn),"film_%06llu_swap.bmp",(unsigned long long)_fc4);
              save_screenshot(_fn);
              static int _fl=0; if(_fl++<6){ printf("[FILM] fc=%llu 已写 film_%06llu_{main,swap}.bmp\n",
                (unsigned long long)_fc4,(unsigned long long)_fc4); fflush(stdout); } } } } }


      //   实测画面只在 frame 100~500 存在，因此必须能在早期帧回读，否则永远看到「全黑」这个假前提�?
      static const char* _rat = getenv("P5D_RTDUMP_AT");
      SceUInt64 _rthr = (_rat && *_rat) ? (SceUInt64)strtoull(_rat,nullptr,10) : 1400ull;
      if (_rtd && *_rtd && g_backend.frame_count >= _rthr) {
        static int _rtn = 0;
        if (_rtn < 3) { _rtn++;
          char _fn[64];
          std::snprintf(_fn, sizeof(_fn), "rt_AA9DC000_%d.bmp", _rtn); save_rt_screenshot(0xAA9DC000u, _fn);
          std::snprintf(_fn, sizeof(_fn), "rt_ADD04000_%d.bmp", _rtn); save_rt_screenshot(0xADD04000u, _fn);
          std::snprintf(_fn, sizeof(_fn), "rt_main_%d.bmp", _rtn);     save_rt_screenshot(cd, _fn);
          // h144k30【同帧对拍】在**同一�?present_frame** 里同时抓 swapchain �?rt�?
          //   �?rt 有内容�?swapchain 没有，说明问题在 vkCmdCopyImage(rt→swapchain) 这一段�?
          { std::snprintf(_fn,sizeof(_fn),"seq_same_%d.bmp",_rtn); save_screenshot(_fn); }
          // h144k27【整池回读】P5D_RTDUMP_ALL=1：遍�?g_rt_pool �?*每一�?*中间面都回读出来�?
          //   用来回答「到底哪张面有真实内容、哪张一直是黑」——不必再靠猜 cd�?
          { const char* _ra=getenv("P5D_RTDUMP_ALL");
            if(_ra&&*_ra){ int _pi=0;
              for(std::map<SceUInt32,OffscreenRT>::iterator _it=g_rt_pool.begin(); _it!=g_rt_pool.end() && _pi<10; ++_it,++_pi){
                std::snprintf(_fn,sizeof(_fn),"rt_pool%d_c%08X_%d.bmp",_pi,_it->first,_rtn);
                save_rt_screenshot(_it->first,_fn); } } }
          // h144k24【指定颜色面回读】P5D_RTDUMP_CD=0xA159C000（可逗号分隔多个�?
          { const char* _rc=getenv("P5D_RTDUMP_CD");
            if(_rc&&*_rc){ std::string _s(_rc); size_t _p=0; int _ci=0;
              while(_p<=_s.size() && _ci<8){ size_t _c=_s.find(',',_p);
                std::string _t=(_c==std::string::npos)?_s.substr(_p):_s.substr(_p,_c-_p);
                if(!_t.empty()){ SceUInt32 _a=(SceUInt32)strtoul(_t.c_str(),nullptr,16);
                  std::snprintf(_fn,sizeof(_fn),"rt_c%08X_%d.bmp",_a,_rtn); save_rt_screenshot(_a,_fn); _ci++; }
                if(_c==std::string::npos) break; _p=_c+1; } } }
        } } }

    VkPresentInfoKHR present_info{}; present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1; present_info.pWaitSemaphores = signal_semaphores;
    present_info.swapchainCount = 1;
    VkSwapchainKHR swapchains[] = { *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain) };
    present_info.pSwapchains = swapchains; present_info.pImageIndices = &g_backend.current_image_index;
    result = vkQueuePresentKHR(*reinterpret_cast<VkQueue*>(&g_backend.present_queue), &present_info);
    // h145k727 分段计时输出（前 20 �?+ �?500 次）
    { auto _pf_t2 = std::chrono::steady_clock::now();
      static int _pfn = 0; _pfn++;
      if (_pfn <= 20 || (_pfn % 500) == 0) {
          double _pre  = std::chrono::duration<double, std::milli>(_pf_t1 - _pf_t0).count();
          double _post = std::chrono::duration<double, std::milli>(_pf_t2 - _pf_t1).count();
          printf("[PFTIME] #%d 前置(RT拷贝/barrier/命令录完)=%.3f ms  submit+present=%.3f ms  合计=%.3f ms\n",
                 _pfn, _pre, _post, _pre + _post);
          fflush(stdout);
      } }
    // �?h145k566【present 结果常备可见】原实现只对 OUT_OF_DATE/SUBOPTIMAL 做处理，
    //   其它错误（如 DEVICE_LOST�?*静默忽略**，且不打�?�?"上屏是否成功"不可知�?
    { static int _pn=0; _pn++;
      if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR && result!=VK_ERROR_OUT_OF_DATE_KHR)
        printf("[PRESENT] #%d imgIdx=%u result=%d(%s) **上屏失败**\n",_pn,
          (unsigned)g_backend.current_image_index,(int)result,vk_result_string(result));
      else if(_pn<=8)
        printf("[PRESENT] #%d imgIdx=%u result=%d(%s)\n",_pn,
          (unsigned)g_backend.current_image_index,(int)result,vk_result_string(result));
      fflush(stdout); }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        recreate_swapchain(g_backend.swapchain_width, g_backend.swapchain_height);
    // �?h145k567：present 成功后该 image �?layout 变为 PRESENT_SRC_KHR（供下一帧的 barrier 使用�?
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        if (_sci < 8u) s_sc_layout[_sci] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    g_backend.current_cmd_buffer = nullptr; g_frame_open = false;
}

// h144p: 命中离屏 RT colorData -> 返回�?view（barrier �?SHADER_READ_ONLY），供纹�?unit 别名采样
// h144k26：优先返�?**快照副本 aliasView** —�?它由 begin_scene_pass 在本 pass 开始渲染前 copy 好，
//   永远停在 SHADER_READ_ONLY，因此即使「同一张面既被渲染又被采样」也能合法采样（原实现是 ALIAS-SKIP 跳过 �?噪声）�?
VkImageView acquire_rt_alias_view(SceUInt32 cd){
    auto it=g_rt_pool.find(cd);
    if(it==g_rt_pool.end()) return VK_NULL_HANDLE;
    OffscreenRT& rt=it->second;
    if(rt.aliasView && rt.aliasLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){
        static long _an=0; if(_an<12){ _an++; fprintf(stderr,"[ALIAS-USE] cd=%08X -> alias view (layout=SHADER_READ_ONLY)\n",cd); fflush(stderr); }
        // ★★�?h145k342【A/B 判决：alias 快照 vs 主图】P5D_ALIAS_MAIN=1 ★★�?
        //   动机：alias 快照�?在本 pass 开始渲�?*之前** copy 好的"（见上方 4338 行注释）�?
        //   若后续合�?pass 采样的是这份快照，它拿到的是**�?pass 开始前**的内容（上一�?全黑�?
        //   �?�?内容确实画了、但 RTDUMP 读到�?自洽，也解释了为�?P5D_ONLY_FH 隔离后仍黑�?
        //   本开关让本函�?*改返回主图视�?* `rt.view`（即实时内容），做一次干净 A/B�?
        //     开了之后画面出�?�?根因�?快照取样时机"；仍然黑 �?快照无关，继续往下查�?
        { static int _am=-1; if(_am<0) _am=std::getenv("P5D_ALIAS_MAIN")?1:0;
          if(_am){ static int _c=0; if(_c<6){ _c++; fprintf(stderr,"[ALIAS-MAIN] cd=%08X 改返回主�?rt.view（A/B）\n",cd); fflush(stderr); }
            return rt.view; } }
        return rt.aliasView;
    }
    VkCommandBuffer cmd=g_backend.current_cmd_buffer;
    if(rt.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && cmd && !g_backend.in_render_pass){
        VkImageMemoryBarrier b{}; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.image=rt.img; b.oldLayout=(VkImageLayout)rt.layout; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.srcAccessMask=(rt.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:0;
        b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkPipelineStageFlags src=(rt.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)?
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        vkCmdPipelineBarrier(cmd,src,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        rt.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    else if(rt.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && g_backend.in_render_pass){
        static int _ab=0; if(_ab<16){_ab++; fprintf(stderr,"[ALIAS-SKIP] cd=%08X layout=%d needs transition inside RP (rely on pass-end barrier)\n",cd,(int)rt.layout); fflush(stderr);} }
    return rt.view;
}

SceInt32 begin_frame() {
    if (!g_backend.initialized) return -1;

    // 等待上一帧完�?
    vkWaitForFences(*reinterpret_cast<VkDevice*>(&g_backend.device), 1,
        reinterpret_cast<VkFence*>(&g_backend.frame_fence), VK_TRUE, UINT64_MAX);
    vkResetFences(*reinterpret_cast<VkDevice*>(&g_backend.device), 1,
        reinterpret_cast<VkFence*>(&g_backend.frame_fence));

    // 获取交换链图�?
    VkResult result = vkAcquireNextImageKHR(
        *reinterpret_cast<VkDevice*>(&g_backend.device),
        *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain),
        UINT64_MAX,
        *reinterpret_cast<VkSemaphore*>(&g_backend.image_available),
        VK_NULL_HANDLE,
        &g_backend.current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        LOGI("Swapchain out of date, recreating...");
        recreate_swapchain(g_backend.swapchain_width, g_backend.swapchain_height);
        return -1; // 重试
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR failed: %s", vk_result_string(result));
        return -1;
    }

    // 开始命令缓冲区
    g_backend.current_cmd_buffer = begin_command_buffer();
    if (!g_backend.current_cmd_buffer) {
        LOGE("Failed to begin command buffer");
        return -1;
    }

    // 开始渲染通道
    cmd_begin_render_pass(g_backend.current_cmd_buffer, g_backend.current_image_index);
    g_backend.frame_count++;
    fprintf(stderr,"[FRAMEBEGIN] frame=%llu img=%u cmd=%p\n",(unsigned long long)g_backend.frame_count,g_backend.current_image_index,(void*)g_backend.current_cmd_buffer);
    return 0;
}

void end_frame() {
    if (!g_backend.current_cmd_buffer) return;

    { FILE* f=fopen("ef_debug.log","a"); if(f){fprintf(f,"end_render_pass\n");fclose(f);} }
    cmd_end_render_pass(g_backend.current_cmd_buffer);
    { FILE* f=fopen("ef_debug.log","a"); if(f){fprintf(f,"end_command_buffer\n");fclose(f);} }
    end_command_buffer(g_backend.current_cmd_buffer);
    { FILE* f=fopen("ef_debug.log","a"); if(f){fprintf(f,"before submit\n");fclose(f);} }

    fprintf(stderr,"[FRAMESUBMIT] frame=%llu img=%u cmd=%p inRP=%d\n",(unsigned long long)g_backend.frame_count,g_backend.current_image_index,(void*)g_backend.current_cmd_buffer,(int)g_backend.in_render_pass);
    // 提交命令缓冲�?
    VkSemaphore wait_semaphores[] = { *reinterpret_cast<VkSemaphore*>(&g_backend.image_available) };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signal_semaphores[] = { *reinterpret_cast<VkSemaphore*>(&g_backend.render_finished) };

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &g_backend.current_cmd_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    VkResult result = vkQueueSubmit(
        *reinterpret_cast<VkQueue*>(&g_backend.graphics_queue),
        1, &submit_info,
        *reinterpret_cast<VkFence*>(&g_backend.frame_fence));
    { FILE* f=fopen("ef_debug.log","a"); if(f){fprintf(f,"after submit result=%d\n",result);fclose(f);} }
    if (result != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed: %s", vk_result_string(result));
    }

    // 【诊断】submit 完成后、present 前，对第一帧截一张（save_screenshot 开头会 QueueWaitIdle 等渲染落地）
    // 帧捕获必须在 present 之前：交换链 image �?vkQueuePresentKHR 后所有权交给呈现引擎�?
    // present 之后再当 TRANSFER_SRC 回读会得到未定义/全黑。submit �?fence、save_screenshot 开�?QueueWaitIdle 等渲染落地�?
    { static int __cap=0; static const SceUInt64 __mk[4]={1500ull,2500ull,3500ull,4500ull};
      if(__cap<4 && g_backend.frame_count>=__mk[__cap]){ char __fn[48]; std::snprintf(__fn,sizeof(__fn),"p5d_frame_%d.bmp",__cap+1);
        SceInt32 __r=save_screenshot(__fn); if(__r==0)__cap++; } }
    if(g_force_capture[0]){ save_screenshot(g_force_capture); g_force_capture[0]=0; }

    // 呈现
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;
    present_info.swapchainCount = 1;
    VkSwapchainKHR swapchains[] = { *reinterpret_cast<VkSwapchainKHR*>(&g_backend.swapchain) };
    present_info.pSwapchains = swapchains;
    present_info.pImageIndices = &g_backend.current_image_index;

    { FILE* f=fopen("ef_debug.log","a"); if(f){fprintf(f,"before present\n");fclose(f);} }
    result = vkQueuePresentKHR(
        *reinterpret_cast<VkQueue*>(&g_backend.present_queue), &present_info);
    { FILE* f=fopen("ef_debug.log","a"); if(f){fprintf(f,"after present result=%d\n",result);fclose(f);} }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(g_backend.swapchain_width, g_backend.swapchain_height);
    } else if (result != VK_SUCCESS) {
        LOGE("vkQueuePresentKHR failed: %s", vk_result_string(result));
    }

    g_backend.current_cmd_buffer = nullptr;
}

SceInt32 backend_init_with_window(const WindowConfig& config) {
    LOGI("=== Vulkan Backend Init with Window ===");
    if (g_backend.initialized) {
        LOGW("Already initialized");
        return 0;
    }

    if (create_window(config) != 0) return -1;
    if (create_instance() != 0) return -1;
    if (create_surface() != 0) return -1;
    if (pick_physical_device() != 0) return -1;
    if (create_logical_device() != 0) return -1;
    if (create_command_pool() != 0) return -1;
    if (create_swapchain() != 0) return -1;
    if (create_swapchain_image_views() != 0) return -1;
    if (create_render_pass() != 0) return -1;
    if (create_framebuffers() != 0) return -1;
    if (create_graphics_pipeline() != 0) return -1;
    if (create_vertex_buffer() != 0) return -1;
    if (create_index_buffer() != 0) return -1;
    if (create_uniform_buffer() != 0) return -1;
    if (create_dummy_texture() != 0) return -1;
    if (create_descriptor_pool_and_set() != 0) return -1;
    if (create_sync_objects() != 0) return -1;

    g_backend.initialized = true;
    g_backend.frame_count = 0;
    LOGI("=== Vulkan Backend Initialized with Swapchain (ready to display) ===");
    p5d_t_mark("Vulkan 后端初始化完成（可上屏）");
    return 0;
}

SceInt32 backend_init() {
    // 无窗口模式（离屏渲染�?
    WindowConfig config;
    return backend_init_with_window(config);
}

void backend_shutdown() {
    if (!g_backend.initialized && !g_backend.window_created) return;
    LOGI("Vulkan backend shutting down...");

    if (g_backend.device) {
        vkDeviceWaitIdle(*reinterpret_cast<VkDevice*>(&g_backend.device));
        cleanup_swapchain();
        if (g_backend.vertex_buffer)
            vkDestroyBuffer(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkBuffer*>(&g_backend.vertex_buffer), nullptr);
        if (g_backend.vertex_buffer_memory)
            vkFreeMemory(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkDeviceMemory*>(&g_backend.vertex_buffer_memory), nullptr);
        if (g_backend.graphics_pipeline)
            vkDestroyPipeline(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkPipeline*>(&g_backend.graphics_pipeline), nullptr);
        if (g_backend.pipeline_layout)
            vkDestroyPipelineLayout(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkPipelineLayout*>(&g_backend.pipeline_layout), nullptr);
        if (g_backend.frame_fence)
            vkDestroyFence(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkFence*>(&g_backend.frame_fence), nullptr);
        if (g_backend.image_available)
            vkDestroySemaphore(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkSemaphore*>(&g_backend.image_available), nullptr);
        if (g_backend.render_finished)
            vkDestroySemaphore(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkSemaphore*>(&g_backend.render_finished), nullptr);
        if (g_backend.render_pass)
            vkDestroyRenderPass(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkRenderPass*>(&g_backend.render_pass), nullptr);
        if (g_backend.command_pool)
            vkDestroyCommandPool(*reinterpret_cast<VkDevice*>(&g_backend.device),
                *reinterpret_cast<VkCommandPool*>(&g_backend.command_pool), nullptr);
        vkDestroyDevice(*reinterpret_cast<VkDevice*>(&g_backend.device), nullptr);
    }
    if (g_backend.surface)
        vkDestroySurfaceKHR(*reinterpret_cast<VkInstance*>(&g_backend.instance),
            *reinterpret_cast<VkSurfaceKHR*>(&g_backend.surface), nullptr);
    if (g_backend.instance)
        vkDestroyInstance(*reinterpret_cast<VkInstance*>(&g_backend.instance), nullptr);

    destroy_window();
    memset(&g_backend, 0, sizeof(g_backend));
    LOGI("Vulkan backend shut down");
}

} // namespace vulkan
} // namespace p5d

#else // P5D_HAS_VULKAN stub



namespace p5d {
namespace vulkan {
BackendState g_backend;
const char* vk_result_string(VkResult r) { (void)r; return "Vulkan not available"; }
SceUInt32 find_memory_type(SceUInt32 a, SceUInt32 b) { (void)a; (void)b; return 0; }
SceInt32 create_window(const WindowConfig& c) { (void)c; return -1; }
void destroy_window() {}
bool process_window_events() { return true; }
SceInt32 backend_init() { LOGW("Vulkan not compiled in"); g_backend.initialized = false; return -1; }
SceInt32 backend_init_with_window(const WindowConfig& c) { (void)c; return -1; }
void backend_shutdown() {}
SceInt32 create_instance() { return -1; }
SceInt32 create_surface() { return -1; }
SceInt32 pick_physical_device() { return -1; }
SceInt32 create_logical_device() { return -1; }
SceInt32 create_command_pool() { return -1; }
SceInt32 create_swapchain() { return -1; }
SceInt32 create_swapchain_image_views() { return -1; }
SceInt32 create_render_pass() { return -1; }
SceInt32 create_framebuffers() { return -1; }
void cleanup_swapchain() {}
SceInt32 recreate_swapchain(SceUInt32 w, SceUInt32 h) { (void)w; (void)h; return -1; }
SceInt32 create_graphics_pipeline() { return -1; }
SceInt32 create_vertex_buffer() { return -1; }
SceInt32 create_uniform_buffer() { return -1; }   // ★ §18.109：档分支本应为纯档(原误留了真实现体)

SceInt32 create_descriptor_pool_and_set() { return -1; }   // ★§18.110 纯档

void update_uniform_buffer(const float* matrix4x4) {
    if (g_backend.uniform_buffer_mapped) {
        memcpy(g_backend.uniform_buffer_mapped, matrix4x4, 64);
    }
}

void cmd_bind_descriptor_set(VkCommandBuffer cmd) { (void)cmd; }   // ★§18.110 纯档

SceInt32 create_sync_objects() { return -1; }
VkCommandBuffer begin_command_buffer() { return nullptr; }
void end_command_buffer(VkCommandBuffer c) { (void)c; }
void submit_command_buffer(VkCommandBuffer c) { (void)c; }
void cmd_begin_render_pass(VkCommandBuffer c, SceUInt32 i) { (void)c; (void)i; }
void cmd_end_render_pass(VkCommandBuffer c) { (void)c; }
void cmd_set_viewport(VkCommandBuffer c, SceFloat x, SceFloat y, SceFloat w, SceFloat h) { (void)c; (void)x; (void)y; (void)w; (void)h; }
void cmd_set_scissor(VkCommandBuffer c, SceUInt32 x, SceUInt32 y, SceUInt32 w, SceUInt32 h) { (void)c; (void)x; (void)y; (void)w; (void)h; }
void cmd_bind_pipeline(VkCommandBuffer c) { (void)c; }
void cmd_bind_vertex_buffer(VkCommandBuffer c, VkBuffer b) { (void)c; (void)b; }
void cmd_draw(VkCommandBuffer c, SceUInt32 a, SceUInt32 b, SceUInt32 d, SceUInt32 e) { (void)c; (void)a; (void)b; (void)d; (void)e; }
void cmd_draw_indexed(VkCommandBuffer c, SceUInt32 a, SceUInt32 b, SceUInt32 d, SceInt32 e, SceUInt32 f) { (void)c; (void)a; (void)b; (void)d; (void)e; (void)f; }
SceInt32 begin_frame() { return -1; }
void end_frame() {}
void request_capture(const char*){}
} // namespace vulkan
} // namespace p5d

// ★ §18.146：档分支一次补齐最后 13 个符号（按 §18.145 清单）
namespace p5d {
namespace vulkan {
void end_scene_pass() {}
void set_fragment_blend(unsigned int v) { (void)v; }
// ★ §18.148：按真分支形态补（SceInt32 / unsigned long long）
SceInt32 select_real_pipeline(const char* vpath, const char* fpath) { (void)vpath; (void)fpath; return -1; }
unsigned long long g_cur_draw_fh = 0;
void set_gxm_viewport(float a, float b, float c, float d, float e, float f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void set_gxm_viewport_enable(int v) { (void)v; }
void update_fragment_uniform_buffer_raw(void const* p, unsigned int n) { (void)p; (void)n; }
void update_uniform_buffer_raw(void const* p, unsigned int n) { (void)p; (void)n; }
void gxm_draw_indexed_real(void const* a, unsigned int b, void const* c, unsigned int d, unsigned int e, unsigned int f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
// ★ §18.148b：按真分支形态补
unsigned long long g_cur_draw_fh_dup_marker = 0;   // 占位(待改名)
// (moved out: 形态待核实)
unsigned int g_cur_prim = 0;
unsigned int g_frame_content_draws = 0;
}  // namespace vulkan
}  // namespace p5d
namespace p5d {
namespace tex {
unsigned int component_count_for_base_format(unsigned int v) { (void)v; return 0; }
bool parse_sce_gxm_texture(unsigned char const* d, SceGxmTextureInfo& o) { (void)d; (void)o; return false; }
}  // namespace tex
}  // namespace p5d

// ★ §18.143：档分支补 begin_scene_pass（真分支 L4901：void(SceUInt32,SceUInt32,SceUInt32)）
namespace p5d {
namespace vulkan {
void begin_scene_pass(SceUInt32 cd, SceUInt32 w, SceUInt32 h) { (void)cd; (void)w; (void)h; }
}  // namespace vulkan
}  // namespace p5d

// ★ §18.140：档分支再补 3 个 void 符号（形态已双处印证）
extern "C" void p5d_movie_set_frame(const void* bgra, int w, int h) { (void)bgra; (void)w; (void)h; }
namespace p5d {
namespace vulkan {
void set_texsrc_addr(SceUInt32 addr) { (void)addr; }
void present_frame(SceUInt32 cd) { (void)cd; }
}  // namespace vulkan
}  // namespace p5d

// ★ §18.136：档分支补形态正确的 3 个 movie 符号（已核实：const char* / void / void）
extern "C" const char* p5d_movie_requested(void) { return nullptr; }
extern "C" void p5d_movie_clear(void) {}
extern "C" void p5d_movend_arm(void) {}

// ★ §18.133：档分支再补 3 个符号（真分支在 L4423/L4440/L5178）
namespace p5d {
namespace vulkan {
SceInt32 rt_pool_probe(int maxn) { (void)maxn; return -1; }
SceInt32 save_rt_screenshot(SceUInt32 cd, const char* filename) { (void)cd; (void)filename; return -1; }
}  // namespace vulkan
}  // namespace p5d
extern "C" unsigned p5d_movie_request_seq(void) { return 0; }

// ★ §18.127：档分支补 p5d_stage_clock（结构体全局）与 p5d_stg_note
// ★ §18.150：已删除（eboot_probe.cpp 自带定义 ⇒ 避免 multiple definition）

// ★ §18.122：档分支再补 3 个 movie 符号（真分支在 L3287/L5172/L5188）
extern "C" void p5d_movie_request(const char* name) { (void)name; }
extern "C" void p5d_movie_opened_notify(void) {}
extern "C" int  p5d_movend_armed(void) { return 0; }

// ★ §18.117：档分支再补两个缺失符号
namespace p5d {
namespace vulkan {
void gxm_draw_arrays(unsigned int count) { (void)count; }
}  // namespace vulkan
extern "C" int p5d_drawlog_active(void) { return 0; }   // ★§18.118：它是函数，不是变量
}  // namespace p5d

// ★ §18.116：档分支补两个缺失的定义（真分支里有；此处需自带 namespace）
namespace p5d {
namespace vulkan {
MaterializeStats materialize_fragment_textures(const void* const*) { return MaterializeStats{}; }
}  // namespace vulkan
}  // namespace p5d

namespace p5d {
namespace tex {
bool resolve_gxm_texture(const SceUInt8* control16, ResolvedGxmTexture& out) {
    (void)control16; (void)out; return false;
}
}  // namespace tex
}  // namespace p5d

#endif


// ★ §18.187：直接计数（不依赖 frame 代理指标）
// 背景：归档的 [PRES]/[PROG] 插桩已不在 ⇒ 据 ARCH_692 新红线，直接测量本身
#include <cstdio>
namespace p5d { namespace vulkan {
long g_n_present = 0;
long g_n_begin_pass = 0;
long g_n_draw_arrays = 0;
struct P5DCountReport {
    ~P5DCountReport() {
        std::fprintf(stderr, "[COUNT] present=%ld begin_pass=%ld draw=%ld\n",
                     g_n_present, g_n_begin_pass, g_n_draw_arrays);
        std::fflush(stderr);
    }
};
static P5DCountReport g_count_report;
} }
