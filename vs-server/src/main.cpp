#include "vsserver/database.hpp"
#include "vsserver/logger.hpp"
#include "vsserver/protocol.hpp"
#include "vsserver/tcp_server.hpp"

#include <cstdlib>
#include <string>

int main(int argc, char *argv[])
{
    vsserver::Logger::init();

    std::string dbErr;
    if (!vsserver::AppDatabase::initialize(dbErr)) {
        VSLOG_ERROR("数据库初始化失败: " + dbErr);
    } else {
        VSLOG_INFO("数据库已就绪 (data/chat.db)");
    }

    std::uint16_t port = vsserver::kDefaultTcpPort;
    if (argc >= 2) {
        const int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) {
            port = static_cast<std::uint16_t>(p);
        } else {
            VSLOG_WARN(std::string("invalid port, using default ")
                       + std::to_string(vsserver::kDefaultTcpPort));
        }
    }

    const int code = vsserver::runTcpServer(port);
    vsserver::AppDatabase::shutdown();
    vsserver::Logger::shutdown();
    return code;
}
