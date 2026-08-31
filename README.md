# MicroTech App Manager 0.6.0

`layers/app_manager` 提供 ESP32-S3 上的应用运行时、静态应用注册、App/Page
导航、LVGL Screen 转换、显示休眠、输入路由和应用级 PM。业务应用位于
`layers/apps/`，系统装配入口位于 `main/app_runtime.c`。

0.6.0 的核心约束是：导航模型和所有 LVGL 对象只由 UI worker 修改；应用和页面
使用静态 App descriptor 和 App 私有路由；每个已挂载页面拥有独立 Screen；所有同步和异步导航都经过
同一个固定容量 command gateway。边缘返回只在永久 Sys Layer 上呈现手势反馈，
Page Screen 始终通过既有导航动画完成切换，不参与实时跟手。

## 1. 设计不变量

以下约束属于公共行为，不应在局部重构中改变：

1. UI worker 是 nav model、页面生命周期和 LVGL 对象的唯一写入者。
2. 物理上最多驻留 8 个 App、保留 12 个 Page，页面私有内存来自固定 arena。
3. 稳定前台只挂载一个 Page Screen；动画期间临时同时保留 source 和 target。
4. 后台 App 保留导航和私有状态，但不保留 Screen、timer、订阅、输入或活动会话。
5. `ONPAUSE` 或 `ONSTOP` 清理失败时保留实例、句柄和 cleanup ownership，以便重试。
6. App descriptor、Page route 和 Page definition 均为固件全生命周期 immutable 对象。
7. 动画、生命周期和必要清理完成后，导航命令才完成。
8. 屏幕休眠和 deinit 会先快进当前动画，再进入稳定页面状态并关闭新准入。
9. PM generation/commit、mailbox 超时和显示 DMA fence 语义保持独立有效。
10. 冷启动期间物理按键和 inactivity PM 保持关闭；首帧、面板和依赖服务提交后才开放。
11. 边缘返回捕获条和 indicator 只属于 Sys Layer，不进入 Page Screen 生命周期。
12. 手势 PRESSING 只更新 indicator；Page Screen 的位置、透明度和样式始终不实时跟手。

## 2. 模块职责

| 路径 | 职责 |
| --- | --- |
| `app_core/include/app_manager_types.h` | descriptor、Typed Blob、导航、动画、生命周期和硬件端口类型 |
| `app_core/include/app_manager.h` | 应用、导航、查询、UI dispatcher、显示和 PM 公共 API |
| `app_core/include/app_manager_config.h` | 装配层使用的 BSP、资源、驻留策略和默认动画配置 |
| `app_core/src/app_manager_builtin.c` | 静态 App registry、路由校验和查询 |
| `app_core/src/app_manager_nav_model.c` | App/Page 固定槽、双向链表、arena 和模型不变量 |
| `app_core/src/app_manager_navigation.c` | 24 槽 sync/async command gateway 和超时语义 |
| `app_core/src/app_manager_lifecycle.c` | 生命周期执行、驻留策略、失败回滚和 cleanup ownership |
| `app_core/src/app_manager_presentation.c` | neutral Screen、Page Screen、全分辨率快照/Image 转场、原生回退和触摸屏障 |
| `app_core/src/app_manager_back_gesture.c` | Sys Layer 双边捕获、程序化 A8 Canvas indicator、返回目标门控和运行时开关 |
| `app_core/src/app_manager_mailbox.c` | 任意任务到 UI worker 的固定容量 dispatcher |
| `app_core/src/app_manager_lvgl_adapter.c` | LVGL、显示、触摸、字体、资源和 suspend/resume 事务 |
| `app_core/src/app_manager_task_switcher.c` | Sys Layer 系统任务切换器：卡片、上滑关闭、清除与生命周期协作 |
| `app_core/src/app_manager_pm*.c` | app-control、输入合并、显示 PM 和 system standby 协作 |
| `app_core/linker.lf` | 保留唯一的 App descriptor 链接段 |
| `app_core/tests/host/` | model、registry、gateway、lifecycle、mailbox、PM 和 adapter 测试 |
| `app_theme/` | 六档语义字体角色 |

主要依赖关系如下：

```text
ordinary task / UI callback / lifecycle callback / Sys Layer gesture
                    |
                    v
        navigation command gateway (24)
                    |
                    v
             UI worker mailbox
                    |
        +-----------+-------------+
        |           |             |
        v           v             v
    lifecycle    nav model    presentation
        |                         |
        v                         v
 static app routes         LVGL Page Screens
```

`app-control` 是显示、亮度、输入和 standby 策略的独立串行域。它通过 mailbox 请求
UI pause/resume，不直接修改 nav model 或页面对象。

## 3. 固定容量与运行模型

| 资源 | 上限 | 满时行为 |
| --- | ---: | --- |
| 物理 App 槽 | 8 | 按驻留策略拒绝或淘汰 |
| 物理 Page 槽 | 12 | `ESP_ERR_NO_MEM` |
| 导航命令槽 | 24 | `ESP_ERR_NO_MEM` |
| App/Page ID | 最多 31 字节加 `\0` | `ESP_ERR_INVALID_ARG` |
| Typed Blob | 96 字节，其中 payload 88 字节 | `ESP_ERR_INVALID_ARG` |
| 动画时长 | 1..2000 ms，0 表示使用默认值 | `ESP_ERR_INVALID_ARG` |

构建生成的只读图片由 Apps/App Theme manifest 聚合，装配层使用
`app_manager_image_resource_config_t` 将产品语义 ID 映射
到 mmap asset index。App descriptor 使用 `uint32_t icon_id` 绑定应用图标，最近任务快照
复制该 ID，任务切换器以 40--48 px 图标显示，资源缺失或 ID 为零时回退 LVGL
symbol。初始化时 App Manager 在 PSRAM 创建小型描述符表，并校验 LVGL 9
BIN magic、`RGB565A8`、尺寸、偶数 stride、独立 A8 平面长度及重复语义 ID；像素 payload
始终直接指向 `res` 分区的 mmap Flash 地址。`app_manager_get_image()` 返回的描述符持续到
`app_manager_deinit()`，因此全部 LVGL Image 对象必须先由正常页面生命周期销毁。

每个 App 节点拥有独立 Page 栈，App 节点本身组成前台历史：

```text
App history:  home <-> settings <-> setup
                                      ^ active_app

settings:     root <-> power <-> about
                                  ^ current_page
```

