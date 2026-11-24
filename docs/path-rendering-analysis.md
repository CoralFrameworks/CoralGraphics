# CoralGraphics Path/Stroke 渲染问题分析

## 摘要

RadioGroup、SearchField、菜单箭头、checkbox 和焦点环出现的“横向残线 / 方块 / 帧间闪烁”，不是单个控件的状态错误，而是几层问题叠加：

1. **CoralUI 的所有圆、线、圆角矩形最终都走 `Canvas::drawPath`。**
2. `drawPath` 的 stroke 分支由 `raster::strokePath` 把路径扩展成独立四边形和圆盘。
3. **`strokePath` 没有尊重 open contour，始终把最后一个点连回第一个点**，会生成原本不存在的闭合边。
4. 小尺寸、抗锯齿、无 shader 的 path 会走 **coverage field** 路径；大尺寸或关闭 AA 的 path 走 **stencil** 路径，而 stencil reset 代码里仍有一个已记录的 open defect。
5. CoralUI 的 Layer 录制缓存和 popup damage 会把单帧的 path 错误放大成“移动鼠标后出现 / 再移动一次又恢复”的现象。

因此，控件层的临时规避（RadioButton 用填充圆、SearchField 裁剪绘制）可以消掉当前症状，但不能修好整个图形栈。

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

所以：

- RadioButton 的圆环；
- SearchField 的放大镜；
- Select 的箭头；
- CheckBox 的勾；
- 焦点环；
- 圆角边框；

全部共享同一条 path/stroke 管线。

### 1.2 Graphite 后端把 stroke 展开为轮廓

`CoralGraphics/modules/graphics/src/gpu/GraphiteCanvasBackend.cpp`：

```cpp
const std::vector<raster::Contour> contours =
    paint.style == Paint::Style::kStroke
        ? raster::strokePath(devicePath, paint.strokeWidth * scale)
        : raster::flattenPath(devicePath);
```

之后：

- `recordCoverageField(...)`：小、抗锯齿、solid path 走 coverage field；
- 否则记录 `DrawList::Kind::Path`，由 `GpuRenderer::drawPath` 走 stencil fan + cover。

---

## 2. 发现的具体问题

### P0：`strokePath` 会把 open contour 强制闭合

`CoralGraphics/modules/graphics/src/raster/Flatten.cpp`，`strokePath`：

```cpp
for (size_t i = 0; i < points.size(); ++i) {
  const Point& a = points[i];
  const Point& b = points[(i + 1) % points.size()];
  appendEdge(a, b, quad.points);
}
```

这里的问题不是 `%` 本身，而是 **没有检查 `contour.closed`**。

对于一个 open polyline：

```text
A(10,10) -> B(30,10) -> C(30,30)
```

实际会生成三条边：

```text
A -> B
B -> C
C -> A   // 不存在，应该没有
```

最后一条 `C -> A` 是一条斜边，会出现在裁剪/覆盖结果里，表现为：

- 控件下面多出一条斜线或横线；
- 在某些缩放/裁剪下只看到一段横向残线；
- 同一个 path 在不同 frame 下因为 coverage field 的边界/裁剪不同而“闪一下”。

`flattenToPolylines` 已经维护了 `contour.closed`，stroke 分支只需要遵守它：

```cpp
const size_t edgeCount = contour.closed ? points.size() : points.size() - 1;
for (size_t i = 0; i < edgeCount; ++i) {
  const Point& a = points[i];
  const Point& b = points[(i + 1) % points.size()];
  ...
}
```

### P0：stroke 端点和 join 的语义不完整

`strokePath` 当前对**每个顶点**都加一个圆盘，并且对**每条边**加一个闭合四边形。它实际上固定为：

```text
round join + round cap
```

这对 UI 图标大多可以接受，但需要注意：

- open contour 的起点/终点也会加圆盘，这是 round cap；
- 如果未来支持 butt/square cap，当前结构会把 cap 和 join 混在一起；
- 每次 stroke 产生 `N` 个四边形 + `N` 个圆盘，全部是独立 contour，依赖 nonzero 填满并集。

这本身不是错误，但让 **coverage field 的 contour 数量和 bounds 比预期大**，更容易触发 field 边界/裁剪问题。

### P1：小 path 走 coverage field，而不是 stencil

`GraphiteCanvasBackend::drawPath` 的门槛：

```cpp
const bool coverage =
    command.state.antiAlias &&
    !paint.hasShader() &&
    !paint.hasColorFilter() &&
    bounds.getWidth() <= 1024.0f &&
    bounds.getHeight() <= 1024.0f;
```

因此 UI 里的图标/焦点环/箭头大多走 coverage field：

```text
CPU 生成 coverage texture
 -> coverage.frag 采样
```

这条路径没有 stencil reset 问题，但会继承 **stroke contour 本身的错误**。也就是说：

- RadioButton 的残线如果来自 stroke，修 `strokePath` 才会消失；
- 只在 UI 里换成填充圆只是绕过。

### P1：大 path 的 stencil reset 仍有 open defect

`GpuRenderer.cpp` 的 stencil reset：

