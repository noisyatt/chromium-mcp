// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/emulation_tool.h"

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

EmulationTool::ExecuteContext::ExecuteContext() = default;
EmulationTool::ExecuteContext::~ExecuteContext() = default;

EmulationTool::EmulationTool() = default;
EmulationTool::~EmulationTool() = default;

std::string EmulationTool::name() const {
  return "emulate";
}

std::string EmulationTool::description() const {
  return "디바이스, 위치정보, 미디어 기능 등 에뮬레이션";
}

base::DictValue EmulationTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ------------------------------------------------------------------
  // viewport: 뷰포트 설정 (디바이스 메트릭 오버라이드)
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "object");
    prop.Set("description",
             "뷰포트 설정. width/height/deviceScaleFactor/isMobile/"
             "hasTouch/isLandscape 를 포함할 수 있다");
    base::DictValue vp_props;

    auto make_int_prop = [](const char* desc) {
      base::DictValue p;
      p.Set("type", "integer");
      p.Set("description", desc);
      return p;
    };
    auto make_num_prop = [](const char* desc) {
      base::DictValue p;
      p.Set("type", "number");
      p.Set("description", desc);
      return p;
    };
    auto make_bool_prop = [](const char* desc) {
      base::DictValue p;
      p.Set("type", "boolean");
      p.Set("description", desc);
      return p;
    };

    vp_props.Set("width",             make_int_prop("뷰포트 너비 (픽셀)"));
    vp_props.Set("height",            make_int_prop("뷰포트 높이 (픽셀)"));
    vp_props.Set("deviceScaleFactor", make_num_prop("디바이스 픽셀 비율 (기본값: 1.0)"));
    vp_props.Set("isMobile",          make_bool_prop("모바일 에뮬레이션 여부 (기본값: false)"));
    vp_props.Set("hasTouch",          make_bool_prop("터치 이벤트 에뮬레이션 여부 (기본값: false)"));
    vp_props.Set("isLandscape",       make_bool_prop("가로 모드 여부 (기본값: false)"));

    prop.Set("properties", std::move(vp_props));
    properties.Set("viewport", std::move(prop));
  }

  // ------------------------------------------------------------------
  // geolocation: 위치 정보 에뮬레이션
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "object");
    prop.Set("description", "위치 정보 에뮬레이션 (latitude, longitude, accuracy)");
    base::DictValue geo_props;
    {
      base::DictValue p; p.Set("type", "number"); p.Set("description", "위도 (-90 ~ 90)");
      geo_props.Set("latitude", std::move(p));
    }
    {
      base::DictValue p; p.Set("type", "number"); p.Set("description", "경도 (-180 ~ 180)");
      geo_props.Set("longitude", std::move(p));
    }
    {
      base::DictValue p; p.Set("type", "number"); p.Set("description", "정확도 미터 (기본값: 1.0)");
      geo_props.Set("accuracy", std::move(p));
    }
    prop.Set("properties", std::move(geo_props));
    properties.Set("geolocation", std::move(prop));
  }

  // ------------------------------------------------------------------
  // colorScheme: 다크/라이트 모드 에뮬레이션
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("light");
    enums.Append("dark");
    enums.Append("auto");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "색상 스킴 에뮬레이션: light/dark/auto (auto=에뮬레이션 해제)");
    properties.Set("colorScheme", std::move(prop));
  }

  // ------------------------------------------------------------------
  // timezone: 타임존 에뮬레이션 (예: "Asia/Seoul")
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "타임존 IANA ID (예: \"Asia/Seoul\", \"America/New_York\"). "
             "빈 문자열로 설정 시 기본값 복원");
    properties.Set("timezone", std::move(prop));
  }

  // ------------------------------------------------------------------
  // locale: 로케일 에뮬레이션 (예: "ko-KR")
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "로케일 BCP 47 태그 (예: \"ko-KR\", \"en-US\"). "
             "빈 문자열로 설정 시 기본값 복원");
    properties.Set("locale", std::move(prop));
  }

  // ------------------------------------------------------------------
  // userAgent: 사용자 에이전트 문자열
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "사용자 에이전트 문자열. "
             "빈 문자열로 설정 시 기본 UA 복원");
    properties.Set("userAgent", std::move(prop));
  }

  // ------------------------------------------------------------------
  // cpuThrottling: CPU 속도 배율 (1=비활성, 2=2배 느림, 4=4배 느림 등)
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "CPU 속도 배율. 1=비활성(기본), 2=2배 느림, 4=4배 느림. "
             "Emulation.setCPUThrottlingRate 호출");
    properties.Set("cpuThrottling", std::move(prop));
  }

  // ------------------------------------------------------------------
  // networkConditions: 네트워크 상태 에뮬레이션 (프리셋 문자열)
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("offline");
    enums.Append("slow3g");
    enums.Append("fast3g");
    enums.Append("slow4g");
    enums.Append("fast4g");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "네트워크 조건 프리셋. custom 설정은 networkConditionsCustom 사용");
    properties.Set("networkConditions", std::move(prop));
  }

  // ------------------------------------------------------------------
  // networkConditionsCustom: 커스텀 네트워크 조건 (object)
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "object");
    prop.Set("description",
             "커스텀 네트워크 조건 설정. "
             "downloadThroughput/uploadThroughput(bytes/sec)/latency(ms)");
    base::DictValue nc_props;
    {
      base::DictValue p; p.Set("type", "number");
      p.Set("description", "다운로드 속도 (bytes/sec, -1=무제한)");
      nc_props.Set("downloadThroughput", std::move(p));
    }
    {
      base::DictValue p; p.Set("type", "number");
      p.Set("description", "업로드 속도 (bytes/sec, -1=무제한)");
      nc_props.Set("uploadThroughput", std::move(p));
    }
    {
      base::DictValue p; p.Set("type", "number");
      p.Set("description", "왕복 지연시간 (ms)");
      nc_props.Set("latency", std::move(p));
    }
    prop.Set("properties", std::move(nc_props));
    properties.Set("networkConditionsCustom", std::move(prop));
  }

  // ------------------------------------------------------------------
  // reset: 모든 에뮬레이션 설정 해제
  // ------------------------------------------------------------------
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description",
             "true로 설정하면 모든 에뮬레이션을 기본값으로 초기화한다. "
             "다른 파라미터와 함께 사용 불가");
    properties.Set("reset", std::move(prop));
  }

  schema.Set("properties", std::move(properties));
  return schema;
}

