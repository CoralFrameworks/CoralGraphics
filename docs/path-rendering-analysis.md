# CoralGraphics Path/Stroke 渲染问题分析与修复

> 本文是对 `docs/path-rendering-analysis.md` 初版的复核与修订。初版把主要根因归到
> `strokePath` 忽略 open contour 和 stencil reset 的 open defect；复核后确认：
> 前者的确存在，但不是最严重的一条；后者在当前代码上**不是**缺陷，初版依据的测试本身
> 选错了形状。真正的首要根因是 stroke 输出的各个片元 **winding 方向不一致**，在 nonzero
> 填充下相互抵消。本文给出证据、修复和回归测试。

## 摘要

RadioGroup、SearchField、菜单箭头、checkbox 和焦点环出现的“横向残线 / 方块 / 帧间闪烁”，
是同一套 path/stroke 管线上几个独立缺陷叠加的结果：

1. **CoralGraphics 的圆、线、圆角矩形最终都走 `Canvas::drawPath`**（这一条初版判断正确）。
2. `drawPath` 的 stroke 分支由 `raster::strokePath` 把路径展开成独立四边形、join 片元和
   cap 片元。
3. **`strokePath` 输出的 edge quad 与 join disc 的绕向相反**。stroke 靠 nonzero 规则把这些
   重叠片元取并集；方向相反时重叠区会抵消为 0。结果是圆角边框每个角出现缺口、描边圆环
   整圈变淡（每个扁平化顶点都是一个 join），也就是 UI 上看到的残线、断环和“闪一下”。
   两点直线之所以碰巧正常，是因为它把同一条边的 quad 发出了两次（正向 + 反向），
   双重 winding 刚好扛住了一个反向 disc——这也是初版和旧测试没发现它的原因。
4. **`strokePath` 不检查 `contour.closed`**：open contour 会多出一条从末点回到首点的
   虚构边。三点及以上的折线（勾、箭头、折线图标）会被这条边填成三角形/斜线。
5. **stroke 不遵守 `Paint::strokeCap` / `strokeJoin`**：函数只接收宽度，固定输出 round
   join + round cap。SVG 的 `stroke-linecap`/`stroke-linejoin` 因此被静默忽略。
6. **stroke 直接用 path 的 fill rule**：若 path 是 even-odd，stroke 的并集会被奇偶规则
   减掉，两点直线甚至完全不可见。
7. **coverage field 帧间缓存有一个提前退出的 slot 检查**：只要前面的 field 内容变化，
   后面的 field 就不再检查“新 box 是否还装得进旧 slot”。一个变大的 field 会被写进过小的
   槽位，覆盖邻居的 texel，产生方块/残线，并且只有在“前面有变化、后面又变大”的帧才出现。
8. **stencil reset 复核结论：当前实现是正确的**。accumulation 只在 fan 三角形内写 stencil，
   cover pass 的 stencil 写掩码为 0，reset 用同一组 fan 三角形 + `StencilOp::Zero`，因此
   清零范围恰好等于可能被写过的范围。初版的“open defect”来自一个用两个**不相交**矩形
   构造的测试：第二个 path 的 cover 矩形就是它自己的 bounds，根本不会覆盖第一个 path 的
   stencil 区域，所以那个测试既不能证明也不能证伪 reset。

---

## 1. 调用链

### 1.1 UI 基元全部落到 path

`CoralGraphics/modules/graphics/src/Canvas.cpp`：

```cpp
void Canvas::drawLine(...)      { ... drawPath(path, paint); }
void Canvas::drawCircle(...)    { drawOval(...); }
void Canvas::drawOval(...)      { drawPath(ovalPath(bounds), paint); }
void Canvas::drawRoundRect(...) { drawPath(roundRectPath(rect), paint); }
```

所以 RadioButton 的圆环、SearchField 的放大镜、Select 的箭头、CheckBox 的勾、焦点环、
圆角边框共享同一条 path/stroke 管线。

### 1.2 后端把 stroke 展开成轮廓

`GraphiteCanvasBackend.cpp`（raster 后端同理）：

```cpp
const bool stroked = paint.style == Paint::Style::kStroke;
const FillType effectiveFillType = stroked ? raster::strokeFillType() : fillType;
const std::vector<raster::Contour> contours = stroked
        ? raster::strokePath(devicePath, raster::strokeStyleFor(paint, scale))
        : raster::flattenPath(devicePath);
```

