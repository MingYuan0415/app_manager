**English** | [简体中文](README_ZH.md)

# MicroTech App Manager 0.6.0

`layers/app_manager` provides the application runtime on the ESP32-S3, static
App registration, App/Page navigation, LVGL Screen transitions, display sleep,
input routing and app-level PM. Business apps live in `layers/apps/`; the system
assembly entry point is `main/app_runtime.c`.

The core constraints of 0.6.0 are: the navigation model and all LVGL objects are
modified only by the UI worker; apps and Pages use static App descriptors and
App-private routes; every mounted Page owns an independent Screen; all
synchronous and asynchronous navigation goes through the same fixed-capacity
command gateway. Edge back presents gesture feedback only on the permanent Sys
Layer; Page Screens always switch through the existing navigation animation and
never track the finger in real time.

## 1. Design Invariants

The following constraints are public behavior and should not be changed in a
local refactor:

1. The UI worker is the only writer of the nav model, page lifecycle and LVGL
   objects.
2. At most 8 Apps physically reside and 12 Pages are retained; page-private
   memory comes from a fixed arena.
3. A stable foreground mounts exactly one Page Screen; during animation the
   source and target are temporarily retained together.
4. Background Apps retain navigation and private state, but no Screen, timer,
   subscription, input or active session.
5. When `ONPAUSE` or `ONSTOP` cleanup fails, the instance, handles and cleanup
   ownership are retained so the cleanup can be retried.
6. App descriptors, Page routes and Page definitions are immutable objects for
   the whole firmware lifetime.
7. A navigation command completes only after animation, lifecycle and required
   cleanup have finished.
8. Screen sleep and deinit first fast-forward the current animation, then enter
   a stable page state and close new admission.
9. PM generation/commit, mailbox timeout and display DMA fence semantics remain
   independently valid.
10. During cold start, physical keys and inactivity PM stay closed; they open
    only after the first frame, the panel and the dependent services have
    committed.
11. The edge back capture strips and indicator belong only to the Sys Layer and
    never enter the Page Screen lifecycle.
12. Gesture PRESSING only updates the indicator; the Page Screen position,
    opacity and style never track the finger in real time.

## 2. Module Responsibilities

| Path | Responsibility |
| --- | --- |
| `app_core/include/app_manager_types.h` | descriptor, Typed Blob, navigation, animation, lifecycle and hardware port types |
| `app_core/include/app_manager.h` | App, navigation, query, UI dispatcher, display and PM public API |
| `app_core/include/app_manager_config.h` | BSP, resource, residence policy and default animation configuration used by the assembly layer |
| `app_core/src/app_manager_builtin.c` | static App registry, route validation and queries |
| `app_core/src/app_manager_nav_model.c` | App/Page fixed slots, doubly linked lists, arena and model invariants |
| `app_core/src/app_manager_navigation.c` | fixed-capacity sync/async command gateway and timeout semantics (product baseline 8 slots) |
| `app_core/src/app_manager_lifecycle.c` | lifecycle execution, residence policy, failure rollback and cleanup ownership |
| `app_core/src/app_manager_presentation.c` | neutral Screen, Page Screen, full-resolution snapshot/Image transition, native fallback and touch barrier |
| `app_core/src/app_manager_back_gesture.c` | Sys Layer dual-side capture, programmatic A8 Canvas indicator, back-target gating and runtime switch |
| `app_core/src/app_manager_mailbox.c` | fixed-capacity dispatcher from any task to the UI worker |
| `app_core/src/app_manager_lvgl_adapter.c` | LVGL, display, touch, font, resource and suspend/resume transactions |
| `app_core/src/app_manager_task_switcher.c` | Sys Layer system task switcher: horizontal card carousel, per-card close (button or swipe up), clear all, device memory overview and lifecycle cooperation |
| `app_core/src/app_manager_pm*.c` | app-control, input merging, display PM and system standby cooperation |
| `app_core/linker.lf` | retains the single App descriptor link section |
| `app_core/tests/host/` | model, registry, gateway, lifecycle, mailbox, PM and adapter tests |
| `app_theme/` | seven semantic font roles and AMOLED color constants |

The main dependencies are:

```text
ordinary task / UI callback / lifecycle callback / Sys Layer gesture
                    |
                    v
        navigation command gateway (8)
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

`app-control` is an independent serial domain for display, brightness, input and
standby policy. It requests UI pause/resume through the mailbox and never
modifies the nav model or page objects directly.

## 3. Fixed Capacity and Runtime Model

| Resource | Limit | Behavior when full |
| --- | ---: | --- |
| Physical App slots | 8 | Reject or evict according to the residence policy |
| Physical Page slots | 12 | `ESP_ERR_NO_MEM` |
| Navigation command slots | `CONFIG_APP_MANAGER_NAV_COMMAND_CAPACITY` (product baseline 8, Kconfig default 24) | `ESP_ERR_NO_MEM` |
| App/Page ID | at most 31 bytes plus `\0` | `ESP_ERR_INVALID_ARG` |
| Typed Blob | 96 bytes, of which 88 bytes are payload | `ESP_ERR_INVALID_ARG` |
| Animation duration | 1..2000 ms, 0 means use the default | `ESP_ERR_INVALID_ARG` |

Read-only images generated by the build are aggregated by the Apps/App Theme
manifests; the assembly layer uses `app_manager_image_resource_config_t` to map
product semantic IDs to mmap asset indexes. The App descriptor binds the app
icon with `uint32_t icon_id`, the recent-task snapshot copies that ID, and the
task switcher draws a 56 px image inside a 64 px circular icon base, falling
back to an LVGL symbol when the resource is missing or the ID is zero. At init,
App Manager creates a small descriptor table in PSRAM and validates the LVGL 9
BIN magic, `RGB565A8`, dimensions, even stride, independent A8 plane length and
duplicate semantic IDs; the pixel payload always points directly at the mmap
Flash address of the `res` partition. The descriptor returned by
`app_manager_get_image()` lives until `app_manager_deinit()`, so all LVGL Image
objects must be destroyed first by the normal page lifecycle.

Each App node owns an independent Page stack; the App nodes themselves form the
foreground history:

```text
App history:  home <-> settings <-> setup
                                      ^ active_app

settings:     root <-> power <-> about
                                  ^ current_page
