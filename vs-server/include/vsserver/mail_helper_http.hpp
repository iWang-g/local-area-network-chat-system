#pragma once

#include <string>

namespace vsserver {

/// 是否已配置环境变量 `LANCS_MAIL_HELPER_URL`（非空即视为启用 HTTP 通知）。
bool mailHelperConfigured();

/// 未配置 URL 时直接返回 true（不落库回滚由调用方保证仅在成功后提交业务状态）。
/// 已配置时 POST JSON 到辅助服务；失败返回 false 并写入 `errOut`。
bool mailHelperNotifyRegisterCode(const std::string &email, const std::string &code, std::string &errOut);

} // namespace vsserver
