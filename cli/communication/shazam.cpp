#include "communication/shazam.h"
#include <curl/curl.h>
#include <algorithm>
#include <random>
#include <sstream>
#include <cstring>
#include <map>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include "communication/timezones.h"
#include "communication/user_agents.h"
#include "utils/uuid4.h"
#include "../../include/vibra.h"

// static variables initialization
constexpr char Shazam::HOST[];

std::size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    std::string *buffer = reinterpret_cast<std::string *>(userp);
    std::size_t realsize = size * nmemb;
    buffer->append(reinterpret_cast<char *>(contents), realsize);
    return realsize;
}

std::string Shazam::Recognize(const Fingerprint *fingerprint, const std::string& proxy)
{
    auto content = getRequestContent(fingerprint->uri, fingerprint->sample_ms);
    auto user_agent = getUserAgent();
    std::string url = getShazamHost();

    CURL *curl = curl_easy_init();
    std::string read_buffer;

    if (curl)
    {
        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Accept-Encoding: gzip, deflate, br");
        headers = curl_slist_append(headers, "Accept: */*");
        headers = curl_slist_append(headers, "Connection: keep-alive");
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Content-Language: en_US");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);

        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

        // Configure proxy if provided
        if (!proxy.empty())
        {
            // Parse proxy format: [type://][user:pass@]host:port
            std::string proxy_str = proxy;
            curl_proxytype proxy_type = CURLPROXY_HTTP;

            // Detect proxy type from URL scheme
            if (proxy_str.find("socks5://") == 0)
            {
                proxy_type = CURLPROXY_SOCKS5;
                proxy_str = proxy_str.substr(9); // Remove "socks5://"
            }
            else if (proxy_str.find("http://") == 0)
            {
                proxy_str = proxy_str.substr(7); // Remove "http://"
            }

            // Extract authentication if present (user:pass@)
            std::string auth;
            size_t at_pos = proxy_str.find('@');
            if (at_pos != std::string::npos)
            {
                auth = proxy_str.substr(0, at_pos);
                proxy_str = proxy_str.substr(at_pos + 1);
            }

            // Set proxy options
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);

            if (!auth.empty())
            {
                curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
            }
        }

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }

        std::int64_t http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200)
        {
            std::cerr << "HTTP code: " << http_code << std::endl;
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return read_buffer;
}

std::string Shazam::getShazamHost()
{
    std::string host = HOST + uuid4::generate() + "/" + uuid4::generate();
    host += "?sync=true&"
            "webv3=true&"
            "sampling=true&"
            "connected=&"
            "shazamapiversion=v3&"
            "sharehub=true&"
            "video=v3";
    return host;
}

std::string Shazam::getRequestContent(const std::string &uri, unsigned int sample_ms)
{
    std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<double> dis_float(0.0, 1.0);

    auto timezone = getTimezone();
    double fuzz = dis_float(gen) * 15.3 - 7.65;

    std::stringstream json_buf;
    json_buf << "{";
    json_buf << "\"geolocation\":{";
    json_buf << "\"altitude\":" << dis_float(gen) * 400 + 100 + fuzz << ",";
    json_buf << "\"latitude\":" << dis_float(gen) * 180 - 90 + fuzz << ",";
    json_buf << "\"longitude\":" << dis_float(gen) * 360 - 180 + fuzz;
    json_buf << "},";
    json_buf << "\"signature\":{";
    json_buf << "\"samplems\":" << sample_ms << ",";
    json_buf << "\"timestamp\":" << time(nullptr) * 1000ULL << ",";
    json_buf << "\"uri\":\"" << uri << "\"";
    json_buf << "},";
    json_buf << "\"timestamp\":" << time(nullptr) * 1000ULL << ",";
    json_buf << "\"timezone\":"
             << "\"" << timezone << "\"";
    json_buf << "}";
    std::string content = json_buf.str();
    return content;
}

std::string Shazam::getUserAgent()
{
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis_useragent(0, USER_AGENTS_SIZE - 1);
    return USER_AGENTS[dis_useragent(gen)];
}

