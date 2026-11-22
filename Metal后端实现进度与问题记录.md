# Metal 后端（macOS / iOS）实现进度与问题记录

> 分支：`feature/metal-backend`
> 范围：`modules/graphics/backend/` 新增 Metal RHI 后端，覆盖 macOS 与 iOS；不改动上层 `gpu::` 门面与 `effect` 渲染图的公开 API。
> 参考实现：`modules/graphics/backend/src/vulkan/`（结构对照）与 `src/gl/`（平台层与资源生命周期约定）。

---

## 1. 结论与完成度

Metal 已经成为一个**可创建、可绘制、可读回、可作为 macOS / iOS 默认后端**的完整 RHI 后端，而不是占位实现：

| 项目 | 状态 |
| --- | --- |
| 设备创建 / 能力查询 / 诊断 | ✅ 完成 |
| 纹理、纹理视图、采样器、缓冲区（含初始数据上传） | ✅ 完成 |
| Shader module（SPIR-V 输入 / GLSL→SPIR-V→MSL） | ✅ 完成 |
| 描述符集布局、描述符集、管线布局 | ✅ 完成 |
| 图形管线、计算管线、深度/模板状态、混合、MSAA | ✅ 完成 |
| 命令缓冲（渲染通道、绘制、间接绘制、计算、拷贝、清除、查询） | ✅ 完成 |
| 交换链与呈现（`CAMetalLayer`、`defaultFramebufferView()`、`endFrame`） | ✅ 完成（窗口环境未在自动化测试中覆盖，见 §9） |
| Fence / Semaphore（`MTLSharedEvent`） | ✅ 完成 |
| 时间戳查询 / 遮挡查询 | ✅ 时间戳；遮挡已编码但未被测试覆盖 |
| 原生纹理导入 | ⛔ 明确返回 `Unsupported`（RHI 缺少 IOSurface 类型，见 §9） |
| iOS | ✅ 已在 iPhone 18 Pro（iOS 27.2）模拟器上构建并跑通四个测试套件；⛔ 真机（签名/设备）尚未验证 |

验证结果（macOS 27.0 / Apple M6 / Xcode 27.2，Release；Vulkan 已在 Apple 平台关闭）：

```text
test_backend   : 103 tests, 100 passed, 3 skipped
test_graphics  : 1115 tests, 1095 passed, 20 skipped
test_geometry  : 183 passed
test_effect    : 111 tests, 110 passed, 1 skipped
ctest (graphics|backend|geometry|effect|fuzz smoke + effect mac rows): 9/9 Passed
effect_mac_demo / _metal / _opengl (examples/effect): 4/4 Passed
```

iOS 模拟器（iPhone 18 Pro, iOS 27.2，`iphonesimulator27.2` SDK，arm64；Vulkan 关闭）结果：

```text
test_backend.app  : 103 tests, 100 passed, 3 skipped   （Metal device: "Apple iOS simulator GPU"）
test_graphics.app : 1115 tests, 1087 passed, 28 skipped, 0 failed
test_geometry.app : 183 passed
test_effect.app   : 111 tests, 110 passed, 1 skipped
```

iOS 的 28 个 skip 中，`WindowSurface.*`（无窗口）、`Facade.DirectGlsl*` / `ContextGroup.*`（GL 专属）、
字形用例（模拟器环境里测试的字体枚举为空）、`GpuSurface.TheWholeSvgCorpusInTheDemoGridMatchesTheRasterizer`
（README 已记录模拟器软件渲染不保证逐像素一致）、`Facade.CompilesGlslAndKeepsBothForms`
（编译期平台是 ES，用例按桌面方言跳过）和 `GpuSurface.MultipleFramesInFlightKeepEveryFrameIsolated`
（用例自身要求 Vulkan 后端）都是环境/平台属性；没有失败。

`test_graphics` 与 `test_effect` 的 GPU 用例默认就走 **Metal**（`getDefaultBackend()` 在 Apple 平台首选 Metal），也就是说画布、字图、图像滤镜、效果图等上层用例已经在 Metal 上跑通。macOS 上 20 个 skip 的原因均为环境属性而非 Metal 回归：

- `WindowSurface.*`：本机为无窗口的 CLI 环境，没有可用的窗口/图层；
- `Facade.DirectGlsl*`、`ContextGroup.*`：GL 专属能力（Metal 不提供 `GLSLDirect`，`capabilities.acceptsGlslDirect == false` 时用例自行跳过）；
- 少量字形用例：`no system font is available in this environment`。

`test_backend` 的 3 个 skip 是：两个 Vulkan 专属用例（本构建没有 Vulkan 后端）与一个
`DirectGlslDrawsWithTheCallersLocationTable`（Metal 迭代先因 `acceptsGlslDirect == false` 跳过整个用例，
属于测试框架逐后端迭代的既有局限，不是 Metal 回归）。

> **关于本机工具链**：Xcode 27.2 已安装在 `/Applications/Xcode.app`，iOS SDK 与 Metal 工具链都在，但
> `xcode-select -p` 仍指向 `/Library/Developer/CommandLineTools`，因此 `xcrun --sdk iphonesimulator` 和
> `xcrun metal` 会报 “cannot be located / unable to find utility”。两种解决方式任选：
>
> ```sh
> sudo xcode-select -s /Applications/Xcode.app     # 全局切换（需要管理员权限）
> # 或者只对当前命令生效：
> DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun --sdk iphonesimulator --show-sdk-path
> ```
>
> 本记录的 iOS 构建/测试都使用 `DEVELOPER_DIR=...` 前缀，没有改动机器的全局设置。另外，本后端在运行时用
> `newLibraryWithSource:` 编译 MSL，并不调用 `xcrun metal` 离线编译，所以 Metal Toolchain 不是运行前提；
> 真正需要的是 Xcode 的 SDK 头文件（`Metal.framework`）。