之后小尺寸、抗锯齿、无 shader 的 solid path 走 coverage field；大尺寸或关闭 AA 的 path
走 stencil fan + cover。

---

## 2. 复核发现的问题

### P0-1：stroke 片元的 winding 方向不一致（首要根因）

旧实现每次 stroke 生成两类片元：

- 每条边一个四边形，点序为 `a+o, b+o, b-o, a-o`；
- 每个顶点一个圆盘，`appendDisc` 按角度 0 → 2π 递增生成。

两者的 signed area 符号相反。旧代码依赖“nonzero 填充把重叠片元取并集”，但 nonzero 只对
**同向**重叠做加法；反向重叠会把 winding 减回去。一个封闭圆角矩形 stroke 的角部因此出现
半圆形空洞：

```text
修复前（4px 描边矩形）              修复后
..-+-+####+-+-..                  ..-+########+-..
..+#.-####-.#+..                  ..+##########+..
..-.########.-..                  ..############..
..+-########-+..                  ..############..
..####....####..                  ..####....####..
```

描边圆环更明显：圆被扁平化成许多顶点，每个顶点都是 join，整圈都落进抵消区，所有像素都
只有部分覆盖（旧输出里一个 255 都没有）。这正是 SearchField 放大镜、Spinner 圆环、
RadioButton 焦点环“发虚 / 断线”的来源。

证据：把旧 `strokePath` 输出中点数大于 4 的 disc 片元反转点序后重新填充，矩形角部缺口
立刻消失；这正是新实现 `StrokeOutline::append` 所做的事——每个片元都按 signed area
统一绕向，再交给 nonzero 填充。

### P0-2：`strokePath` 忽略 open contour

旧实现：

```cpp
for (size_t i = 0; i < points.size(); ++i) {
  const Point& b = points[(i + 1) % points.size()];
  appendEdge(a, b, quad.points);
}
```

没有检查 `contour.closed`。三点折线 `A -> B -> C` 会多生成一条 `C -> A`，表现为斜线、
三角形色块或横向残线。两点直线不受影响（`B -> A` 与 `A -> B` 是同一个 quad，方向也相同）。

修复时还要处理“用 `lineTo(start)` 显式闭合”的写法：`flattenToPolylines` 会丢掉重复的
末点，所以必须在丢掉之前把 contour 记为 closed，否则修好 open contour 后又会丢掉显式
闭合边。现在 `finishContour` 用 `closing || returnsToStart` 记录闭合。

### P0-3：stroke 不接受 cap/join

`Paint` 的默认值是 `strokeCap = kButt`、`strokeJoin = kMiter`，SVG 也解析并写入这两个
字段；但旧的 `strokePath(path, width)` 固定输出 round join + round cap，调用方传入的
值根本到不了几何层。后果：

- 每条线都会比声明的长度多出半个线宽的圆头；
- 直角边框的角被画成圆角；
- SVG 的 `stroke-linecap="butt"` / `stroke-linejoin="miter"` 不生效。

现在 `StrokeStyle` 携带 width/cap/join/miterLimit，`StrokeOptions.h` 负责从 `Paint` 映射；
butt/round/square 与 miter/round/bevel 都有实现。

### P0-4：stroke 继承了 path 的 fill rule

stroke 的轮廓是一组重叠片元，**必须**用 nonzero 填充。若 path 自己设了 even-odd，旧代码
会把同一个 rule 传给 stroke，重叠区按奇偶抵消——两点直线的 quad 被发出两次，直接完全
消失。现在 `Canvas::drawPath`、两个后端和 `StrokeRec` 都把 stroke 的填充规则钳到
`FillType::Winding`。

### P1：coverage field 缓存的 slot 检查提前终止

`GpuRenderer::resolveCoverageFields` 用 `everyFieldFits` 决定能否保留上一帧的 atlas 布局。
旧循环：

```cpp
if (!everyFieldSame) continue;          // 第一个内容变化后，后续 field 全部跳过
const FieldLattice& before = ...;
if (!before.slotHolds(identity)) everyFieldFits = false;   // 这句不再执行
```