std::string Shazam::getTimezone()
{
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis_timezone(0, EUROPE_TIMEZONES_SIZE - 1);
    return EUROPE_TIMEZONES[dis_timezone(gen)];
}

std::string Shazam::FetchExitIP(const std::string& proxy)
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        return "";
    }

    std::string read_buffer;

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.country.is");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    // Configure proxy if provided
    if (!proxy.empty())
    {
        std::string proxy_str = proxy;
        curl_proxytype proxy_type = CURLPROXY_HTTP;

        if (proxy_str.find("socks5://") == 0)
        {
            proxy_type = CURLPROXY_SOCKS5;
            proxy_str = proxy_str.substr(9);
        }
        else if (proxy_str.find("http://") == 0)
        {
            proxy_str = proxy_str.substr(7);
        }

        std::string auth;
        size_t at_pos = proxy_str.find('@');
        if (at_pos != std::string::npos)
        {
            auth = proxy_str.substr(0, at_pos);
            proxy_str = proxy_str.substr(at_pos + 1);
        }

        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);

        if (!auth.empty())
        {
            curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
        }
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        return "";
    }

    // Parse IP from response: {"ip":"x.x.x.x","country":"XX"}
    size_t ip_start = read_buffer.find("\"ip\":\"");
    if (ip_start != std::string::npos)
    {
        ip_start += 6;
        size_t ip_end = read_buffer.find("\"", ip_start);
        if (ip_end != std::string::npos)
        {
            return read_buffer.substr(ip_start, ip_end - ip_start);
        }
    }

    return "";
}

std::string Shazam::extractTrackId(const std::string& response)
{
    // Find "key":"XXXXX" in track object
    size_t track_pos = response.find("\"track\":");
    if (track_pos == std::string::npos) return "";

    size_t key_pos = response.find("\"key\":\"", track_pos);
    if (key_pos == std::string::npos) return "";

    key_pos += 7;
    size_t key_end = response.find("\"", key_pos);
    if (key_end == std::string::npos) return "";

    return response.substr(key_pos, key_end - key_pos);
}

std::string Shazam::extractTitle(const std::string& response)
{
    size_t track_pos = response.find("\"track\":");
    if (track_pos == std::string::npos) return "";

    size_t title_pos = response.find("\"title\":\"", track_pos);
    if (title_pos == std::string::npos) return "";

    title_pos += 9;
    size_t title_end = response.find("\"", title_pos);
    if (title_end == std::string::npos) return "";

    return response.substr(title_pos, title_end - title_pos);
}

std::string Shazam::extractArtist(const std::string& response)
{
    size_t track_pos = response.find("\"track\":");
    if (track_pos == std::string::npos) return "";

    size_t artist_pos = response.find("\"subtitle\":\"", track_pos);
    if (artist_pos == std::string::npos) return "";

    artist_pos += 12;
    size_t artist_end = response.find("\"", artist_pos);
    if (artist_end == std::string::npos) return "";

    return response.substr(artist_pos, artist_end - artist_pos);
}

int Shazam::extractMatchCount(const std::string& response)
{
    int count = 0;
    size_t pos = 0;
    while ((pos = response.find("\"id\":", pos)) != std::string::npos) {
        count++;
        pos += 5;
    }
    // Subtract 1 for each non-match "id" (like artist id)
    // Actually, just count matches array entries
    count = 0;
    pos = response.find("\"matches\":[");
    if (pos != std::string::npos) {
        size_t end = response.find("]", pos);
        std::string matches_str = response.substr(pos, end - pos);
        pos = 0;
        while ((pos = matches_str.find("{\"id\":", pos)) != std::string::npos) {
            count++;
            pos += 6;
        }
    }
    return count;
}