---

## 2. 架构对照：RHI → Metal

| RHI（`backend::`） | Metal | 说明 |
| --- | --- | --- |
| `Device` | `MetalDevice` + `id<MTLDevice>` / `id<MTLCommandQueue>` | 单队列；`getQueue(Compute/Transfer)` 返回 `nullptr`，与 Vulkan 一致 |
| `CommandBuffer` | `MetalCommandBuffer` + `id<MTLCommandBuffer>` + 每种 encoder | 立即编码；绑定状态延迟到 draw 应用 |
| `Texture` / `TextureView` | `id<MTLTexture>`，视图用 `newTextureViewWithPixelFormat:textureType:levels:slices:` | 全范围同格式视图直接复用底层纹理 |
| `Buffer` | `id<MTLBuffer>` | `GpuOnly` → `Private`（初始数据用一次性 blit 上传）；其余 → `Shared` |
| `Sampler` | `id<MTLSamplerState>` | 比较采样通过 `compareFunction` 表达 |
| `ShaderModule` | SPIR-V + 反射（**不持有** `MTLLibrary`） | MSL 在管线创建时生成，原因见 §6 |
| `DescriptorSetLayout` / `DescriptorSet` | 纯 CPU 状态 | Metal 没有描述符集对象 |
| `PipelineLayout` | 槽位分配表 `MetalPipelineLayout` | 把 `(set, binding)` 映射到 `[[buffer]]/[[texture]]/[[sampler]]` |
| `RenderPipeline` | `id<MTLRenderPipelineState>` + `id<MTLDepthStencilState>` | 每次创建管线同时构造深度/模板状态 |
| `ComputePipeline` | `id<MTLComputePipelineState>` | 线程组大小来自描述符 |
| `RenderPass` | 只保存格式/采样数的兼容性键 | 真正的渲染通道在 `beginRenderPass` 时构造 `MTLRenderPassDescriptor` |
| `Framebuffer` | 仅校验记录 | Metal 无原生 framebuffer 对象 |
| `Fence` | `MTLSharedEvent` + 单调值 | `waitUntilSignaledValue:timeoutMS:` 实现超时等待 |
| `Semaphore` | `MTLSharedEvent` + 值 | 提交时 `encodeSignalEvent:` / `encodeWaitForEvent:` |
| QuerySet（时间戳） | `MTLCounterSampleBuffer` | 通过 blit encoder 采样，纳秒 |
| QuerySet（遮挡） | visibility result buffer（`MTLBuffer` + 64 位计数） | `setVisibilityResultMode:` |
| Surface / Swapchain | `CAMetalLayer` + `CAMetalDrawable` | 格式固定授予 `BGRA8Unorm`（见 §7.9） |
| Pipeline cache | 未实现 | 可接入 `MTLBinaryArchive`（§9） |

三个关键结构决策：

1. **尽早编码、延迟应用状态。** 与 Vulkan 后端一样，`CommandBuffer` 的每条调用立即落到 encoder；但管线/描述符/顶点缓冲/视口等“绑定状态”先记在命令缓冲里，在 `draw` 前一次性应用。这样与 RHI 的调用顺序解耦，也避免“后绑定的描述符影响前一次绘制”。
2. **MSL 在管线创建时生成。** `(set, binding) → 槽位` 的分配是 **pipeline layout** 的属性；同一个 shader module 可以合法地链接到不同 layout。如果提前在 `createShaderModule` 翻译，槽位会被烧死在 MSL 文本里。代价是同一 module 在多个管线中重复编译 MSL（后续可用 `(module, layout)` 缓存）。
3. **`defaultFramebufferView()` 解析为当前 drawable。** 上层（`gpu::WindowSurface` / `GpuRenderer::createWindowTarget`）用哨兵视图表示“交换链图像”，后端在 `beginRenderPass` 时把它替换为 `CAMetalDrawable.texture`。

---

## 3. 代码结构

新增文件（全部位于 `modules/graphics/backend/`）：

```text
include/private/backend/metal/
  MetalDevice.h          资源 payload、MetalDevice、槽位约定
  MetalCommandBuffer.h   命令缓冲与 encoder 状态
  MetalEnums.h           RHI ↔ Metal 枚举/格式映射与槽位常量
  MetalFactory.h         纯 C++ 工厂声明（Device.cpp 可包含）

src/metal/
  MetalDevice.mm         生命周期、能力探测、同步、诊断、队列、工厂实现
  MetalResources.mm      纹理/缓冲/采样器/Shader/描述符/管线/渲染通道/查询/互操作
  MetalCommandBuffer.mm  录制、状态应用、绘制、拷贝、清除、查询、提交
  MetalPresentation.mm   Surface、CAMetalLayer 交换链、beginFrame/endFrame/resize
  MetalEnums.mm          格式、顶点格式、混合、深度/模板、采样器、拓扑映射
  MetalCross.mm/.h       SPIR-V → MSL（SPIRV-Cross CompilerMSL 封装）

tests/test_MetalCross.cpp  MSL 翻译的无设备单测（6 个用例）
```

集成点：

