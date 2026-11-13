#include "urt/render/renderer.hpp"

#include "urt/core/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

// SPIR-V produced from shaders/scene.vert and scene.frag (see render/CMakeLists.txt).
#include "generated/scene_frag.h"
#include "generated/scene_vert.h"

namespace urt::render {
namespace {

constexpr const char* validation_layer_name = "VK_LAYER_KHRONOS_validation";

// Logs and returns false from the enclosing function if a Vulkan call fails.
#define URT_VK_CHECK(call, what)                                                              \
    do {                                                                                      \
        const VkResult urt_vk_result = (call);                                                \
        if (urt_vk_result != VK_SUCCESS) {                                                    \
            ::urt::log::error("render", "{} failed (VkResult {})", what,                      \
                              static_cast<int>(urt_vk_result));                               \
            return false;                                                                     \
        }                                                                                     \
    } while (false)

struct Vertex {
    float position[3];
    float normal[3];
};

// Sent with every draw call. Must match the Push block in the shaders.
struct PushConstants {
    glm::mat4 mvp;
    glm::vec4 color;
    glm::vec4 params;
    glm::vec4 light;
};
static_assert(sizeof(PushConstants) == 112, "must match the shader's push constant block");

constexpr std::uint32_t cube_vertex_count = 36;

// A cube from -0.5 to 0.5 on every axis: 6 faces, 2 triangles each.
std::vector<Vertex> make_cube() {
    struct Face {
        glm::vec3 normal;
        glm::vec3 u;
        glm::vec3 v;
    };
    const std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    }};

    std::vector<Vertex> vertices;
    vertices.reserve(cube_vertex_count);
    for (const Face& face : faces) {
        const glm::vec3 centre = face.normal * 0.5f;
        const glm::vec3 corners[4] = {
            centre - face.u * 0.5f - face.v * 0.5f,
            centre + face.u * 0.5f - face.v * 0.5f,
            centre + face.u * 0.5f + face.v * 0.5f,
            centre - face.u * 0.5f + face.v * 0.5f,
        };
        for (const int index : {0, 1, 2, 0, 2, 3}) {
            const glm::vec3& p = corners[index];
            vertices.push_back(Vertex{{p.x, p.y, p.z}, {face.normal.x, face.normal.y, face.normal.z}});
        }
    }
    return vertices;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user_data) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        static_cast<std::atomic<std::uint32_t>*>(user_data)->fetch_add(1);
        log::error("vulkan", "{}", data->pMessage);
    } else {
        log::warn("vulkan", "{}", data->pMessage);
    }
    return VK_FALSE; // never abort the call that triggered the message
}

} // namespace

struct Renderer::Impl {
    SurfaceProvider provider;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    bool validation = false;
    std::atomic<std::uint32_t> validation_errors{0};

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    std::string gpu_name;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t graphics_family = 0;
    std::uint32_t present_family = 0;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;

    // Everything from here to `framebuffers` depends on the window size and is
    // rebuilt when the window is resized.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    int provider_width = 0;  // window size the swapchain was built for
    int provider_height = 0;
    bool swapchain_ready = false;
    bool swapchain_stale = false; // the driver asked for a rebuild
    bool can_capture = false;
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> render_finished; // one per swapchain image

    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence frame_done = VK_NULL_HANDLE;

    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;

    std::string screenshot_path; // empty = none requested

    ~Impl() { destroy(); }

    bool create_instance(bool want_validation);
    bool pick_gpu();
    bool create_device();
    bool create_swapchain();
    bool create_render_pass();
    bool create_pipeline();
    bool create_frame_objects();
    bool create_vertex_buffer();
    bool recreate_swapchain();
    void destroy_swapchain_objects();
    void destroy();

    bool find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags wanted, std::uint32_t& out) const;
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                       VkDeviceMemory& memory) const;
    void record(std::uint32_t image_index, const Camera& camera, std::span<const Box> boxes,
                VkBuffer capture_buffer) const;
    bool write_screenshot(VkDeviceMemory memory, const std::string& path) const;
};

// --- setup --------------------------------------------------------------------