`everyFieldSame`（是否完全没变）和 `everyFieldFits`（每个新 box 是否还装得进旧 slot）是
两个独立问题。一个“field 0 内容变了、field 1 同时变大”的帧会保留旧布局，然后把变大的
field 1 写进过小的槽位，覆盖相邻 field 的 texel。表现为帧间出现方块/残线，且只有特定
组合的帧才触发。修复后 slot 检查对每个 field 都执行；回归测试
`GpuSurface.AFieldThatGrowsAfterAnEarlierFieldChangedIsStillRepacked` 在原逻辑下会在
`(14, 13)` 与 cold render 不一致。

### P1（更正）：stencil reset 不是缺陷

初版称 `Variant::StencilReset` 没有清掉 stencil，依据是 `OnePathDoesNotBleedIntoTheNext`。
但该测试画的是两个**不相交**矩形，第二个 path 的 cover 矩形根本不会覆盖第一个 path 的
stencil 区域，所以它无法暴露这个问题。

真正能暴露问题的形状是：先画一个大矩形，再在它内部画一个**关闭 AA 的圆**。圆的 cover
矩形是它的 bounding box，box 内、圆外的像素如果仍保留前一个 path 的 stencil，就会被
第二个 path 的颜色整块涂满——也就是“图标变成方块”。新增测试
`GpuSurface.ANonAntialiasedPathDoesNotBleedIntoTheNext` 正是这个形状，Metal 与 OpenGL
都通过。原因也成立：accumulation 只在它光栅化的 fan 三角形内写值，cover 的 stencil
writeMask 为 0，reset 用同一组三角形 + `StencilOp::Zero`，清零范围恰好等于可能写过的
范围。因此不需要全 target clear（早期尝试全 target clear 会破坏背景）。

### P2：Layer 录制缓存 / damage（仍属 CoralUI）

CoralUI 的 `Layer` 会把子树拆成 child segments 并缓存；只做 panel damage 时可能复用上一帧
的 display list。单帧的 path 错误会被缓存放大成“移动鼠标后出现、再移动又恢复”。这属于
CoralUI 的 Layer/damage 设计，不是 path 几何本身；但它会放大上面 P1 这类帧间缓存错误，
所以在修 CoralGraphics 的缓存逻辑时也要一起看。

---

## 3. 当前症状与根因对应

| 症状 | 根因 | 状态 |
|---|---|---|
| RadioGroup 选中行下横线 / 焦点环缺口 | stroke 片元 winding 相反；旧焦点环 stroke | 已修（CoralUI 的填充圆 workaround 现在可以移除） |
| SearchField 放大镜发虚 / 断环 | 描边圆每个顶点 join 片元与 edge 片元抵消 | 已修 |
| 下拉箭头 / 勾的端点形状不对 | 固定 round cap，忽略 `strokeCap` | 已修 |
| 三点折线图标出现斜线/三角块 | open contour 被强制闭合 | 已修 |
| even-odd path 的 stroke 消失 | stroke 继承 even-odd | 已修 |
| hover 后图标闪一下 | CoralUI Layer 缓存 + 帧间 field 缓存 | CoralGraphics 缓存 bug 已修；Layer/damage 仍待 CoralUI 处理 |
| 非 AA path 变成方块 | 初版怀疑 stencil reset，实测不是 | 复核为不是缺陷；新增真实回归测试 |
| 大面积方块 | 初版怀疑 stencil reset / layer cache | 需按上面的新测试重新排查；stencil 侧已排除 |

---

## 4. 实际修复内容

### 4.1 stroke 几何重写

文件：

- `modules/graphics/include/private/graphics/Flatten.h`
- `modules/graphics/src/raster/Flatten.cpp`

要点：

- 新增 `StrokeCap` / `StrokeJoin` / `StrokeStyle`；`strokePath` 改为接收 `StrokeStyle`。
- `StrokeOutline::append` 统一所有片元的绕向（signed area 为正则反转），从结构上消除
  “某类片元方向相反”的整类问题。
- edge quad、round/bevel/miter join、butt/round/square cap 分别实现；miter 尊重
  `miterLimit`，超限退化为 bevel。
- open contour 只发 `n - 1` 条边，端点按 cap 处理；closed contour 发 `n` 条边并在每个
  顶点（含首尾 wrap）做 join。
- `flattenToPolylines` 在丢掉重复末点时记录“显式闭合”。

### 4.2 cap/join/fill rule 贯通

文件：