void EmulationTool::Execute(const base::DictValue& arguments,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback) {
  auto ctx = std::make_unique<ExecuteContext>();
  ctx->callback = std::move(callback);

  // ------------------------------------------------------------------
  // reset=true: 모든 에뮬레이션 설정 초기화 (다른 파라미터 무시)
  // ------------------------------------------------------------------
  std::optional<bool> reset = arguments.FindBool("reset");
  if (reset.has_value() && *reset) {
    LOG(INFO) << "[EmulationTool] reset: 모든 에뮬레이션 해제";
    AppendResetCommands(ctx->commands);
    ExecuteNextCommand(std::move(ctx), session);
    return;
  }

  // ------------------------------------------------------------------
  // 1. viewport: Emulation.setDeviceMetricsOverride
  // ------------------------------------------------------------------
  const base::DictValue* viewport = arguments.FindDict("viewport");
  if (viewport) {
    base::DictValue params;
    std::optional<int>    width       = viewport->FindInt("width");
    std::optional<int>    height      = viewport->FindInt("height");
    std::optional<double> dpr         = viewport->FindDouble("deviceScaleFactor");
    std::optional<bool>   is_mobile   = viewport->FindBool("isMobile");
    std::optional<bool>   has_touch   = viewport->FindBool("hasTouch");
    std::optional<bool>   is_landscape = viewport->FindBool("isLandscape");

    params.Set("width",             width.value_or(0));
    params.Set("height",            height.value_or(0));
    params.Set("deviceScaleFactor", dpr.value_or(1.0));
    params.Set("mobile",            is_mobile.value_or(false));
    params.Set("hasTouch",          has_touch.value_or(false));
    params.Set("screenOrientationType",
               is_landscape.value_or(false)
                   ? "landscapePrimary"
                   : "portraitPrimary");

    // 스크린 크기도 뷰포트와 동일하게 맞춘다.
    if (width.has_value() && height.has_value()) {
      base::DictValue screen_size;
      screen_size.Set("width",  *width);
      screen_size.Set("height", *height);
      params.Set("screenSize", std::move(screen_size));
    }

    LOG(INFO) << "[EmulationTool] viewport 추가: "
              << width.value_or(0) << "x" << height.value_or(0);
    ctx->commands.emplace_back("Emulation.setDeviceMetricsOverride",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 2. geolocation: Emulation.setGeolocationOverride
  // ------------------------------------------------------------------
  const base::DictValue* geolocation = arguments.FindDict("geolocation");
  if (geolocation) {
    base::DictValue params;
    std::optional<double> lat = geolocation->FindDouble("latitude");
    std::optional<double> lng = geolocation->FindDouble("longitude");
    std::optional<double> acc = geolocation->FindDouble("accuracy");
    if (lat.has_value()) params.Set("latitude",  *lat);
    if (lng.has_value()) params.Set("longitude", *lng);
    params.Set("accuracy", acc.value_or(1.0));
    LOG(INFO) << "[EmulationTool] geolocation 추가: "
              << lat.value_or(0.0) << ", " << lng.value_or(0.0);
    ctx->commands.emplace_back("Emulation.setGeolocationOverride",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 3. colorScheme: Emulation.setEmulatedMedia (prefers-color-scheme)
  // ------------------------------------------------------------------
  const std::string* color_scheme = arguments.FindString("colorScheme");
  if (color_scheme) {
    base::DictValue params;
    if (*color_scheme == "auto") {
      // auto = 에뮬레이션 해제 (빈 features 목록)
      params.Set("features", base::ListValue());
    } else {
      base::DictValue feature;
      feature.Set("name",  "prefers-color-scheme");
      feature.Set("value", *color_scheme);
      base::ListValue features;
      features.Append(std::move(feature));
      params.Set("features", std::move(features));
    }
    LOG(INFO) << "[EmulationTool] colorScheme 추가: " << *color_scheme;
    ctx->commands.emplace_back("Emulation.setEmulatedMedia",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 4. timezone: Emulation.setTimezoneOverride
  // ------------------------------------------------------------------
  const std::string* timezone = arguments.FindString("timezone");
  if (timezone) {
    base::DictValue params;
    params.Set("timezoneId", *timezone);
    LOG(INFO) << "[EmulationTool] timezone 추가: " << *timezone;
    ctx->commands.emplace_back("Emulation.setTimezoneOverride",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 5. locale: Emulation.setLocaleOverride
  // ------------------------------------------------------------------
  const std::string* locale = arguments.FindString("locale");
  if (locale) {
    base::DictValue params;
    params.Set("locale", *locale);
    LOG(INFO) << "[EmulationTool] locale 추가: " << *locale;
    ctx->commands.emplace_back("Emulation.setLocaleOverride",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 6. userAgent: Emulation.setUserAgentOverride
  // ------------------------------------------------------------------
  const std::string* user_agent = arguments.FindString("userAgent");
  if (user_agent) {
    base::DictValue params;
    params.Set("userAgent", *user_agent);
    LOG(INFO) << "[EmulationTool] userAgent 추가";
    ctx->commands.emplace_back("Emulation.setUserAgentOverride",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 7. cpuThrottling: Emulation.setCPUThrottlingRate
  // ------------------------------------------------------------------
  std::optional<double> cpu_throttling = arguments.FindDouble("cpuThrottling");
  if (cpu_throttling.has_value()) {
    base::DictValue params;
    params.Set("rate", *cpu_throttling);
    LOG(INFO) << "[EmulationTool] cpuThrottling 추가: " << *cpu_throttling;
    ctx->commands.emplace_back("Emulation.setCPUThrottlingRate",
                               std::move(params));
  }

  // ------------------------------------------------------------------
  // 8. networkConditions (프리셋): Network.emulateNetworkConditions
  // ------------------------------------------------------------------
  const std::string* net_conditions = arguments.FindString("networkConditions");
  if (net_conditions) {
    auto preset = GetNetworkPreset(*net_conditions);
    if (preset.has_value()) {
      base::DictValue params;
      params.Set("offline",             preset->offline);
      params.Set("downloadThroughput",  preset->download_throughput);
      params.Set("uploadThroughput",    preset->upload_throughput);
      params.Set("latency",             preset->latency);
      LOG(INFO) << "[EmulationTool] networkConditions 추가: " << *net_conditions;
      ctx->commands.emplace_back("Network.emulateNetworkConditions",
                                 std::move(params));
    } else {
      LOG(WARNING) << "[EmulationTool] 알 수 없는 networkConditions 프리셋: "
                   << *net_conditions;
      ctx->errors.push_back(
          "networkConditions: 알 수 없는 프리셋 '" + *net_conditions + "'");
    }
  }

  // ------------------------------------------------------------------
  // 9. networkConditionsCustom: Network.emulateNetworkConditions (커스텀)
  // ------------------------------------------------------------------
  const base::DictValue* net_custom =
      arguments.FindDict("networkConditionsCustom");
  if (net_custom) {
    base::DictValue params;
    std::optional<double> dl  = net_custom->FindDouble("downloadThroughput");
    std::optional<double> ul  = net_custom->FindDouble("uploadThroughput");
    std::optional<double> lat = net_custom->FindDouble("latency");
    params.Set("offline",            false);
    params.Set("downloadThroughput", dl.value_or(-1.0));
    params.Set("uploadThroughput",   ul.value_or(-1.0));
    params.Set("latency",            lat.value_or(0.0));
    LOG(INFO) << "[EmulationTool] networkConditionsCustom 추가: "
              << "dl=" << dl.value_or(-1.0)
              << " ul=" << ul.value_or(-1.0)
              << " lat=" << lat.value_or(0.0);
    ctx->commands.emplace_back("Network.emulateNetworkConditions",
                               std::move(params));
  }

  // 설정할 항목이 없으면 즉시 오류 반환
  if (ctx->commands.empty() && ctx->errors.empty()) {
    LOG(WARNING) << "[EmulationTool] 설정할 에뮬레이션 파라미터 없음";
    std::move(ctx->callback).Run(MakeErrorResult(
        "설정할 에뮬레이션 파라미터가 없습니다. "
        "viewport/geolocation/colorScheme/timezone/locale/"
        "userAgent/cpuThrottling/networkConditions/reset 중 "
        "하나 이상을 지정하세요"));
    return;
  }

  ExecuteNextCommand(std::move(ctx), session);
}

// static
void EmulationTool::AppendResetCommands(
    std::vector<std::pair<std::string, base::DictValue>>& commands) {
  // 뷰포트 리셋: width=0, height=0, deviceScaleFactor=0 으로 설정하면
  // Chromium이 오버라이드를 해제한다.
  {
    base::DictValue p;
    p.Set("width", 0); p.Set("height", 0);
    p.Set("deviceScaleFactor", 0.0);
    p.Set("mobile", false);
    commands.emplace_back("Emulation.clearDeviceMetricsOverride", std::move(p));
  }
  // 위치 정보 리셋
  commands.emplace_back("Emulation.clearGeolocationOverride",
                        base::DictValue());
  // 색상 스킴 리셋 (빈 features)
  {
    base::DictValue p;
    p.Set("features", base::ListValue());
    commands.emplace_back("Emulation.setEmulatedMedia", std::move(p));
  }
  // 타임존 리셋 (빈 문자열 = 기본값)
  {
    base::DictValue p;
    p.Set("timezoneId", "");
    commands.emplace_back("Emulation.setTimezoneOverride", std::move(p));
  }
  // 로케일 리셋
  {
    base::DictValue p;
    p.Set("locale", "");
    commands.emplace_back("Emulation.setLocaleOverride", std::move(p));
  }
  // UA 리셋
  {
    base::DictValue p;
    p.Set("userAgent", "");
    commands.emplace_back("Emulation.setUserAgentOverride", std::move(p));
  }
  // CPU 스로틀 리셋 (rate=1 = 비활성)
  {
    base::DictValue p;
    p.Set("rate", 1.0);
    commands.emplace_back("Emulation.setCPUThrottlingRate", std::move(p));
  }
  // 네트워크 조건 리셋 (제한 없음)
  {
    base::DictValue p;
    p.Set("offline", false);
    p.Set("downloadThroughput", -1.0);
    p.Set("uploadThroughput",   -1.0);
    p.Set("latency",            0.0);
    commands.emplace_back("Network.emulateNetworkConditions", std::move(p));
  }
}

// static
std::optional<EmulationTool::NetworkPreset>
EmulationTool::GetNetworkPreset(const std::string& preset_name) {
  // 단위: bytes/sec  (Chrome DevTools의 프리셋 기준)
  // slow3g  : 400 kbps down / 400 kbps up / 2000 ms latency
  // fast3g  : 1.5 Mbps down / 750 kbps up / 562.5 ms latency
  // slow4g  : 4 Mbps down   / 3 Mbps up   / 170 ms latency
  // fast4g  : 20 Mbps down  / 10 Mbps up  / 20 ms latency
  if (preset_name == "offline") {
    return NetworkPreset{true, 0.0, 0.0, 0.0};
  }
  if (preset_name == "slow3g") {
    return NetworkPreset{false, 50 * 1024.0, 50 * 1024.0, 2000.0};
  }
  if (preset_name == "fast3g") {
    return NetworkPreset{false, 188 * 1024.0, 94 * 1024.0, 562.0};
  }
  if (preset_name == "slow4g") {
    return NetworkPreset{false, 500 * 1024.0, 375 * 1024.0, 170.0};
  }
  if (preset_name == "fast4g") {
    return NetworkPreset{false, 2500 * 1024.0, 1250 * 1024.0, 20.0};
  }
  return std::nullopt;
}

void EmulationTool::ExecuteNextCommand(std::unique_ptr<ExecuteContext> ctx,
                                       McpSession* session) {
  // 모든 명령 완료 시 최종 결과 반환
  if (ctx->current_index >= ctx->commands.size()) {
    base::DictValue result;
    result.Set("success", ctx->errors.empty());

    base::ListValue applied_list;
    for (const auto& s : ctx->applied) {
      applied_list.Append(s);
    }
    result.Set("applied", std::move(applied_list));

    if (!ctx->errors.empty()) {
      base::ListValue error_list;
      for (const auto& e : ctx->errors) {
        error_list.Append(e);
      }
      result.Set("errors", std::move(error_list));
    }

    LOG(INFO) << "[EmulationTool] 완료: "
              << ctx->applied.size() << "개 성공, "
              << ctx->errors.size() << "개 실패";

    if (ctx->errors.empty()) {
      std::move(ctx->callback).Run(MakeJsonResult(std::move(result)));
    } else {
      // 에러가 있으면 에러 목록을 포함한 JSON 결과로 반환
      std::move(ctx->callback).Run(MakeJsonResult(std::move(result)));
    }
    return;
  }

  // 현재 인덱스의 명령 실행
  auto& [method, params] = ctx->commands[ctx->current_index];
  std::string setting_name = method;
  ctx->current_index++;

  LOG(INFO) << "[EmulationTool] 명령 실행: " << method;

  session->SendCdpCommand(
      method, std::move(params),
      base::BindOnce(&EmulationTool::OnCommandResponse,
                     weak_factory_.GetWeakPtr(), std::move(ctx), session,
                     setting_name));
}

void EmulationTool::OnCommandResponse(std::unique_ptr<ExecuteContext> ctx,
                                      McpSession* session,
                                      const std::string& setting_name,
                                      base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err_dict = response.GetDict().FindDict("error");
    if (err_dict) {
      const std::string* msg = err_dict->FindString("message");
      std::string err_msg = msg ? *msg : "알 수 없는 오류";
      LOG(WARNING) << "[EmulationTool] " << setting_name
                   << " 실패: " << err_msg;
      ctx->errors.push_back(setting_name + ": " + err_msg);
    } else {
      LOG(INFO) << "[EmulationTool] " << setting_name << " 성공";
      ctx->applied.push_back(setting_name);
    }
  } else {
    // 일부 CDP 명령은 빈 응답을 반환한다.
    LOG(INFO) << "[EmulationTool] " << setting_name << " 완료 (빈 응답)";
    ctx->applied.push_back(setting_name);
  }

  ExecuteNextCommand(std::move(ctx), session);
}

}  // namespace mcp