页面 arena 按 `APP_MANAGER_MAX_PAGE_INSTANCES` 分槽，每槽大小由
`CONFIG_APP_MANAGER_PAGE_STATE_BYTES` 唯一确定。Page definition 的 `memory_size` 必须
不大于 `APP_MANAGER_PAGE_STATE_BYTES`；页面分配和释放时只清零声明的范围。
`app_manager_this_page_memory()` 只在当前生命周期回调期间有效；ops 通过 callback-scoped
context 访问同一 Page state。页面成功释放后不得继续保存该指针。

`app_manager_config_t.max_resident_apps` 为 0 时使用物理上限 8。驻留策略为：

- `APP_MANAGER_RESIDENT_REJECT`：达到上限后拒绝新 App。
- `APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND`：淘汰最旧、非 pinned 的后台 App。

`APP_MANAGER_APP_FLAG_PINNED` 阻止自动淘汰和普通后台清理。HOME 应标为 pinned；
双击 HOME 打开系统任务切换器、单击 HOME 回到 HOME 根页（见第 10.1 节）。需要释放
其他后台 App 时提交 `APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND`。

## 4. 静态 Registry 与 App-owned Routes

Page 实现由 immutable definition 描述，App 通过私有 route 表显式选择可导航页面：

```c
static const app_manager_page_definition_t s_example_root_definition =
{
    .handler = _example_page_handler,
    .memory_size = sizeof(example_page_state_t),
};

static const app_manager_page_route_t s_example_routes[] =
{
    {
        .page_id = "root",
        .definition = &s_example_root_definition,
        .user_data = NULL,
    },
};

APP_MANAGER_APP_EXPORT(example, 0U, "example", "example", "root",
                       APP_MANAGER_APP_FLAG_NONE, s_example_routes);
```

只有 App descriptor 写入 `.app_manager_apps`；`app_core/linker.lf` 对该段执行
`KEEP()` 并生成首尾符号。definition 和 route 表是普通 `const` 对象，通过 App
descriptor 可达，不存在独立 Page 链接段。宿主测试没有 ESP-IDF linker fragment，
因此内部 fixture 可通过 `app_manager_builtin.h` 中的
`app_manager_register_builtin_descriptors()` 安装固件生命周期内有效的 App 数组；
该函数不是运行时注册 API。

初始化按编译期单页容量一次性校验：

- App ID、Page ID 非空且长度合法；
- App ID 全局不重复，同一 App 内 Page ID 不重复；
- flags 合法，route 数组与数量一致且非空；
- 每个 App 的根 Page 恰好存在于自己的 route 表；
- route 的 definition 非空，且 definition 必须且只能设置 `.handler` 或 `.ops`；
- definition 的 `memory_size` 不超过 `APP_MANAGER_PAGE_STATE_BYTES`。

同一 Page ID 可存在于不同 App；同一 definition 也可由多个 App route 引用。每个 route
拥有独立的 `page_id` 和稳定、非托管 `user_data`，每次页面实例仍获得独立私有内存和
导航栈。公共页面必须由目标 App 显式 opt-in；未绑定 route 时 `(app_id, page_id)` 查询
返回 `ESP_ERR_NOT_FOUND`。所有 descriptor、route 和 definition 生命周期覆盖整个固件
运行期，不提供动态 App、provider、lease、运行时裸 handler 创建或自由挂载接口。

## 5. Typed Blob 与 New Intent

导航参数使用固定 96 字节的版本化值：

```c
typedef struct app_manager_typed_blob
{
    uint16_t version;
    uint16_t type;
    uint16_t size;
    uint16_t reserved;
    uint8_t payload[88];
} app_manager_typed_blob_t;
```

提交命令时 request、ID 字符串和整个 blob 都复制到 command slot；调用返回后，调用方
可立即释放 request 和 ID 存储。异步 completion 的 `context` 不复制，调用方必须保证它
持续有效直到 completion 执行。

有效 blob 必须满足：

- `version == APP_MANAGER_TYPED_BLOB_VERSION`；
- `size <= APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES`；
- `reserved == 0`。

首次创建 App/Page 时，参数在 `ONSTART` 前已经可通过
`app_manager_this_page_arguments()` 读取。重复 RUN 已有 App 或 OPEN 已有 Page 会完整
替换原参数并发送 `ONNEWINTENT`。后台目标的 `ONNEWINTENT` 发生在其
`ONMOUNT/ONRESUME` 之前；若目标正在前台，Screen 仍然可用，但 UI 只能在
`ONMOUNT` 中创建。

## 6. 页面生命周期

一个正常页面实例遵循：

```text
ONSTART -> ONMOUNT -> ONRESUME -> ONPAUSE -> ONUNMOUNT -> ONSTOP
```

`ONNEWINTENT` 是已有实例接收新参数的旁路消息，不替代上述阶段。普通导航中，source
成功 `ONPAUSE` 后，target 会在 presentation start 和目标首帧之前收到 `ONRESUME`。
此时 target 已拥有前台活动资源，但 nav model 尚未提交；动画成功结束、source 清理完成
后才成为稳定前台。若提交前失败，已经 RESUME 的 target 会收到补偿性 `ONPAUSE`，因此
页面必须允许一次 `ONRESUME -> ONPAUSE` 配对发生在从未稳定可见的导航尝试中。

| 消息 | 所有权边界 | 应执行的操作 |
| --- | --- | --- |
| `ONSTART` | Page 实例和私有内存开始存在 | 初始化非视觉状态和长期句柄的无效值 |
| `ONMOUNT` | 框架已创建该 Page 的 Screen | 只在 `app_manager_this_page_screen()` 下创建 UI |
| `ONRESUME` | source 已暂停，Page 即将参与目标首帧；nav 尚未提交 | 启动 timer、订阅、输入和活动会话 |
| `ONPAUSE` | Page 离开前台，或未提交导航正在补偿回滚 | 停止 timer、退订、关闭输入和活动会话 |
| `ONUNMOUNT` | Screen 仍可访问 | 删除页面 UI 并清空所有 LVGL 对象指针 |
| `ONSTOP` | Page 实例即将释放 | 释放剩余非视觉资源；此时没有可用 Screen |
| `ONNEWINTENT` | 已有实例参数已更新 | 读取并消费最新 Typed Blob |

资源放置规则：

- `ONMOUNT/ONUNMOUNT` 只管理 LVGL UI。
- `ONRESUME/ONPAUSE` 只管理前台活动资源。
- `ONSTART/ONSTOP` 管理页面实例级非视觉状态。
- `ONRESUME` 启动的 timer、订阅、输入和活动会话可在动画期间产生副作用；回调只能操作
  target 自己的已挂载 Screen，不得把动画完成或 nav model 提交作为隐含前提。
