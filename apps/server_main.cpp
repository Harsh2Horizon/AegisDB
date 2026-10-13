#include "server/server.h"
#include <iostream>

int main(int argc, char **argv) {
    try {
        auto options = aegisdb::server::parseOptions(argc, argv);
        auto opened = aegisdb::Database::open(options.directory, options.storage);
        if (!opened) {
            std::cerr << "AegisDB: database open failed (code "
                      << static_cast<int>(opened.status().code) << "): " << opened.status().message
                      << "\n";
            return 1;
        }
        auto db = std::move(opened).value();
        aegisdb::server::DatabaseServer server(*db, std::move(options));
        server.run();
        return 0;
    } catch (const aegisdb::detail::Error &error) {
        std::cerr << "AegisDB: " << error.what() << "\n";
        return 1;
    } catch (const std::exception &error) {
        // Operator logs may contain infrastructure context; HTTP responses remain sanitized.
        std::cerr << "AegisDB: server startup failed: " << error.what() << '\n';
        return 1;
    }
}
