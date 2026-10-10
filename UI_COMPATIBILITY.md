# Responsive UI and browser review

Reviewed 2026-10-10 against the shipped HTML/CSS/JavaScript, with a real WASM core and the public RSPCP2VRCP homebrew fixture. This review covers the library, game menu, save/load slots, settings and game presentation. Browser UI compatibility does not establish commercial-game compatibility or physical-device performance.

## Findings and changes

| Area | Original code behavior | Implemented behavior |
| --- | --- | --- |
| Short landscape menus | The panel clipped overflow, but the game-menu page had no scroll container. Reset and Library could become unreachable. | The entire game-menu page scrolls inside the constrained panel. Settings/state bodies flex and scroll below their headers. |
| Very short state menus | WebKit preserved tall thumbnail cards at 360×240; a complete slot could not fit beneath the fixed header, even after scrolling. | At heights up to 360px, slots use a compact layout with a 40px thumbnail beside their text and a minimum 64px height. |
| Narrow settings | Segmented choices used an unwrapped inline flex row; Controller Pak/Rumble Pak/Empty could exceed the available width. | Choices wrap; every enabled choice remains reachable. Tabs divide the available width without imposing intrinsic minimum widths. |
| Short, narrow library | The empty-library two-column layout activated on height alone, including narrow windows. Long adapter names could widen status pills. | The two-column layout also requires sufficient width. The SVG branding scales uniformly to fit beside Settings, status text wraps and cartridge columns can shrink below their preferred size. |
| Safe areas and browser chrome | Portrait sheets discarded side safe-area padding. The page used the initial percentage viewport height. | Portrait sheets preserve side insets; the body uses dynamic viewport height with the existing percentage-height fallback. Embedded-host inset detection remains in place. |
| High-DPI/ultrawide presentation | Width and height were independently capped at 4096, changing the GPU canvas aspect ratio. | One scale factor limits both dimensions together. Software presentation continues to letterbox to 4:3 unless Stretch is selected. |
| Keyboard/dialog navigation | Dialogs had no accessible name, focus containment, opener restoration or inactive background. Replacement selects lacked radio states and keyboard navigation. | Named dialogs focus their controls, contain Tab/Shift+Tab, restore focus on Back/Close, and make background surfaces inert. Settings tabs and radio groups expose states and support arrow/Home/End navigation. |
| Safari focus restoration | A clicked Settings button did not necessarily become `activeElement`, so recording that element restored the wrong control on Back. | Navigation records the explicit button that opened each page. Native Safari and WebKit reproduce and verify this case. |
| Touch/menu transitions | A held touch could survive opening a menu, and control pointers were not explicitly captured. | Opening a dialog releases guest input; touch controls capture pointers and release them on cancellation/focus loss. |
| Fullscreen | Only the standard API was checked; denied entry closed the menu and silently ignored failure. | Standard and WebKit-prefixed APIs are feature-detected. Unavailable fullscreen is hidden; rejected entry leaves the menu open and explains the failure. |
| Accessibility preferences | Pinch zoom was disabled and menu animation always ran. | Zoom is permitted, reduced-motion preferences are respected, and keyboard focus is visible. |
| Startup notifications | A long game-ready notification could cover menu labels immediately after opening a cartridge. | Opening a sheet dismisses that redundant notification. Storage/renderer warnings remain visible, and notifications expose a polite live region. |

## How the interface scales

CSS pixels determine menu size; display pixel density determines canvas allocation separately.

| Layout condition | Behavior |
| --- | --- |
| Portrait width ≤560px | A bottom sheet uses the width between side safe areas. Settings height is capped at 680px and 86% of the sheet's available height. Bottom padding clears the home indicator. |
| Width ≥620px and height ≤520px | The menu can widen to 660px; action tiles become one row beneath Resume, and state slots use four columns. |
| Height ≤360px | Compact state cards retain a thumbnail and labels; controls remain reachable by scrolling. |
| Other tablet/desktop windows | Centered game/state panels cap at 470px; settings cap at 600px wide and 640px high. Available viewport height still constrains each panel. |
| Library | Content caps at 980px. The grid adds columns as space permits, preferring 148px cartridges with 14px gaps; narrow windows scroll vertically. |
| Game image | Default 4:3 is centered with bars when needed; Stretch fills the picture area. Portrait touch layouts reserve pad space when it fits; landscape layouts overlay the pad. |
| High-DPI and large displays | CSS dimensions remain stable. GPU presentation uses DPR up to 3, reduced uniformly when either backing-store dimension would exceed 4096px. Native/2×/4× rendering resolution is a separate setting. |

## Verification matrix