- `ONRESUME/ONPAUSE` 必须成对可补偿。`ONPAUSE` 清理失败时保留有效 handle 并报告错误，
  不得提前销毁 UI 或假定 source 已恢复。
- 不得用 `lv_screen_active()` 作为页面根节点；它在动画期间并不表示当前 lifecycle callback 的
  所有者。
- lifecycle callback、ops context、`this_page_*` 返回值和 route `user_data` 不提供跨任务
  UI 所有权。
- 清理错误只允许从 `ONPAUSE` 或 `ONSTOP` 报告。

## 7. 独立 Screen 与转换引擎

presentation 初始化一个永久 neutral Screen。Page 在 MOUNT 前由框架创建独立
LVGL Screen，在 UNMOUNT 后删除。稳定前台只有一个 Page Screen；后台页面没有
LVGL 对象。默认 `RGB565` 配置为转场准备两张常驻的全分辨率 RGB565 PSRAM 快照：在
368 x 448 面板上每张 329,728 B、合计 659,456 B（644 KiB）。它们由
`CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION` 控制，不使用 internal/DMA heap。

对有效的非 `NONE` 转场，后端先采集 source 与 target，再将两张 Image 放入 target Screen
的全屏不透明 child overlay 并只动画 Image；target 因而在动画开始后保持 active，而其 live
对象树被 overlay 覆盖。动画完成时先删除 overlay，随后才交付既有 lifecycle completion，
避免 source 清理时仍有 Image 引用快照。快照模式只支持普通 `RGB565`，并依赖 PSRAM；
`RGB565_SWAPPED` 实验配置、显式关闭功能或快照资源/采集/对象/动画失败时，自动回退
`lv_screen_load_anim()` 原生路径。原生调用仍使用 `auto_del=false`，Screen 的最终删除继续由
lifecycle 决定。非空 Page Screen 必须至少有一个覆盖全屏的不透明直接 child，整个对象树也
不得设置 `LV_OBJ_FLAG_OVERFLOW_VISIBLE`；透明根或需要屏外绘制的页面会在该次转场回退
原生动画。

支持 14 种实际效果和一个配置占位值：

| 类别 | 枚举 |
| --- | --- |
| 配置占位 | `DEFAULT` |
| 立即切换 | `NONE` |
| 淡入 | `FADE` |
| 覆盖 | `COVER_LEFT`、`COVER_RIGHT`、`COVER_UP`、`COVER_DOWN` |
| 推动 | `PUSH_LEFT`、`PUSH_RIGHT`、`PUSH_UP`、`PUSH_DOWN` |
| 揭示 | `REVEAL_LEFT`、`REVEAL_RIGHT`、`REVEAL_UP`、`REVEAL_DOWN` |

四类默认转换保存在 `app_manager_config_t`：

| 场景 | 默认值 |
| --- | --- |
| App 前进 | `FADE`, 220 ms |
| App 返回 | `FADE`, 220 ms |
| Page 前进 | `PUSH_LEFT`, 220 ms |
| Page 返回 | `PUSH_RIGHT`, 220 ms |

每个 `app_manager_nav_request_t` 可覆盖 effect 和 duration。`DEFAULT` 使用场景默认
effect；duration 为 0 使用场景默认时长；`NONE` 始终禁用动画并忽略时长。

### 7.1 提交顺序

App/Page 前进：

```text
target START 或 NEW_INTENT
target MOUNT
source PAUSE
target RESUME
presentation start，提交目标首帧并播放 source -> target
source UNMOUNT
提交 nav model
完成命令
```

Page 返回或 App 退出：

```text
target MOUNT（没有目标时使用 neutral）
source PAUSE
target RESUME（没有目标时跳过）
presentation start，提交目标首帧并播放 source -> target/neutral
source UNMOUNT -> STOP
提交 nav model
完成命令
```

target `ONRESUME` 先于 presentation start，因此它的 timer、订阅、输入和活动会话在整段
动画期间已经运行；透明屏障只阻止 LVGL 触摸，不撤销这些资源产生的服务请求、事件回调
或其他外部副作用。target lifecycle callback 此时可以更新自己的 Screen，但不得依据 active/present
查询判断导航已经提交。

presentation 无法启动时，若能立即加载 target，则降级为 `NONE` 并继续执行动画后的清理
和提交；否则框架先向 target 发送补偿性 `ONPAUSE`，再 UNMOUNT target，随后加载并
RESUME source。动画结束后的 source `STOP` 失败也使用相同补偿顺序，最终以原错误完成
命令。target 补偿 PAUSE 失败时不得 UNMOUNT target 或 RESUME source；框架保留 target
的 Screen 和活动资源并提交为前台，以原始错误完成命令。source 根据转换类型保留为
`UNMOUNT_ONLY`、`DESTROY_PAGE` 或 `DESTROY_APP` cleanup owner；后续导航先重试该动作，
避免两个页面同时持有前台会话。`UNMOUNT_ONLY` 不会 STOP 后台驻留 Page；
`DESTROY_APP` 只销毁页面、Screen 和运行节点，静态 descriptor 始终有效。

`BACK_TO` 只在起点和最终目标之间播放一次动画；中间页面按栈顶到栈底执行 STOP/free，
清理失败前已经成功移除的页面属于允许的部分提交。

动画期间 presentation 在 Sys Layer 放置透明触摸屏障。它只阻止 LVGL 触摸，不阻止
HOME、POWER 或 app-control。新导航到达时先把当前动画推进到完成回调和稳定 target，
再执行 FIFO 中的下一条命令。Screen 创建或动画启动失败遵循上述降级或补偿回滚规则，
任何失败都不能绕过 target PAUSE、遗失 cleanup ownership 或形成双 active 会话。

`app_manager_is_transitioning()` 可区分动画临时状态。动画期间 active/present/userdata
等拓扑查询返回与 Screen 暂停相同的保守值，不能把 `false` 或 `NULL` 解释为实例已被
销毁。

### 7.2 Sys Layer 边缘返回

边缘返回属于 app-manager 的系统级输入，不属于任何 App 或 Page。框架在 LVGL
Sys Layer 上永久创建左右两个透明捕获条；捕获条使用 `LV_OBJ_FLAG_CLICKABLE` 和
`LV_OBJ_FLAG_PRESS_LOCK`，保证按下后移出窄边缘区域仍能收到本次指针序列。手势反馈是
独立的 `64x144` A8 Canvas indicator，不接收点击，也不进入 Page 的
MOUNT/UNMOUNT 生命周期。框架不携带静态图片，也不依赖 LVGL Vector 或 ThorVG；弧形
alpha mask 在初始化时以 Q15 整数运算程序化生成。indicator 从本次手势所属的屏幕外侧
水平移入；两侧图标文本都使用 `LV_SYMBOL_LEFT`，不改用 `LV_SYMBOL_RIGHT`。