std::string Shazam::RecognizePrecise(const std::vector<Fingerprint*>& fingerprints, const std::string& proxy)
{
    std::vector<SegmentResult> results;

    // Create single CURL handle to reuse for all requests (like a real app)
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        return "{\"error\":\"Failed to initialize CURL\"}";
    }

    // Setup headers once
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept-Encoding: gzip, deflate, br");
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "Connection: keep-alive");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Content-Language: en_US");

    // Set common options
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    // Configure proxy if provided
    if (!proxy.empty())
    {
        std::string proxy_str = proxy;
        curl_proxytype proxy_type = CURLPROXY_HTTP;

        if (proxy_str.find("socks5://") == 0)
        {
            proxy_type = CURLPROXY_SOCKS5;
            proxy_str = proxy_str.substr(9);
        }
        else if (proxy_str.find("http://") == 0)
        {
            proxy_str = proxy_str.substr(7);
        }

        std::string auth;
        size_t at_pos = proxy_str.find('@');
        if (at_pos != std::string::npos)
        {
            auth = proxy_str.substr(0, at_pos);
            proxy_str = proxy_str.substr(at_pos + 1);
        }

        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);

        if (!auth.empty())
        {
            curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
        }
    }

    // Get consistent user agent for all requests
    auto user_agent = getUserAgent();
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

    // Process fingerprints sequentially, reusing connection
    for (size_t i = 0; i < fingerprints.size(); i++) {
        std::string read_buffer;
        std::string url = getShazamHost();
        auto content = getRequestContent(fingerprints[i]->uri, fingerprints[i]->sample_ms);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, content.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }

        SegmentResult result;
        result.offset_ms = fingerprints[i]->offset_ms;
        result.response = read_buffer;
        result.track_id = extractTrackId(read_buffer);
        result.title = extractTitle(read_buffer);
        result.artist = extractArtist(read_buffer);
        result.match_count = extractMatchCount(read_buffer);

        results.push_back(result);

        // Check for early termination conditions
        if (results.size() >= 2) {
            // Check for 2 consecutive matches
            if (!result.track_id.empty() &&
                result.track_id == results[results.size() - 2].track_id) {
                // Return confident match
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);

                std::string final_response = result.response;
                size_t last_brace = final_response.rfind('}');
                if (last_brace != std::string::npos) {
                    std::string extra = ",\"vibra_segments_checked\":" + std::to_string(results.size()) +
                                       ",\"vibra_confident\":true";
                    final_response.insert(last_brace, extra);
                }
                return final_response;
            }

            // Check for 2 out of 3 (or more) same matches
            if (results.size() >= 3) {
                std::map<std::string, int> vote_count;
                std::map<std::string, size_t> track_index;

                for (size_t j = 0; j < results.size(); j++) {
                    if (!results[j].track_id.empty()) {
                        vote_count[results[j].track_id]++;
                        track_index[results[j].track_id] = j;
                    }
                }

                // Find if any track has 2+ votes
                for (const auto& pair : vote_count) {
                    if (pair.second >= 2) {
                        // Return confident match
                        curl_slist_free_all(headers);
                        curl_easy_cleanup(curl);

                        size_t idx = track_index[pair.first];
                        std::string final_response = results[idx].response;
                        size_t last_brace = final_response.rfind('}');
                        if (last_brace != std::string::npos) {
                            std::string extra = ",\"vibra_segments_checked\":" + std::to_string(results.size()) +
                                               ",\"vibra_confident\":true";
                            final_response.insert(last_brace, extra);
                        }
                        return final_response;
                    }
                }
            }
        }
    }

    // Cleanup
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // No confident match found - return ambiguous result
    std::stringstream json;
    json << "{\"matches\":[],\"vibra_segments_checked\":" << results.size();
    json << ",\"vibra_confident\":false,\"vibra_ambiguous\":[";

    bool first = true;
    for (const auto& r : results) {
        if (!r.track_id.empty()) {
            if (!first) json << ",";
            first = false;
            json << "{\"offset_ms\":" << r.offset_ms;
            json << ",\"track_id\":\"" << r.track_id << "\"";
            json << ",\"title\":\"" << r.title << "\"";
            json << ",\"artist\":\"" << r.artist << "\"";
            json << ",\"match_count\":" << r.match_count << "}";
        }
    }

    json << "]}";
    return json.str();
}

