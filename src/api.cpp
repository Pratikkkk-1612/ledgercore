#include <drogon/drogon.h>
#include <functional>

int main()
{
    // Register the health check endpoint.
    drogon::app().registerHandler(
        "/health",

        [](const drogon::HttpRequestPtr &,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback)
        {
            Json::Value response;

            response["status"] = "ok";
            response["service"] = "LedgerCore API";

            auto httpResponse =
                drogon::HttpResponse::newHttpJsonResponse(response);

            callback(httpResponse);
        },

        {drogon::Get}
    );

    // Start the server locally.
    drogon::app().addListener("127.0.0.1", 8080);

    drogon::app().run();

    return 0;
}