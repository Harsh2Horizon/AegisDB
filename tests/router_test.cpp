#include "server/base64.h"
#include "server/server.h"
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>
using namespace aegisdb;
using namespace aegisdb::server;
TEST(HttpRouter, RealEngineRoutesBinarySnapshotsBatchesAndMaintenance) {
    const auto path =
        std::filesystem::temp_directory_path() / ("aegis-router-" + std::to_string(getpid()));
    std::filesystem::remove_all(path);
    ServerOptions options;
    options.storage.automaticCompaction = false;
    options.storage.levelCount = 3;
    options.maxServerSnapshots = 1;
    auto opened = Database::open(path, options.storage);
    ASSERT_TRUE(opened) << opened.status().message;
    auto db = std::move(opened).value();
    auto now = std::chrono::steady_clock::now();
    {
        ApiRouter router(*db, options, [&] { return now; });
        auto call = [&](const char *path, json::value body = json::object{},
                        http::verb method = http::verb::post) {
            Request request{method, path, 11};
            request.set(http::field::content_type, "application/json");
            request.body() = json::serialize(body);
            auto reply = router.route(request);
            return std::pair{reply.result_int(), json::parse(reply.body())};
        };
        const auto key = base64Encode(std::string("\0\xff", 2));
        EXPECT_EQ(call("/v1/kv/put",
                       json::object{{"key_base64", key}, {"value_base64", base64Encode("old")}})
                      .first,
                  200);
        auto [code, snapshot] = call("/v1/snapshots");
        ASSERT_EQ(code, 201);
        const auto id = snapshot.as_object().at("snapshot_id").as_string();
        EXPECT_EQ(call("/v1/snapshots").first, 429);
        json::array ops{json::object{{"type", "put"},
                                     {"key_base64", key},
                                     {"value_base64", base64Encode("new")}},
                        json::object{{"type", "put"},
                                     {"key_base64", base64Encode("z")},
                                     {"value_base64", base64Encode("Z")}}};
        EXPECT_EQ(call("/v1/kv/batch", json::object{{"operations", ops}}).first, 200);
        auto [getCode, value] =
            call("/v1/kv/get", json::object{{"key_base64", key}, {"snapshot_id", id}});
        ASSERT_EQ(getCode, 200);
        EXPECT_EQ(std::string(value.as_object().at("value_base64").as_string()),
                  base64Encode("old"));
        EXPECT_EQ(call("/v1/kv/get", json::object{{"key_base64", "AB=="}}).first, 400);
        for (auto endpoint : {"/v1/admin/flush", "/v1/admin/compact"}) {
            auto [accepted, job] = call(endpoint);
            ASSERT_EQ(accepted, 202);
            const auto statusPath = "/v1/admin/operations/" +
                                    std::string(job.as_object().at("operation_id").as_string());
            bool done = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                auto [status, state] = call(statusPath.c_str(), json::object{}, http::verb::get);
                ASSERT_EQ(status, 200);
                if (state.as_object().at("state") == "done") {
                    done = true;
                    break;
                }
                ASSERT_NE(state.as_object().at("state"), "failed");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            EXPECT_TRUE(done);
        }
        auto [scanCode, page] = call("/v1/kv/scan", json::object{{"limit", 1}});
        ASSERT_EQ(scanCode, 200);
        EXPECT_EQ(page.as_object().at("entries").as_array().size(), 1);
        EXPECT_FALSE(page.as_object().at("next_after_key_base64").is_null());
        EXPECT_EQ(call("/v1/kv/delete", json::object{{"key_base64", key}}).first, 200);
        EXPECT_FALSE(call("/v1/kv/get", json::object{{"key_base64", key}})
                         .second.as_object()
                         .at("found")
                         .as_bool());
        now += options.snapshotTTL + std::chrono::seconds(1);
        router.expireSnapshots();
        EXPECT_EQ(call("/v1/kv/get", json::object{{"key_base64", key}, {"snapshot_id", id}}).first,
                  404);
        EXPECT_EQ(call("/v1/snapshots").first, 201);
        router.stopAdmission();
        EXPECT_EQ(call("/v1/admin/flush").first, 503);
    }
    db.reset();
    std::filesystem::remove_all(path);
}
