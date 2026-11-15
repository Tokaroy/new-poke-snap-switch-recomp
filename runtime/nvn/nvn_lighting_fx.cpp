// SPDX-License-Identifier: MIT
// "Enhanced lighting" screen-space pass (project code, ours): GTAO ambient occlusion plus denoised contact shadows as one Fx pre-tonemap effect.
//   SNAP_LIGHTING=enhanced|maximum turns it on (runtime/hle/lighting_preset.h); SNAP_LIGHT_FX=1|0 forces it on / off whatever the preset says.
//   Three full-screen passes per scene frame at render resolution, on the linear HDR scene colour before the upscaler and the tonemap:
//     1. lfx_ao.frag      GTAO (horizon search in screen-space slices) + the contact-shadow march, written noisy into an RG8 image
//     2. lfx_resolve.frag depth-aware 5x5 bilateral blur + temporal accumulation (camera-motion reprojection, view-distance rejection, neighbourhood clamp)
//     3. lfx_comp.frag    colour *= AO factor * contact factor (multiply blend; the distance fade lives here)
//   The game draws no ambient occlusion in the course (its HBAO only runs near the photo select), so this adds a look the original does not have. Photo and
//   Evaluation frames are skipped by the Fx plumbing (nvn_fx.cpp), so photo scores do not depend on it. While it is on, the older SNAP_CONTACT_SHADOWS effect
//   (nvn_contact.cpp, the same march without the denoise) stays off; its sky gate (outdoors only) is reused here.
//   Tunables (env, test aids): SNAP_LIGHT_AO=0 (AO off), SNAP_LIGHT_CONTACT=0 (contact shadows off), SNAP_LIGHT_AO_RADIUS world units (1.0),
//   SNAP_LIGHT_AO_STRENGTH (0.85), SNAP_LIGHT_AO_POWER (1.6), SNAP_LIGHT_CONTACT_STRENGTH (0.6), SNAP_LIGHT_CONTACT_LEN (0.5), SNAP_LIGHT_CONTACT_THICK (0.2),
//   SNAP_LIGHT_FADE view distance (60), SNAP_LIGHT_ALPHA temporal weight of the newest frame (0.1), SNAP_LIGHT_DEBUG=1 AO only, 2 contact only,
//   3 raw (unfiltered) AO, SNAP_LIGHT_AB=<P> (see lighting_preset.h).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/lighting_preset.h"
#include "nvn_fx.h"
#include "nvn_internal.h"
#include "nvn_lighting_spv.h"

namespace nvn {
namespace {
struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; };
struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
  VkShaderModule fsAo = VK_NULL_HANDLE, fsRes = VK_NULL_HANDLE, fsComp = VK_NULL_HANDLE;
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
  VkPipeline pipeAo = VK_NULL_HANDLE, pipeRes = VK_NULL_HANDLE, pipeComp = VK_NULL_HANDLE, pipeDbg = VK_NULL_HANDLE; VkFormat sceneFmt = VK_FORMAT_UNDEFINED;
  Img raw, hist[2]; uint32_t w = 0, h = 0; int cur = 0;
  bool haveHist = false; int lastPresent = -1000; double prevVp[4][4] = {};
} g;

float EnvF(const char* n, float def) { const char* e = getenv(n); return e && *e ? (float)atof(e) : def; }
bool EnvOff(const char* n) { const char* e = getenv(n); return e && *e && atoi(e) == 0; }
bool Configured() {   // the preset / SNAP_LIGHT_FX say yes (not looking at the A/B test aid or a failed setup)
  static const int mode = [] { const char* e = getenv("SNAP_LIGHT_FX"); return e && *e ? (atoi(e) != 0 ? 1 : 0) : -1; }();
  return mode >= 0 ? mode == 1 : snaplight::enhanced();
}
bool Enabled() { return Configured() && !g.failed && snaplight::ab_on((unsigned long long)g_presentCount); }

struct PcAo { float ivp[16]; float dclip[4]; float light[4]; float size[4]; float q[4]; };
struct PcRes { float reproj[16]; float size[4]; float q[4]; };
struct PcComp { float k[4]; float size[4]; };
static_assert(sizeof(PcAo) == 128 && sizeof(PcRes) <= 128 && sizeof(PcComp) <= 128, "push constants must fit the 128 byte guarantee");