bool Renderer::Impl::create_instance(bool want_validation) {
    std::vector<const char*> extensions = provider.instance_extensions;
    std::vector<const char*> layers;

    if (want_validation) {
        std::uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        const bool found = std::any_of(available.begin(), available.end(), [](const VkLayerProperties& layer) {
            return std::strcmp(layer.layerName, validation_layer_name) == 0;
        });
        if (found) {
            layers.push_back(validation_layer_name);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validation = true;
        } else {
            log::warn("render", "Vulkan validation layer not found (it is part of the Vulkan SDK)");
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "URT";
    app.pEngineName = "URT";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    URT_VK_CHECK(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");

    if (validation) {
        // Extension functions are not exported by the loader; look them up.
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT debug{};
            debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = debug_callback;
            debug.pUserData = &validation_errors;
            URT_VK_CHECK(create(instance, &debug, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");
        }
    }
    return true;
}

bool Renderer::Impl::pick_gpu() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    int best_score = -1;
    for (const VkPhysicalDevice candidate : devices) {
        // The device must be able to present to our surface...
        std::uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, extensions.data());
        const bool has_swapchain =
            std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            });
        if (!has_swapchain) {
            continue;
        }

        // ...and have a queue family for drawing and one for presenting.
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());

        bool has_graphics = false;
        bool has_present = false;
        std::uint32_t graphics = 0;
        std::uint32_t present = 0;
        for (std::uint32_t i = 0; i < family_count; ++i) {
            const bool draws = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            VkBool32 presents = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &presents);
            if (draws && presents == VK_TRUE) {
                // One family doing both is the common and simplest case.
                graphics = present = i;
                has_graphics = has_present = true;
                break;
            }
            if (draws && !has_graphics) {
                graphics = i;
                has_graphics = true;
            }
            if (presents == VK_TRUE && !has_present) {
                present = i;
                has_present = true;
            }
        }
        if (!has_graphics || !has_present) {
            continue;
        }

        std::uint32_t format_count = 0;
        std::uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &format_count, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &mode_count, nullptr);
        if (format_count == 0 || mode_count == 0) {
            continue;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        int score = 0;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score = 2; // a dedicated graphics card beats the one built into the CPU
        } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score = 1;
        }
        if (score > best_score) {
            best_score = score;
            gpu = candidate;
            gpu_name = properties.deviceName;
            graphics_family = graphics;
            present_family = present;
        }
    }

    if (gpu == VK_NULL_HANDLE) {
        log::error("render", "no graphics device can draw to this window");
        return false;
    }
    return true;
}

bool Renderer::Impl::create_device() {
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    for (const std::uint32_t family : {graphics_family, present_family}) {
        if (!queues.empty() && queues.front().queueFamilyIndex == family) {
            continue; // same family for both: ask for it once
        }
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        queues.push_back(queue);
    }

    const char* const extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
    info.pQueueCreateInfos = queues.data();
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    URT_VK_CHECK(vkCreateDevice(gpu, &info, nullptr, &device), "vkCreateDevice");

    vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
    vkGetDeviceQueue(device, present_family, 0, &present_queue);
    return true;
}

bool Renderer::Impl::find_memory_type(std::uint32_t type_bits, VkMemoryPropertyFlags wanted,
                                      std::uint32_t& out) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const bool allowed = (type_bits & (1u << i)) != 0;
        if (allowed && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            out = i;
            return true;
        }
    }
    log::error("render", "no suitable GPU memory type");
    return false;
}

