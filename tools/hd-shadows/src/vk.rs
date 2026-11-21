//! Vulkan ray query baker (ash). One acceleration structure per course level (solid triangles plus alpha-tested cards),
//! one compute dispatch per mesh object (bake) or per sun direction (calibration points).
use ash::vk;
use std::ffi::CStr;

pub struct Buf {
    pub buf: vk::Buffer,
    pub mem: vk::DeviceMemory,
    pub size: u64,
    pub addr: u64,
}

pub struct Gpu {
    _entry: ash::Entry,
    pub instance: ash::Instance,
    pub dev: ash::Device,
    pub phys: vk::PhysicalDevice,
    pub queue: vk::Queue,
    pub qfam: u32,
    pub pool: vk::CommandPool,
    pub mem_props: vk::PhysicalDeviceMemoryProperties,
    accel: ash::khr::acceleration_structure::Device,
    pub name: String,
    blas: Vec<vk::AccelerationStructureKHR>,
    tlas: vk::AccelerationStructureKHR,
    scene_bufs: Vec<Buf>,
    /// card triangles, mask table, mask bytes (bindings 4, 5, 6)
    card_bufs: Vec<Buf>,
    pipe: vk::Pipeline,
    pipe_points: vk::Pipeline,
    layout: vk::PipelineLayout,
    dsl: vk::DescriptorSetLayout,
    dpool: vk::DescriptorPool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Push {
    pub sun: [f32; 3],
    pub cone: f32,
    pub size: u32,
    pub samples: u32,
    pub bias: f32,
    pub tmin: f32,
    pub row_start: u32,
    pub seed: u32,
    /// FLAG_* of trace.glsl
    pub flags: u32,
}

pub const FLAG_NO_NDL: u32 = 1;
pub const FLAG_CULL_BACK: u32 = 2;
pub const FLAG_CULL_FRONT: u32 = 4;

/// Alpha-tested part of a scene: per card triangle (uv0 of its 3 corners, mask index) and the masks.
#[derive(Default)]
pub struct Cards {
    /// 32 bytes per card triangle, in the order of the card vertices given to `set_scene`
    pub tris: Vec<u8>,
    /// 16 bytes per mask: offset, w, h, ref
    pub masks: Vec<u8>,
    pub data: Vec<u8>,
}

/// Points uploaded once and traced for many sun directions.
pub struct PointSet {
    pts: Buf,
    pub n: u32,
}

const SPV_BAKE: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/bake.spv"));
const SPV_POINTS: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/points.spv"));

impl Gpu {
    pub fn new() -> Result<Gpu, String> {
        unsafe {
            let entry = ash::Entry::load().map_err(|e| format!("vulkan loader: {e}"))?;
            let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
            let inst = entry
                .create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None)
                .map_err(|e| format!("instance: {e}"))?;
            let mut best = None;
            for p in inst.enumerate_physical_devices().map_err(|e| e.to_string())? {
                let exts = inst.enumerate_device_extension_properties(p).unwrap_or_default();
                let has = |n: &CStr| exts.iter().any(|e| CStr::from_ptr(e.extension_name.as_ptr()) == n);
                if has(ash::khr::ray_query::NAME) && has(ash::khr::acceleration_structure::NAME) {
                    let pr = inst.get_physical_device_properties(p);
                    let score = if pr.device_type == vk::PhysicalDeviceType::DISCRETE_GPU { 2 } else { 1 };
                    if best.map_or(true, |(s, _)| score > s) {
                        best = Some((score, p));
                    }
                }
            }
            let (_, phys) = best.ok_or("no GPU with ray query (VK_KHR_ray_query)")?;
            let props = inst.get_physical_device_properties(phys);
            let name = CStr::from_ptr(props.device_name.as_ptr()).to_string_lossy().to_string();
            let qfam = inst
                .get_physical_device_queue_family_properties(phys)
                .iter()
                .position(|q| q.queue_flags.contains(vk::QueueFlags::COMPUTE))
                .ok_or("no compute queue")? as u32;
            let prio = [1.0f32];
            let qinfo = [vk::DeviceQueueCreateInfo::default().queue_family_index(qfam).queue_priorities(&prio)];
            let mut f12 = vk::PhysicalDeviceVulkan12Features::default().buffer_device_address(true).scalar_block_layout(true);
            let mut fas = vk::PhysicalDeviceAccelerationStructureFeaturesKHR::default().acceleration_structure(true);
            let mut frq = vk::PhysicalDeviceRayQueryFeaturesKHR::default().ray_query(true);
            let exts = [
                ash::khr::ray_query::NAME.as_ptr(),
                ash::khr::acceleration_structure::NAME.as_ptr(),
                ash::khr::deferred_host_operations::NAME.as_ptr(),
            ];
            let dev = inst
                .create_device(
                    phys,
                    &vk::DeviceCreateInfo::default()
                        .queue_create_infos(&qinfo)
                        .enabled_extension_names(&exts)
                        .push_next(&mut f12)
                        .push_next(&mut fas)
                        .push_next(&mut frq),
                    None,
                )
                .map_err(|e| format!("device: {e}"))?;
            let queue = dev.get_device_queue(qfam, 0);
            let pool = dev
                .create_command_pool(
                    &vk::CommandPoolCreateInfo::default().queue_family_index(qfam).flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER),
                    None,
                )
                .map_err(|e| e.to_string())?;
            let mem_props = inst.get_physical_device_memory_properties(phys);
            let accel = ash::khr::acceleration_structure::Device::new(&inst, &dev);

