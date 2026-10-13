#include <aegisdb/db.h>
#include <iostream>
int main(int argc, char **argv) {
    auto opened = aegisdb::Database::open(argc > 1 ? argv[1] : "example-data");
    if (!opened) {
        std::cerr << opened.status().message << '\n';
        return 1;
    }
    auto db = std::move(opened).value();
    auto status = db->put("hello", "world");
    if (!status) {
        std::cerr << status.message << '\n';
        return 1;
    }
    auto snapshot = db->createSnapshot();
    if (!snapshot)
        return 1;
    if (!db->put("hello", "new"))
        return 1;
    auto old = db->get("hello", {snapshot.value()});
    if (!old)
        return 1;
    std::cout << old.value() << '\n';
    return 0;
}