- 代码里仍保留 `OPEN DEFECT: this pass does not appear to clear the stencil` 注释；
- reset 用 fan 重绘：
  ```cpp
  return bindFans(resetPipeline, mPathSet);
  ```
- 对于自重叠、圆角、多 contour 的 path，reset 和 accumulation 的覆盖可能不完全一致；
- 一旦残留 winding，下一个 path 的 cover 会命中错误 stencil。

这条路径主要影响：

- 关闭抗锯齿的 path；
- 带 shader / color filter 的 path；
- 大于 1024 的 path。

它不是当前小图标的主路径，但属于同一套 path 架构的潜在缺陷。

### P2：Layer 录制缓存会放大单帧错误

CoralUI 的 `Layer` 会把子树拆成 child segments 并缓存。popup/menu hover 只把 menu segment 标脏时：

- page segment 可能复用上一帧的绘制列表；
- path 的 image/atlas、bounds 或 damage 与当前帧不一致；
- 单帧的线条/方块会留在缓存里，直到下一次整页损坏。

这解释了两个 UI 层现象：

- 只做 panel damage 时“闪一下”；
- 只做 full frame 时“方块残留”。

这属于 CoralUI 的 Layer/damage 设计问题，不属于 path 几何本身；报告将它和 GPU path 问题分开。

---

## 3. 当前症状与根因对应

| 症状 | 最可能来源 | 说明 |
|---|---|---|
| RadioGroup 选中行下横线 | `strokePath` open contour 闭合 / 旧焦点环 stroke | RadioButton 已临时改为填充圆规避 |
| SearchField 放大镜异常 | stroke 轮廓 / coverage field | 放大镜是 circle + line 的 stroke |
| 下拉箭头方块 | stroke/coverage field 或 stencil 残留 | 箭头是 `drawLine` |
| 菜单 checkmark 方块 | 小 path coverage field | checkmark 是两条 stroke |
| hover 后图标闪一下 | Layer 录制缓存 + damage | 不一定是 path 本身 |
| 大面积方块 | stencil reset / layer cache | 需要分路径排查 |

---

## 4. 建议修复顺序

### 第 1 步：修 `strokePath` 的 open contour（低风险、先做）

文件：

```text
CoralGraphics/modules/graphics/src/raster/Flatten.cpp
```

修改：

- `strokePath` 检查 `contour.closed`；
- open contour 只生成 `points.size() - 1` 条 edge；
- 保留端点圆盘作为 round cap；
- 增加回归测试。

测试建议：

```cpp
// 三点 L 形 open stroke
// 断言 A->C 的对角线区域没有覆盖
```

以及：

```cpp
// 闭合矩形 stroke 仍然生成最后一条闭合边
```

### 第 2 步：为 stroke / coverage field 增加 GPU 回归

在 `GpuSurface` 增加：

- 小尺寸 stroke 圆环；
- open polyline；
- 关闭 AA 的同一形状（走 stencil）；
- 对比 CPU raster 和 GPU 的像素。

### 第 3 步：修 stencil reset

当前 fan reset 的已知缺陷需要真正解决。建议：

1. 先用 `CORAL_GPU_TRACE_PATHS=1` 和 stencil state trace 确认 reset pass 实际状态；
2. 用 **cover 区域清零** 而不是 fan 重绘，但必须只清 path 的 cover 区域，不能覆盖整帧；
3. 或者对每个 path 用独立 stencil pass，避免跨 path 状态；
4. 增加 `GpuSurface.OnePathDoesNotBleedIntoTheNext` 的自重叠/圆角版本。

注意：之前的 cover-quad 尝试导致 DemoFrame 红紫背景，说明 reset 的覆盖范围/颜色写 mask 不能被简单扩大到全 target。

### 第 4 步：UI 层减少对 stroke 的依赖

在 GPU 路径修好之前，可以保留这些规避：

- RadioButton 用填充圆（已提交）；
- SearchField 绘制裁剪（已提交）；
- 简单勾/箭头尽量用 quad 组合，而不是 `drawLine`。

但这些只是 workaround，不应替代 CoralGraphics 的 stroke 修复。

---

## 5. 当前状态

CoralUI：

```text
7b8c9ac Roll back the menu render experiments
7526831 Clip SearchField paint to its bounds
bbcc917 Draw the radio focus ring around the indicator
eebe0c8 Draw radio indicators with filled circles
```

菜单实验保留在：

```text
backup/menu-render-investigation-2026-11-24
```

CoralGraphics：

```text
e4e534b Revert "Clear the whole path cover when resetting stencil"
```

即当前回到 fan reset，未解决 stencil 的 open defect。

---

## 6. 结论

优先修 **`strokePath` 忽略 open contour**。这是最小、最明确、可测试的几何错误，能解释多段 open path 的斜线/横线。

stencil reset 是第二个独立问题，影响关闭 AA、带 shader 或大尺寸 path；不应再通过 UI 全帧重绘绕过。

Layer 录制缓存/damage 是第三个问题，属于 CoralUI；它解释“为什么同一个 path 在某些 frame 里正常、某些 frame 里变方块”，但根因仍然在底层 path 输出。