- `src/Device.cpp`：`Backend::Metal` 分支调用 `metal::createMetalDevice(configuration, outDevice)`（通过纯 C++ 的 `MetalFactory.h`，避免 `.cpp` 引入 Metal 头）。
- `backend/CMakeLists.txt`：新增 `CORAL_BACKEND_SUPPORTS_METAL`、`CORAL_BACKEND_HAS_METAL`、frameworks（Metal/QuartzCore/Foundation）、**逐文件** `-fobjc-arc`。
- `third_party/SPIRV-Cross/coral/CMakeLists.txt`：加入 `spirv_msl.cpp/.hpp`。
- `modules/graphics/src/gpu/GpuRenderer.cpp`：`usesVulkanShaderSet()` 现在同时接受 Vulkan 与 Metal —— 画布 shader 需要 SPIR-V 的 `DescriptorSet/Binding` 装饰，Metal 与 Vulkan 消费的是同一套嵌入 shader。
- `backend/tests/test_PlatformRunner.cpp`：期望后端列表把 Metal 放在首位，`implemented = true`（原因见 §7.12）。

---

## 4. RAII 的落实

要求是“C++ 实现严格遵循 RAII”。Metal 后端用三层机制做到，且**没有任何一处手工 `release`/`retain`**：

1. **资源归属交给 `HandlePool`。** 每种资源一个 `HandlePool<Tag, Payload>`（与 Vulkan 完全一致）。`destroyX` 只做 `payload = Payload{}`（释放 Metal 对象）再 `pool.free(handle)`；设备析构时池的析构函数负责全部剩余资源。
2. **payload 用 ARC 管理 Metal 对象。** `MetalTexture { id<MTLTexture> texture; ... }` 这类结构体的 `__strong` 成员由 ARC 生成 retain/release；`Payload{}` 赋值即释放。因此 `HandlePool::free` 本身就是一次完整、异常安全的释放。ARC 只加在 `src/metal/*.mm` 上（`set_source_files_properties(... COMPILE_OPTIONS "-fobjc-arc")`），因为 OpenGL 的 `GlPlatformEagl.mm` 使用手写 retain/release，不能整库开启 ARC。
3. **设备创建用 `std::unique_ptr` 守卫。** `createMetalDevice` 构造 `unique_ptr<MetalDevice>`，`initialize()` 成功后才 `release()` 交给调用方；失败路径不泄漏半成品设备。这与 `Device.cpp` 中 OpenGL/Vulkan 分支的写法一致。

其他生命周期细节：

- **命令缓冲暂存区（staging）** 的 `id<MTLBuffer>` 存放在 `std::vector<id<MTLBuffer>>` 成员里，直到 `reset()/begin()/析构` 才释放 —— 因为 GPU 可能在录制结束后才真正执行 blit。`abandon()` 在设备销毁未提交命令缓冲时丢弃录制。
- **Surface 强引用 `CAMetalLayer`**：`MetalSurface::layer` 是 `__strong`，由池槽位释放；应用仍拥有它自己的引用。
- **没有裸 `new`/`delete` 资源**：唯一的裸指针是 `CommandBuffer*`，由 `createCommandBuffer` 的 `unique_ptr::release()` 移交、`destroyCommandBuffer` 的 `delete` 收回，这是 RHI 已约定的所有权边界。
- **错误路径不留下半初始化对象**：所有 `createX` 先构造局部 payload，成功后才 `pool.allocate`；`createTexture` 失败前不写入 `*outHandle`（始终写 invalid）。

---

## 5. 描述符槽位约定

Metal 没有描述符集，`(set, binding)` 必须落到三张扁平参数表。约定集中在 `MetalEnums.h`：

| 槽位范围 | 用途 |
| --- | --- |
| `buffer(0..7)` | 顶点流缓冲，索引即 `VertexAttribute::bufferIndex`（`kMaxVertexBufferSlots = 8`） |
| `buffer(8..29)` | 描述符集中的 uniform / storage / read-only storage buffer |
| `buffer(30)` | push constant 块（`kPushConstantBufferSlot`） |
| `texture(0..30)` | 采样纹理 / storage 纹理 |
| `sampler(0..15)` | 采样器 |

分配规则：

- `MetalDescriptorSetLayout` 在创建时按声明顺序为每个 binding 记录“类内起始索引”（纹理/采样器/缓冲分开计数，数组元素占连续槽位）。
- `MetalPipelineLayout` 在创建时按 set 顺序累加，得到每个 set 的 `textureBase / samplerBase / bufferBase`；缓冲区基址从 `kFirstResourceBufferSlot = 8` 开始。
- 管线创建时，用同一张 `MetalPipelineLayout` 生成 `spv::MSLResourceBinding` 交给 SPIRV-Cross；命令缓冲 `bindDescriptorSet` 时用同一张表计算槽位。**两处读同一份数据**，因此不会漂移。
- 分辨率上限在 `createPipelineLayout` 检查：纹理 > 31、采样器 > 16、缓冲 > 22 时返回 `Unsupported` 并给出可读消息。
- 未写入的 binding 绑定设备级 fallback（1×1 透明纹理、linear/clamp 采样器、1 KiB 零缓冲），与 Vulkan 后端的策略一致 —— Metal 对未绑定纹理/采样器会在校验时报错。
- 组合采样器（`TextureSampler`）在 MSL 里是**两个参数**（`[[texture(n)]]` + `[[sampler(m)]]`），命令缓冲会同时绑定两半。

---

## 6. Shader 路径

```text
GLSL 源 ──shaderc(vulkan env)──▶ SPIR-V ──SPIRV-Cross CompilerMSL──▶ MSL ──MTLDevice 编译──▶ MTLLibrary ──▶ MTLFunction
SPIR-V ──────────────────────────────────┘
```