```

The page arena is split into slots by `APP_MANAGER_MAX_PAGE_INSTANCES`; the size
of each slot is defined solely by `CONFIG_APP_MANAGER_PAGE_STATE_BYTES`. The
`memory_size` of a Page definition must not exceed
`APP_MANAGER_PAGE_STATE_BYTES`; page allocation and release only zero the
declared range. `app_manager_this_page_memory()` is valid only during the
current lifecycle callback; ops reach the same Page state through the
callback-scoped context. The pointer must not be kept after the page has been
released successfully.

When `app_manager_config_t.max_resident_apps` is 0, the physical limit of 8 is
used. The residence policies are:

- `APP_MANAGER_RESIDENT_REJECT`: reject new Apps once the limit is reached.
- `APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND`: evict the oldest, non-pinned
  background App.

`APP_MANAGER_APP_FLAG_PINNED` prevents automatic eviction and ordinary
background cleanup. HOME should be marked pinned; double-clicking HOME opens the
system task switcher; a single HOME click first closes the task switcher or
control center when that surface is visible, otherwise it routes by the current
active App: enter the app directory when Home is active, else return to the HOME
root page (see section 10.1). To release other background Apps, submit
`APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND`.

## 4. Static Registry and App-owned Routes

A Page implementation is described by an immutable definition, and an App
explicitly selects its navigable pages through a private route table:

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

Only App descriptors are written to `.app_manager_apps`; `app_core/linker.lf`
applies `KEEP()` to that section and generates the start and end symbols. The
definition and route tables are ordinary `const` objects reachable through the
App descriptor, and there is no separate Page link section. Host tests have no
ESP-IDF linker fragment, so internal fixtures may install a firmware-lifetime
App array through `app_manager_register_builtin_descriptors()` from
`app_manager_builtin.h`; that function is not a runtime registration API.

Initialization validates everything once against the compile-time per-page
capacity:

- App IDs and Page IDs are non-empty with valid lengths;
- App IDs are globally unique and Page IDs are unique within an App;
- flags are valid, and the route array is non-empty and matches its count;
- the root Page of every App exists exactly in its own route table;
- route definitions are non-null, and a definition must set exactly one of
  `.handler` or `.ops`;
- the definition `memory_size` does not exceed `APP_MANAGER_PAGE_STATE_BYTES`.

The same Page ID may exist in different Apps, and the same definition may be
referenced by multiple App routes. Each route owns its own `page_id` and a
stable, unmanaged `user_data`, while every page instance still gets independent
private memory and navigation stack. Shared pages must be explicitly opted in by
the target App; navigation returns `ESP_ERR_NOT_FOUND` when no route is bound,
and public boolean queries such as `app_manager_is_page_present()` return
`false`. All descriptors, routes and definitions live for the entire firmware
runtime; there is no dynamic App, provider, lease, runtime bare-handler creation
or free-mount interface.

## 5. Typed Blob and New Intent

Navigation arguments use a fixed 96-byte versioned value:

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

When a command is submitted, the request, the ID strings and the whole blob are
copied into the command slot, so the caller may release the request and ID
storage as soon as the call returns. The `context` of an asynchronous completion
is not copied, and the caller must keep it valid until the completion runs.

A valid blob must satisfy:

- `version == APP_MANAGER_TYPED_BLOB_VERSION`;
- `size <= APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES`;
- `reserved == 0`.

When an App/Page is first created, the arguments are already readable through
`app_manager_this_page_arguments()` before `ONSTART`. A repeated RUN of an
existing App, or OPEN of an existing Page, fully replaces the previous arguments
and sends `ONNEWINTENT`. For a background target, `ONNEWINTENT` happens before
its `ONMOUNT/ONRESUME`; if the target is in the foreground the Screen is still
available, but UI may only be created in `ONMOUNT`.

## 6. Page Lifecycle

A normal page instance follows:

```text
ONSTART -> ONMOUNT -> ONRESUME -> ONPAUSE -> ONUNMOUNT -> ONSTOP
```

`ONNEWINTENT` is a side-channel message that delivers new arguments to an
existing instance and does not replace the phases above. In ordinary navigation,
after the source successfully `ONPAUSE`s, the target receives `ONRESUME` before
presentation start and the target first frame. At that point the target already
owns foreground active resources but the nav model has not committed yet; it
becomes the stable foreground only after the animation ends successfully and
source cleanup completes. If it fails before the commit, a target that already
RESUMEd receives a compensating `ONPAUSE`, so pages must allow one
`ONRESUME -> ONPAUSE` pair to occur in a navigation attempt that never became
stably visible.

| Message | Ownership boundary | What should be done |
| --- | --- | --- |
| `ONSTART` | The Page instance and private memory start to exist | Initialize non-visual state and invalid values for long-lived handles |
| `ONMOUNT` | The framework has created the Screen for this Page | Create UI only under `app_manager_this_page_screen()` |
| `ONRESUME` | The source has paused, the Page is about to join the target first frame; nav has not committed | Start timers, subscriptions, input and active sessions |
| `ONPAUSE` | The Page leaves the foreground, or an uncommitted navigation is compensating a rollback | Stop timers, unsubscribe, close input and active sessions |
| `ONUNMOUNT` | The Screen is still accessible | Delete page UI and clear all LVGL object pointers |
| `ONSTOP` | The Page instance is about to be released | Release remaining non-visual resources; no Screen is available here |
| `ONNEWINTENT` | The arguments of an existing instance have been updated | Read and consume the latest Typed Blob |

Resource placement rules:

- `ONMOUNT/ONUNMOUNT` manage LVGL UI only.
- `ONRESUME/ONPAUSE` manage foreground active resources only.
- `ONSTART/ONSTOP` manage page instance-level non-visual state.
- Timers, subscriptions, input and active sessions started by `ONRESUME` can
  produce side effects during the animation; callbacks may only operate on the
  target's own mounted Screen and must not implicitly assume that the animation
  has finished or the nav model has committed.
- `ONRESUME/ONPAUSE` must be compensable as a pair. When `ONPAUSE` cleanup
  fails, retain the valid handles and report the error; do not destroy UI early
  or assume the source has been restored.
- Do not use `lv_screen_active()` as the page root; during an animation it does
  not represent the owner of the current lifecycle callback.
- Lifecycle callbacks, ops context, `this_page_*` return values and route
  `user_data` do not provide cross-task UI ownership.
- Cleanup errors may only be reported from `ONPAUSE` or `ONSTOP`.

## 7. Per-Page Screen and Transition Engine

presentation initializes one permanent neutral Screen. Before MOUNT the
framework creates an independent LVGL Screen for the Page and deletes it after
UNMOUNT. A stable foreground has exactly one Page Screen; background pages have
no LVGL objects. The default `RGB565` configuration prepares two resident
full-resolution RGB565 PSRAM snapshots for transitions: 329,728 B each on a 368
x 448 panel, 659,456 B total (644 KiB). They are controlled by
`CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION` and do not use the
internal/DMA heap.

For a valid non-`NONE` transition the backend first captures the source and the
target, then places both Images into a full-screen opaque child overlay on the
target Screen and animates only the Images; the target therefore stays active
once the animation starts while its live object tree is covered by the overlay.
When the animation completes, the overlay is deleted first and only then is the
existing lifecycle completion delivered, so source cleanup never runs while an
Image still references a snapshot. Snapshot mode supports only plain `RGB565`
and depends on PSRAM; the `RGB565_SWAPPED` experimental configuration, an
explicit disable, or a snapshot resource/capture/object/animation failure falls
back automatically to the native `lv_screen_load_anim()` path. The native call
still uses `auto_del=false`, and final Screen deletion continues to be decided
by the lifecycle. A non-empty Page Screen must have at least one opaque direct
child covering the full screen, and the whole object tree must not set
`LV_OBJ_FLAG_OVERFLOW_VISIBLE`; a transparent root or a page that needs
off-screen drawing falls back to the native animation for that transition.

14 real effects plus one configuration placeholder are supported:

| Category | Enum |
| --- | --- |
| Configuration placeholder | `DEFAULT` |
| Immediate switch | `NONE` |
| Fade | `FADE` |
| Cover | `COVER_LEFT`, `COVER_RIGHT`, `COVER_UP`, `COVER_DOWN` |
| Push | `PUSH_LEFT`, `PUSH_RIGHT`, `PUSH_UP`, `PUSH_DOWN` |
| Reveal | `REVEAL_LEFT`, `REVEAL_RIGHT`, `REVEAL_UP`, `REVEAL_DOWN` |

Four default transitions are stored in `app_manager_config_t`:

| Scenario | Default |
| --- | --- |
| App forward | `FADE`, 220 ms |
| App back | `FADE`, 220 ms |
| Page forward | `PUSH_LEFT`, 220 ms |
| Page back | `PUSH_RIGHT`, 220 ms |

Each `app_manager_nav_request_t` may override the effect and duration. `DEFAULT`
uses the scenario default effect; a duration of 0 uses the scenario default
duration; `NONE` always disables the animation and ignores the duration.

### 7.1 Commit Order

App/Page forward:

```text
target START or NEW_INTENT
target MOUNT
source PAUSE
target RESUME
presentation start, commit the target first frame and play source -> target
source UNMOUNT
commit nav model
complete command
```

Page back or App exit:

```text
target MOUNT (neutral when there is no target)
source PAUSE
target RESUME (skipped when there is no target)
presentation start, commit the target first frame and play source -> target/neutral
source UNMOUNT -> STOP
commit nav model
complete command
```

The target `ONRESUME` precedes presentation start, so its timers, subscriptions,
input and active sessions are already running for the whole animation; the
transparent barrier only blocks LVGL touches and does not withdraw service
requests, event callbacks or other external side effects produced by those
resources. The target lifecycle callback may update its own Screen at this
point, but must not judge whether navigation has committed from active/present
queries.

When presentation cannot start, the framework degrades to `NONE` and continues
with the post-animation cleanup and commit if the target can be loaded
immediately; otherwise it first sends a compensating `ONPAUSE` to the target,
then restores the source (START/MOUNT/immediate load/RESUME), and finally
UNMOUNTs the target and rolls back its created state. A source `STOP` failure
after the animation uses the same compensation order and still completes the
command with the original error. If the compensating target PAUSE fails, the
framework must not UNMOUNT the target or RESUME the source; it keeps the target
Screen and active resources and commits it as the foreground, completing the
command with the original error. Depending on the transition type, the source is
retained as a `UNMOUNT_ONLY`, `DESTROY_PAGE` or `DESTROY_APP` cleanup owner; the
next navigation retries that action first, so two pages never hold a foreground
session at the same time. `UNMOUNT_ONLY` does not STOP a background resident
Page; `DESTROY_APP` destroys only the page, Screen and runtime node, and the
static descriptor always stays valid.

`BACK_TO` plays one animation between the start point and the final target only;
intermediate pages run STOP/free from stack top to bottom, and pages already
removed successfully before a cleanup failure count as an allowed partial
commit.

During an animation presentation places a transparent touch barrier on the Sys
Layer. It blocks LVGL touches only, not HOME, POWER or app-control. When new
navigation arrives, the current animation is first pushed to its completion
callback and stable target, and then the next command in the FIFO runs. Screen
creation or animation start failures follow the degradation or compensating
rollback rules above; no failure may bypass target PAUSE, lose cleanup ownership
or produce two active sessions.

`app_manager_is_transitioning()` distinguishes the temporary animation state.
During an animation the topology queries such as active/present/userdata return
the same conservative values as a paused Screen, so `false` or `NULL` must not
be read as the instance having been destroyed.

### 7.2 Sys Layer Edge Back

Edge back is a system-level input of the app-manager and belongs to no App or
Page. The framework permanently creates a transparent capture strip on each of
the left and right sides of the LVGL Sys Layer; the strips use
`LV_OBJ_FLAG_CLICKABLE` and `LV_OBJ_FLAG_PRESS_LOCK` so that a press that moves
out of the narrow edge region still receives the whole pointer sequence. The
gesture feedback is an independent `64x144` A8 Canvas indicator that accepts no
clicks and never enters the Page MOUNT/UNMOUNT lifecycle. The framework carries
no static images and depends on neither LVGL Vector nor ThorVG; the arc-shaped
alpha mask is generated programmatically with Q15 integer arithmetic at init.
The indicator slides in horizontally from outside the screen edge that owns the
gesture; both side labels use `LV_SYMBOL_LEFT` and never switch to
`LV_SYMBOL_RIGHT`.

A valid gesture must satisfy all of:

- the global edge back switch is enabled;
- the Screen is in a stable interactive state, with no page animation, Screen
  suspend or deinit;
- the current page has a valid back target;
- the pointer starts on the left or right capture strip and moves into the
  screen to the commit threshold.

The 0.6.0 geometry parameters are computed from the display resolution and
clamped to lower and upper bounds:

| Parameter | Rule |
| --- | --- |
| Edge capture width | 8% of screen width, clamped to 24..32 px; 29 px on a 368 px wide screen |
| Commit distance | 15% of screen width, clamped to 48..80 px; 55 px on a 368 px wide screen |
| Direction slop | 3% of screen width, clamped to 10..16 px |
| Armed release hysteresis | 1/5 of the commit distance, clamped to 10..18 px |
| Canvas size | fixed 64x144 px, never scaled or deformed during the gesture |
| Indicator Y anchor | centered on the touch point at press time, clamped to an 8 px vertical margin, fixed within the gesture |
| Indicator style | fixed `APP_THEME_COLOR_PLUME_HI` (`#222A31`), opacity 220; only the contour edge uses 1 px alpha antialiasing |