bool Renderer::Impl::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                   VkDeviceMemory& memory) const {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    URT_VK_CHECK(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    // Host visible: the CPU can map and read or write it directly.
    if (!find_memory_type(requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          allocate.memoryTypeIndex)) {
        return false;
    }
    URT_VK_CHECK(vkAllocateMemory(device, &allocate, nullptr, &memory), "vkAllocateMemory");
    URT_VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
    return true;
}

// Builds the swapchain and everything sized like the window. Leaves
// swapchain_ready false (and returns true) while the window has no area.
bool Renderer::Impl::create_swapchain() {
    swapchain_ready = false;
    swapchain_stale = false;
    provider.framebuffer_size(provider_width, provider_height);

    VkSurfaceCapabilitiesKHR caps{};
    URT_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps),
                 "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    // Most systems dictate the size; some leave it to us (marked by UINT32_MAX).
    VkExtent2D new_extent = caps.currentExtent;
    if (new_extent.width == std::numeric_limits<std::uint32_t>::max()) {
        new_extent.width = std::clamp(static_cast<std::uint32_t>(std::max(provider_width, 0)),
                                      caps.minImageExtent.width, caps.maxImageExtent.width);
        new_extent.height = std::clamp(static_cast<std::uint32_t>(std::max(provider_height, 0)),
                                       caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (new_extent.width == 0 || new_extent.height == 0) {
        return true; // minimised: try again later
    }

    std::uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, formats.data());
    if (formats.empty()) {
        log::error("render", "the surface reports no image formats");
        return false;
    }
    // Plain 8-bit colour without automatic gamma conversion, so the colours
    // in the code are the colours on screen.
    VkSurfaceFormatKHR chosen = formats.front();
    for (const VkSurfaceFormatKHR& format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = format;
            break;
        }
    }

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    // Screenshots copy from the swapchain image, which needs this usage flag.
    can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & alpha) == 0) {
        for (const VkCompositeAlphaFlagBitsKHR option :
             {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if ((caps.supportedCompositeAlpha & option) != 0) {
                alpha = option;
                break;
            }
        }
    }

    const VkSwapchainKHR old_swapchain = swapchain;
    const std::uint32_t families[] = {graphics_family, present_family};

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = new_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (can_capture) {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (graphics_family != present_family) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; every driver has it
    info.clipped = VK_TRUE;
    info.oldSwapchain = old_swapchain;
    URT_VK_CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, old_swapchain, nullptr);
    }
    color_format = chosen.format;
    extent = new_extent;

    std::uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &actual_count, nullptr);
    images.resize(actual_count);
    URT_VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &actual_count, images.data()),
                 "vkGetSwapchainImagesKHR");

    image_views.assign(images.size(), VK_NULL_HANDLE);
    for (std::size_t i = 0; i < images.size(); ++i) {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = color_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        URT_VK_CHECK(vkCreateImageView(device, &view, nullptr, &image_views[i]), "vkCreateImageView");
    }

    // Depth buffer: remembers how far the nearest drawn surface is at each
    // pixel, so nearer boxes hide farther ones regardless of draw order.
    if (depth_format == VK_FORMAT_UNDEFINED) {
        for (const VkFormat candidate :
             {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(gpu, candidate, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                depth_format = candidate;
                break;
            }
        }
        if (depth_format == VK_FORMAT_UNDEFINED) {
            log::error("render", "no depth buffer format is supported");
            return false;
        }
    }

    VkImageCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth.imageType = VK_IMAGE_TYPE_2D;
    depth.format = depth_format;
    depth.extent = {extent.width, extent.height, 1};
    depth.mipLevels = 1;
    depth.arrayLayers = 1;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    URT_VK_CHECK(vkCreateImage(device, &depth, nullptr, &depth_image), "vkCreateImage (depth)");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, depth_image, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    if (!find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          allocate.memoryTypeIndex)) {
        return false;
    }
    URT_VK_CHECK(vkAllocateMemory(device, &allocate, nullptr, &depth_memory), "vkAllocateMemory (depth)");
    URT_VK_CHECK(vkBindImageMemory(device, depth_image, depth_memory, 0), "vkBindImageMemory (depth)");

    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = depth_image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = depth_format;
    depth_view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    URT_VK_CHECK(vkCreateImageView(device, &depth_view_info, nullptr, &depth_view),
                 "vkCreateImageView (depth)");

    // The render pass needs the formats, so it is created with the first
    // swapchain and reused afterwards.
    if (render_pass == VK_NULL_HANDLE && !create_render_pass()) {
        return false;
    }

    framebuffers.assign(images.size(), VK_NULL_HANDLE);
    render_finished.assign(images.size(), VK_NULL_HANDLE);
    for (std::size_t i = 0; i < images.size(); ++i) {
        const VkImageView attachments[] = {image_views[i], depth_view};
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = render_pass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = attachments;
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        URT_VK_CHECK(vkCreateFramebuffer(device, &framebuffer, nullptr, &framebuffers[i]),
                     "vkCreateFramebuffer");

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        URT_VK_CHECK(vkCreateSemaphore(device, &semaphore, nullptr, &render_finished[i]),
                     "vkCreateSemaphore");
    }

    swapchain_ready = true;
    return true;
}

bool Renderer::Impl::create_render_pass() {
    VkAttachmentDescription attachments[2]{};
    // Colour: cleared at the start, kept at the end, then handed to the screen.
    attachments[0].format = color_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    // Depth: only needed while drawing.
    attachments[1].format = depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;

    // Wait for the swapchain image to be ours before the pass touches it.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = dependency.srcStageMask;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    URT_VK_CHECK(vkCreateRenderPass(device, &info, nullptr, &render_pass), "vkCreateRenderPass");
    return true;
}