- `Language::GLSL` 使用 `compileGlslToSpirvVulkan`，而不是 OpenGL 环境的 `compileGlslToSpirv`：后者不生成 `DescriptorSet/Binding` 装饰，Metal 会把所有资源都放在 `(0,0)`。
- `Language::GLSLDirect` 明确返回 `Unsupported`（Metal 的原生语言是 MSL，不是 GLSL），`Capabilities::acceptsGlslDirect = false` 让上层提前知道。
- `Capabilities::acceptsSpirv = true`：Metal 后端在运行时消费 SPIR-V 并自行翻译，因此 SPIR-V 反射/回读相关用例照常运行。
- MSL 选项（`MetalCross.mm`）：
  - `CompilerMSL::Options`：`platform`（iOS/macOS）、`msl_version 2.2`、`argument_buffers = false`、`enable_decoration_binding = false`、`pad_fragment_output_components = true`。
  - **同时**设置 `CompilerGLSL::Options`：`vertex.flip_vert_y = true`、`vertex.fixup_clipspace = false`。`CompilerMSL::Options` 不继承 GLSL 的这组选项，但 MSL 代码生成会读取它们。RHI 的 SPIR-V 是 Vulkan 的 Y 向下、深度 [0,1] 约定：Metal 的 NDC 是 Y 向上、深度 [0,1]，因此只需翻转 Y，不能同时做 clipspace 修正（否则 z 会被平移到 0.5）。
  - 入口点：SPIRV-Cross 会把 `main` 改名为 `main0`，`MslCompileResult::entryPoint` 返回 `get_cleansed_entry_point_name()` 的结果，后端用它调用 `newFunctionWithName:`。
  - 执行模型校验：模块自身的 `ExecutionModel` 与 `ShaderStage` 不一致时报错（与 `ShaderCross` 一致）。
  - `MslCompileResult::attributeLocations` 暴露顶点阶段实际读取的 `Location`，用于 §7.2 的补全。

---

## 7. 遇到的问题与处理（按发现顺序）

### 7.1 uniform buffer 落在了 `[[buffer(0)]]` —— 静默画不出东西

**现象**：`GpuSurface.AnOffscreenFrameDrawsAndReadsBack` 能创建管线、录到 2 个 indexed draw、读回也成功，但像素全 0。
**根因**：`MetalPipelineLayout` 的 `bufferBase` 从 0 开始算，而 0..7 是顶点流参数表。于是 `quad.vert` 的 uniform block 与顶点流抢同一个槽位，顶点着色器读到的 `uScale` 全是 0，几何退化成一点。
**修复**：`bufferBase = kFirstResourceBufferSlot + 累计值`；同时在文档与常量注释里写明“顶点流与 shader buffer 共用一张参数表”。
**教训**：这不是 Metal 校验错误（槽位 0 合法），只能靠“同一份映射表用于翻译与绑定”来避免再次分叉。

### 7.2 `quad.vert` 的共享变体：Metal 拒绝缺少 attribute 的管线

**现象**：画布管线创建失败：`Vertex attribute aSdf(4) is missing from the vertex descriptor`。
**根因**：`quad.vert` 被 SDF 与非 SDF 两种变体共用。SDF 变体声明 9 个顶点属性（locations 0..8），非 SDF 变体只声明 4 个；OpenGL 允许缺失属性（读到未定义值），Metal 在管线创建时直接拒绝。
**修复**：`MslCompileResult::attributeLocations` 给出顶点函数实际读取的 location；`createRenderPipeline` 对比管线声明，缺失的属性补到一条“哑流槽位”（`MetalRenderPipeline::dummyVertexSlot`），绘制时把设备的零填充缓冲绑到该槽位。这样缺失属性的读取是**定义良好的 0**，而不是越界或管线创建失败。
**注意**：只有确实存在缺失属性时才创建该 layout —— Metal 对“没有任何 attribute 引用”的 layout 同样会断言（见 7.3）。

### 7.3 MSAA 的 resolve 把上一 pass 的结果覆盖掉

**现象**：4x MSAA 的 `GpuSurface` 用例中，绘制 pass 结束后 snapshot 全 0；RHI 层面的单 pass MSAA 用例却通过。
**根因**：`MTLStoreActionMultisampleResolve` 在解析的同时**丢弃**多重采样纹理内容。画布的一帧由多个 pass 组成（clear pass → draw pass → readback 的空 pass），后一个 pass 以 `LoadOp::Load` 读取已被丢弃的 MSAA 内容并再次 resolve，把正确的红色覆盖成未定义值。
**修复**：当 RHI 的 `StoreOp` 为 `Store` 时使用 `MTLStoreActionStoreAndMultisampleResolve`，保留 MSAA 内容；`DontCare` 时才用纯 resolve。这与 Vulkan resolve attachment 的语义一致。
**教训**：Metal 的 store action 同时表达“是否保留”和“是否解析”，不能只按“有 resolveTarget”就选 `MultisampleResolve`。

### 7.4 `setLabel:` 传 nil 会断言

**现象**：`[MTLSamplerDescriptorInternal setLabel:]: failed assertion 'label must not be nil.'`，随后又出现在 `MTLRenderPipelineDescriptorInternal` 上。
**根因**：Objective-C 的 `setLabel:` 断言非 nil，而 `label = cond ? value : nil` 仍然会调用 setter。
**修复**：所有 label 赋值都改成 `if (label != nullptr) obj.label = ...;`（采样器、渲染管线描述符等）。
**顺带发现**：`MTLTextureDescriptor` 没有 `label` 属性；`MTLRenderPipelineState.label` 是只读；`MTLCounterSampleBufferDescriptor.label` 只读 —— 这些都在编译期或运行期被逐个纠正。