- 新增 `modules/graphics/include/private/graphics/StrokeOptions.h`
- `modules/graphics/src/gpu/GraphiteCanvasBackend.cpp`
- `modules/graphics/src/raster/RasterCanvasBackend.cpp`
- `modules/graphics/src/Canvas.cpp`
- `modules/graphics/src/StrokeRec.cpp`

要点：

- `strokeStyleFor(paint, scale)` 把 width/cap/join/miter 映射到 raster 层。
- 三个入口都把 stroke 的填充规则钳到 nonzero。
- `StrokeRec::applyToPath` 使用完整的 stroke 参数，不再只传宽度。

### 4.3 coverage field 缓存

文件：`modules/graphics/src/gpu/GpuRenderer.cpp`

- `everyFieldSame` 与 `everyFieldFits` 分开计算；slot 检查对每个 field 都执行。
- 保留“内容不变 / 只是移动”时复用 atlas 的既有优化。

### 4.4 文档与过期注释

- `GpuRenderer::drawPath` 里原来的 `OPEN DEFECT` 长注释改为“为什么 fan reset 是正确的”，
  并记录初版结论为什么错。
- `OnePathDoesNotBleedIntoTheNext` 的注释改为它实际覆盖的范围，并指向新的非 AA 测试。

---

## 5. 回归测试

CPU / raster（`modules/graphics/tests/test_Canvas.cpp`）：

- `CanvasTest.AnOpenPolylineIsNotClosedByItsStroke`
- `CanvasTest.AClosedContourJoinsItsCornersWithoutHoles`
- `CanvasTest.AContourThatReturnsToItsStartKeepsItsClosingEdge`
- `CanvasTest.AStrokedCircleIsASolidRing`
- `CanvasTest.AStrokeHonoursItsCap`
- `CanvasTest.AStrokeHonoursItsJoin`
- `CanvasTest.AnEvenOddStrokeIsStillFilled`
- `CanvasShapes.ALineIsStrokedBetweenItsPoints`（更新为 butt cap 语义）

GPU（`modules/graphics/tests/test_Gpu.cpp`）：

- `GpuSurface.AnOpenPolylineIsNotClosedByItsStrokeOnTheHardwarePath`
- `GpuSurface.AStrokedCircleIsASolidRingOnTheHardwarePath`
- `GpuSurface.AStrokedCircleIsASolidRingThroughTheStencilRoute`
- `GpuSurface.AnEvenOddStrokeIsFilledOnTheHardwarePath`
- `GpuSurface.ANonAntialiasedPathDoesNotBleedIntoTheNext`（真正走 stencil 的隔离测试）
- `GpuSurface.AFieldThatGrowsAfterAnEarlierFieldChangedIsStillRepacked`

Lottie trim 测试更新：

- `LottieTrim.ATrimmedStrokeIsShorterThanTheUntrimmedOne`
- `LottieTrim.TheTwoModesDifferOnASecondShape`

这两个测试原来把“默认 round cap 会多出半圆”写成期望；格式的默认 cap 是 butt，旧期望
实际是在断言渲染器的 bug。现在按 butt cap 语义更新。

---

## 6. 验证

- `test_graphics`：1136 个测试，1110 通过、26 跳过、0 失败（Metal 默认后端）。
- 关键 GPU 测试在 `CORAL_RHI_BACKEND=opengl` 下也通过。
- 用修复后的 CoralGraphics 重新构建并运行 CoralUI 的 `test_ui`：747 个测试，741 通过、
  6 跳过、0 失败（CoralUI 通过 `../CoralGraphics` 引用当前工作区，所以这是真实的端到端
  验证）。

---

## 7. 后续建议

1. CoralUI 可以把 RadioButton 的填充圆、SearchField 的裁剪等 workaround 回退为真正的
   stroke；根因已在 CoralGraphics 修复。
2. Lottie 目前不解析 `lc` / `lj`（line cap/join），SVG 已解析但之前被忽略；现在
   CoralGraphics 已支持，Lottie 解析可以作为下一步。
3. 非均匀缩放下 stroke 宽度仍按 `scaleOf(transform)` 取一个标量，是文档里已声明的近似；
   若要在强 skew 下精确，需要把宽度作为椭圆或做设备空间 offset。
4. hairline（1 device px）仍未实现，`StrokeRec` 会拒绝并让调用方按未描边处理。
5. CoralUI 的 Layer/damage 缓存仍需单独审查；它能放大帧间缓存问题，但不是 path 几何
   问题的根源。
