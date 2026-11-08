# 自审记录：ADR-0149 实现纰漏

本文档记录对照 ADR-0149 契约逐条复查实现后发现的问题。每一项都给出具体位置、
失败后果、以及修复方式——不是"已知局限"清单，而是"必须修"清单。

---

## 1. 【严重】新机制没有任何端到端测试（最实质的偷懒）

**现象**：`tests/flux/integration/test_partial_paint.c` 使用的表面来自
`flux_surface_desc` 未设置 `vk_surface_khr`，即 **offscreen surface**。

**后果**：`content_backed = !s->offscreen && s->content.image` 在测试中恒为
false。也就是说：

- content image 从未被创建（`content_image_create` 开头就 `if (s->offscreen) return`）；
- `flux_surface_sync_content` 第一行就返回 `FLUX_OK`；
- `flux_damage_history_commit` 从未执行；
- 整个 ADR-0149 的 present 路径 **一次都没跑过**。

而我却在上一轮报告里写了"`test_partial_paint`（56 检查，lavapipe）通过"，
并把它当作机制闭环的证据。这是错的——它测的是 offscreen 的旧路径（本来就支持
局部 pass），与新机制无关。

**根因**：我只检查了"测试通过"，没有检查"测试是否覆盖了我改的代码路径"。
这正是 ADR-0144 负知识里那条"Never write a partial-paint test that pre-fills the
destination"的同类错误：测试让实现看起来正确，而它没有触碰被测契约。

**修复**：用 `VK_EXT_headless_surface` 构造真正的 **windowed surface**
（`s->offscreen == false`，有 swapchain，有 content image），端到端断言：

1. 局部 pass 只写脏区，其余区保留完整帧（读回走 content image）；
2. 连续帧数 > 轮转图像数 × 2，每张图都必须是一致完整帧（稳态不发散）；
3. 两张远离的矩形不被塌缩成包围盒（中间的带必须保留）。

已验证 `vkCreateHeadlessSurfaceEXT` 在本机（llvmpipe）返回 `VK_SUCCESS`。

---

## 2. 【严重】超过 64 个矩形时静默截断，与契约相反

**位置**：`libs/flux/src/canvas/internal.h` 的 `canvas_pass_render_bounds` /
`canvas_pass_areas` / `canvas_damage_select`，三处都是：

```c
const uint32_t count = config->damage.count < FLUX_DAMAGE_CAPACITY
                           ? config->damage.count : FLUX_DAMAGE_CAPACITY;
```

**后果**：调用方传入 100 个矩形时，后 36 个被**静默丢弃**。契约写的是
"超容量退化为全屏，绝不截断"，实现做的是截断。截断的后果正是 ADR 里反复强调的
**残留脏像素**——被丢弃的区域永远不会被重绘。

同时 `flux_frame_damage_set` 的注释也自相矛盾：注释写"空集表示全屏"，但
`canvas_pass_covers_extent` 把"空集"解释为全屏、把"超容量"解释为截断，两者不一致。

**修复**：`canvas.c` 在解析 descriptor 时立刻检查
`damage.count > FLUX_DAMAGE_CAPACITY`，**退化为全屏**（`damage = {}`,
`clear_extent = true`），并把超容量计数暴露给 `flux_frame_record_written` 的
overflow 路径。三个 helper 里的 `?:` 截断随之删除，改为断言 count 合法。

---

## 3. 【中】`damage_scope_rejected` 是死字段

**位置**：`libs/flux/src/core/internal.h:993`。

**后果**：声明、注释俱全，全仓库零引用。这是纯粹的架构腐化：一个"看起来
在做契约检查"的字段，实际什么也没做，会误导下一个读者以为越界 scope 已被处理。

**修复**：删除。真正需要的诊断应该在 `canvas.c` 的解析点产生（见第 2 项）。

---

## 4. 【中】canvas.h 公共头谎称支持 `maxScissorSlots` 协商

**位置**：`libs/flux/include/flux/canvas.h:170-175`：

> "A device whose `maxScissorSlots` is smaller lowers the effective bound and
> reports `FLUX_ERROR_UNSUPPORTED` from the pass rather than silently narrowing
> the scope."

**后果**：这是**不实文档**。实现从不查询 `maxScissorSlots`，也从不因此报
`FLUX_ERROR_UNSUPPORTED`。实际上根本不需要协商：所有 `vkCmdSetScissor` 调用都是
`count = 1`（每次 draw 一个矩形），所以 `FLUX_DAMAGE_CAPACITY = 64` 与
`maxScissorSlots` 无关。

不实文档比没有文档更糟——它会让下一个人以为存在一条不存在的代码路径。

**修复**：改写为事实：本实现每次 draw 只用一个 scissor，因此矩形数量与
`maxScissorSlots` 无关；容量上限来自 history 契约（`FLUX_FRAME_DAMAGE_CAPACITY`）。

---

## 5. 【中】非 canvas 的 windowed pass 不记录写入区

**位置**：`flux_frame_begin_pass` 被 `examples/flux/scene_cube.c:219`、
`examples/vista/gltf_viewer.c:322` 直接调用，不经 canvas。

**后果**：这些路径向 content image 写入，但没有任何人调用
`flux_frame_record_written`。若一个宿主的帧里同时有 canvas pass 和裸 pass，
history 会**漏记**裸 pass 写过的区域。如果该帧唯一的写入来自裸 pass，history 会
commit 一个空集——按设计空集 = "全屏写入"，反而安全；但"canvas 写了 A、裸 pass
写了 B"的混合帧会漏 B。

**修复**：在 `flux_frame_begin_pass` 里，当 attachment 未指定（即渲染到帧的
render image）时，按 renderArea 记录写入区。这样无论调用方是谁，写过的区域都
进 history。

---

## 6. 【低】逐 draw scissor 的"并集"路径没有测试

**位置**：`canvas_damage_select` — 一个 draw 跨越两个脏矩形时取并集。

**后果**：这条分支没有测试覆盖。它是安全的（overshoot 只是多画），但未验证。

**修复**：在 headless-surface 测试里覆盖——一个跨越两带的 draw 必须让两带都
被重绘，且中间带**不被**重绘。

---

## 不属于本 ADR 范围、但已确认的既有问题

- `tile_gpu_compute` 在 llvmpipe 上 SIGSEGV：已在 HEAD 的独立 worktree 复现，
  与本次改动无关。
- `device_hardware` 要求非 CPU 设备：环境限制。
- bench SLA 需要 Tracy 的 8086 端口，被无关的 `synapse` 进程占用：环境限制。
