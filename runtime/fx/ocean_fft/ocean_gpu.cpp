// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see LICENSE.
#include "ocean_gpu.h"
#include <array>
#include <limits>
#include <new>

namespace ocean_fft {
namespace {
struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
struct alignas(16) Push {
    std::array<uint32_t, 4> grid{}, pass{};
    std::array<float, 4> wind{}, tiles{}, detail{}, frame{};
};
static_assert(sizeof(Push) == 96, "GLSL push layout differs");
enum { Initial, Ping, Pong, Displacement, Normal, Foam, ImageCount };
void dependency(VkCommandBuffer cb, VkPipelineStageFlags source, VkAccessFlags access,
                VkPipelineStageFlags destination, VkAccessFlags needed) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = access; barrier.dstAccessMask = needed;
    vkCmdPipelineBarrier(cb, source, destination, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
}  // namespace

struct OceanGpu {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    Params params;
    Push push;
    std::array<Image, ImageCount> images;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> pipelines{};
    bool initialized = false;
    double seconds = 0;
};

namespace {
bool make_image(OceanGpu* o, Image& out, VkFormat format, uint32_t layers) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
    info.extent = {o->params.resolution, o->params.resolution, 1};
    info.mipLevels = 1; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(o->device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(o->device, out.image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(o->physical, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(o->device, &allocation, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindImageMemory(o->device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    return vkCreateImageView(o->device, &view, nullptr, &out.view) == VK_SUCCESS;
}
bool make_pipeline(OceanGpu* o, Spirv code, VkPipeline& pipeline) {
    if (!code.words || code.count < 5 || code.count > (std::numeric_limits<size_t>::max)() / sizeof(uint32_t) || code.words[0] != 0x07230203u) return false;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = code.count * sizeof(uint32_t); module_info.pCode = code.words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(o->device, &module_info, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module = module; compute.stage.pName = "main";
    compute.layout = o->layout;
    const VkResult result = vkCreateComputePipelines(o->device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline);
    vkDestroyShaderModule(o->device, module, nullptr);
    return result == VK_SUCCESS;
}
void dispatch(OceanGpu* o, VkCommandBuffer cb, unsigned pipeline, unsigned layers) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, o->pipelines[pipeline]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, o->layout, 0, 1, &o->set, 0, nullptr);
    vkCmdPushConstants(cb, o->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &o->push);
    const uint32_t groups = (o->params.resolution + 7) / 8;
    vkCmdDispatch(cb, groups, groups, layers);
    dependency(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
}  // namespace

OceanGpu* create(const CreateInfo& info, std::string* error) {
    if (error) error->clear();
    if (!validate(info.params, error)) return nullptr;
    auto reject = [&](const char* text) { if (error) *error = text; return static_cast<OceanGpu*>(nullptr); };
    if (!info.physical || !info.device || !info.storage_image_extended_formats_enabled)
        return reject("device/physical device and enabled shaderStorageImageExtendedFormats are required");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(info.physical, &properties);
    if (properties.limits.maxImageArrayLayers < kFieldCount * info.params.cascades ||
        properties.limits.maxPerStageDescriptorStorageImages < ImageCount ||
        properties.limits.maxDescriptorSetStorageImages < ImageCount || properties.limits.maxPushConstantsSize < sizeof(Push) ||
        properties.limits.maxComputeWorkGroupInvocations < 64 || properties.limits.maxComputeWorkGroupSize[0] < 8 ||
        properties.limits.maxComputeWorkGroupSize[1] < 8 || properties.limits.maxComputeWorkGroupCount[2] < kFieldCount * info.params.cascades ||
        properties.limits.maxComputeWorkGroupCount[0] < (info.params.resolution + 7) / 8 ||
        properties.limits.maxComputeWorkGroupCount[1] < (info.params.resolution + 7) / 8)
        return reject("device storage-image, array, push-constant, or compute limits are too small");
    for (VkFormat format : {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}) {
        VkFormatProperties features{}; vkGetPhysicalDeviceFormatProperties(info.physical, format, &features);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((features.optimalTilingFeatures & need) != need) return reject("float image storage and linear sampling are not supported");
    }
    OceanGpu* o = new (std::nothrow) OceanGpu;
    if (!o) return reject("host allocation failed");
    o->device = info.device; o->physical = info.physical; o->params = info.params;
    auto fail = [&](const char* message) { destroy(o); return reject(message); };
    for (unsigned i = 0; i < ImageCount; ++i) {
        const unsigned layers = (i == Ping || i == Pong) ? info.params.cascades * kFieldCount : info.params.cascades;
        const VkFormat format = (i == Ping || i == Pong || i == Foam) ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        if (!make_image(o, o->images[i], format, layers)) return fail("image allocation, bind, or view creation failed");
    }
    std::array<VkDescriptorSetLayoutBinding, ImageCount> bindings{};
    for (unsigned i = 0; i < ImageCount; ++i) {
        bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = ImageCount; set_layout.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(o->device, &set_layout, nullptr, &o->set_layout) != VK_SUCCESS) return fail("descriptor layout failed");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &o->set_layout; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(o->device, &layout, nullptr, &o->layout) != VK_SUCCESS) return fail("pipeline layout failed");
    const std::array<Spirv, 4> shaders{info.init, info.evolve, info.ifft, info.output};
    for (unsigned i = 0; i < shaders.size(); ++i) if (!make_pipeline(o, shaders[i], o->pipelines[i])) return fail("compute pipeline failed; check SPIR-V inputs");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ImageCount};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    if (vkCreateDescriptorPool(o->device, &pool, nullptr, &o->pool) != VK_SUCCESS) return fail("descriptor pool failed");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = o->pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &o->set_layout;
    if (vkAllocateDescriptorSets(o->device, &allocate, &o->set) != VK_SUCCESS) return fail("descriptor allocation failed");
    std::array<VkDescriptorImageInfo, ImageCount> image_info{};
    std::array<VkWriteDescriptorSet, ImageCount> writes{};
    for (unsigned i = 0; i < ImageCount; ++i) {
        image_info[i].imageView = o->images[i].view; image_info[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = o->set; writes[i].dstBinding = i;
        writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[i].pImageInfo = &image_info[i];
    }
    vkUpdateDescriptorSets(o->device, ImageCount, writes.data(), 0, nullptr);
    unsigned log_n = 0; for (unsigned n = info.params.resolution; n > 1; n >>= 1) ++log_n;
    o->push.grid = {info.params.resolution, log_n, info.params.cascades, info.params.seed};
    const double angle = info.params.wind_degrees * kPi / 180;
    o->push.wind = {info.params.wind_mph * (5280.0f / 3600.0f), float(std::cos(angle)), float(std::sin(angle)), info.params.fetch_feet};
    o->push.tiles = {info.params.tile_feet[0], info.params.tile_feet[1], info.params.tile_feet[2], info.params.amplitude};
    o->push.detail = {info.params.choppiness, info.params.damping_feet, info.params.foam_threshold, info.params.foam_decay};
    o->push.frame = {0, 0, info.params.foam_gain, float(kGravity)};
    return o;
}

bool update(OceanGpu* o, VkCommandBuffer cb, double dt, Outputs* outputs, std::string* error) {
    if (error) error->clear();
    if (!o || !cb || !std::isfinite(dt) || dt < 0 || dt > kMaxDeltaSeconds || (o && o->seconds + dt > kMaxSeconds)) {
        if (error) *error = "valid ocean/command buffer, dt in [0,3600], and elapsed time at most 10000000 seconds are required";
        return false;
    }
    o->seconds += dt; o->push.frame[0] = float(o->seconds); o->push.frame[1] = float(dt);
    dependency(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!o->initialized) {
        std::array<VkImageMemoryBarrier, ImageCount> barriers{};
        for (unsigned i = 0; i < ImageCount; ++i) {
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[i].image = o->images[i].image;
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, ImageCount, barriers.data());
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, o->params.cascades};
        VkClearColorValue clear{};
        vkCmdClearColorImage(cb, o->images[Displacement].image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        clear.float32[1] = 1;
        vkCmdClearColorImage(cb, o->images[Normal].image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        vkCmdClearColorImage(cb, o->images[Foam].image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        dependency(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        dispatch(o, cb, 0, o->params.cascades);
        o->initialized = true;
    }
    dispatch(o, cb, 1, o->params.cascades);
    o->push.pass[2] = 0;
    for (unsigned axis = 0; axis < 2; ++axis) for (unsigned stage = 0; stage < o->push.grid[1]; ++stage) {
        o->push.pass[0] = axis; o->push.pass[1] = stage;
        dispatch(o, cb, 2, o->params.cascades * kFieldCount);
        o->push.pass[2] ^= 1;
    }
    dispatch(o, cb, 3, o->params.cascades);
    dependency(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    if (outputs) *outputs = {o->images[Displacement].view, o->images[Normal].view, o->images[Foam].view,
                             o->params.resolution, o->params.cascades, VK_IMAGE_LAYOUT_GENERAL};
    return true;
}

void destroy(OceanGpu* o) {
    if (!o) return;
    for (VkPipeline p : o->pipelines) if (p) vkDestroyPipeline(o->device, p, nullptr);
    if (o->pool) vkDestroyDescriptorPool(o->device, o->pool, nullptr);
    if (o->layout) vkDestroyPipelineLayout(o->device, o->layout, nullptr);
    if (o->set_layout) vkDestroyDescriptorSetLayout(o->device, o->set_layout, nullptr);
    for (Image& image : o->images) {
        if (image.view) vkDestroyImageView(o->device, image.view, nullptr);
        if (image.image) vkDestroyImage(o->device, image.image, nullptr);
        if (image.memory) vkFreeMemory(o->device, image.memory, nullptr);
    }
    delete o;
}
}  // namespace ocean_fft