void ImgBar(VkCommandBuffer cb, Img& im, VkImageLayout to) {
  if (im.layout == to) return;
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = im.layout; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  im.layout = to;
}
bool MakeImg(Host* h, Img& o, uint32_t w, uint32_t hh, VkFormat fmt) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {w, hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  o = Img{};
  if (vkCreateImage(h->device, &ci, nullptr, &o.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) != VK_SUCCESS || vkBindImageMemory(h->device, o.img, o.mem, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = fmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS;
}

bool Setup(Host* h, VkFormat sceneFmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.nearest) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(h->device, &si, nullptr, &g.linear) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 3; dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    g.fsAo = FxMakeModule(h, kLfxAoFrag, sizeof kLfxAoFrag);
    g.fsRes = FxMakeModule(h, kLfxResolveFrag, sizeof kLfxResolveFrag);
    g.fsComp = FxMakeModule(h, kLfxCompFrag, sizeof kLfxCompFrag);
    if (!g.fsAo || !g.fsRes || !g.fsComp) return false;
    g.pipeAo = FxMakeFullscreenPipeline(h, g.pl, g.fsAo, VK_FORMAT_R8G8_UNORM, false);
    g.pipeRes = FxMakeFullscreenPipeline(h, g.pl, g.fsRes, VK_FORMAT_R16G16B16A16_SFLOAT, false);
    if (!g.pipeAo || !g.pipeRes) return false;
    g.ready = true;
  }
  if (g.sceneFmt != sceneFmt || !g.pipeComp) {   // the scene colour format is fixed (RGBA16F), so this runs once
    g.pipeComp = FxMakeFullscreenPipeline(h, g.pl, g.fsComp, sceneFmt, true);
    g.pipeDbg = FxMakeFullscreenPipeline(h, g.pl, g.fsComp, sceneFmt, false);
    g.sceneFmt = sceneFmt;
  }
  return g.pipeComp != VK_NULL_HANDLE;
}

