#ifndef CLI_BULK_PROCESSOR_H_
#define CLI_BULK_PROCESSOR_H_

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <csignal>

struct BulkResult {
    std::string file_path;
    std::string json_response;
    bool success;
    std::string error_message;
    std::string ip_address;
};

struct ProxyConfig {
    std::string host;
    int port = 0;
    std::string username;
    std::string password;
    std::string type;  // "http", "socks5", etc.
    std::string rotation_url;  // URL to fetch new proxy from
};

struct BulkStats {
    std::atomic<int> total_files{0};
    std::atomic<int> processed{0};
    std::atomic<int> successful{0};
    std::atomic<int> failed{0};
    std::atomic<int> skipped{0};
};

class BulkProcessor {
public:
    BulkProcessor(const std::string& directory_path, const std::string& output_json_path,
                  int num_threads = 1, bool resume = false, int delay_seconds = 2);

    // Main processing function
    void Process();

    // Signal handling
    static void SignalHandler(int signal);
    static BulkProcessor* current_instance_;

    // Configuration
    void SetThreadCount(int threads) { num_threads_ = threads; }
    void EnableResume(bool enable) { resume_enabled_ = enable; }
    void SetSupportedFormats(const std::vector<std::string>& formats);
    void SetProxyConfig(const ProxyConfig& config);

    // Get statistics
    const BulkStats& GetStats() const { return stats_; }

private:
    // File discovery
    std::vector<std::string> ScanDirectory();
    bool IsSupportedFormat(const std::string& file_path);

    // Cache management
    void LoadCache();
    void SaveCache();
    bool IsAlreadyProcessed(const std::string& file_path);
    void AddToCache(const BulkResult& result);

    // Proxy management
    std::string GetCurrentProxy();
    void RotateProxy(int timeout_seconds = 60);
    std::string FetchProxyFromURL(const std::string& url);

    // IP detection
    std::string FetchCurrentIP();

    // Proxy testing
    bool TestProxy(const std::string& proxy, int timeout_seconds = 10);

    // Processing
    void ProcessFile(const std::string& file_path);
    void WorkerThread();

    // Progress display
    void DisplayProgress();
    void PrintFinalReport();
    void AutoSaveThread();

    // Member variables
    std::string directory_path_;
    std::string output_json_path_;
    int num_threads_;
    bool resume_enabled_;
    int delay_seconds_;

    std::vector<std::string> supported_formats_;
    std::vector<std::string> files_to_process_;
    std::map<std::string, BulkResult> results_cache_;

    ProxyConfig proxy_config_;
    std::string current_proxy_;
    std::mutex proxy_mutex_;
    int proxy_rotation_timeout_;
    std::chrono::steady_clock::time_point last_rotation_time_;
    std::atomic<bool> rotation_in_progress_{false};

    BulkStats stats_;
    std::mutex cache_mutex_;
    std::mutex queue_mutex_;
    std::mutex console_mutex_;
    std::mutex recognition_mutex_;  // Serialize recognition calls for thread safety

    // Rate limiting state
    std::atomic<bool> rate_limited_{false};
    std::atomic<int> rate_limit_retry_count_{0};
    std::chrono::steady_clock::time_point rate_limit_until_;
    std::mutex rate_limit_mutex_;

    size_t next_file_index_;
    std::atomic<bool> processing_complete_{false};
};

#endif // CLI_BULK_PROCESSOR_H_