一次有效手势必须同时满足：

- 全局边缘返回开关已启用；
- Screen 处于稳定可交互状态，没有页面动画、Screen suspend 或 deinit；
- 当前页面存在合法返回目标；
- 指针从左侧或右侧捕获条开始，并朝屏幕内部移动到提交阈值。

0.6.0 的几何参数按显示分辨率计算并限制上下界：

| 参数 | 计算规则 |
| --- | --- |
| 边缘捕获宽度 | 屏宽 8%，限制为 24..32 px；368 px 屏宽下为 29 px |
| 提交距离 | 屏宽 15%，限制为 48..80 px；368 px 屏宽下为 55 px |
| 方向判定 slop | 屏宽 3%，限制为 10..16 px |
| armed 回退滞回 | 提交距离的 1/5，限制为 10..18 px |
| Canvas 尺寸 | 固定 64x144 px，手势期间不缩放、不变形 |
| indicator Y 锚点 | 按下时以触点居中并限制 8 px 垂直边距，本次手势内保持固定 |
| indicator 样式 | 固定 `#202124`、不透明度 220；仅轮廓边缘使用 1 px alpha 抗锯齿 |

Canvas 使用一块 9,220 bytes（约 9.0 KiB）的固定内部 BSS A8 draw buffer，不在手势期间分配内存。
弧形按行计算：先按 `u = abs(2*y-(H-1))/(H-1)` 将行坐标归一化到 Q15，再计算
`smooth = u * u * (3 - 2 * u)` 和 `depth = 62 - (62 - 28) * smooth`，把有效深度
从中部 62 px 平滑收窄到上下端 28 px。
左侧填充 `[0, depth)`，右侧填充 `[64-depth, 64)`；左右手势共用同一缓冲区，仅在捕获
侧发生变化时重新生成镜像轮廓并 invalidate Canvas。

向内位移达到 slop，且垂直位移不大于向内位移的两倍时锁定水平方向。尚未锁定时，只有
垂直位移达到两个 slop，且大于两倍的非负向内位移时才抑制本次序列；位于两者之间的
斜向移动保持未决并继续观察，避免轻微垂直偏移导致误取消。明显向外移动超过一个 slop
时仍立即取消。向内位移达到提交距离后进入 armed；回撤到“提交距离减滞回值”以下才
解除 armed，避免阈值附近抖动反复切换。

Home 始终没有手势返回入口，即使全局开关已启用。其他 App 的子页返回上一 Page；
非 Home 根页只有在 App 历史中存在前一个 App 时才能手势返回。没有目标时捕获条保持
无效，边缘输入继续由正常 LVGL hit-test 处理。

`PRESSING` 期间框架只按 `inward / trigger_distance` 更新 Canvas 的水平位置。左侧 X 从
`-64` 移到 `0`，右侧 X 从屏宽移到“屏宽减 64”；Canvas 尺寸、轮廓、不透明度和 Y
锚点全程不变。`LV_SYMBOL_LEFT` Label 是 Canvas 子对象，仅在进入 ARMED 后显示，回撤
解除 ARMED 后立即隐藏。Page Screen 的坐标、透明度、样式和 active 状态不随手指实时
变化，也不创建全屏快照。`PRESSING` 阶段绝不采集快照；释放前未达到阈值、方向错误、输入
取消或状态被抢占时，只复位
indicator，不产生导航命令；达到阈值后提交一条既有异步 `BACK`，并由 Page/App 返回
场景的默认动画完成实际 Screen 切换。

按下时手势控制器会取得 navigation epoch token，并在移动和释放时持续校验。若期间已有
其他导航获得准入，token 立即失效，本次手势只隐藏 indicator，不再提交过期 `BACK`；
释放时的条件提交还会再次核对 epoch 和当前 source App。成功提交后命令仍进入统一的
24 槽 FIFO，不建立第二套导航队列。

新导航开始、Screen suspend 或 deinit 会取消尚未提交的指针序列并隐藏 indicator；
Screen resume 或导航稳定结束后重新计算当前页面是否允许捕获。已经成功准入 navigation
gateway 的手势 `BACK` 不会因随后关闭开关而撤销，仍按普通异步导航的 exactly-once
完成语义达到稳定状态。

运行时开关是公共 API，不增加 `app_manager_config_t` 字段：

```c
esp_err_t app_manager_back_gesture_set_enabled(bool enabled);
bool app_manager_back_gesture_is_enabled(void);
```

初始化成功后全局开关默认为 `true`。
`app_manager_back_gesture_set_enabled(false)` 会阻止新手势并复位尚未提交的 indicator；
`app_manager_back_gesture_is_enabled()` 只报告这个全局策略值，不表示当前页面一定存在返回
目标。Home、无 App 历史、Screen 暂停或动画期间，策略值仍可能为 `true`，但有效捕获为
`false`；初始化前和 deinit 后 getter 返回 `false`。setter 在策略已应用或已保存供下次
Screen resume 使用时返回 `ESP_OK`，否则返回准入或 UI dispatcher 错误。业务页面不得
直接访问或重排 Sys Layer 捕获对象。

## 8. 统一导航 Gateway

所有导航使用 `app_manager_nav_request_t`：

| operation | 语义 |
| --- | --- |
| `RUN` | 创建 App，或把已有 App 提到前台 |
| `EXIT` | 退出指定 App |
| `EXIT_SELF` | 退出发起生命周期回调所属的 App |
| `OPEN_PAGE` | 打开静态 Page；必要时启动或前置目标 App |
| `BACK` | 返回一页；根页返回等价于退出当前 App |
| `BACK_TO` | 返回到当前 App 栈中已有的指定 Page |
| `REMOVE_PAGE` | 删除指定 Page；删除当前页按 BACK 处理 |
| `CLEANUP_BACKGROUND` | 重试清理所有非 pinned 后台 App |

`EXIT_SELF` 只在 lifecycle callback 内接受，request 的 `app_id` 必须为 `NULL`；gateway
会在 callback 返回前复制 owner ID。普通 task 或一般 LVGL event 应使用 `EXIT`、`BACK`
或其他显式导航操作。