bool EnsureImages(Host* h, uint32_t w, uint32_t hh) {
  if (g.w == w && g.h == hh && g.raw.img) return true;
  if (g.raw.img) fprintf(stderr, "lighting: render size changed to %ux%u, the old images are kept until exit\n", w, hh);   // a command buffer may still read them
  if (!MakeImg(h, g.raw, w, hh, VK_FORMAT_R8G8_UNORM) || !MakeImg(h, g.hist[0], w, hh, VK_FORMAT_R16G16B16A16_SFLOAT) || !MakeImg(h, g.hist[1], w, hh, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
  g.w = w; g.h = hh; g.haveHist = false; g.cur = 0;
  return true;
}

// One full-screen pass into one of our own images (whole image, contents not needed), then a memory barrier like EndRendering.
void DrawTo(ReplayCtx& cx, Img& t, VkPipeline pipe, VkDescriptorSet set, const void* pc, uint32_t pcBytes) {
  VkCommandBuffer cb = cx.cb;
  ImgBar(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  ca.imageView = t.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {g.w, g.h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
  vkCmdBeginRendering(cb, &ri);
  VkViewport vp{0, 0, (float)g.w, (float)g.h, 0, 1}; VkRect2D sc{{0, 0}, {g.w, g.h}};
  vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pl, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cb, g.pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcBytes, pc);
  vkCmdDraw(cb, 3, 1, 0, 0);
  vkCmdEndRendering(cb);
  ImgBar(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Bind(Host* h, VkDescriptorSet set, uint32_t binding, VkSampler s, VkImageView v, VkImageLayout l) {
  VkDescriptorImageInfo ii{s, v, l};
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
  vkUpdateDescriptorSets(h->device, 1, &w, 0, nullptr);
}

void Pre(ReplayCtx& cx) {
  if (g.failed) return;
  const FxCam* cam = FxCamera();
  TextureImpl* c = FxSceneColor(); TextureImpl* z = FxSceneDepth();
  if (!cam || !c || !z || c->HW() != z->HW() || c->HH() != z->HH() || !FxWritable(c)) return;
  const bool wantAo = !EnvOff("SNAP_LIGHT_AO"), wantCt = !EnvOff("SNAP_LIGHT_CONTACT");
  if (!wantAo && !wantCt) return;
  if (!Setup(cx.h, c->fi->vk) || !EnsureImages(cx.h, c->HW(), c->HH())) { g.failed = true; fprintf(stderr, "lighting: setup failed, disabled\n"); return; }

  // the sun, for contact shadows: only where there is a dynamic shadow pass and the sky is in view
  const FxCaster* cs = FxCasterNow();
  bool contact = wantCt && cs && ContactSkyGate(cx, z, cam->nearZ);
  float L[3] = {0, 1, 0};
  if (cs) {   // toward the light: the caster matrix' depth axis points away from the light (or toward it): take the sign that points up
    float sgn = EnvF("SNAP_CONTACT_LSIGN", 0.f);
    if (sgn == 0.f) sgn = cs->axis[1] > 0.f ? 1.f : -1.f;
    for (int i = 0; i < 3; i++) L[i] = cs->axis[i] * (sgn < 0 ? -1.f : 1.f);
  }
  static unsigned long long nTot = 0, nContact = 0;
  nTot++; nContact += contact;
  if (nTot % 600 == 0 && getenv("SNAP_FX_LOG")) fprintf(stderr, "lighting: p%d stats: %llu scene frames, contact shadows on %llu\n", g_presentCount, nTot, nContact);

  float jx, jy; FxJitter(&jx, &jy);
  const int lvl = snaplight::level();
  PcAo pa{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pa.ivp[k * 4 + r] = (float)cam->ivp[r][k];
  for (int r = 0; r < 4; r++) pa.dclip[r] = (float)(cam->vp[r][0] * L[0] + cam->vp[r][1] * L[1] + cam->vp[r][2] * L[2]);
  pa.light[0] = L[0]; pa.light[1] = L[1]; pa.light[2] = L[2]; pa.light[3] = (float)(g_presentCount & 1023);
  pa.size[0] = (float)g.w; pa.size[1] = (float)g.h; pa.size[2] = jx; pa.size[3] = jy;
  pa.q[0] = wantAo ? EnvF("SNAP_LIGHT_AO_RADIUS", 1.0f) : 0.f;
  pa.q[1] = contact ? EnvF("SNAP_LIGHT_CONTACT_LEN", 0.5f) : 0.f;
  pa.q[2] = EnvF("SNAP_LIGHT_CONTACT_THICK", 0.2f);
  pa.q[3] = (float)(lvl >= 2 ? 3 : 2);

  // history: usable when the previous result is from a recent scene frame at the same size (photo / menu gaps reset it)
  const bool histOk = g.haveHist && g_presentCount - g.lastPresent <= 4;
  PcRes pr{};
  {
    double R[4][4];   // reproj = prevVP * ivp: this frame's (ndc, depth, 1) -> the previous frame's clip space
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { double s = 0; for (int j = 0; j < 4; j++) s += g.prevVp[r][j] * cam->ivp[j][k]; R[r][k] = s; }
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pr.reproj[k * 4 + r] = (float)R[r][k];
  }
  pr.size[0] = (float)g.w; pr.size[1] = (float)g.h; pr.size[2] = histOk ? 1.f : 0.f; pr.size[3] = cam->nearZ;
  pr.q[0] = EnvF("SNAP_LIGHT_ALPHA", 0.1f);
  PcComp pk{};
  pk.k[0] = wantAo ? EnvF("SNAP_LIGHT_AO_STRENGTH", 0.85f) : 0.f; pk.k[1] = contact ? EnvF("SNAP_LIGHT_CONTACT_STRENGTH", 0.6f) : 0.f;
  pk.k[2] = EnvF("SNAP_LIGHT_FADE", 60.f); pk.k[3] = EnvF("SNAP_LIGHT_AO_POWER", 1.6f);
  pk.size[0] = (float)g.w; pk.size[1] = (float)g.h; pk.size[2] = EnvF("SNAP_LIGHT_DEBUG", 0.f);
  const bool rawDbg = pk.size[2] > 2.5f;   // 3: the raw AO factor, no resolve (shows what the denoise removes)
  if (rawDbg) pk.size[2] = 1.f;

  static int logged; if (logged++ < 3) fprintf(stderr, "lighting: p%d %ux%u AO %d contact %d light toward (%.3f %.3f %.3f) history %d\n", g_presentCount, g.w, g.h, (int)wantAo, (int)contact, L[0], L[1], L[2], (int)histOk);

  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  Img& out = g.hist[g.cur]; Img& old = g.hist[g.cur ^ 1];
  VkDescriptorSet s1 = FxAllocSet(cx, g.dsl), s2 = FxAllocSet(cx, g.dsl), s3 = FxAllocSet(cx, g.dsl);
  if (dv && s1 && s2 && s3) {
    // history image never written yet: give it a defined layout (its contents are not read while pr.size[2] is 0)
    if (old.layout == VK_IMAGE_LAYOUT_UNDEFINED) { ImgBar(cx.cb, old, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
    Bind(cx.h, s1, 0, g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    DrawTo(cx, g.raw, g.pipeAo, s1, &pa, sizeof pa);
    Bind(cx.h, s2, 0, g.nearest, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    Bind(cx.h, s2, 1, g.nearest, g.raw.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Bind(cx.h, s2, 2, g.linear, old.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    DrawTo(cx, out, g.pipeRes, s2, &pr, sizeof pr);
    Bind(cx.h, s3, 0, g.nearest, rawDbg ? g.raw.view : out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (rawDbg) pk.k[2] = 1e9f;
    const bool dbg = pk.size[2] > 0.5f;
    FxDrawFullscreen(cx, c, dbg && g.pipeDbg ? g.pipeDbg : g.pipeComp, g.pl, s3, &pk, sizeof pk);
    FxNoteRan();
    g.haveHist = true; g.lastPresent = g_presentCount; g.cur ^= 1;
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) g.prevVp[r][k] = cam->vp[r][k];
  }
  FxDepthDone(cx, z, prev);
}

const FxEffect kEffect = {"enhanced_lighting", Enabled, Pre, nullptr, true};
struct Reg { Reg() { FxRegister(&kEffect); } } reg;
}  // namespace

bool LightingFxEnabled() { return Configured(); }
}  // namespace nvn