### 7.5 时间戳查询的能力检查看了错误的采样点

**现象**：`MTLBlitCommandEncoder:sampleCountersInBuffer:atSampleIndex:withBarrier not supported on this device`。
**根因**：能力探测用 `MTLCounterSamplingPointAtStageBoundary`，而实现是在 blit encoder 上采样；M6 支持前者但不支持 blit 采样。
**修复**：能力位改用 `supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary`；`createQuerySet(Timestamp)` 不满足时返回 `Unsupported`，`endQuery` 也不会再发非法调用。
**教训**：这是本仓库反复出现的“检查对象与实现对象不一致”问题的一个新实例（检查通过、实现断言）。

### 7.6 两个 Options 结构与 clip space / 绕序

见 §6。额外的连带修复：MSL 中 Y 被翻转后，`FrontFace::CounterClockwise` 在屏幕上表现为顺时针，因此 `setFrontFacingWinding:` 需要取反（`toMtlWinding(frontFace, flipY=true)`），否则 `CullMode::Back` 会剔掉相反的面。

### 7.7 三角形扇（TriangleFan）没有 Metal 图元

`PrimitiveTopology::TriangleFan` 在 Metal 中不存在。后端按 Vulkan 的做法生成一张 `(0, i-1, i)` 的 UINT16 索引缓冲（最多 65535 个顶点），绘制时改用 `drawIndexedPrimitives:` 并以 `baseVertex = firstVertex` 表达起始顶点。索引缓冲由设备持有（`ensureFanIndexBuffer`），在设备初始化时创建 —— 而不是首次绘制时，因为命令缓冲可以在任意线程录制，
惰性创建会把一次设备状态写入放到录制路径上。

### 7.8 呈现路径：CAMetalLayer 与“提交后再 present”

- `SurfaceDescriptor::nativeWindow` 是 `CAMetalLayer*`；`createSurface` 会校验类型并把它接到同一个 `MTLDevice`。
- `createSwapchain` 设置 `pixelFormat/framebufferOnly=NO/drawableSize/maximumDrawableCount/displaySyncEnabled`；drawable 格式固定授予 `BGRA8Unorm`（`SwapchainInfo::format` 会如实报告，上层据此创建窗口目标管线）。
- `beginFrame` 调 `nextDrawable`，把 acquire semaphore 在 CPU 侧置位（因为 acquire 本身是同步完成的）；同时记录 `mCurrentFrameSwapchain`，让 `defaultFramebufferView()` 解析到当前 drawable。
- `endFrame` 单独开一个 `MTLCommandBuffer`：`encodeWaitForEvent:`（如果有 wait semaphore）→ `presentDrawable:` → 信号设备完成事件 → commit。同一个 `MTLCommandQueue` 按提交顺序执行，因此渲染命令先于 present；这与 Vulkan 用 semaphore 把渲染与 present 分开是同一形状。
- `resizeSwapchain` 只需更新 `layer.drawableSize`，图像由 layer 自己的池回收。

### 7.9 格式映射的取舍

- **没有 RGB8/BGR8 三通道格式**：RHI 的 `R8G8B8*` / `B8G8R8*` 在 Metal 中映射为 `Invalid`，能力表如实报告不支持，而不是用 RGBA8 顶替（字节布局与采样值都会不同）。
- **D24_UNORM_S8_UINT → Depth32Float_Stencil8**：Metal 没有 24 位深度。选择 32F+Stencil 是 Dawn 等实现的标准做法，既保留模板又保持渲染通道格式一致。
- **ETC2_RGBA8 不存在**：Metal 只有 ETC2 RGB8；RGBA8 ETC2 返回 `Invalid`。
- **BC 仅 macOS，ETC2/ASTC 仅 Apple GPU**：能力表按平台/`supportsFamily:Apple2` 过滤。
- **交换链格式**：`CAMetalLayer` 的 drawable 固定 `BGRA8Unorm`，因此 `SwapchainInfo::format` 是 `B8G8R8A8_UNORM`；上层已经按“授予的格式”建管线。

### 7.10 命令缓冲的 encoder 互斥与“挂起渲染通道”

Metal 同一时刻只能有一个 encoder。命令缓冲用一个 `suspendRenderPass()/resumeRenderPass()` 处理“渲染通道中间来了一个拷贝/清除/时间戳”的情况：结束 render encoder，做 blit，再以 `MTLLoadActionLoad` 重建 render encoder（保留 store/resolve）。`endRenderPass` 仍然只应用一次调用方给的 store/resolve 语义。

### 7.11 清除的粒度

Metal 的 clear 是渲染通道的 load action，只能清整个 attachment。`clearColorTexture`/`clearDepthStencilTexture` 对**子资源范围**（非整张纹理/非全部 mip）返回 `Unsupported` 并说明原因，而不是悄悄清整张。

### 7.12 测试框架的顺序问题（不是产品缺陷，但影响验证）

`test_PlatformRunner.cpp` 的 `forEachExpectedBackend` 在某个后端不可用时调用 `GTEST_SKIP()`，该宏结束**整个用例**。原先列表顺序是 OpenGL → Vulkan → Metal，而 macOS 上 Vulkan 编译进来却没有 loader，于是 Metal 永远轮不到执行。修复：把 Metal 放到列表首位（它也是 Apple 平台的首选后端），并把 `implemented` 设为 `true`，让 Metal 创建失败成为真正的失败而不是跳过。

这个局限现在还剩一个可见后果：Metal 因 `acceptsGlslDirect == false` 跳过 `DirectGlslDrawsWithTheCallersLocationTable` 时，OpenGL 那一轮不会被跑到。这是 gtest 没有“只跳过当前迭代”的表达能力造成的，记录下来而不是为了让数字好看去改测试语义。