`OPEN_PAGE` 只接受 registry 中的静态 Page ID：

- App 不存在时创建 App、根页以及目标页；
- App 在后台时把它提到前台；
- Page 已存在时更新 intent，并执行一次 back-to；
- Page 不存在时创建并压入目标 App 的页面栈。

### 8.1 同步 API

```c
app_manager_nav_request_t request =
{
    .operation = APP_MANAGER_NAV_OP_RUN,
    .app_id = "settings",
    .transition = {.effect = APP_MANAGER_TRANSITION_DEFAULT},
};

esp_err_t result = app_manager_navigate(&request, UINT32_MAX);
```

`app_manager_navigate()` 只能由非 UI task 调用。它等待动画、生命周期和必要清理达到
稳定结果；在 UI worker 上调用返回 `ESP_ERR_INVALID_STATE`。

同步超时沿用 mailbox 安全语义：

- 命令尚未开始时取消，回调不会执行；
- 命令已经开始时继续等待稳定完成，但最终向调用者返回 `ESP_ERR_TIMEOUT`。

因此同步 API 返回后，栈上 request 可以安全销毁，即使实际等待超过指定 timeout。

### 8.2 异步 API

```c
static void _navigation_complete(esp_err_t result, void *context)
{
    /* 在 UI worker 上运行。 */
}

app_manager_nav_request_t request =
{
    .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
    .app_id = "settings",
    .page_id = "power",
};

esp_err_t admitted = app_manager_navigate_async(
                         &request, _navigation_complete, context);
```

`app_manager_navigate_async()` 可从普通 task、LVGL event、navigation completion 或
lifecycle callback 调用。返回值只表示 command slot 和 mailbox 准入；准入成功后的最终
结果由 UI worker 上的 completion 给出。生命周期回调内的重入请求进入同一 FIFO，不会
递归修改当前 nav model。

固定 command pool 有 24 槽。池满或 mailbox 拒绝时立即返回 `ESP_ERR_NO_MEM`，此时
completion 不会执行。

## 9. 清理失败与失败回滚

raw handler 是 `void`。`ONPAUSE` 或 `ONSTOP` 释放资源失败时必须报告：

```c
esp_err_t result = event_bus_unsubscribe(state->subscription);
if (result != ESP_OK)
{
    app_manager_this_page_report_cleanup_error(result);
    return; /* 保留有效 handle，后续重试。 */
}
state->subscription = EVENT_BUS_SUB_HANDLE_INVALID;
```

typed ops 的 `pause` 和 `stop` 直接返回 `esp_err_t`；未提供的回调视为成功的 no-op，
不得再调用 `app_manager_this_page_report_cleanup_error()` 重复报告。

框架只记录当前回调报告的第一个非 `ESP_OK` 错误：

- PAUSE 失败时页面仍保持 active，不继续 UNMOUNT/STOP；
- STOP 失败时实例、私有内存和未释放句柄继续归原 owner；
- 未提交 target 已经 RESUME 时，回滚必须先执行 target PAUSE，再 UNMOUNT target 和
  RESUME source；应用应把它视为正常的补偿性生命周期配对；
- target 的补偿 PAUSE 失败时保留 target 的 Screen、活动资源和 recovery ownership，
  不得继续 UNMOUNT target 或 RESUME source；
- 返回/退出的 STOP 失败会在 target 补偿成功后恢复 source Page，而不是提交半个前台；
- 后台清理、驻留淘汰和 deinit 可再次执行同一清理；
- 释放失败后不得提前清空 handle，否则框架无法替业务代码恢复所有权证据。

`ONMOUNT/ONUNMOUNT` 不提供 cleanup error 通道。LVGL Screen 创建、删除或动画启动失败
由 presentation/lifecycle 自身回滚。任何失败路径都不得丢失 Page slot、Screen
或 cleanup ownership。

## 10. Screen Suspend、Resume 与 Deinit

### 10.1 双击 HOME 与系统任务切换器

HOME 单击与双击由 app-control 在 350 ms 窗口内消歧：窗口内的第二次点击提交
`APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW` 系统命令；窗口到期且没有第二次点击时
提交 `OPEN_PAGE(home, root)`。两种动作都经统一 navigation gateway 异步提交，且只有
worker 状态机决出唯一动作后才准入，因此导航 FIFO 中不会出现可被取消的先行命令。
切换器已打开时再次双击会将其关闭并回到原前台任务。显示休眠或 standby 唤醒开始的
HOME 序列只负责唤醒，不参与消歧；长按、POWER 关屏、standby 准备和 deinit 都取消
待定窗口。worker 按窗口剩余时间缩短轮询等待，单击延迟不超过 350 ms 加一个轮询周期。

最近任务是 App Manager 的系统级界面（`app_manager_task_switcher.c`），不属于任何
App 或 Page：它在 LVGL Sys Layer 创建全屏表面，原前台 App 保持运行且不收到
PAUSE/UNMOUNT，触摸由切换器完全接管。打开时经
`app_manager_lifecycle_recent_tasks_call()` 复制 MRU 任务列表（含当前任务与
`cleanup_pending` 状态）并构建紧凑横向卡片；每张卡片显示应用图标和名称，不保留屏幕
像素缓存。点击当前任务
卡片或空白背景关闭切换器并回原任务；点击后台任务卡片通过普通 RUN 恢复（导航
gateway 在执行前自动隐藏切换器）；上滑卡片经 `EXIT` 关闭该任务并原地刷新列表；
横向滑动不跟手：卡片行永久禁用原生指针滚动（无 SCROLLABLE/scroll chain/snap），
拖动期间只累计位移，松手时若最终水平位移达到 40 px 且水平主导则按方向翻一张
（页距为卡片宽 230 px 加间距 12 px = 242 px）；行按主轴起始对齐并在两侧留
半屏边距，使每个页目标恰好可达、不会在分页中途被 LVGL 有界化钳制。未达阈值时
位置不变，因此任何短上滑都绝不会产生残留位移或 LVGL throw/snap 接管。手势分类
只看松手时相对按下的最终位移：垂直主导且 `dy <= -40`、`|dx| <= 60` 关闭；水平
主导且 `|dx| >= 40` 分页；任一轴最大位移（含仅出现在释放样本中的位移）达到
10 px 即抑制本次 CLICKED，背景区域拖动同样抑制。列表重建（EXIT 完成、淘汰、
清除失败重试、重新打开）一律回到第一张 MRU 卡片；分页动画进行中再次按下会先
稳定到已记录目标页。
"清除"经复合系统命令 `CLEAR_ALL` 先切换 Home，再在同一个 gateway 命令内销毁全部
非 pinned/hidden 任务（含打开前的前台任务），完成后关闭切换器。普通导航在执行前
同步隐藏切换器；切换器自己发起的 `EXIT` 通过命令级 `keep_switcher_alive` 标记保持
可见并显示 busy 外壳，完成或失败后原地重建。screen suspend 先取消指针序列并销毁切换器对象，再
进入页面 PAUSE；resume 后保持关闭。deinit 删除切换器和 presentation Sys Layer 对象。