The required workflow builds the app once, then tests that artifact on independent hosted runners. [Actions run 38074459992](https://github.com/PowerBeef/Photon64/actions/runs/38074459992) completed successfully for `f361f9e60030de51f9568af3553091da515990be`: all eight jobs PASS. The four responsive lanes each pass 240 phase checks across 15 viewports (960 total), and native Safari passes 33 phase checks across three actual window viewports. All six Playwright lifecycle lanes pass with no page errors. The table below records observed versions and bounded coverage.

| Lane | Coverage | Current evidence |
| --- | --- | --- |
| Local and hosted baseline | Built WASM/app; 15 input checks and 42 Node tests, including real-core persistence and high-DPI allocation regression; full native/WASM/reference baseline | PASS |
| Playwright Chromium 153.0.8010.12 on Linux | Desktop/mobile lifecycle; 15-viewport responsive matrix | PASS |
| Branded Google Chrome 155.0.8059.39 on Linux | Lifecycle and 15-viewport matrix, including touch emulation | PASS |
| Branded Microsoft Edge 155.0.4283.45 on Linux | Lifecycle and 15-viewport matrix, including touch emulation | PASS |
| Playwright WebKit 26.6 on macOS 15.7.9 | Desktop/mobile lifecycle; 15-viewport responsive matrix | PASS |
| Branded Safari 26.6.1 on macOS 15.7.9 | Native WebDriver clicks/keys; actual CSS viewports 390×792, 1000×448 and 1000×688; File import, WASM, state-menu actions, battery persistence and cached reload | PASS for UI/session tests; OS-backed file probe SKIP, described below |
| Physical iOS Safari / Android Chrome | Browser bars, real notch/keyboard, gestures, multitouch, GPU, thermal behavior | Not executed; requires devices |

The responsive matrix uses 15 CSS viewports: 280×653, 320×568, 390×844, 844×390, 568×320, 360×240, 500×500, 768×1024, 1024×768, 320×1024, 1280×720, 1440×900, 640×360, 3440×1440 and 3840×2160. Device pixel ratios range from 1 to 3. Each case visits empty/populated libraries, home settings, game menu, all four save/load slots, all four settings tabs, rotation with a menu open, 4:3/Stretch presentation and resumed gameplay. Mobile and short-window cases also exercise 70%/130% pad sizes, minimum/maximum pad height and D-pad visibility. Slot checks wait for asynchronous storage metadata before measuring; disabled load slots are checked too. The 640×360 case approximates the CSS space available to a 1280×720 desktop at 200% zoom; it does not automate browser zoom itself.

Assertions inspect actual rendered rectangles and clipping ancestors after scrolling controls into view, horizontal overflow, injected safe-area exclusions, focus, pause/resume, radio/tab navigation, fullscreen rejection, page errors and canvas aspect ratio. Mobile contexts use browser-generated touchscreen taps, including saving/loading a state through its menu slot. The responsive held-input injection uses a synthetic PointerEvent; lifecycle Chromium also uses CDP touch input. CPU profiles are explicitly SKIP in WebKit. Safe-area values are injected for repeatability; that does not test real device `env()` values. Software rendering isolates UI behavior; the GPU backing-store geometry is checked, but device GPU execution is not covered here. Game controls retain `touch-action: none` for controller gestures; menus and the library permit browser zoom.

Native Safari records actual CSS viewport dimensions because Safari and the hosted display can clamp requested window sizes. A Playwright WebKit pass is reported separately from branded Safari. Safari-driver startup failure fails its required job; it is not converted into a compatibility pass.

The native Safari runner reports `NotReadableError: The I/O read operation failed` when WebDriver supplies an OS-backed file. An independent hidden file input with **no app handler and no selection clearing** reproduces the same failure, despite reporting the correct 1,052,672-byte file size. The exact driver/permission cause is not established. This OS-backed file probe is recorded as **SKIP**, not PASS. The Safari UI lane then fetches the public fixture from its test server, constructs a browser-backed `File`, and supplies it through the app's unchanged file-input/change/`openFile` path. That import, actual WASM execution, native clicks/keys, state-slot save/load and persistent reload all pass. This does not validate the native OS chooser or its filesystem permissions; those still need a manual desktop Safari check. App import failures on readable files continue to fail the lane.

## Reproduce and inspect evidence

```sh
tools/setup.sh
. out/env.sh
npm run build
npm test
npx playwright install --with-deps chromium webkit
BROWSER_ENGINE=chromium node tools/responsivecheck.mjs
BROWSER_ENGINE=webkit node tools/responsivecheck.mjs
BROWSER_ENGINE=webkit BROWSER_MOBILE=1 node tools/browsercheck.mjs testroms/RSPCP2VRCP.N64
# macOS, after enabling Safari Remote Automation:
node tools/safaricheck.mjs
```

Use an independent browser runner as required by [AGENTS.md](AGENTS.md). For fresh hosted runners, install `chrome`/`msedge` and set `BROWSER_CHANNEL` accordingly. The workflow retains `result.json`, screenshots, lifecycle traces and Chromium CPU profiles as artifacts. The Safari harness uses Apple's bundled driver and Node's HTTP APIs, without an extra automation dependency.

## Remaining device checks

On actual iPhone/iPad and Android hardware, repeat library → game → menu → settings → states → resume while rotating, expanding/collapsing browser bars and switching apps. Check touch scrolling, pinch zoom, long-press behavior, simultaneous held controls and menu entry, safe areas, file/ZIP import, audio resume, storage across browser restart and fullscreen/PWA behavior. Check the native desktop Safari OS file chooser, keyboard navigation settings, trackpad zoom and an external display. Also check Windows/macOS Chrome and Edge with their native fonts and mixed-DPI displays; the branded Chromium jobs currently use Linux. These checks remain necessary before claiming unrestricted support on those devices.

Primary references: [Playwright browser support and WebKit/Safari distinction](https://playwright.dev/docs/browsers), [Safari dynamic viewport units](https://webkit.org/blog/12445/new-webkit-features-in-safari-15-4/), [Apple WebDriver setup](https://developer.apple.com/documentation/safari-developer-tools/macos-enabling-webdriver), [native Safari WebDriver](https://webkit.org/blog/6900/webdriver-support-in-safari-10/), and [iOS WebDriver device requirements](https://webkit.org/blog/9395/webdriver-is-coming-to-safari-in-ios-13/).