### 7.13 环境带来的 skip 不是回归

从错误的目录运行时，字形/资源类用例会因为找不到资源而 skip（例如 `no system font is available in this environment`）。用 CTest（工作目录已设置）或从 `modules/graphics` 目录运行即可；CTest 结果是 6/6 通过。

### 7.14 iOS 首次交叉编译：`displaySyncEnabled` 在 iOS 上不存在

**现象**：iOS 构建在 `MetalPresentation.mm` 失败：
`error: 'displaySyncEnabled' is unavailable: not available on iOS`。
**根因**：`CAMetalLayer.displaySyncEnabled` 是 macOS 专属属性；iOS 的图层始终随显示器刷新呈现，没有逐图层的同步开关。
**修复**：
- 该属性放进 `#if !TARGET_OS_IPHONE`；
- `createSwapchain` 在 iOS 上把 `Immediate`/`Mailbox` 请求**如实降级为 `Fifo`**（`SwapchainInfo::presentMode` 报告实际授予的模式）；
- `Capabilities::mailboxPresent` 在 iOS 上为 `false`（macOS 仍为 `true`）。

这条修复让 iOS 构建一次通过，也说明“能力表说支持”与“平台真的能授予”必须由同一个平台条件决定。

### 7.15 macOS / iOS 不再构建 Vulkan 后端

**背景**：Apple 平台的 Vulkan 需要 MoltenVK 之类的额外 loader，而本树在 macOS 上编译 Vulkan 后端的结果只是每次创建设备时打印
`no Vulkan loader could be opened`，然后回退到 Metal/OpenGL。`effect_mac_demo` 也会把 Vulkan 列为 “unavailable”，
在验证输出里制造一条看起来像错误的日志。

**处理**：
- `CORAL_BACKEND_SUPPORTS_VULKAN` 在 `APPLE` 上被 **FORCE 成 OFF**（root `CMakeLists.txt`、`cmake/CoralGraphicsDependencies.cmake`、
  `modules/graphics/backend/CMakeLists.txt` 三处都处理），因此 CLion 等已有构建目录里的缓存 `=ON` 会在下次重新配置时被纠正；
- 默认后端优先级在 macOS 上改为 Metal → OpenGL（iOS 本来就是 Metal → OpenGL ES）；
- `effect_mac_demo` 只验证 Metal 与 OpenGL，`--backend vulkan` 会明确提示“Apple 平台不提供 Vulkan”；
- Windows / Linux / Android 的 Vulkan 默认值不受影响。

**一个不能顺手改错的依赖**：Metal 消费的是 **Vulkan 环境编译出来的画布 shader 集**（它带 `DescriptorSet/Binding` 装饰，
Metal 的槽位表就是从这些装饰映射出来的）。原先 `modules/graphics/CMakeLists.txt` 用 `CORAL_BACKEND_SUPPORTS_VULKAN`
控制是否生成/嵌入这套 shader；在“Metal ON、Vulkan OFF”的 Apple 构建里，这会把 Metal 需要的 shader 集一起关掉。
现在改为独立的 `CORAL_GRAPHICS_NEEDS_VULKAN_SHADER_SET = VULKAN OR METAL`，并且只在真正生成该集合时才把目录传给
嵌入脚本（脚本对“传了目录却不存在”是报错而不是静默跳过）。

---

## 8. 验证记录

### 8.1 macOS

```sh
/opt/homebrew/bin/cmake -S . -B build-mac -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
/opt/homebrew/bin/cmake --build build-mac -j8

ctest --test-dir build-mac \
      -R "test_(graphics|backend|geometry)|effect_(demo|public_header_boundary)|fuzz_parsers_smoke" \
      --output-on-failure
# 100% tests passed out of 6（effect 的 3 条 mac 行在下面的命令里）

# examples/effect 的 macOS 验证（新增）
ctest --test-dir build-mac -R 'effect_(demo|mac_demo)' --output-on-failure
# effect_demo, effect_mac_demo, effect_mac_demo_metal, effect_mac_demo_opengl: 4/4 Passed

# 单独观察 Metal
./build-mac/modules/graphics/backend/test_backend --gtest_filter='PlatformRunner.*:MetalCross.*'
CORAL_RHI_BACKEND=metal ./build-mac/modules/graphics/backend/test_backend
```

### 8.2 iOS 模拟器

`xcode-select` 仍指向 CLT 时，所有命令前加 `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`
（见 §1 的工具链说明），或先执行 `sudo xcode-select -s /Applications/Xcode.app`。

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
/opt/homebrew/bin/cmake -S . -B build-ios-metal -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
  -DCORAL_SHADERS_COMPILER="$PWD/build-mac/modules/graphics/tools/shader_compiler/shader_compiler"

DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
/opt/homebrew/bin/cmake --build build-ios-metal --target test_backend test_graphics test_effect test_geometry -j8

# 模拟器：先 boot 一台（本机为 iPhone 18 Pro, iOS 27.2）
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun simctl boot "iPhone 18 Pro" || true

cd modules/graphics/backend   # 从模块目录运行，等价于 CTest 的工作目录
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun simctl spawn booted \
    "$PWD/../../../build-ios-metal/modules/graphics/backend/test_backend.app/test_backend"