            // pipelines: one layout, binding 0 the scene, 1..3 per dispatch, 4..6 the scene's cards
            let mut binds = vec![vk::DescriptorSetLayoutBinding::default()
                .binding(0)
                .descriptor_type(vk::DescriptorType::ACCELERATION_STRUCTURE_KHR)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::COMPUTE)];
            for b in 1..=6 {
                binds.push(
                    vk::DescriptorSetLayoutBinding::default()
                        .binding(b)
                        .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                        .descriptor_count(1)
                        .stage_flags(vk::ShaderStageFlags::COMPUTE),
                );
            }
            let dsl = dev.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&binds), None).map_err(|e| e.to_string())?;
            let pcr = [vk::PushConstantRange::default().stage_flags(vk::ShaderStageFlags::COMPUTE).size(std::mem::size_of::<Push>() as u32)];
            let dsls = [dsl];
            let layout = dev
                .create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&dsls).push_constant_ranges(&pcr), None)
                .map_err(|e| e.to_string())?;
            let make = |spv: &[u8]| -> Result<vk::Pipeline, String> {
                let words: Vec<u32> = spv.chunks(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
                let module = dev.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None).map_err(|e| e.to_string())?;
                let stage = vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::COMPUTE).module(module).name(c"main");
                let p = dev
                    .create_compute_pipelines(vk::PipelineCache::null(), &[vk::ComputePipelineCreateInfo::default().stage(stage).layout(layout)], None)
                    .map_err(|e| format!("pipeline: {:?}", e.1))?[0];
                dev.destroy_shader_module(module, None);
                Ok(p)
            };
            let pipe = make(SPV_BAKE)?;
            let pipe_points = make(SPV_POINTS)?;
            let sizes = [
                vk::DescriptorPoolSize::default().ty(vk::DescriptorType::ACCELERATION_STRUCTURE_KHR).descriptor_count(16),
                vk::DescriptorPoolSize::default().ty(vk::DescriptorType::STORAGE_BUFFER).descriptor_count(96),
            ];
            let dpool = dev
                .create_descriptor_pool(&vk::DescriptorPoolCreateInfo::default().max_sets(16).pool_sizes(&sizes).flags(vk::DescriptorPoolCreateFlags::FREE_DESCRIPTOR_SET), None)
                .map_err(|e| e.to_string())?;
            Ok(Gpu {
                _entry: entry,
                instance: inst,
                dev,
                phys,
                queue,
                qfam,
                pool,
                mem_props,
                accel,
                name,
                blas: vec![],
                tlas: vk::AccelerationStructureKHR::null(),
                scene_bufs: vec![],
                card_bufs: vec![],
                pipe,
                pipe_points,
                layout,
                dsl,
                dpool,
            })
        }
    }

    fn mem_type(&self, bits: u32, want: vk::MemoryPropertyFlags) -> u32 {
        (0..self.mem_props.memory_type_count)
            .find(|&i| bits & (1 << i) != 0 && self.mem_props.memory_types[i as usize].property_flags.contains(want))
            .expect("memory type")
    }

    pub fn buffer(&self, size: u64, usage: vk::BufferUsageFlags, host: bool) -> Buf {
        unsafe {
            let buf = self
                .dev
                .create_buffer(&vk::BufferCreateInfo::default().size(size.max(16)).usage(usage | vk::BufferUsageFlags::SHADER_DEVICE_ADDRESS), None)
                .unwrap();
            let req = self.dev.get_buffer_memory_requirements(buf);
            let flags = if host {
                vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT
            } else {
                vk::MemoryPropertyFlags::DEVICE_LOCAL
            };
            let mut fl = vk::MemoryAllocateFlagsInfo::default().flags(vk::MemoryAllocateFlags::DEVICE_ADDRESS);
            let mem = self
                .dev
                .allocate_memory(
                    &vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(self.mem_type(req.memory_type_bits, flags)).push_next(&mut fl),
                    None,
                )
                .expect("alloc");
            self.dev.bind_buffer_memory(buf, mem, 0).unwrap();
            let addr = self.dev.get_buffer_device_address(&vk::BufferDeviceAddressInfo::default().buffer(buf));
            Buf { buf, mem, size: req.size, addr }
        }
    }

    pub fn free(&self, b: Buf) {
        unsafe {
            self.dev.destroy_buffer(b.buf, None);
            self.dev.free_memory(b.mem, None);
        }
    }

    pub fn upload(&self, b: &Buf, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        unsafe {
            let p = self.dev.map_memory(b.mem, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()).unwrap() as *mut u8;
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len());
            self.dev.unmap_memory(b.mem);
        }
    }

    fn host_buf(&self, bytes: &[u8]) -> Buf {
        let b = self.buffer(bytes.len().max(16) as u64, vk::BufferUsageFlags::STORAGE_BUFFER, true);
        self.upload(&b, bytes);
        b
    }

    fn one_shot(&self, f: impl FnOnce(vk::CommandBuffer)) {
        unsafe {
            let cb = self
                .dev
                .allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(self.pool).command_buffer_count(1))
                .unwrap()[0];
            self.dev.begin_command_buffer(cb, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT)).unwrap();
            f(cb);
            self.dev.end_command_buffer(cb).unwrap();
            let cbs = [cb];
            self.dev.queue_submit(self.queue, &[vk::SubmitInfo::default().command_buffers(&cbs)], vk::Fence::null()).unwrap();
            self.dev.queue_wait_idle(self.queue).unwrap();
            self.dev.free_command_buffers(self.pool, &cbs);
        }
    }

    fn drop_scene(&mut self) {
        unsafe {
            if self.tlas != vk::AccelerationStructureKHR::null() {
                self.accel.destroy_acceleration_structure(self.tlas, None);
                self.tlas = vk::AccelerationStructureKHR::null();
            }
            for b in std::mem::take(&mut self.blas) {
                self.accel.destroy_acceleration_structure(b, None);
            }
        }
        for b in std::mem::take(&mut self.scene_bufs).into_iter().chain(std::mem::take(&mut self.card_bufs)) {
            self.free(b);
        }
    }

    /// One bottom-level structure from triangle lists (3 vertices each); `true` = opaque geometry. Geometry order is kept,
    /// empty lists are skipped. Returns its device address.
    unsafe fn build_blas(&mut self, parts: &[(&[[f32; 3]], bool)]) -> Result<u64, String> {
        let mut all: Vec<[f32; 3]> = vec![];
        let mut spans = vec![];
        for (v, opaque) in parts {
            let n = (v.len() / 3) as u32;
            if n > 0 {
                spans.push((all.len() as u64, n, *opaque));
                all.extend_from_slice(&v[..n as usize * 3]);
            }
        }
        let vb = self.buffer(
            (all.len() * 12) as u64,
            vk::BufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR | vk::BufferUsageFlags::STORAGE_BUFFER,
            true,
        );
        self.upload(&vb, std::slice::from_raw_parts(all.as_ptr() as *const u8, all.len() * 12));
        let mut geo = vec![];
        let mut counts = vec![];
        for &(first, n, opaque) in &spans {
            let tri = vk::AccelerationStructureGeometryTrianglesDataKHR::default()
                .vertex_format(vk::Format::R32G32B32_SFLOAT)
                .vertex_data(vk::DeviceOrHostAddressConstKHR { device_address: vb.addr + first * 12 })
                .vertex_stride(12)
                .max_vertex(n * 3 - 1)
                .index_type(vk::IndexType::NONE_KHR);
            geo.push(
                vk::AccelerationStructureGeometryKHR::default()
                    .geometry_type(vk::GeometryTypeKHR::TRIANGLES)
                    .flags(if opaque { vk::GeometryFlagsKHR::OPAQUE } else { vk::GeometryFlagsKHR::empty() })
                    .geometry(vk::AccelerationStructureGeometryDataKHR { triangles: tri }),
            );
            counts.push(n);
        }
        let mut bi = vk::AccelerationStructureBuildGeometryInfoKHR::default()
            .ty(vk::AccelerationStructureTypeKHR::BOTTOM_LEVEL)
            .flags(vk::BuildAccelerationStructureFlagsKHR::PREFER_FAST_TRACE)
            .mode(vk::BuildAccelerationStructureModeKHR::BUILD)
            .geometries(&geo);
        let mut sz = vk::AccelerationStructureBuildSizesInfoKHR::default();
        self.accel.get_acceleration_structure_build_sizes(vk::AccelerationStructureBuildTypeKHR::DEVICE, &bi, &counts, &mut sz);
        let blas_buf = self.buffer(sz.acceleration_structure_size, vk::BufferUsageFlags::ACCELERATION_STRUCTURE_STORAGE_KHR, false);
        let scratch = self.buffer(sz.build_scratch_size + 256, vk::BufferUsageFlags::STORAGE_BUFFER, false);
        let blas = self
            .accel
            .create_acceleration_structure(
                &vk::AccelerationStructureCreateInfoKHR::default().buffer(blas_buf.buf).size(sz.acceleration_structure_size).ty(vk::AccelerationStructureTypeKHR::BOTTOM_LEVEL),
                None,
            )
            .map_err(|e| e.to_string())?;
        bi = bi.dst_acceleration_structure(blas).scratch_data(vk::DeviceOrHostAddressKHR { device_address: (scratch.addr + 127) & !127 });
        let ranges: Vec<_> = counts.iter().map(|&n| vk::AccelerationStructureBuildRangeInfoKHR::default().primitive_count(n)).collect();
        self.one_shot(|cb| {
            self.accel.cmd_build_acceleration_structures(cb, &[bi], &[&ranges]);
        });
        let addr = self.accel.get_acceleration_structure_device_address(&vk::AccelerationStructureDeviceAddressInfoKHR::default().acceleration_structure(blas));
        self.free(scratch);
        self.free(vb);
        self.blas.push(blas);
        self.scene_bufs.push(blas_buf);
        Ok(addr)
    }

    /// Builds the level's acceleration structure from world-space triangles (3 vertices each). `solid` triangles are
    /// opaque and, with FLAG_CULL_*, seen from one side only; `two_sided` ones are opaque and always seen; `card`
    /// triangles are alpha-tested through `cards` (one entry per card triangle, same order) and culled like `solid`
    /// (the game's own bakes match better that way: a leaf card shades from its front only).
    pub fn set_scene(&mut self, solid: &[[f32; 3]], two_sided: &[[f32; 3]], card: &[[f32; 3]], cards: &Cards) -> Result<(), String> {
        self.drop_scene();
        if solid.len() + two_sided.len() + card.len() < 3 {
            return Err("empty scene".into());
        }
        unsafe {
            let mut insts = vec![];
            if solid.len() + card.len() >= 3 {
                // the card geometry must be the only non-opaque one (trace.glsl reads its primitive index)
                insts.push((self.build_blas(&[(solid, true), (card, false)])?, 0u8));
            }
            if two_sided.len() >= 3 {
                insts.push((self.build_blas(&[(two_sided, true)])?, vk::GeometryInstanceFlagsKHR::TRIANGLE_FACING_CULL_DISABLE.as_raw() as u8));
            }
            let data: Vec<vk::AccelerationStructureInstanceKHR> = insts
                .iter()
                .map(|&(addr, flags)| vk::AccelerationStructureInstanceKHR {
                    transform: vk::TransformMatrixKHR { matrix: [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0] },
                    instance_custom_index_and_mask: vk::Packed24_8::new(0, 0xFF),
                    instance_shader_binding_table_record_offset_and_flags: vk::Packed24_8::new(0, flags),
                    acceleration_structure_reference: vk::AccelerationStructureReferenceKHR { device_handle: addr },
                })
                .collect();
            let bytes = std::slice::from_raw_parts(data.as_ptr() as *const u8, std::mem::size_of_val(&data[..]));
            let ib = self.buffer(bytes.len() as u64, vk::BufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR, true);
            self.upload(&ib, bytes);
            let igeo = vk::AccelerationStructureGeometryInstancesDataKHR::default().data(vk::DeviceOrHostAddressConstKHR { device_address: ib.addr });
            let tgeo = [vk::AccelerationStructureGeometryKHR::default()
                .geometry_type(vk::GeometryTypeKHR::INSTANCES)
                .geometry(vk::AccelerationStructureGeometryDataKHR { instances: igeo })];
            let mut tbi = vk::AccelerationStructureBuildGeometryInfoKHR::default()
                .ty(vk::AccelerationStructureTypeKHR::TOP_LEVEL)
                .flags(vk::BuildAccelerationStructureFlagsKHR::PREFER_FAST_TRACE)
                .mode(vk::BuildAccelerationStructureModeKHR::BUILD)
                .geometries(&tgeo);
            let ninst = data.len() as u32;
            let mut tsz = vk::AccelerationStructureBuildSizesInfoKHR::default();
            self.accel.get_acceleration_structure_build_sizes(vk::AccelerationStructureBuildTypeKHR::DEVICE, &tbi, &[ninst], &mut tsz);
            let tlas_buf = self.buffer(tsz.acceleration_structure_size, vk::BufferUsageFlags::ACCELERATION_STRUCTURE_STORAGE_KHR, false);
            let tscratch = self.buffer(tsz.build_scratch_size + 256, vk::BufferUsageFlags::STORAGE_BUFFER, false);
            let tlas = self
                .accel
                .create_acceleration_structure(
                    &vk::AccelerationStructureCreateInfoKHR::default().buffer(tlas_buf.buf).size(tsz.acceleration_structure_size).ty(vk::AccelerationStructureTypeKHR::TOP_LEVEL),
                    None,
                )
                .map_err(|e| e.to_string())?;
            tbi = tbi.dst_acceleration_structure(tlas).scratch_data(vk::DeviceOrHostAddressKHR { device_address: (tscratch.addr + 127) & !127 });
            let trange = [vk::AccelerationStructureBuildRangeInfoKHR::default().primitive_count(ninst)];
            self.one_shot(|cb| {
                self.accel.cmd_build_acceleration_structures(cb, &[tbi], &[&trange]);
            });
            self.free(tscratch);
            self.free(ib);
            self.tlas = tlas;
            self.scene_bufs.push(tlas_buf);
            let mut masks = cards.masks.clone();
            if masks.is_empty() {
                masks = vec![0u8; 16];
                masks[4] = 1;
                masks[8] = 1; // one 1x1 mask, never read
            }
            self.card_bufs = vec![self.host_buf(&cards.tris), self.host_buf(&masks), self.host_buf(&cards.data)];
            Ok(())
        }
    }

    /// Descriptor set with the scene (bindings 0, 4..6) and the given buffers at 1..3.
    fn make_set(&self, b1: &Buf, b2: &Buf, b3: &Buf) -> vk::DescriptorSet {
        unsafe {
            let dsl = [self.dsl];
            let set = self.dev.allocate_descriptor_sets(&vk::DescriptorSetAllocateInfo::default().descriptor_pool(self.dpool).set_layouts(&dsl)).unwrap()[0];
            let tl = [self.tlas];
            let mut asw = vk::WriteDescriptorSetAccelerationStructureKHR::default().acceleration_structures(&tl);
            let bufs = [b1, b2, b3, &self.card_bufs[0], &self.card_bufs[1], &self.card_bufs[2]];
            let infos: Vec<[vk::DescriptorBufferInfo; 1]> = bufs.iter().map(|b| [vk::DescriptorBufferInfo::default().buffer(b.buf).range(vk::WHOLE_SIZE)]).collect();
            let mut w = vec![vk::WriteDescriptorSet::default().dst_set(set).dst_binding(0).descriptor_type(vk::DescriptorType::ACCELERATION_STRUCTURE_KHR).descriptor_count(1).push_next(&mut asw)];
            for (i, inf) in infos.iter().enumerate() {
                w.push(vk::WriteDescriptorSet::default().dst_set(set).dst_binding(i as u32 + 1).descriptor_type(vk::DescriptorType::STORAGE_BUFFER).buffer_info(inf));
            }
            self.dev.update_descriptor_sets(&w, &[]);
            set
        }
    }

    fn read_bytes(&self, b: &Buf, n: usize) -> Vec<u8> {
        unsafe {
            let mut out = vec![0u8; n];
            let ptr = self.dev.map_memory(b.mem, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()).unwrap() as *const u8;
            std::ptr::copy_nonoverlapping(ptr, out.as_mut_ptr(), n);
            self.dev.unmap_memory(b.mem);
            out
        }
    }

    fn dispatch(&self, pipe: vk::Pipeline, set: vk::DescriptorSet, p: &Push, gx: u32, gy: u32) {
        unsafe {
            self.one_shot(|cb| {
                self.dev.cmd_bind_pipeline(cb, vk::PipelineBindPoint::COMPUTE, pipe);
                self.dev.cmd_bind_descriptor_sets(cb, vk::PipelineBindPoint::COMPUTE, self.layout, 0, &[set], &[]);
                self.dev.cmd_push_constants(cb, self.layout, vk::ShaderStageFlags::COMPUTE, 0, std::slice::from_raw_parts(p as *const _ as *const u8, std::mem::size_of::<Push>()));
                self.dev.cmd_dispatch(cb, gx, gy, 1);
            });
        }
    }

    /// Bakes one atlas. `tris` is the triangle table (see bake.comp), `tex_tri` one u32 per texel. Returns size*size bytes.
    pub fn bake(&self, tris: &[u8], tex_tri: &[u32], size: u32, push: Push, rows_per_dispatch: u32) -> Vec<u8> {
        unsafe {
            let tb = self.host_buf(tris);
            let xb = self.host_buf(std::slice::from_raw_parts(tex_tri.as_ptr() as *const u8, tex_tri.len() * 4));
            let words = (size as u64 * size as u64 + 3) / 4;
            let ob = self.host_buf(&vec![0u8; (words * 4) as usize]);
            let set = self.make_set(&tb, &xb, &ob);
            let rows = rows_per_dispatch.max(8);
            let mut y = 0;
            while y < size {
                let n = rows.min(size - y);
                let mut p = push;
                p.row_start = y;
                self.dispatch(self.pipe, set, &p, (size + 7) / 8, (n + 7) / 8);
                y += n;
            }
            let out = self.read_bytes(&ob, (size as usize) * (size as usize));
            self.dev.free_descriptor_sets(self.dpool, &[set]).ok();
            self.free(tb);
            self.free(xb);
            self.free(ob);
            out
        }
    }

    /// Uploads calibration points (48 bytes each, see points.comp) for the current scene.
    pub fn points(&self, bytes: &[u8]) -> PointSet {
        PointSet { pts: self.host_buf(bytes), n: (bytes.len() / 48) as u32 }
    }

    /// Sun occlusion (0..255) of every point for each of `dirs` (unit vectors towards the sun): one row per direction.
    pub fn trace_points(&self, ps: &PointSet, dirs: &[[f32; 3]], push: Push) -> Vec<Vec<u8>> {
        let mut p = push;
        p.size = ps.n;
        let stride = ((ps.n as usize) + 3) & !3;
        let per = (256usize << 20) / stride.max(1);
        let mut rows = Vec::with_capacity(dirs.len());
        for chunk in dirs.chunks(per.clamp(1, 4096)) {
            let mut db = Vec::with_capacity(chunk.len() * 16);
            for d in chunk {
                for v in [d[0], d[1], d[2], 0.0] {
                    db.extend_from_slice(&v.to_le_bytes());
                }
            }
            let dirb = self.host_buf(&db);
            let out = self.host_buf(&vec![0u8; stride * chunk.len()]);
            let set = self.make_set(&ps.pts, &dirb, &out);
            // callers keep point sets under 4M points (the group count limit is 65535)
            self.dispatch(self.pipe_points, set, &p, (ps.n + 63) / 64, chunk.len() as u32);
            let all = self.read_bytes(&out, stride * chunk.len());
            for k in 0..chunk.len() {
                rows.push(all[k * stride..k * stride + ps.n as usize].to_vec());
            }
            unsafe {
                self.dev.free_descriptor_sets(self.dpool, &[set]).ok();
            }
            self.free(dirb);
            self.free(out);
        }
        rows
    }

    pub fn free_points(&self, ps: PointSet) {
        self.free(ps.pts);
    }
}