bool Renderer::Impl::create_pipeline() {
    const auto make_module = [this](const std::uint32_t* code, std::size_t bytes, VkShaderModule& out) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes;
        info.pCode = code;
        return vkCreateShaderModule(device, &info, nullptr, &out);
    };
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    URT_VK_CHECK(make_module(scene_vert, sizeof(scene_vert), vertex_module), "vkCreateShaderModule (vertex)");
    if (make_module(scene_frag, sizeof(scene_frag), fragment_module) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vertex_module, nullptr);
        log::error("render", "vkCreateShaderModule (fragment) failed");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport and scissor are set per frame, so resizing the window does not
    // require a new pipeline.
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // boxes are cheap; the depth buffer sorts it out
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                         sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push_range;
    VkResult result = vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout);

    if (result == VK_SUCCESS) {
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipeline_layout;
        info.renderPass = render_pass;
        info.subpass = 0;
        result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    }

    // The pipeline keeps its own copy of the shader code.
    vkDestroyShaderModule(device, vertex_module, nullptr);
    vkDestroyShaderModule(device, fragment_module, nullptr);
    URT_VK_CHECK(result, "creating the graphics pipeline");
    return true;
}

bool Renderer::Impl::create_frame_objects() {
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = graphics_family;
    URT_VK_CHECK(vkCreateCommandPool(device, &pool, nullptr, &command_pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    URT_VK_CHECK(vkAllocateCommandBuffers(device, &allocate, &command_buffer), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo semaphore{};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    URT_VK_CHECK(vkCreateSemaphore(device, &semaphore, nullptr, &image_available), "vkCreateSemaphore");

    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first frame does not wait forever
    URT_VK_CHECK(vkCreateFence(device, &fence, nullptr, &frame_done), "vkCreateFence");
    return true;
}

bool Renderer::Impl::create_vertex_buffer() {
    const std::vector<Vertex> cube = make_cube();
    const VkDeviceSize size = cube.size() * sizeof(Vertex);
    if (!create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_memory)) {
        return false;
    }
    void* mapped = nullptr;
    URT_VK_CHECK(vkMapMemory(device, vertex_memory, 0, size, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, cube.data(), static_cast<std::size_t>(size));
    vkUnmapMemory(device, vertex_memory);
    return true;
}

// --- teardown -----------------------------------------------------------------

void Renderer::Impl::destroy_swapchain_objects() {
    for (const VkSemaphore semaphore : render_finished) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (const VkFramebuffer framebuffer : framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    render_finished.clear();
    framebuffers.clear();

    vkDestroyImageView(device, depth_view, nullptr);
    vkDestroyImage(device, depth_image, nullptr);
    vkFreeMemory(device, depth_memory, nullptr);
    depth_view = VK_NULL_HANDLE;
    depth_image = VK_NULL_HANDLE;
    depth_memory = VK_NULL_HANDLE;

    for (const VkImageView view : image_views) {
        vkDestroyImageView(device, view, nullptr);
    }
    image_views.clear();
    images.clear();
    // The swapchain itself stays: create_swapchain() passes it on as
    // oldSwapchain and destroys it afterwards.
}

bool Renderer::Impl::recreate_swapchain() {
    // Nothing may still be using the objects we are about to destroy.
    vkDeviceWaitIdle(device);
    destroy_swapchain_objects();
    return create_swapchain();
}

void Renderer::Impl::destroy() {
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        destroy_swapchain_objects();
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyBuffer(device, vertex_buffer, nullptr);
        vkFreeMemory(device, vertex_memory, nullptr);
        vkDestroyFence(device, frame_done, nullptr);
        vkDestroySemaphore(device, image_available, nullptr);
        vkDestroyCommandPool(device, command_pool, nullptr); // frees its command buffers too
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        vkDestroyRenderPass(device, render_pass, nullptr);
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (messenger != VK_NULL_HANDLE) {
            const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_messenger != nullptr) {
                destroy_messenger(instance, messenger, nullptr);
            }
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

// --- drawing ------------------------------------------------------------------

void Renderer::Impl::record(std::uint32_t image_index, const Camera& camera,
                            std::span<const Box> boxes, VkBuffer capture_buffer) const {
    const VkCommandBuffer cmd = command_buffer;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clear[2]{};
    clear[0].color = {{0.10f, 0.11f, 0.13f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0}; // 1.0 = as far away as possible

    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass;
    pass.framebuffer = framebuffers[image_index];
    pass.renderArea = {{0, 0}, extent};
    pass.clearValueCount = 2;
    pass.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                              static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);

    // Camera. Vulkan's clip space has depth 0..1 and Y pointing down, unlike
    // OpenGL, hence the ZO ("zero to one") projection and the flipped Y.
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(camera.fov_degrees), aspect, 0.1f, 500.0f);
    projection[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAtRH(camera.eye, camera.target, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 view_projection = projection * view;
    const glm::vec3 light_world = glm::normalize(glm::vec3{0.4f, 1.0f, 0.6f});

    const auto draw_cube = [&](const PushConstants& push) {
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &push);
        vkCmdDraw(cmd, cube_vertex_count, 1, 0, 0);
    };

    // Ground: the cube flattened into a large thin slab whose top is at y = 0.
    // It follows the camera target; the grid lines come from world
    // coordinates in the fragment shader, so they stay put.
    {
        constexpr float ground_size = 400.0f;
        constexpr float thickness = 0.02f;
        glm::mat4 model = glm::translate(
            glm::mat4{1.0f}, glm::vec3{camera.target.x, -thickness * 0.5f, camera.target.z});
        model = glm::scale(model, glm::vec3{ground_size, thickness, ground_size});
        PushConstants push{};
        push.mvp = view_projection * model;
        push.color = {0.16f, 0.17f, 0.20f, 1.0f};
        push.params = {1.0f, ground_size, camera.target.x, camera.target.z};
        push.light = {light_world, 0.0f};
        draw_cube(push);
    }

    for (const Box& box : boxes) {
        // Scale, then rotate, then move (matrices apply right to left).
        // A positive rotation around Y turns +X towards -Z; our yaw turns +X
        // towards +Z, hence the minus.
        const glm::mat4 rotation = glm::rotate(glm::mat4{1.0f}, -box.yaw, glm::vec3{0.0f, 1.0f, 0.0f});
        glm::mat4 model = glm::translate(glm::mat4{1.0f}, box.position + glm::vec3{0.0f, box.size.y * 0.5f, 0.0f});
        model = model * rotation;
        model = glm::scale(model, box.size);

        PushConstants push{};
        push.mvp = view_projection * model;
        push.color = {box.color, 1.0f};
        push.params = {0.0f, 0.0f, 0.0f, 0.0f};
        // The shader lights in object space, so bring the light there by
        // undoing the box's rotation (the transpose of a rotation is its inverse).
        push.light = {glm::transpose(glm::mat3{rotation}) * light_world, 0.0f};
        draw_cube(push);
    }

    vkCmdEndRenderPass(cmd);

    if (capture_buffer != VK_NULL_HANDLE) {
        // Screenshot: borrow the finished image for a copy, then put it back
        // into the state the screen expects.
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[image_index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, images[image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               capture_buffer, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(cmd);
}

bool Renderer::Impl::write_screenshot(VkDeviceMemory memory, const std::string& path) const {
    const std::size_t pixel_count = static_cast<std::size_t>(extent.width) * extent.height;
    void* mapped = nullptr;
    URT_VK_CHECK(vkMapMemory(device, memory, 0, pixel_count * 4, 0, &mapped), "vkMapMemory (screenshot)");
    const auto* pixels = static_cast<const std::uint8_t*>(mapped);

    // Some surfaces store blue first. PPM wants red, green, blue.
    const bool blue_first = color_format == VK_FORMAT_B8G8R8A8_UNORM || color_format == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<char> rgb(pixel_count * 3);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        rgb[i * 3 + 0] = static_cast<char>(pixels[i * 4 + (blue_first ? 2 : 0)]);
        rgb[i * 3 + 1] = static_cast<char>(pixels[i * 4 + 1]);
        rgb[i * 3 + 2] = static_cast<char>(pixels[i * 4 + (blue_first ? 0 : 2)]);
    }
    vkUnmapMemory(device, memory);

    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
    file.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    if (!file) {
        log::error("render", "cannot write screenshot to '{}'", path);
        return false;
    }
    log::info("render", "saved screenshot to '{}'", path);
    return true;
}

// --- public interface ------------------------------------------------------------

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::init(const SurfaceProvider& surface, const RendererConfig& config) {
    Impl& r = *impl_;
    if (r.instance != VK_NULL_HANDLE) {
        log::error("render", "the renderer is already initialised");
        return false;
    }
    if (!surface.create_surface || !surface.framebuffer_size) {
        log::error("render", "the surface provider is incomplete");
        return false;
    }
    r.provider = surface;

    if (!r.create_instance(config.validation)) {
        return false;
    }
    r.surface = r.provider.create_surface(r.instance);
    if (r.surface == VK_NULL_HANDLE) {
        log::error("render", "cannot create a drawing surface for the window");
        return false;
    }
    if (!r.pick_gpu() || !r.create_device() || !r.create_frame_objects() || !r.create_vertex_buffer() ||
        !r.create_swapchain()) {
        return false;
    }
    // The pipeline needs the render pass, which needs the first swapchain. If
    // the window started minimised, both are created with the first frame.
    if (r.swapchain_ready && !r.create_pipeline()) {
        return false;
    }
    log::info("render", "Vulkan renderer ready on '{}'{}", r.gpu_name,
              r.validation ? " with validation" : "");
    return true;
}

bool Renderer::draw_frame(const Camera& camera, std::span<const Box> boxes) {
    Impl& r = *impl_;
    if (r.device == VK_NULL_HANDLE) {
        return false;
    }

    int width = 0;
    int height = 0;
    r.provider.framebuffer_size(width, height);
    if (width <= 0 || height <= 0) {
        return true; // minimised: nothing to draw on
    }
    if (!r.swapchain_ready || r.swapchain_stale || width != r.provider_width || height != r.provider_height) {
        if (!r.recreate_swapchain()) {
            return false;
        }
        if (!r.swapchain_ready) {
            return true;
        }
    }
    if (r.pipeline == VK_NULL_HANDLE && !r.create_pipeline()) {
        return false;
    }

    // Wait until the GPU has finished the previous frame; then its command
    // buffer is free to be reused.
    vkWaitForFences(r.device, 1, &r.frame_done, VK_TRUE, std::numeric_limits<std::uint64_t>::max());

    std::uint32_t image_index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(r.device, r.swapchain, std::numeric_limits<std::uint64_t>::max(),
                                                    r.image_available, VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        r.swapchain_stale = true; // rebuilt at the start of the next frame
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        log::error("render", "vkAcquireNextImageKHR failed (VkResult {})", static_cast<int>(acquired));
        return false;
    }

    // Screenshot target, if one was requested and the surface allows copies.
    VkBuffer capture_buffer = VK_NULL_HANDLE;
    VkDeviceMemory capture_memory = VK_NULL_HANDLE;
    std::string capture_path;
    if (!r.screenshot_path.empty()) {
        capture_path = std::exchange(r.screenshot_path, {});
        const VkDeviceSize size = static_cast<VkDeviceSize>(r.extent.width) * r.extent.height * 4;
        if (!r.can_capture) {
            log::warn("render", "this surface does not allow screenshots");
        } else if (!r.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, capture_buffer, capture_memory)) {
            return false;
        }
    }

    vkResetFences(r.device, 1, &r.frame_done);
    vkResetCommandBuffer(r.command_buffer, 0);
    r.record(image_index, camera, boxes, capture_buffer);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &r.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &r.command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &r.render_finished[image_index];
    URT_VK_CHECK(vkQueueSubmit(r.graphics_queue, 1, &submit, r.frame_done), "vkQueueSubmit");

    if (capture_buffer != VK_NULL_HANDLE) {
        vkWaitForFences(r.device, 1, &r.frame_done, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
        r.write_screenshot(capture_memory, capture_path);
        vkDestroyBuffer(r.device, capture_buffer, nullptr);
        vkFreeMemory(r.device, capture_memory, nullptr);
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &r.render_finished[image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &r.swapchain;
    present.pImageIndices = &image_index;
    const VkResult presented = vkQueuePresentKHR(r.present_queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        r.swapchain_stale = true;
    } else if (presented != VK_SUCCESS) {
        log::error("render", "vkQueuePresentKHR failed (VkResult {})", static_cast<int>(presented));
        return false;
    }
    return true;
}

void Renderer::request_screenshot(std::string path) { impl_->screenshot_path = std::move(path); }

std::uint32_t Renderer::validation_error_count() const { return impl_->validation_errors.load(); }

bool Renderer::validation_active() const { return impl_->validation; }

const std::string& Renderer::device_name() const { return impl_->gpu_name; }

} // namespace urt::render
