# Responsive UI and browser review

Reviewed 2026-10-10 against the shipped HTML/CSS/JavaScript, with a real WASM core and the public RSPCP2VRCP homebrew fixture. This review covers the library, game menu, save/load slots, settings and game presentation. Browser UI compatibility does not establish commercial-game compatibility or physical-device performance.

## Findings and changes

| Area | Original code behavior | Implemented behavior |
| --- | --- | --- |
| Short landscape menus | The panel clipped overflow, but the game-menu page had no scroll container. Reset and Library could become unreachable. | The entire game-menu page scrolls inside the constrained panel. Settings/state bodies flex and scroll below their headers. |
| Narrow settings | Segmented choices used an unwrapped inline flex row; Controller Pak/Rumble Pak/Empty could exceed the available width. | Choices wrap; every enabled choice remains reachable. Tabs divide the available width without imposing intrinsic minimum widths. |
| Short, narrow library | The empty-library two-column layout activated on height alone, including narrow windows. Long adapter names could widen status pills. | The two-column layout also requires sufficient width. Branding truncates, status text wraps and cartridge columns can shrink below their preferred size. |
| Safe areas and browser chrome | Portrait sheets discarded side safe-area padding. The page used the initial percentage viewport height. | Portrait sheets preserve side insets; the body uses dynamic viewport height with the existing percentage-height fallback. Embedded-host inset detection remains in place. |
| High-DPI/ultrawide presentation | Width and height were independently capped at 4096, changing the GPU canvas aspect ratio. | One scale factor limits both dimensions together. Software presentation continues to letterbox to 4:3 unless Stretch is selected. |
| Keyboard/dialog navigation | Dialogs had no accessible name, focus containment, opener restoration or inactive background. Replacement selects lacked radio states and keyboard navigation. | Named dialogs focus their controls, contain Tab/Shift+Tab, restore focus on Back/Close, and make background surfaces inert. Settings tabs and radio groups expose states and support arrow/Home/End navigation. |
| Touch/menu transitions | A held touch could survive opening a menu, and control pointers were not explicitly captured. | Opening a dialog releases guest input; touch controls capture pointers and release them on cancellation/focus loss. |
| Fullscreen | Only the standard API was checked; denied entry closed the menu and silently ignored failure. | Standard and WebKit-prefixed APIs are feature-detected. Unavailable fullscreen is hidden; rejected entry leaves the menu open and explains the failure. |
| Accessibility preferences | Pinch zoom was disabled and menu animation always ran. | Zoom is permitted, reduced-motion preferences are respected, and keyboard focus is visible. |

## Verification matrix

The required workflow builds the app once, then tests that artifact on independent hosted runners. Results are pending the first run of this expanded matrix; this table will be updated with observed versions and outcomes.

| Lane | Coverage | Current evidence |
| --- | --- | --- |
| Local baseline | Built WASM/app; 15 input checks and 42 Node tests, including real-core persistence and high-DPI allocation regression | Focused tests PASS; full baseline running |
| Playwright Chromium | Desktop/mobile lifecycle; responsive viewport matrix | Pending |
| Branded Google Chrome | Lifecycle and responsive matrix, including touch emulation | Pending |
| Branded Microsoft Edge | Lifecycle and responsive matrix, including touch emulation | Pending |
| Playwright WebKit on macOS | Desktop/mobile lifecycle; responsive matrix | Pending |
| Branded Safari on macOS | Native WebDriver clicks/keyboard, constrained windows, settings/state menus, WASM execution, battery/state persistence and cached reload | Pending |
| Physical iOS Safari / Android Chrome | Browser bars, real notch/keyboard, gestures, multitouch, GPU, thermal behavior | Not executed; requires devices |

The responsive matrix uses 15 CSS viewports: 280×653, 320×568, 390×844, 844×390, 568×320, 360×240, 500×500, 768×1024, 1024×768, 320×1024, 1280×720, 1440×900, 640×360, 3440×1440 and 3840×2160. Device pixel ratios range from 1 to 3. Each case visits empty/populated libraries, home settings, game menu, save/load menus, all four settings tabs, rotation with a menu open, and resumed gameplay. The 640×360 case approximates the CSS space available to a 1280×720 desktop at 200% zoom; it does not automate browser zoom itself.

Assertions inspect actual rendered rectangles and clipping ancestors after scrolling controls into view, horizontal overflow, injected safe-area exclusions, focus, pause/resume, radio/tab navigation, fullscreen rejection, page errors and canvas aspect ratio. Mobile contexts use browser-generated touchscreen taps; WebKit's held-input failure injection uses a synthetic PointerEvent because CDP is Chromium-only. CPU profiles are explicitly SKIP in WebKit. Safe-area values are injected for repeatability; that does not test real device `env()` values. Software rendering isolates UI behavior; the GPU backing-store geometry is checked, but device GPU execution is not covered here.

Native Safari records actual CSS viewport dimensions because Safari and the hosted display can clamp requested window sizes. A Playwright WebKit pass is reported separately from branded Safari. Safari-driver startup failure fails its required job; it is not converted into a compatibility pass.

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

On actual iPhone/iPad and Android hardware, repeat library → game → menu → settings → states → resume while rotating, expanding/collapsing browser bars and switching apps. Check touch scrolling, pinch zoom, long-press behavior, simultaneous held controls and menu entry, safe areas, file/ZIP import, audio resume, storage across browser restart and fullscreen/PWA behavior. Check real desktop Safari with keyboard navigation settings, trackpad zoom and an external display. These checks remain necessary before claiming unrestricted support on those devices.

Primary references: [Playwright browser support and WebKit/Safari distinction](https://playwright.dev/docs/browsers), [Safari dynamic viewport units](https://webkit.org/blog/12445/new-webkit-features-in-safari-15-4/), [Apple WebDriver setup](https://developer.apple.com/documentation/safari-developer-tools/macos-enabling-webdriver), [native Safari WebDriver](https://webkit.org/blog/6900/webdriver-support-in-safari-10/), and [iOS WebDriver device requirements](https://webkit.org/blog/9395/webdriver-is-coming-to-safari-in-ios-13/).