最近任务数据由 `app_manager_get_recent_tasks()` 提供：按 MRU 顺序返回最多
`APP_MANAGER_MAX_RECENT_TASKS` 个非 pinned、非 hidden 的驻留 App，每项包含静态
descriptor（含 `display_name` 和 `icon_id`）、保留的当前 Page ID、`active` 与
`cleanup_pending` 状态。App 销毁（EXIT、淘汰、回滚清理、deinit）时由生命周期统一
释放实例；图标缺失时切换器使用符号回退。
清理失败的任务保留在列表中并标记 `cleanup_pending`，允许再次重试；处于该状态的
实例从公共拓扑查询（`app_manager_is_page_present` 等）保守隐藏，但保留驻留计数、
保留在 recent-tasks 列表中，下一次导航先重试其清理再继续。

Screen suspend 不播放动画：

```text
快进当前动画并提交稳定 target
current PAUSE
load neutral
current UNMOUNT，删除 Page Screen
保存运行 App 数快照
关闭 Screen 访问准入并排空在途访问
pause LVGL adapter
等待 panel IO / DMA fence
提交硬件 suspend
```

Screen resume 同样不播放动画：

```text
硬件 resume_prepare（面板仍隐藏）
resume LVGL adapter
current MOUNT
load current Screen
current RESUME
刷新并等待 fence
恢复 indev、亮度和 resume_commit
```

PAUSING、PAUSED 和动画期间的拓扑查询是保守值。成功进入 PAUSED 后，
`app_manager_get_running_apps()` 和 `app_manager_is_all_closed()` 使用暂停时保存的
running-count 快照；其余 active/present 查询返回 `false` 或 `NULL`。

`app_manager_deinit()` 不能从 UI worker 调用。正常顺序为：

1. app-control 停止生产者，注销输入并取消 standby；
2. navigation 停止准入，排空命令并快进最后动画；
3. lifecycle 置 CLOSED，等待已准入访问退出；
4. UI worker 执行 PAUSE/UNMOUNT/STOP、删除 Screen 并释放运行节点；
5. presentation 删除 neutral Screen、屏障和 completion timer；
6. 关闭 mailbox，pause/fence/deinit LVGL adapter；
7. 释放字体、主题和资源映射。

任一步清理失败都保留对应 ready/ownership 状态；再次调用 `app_manager_deinit()` 继续
重试。冷启动、Screen resume 和 deinit 不播放页面动画。

## 11. 显示与 PM 协作

app-control task 串行管理显示和 standby：

```text
ACTIVE -> DIM -> SCREEN_OFF -> STANDBY_PREPARING -> STANDBY
   ^        |          |                 |              |
   +--------+----------+-----------------+--------------+
                    user activity / wake
```

POWER、HOME、touch 和 system wake 通过 generation/pending 状态合并。standby guard 在
系统提交点重新检查本地 epoch、显示规范休眠状态和 wake intent；commit callback 只做
无阻塞原子记录，最终状态仍由 app-control 核对后发布。
显示已休眠时，由物理按键开始的唤醒序列只恢复显示，其尾随 `UP/CLICK` 不再重放为
导航或关屏操作；下一次在活动显示上开始的独立按键序列恢复正常语义。

亮度寄存器和 LVGL color flush 共用 panel IO。所有正常亮度、临时亮度、DIM 和恢复亮度
都同步派发到 UI worker，不能从 app-control 或启动 task 直接调用 BSP panel 操作。冷启动
的 input/PM admission 在 `app_manager_init()` 后仍保持关闭；`app_manager_startup_commit()`
注册物理输入并从首屏真正可操作的时刻重置 inactivity epoch。

显示恢复遵循 `resume_prepare -> adapter resume -> Page MOUNT/RESUME -> refresh/fence ->
resume_commit`。面板显示和物理触摸只在完整恢复帧提交后启用，避免旧帧、触摸穿透和
DMA 未完成。

非 TE 显示路径的三个容量参数承担不同职责：App Manager 使用 60 行 PSRAM 双绘制缓冲，
BSP 将每次 SPI DMA 传输限制为 10 行（7,360 B），并将传输队列深度限制为 2；因此 LVGL
可以按较大条带渲染，同时内部 DMA bounce buffer 峰值限制为 14,720 B。该路径不启用
PSRAM 直 DMA，也不依赖周期性 SPI fence。

`CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS` 仅用于开发验收。启用后，低优先级 PSRAM 栈
任务周期采样 adapter FPS、DMA/internal heap 和 PSRAM；LVGL display event 同时记录
render、flush、flush-wait，panel 代理记录提交次数、像素、失败和同步调用耗时。
presentation 只在 `lv_anim_get()` 确认真正启动后记录动画边界，并按具体 effect 汇总
active duration、final-flush interval 的 P50/P95/P99/max、33,333 us 达标数以及连续超过
41,667 us 的最大长度。adapter FPS 只作为观察信息；benchmark 的 `passed` 仅表示采样、
DMA、panel 提交和动画统计完整性通过，具体帧率门槛由上板验收逻辑判定。停止诊断时先
注销 display event，再确认任务退出并释放 PSRAM 栈，之后才允许销毁 LVGL adapter。
characterization 的第一阶段固定为 display-only，第二阶段的 full、audio-only 或
tcp-only 由根 benchmark 运行时 profile 选择；STRESS 使用 profile 中的 load。campaign
模式、时长、IPv4 host、port 和 rate 不属于 App Manager Kconfig，由
`tests/display/profiles/*.json` 生成的类型化配置传给 `display_benchmark_start()`。
App Manager Kconfig 只保留 diagnostics 和显示静态资源/功能 gate。

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` 默认关闭。仅在调试生命周期顺序时启用；它统一
输出 `START/MOUNT/RESUME/PAUSE/UNMOUNT/STOP/NEW_INTENT` 的 INFO trace。关闭时编译器会
裁剪事件名表和日志调用，不影响 lifecycle callback、observer 或状态迁移。

## 12. 应用开发契约

本节描述 `layers/apps` 中内置 App/Page 的现行契约。公共类型和容量以
`app_core/include/app_manager_types.h` 和 `app_core/include/app_manager.h` 为准，应用不
复制容量、队列或版本常量。

### 12.1 Page 模型与定义

内置 Page 按实际资源所有权组合以下模型：

| Page 模型 | 生命周期阶段 | 所有资源 |
| --- | --- | --- |
| 静态 | MOUNT、UNMOUNT | LVGL 对象 |
| 刷新 | MOUNT、RESUME、PAUSE、UNMOUNT | timer、UI subscription |
| 会话 | START 到 STOP | retained state、活动服务会话 |
| Worker | RESUME、PAUSE | 页面 worker、线程安全 snapshot |
| 参数 | START 或 NEWINTENT，以及所需 UI 阶段 | Typed Blob、route data |

Page definition 必须且只能设置 `.ops` 或 `.handler`。类型化 ops 用于普通生命周期阶段；
raw handler 保留直接接收 `app_manager_msg_type_t` 和 route `user_data` 的兼容能力。未提供
的 ops 回调是成功的 no-op。

Page state 只保存实例私有状态，其 `memory_size` 不超过
`APP_MANAGER_PAGE_STATE_BYTES`；大 payload 独立分配，不放入 Page state，也不通过跨实例
全局状态保存。immutable Page definition 由所属 App 的静态 route 绑定，同一 definition
可由多个 App 显式引用，但每个实例拥有独立 state 和导航栈。运行时不存在脱离 route 的
裸 Page。

### 12.2 Context 与生命周期所有权

`app_manager_page_context_t` 仅在当前 ops 回调期间有效：

- `state` 指向框架清零的 Page 私有内存，持续到成功 STOP；START 只需设置无效 handle 和
  其他非零初值。
- `route_user_data` 来自 immutable route，其存储覆盖整个固件运行期。
- `screen` 在 MOUNT、RESUME、PAUSE、UNMOUNT 中有效；START、STOP 中为 `NULL`；
  NEWINTENT 仅在实例已挂载时携带 Screen。
- `arguments` 指向当前不可变 Typed Blob，只能在回调期间读取；回调结束后仍需使用的
  payload 由 Page 校验并复制。

正常生命周期为：

```text
START -> MOUNT -> RESUME -> PAUSE -> UNMOUNT -> STOP
```

已有实例接收新参数时旁路调用 NEWINTENT。导航提交前失败时，target 可能收到补偿性的
`RESUME -> PAUSE`，即使它从未成为稳定前台。

| 阶段 | 当前所有权边界 |
| --- | --- |
| START | 初始化实例级非视觉状态和无效 handle |
| MOUNT | 在 `context->screen` 下创建完整 UI |
| RESUME | 启动 timer、订阅、输入、worker 和活动会话 |
| PAUSE | 逆序停止活动资源；失败时保留有效 handle 并返回错误 |
| UNMOUNT | 删除 UI 并清空全部 LVGL 指针 |
| STOP | 释放 START 资源，不重复执行 PAUSE |
| NEWINTENT | 校验并复制最新 arguments，按当前挂载状态更新 UI |

ops 的 PAUSE/STOP 直接返回 `esp_err_t`；raw handler 的 PAUSE/STOP 通过
`app_manager_this_page_report_cleanup_error()` 报告第一个释放错误，两种通道不混用。
清理失败时，实例、资源和有效 handle 仍归原 owner，后续生命周期请求可重试。PAUSE 或
STOP 成功后才清空对应 handle。

### 12.3 导航与参数

- 普通 RUN、OPEN_PAGE 和 BACK 由 `app_ui_request_*` 发起。
- 自定义转场、Typed Blob 或 completion 使用 `app_manager_navigate_async()`。
- LVGL event callback 使用异步导航；UI worker 不调用同步
  `app_manager_navigate()`。
- request、ID 和 Typed Blob 在准入时按值复制；completion context 不复制。
- 参数 Page 使用应用私有 type 和 payload，并校验 blob version、type 和精确 size，再以
  `memcpy` 解码。
- 目标 Page 必须存在于目标 App 的静态 route 表。

Weather 的 root、forecast、alerts 和 alert-detail 使用类型化 ops；alert-detail 通过
Typed Blob 接收预警标识并处理 NEWINTENT：已挂载实例立即刷新，后台实例保留复制后的参数
并在 RESUME 时刷新。Weather 同时是订阅 handle、immutable snapshot 和 PAUSE 所有权的
现有参考。主页、应用目录、设置、网络设置、时钟、录音、水平仪和诊断页面均采用同一 typed ops 约定。

### 12.4 并发与资源边界

- UI worker 不执行文件、音频、网络解析或硬件阻塞操作。
- 页面 worker 只通过线程安全 snapshot 或 UI dispatcher 更新 UI，并在 PAUSE 返回前同步
  停止。
- UI subscription 不保证 publisher 已停止；退订失败时保留 handle。
- LVGL 指针只在 mounted 生命周期解引用，动画回调只修改本 Page 的 Screen。
- 应用不创建第二个 LVGL task、tick、Screen manager，也不绕过 App Manager 获取显示
  所有权。

### 12.5 验证边界

App Manager 宿主测试覆盖 registry、lifecycle、ops/raw 互斥、context、参数以及 PAUSE/
STOP 失败重试。应用和跨层宿主测试覆盖真实 Page 的 Screen、object、timer、订阅、worker
和 snapshot 基线。相关命令与 sanitizer 配置见第 14 节。

宿主测试不证明真机显示、触摸、DMA、射频、功耗或性能；这些结果只由对应的上板测试和
资源观测确认。

## 13. 初始化配置

`app_manager_config_t` 中与 0.6.0 导航直接相关的字段：

| 字段 | 含义 |
| --- | --- |
| `max_resident_apps` | 驻留上限；0 使用物理上限 8 |
| `resident_policy` | 满容量拒绝或淘汰最旧后台 App |
| `app_forward_transition` | App 前进默认动画 |
| `app_back_transition` | App 返回默认动画 |
| `page_forward_transition` | Page 前进默认动画 |
| `page_back_transition` | Page 返回默认动画 |

其余必需字段提供 display/touch、字体和资源盘符，以及 `screen_ops`、`input_ops`、
`standby_ops`。配置结构由初始化路径读取，硬件操作表会复制到内部 owner。边缘返回不增加
配置字段；初始化后默认启用，需要运行时关闭时调用公共开关 API。

推荐启动顺序：

```text
BSP / system PM ready
app_manager_init（input/PM admission closed）
register UI dispatcher with event bus
start power service
navigate RUN HOME
app_manager_display_commit_initial（show panel, keep indev closed）
start connectivity services
app_manager_startup_commit（open touch, key, and PM admission）
```

`main/app_runtime.c` 先显示 Home，再初始化连接服务，因此 Wi-Fi/BLE 启动耗时不会延长
黑屏；但物理 HOME/POWER 在连接层完成前仍不会进入导航或 PM 状态机。任何 Page 的
`ONRESUME` 都发生在 UI dispatcher 和 power service 安装之后。

## 14. 测试与验证

app-core 宿主套件：

```sh
cmake -S layers/app_manager/app_core/tests/host \
      -B /tmp/mt-app-core -G Ninja
