# 완벽한 헤드리스 크로미움 스텔스 설계 (C++ 소스코드 패치)

## 개요
이 문서는 Chromium-MCP 프로젝트에서 `--headless` 플래그를 사용하여 화면 없이 동작할 때, Cloudflare Turnstile, DataDome 등 고급 봇 탐지 시스템을 우회하기 위해 Chromium의 C++ 소스코드를 직접 수정하는 패치 가이드입니다.
JavaScript 인젝션(Puppeteer-stealth) 방식의 한계를 극복하고, 렌더링 엔진 자체를 "사람이 쓰는 진짜 Mac 브라우저"로 위장합니다.

---

## 1. `navigator.webdriver` 강제 비활성화
Headless 모드나 CDP가 연결되면 자바스크립트에 `navigator.webdriver = true`가 강제 노출됩니다. 이를 소스 레벨에서 영구적으로 차단합니다.

**수정 대상 파일:**
`third_party/blink/renderer/core/frame/navigator_automation_information.cc` (버전에 따라 `navigator.cc`)

**패치 방법:**
`webdriver()` 함수가 무조건 `false`를 반환하도록 하드코딩합니다.
```cpp
// 패치 전
bool NavigatorAutomationInformation::webdriver() const {
  return GetFrame() && GetFrame()->GetSystemHost()->IsWebdriver();
}

// 패치 후
bool NavigatorAutomationInformation::webdriver() const {
  // 항상 false를 반환하여 봇 탐지 우회
  return false;
}
```

---

## 2. User-Agent에서 "Headless" 문자열 제거
Headless 모드 실행 시 `HeadlessChrome/...` 이라는 문자열이 묻어나는 것을 방지합니다.

**수정 대상 파일:**
`components/embedder_support/user_agent_utils.cc` 또는 `content/common/user_agent.cc`

**패치 방법:**
`BuildUserAgentFromProduct` 함수에서 Headless 문자열 조합을 일반 모드와 동일하게 맞춥니다.
```cpp
// 패치 전
std::string BuildUserAgentFromProduct(const std::string& product) {
  ...
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless)) {
    return base::StringPrintf("Mozilla/5.0 (%s) AppleWebKit/537.36 (KHTML, like Gecko) HeadlessChrome/%s Safari/537.36", ...);
  }
}

// 패치 후: 조건문을 지우거나, 일반 크롬과 동일한 문자열을 강제 반환
```

---

## 3. WebGL 및 GPU 렌더러 핑거프린트 속이기 (핵심)
Headless 모드는 소프트웨어 렌더러를 사용하므로 WebGL에서 `Google SwiftShader`가 반환되어 100% 봇으로 차단됩니다. 이를 Apple GPU로 위장합니다.

**수정 대상 파일:**
`gpu/config/gpu_info_collector_mac.mm` (또는 공통 `gpu/config/gpu_info_collector.cc`)

**패치 방법:**
GPU 정보를 수집하는 함수가 끝날 때, 반환되는 `GPUInfo` 구조체의 벤더명과 렌더러명을 강제로 덮어씁니다.
```cpp
// gpu_info_collector_mac.mm 내부 CollectContextGraphicsInfo() 등 
// 또는 GpuInfoCollector가 값을 반환하기 직전

gpu_info->gl_vendor = "Apple";
gpu_info->gl_renderer = "Apple M2 Max"; 
gpu_info->active_gpu().vendor_string = "Apple";
gpu_info->active_gpu().device_string = "Apple M2 Max";
```
*(참고: `gpu/config/gpu_switches.cc` 의 `--gpu-testing-gl-renderer` 플래그를 기본 활성화되게 만드는 방법도 있습니다.)*

---

## 4. `navigator.plugins` 와 `navigator.mimeTypes` 채우기
Headless 브라우저는 플러그인이 0개입니다. 일반 브라우저처럼 기본 플러그인(PDF Viewer 등)이 있는 것처럼 속여야 합니다.

**수정 대상 파일:**
`third_party/blink/renderer/core/page/plugin_data.cc`

**패치 방법:**
`PluginData::UpdatePluginList()` 내부에서, 플러그인 목록이 비어있더라도 강제로 하드코딩된 PDF Viewer 정보를 채워 넣습니다.
```cpp
void PluginData::UpdatePluginList() {
  // 원래 로직을 통해 plugins_ 리스트를 가져온 뒤...
  
  // 패치: 만약 리스트가 비어있거나 Headless 모드라면 가짜 플러그인 주입
  if (plugins_.empty()) {
    auto* pdf_plugin = MakeGarbageCollected<PluginInfo>("Chrome PDF Viewer", "internal-pdf-viewer", "Portable Document Format");
    pdf_plugin->AddMimeType(MakeGarbageCollected<MimeClassInfo>("application/pdf", "pdf", "Portable Document Format"));
    plugins_.push_back(pdf_plugin);
  }
}
```

---

## 5. Hardware Concurrency 및 Device Memory
서버나 도커 환경의 비정상적인 CPU 코어/램 용량이 노출되는 것을 막습니다.

**수정 대상 파일:**
`third_party/blink/renderer/core/frame/navigator_concurrent_hardware.cc` 및 `navigator_device_memory.cc`

**패치 방법:**
표준적인 Mac 환경(예: 8코어, 8GB 램)의 값을 하드코딩합니다.
```cpp
unsigned NavigatorConcurrentHardware::hardwareConcurrency() const {
  return 8; // 항상 8코어로 위장
}

float NavigatorDeviceMemory::deviceMemory() const {
  return 8.0f; // 항상 8GB 램으로 위장
}
```

---

## 빌드 및 적용 (Mac 기준)
위의 C++ 파일들을 수정한 후, Chromium을 다시 컴파일해야 합니다.
```bash
ninja -C out/Default chrome
```
이후 생성된 바이너리를 `--headless` 옵션으로 실행하더라도, 자바스크립트 레벨에서는 완벽한 화면이 있는 `Apple M2` 환경의 진짜 크롬으로 인식됩니다.