# graphics / effect / geometry 同理，路径分别为
#   build-ios-metal/modules/graphics/test_graphics.app/test_graphics
#   build-ios-metal/modules/effect/test_effect.app/test_effect
#   build-ios-metal/modules/graphics/geometry/test_geometry.app/test_geometry
```

模拟器里 `test_backend` 报告的设备是 `Apple iOS simulator GPU`（`Metal 2.2+`，`coral/backend/metal`），
`PlatformRunner.MultisampleResolveProducesTheDrawnColour`、`IndexedDraws...`、`PushConstants...` 在
模拟器上同样通过，说明 MSL 翻译、参数表和 resolve 路径在 iOS 上是同一套代码在工作。

### 8.3 examples/effect 的 macOS 验证

`examples/effect/effect_mac_demo.cpp` 是新增的 macOS 验证程序（详见 §11.1）：

```sh
./build-mac/bin/effect_mac_demo                 # Metal + OpenGL 全部验证
./build-mac/bin/effect_mac_demo --backend metal # 固定后端；回退到别的后端算失败
./build-mac/bin/effect_mac_demo --backend opengl
./build-mac/bin/effect_mac_demo --backend vulkan # Apple 平台不提供 Vulkan：明确报错，exit 1
```

实际输出：

```text
Metal: coral/backend/metal / Apple M6 / Metal 2.2+
Metal: colour bars ok (8 of 8)
OpenGL: coral/backend/gl-desktop/CGL / Apple M6 / OpenGL 4.1 core
OpenGL: colour bars ok (8 of 8)
OpenGL: matches Metal

verified: 2, unavailable: 0, failed: 0
```

关键证据（Metal 专属）：

- `PlatformRunner.IndexedDrawsKeepTheirIndicesWhenTheVertexArrayChanges`：Metal 首轮输出
  `left ff0000ff, middle ff000000, right ffff0000`，与期望逐字节一致。
- `PlatformRunner.PushConstantsReachTheShader`：Metal 首轮输出 `centre ffc08040 -> rgba(64, 128, 192)`。
- `PlatformRunner.MultisampleResolveProducesTheDrawnColour`：该用例原先只对 Vulkan 开放，现在也覆盖 Metal（`centre ff0000ff`），正是它暴露了 7.3 的 resolve 覆盖问题。
- `MetalCross.*`：6 个无设备单测，覆盖空输入、魔数错误、截断模块、真实 SPIR-V 的 MSL 翻译（`[[color(0)]]`、`[[texture(n)]]`、`[[sampler(n)]]`、`[[buffer(30)]]`、入口点 `main0`）与阶段不匹配。

---

## 9. 已知限制与后续工作

按建议优先级排列：

1. **iOS 真机未验证（模拟器已通过）。** iPhone 18 Pro / iOS 27.2 模拟器上四个测试套件全部通过；真机还差签名与设备运行
   （`-DCMAKE_OSX_SYSROOT=iphoneos` + 应用的签名设置），以及真实 GPU 上 `MTLTextureType1D`、wireframe fill
   （已在 `mIos` 上关闭 `fillModeNonSolid`）、`MTLCounterSampleBuffer` 采样点的差异。模拟器的 Metal 是软件/半虚拟化实现，
   性能数字不代表真机。
2. **原生纹理导入缺失。** RHI 的 `NativeTextureKind` 没有 Metal/IOSurface 类型；视频解码器的零拷贝需要新增一种 kind（`IOSurface` 或 `CVMetalTextureCache`），这是 RHI 变更而不是后端私事。当前 `importNativeTexture` 返回 `Unsupported` 且 `supportsNativeTextureImport` 全为 `false`。
3. **HDR / Display-P3 呈现未实现。** `createSwapchain` 对非 `Srgb` 请求降级为 SDR 并告警；`setHdrMetadata` 走默认 `Unsupported`。实现路径是 `CAMetalLayer.wantsExtendedDynamicRangeContent` + `colorspace` + 上层 PQ 输出。
4. **`MTLBinaryArchive` 管线缓存未接入。** `DeviceConfiguration::pipelineCachePath` 目前被忽略；可仿照 Vulkan 的 pipeline cache（加载/销毁时写回）。
5. **异步管线创建未实现。** `createRenderPipelineAsync`/`createShaderModuleAsync` 使用基类默认实现（返回 `false`）。
6. **Argument buffer 未使用。** 当前是经典显式槽位映射；对多数场景足够，但大量绑定时 argument buffer 能减少编码开销。切换时需要 `MSLResourceBinding` 为每个 set 的 argument buffer 单独分配槽位（避免与顶点流冲突）并使用 `MTLArgumentEncoder`。
7. **`MTLHeap` / 子分配未使用。** 每次资源创建是一次 `newTexture`/`newBuffer`；高频创建/销毁的场景可引入 heap 或自有子分配器。
8. **查询覆盖有限。** 时间戳依赖 blit 采样点；遮挡查询已编码但未被测试覆盖；`PipelineStatisticsQuery` 明确不支持。`isQueryResultAvailable` 恒为 `true`（Metal 没有逐查询可用性信号）。
9. **校验层。** `DeviceConfiguration::enableValidation` 无法在运行时打开 Metal Validation（它由 Xcode scheme 或环境变量控制），已在 `appendDiagnostics` 里如实说明。建议调试时使用 `MTL_DEBUG_LAYER=1`/Xcode 的 Metal Validation。
10. **窗口演示。** 目前没有使用 `CAMetalLayer` 的 macOS/iOS 演示程序；`WindowSurface` 的 Metal 路径逻辑已完成，但需要宿主视图（`NSView`/`UIView` 的 `layerClass` 改为 `CAMetalLayer`）才能被自动化覆盖。`examples/graphics/mac` 的 `mac_demo` 仍走 CGL/OpenGL。
11. **每管线重复 MSL 编译。** 同一 shader module 被多个管线使用时重复 `newLibraryWithSource:`。可加 `(module, pipelineLayout)` 缓存，或在 `MetalShaderModule` 里缓存“单 set 布局”的常见情形。
12. **`MTLRenderPipelineDescriptor` 未使用 `inputPrimitiveTopology`/layered rendering**；`SampleRateShading`、geometry/tessellation 能力声明为不支持（与实现一致）。

---

## 10. 变更文件清单

```text
新增
  modules/graphics/backend/include/private/backend/metal/MetalDevice.h
  modules/graphics/backend/include/private/backend/metal/MetalCommandBuffer.h
  modules/graphics/backend/include/private/backend/metal/MetalEnums.h
  modules/graphics/backend/include/private/backend/metal/MetalFactory.h
  modules/graphics/backend/src/metal/MetalDevice.mm
  modules/graphics/backend/src/metal/MetalResources.mm
  modules/graphics/backend/src/metal/MetalCommandBuffer.mm
  modules/graphics/backend/src/metal/MetalPresentation.mm
  modules/graphics/backend/src/metal/MetalEnums.mm
  modules/graphics/backend/src/metal/MetalCross.h
  modules/graphics/backend/src/metal/MetalCross.mm
  modules/graphics/backend/tests/test_MetalCross.cpp
  examples/effect/EffectVerification.h
  examples/effect/EffectVerification.cpp
  examples/effect/effect_mac_demo.cpp
  Metal后端实现进度与问题记录.md   （本文件）