cmake --build /tmp/mt-app-core
ctest --test-dir /tmp/mt-app-core --output-on-failure
```

该套件包括 `mailbox`、`navigation`、`registry`、`nav_model`、`lifecycle`、`control`
和 `adapter`。其中 navigation/lifecycle 测试覆盖边缘返回的 epoch 准入、条件 BACK、
并发 FIFO 和稳定 back-target；Sys Layer 对象、几何识别及跨层资源清理由下述
cross-layer 套件覆盖。ASan/UBSan 与 TSan 分别使用：

```sh
cmake -S layers/app_manager/app_core/tests/host \
      -B /tmp/mt-app-core-asan -G Ninja \
      -DAPP_MANAGER_SANITIZER=address
cmake --build /tmp/mt-app-core-asan
ctest --test-dir /tmp/mt-app-core-asan --output-on-failure

cmake -S layers/app_manager/app_core/tests/host \
      -B /tmp/mt-app-core-tsan -G Ninja \
      -DAPP_MANAGER_SANITIZER=thread
cmake --build /tmp/mt-app-core-tsan
ctest --test-dir /tmp/mt-app-core-tsan --output-on-failure
```

应用、main 和跨层套件：

```sh
cmake -S layers/apps/tests/host -B /tmp/mt-apps -G Ninja
cmake --build /tmp/mt-apps
ctest --test-dir /tmp/mt-apps --output-on-failure

cmake -S main/tests/host -B /tmp/mt-main -G Ninja
cmake --build /tmp/mt-main
ctest --test-dir /tmp/mt-main --output-on-failure

cmake -S tests/integration -B /tmp/mt-integration -G Ninja
cmake --build /tmp/mt-integration
ctest --test-dir /tmp/mt-integration --output-on-failure
```

对应 sanitizer cache 变量为 `APPS_SANITIZER`、`MAIN_HOST_SANITIZER` 和
`CROSS_LAYER_SANITIZER`，取值同样是 `address` 或 `thread`。跨层 `presentation` 测试
覆盖 14 种效果、快照起始/中间/完成、原生回退、立即完成、触摸屏障和快进；`cross_layer` 覆盖实际应用的
App/Page 前进返回、Screen pause/resume、清理回滚和 Sys Layer 边缘手势。手势断言除
导航结果外，还应检查稳定时一个 Page Screen、普通非 `NONE` 转场的 overlay/Image 在完成后
释放、`PRESSING` 期间不创建快照、Page Screen 不随 `PRESSING` 改变、Canvas 固定为
`64x144`、indicator Y 锚点固定、左右弧形互为镜像，
以及 indicator、timer、订阅和活动会话在取消或完成后回到基线。A8 buffer 测试还应采样
顶部、四分之一、中部和底部的 depth 与边缘 alpha，验证 Q15 曲线和 1 px 抗锯齿；
368 px 屏宽下检查捕获条为 29 px、提交距离为 55 px，并分别检查左侧 X 为
`-64/-31/0`、右侧 X 为 `368/335/304`（中间值对应 inward=28）；左侧 `x=28` 命中而
`x=29` 不命中，右侧
`x=339` 命中而 `x=338` 不命中，提交边界应覆盖向内位移 `54/55`。同时确认轻微斜滑
能够锁定水平，明显垂直或向外移动被取消，以及箭头在阈值前隐藏、ARMED 后显示、迟滞
回撤后再次隐藏。

提交前还应执行：

```sh
git diff --check
idf.py set-target esp32s3
idf.py build
idf.py size
```

C/C++ 文件按 `doc/code-style.md` 运行 AStyle。显示和生命周期变更仍需上板循环验证：

- App/Page 双向切换至少 100 次，无空白帧和触摸穿透；
- 左右边缘分别执行至少 100 次提交和取消，Home、无目标、休眠和动画抢占均不误导航；
- 拖动时 Sys Layer Canvas 仅从屏幕外水平平移，固定尺寸、轮廓、不透明度和 Y 锚点；
  箭头仅在 ARMED 时可见，Page Screen 在释放提交前保持不动；
- 动画平均帧率不低于 25 FPS；
- Screen、LVGL object、timer、订阅和活动会话无增长；
- 无 panel IO、DMA 或 fence 错误；
- 记录内部 RAM/PSRAM 低水位及 ROM/RAM 增量，新增内部 BSS 不超过 10 KiB。

## 15. 明确边界

0.6.0 不包含：

- 动态 App、provider 插件 ABI、运行时 Page 挂载、裸 handler 创建或脚本应用；
- Page Screen 实时跟手返回或自定义 ZOOM；边缘拖动只更新 Sys Layer indicator，释放提交后
  才按普通 Page/App 返回动画决定是否创建全分辨率快照；
- 静态 indicator 图片、LVGL Vector 或 ThorVG；弧形只由固定 A8 Canvas 程序化生成；
- popup/overlay manager；系统任务切换器是 App Manager 自有的 Sys Layer 界面，
  不属于业务 overlay 框架，业务 App 不得创建或复用该表面；
- 多个 app-manager 实例；
- 对 ESP-IDF、LVGL 或 `managed_components/` 的本地修改。

## 16. 推荐阅读顺序

1. `app_core/include/app_manager_types.h`
2. `app_core/include/app_manager.h`
3. `app_core/src/app_manager_navigation.c`
4. `app_core/src/app_manager_nav_model.c`
5. `app_core/src/app_manager_lifecycle.c`
6. `app_core/src/app_manager_presentation.c`
7. `app_core/src/app_manager_back_gesture.c`
8. `app_core/src/app_manager_builtin.c`
9. `app_core/src/app_manager_lvgl_adapter.c`
10. `app_core/tests/host/`
11. `tests/integration/test_cross_layer.c`

## 17. License

本模块采用 [MIT License](LICENSE)。