The Canvas uses one fixed 9,220-byte draw buffer (64x144 A8 plus 4 B alignment
slack) in external RAM BSS (`APP_MANAGER_EXT_BSS` -> `.ext_ram.bss`; the product
enables `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) and allocates no memory
during the gesture. The arc is computed per row: the row coordinate is first
normalized to Q15 with `u = abs(2*y-(H-1))/(H-1)`, then
`smooth = u * u * (3 - 2 * u)` and `depth = 62 - (62 - 28) * smooth` narrow the
effective depth smoothly from 62 px in the middle to 28 px at the top and
bottom ends. The left side fills `[0, depth)` and the right side fills
`[64-depth, 64)`; both directions share the same buffer and regenerate the
mirrored contour and invalidate the Canvas only when the capture side changes.

The horizontal direction locks once the inward displacement reaches the slop and
the vertical displacement is no more than twice the inward displacement. Before
the lock, the sequence is suppressed only when the vertical displacement reaches
two slops and is greater than twice the non-negative inward displacement;
diagonal movement between those two conditions stays pending and keeps
observing, so a slight vertical offset does not cause a false cancel. An obvious
outward movement larger than one slop still cancels immediately. Reaching the
commit distance enters armed; only a retraction below "commit distance minus
hysteresis" leaves armed, so jitter around the threshold does not flip the state
repeatedly.

Home never has a gesture back entry even when the global switch is enabled. A
sub-page of another App returns to the previous Page; a non-Home root page can
gesture back only when a previous App exists in the App history. With no target
the capture strip stays inactive and edge input continues through the normal
LVGL hit-test.

During `PRESSING` the framework only updates the Canvas horizontal position by
`inward / trigger_distance`. The left X moves from `-64` to `0`, the right X
moves from the screen width to "screen width minus 64"; the Canvas size,
contour, opacity and Y anchor stay unchanged throughout. The `LV_SYMBOL_LEFT`
Label is a Canvas child, appears only after ARMED is entered and hides
immediately once a retraction leaves ARMED. The Page Screen coordinates,
opacity, style and active state do not change with the finger in real time, and
no full-screen snapshot is created. The `PRESSING` phase never captures a
snapshot; when the threshold is not reached before release, the direction is
wrong, input is cancelled or the state is preempted, only the indicator is reset
and no navigation command is produced; after the threshold is reached one
existing asynchronous `BACK` is submitted, and the default animation of the
Page/App back scenario performs the actual Screen switch.

On press the gesture controller takes a navigation epoch token and keeps
validating it while moving and on release. If another navigation gained
admission in the meantime, the token becomes invalid at once, the gesture only
hides the indicator and no longer submits a stale `BACK`; the conditional commit
on release also re-checks the epoch and the current source App. After a
successful submit the command still enters the unified fixed-capacity command
FIFO; no second navigation queue is created.

New navigation, Screen suspend or deinit cancels an unsubmitted pointer sequence
and hides the indicator; Screen resume or a stable navigation end recomputes
whether the current page may capture. A gesture `BACK` that already gained
navigation gateway admission is not withdrawn by a later switch-off and still
reaches a stable state with the exactly-once completion semantics of ordinary
asynchronous navigation.

The runtime switch is public API and adds no `app_manager_config_t` field:

```c
esp_err_t app_manager_back_gesture_set_enabled(bool enabled);
bool app_manager_back_gesture_is_enabled(void);
```

The global switch defaults to `true` after successful initialization.
`app_manager_back_gesture_set_enabled(false)` blocks new gestures and resets an
unsubmitted indicator; `app_manager_back_gesture_is_enabled()` reports only this
global policy value and does not mean the current page necessarily has a back
target. On Home, with no App history, while the Screen is paused or during an
animation the policy value may still be `true` while effective capture is
`false`; before initialization and after deinit the getter returns `false`. The
setter returns `ESP_OK` when the policy is already applied or saved for the next
Screen resume, otherwise it returns an admission or UI dispatcher error.
Business pages must not access or rearrange the Sys Layer capture objects
directly.

### 7.3 Control Center

The control center is a system-level interface of the app-manager
(`app_manager_control_center.c`) and belongs to no App or Page. It creates a top
capture band on the LVGL Sys Layer (height 40 px; horizontally inset on the left
by the edge back width, 29 px on a 368 px wide screen, and reserving 58 px on
the right for the page header action button, i.e. 42 px button + 16 px page
margin). The capture band is invisible and its height is the only capture region
— a press must land inside the band to take over the gesture, so 40 px is chosen
to cover a finger resting on the screen border; the right end of the band stops
left of the `app_ui_page_set_action()` button and never preempts the page's own
widgets. A downward swipe of more than 48 px from the top of any stable page
opens it; while the surface is open the capture band is disabled, and clicking
the scrim (outside the card), edge-back, ordinary navigation or a single click
on HOME all close it.

Transition: when opening, the card slides in from above the screen to its final
position and the scrim fades from fully transparent to `CC_SCRIM_OPA` in step;
closing by clicking the scrim plays the reverse. Both are 180 ms (matching the
task switcher fade-in duration) and are driven by the same progress animation
for card offset and scrim opacity, so they never drift apart. The surface counts
as invisible as soon as the exit starts (`is_visible()` returns false and the
capture band stays disabled), and the objects are hidden and the capture band
restored only after the animation ends, so a retracting swipe cannot pull it up
again. Screen off, standby, navigation begin, policy disable, `hide()` and
deinit use an instant hide and cancel the running animation without blocking the
transition. During the transition the tiles, both sliders and the
screen-off/standby buttons accept no input.

Surface content: time/date, a brightness slider (a temporary brightness preview
while dragging, persisted on release), a speaker volume slider in 0–100 %
(applied as the knob moves, with the level rendered right-aligned in the row
and re-read from the service on every 1 s poll, which also greys the row while
the bridge reports no audio path), Wi-Fi and Bluetooth tiles (the circular
icon base and the icon share a tri-state color — off `INK_SOFT` / enabled but
not connected `AMBER` / connected `MINT` — and the base uses the corresponding
tint), screen off now and standby now. The value label tracks every knob step
while the codec write is rate-limited to `CC_VOLUME_APPLY_MS` (60 ms) and always
forced on release, so a drag costs one serialized codec transfer per window
instead of one per pixel; a level the service refuses is not retried on release,
and the 1 s poll restores the row from the service. Nothing in that throttled
path persists: the settle commits once per drag through
`set_volume_persisted`, which applies the level and queues the one-byte NVS
write on the chore worker, so the LVGL worker never executes a flash write;
brightness likewise keeps its NVS write on release.
A settle that lands on the already-stored level sends nothing, and a refused
level is never stored. The surface is built lazily:
`app_manager_get_image()` is usable only after `app_manager_init()` reaches
READY, so the icon nodes are created on the first `show`; the first open has to
build the objects and decode the icons and its entrance animation may start a
frame or two late, while later opens have no such cost. The card height is
derived from the rows it actually builds, so a missing bridge shortens it
instead of leaving a gap.

Service bridge: app_manager does not depend on
connectivity/device_link/time/audio; the control center reads and requests
through the injected `app_manager_control_center_ops_t` (an optional
`app_manager_config_t` field); `main/app_runtime_control.c` adapts it from
`connectivity_manager`, `device_link_service`, `time_service` and
`audio_service`. A null callback hides the corresponding tile or row; the volume
row additionally requires both `get_volume` and `set_volume`, while a missing
`set_volume_persisted` only drops persistence. The ops getters run on the LVGL
worker and are non-blocking; the setters only enqueue (0 wait for Bluetooth) or
perform the short codec control transfer the audio service serializes. The
settle's one-byte flash commit is queued by `audio_service` to the chore worker,
so the LVGL worker never executes it. Persistence is therefore not
app_manager's concern: the `volume` key is owned by `audio_service`, which
restores it after `audio_service_init()` and clears it on a factory reset.
`main/app_runtime_control.c` only forwards the settle, so the surface keeps the
distinction between a drag preview and a committed level without knowing where
the level is stored.

Gating and ownership follow the task switcher: pulling down is allowed only when
the Screen is stable, with no transition, not suspended and with the task
switcher invisible; navigation begin hides it synchronously (cancelling the
transition); screen suspend cancels the animation and hides it; deinit cancels
the animation first and then releases the surface and the capture band.

Runtime switch and public API:

```c
esp_err_t app_manager_control_center_show(void);
esp_err_t app_manager_control_center_hide(void);
bool      app_manager_control_center_is_visible(void);
esp_err_t app_manager_control_center_set_enabled(bool enabled);
bool      app_manager_control_center_is_enabled(void);
esp_err_t app_manager_pm_request_standby(void);
```

`app_manager_pm_request_standby()` reuses the suspend path and enters standby
immediately after the screen turns off, without waiting for the configured
standby delay; any user activity or wake cancels that intent.

## 8. Unified Navigation Gateway

All navigation uses `app_manager_nav_request_t`:

| operation | Semantics |
| --- | --- |
| `RUN` | Create an App, or bring an existing App to the foreground |
| `EXIT` | Exit the specified App |
| `EXIT_SELF` | Exit the App that owns the calling lifecycle callback |
| `OPEN_PAGE` | Open a static Page; start or foreground the target App when needed |
| `BACK` | Go back one page; back on a root page is equivalent to exiting the current App |
| `BACK_TO` | Return to a specified Page already present in the current App stack |
| `REMOVE_PAGE` | Delete the specified Page; deleting the current page is handled as BACK |
| `CLEANUP_BACKGROUND` | Retry cleanup of all non-pinned background Apps |

`EXIT_SELF` is accepted only inside a lifecycle callback and the request
`app_id` must be `NULL`; the gateway copies the owner ID before the callback
returns. Ordinary tasks or general LVGL events should use `EXIT`, `BACK` or
another explicit navigation operation.

`OPEN_PAGE` accepts only static Page IDs from the registry:

- if the App does not exist, create the App, the root page and the target page;
- if the App is in the background, bring it to the foreground;
- if the Page already exists, update the intent and perform a back-to;
- if the Page does not exist, create it and push it onto the target App page
  stack.

### 8.1 Synchronous API

```c
app_manager_nav_request_t request =
{
    .operation = APP_MANAGER_NAV_OP_RUN,
    .app_id = "settings",
    .transition = {.effect = APP_MANAGER_TRANSITION_DEFAULT},
};

esp_err_t result = app_manager_navigate(&request, UINT32_MAX);
```

`app_manager_navigate()` may only be called from a non-UI task. It waits for the
animation, lifecycle and required cleanup to reach a stable result; calling it
on the UI worker returns `ESP_ERR_INVALID_STATE`.

A synchronous timeout follows the mailbox safety semantics:

- if the command has not started, it is cancelled and no callback runs;
- if the command has already started, the call keeps waiting for a stable
  completion but finally returns `ESP_ERR_TIMEOUT` to the caller.

Therefore, once the synchronous API returns, a stack-allocated request may be
destroyed safely even if the actual wait exceeded the specified timeout.

### 8.2 Asynchronous API

```c
static void _navigation_complete(esp_err_t result, void *context)
{
    /* Runs on the UI worker. */
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

`app_manager_navigate_async()` may be called from an ordinary task, an LVGL
event, a navigation completion or a lifecycle callback. The return value only
reports command slot and mailbox admission; the final result after successful
admission is given by the completion on the UI worker. Re-entrant requests from
inside a lifecycle callback enter the same FIFO and never modify the current nav
model recursively.

The fixed command pool capacity is set by
`CONFIG_APP_MANAGER_NAV_COMMAND_CAPACITY` (product baseline 8). When the pool is
full or the mailbox rejects the request, `ESP_ERR_NO_MEM` is returned
immediately and the completion does not run.

## 9. Cleanup Failure and Rollback

A raw handler returns `void`. A failure to release resources in `ONPAUSE` or
`ONSTOP` must be reported:

```c
esp_err_t result = event_bus_unsubscribe(state->subscription);
if (result != ESP_OK)
{
    app_manager_this_page_report_cleanup_error(result);
    return; /* Keep the valid handle and retry later. */
}
state->subscription = EVENT_BUS_SUB_HANDLE_INVALID;
```

The typed ops `pause` and `stop` return `esp_err_t` directly; a missing callback
counts as a successful no-op and must not call
`app_manager_this_page_report_cleanup_error()` to report it again.

The framework records only the first non-`ESP_OK` error reported in the current
callback:

- on a PAUSE failure the page stays active and UNMOUNT/STOP do not continue;
- on a STOP failure the instance, private memory and unreleased handles stay
  with the original owner;
- when an uncommitted target has already RESUMEd, the rollback must first PAUSE
  the target, then UNMOUNT the target and RESUME the source; apps should treat
  this as a normal compensating lifecycle pair;
- when the compensating target PAUSE fails, the target Screen, active resources
  and recovery ownership are retained and the framework must not continue to
  UNMOUNT the target or RESUME the source;
- a STOP failure on back/exit restores the source Page after the target
  compensation succeeds instead of committing half a foreground;
- background cleanup, residence eviction and deinit may run the same cleanup
  again;
- handles must not be cleared early after a release failure, otherwise the
  framework cannot keep the ownership evidence for the business code.

`ONMOUNT/ONUNMOUNT` provide no cleanup error channel. LVGL Screen creation,
deletion or animation start failures are rolled back by presentation/lifecycle
itself. No failure path may lose a Page slot, a Screen or cleanup ownership.

## 10. Screen Suspend, Resume, and Deinit

### 10.1 Double-click HOME and the System Task Switcher

A single and a double click on HOME are disambiguated by app-control within a
350 ms window: a second click inside the window submits the
`APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW` system command; when the window
expires without a second click the single-click action is resolved: when the
task switcher is visible, submit `APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_SHOW` to
toggle it closed (when it is visible together with an overlaid control center,
the switcher on top is closed); when the control center is visible, submit
`APP_MANAGER_NAV_SYSTEM_CONTROL_CENTER_SHOW` to toggle it closed and play the
exit animation; when neither is visible, route by the current active App
snapshot: submit `RUN` to enter the app directory when Home is active, otherwise
submit `OPEN_PAGE(home, root)`, and do nothing when the snapshot is unavailable.
All these actions are submitted asynchronously through the unified navigation
gateway and are admitted only after the worker state machine has decided on a
single action, so the navigation FIFO never contains a preceding command that
could be cancelled. A second double click while the switcher is open closes it
and returns to the original foreground task. A HOME sequence that starts with
display sleep or a standby wake only wakes the device and takes no part in
disambiguation; a long press, POWER screen off, standby preparation and deinit
all cancel the pending window. The worker shortens its poll wait by the
remaining window time, so the single-click latency is at most 350 ms plus one
poll period.

Recent tasks are a system-level interface of App Manager
(`app_manager_task_switcher.c`) and belong to no App or Page: it creates a
full-screen surface on the LVGL Sys Layer; while open, the original foreground
App keeps running and receives no PAUSE/UNMOUNT, and touch is fully taken over
by the switcher — the full-screen root stays CLICKABLE, the title bar and the
memory footer also swallow clicks, and the covered page receives no touch at
all; clicking the surface itself (not a card) or clicking HOME once closes the
switcher and returns to the original task. Opening copies the MRU task list
(including the current task and `cleanup_pending` state) through
`app_manager_lifecycle_recent_tasks_call()` and builds a horizontally scrollable
card carousel: each content-sized card is 240 px wide and about 187 px high,
with a 56 px image inside a 64 px circular icon base at the top, the name and
status line (`"当前"` / `"点按切换"` / `"关闭失败"`, i.e. current / switch with a tap /
close failed) centered below the icon, and a 44 px independent close button at
the top right of every non-current card; no screen pixel cache is kept. The
bottom of the surface shows internal RAM and PSRAM used/total text and usage
bars (turning red at ≥85%), refreshed on open/rebuild and every 1 s; the surface
fades in for 180 ms on open. Clicking a background task card restores it through
ordinary RUN (the navigation gateway hides the switcher automatically before
execution); "close" is submitted through the card's top right button or an
upward swipe of more than 64 px on that card (horizontal displacement not
exceeding 64 px) as `EXIT` and the list is rebuilt in place. CLICKED
suppression: the horizontal scrolling that follows the carousel is suppressed by
the native LVGL scroll; a drag that did not become a scroll swallows the click
on release once it exceeds 10 px, and cards and close buttons suppress it the
same way. Click actions capture the `app_id` and the list generation at event
time and execute through `lv_async_call`, covering activation, the close button
and swipe-to-close, so a list rebuild in the same frame cannot make a deferred
action hit the wrong task. On a failed switch the switcher reopens and shows
`"切换失败"` (switch failed) on the corresponding card. A list rebuild (close
completion, eviction, cleanup-failure retry, reopening) returns to the carousel
start position. "Clear all" is a composite system command `CLEAR_ALL` that first
switches to Home and then, inside the same gateway command, destroys all
non-pinned/hidden tasks (including the foreground task from before opening) and
closes the switcher when done. Ordinary navigation hides the switcher
synchronously before execution; an `EXIT` issued by the switcher itself stays
visible through the command-level `keep_switcher_alive` flag and shows a busy
shell, then rebuilds in place after completion or failure. screen suspend first
cancels the pointer sequence, clears the cards and the busy shell and hides the
surface (the root skeleton is kept), and only then enters page PAUSE; after
resume it stays closed. deinit deletes the switcher and the presentation Sys
Layer objects.

Recent-task data is provided by `app_manager_get_recent_tasks()`: up to
`APP_MANAGER_MAX_RECENT_TASKS` non-pinned, non-hidden resident Apps in MRU
order, each item carrying the static descriptor (including `display_name` and
`icon_id`), the retained current Page ID, and the `active` and `cleanup_pending`
states. When an App is destroyed (EXIT, eviction, rollback cleanup, deinit) the
lifecycle releases the instance uniformly; when an icon is missing the switcher
falls back to a symbol. A task whose cleanup failed stays in the list marked
`cleanup_pending` and may be retried; an instance in that state is
conservatively hidden from public topology queries
(`app_manager_is_page_present` etc.) while keeping its residence count and
staying in the recent-tasks list, and the next navigation retries its cleanup
before continuing.

Screen suspend plays no animation:

```text
fast-forward the current animation and commit the stable target
current PAUSE
load neutral
current UNMOUNT, delete the Page Screen
save the running App count snapshot
close Screen access admission and drain in-flight access
pause LVGL adapter
wait for panel IO / DMA fence
commit hardware suspend
```

Screen resume likewise plays no animation:

```text
hardware resume_prepare (panel still hidden)
resume LVGL adapter
current MOUNT
load current Screen
current RESUME
refresh and wait for the fence
restore indev, brightness and resume_commit
```

Topology queries during PAUSING, PAUSED and animation are conservative values.
After PAUSED has been entered successfully, `app_manager_get_running_apps()` and
`app_manager_is_all_closed()` use the running-count snapshot saved at pause
time; the remaining active/present queries return `false` or `NULL`.

`app_manager_deinit()` must not be called from the UI worker. The normal order
is:

1. app-control stops producers, unregisters input and cancels standby
   (`app_manager_control_deinit`);
2. edge back stops admission; navigation stops admission, drains commands and
   fast-forwards the last animation;
3. lifecycle enters CLOSED and waits for admitted access to leave;
4. the UI worker deinits, in order, the control center, the task switcher,
   lifecycle (PAUSE/UNMOUNT/STOP, delete Screens, release runtime nodes), the
   edge back objects and presentation (neutral Screen, barrier, completion
   timer);
5. reset the registry, close the mailbox and release the command pool;
6. pause and fence the LVGL adapter and release themes and fonts (the adapter
   VFS stays mounted);
7. deinit the LVGL adapter;
8. release the resource mappings (mmap assets).

A cleanup failure at any step keeps the corresponding ready/ownership state; a
further `app_manager_deinit()` call keeps retrying. Cold start, Screen resume
and deinit play no page animation.

## 11. Display and PM Cooperation

The app-control task manages display and standby serially:

```text
ACTIVE -> DIM -> SCREEN_OFF -> STANDBY_PREPARING -> STANDBY
   ^        |          |                 |              |
   +--------+----------+-----------------+--------------+
                    user activity / wake
```

POWER, HOME, touch and system wake are merged through generation/pending state.
The standby guard re-checks the local epoch, the canonical display sleep state
and the wake intent at the system commit point; the commit callback only makes
non-blocking atomic records, and the final state is still published after
app-control has reconciled it. When the display is already asleep, a wake
sequence started by a physical key only restores the display and its trailing
`UP/CLICK` is no longer replayed as navigation or screen-off; the next
independent key sequence started on an active display resumes normal semantics.

The brightness register and the LVGL color flush share panel IO. All normal,
temporary, DIM and restore brightness are dispatched synchronously to the UI
worker; BSP panel operations must never be called directly from app-control or
the startup task. Cold-start input/PM admission stays closed after
`app_manager_init()`; `app_manager_startup_commit()` registers physical input
and resets the inactivity epoch at the moment the first screen really becomes
usable. Normal brightness, screen-off and standby delays are persisted by App
Manager itself. The DIM delay is read-only, comes from the NVS key `pm_dim_to`
and defaults to 15 s; there is currently no public DIM setting API. The
synchronous PM setters only accept non-UI worker callers (returning
`ESP_ERR_INVALID_STATE` on the UI worker); the settings pages submit through the
`*_async` post and observe the persisted result through the getters. Factory
reset calls `app_manager_factory_reset_persisted_state()` before initialization
to delete these settings idempotently, and the following initialization restores
the defaults.

Display resume follows `resume_prepare -> adapter resume -> Page MOUNT/RESUME ->
refresh/fence -> resume_commit`. Panel display and physical touch are enabled
only after the complete restore frame has been committed, avoiding stale frames,
touch pass-through and incomplete DMA.

On the non-TE display path three capacity parameters play different roles: App
Manager uses 60-row PSRAM double draw buffers, BSP limits each SPI DMA transfer
to 10 rows (7,360 B) and the transfer queue depth to 2; LVGL can therefore
render in larger strips while the internal DMA bounce buffer peaks at 14,720 B.
That path enables no PSRAM direct DMA and depends on no periodic SPI fence.

`CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS` is for development acceptance only.
When enabled, a low-priority PSRAM-stack task periodically samples adapter FPS,
DMA/internal heap and PSRAM; LVGL display events also record render, flush and
flush-wait, and the panel proxy records commit count, pixels, failures and
synchronous call duration. presentation records animation boundaries only after
`lv_anim_get()` confirms the animation really started, and aggregates per effect
the active duration, the P50/P95/P99/max of the final-flush interval, the count
meeting 33,333 us and the longest run above 41,667 us. adapter FPS is
observation only; benchmark `passed` means only that sampling, DMA, panel commit
and animation statistics are complete, and the actual frame-rate threshold is
judged by the on-device acceptance logic. Stopping diagnostics unregisters the
display event first, then confirms the task has exited and releases the PSRAM
stack, and only after that may the LVGL adapter be destroyed. The first
characterization stage is fixed to display-only; the second stage, full,
audio-only or tcp-only, is chosen by the root benchmark runtime profile; STRESS
uses the load from the profile. The campaign mode, duration, IPv4 host, port and
rate are not part of App Manager Kconfig but are passed to
`display_benchmark_start()` as typed configuration generated from
`tests/display/profiles/*.json`. App Manager Kconfig keeps only diagnostics and
the display static resource/feature gates.

`CONFIG_APP_MANAGER_LIFECYCLE_DEBUG_LOG` is off by default. Enable it only when
debugging lifecycle ordering; it uniformly emits INFO traces for
`START/MOUNT/RESUME/PAUSE/UNMOUNT/STOP/NEW_INTENT`. When disabled the compiler
strips the event name table and the log calls, without affecting lifecycle
callbacks, observers or state transitions.

`APP_MANAGER_MSG_COUNT` is the number of lifecycle messages and an array bound,
not a dispatchable message. The old name `APP_MANAGER_MSG_USER_END` is kept only
as a source-compatibility alias and must not be used by new code.

## 12. Application Development Contract

This section describes the current contract for the built-in Apps/Pages in
`layers/apps`. Public types and capacities are defined by
`app_core/include/app_manager_types.h` and `app_core/include/app_manager.h`;
apps do not duplicate capacity, queue or version constants.

### 12.1 Page Models and Definitions

Built-in Pages combine the following models according to their actual resource
ownership:

| Page model | Lifecycle phases | Owned resources |
| --- | --- | --- |
| Static | MOUNT, UNMOUNT | LVGL objects |
| Refresh | MOUNT, RESUME, PAUSE, UNMOUNT | timer, UI subscription |
| Session | START to STOP | retained state, active service sessions |
| Worker | RESUME, PAUSE | page worker, thread-safe snapshot |
| Arguments | START or NEWINTENT, plus the required UI phases | Typed Blob, route data |

A Page definition must set exactly one of `.ops` or `.handler`. Typed ops are
used for ordinary lifecycle phases; the raw handler is retained only as a
compatibility capability that receives `app_manager_msg_type_t` and the route
`user_data` directly and is not the preferred interface for new apps. An app
only implements the ops it actually needs; missing callbacks are successful
no-ops. `STOP` stays as the final instance cleanup boundary even though current
product pages do not use it yet.

Page state holds instance-private state only, and its `memory_size` does not
exceed `APP_MANAGER_PAGE_STATE_BYTES`; large payloads are allocated separately
and are neither placed in Page state nor kept in cross-instance global state. An
immutable Page definition is bound by the static route of its owning App; the
same definition may be referenced explicitly by several Apps, but every instance
owns independent state and a navigation stack. At runtime there is no bare Page
without a route.

### 12.2 Context and Lifecycle Ownership

`app_manager_page_context_t` is valid only during the current ops callback:

- `state` points at the framework-zeroed Page private memory and lasts until a
  successful STOP; START only needs to set invalid handles and other non-zero
  initial values.
- `route_user_data` comes from the immutable route and its storage covers the
  whole firmware runtime.
- `screen` is valid in MOUNT, RESUME, PAUSE and UNMOUNT; it is `NULL` in START
  and STOP; NEWINTENT carries a Screen only when the instance is already
  mounted.
- `arguments` points at the current immutable Typed Blob and can only be read
  during the callback; payload that must be used after the callback is validated
  and copied by the Page.

The normal lifecycle is:

```text
START -> MOUNT -> RESUME -> PAUSE -> UNMOUNT -> STOP
```

An existing instance receives new arguments through the side-channel NEWINTENT
call. If navigation fails before commit, the target may receive a compensating
`RESUME -> PAUSE` even though it never became the stable foreground.

| Phase | Current ownership boundary |
| --- | --- |
| START | Initialize instance-level non-visual state and invalid handles |
| MOUNT | Create the complete UI under `context->screen` |
| RESUME | Start timers, subscriptions, input, workers and active sessions |
| PAUSE | Stop active resources in reverse order; on failure keep valid handles and return the error |
| UNMOUNT | Delete UI and clear every LVGL pointer |
| STOP | Release START resources without repeating PAUSE |
| NEWINTENT | Validate and copy the latest arguments and update the UI for the current mount state |

The ops PAUSE/STOP return `esp_err_t` directly; the raw handler PAUSE/STOP
report the first release error through
`app_manager_this_page_report_cleanup_error()`, and the two channels are never
mixed. On a cleanup failure the instance, resources and valid handles stay with
the original owner and later lifecycle requests may retry. The corresponding
handles are cleared only after PAUSE or STOP succeeds.

### 12.3 Navigation and Arguments

- Ordinary RUN, OPEN_PAGE and BACK are issued by `app_ui_request_*`.
- Custom transitions, Typed Blob or completion use
  `app_manager_navigate_async()`.
- LVGL event callbacks use asynchronous navigation; the UI worker never calls
  the synchronous `app_manager_navigate()`.
- The request, IDs and Typed Blob are copied by value at admission; the
  completion context is not copied.
- Argument pages use an app-private type and payload, validate the blob version,
  type and exact size, and then decode with `memcpy`.
- The target Page must exist in the static route table of the target App.

Weather root, forecast, alerts and alert-detail use typed ops; alert-detail
receives the alert identifier through a Typed Blob and handles NEWINTENT: a
mounted instance refreshes immediately, while a background instance keeps the
copied arguments and refreshes on RESUME. Weather is also the existing reference
for subscription handles, immutable snapshots and PAUSE ownership. The home, app
directory, settings, network settings, clock, recording, level and diagnostics
pages all follow the same typed ops convention.

### 12.4 Concurrency and Resource Boundaries

- The UI worker performs no file, audio, network parsing or blocking hardware
  operations.
- Page workers update the UI only through thread-safe snapshots or the UI
  dispatcher, and stop synchronously before PAUSE returns.
- A UI subscription does not guarantee that the publisher has stopped; keep the
  handle when unsubscribe fails.
- LVGL pointers are dereferenced only in the mounted lifecycle, and animation
  callbacks modify only this Page's Screen.
- Apps do not create a second LVGL task, tick or Screen manager, and do not
  obtain display ownership by bypassing App Manager.

### 12.5 Verification Boundary

The App Manager host tests cover registry, lifecycle, ops/raw mutual exclusion,
context, arguments and PAUSE/STOP failure retry. App and cross-layer host tests
cover the Screen, object, timer, subscription, worker and snapshot baselines of
the real Pages. The relevant commands and sanitizer configurations are in
section 14.

Host tests do not prove on-device display, touch, DMA, RF, power or performance;
those results are confirmed only by the corresponding board tests and resource
observations.

## 13. Initialization Configuration

The fields of `app_manager_config_t` directly relevant to 0.6.0 navigation:

| Field | Meaning |
| --- | --- |
| `resident_policy` | Reject at full capacity or evict the oldest background App |
| `app_forward_transition` | Default App forward animation |
| `app_back_transition` | Default App back animation |
| `page_forward_transition` | Default Page forward animation |
| `page_back_transition` | Default Page back animation |
| `control_center_ops` | Optional; control center service bridge, a NULL callback hides the tile or row it would serve |
| `font_sizes` / `font_count` | When non-NULL, they must be `APP_THEME_FONT_MAX` (7) entries in role order; the same applies to `font_count` |
| `max_resident_apps` | Residence limit; 0 uses the physical limit 8, and values above 8 are rejected |
| `control_task_priority` | app-control worker priority, must be in `[1, configMAX_PRIORITIES)` |

The remaining required fields provide display/touch, fonts and the resource
drive letter, as well as `screen_ops`, `input_ops` and `standby_ops`. The
suspend/resume_prepare of `screen_ops` run on app-control, while
set_brightness/set_brightness_temp/resume_commit are dispatched to the UI
worker. The configuration structure is read by the initialization path and the
hardware operation table is copied into an internal owner. Edge back adds no
configuration field; it is enabled by default after initialization, and the
public switch API is called when it must be disabled at runtime.

Recommended startup order:

```text
BSP / system PM ready
app_manager_init (input/PM admission closed)
register UI dispatcher with event bus
start power service
start connectivity services
navigate RUN SETUP (first boot) or HOME
app_manager_display_commit_initial (show panel, keep indev closed)
app_manager_startup_commit (open touch, key, and PM admission)
```

`main/app_runtime.c` initializes the connectivity services before the first
frame is committed; the panel stays off until display_commit_initial, so
connectivity service initialization counts toward the hidden time before the
first frame. The first boot enters SETUP and later boots enter HOME.

## 14. Tests and Verification

app-core host suite:

```sh
cmake -S layers/app_manager/app_core/tests/host \
      -B /tmp/mt-app-core -G Ninja
cmake --build /tmp/mt-app-core
ctest --test-dir /tmp/mt-app-core --output-on-failure
```

The suite includes `mailbox`, `navigation`, `registry`, `nav_model`, `lifecycle`
(including the debug-log variant), `persistence`, `control`, `adapter`
(including height and OS combination variants) and `display_diagnostics`
(including enabled /disabled and single-task/no-OS variants). The
navigation/lifecycle tests cover edge back epoch admission, conditional BACK,
the concurrent FIFO and the stable back-target; Sys Layer objects, geometry
recognition and cross-layer resource cleanup are covered by the cross-layer
suite below. ASan/UBSan and TSan use:

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

App, main and cross-layer suites:

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

The corresponding sanitizer cache variables are `APPS_SANITIZER`,
`MAIN_HOST_SANITIZER` and `CROSS_LAYER_SANITIZER`, again taking `address` or
`thread`. The cross-layer `presentation` tests cover the 14 effects, snapshot
start/middle/completion, native fallback, immediate completion, the touch
barrier and fast-forward; `cross_layer` covers real app App/Page forward and
back, Screen pause/resume, cleanup rollback and Sys Layer edge gestures. Besides
the navigation result, gesture assertions should also check that exactly one
Page Screen exists at stability, that the overlay/Image of an ordinary
non-`NONE` transition is released after completion, that no snapshot is created
during `PRESSING`, that the Page Screen does not change with `PRESSING`, that
the Canvas stays `64x144`, that the indicator Y anchor is fixed, that the left
and right arcs mirror each other, and that the indicator, timers, subscriptions
and active sessions return to the baseline after cancellation or completion. A8
buffer tests should additionally sample depth and edge alpha at the top,
quarter, middle and bottom to verify the Q15 curve and the 1 px antialiasing; on
a 368 px wide screen, check the 29 px capture strip and the 55 px commit
distance, and separately check that the left X is `-64/-31/0` and the right X is
`368/335/304` (the middle values correspond to inward=28); the left hit at
`x=28` with no hit at `x=29`, and the right hit at `x=339` with no hit at
`x=338`, and the commit boundary should cover inward displacements `54/55`. Also
confirm that a slight diagonal swipe locks the horizontal direction, that an
obvious vertical or outward movement is cancelled, and that the arrow is hidden
before the threshold, visible after ARMED and hidden again after a hysteresis
retraction.

The control center navigation system command
(`APP_MANAGER_NAV_SYSTEM_CONTROL_CENTER_SHOW`) is covered by a stub in the
app-core host suite; the Sys Layer surface and the top capture band are compiled
as real modules in the cross-layer suite, and `sim.control_center` provides
deterministic open/close and visibility queries.

The pre-commit checklist follows the root repository `AGENTS.md` and
`doc/code-style.md` §8.2; this section adds no separate gate. C/C++ files run
AStyle as described in `doc/code-style.md`. Display and lifecycle changes still
need on-device loop verification:

- App/Page bidirectional switching at least 100 times, with no blank frames and
  no touch pass-through;
- at least 100 commits and cancellations on the left and right edges
  respectively, with no false navigation on Home, no-target, sleep or animation
  preemption;
- while dragging, the Sys Layer Canvas only translates horizontally from
  off-screen, with fixed size, contour, opacity and Y anchor; the arrow is
  visible only in ARMED and the Page Screen stays still until the release
  commit;
- average animation frame rate of at least 25 FPS;
- no growth in Screens, LVGL objects, timers, subscriptions and active sessions;
- no panel IO, DMA or fence errors;
- record internal RAM/PSRAM low-water marks and ROM/RAM deltas; newly added
  internal BSS must not exceed 10 KiB, and the gesture A8 buffer lives in PSRAM
  and does not count toward that budget.

## 15. Explicit Boundaries

0.6.0 does not include:

- dynamic Apps, provider plugin ABI, runtime Page mounting, bare handler
  creation or scripted apps;
- real-time finger-following Page Screen back or custom ZOOM; edge dragging only
  updates the Sys Layer indicator, and only after the release commit does the
  ordinary Page/App back animation decide whether to create a full-resolution
  snapshot;
- static indicator images, LVGL Vector or ThorVG; the arc is generated
  programmatically only by the fixed A8 Canvas;
- a popup/overlay manager; the system task switcher is an App Manager-owned Sys
  Layer interface that is not part of a business overlay framework, and business
  apps must not create or reuse that surface;
- multiple app-manager instances;
- local modifications to ESP-IDF, LVGL or `managed_components/`.

## 16. Recommended Reading Order

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

This module is released under the [MIT License](LICENSE).