修改
  CMakeLists.txt                                 新增 CORAL_BACKEND_SUPPORTS_METAL；Apple 上强制关闭 Vulkan
  cmake/CoralGraphicsDependencies.cmake          Apple 上强制关闭 Vulkan
  modules/graphics/CMakeLists.txt                Metal 与 Vulkan 共用 Vulkan 环境 shader 集
  modules/graphics/backend/src/Device.cpp        macOS 优先级去掉 Vulkan
  modules/graphics/backend/CMakeLists.txt        Metal 选项/源文件/frameworks/ARC/测试/配置规则
  third_party/SPIRV-Cross/coral/CMakeLists.txt   编译 spirv_msl.cpp
  modules/graphics/backend/src/Device.cpp        Backend::Metal 分支
  modules/graphics/src/gpu/GpuRenderer.cpp       usesVulkanShaderSet 接受 Metal
  modules/graphics/backend/tests/test_PlatformRunner.cpp
                                                 Metal 排到首位、implemented=true、MSAA 用例覆盖 Metal
  README.md / modules/graphics/README.md         Metal 选项、默认后端、目录与说明
  examples/effect/CMakeLists.txt                 新增 effect_mac_demo 与 3 条 CTest
  examples/README.md                             macOS 验证命令、测试表、Metal 默认后端说明
```

---

## 11. 复现与调试小抄

### 11.1 examples/effect 的 macOS 验证

`effect_demo` 只回答“图跑起来了”：它打印一帧的中心像素，但不判断像素对不对，也不说明用的是哪个后端。
新增的 `effect_mac_demo`（`examples/effect/effect_mac_demo.cpp` + `EffectVerification.{h,cpp}`）回答的是验证问题：

- **同一张确定性图**：`TestPatternInputNode`（8 条 SMPTE 彩条）→ copy pass → `ReadbackOutputNode`；
- **逐条检查**：在每条彩条中心取像素，核对预期的 RGB 与不透明度（容差 2），失败时打印条号、坐标和实际像素；
- **跨后端比较**：Metal 是参照，OpenGL 的结果必须逐像素在容差内一致；不一致时打印第一个差异像素；
- **固定后端**：`--backend metal|opengl|vulkan` 通过 `EffectEngineConfig::externalDevice` 先以
  `allowFallback=false` 创建设备，因此“请求 Metal 却回退到 OpenGL”是失败而不是静默通过；后端不可用返回 77；
- **注册方式**：`effect_mac_demo`（全部后端）、`effect_mac_demo_metal`、`effect_mac_demo_opengl` 三条 CTest，
  失败时能直接看出是哪个后端。

之所以用离屏读回而不是开窗：验证要能在构建机/CI 上跑，窗口只会让它变成需要人盯着的东西。窗口呈现路径的验证仍列在 §9 第 10 条。

```sh
# 只用 Metal 跑后端套件
CORAL_RHI_BACKEND=metal ./build-mac/modules/graphics/backend/test_backend

# 只跑某个后端用例（Metal 总是第一个被迭代）
./build-mac/modules/graphics/backend/test_backend \
    --gtest_filter='PlatformRunner.IndexedDrawsKeepTheirIndicesWhenTheVertexArrayChanges'

# 画布路径（默认 Metal）
./build-mac/modules/graphics/test_graphics --gtest_filter='GpuSurface.*'

# 命令数/通道跟踪（上层已有的开关）
CORAL_GL_TRACE_COMMANDS=1 CORAL_GPU_TRACE_PASSES=1 \
    ./build-mac/modules/graphics/test_graphics --gtest_filter='GpuSurface.AnOffscreenFrameDrawsAndReadsBack'

# Metal 校验（由环境变量而不是设备标志控制）
MTL_DEBUG_LAYER=1 ./build-mac/modules/graphics/backend/test_backend
```

排障顺序建议：先看 `appendDiagnostics()` 的设备行（`Metal 2.2+` / `coral/backend/metal` / GPU family），再看是否出现 `[backend/metal]` 的 error；如果像素为空但 draw 计数正确，优先检查 §7.1 的槽位映射与 §7.3 的 resolve 语义。