std::string Shazam::extractAppleMusicId(const std::string& response)
{
    // Find "applemusicplay" action with id
    size_t pos = response.find("\"type\":\"applemusicplay\"");
    if (pos == std::string::npos) return "";

    size_t id_pos = response.find("\"id\":\"", pos);
    if (id_pos == std::string::npos) return "";

    id_pos += 6;
    size_t id_end = response.find("\"", id_pos);
    if (id_end == std::string::npos) return "";

    return response.substr(id_pos, id_end - id_pos);
}

std::string Shazam::FetchAppleMusicMetadata(const std::string& response, const std::string& proxy)
{
    std::string apple_id = extractAppleMusicId(response);
    if (apple_id.empty())
    {
        return response;
    }

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        return response;
    }

    std::string read_buffer;
    std::string url = "https://music.apple.com/song/" + apple_id;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");

    // Configure proxy if provided
    if (!proxy.empty())
    {
        std::string proxy_str = proxy;
        curl_proxytype proxy_type = CURLPROXY_HTTP;

        if (proxy_str.find("socks5://") == 0)
        {
            proxy_type = CURLPROXY_SOCKS5;
            proxy_str = proxy_str.substr(9);
        }
        else if (proxy_str.find("http://") == 0)
        {
            proxy_str = proxy_str.substr(7);
        }

        std::string auth;
        size_t at_pos = proxy_str.find('@');
        if (at_pos != std::string::npos)
        {
            auth = proxy_str.substr(0, at_pos);
            proxy_str = proxy_str.substr(at_pos + 1);
        }

        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);

        if (!auth.empty())
        {
            curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
        }
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        return response;
    }

    // Find schema:song JSON-LD
    size_t schema_start = read_buffer.find("<script id=\"schema:song\" type=\"application/ld+json\">");
    if (schema_start == std::string::npos)
    {
        // Try alternate format
        schema_start = read_buffer.find("<script id=schema:song type=\"application/ld+json\">");
        if (schema_start == std::string::npos)
        {
            return response;
        }
        schema_start += 50;
    }
    else
    {
        schema_start += 52;
    }

    size_t schema_end = read_buffer.find("</script>", schema_start);
    if (schema_end == std::string::npos)
    {
        return response;
    }

    std::string schema_json = read_buffer.substr(schema_start, schema_end - schema_start);

    // Extract fields from schema.org JSON
    std::string apple_metadata;

    // Extract datePublished
    size_t date_pos = schema_json.find("\"datePublished\":\"");
    if (date_pos != std::string::npos)
    {
        date_pos += 17;
        size_t date_end = schema_json.find("\"", date_pos);
        if (date_end != std::string::npos)
        {
            apple_metadata += ",\"apple_release_date\":\"" + schema_json.substr(date_pos, date_end - date_pos) + "\"";
        }
    }

    // Extract duration (timeRequired or duration)
    size_t dur_pos = schema_json.find("\"duration\":\"PT");
    if (dur_pos != std::string::npos)
    {
        dur_pos += 13;
        size_t dur_end = schema_json.find("\"", dur_pos);
        if (dur_end != std::string::npos)
        {
            apple_metadata += ",\"apple_duration\":\"" + schema_json.substr(dur_pos, dur_end - dur_pos) + "\"";
        }
    }

    // Extract genre array
    size_t genre_pos = schema_json.find("\"genre\":[");
    if (genre_pos != std::string::npos)
    {
        genre_pos += 8;
        size_t genre_end = schema_json.find("]", genre_pos);
        if (genre_end != std::string::npos)
        {
            apple_metadata += ",\"apple_genres\":" + schema_json.substr(genre_pos, genre_end - genre_pos + 1);
        }
    }

    // Extract preview URL (contentUrl)
    size_t preview_pos = schema_json.find("\"contentUrl\":\"");
    if (preview_pos != std::string::npos)
    {
        preview_pos += 14;
        size_t preview_end = schema_json.find("\"", preview_pos);
        if (preview_end != std::string::npos)
        {
            apple_metadata += ",\"apple_preview_url\":\"" + schema_json.substr(preview_pos, preview_end - preview_pos) + "\"";
        }
    }

    // Extract album name
    size_t album_pos = schema_json.find("\"inAlbum\":");
    if (album_pos != std::string::npos)
    {
        size_t album_name_pos = schema_json.find("\"name\":\"", album_pos);
        if (album_name_pos != std::string::npos)
        {
            album_name_pos += 8;
            size_t album_name_end = schema_json.find("\"", album_name_pos);
            if (album_name_end != std::string::npos)
            {
                apple_metadata += ",\"apple_album\":\"" + schema_json.substr(album_name_pos, album_name_end - album_name_pos) + "\"";
            }
        }
    }

    // Extract byArtist array
    size_t by_artist_pos = schema_json.find("\"byArtist\":[");
    if (by_artist_pos != std::string::npos)
    {
        size_t arr_end = schema_json.find("]", by_artist_pos);
        if (arr_end != std::string::npos)
        {
            // Find only the first byArtist array (track level, not album level)
            apple_metadata += ",\"apple_artists\":" + schema_json.substr(by_artist_pos + 11, arr_end - by_artist_pos - 10);
        }
    }

    // Extract high-res image
    size_t img_pos = schema_json.find("\"image\":\"https://");
    if (img_pos != std::string::npos)
    {
        img_pos += 9;
        size_t img_end = schema_json.find("\"", img_pos);
        if (img_end != std::string::npos)
        {
            apple_metadata += ",\"apple_image\":\"" + schema_json.substr(img_pos, img_end - img_pos) + "\"";
        }
    }

    if (apple_metadata.empty())
    {
        return response;
    }

    // Insert apple metadata into response
    std::string result = response;
    size_t last_brace = result.rfind('}');
    if (last_brace != std::string::npos)
    {
        result.insert(last_brace, apple_metadata);
    }

    return result;
}

