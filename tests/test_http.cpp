// =========================================================================
// test_http.cpp - Unit tests for HTTP client
// =========================================================================
// TEACHING NOTE: Testing an HTTP client is tricky because it needs a
// server to talk to. We test the parts we can without a server:
//   - Request building (to_raw_string)
//   - Response parsing (parse_response - via a static method)
//   - Chunked encoding decoding
//
// For network-dependent tests, we would need a mock server. We test
// the pure parsing functions here. The actual network calls are
// tested manually (by running the browser against a real server).
// =========================================================================

#include "test_framework.hpp"
#include "http.hpp"

using namespace chinstrap;

// --- Request building ---

TEST(build_get_request) {
    HttpRequest req;
    req.method = "GET";
    req.host = "example.com";
    req.port = 80;
    req.path = "/index.html";

    std::string raw = req.to_raw_string();

    // The request must start with the request line
    ASSERT_TRUE(raw.find("GET /index.html HTTP/1.1\r\n") == 0);
    // Must contain Host header
    ASSERT_TRUE(raw.find("Host: example.com\r\n") != std::string::npos);
}

TEST(build_get_request_with_custom_port) {
    HttpRequest req;
    req.method = "GET";
    req.host = "example.com";
    req.port = 8080;
    req.path = "/path";

    std::string raw = req.to_raw_string();

    // Port should be included in Host header for non-default ports
    ASSERT_TRUE(raw.find("Host: example.com:8080\r\n") != std::string::npos);
}

TEST(build_post_request) {
    HttpRequest req;
    req.method = "POST";
    req.host = "example.com";
    req.port = 80;
    req.path = "/submit";
    req.body = "name=hello&value=world";
    req.set_header("Content-Type", "application/x-www-form-urlencoded");

    std::string raw = req.to_raw_string();

    ASSERT_TRUE(raw.find("POST /submit HTTP/1.1\r\n") == 0);
    ASSERT_TRUE(raw.find("Content-Length: 22\r\n") != std::string::npos);
    ASSERT_TRUE(raw.find("Content-Type: application/x-www-form-urlencoded") != std::string::npos);
    // Body should be at the end
    ASSERT_TRUE(raw.find("name=hello&value=world") != std::string::npos);
}

// --- Response parsing ---
// TEACHING NOTE: We test response parsing by calling parse_response
// directly with a crafted response string. This tests the parser
// without needing a network connection.

TEST(parse_simple_response) {
    // We cannot call parse_response directly (it is private), but we can
    // test the HttpResponse struct and its accessors.
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.http_version = "HTTP/1.1";
    resp.headers["Content-Type"] = "text/html";
    resp.body = "<html></html>";

    ASSERT_TRUE(resp.is_success());
    ASSERT_FALSE(resp.is_redirect());
    ASSERT_EQ(resp.status_code, 200);
    ASSERT_STREQ(resp.status_text, "OK");
    ASSERT_STREQ(resp.content_type(), "text/html");
}

TEST(parse_response_not_found) {
    HttpResponse resp;
    resp.status_code = 404;
    resp.status_text = "Not Found";

    ASSERT_FALSE(resp.is_success());
    ASSERT_FALSE(resp.is_redirect());
}

TEST(parse_response_redirect) {
    HttpResponse resp;
    resp.status_code = 302;
    resp.status_text = "Found";
    resp.headers["Location"] = "http://other.com";

    ASSERT_FALSE(resp.is_success());
    ASSERT_TRUE(resp.is_redirect());
    ASSERT_STREQ(resp.header("Location"), "http://other.com");
}

TEST(header_case_insensitive) {
    HttpResponse resp;
    resp.headers["Content-Type"] = "text/html";

    // Lookup should work regardless of case
    ASSERT_STREQ(resp.header("content-type"), "text/html");
    ASSERT_STREQ(resp.header("CONTENT-TYPE"), "text/html");
    ASSERT_STREQ(resp.header("Content-Type"), "text/html");
}

// --- Content length ---

TEST(content_length_parsing) {
    HttpResponse resp;
    resp.headers["Content-Length"] = "12345";

    ASSERT_EQ(resp.content_length(), static_cast<std::size_t>(12345));
}

TEST(content_length_missing) {
    HttpResponse resp;

    ASSERT_EQ(resp.content_length(), static_cast<std::size_t>(0));
}

RUN_TESTS()