// Helper to extract string between quotes after a key
static std::string extractJsonString(const std::string& json, const std::string& key, size_t start_pos = 0)
{
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search, start_pos);
    if (pos == std::string::npos) return "";

    pos += search.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";

    return json.substr(pos, end - pos);
}

// Helper to extract number after a key
static std::string extractJsonNumber(const std::string& json, const std::string& key, size_t start_pos = 0)
{
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search, start_pos);
    if (pos == std::string::npos) return "";

    pos += search.length();
    size_t end = pos;
    while (end < json.length() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-' || json[end] == 'E' || json[end] == 'e' || json[end] == '+'))
    {
        end++;
    }

    return json.substr(pos, end - pos);
}

// Helper to escape JSON string
static std::string escapeJson(const std::string& s)
{
    std::string result;
    for (char c : s)
    {
        switch (c)
        {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

std::string Shazam::BuildUnifiedResponse(const std::string& response)
{
    std::stringstream unified;

    // Check if we have a track
    size_t track_pos = response.find("\"track\":");
    if (track_pos == std::string::npos)
    {
        // No match found
        unified << "{\"status\":\"no_match\",\"result\":null}";
        return unified.str();
    }

    unified << "{\"status\":\"success\",\"result\":{";

    // === Core Metadata ===
    std::string title = extractJsonString(response, "title", track_pos);
    std::string full_artist = extractJsonString(response, "subtitle", track_pos);
    std::string key = extractJsonString(response, "key", track_pos);
    std::string isrc = extractJsonString(response, "isrc", track_pos);
    std::string genre = extractJsonString(response, "primary", track_pos);
    std::string albumadamid = extractJsonString(response, "albumadamid", track_pos);

    unified << "\"title\":\"" << escapeJson(title) << "\"";
    unified << ",\"full_artist\":\"" << escapeJson(full_artist) << "\"";

    // Extract individual artists from Apple Music apple_artists if available
    std::string main_artist;
    std::vector<std::string> feat_artists;

    // Look for apple_artists array (injected from Apple Music)
    size_t by_artist_pos = response.find("\"apple_artists\":[");
    if (by_artist_pos != std::string::npos)
    {
        size_t arr_end = response.find("]", by_artist_pos);
        std::string artists_section = response.substr(by_artist_pos, arr_end - by_artist_pos);

        // Find all artist names
        size_t search_pos = 0;
        bool first = true;
        while (true)
        {
            size_t name_pos = artists_section.find("\"name\":\"", search_pos);
            if (name_pos == std::string::npos) break;

            name_pos += 8;
            size_t name_end = artists_section.find("\"", name_pos);
            if (name_end == std::string::npos) break;

            std::string artist_name = artists_section.substr(name_pos, name_end - name_pos);
            if (first)
            {
                main_artist = artist_name;
                first = false;
            }
            else
            {
                feat_artists.push_back(artist_name);
            }
            search_pos = name_end;
        }
    }

    // Use main artist if found, otherwise use full_artist
    if (!main_artist.empty())
    {
        unified << ",\"artist\":\"" << escapeJson(main_artist) << "\"";
    }
    else
    {
        unified << ",\"artist\":\"" << escapeJson(full_artist) << "\"";
    }

    // Add feat_artists array
    unified << ",\"feat_artists\":[";
    for (size_t i = 0; i < feat_artists.size(); i++)
    {
        if (i > 0) unified << ",";
        unified << "\"" << escapeJson(feat_artists[i]) << "\"";
    }
    unified << "]";

    // === Sections Metadata (Album, Label, Year) ===
    std::string album, label, year;
    size_t sections_pos = response.find("\"sections\":", track_pos);
    if (sections_pos != std::string::npos)
    {
        size_t metadata_pos = response.find("\"metadata\":", sections_pos);
        if (metadata_pos != std::string::npos)
        {
            // Find Album
            size_t album_pos = response.find("\"Album\"", metadata_pos);
            if (album_pos != std::string::npos)
            {
                album = extractJsonString(response, "text", album_pos);
            }

            // Find Label
            size_t label_pos = response.find("\"Label\"", metadata_pos);
            if (label_pos != std::string::npos)
            {
                label = extractJsonString(response, "text", label_pos);
            }

            // Find Year (Sorti or Released)
            size_t year_pos = response.find("\"Sorti\"", metadata_pos);
            if (year_pos == std::string::npos)
            {
                year_pos = response.find("\"Released\"", metadata_pos);
            }
            if (year_pos != std::string::npos)
            {
                year = extractJsonString(response, "text", year_pos);
            }
        }
    }

    unified << ",\"album\":" << (album.empty() ? "null" : "\"" + escapeJson(album) + "\"");
    unified << ",\"label\":" << (label.empty() ? "null" : "\"" + escapeJson(label) + "\"");
    unified << ",\"year\":" << (year.empty() ? "null" : year);
    unified << ",\"genre\":" << (genre.empty() ? "null" : "\"" + escapeJson(genre) + "\"");
    unified << ",\"isrc\":" << (isrc.empty() ? "null" : "\"" + isrc + "\"");

    // === Apple Music enrichment (if present) ===
    std::string apple_date = extractJsonString(response, "apple_release_date");
    std::string apple_duration = extractJsonString(response, "apple_duration");

    if (!apple_date.empty())
    {
        unified << ",\"release_date\":\"" << apple_date << "\"";
    }
    if (!apple_duration.empty())
    {
        unified << ",\"duration\":\"" << apple_duration << "\"";
    }

    // Apple genres array
    size_t apple_genres_pos = response.find("\"apple_genres\":");
    if (apple_genres_pos != std::string::npos)
    {
        size_t arr_start = response.find("[", apple_genres_pos);
        size_t arr_end = response.find("]", arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos)
        {
            unified << ",\"genres\":" << response.substr(arr_start, arr_end - arr_start + 1);
        }
    }

    // === Images ===
    unified << ",\"images\":{";
    std::string coverart = extractJsonString(response, "coverart", track_pos);
    std::string coverarthq = extractJsonString(response, "coverarthq", track_pos);
    std::string background = extractJsonString(response, "background", track_pos);

    // Get high-res image from Apple Music (1200x630)
    std::string large_image = extractJsonString(response, "apple_image");

    unified << "\"coverart\":" << (coverart.empty() ? "null" : "\"" + coverart + "\"");
    unified << ",\"coverart_hq\":" << (coverarthq.empty() ? "null" : "\"" + coverarthq + "\"");
    unified << ",\"background\":" << (background.empty() ? "null" : "\"" + background + "\"");
    unified << ",\"large\":" << (large_image.empty() ? "null" : "\"" + large_image + "\"");
    unified << "}";

    // === External IDs ===
    unified << ",\"external_ids\":{";
    unified << "\"shazam\":\"" << key << "\"";

    // Shazam artist ID
    size_t artists_pos = response.find("\"artists\":", track_pos);
    std::string shazam_artist_id;
    std::string adamid;
    if (artists_pos != std::string::npos)
    {
        shazam_artist_id = extractJsonString(response, "id", artists_pos);
        adamid = extractJsonString(response, "adamid", artists_pos);
    }
    unified << ",\"shazam_artist\":" << (shazam_artist_id.empty() ? "null" : "\"" + shazam_artist_id + "\"");

    // Apple Music track ID
    std::string apple_id = extractAppleMusicId(response);
    unified << ",\"apple_music\":" << (apple_id.empty() ? "null" : "\"" + apple_id + "\"");
    unified << ",\"apple_music_album\":" << (albumadamid.empty() ? "null" : "\"" + albumadamid + "\"");
    unified << ",\"apple_music_artist\":" << (adamid.empty() ? "null" : "\"" + adamid + "\"");

    unified << "}";

    // === Links ===
    unified << ",\"links\":{";

    // Shazam URL
    std::string shazam_url = extractJsonString(response, "url", track_pos);
    unified << "\"shazam\":" << (shazam_url.empty() ? "null" : "\"" + shazam_url + "\"");

    // Apple Music URL
    std::string apple_music_url;
    if (!apple_id.empty())
    {
        apple_music_url = "https://music.apple.com/song/" + apple_id;
    }
    unified << ",\"apple_music\":" << (apple_music_url.empty() ? "null" : "\"" + apple_music_url + "\"");

    // Preview URL (from hub actions)
    size_t hub_pos = response.find("\"hub\":", track_pos);
    std::string preview_url;
    if (hub_pos != std::string::npos)
    {
        // Find the uri action with audio preview
        size_t uri_pos = response.find("\"type\":\"uri\"", hub_pos);
        if (uri_pos != std::string::npos)
        {
            preview_url = extractJsonString(response, "uri", uri_pos);
        }
    }
    unified << ",\"preview\":" << (preview_url.empty() ? "null" : "\"" + preview_url + "\"");

    // Spotify - look for spotify: URI
    std::string spotify_uri;
    size_t spotify_pos = response.find("spotify:search:", track_pos);
    if (spotify_pos != std::string::npos)
    {
        // Find the start of this URI value
        size_t uri_start = response.rfind("\"", spotify_pos);
        if (uri_start != std::string::npos)
        {
            uri_start++;
            size_t uri_end = response.find("\"", spotify_pos);
            if (uri_end != std::string::npos)
            {
                spotify_uri = response.substr(uri_start, uri_end - uri_start);
            }
        }
    }
    unified << ",\"spotify\":" << (spotify_uri.empty() ? "null" : "\"" + spotify_uri + "\"");

    // YouTube Music - look for music.youtube.com
    std::string youtube_uri;
    size_t youtube_pos = response.find("music.youtube.com", track_pos);
    if (youtube_pos != std::string::npos)
    {
        size_t uri_start = response.rfind("\"", youtube_pos);
        if (uri_start != std::string::npos)
        {
            uri_start++;
            size_t uri_end = response.find("\"", youtube_pos);
            if (uri_end != std::string::npos)
            {
                youtube_uri = response.substr(uri_start, uri_end - uri_start);
            }
        }
    }
    unified << ",\"youtube_music\":" << (youtube_uri.empty() ? "null" : "\"" + youtube_uri + "\"");

    // Deezer - look for deezer-query://
    std::string deezer_uri;
    size_t deezer_pos = response.find("deezer-query://", track_pos);
    if (deezer_pos != std::string::npos)
    {
        size_t uri_start = response.rfind("\"", deezer_pos);
        if (uri_start != std::string::npos)
        {
            uri_start++;
            size_t uri_end = response.find("\"", deezer_pos);
            if (uri_end != std::string::npos)
            {
                deezer_uri = response.substr(uri_start, uri_end - uri_start);
            }
        }
    }
    unified << ",\"deezer\":" << (deezer_uri.empty() ? "null" : "\"" + deezer_uri + "\"");

    unified << "}";

    // === Match Quality ===
    unified << ",\"match\":{";
    size_t matches_pos = response.find("\"matches\":");
    if (matches_pos != std::string::npos)
    {
        std::string offset = extractJsonNumber(response, "offset", matches_pos);
        std::string timeskew = extractJsonNumber(response, "timeskew", matches_pos);
        std::string frequencyskew = extractJsonNumber(response, "frequencyskew", matches_pos);

        unified << "\"offset\":" << (offset.empty() ? "0" : offset);
        unified << ",\"timeskew\":" << (timeskew.empty() ? "0" : timeskew);
        unified << ",\"frequencyskew\":" << (frequencyskew.empty() ? "0" : frequencyskew);
    }
    unified << "}";

    // === Related Tracks URL ===
    std::string related_url = extractJsonString(response, "relatedtracksurl", track_pos);
    unified << ",\"related_tracks_url\":" << (related_url.empty() ? "null" : "\"" + related_url + "\"");

    // === Request Metadata ===
    unified << ",\"request\":{";
    std::string timestamp = extractJsonNumber(response, "timestamp");
    std::string timezone = extractJsonString(response, "timezone");

    unified << "\"timestamp\":" << (timestamp.empty() ? "null" : timestamp);
    unified << ",\"timezone\":" << (timezone.empty() ? "null" : "\"" + timezone + "\"");

    // Location
    size_t location_pos = response.find("\"location\":");
    if (location_pos != std::string::npos)
    {
        std::string lat = extractJsonNumber(response, "latitude", location_pos);
        std::string lon = extractJsonNumber(response, "longitude", location_pos);
        std::string alt = extractJsonNumber(response, "altitude", location_pos);

        unified << ",\"location\":{";
        unified << "\"latitude\":" << (lat.empty() ? "null" : lat);
        unified << ",\"longitude\":" << (lon.empty() ? "null" : lon);
        unified << ",\"altitude\":" << (alt.empty() ? "null" : alt);
        unified << "}";
    }
    unified << "}";

    // === Vibra Info ===
    std::string segments = extractJsonNumber(response, "vibra_segments_checked");
    std::string offset_ms = extractJsonNumber(response, "vibra_offset_ms");
    bool confident = response.find("\"vibra_confident\":true") != std::string::npos;

    if (!segments.empty() || !offset_ms.empty())
    {
        unified << ",\"vibra\":{";
        bool first = true;
        if (!segments.empty())
        {
            unified << "\"segments_checked\":" << segments;
            first = false;
        }
        if (!offset_ms.empty())
        {
            if (!first) unified << ",";
            unified << "\"offset_ms\":" << offset_ms;
            first = false;
        }
        if (!segments.empty())
        {
            unified << ",\"confident\":" << (confident ? "true" : "false");
        }
        unified << "}";
    }

    unified << "}}";

    return unified.str();
}

bool Shazam::RequestNewTorCircuit(const std::string& password)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return false;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(9051);  // Tor control port
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return false;
    }

    // Send AUTHENTICATE command
    std::string auth_cmd;
    if (password.empty())
    {
        auth_cmd = "AUTHENTICATE\r\n";
    }
    else
    {
        auth_cmd = "AUTHENTICATE \"" + password + "\"\r\n";
    }
    send(sock, auth_cmd.c_str(), auth_cmd.length(), 0);

    // Read response
    char buffer[256];
    int bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0 || strncmp(buffer, "250", 3) != 0)
    {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return false;
    }

    // Send SIGNAL NEWNYM to get new circuit
    std::string newnym_cmd = "SIGNAL NEWNYM\r\n";
    send(sock, newnym_cmd.c_str(), newnym_cmd.length(), 0);

    // Read response
    bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0);
    bool success = (bytes_read > 0 && strncmp(buffer, "250", 3) == 0);

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    return success;
}
