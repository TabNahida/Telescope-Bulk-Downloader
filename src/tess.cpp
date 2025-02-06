#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Include httplib header file; make sure CPPHTTPLIB_OPENSSL_SUPPORT is defined for SSL support.
#include "httplib.h"

namespace fs = std::filesystem;

// -------------------- Wrapper Classes --------------------

// BaseClient is an abstract interface to hide the difference between HTTP and HTTPS clients.
class BaseClient
{
  public:
    virtual ~BaseClient()
    {
    }
    // Simple Get without callbacks.
    virtual httplib::Result Get(const char *path, const httplib::Headers &headers) = 0;
    // Get with callbacks (for streaming downloads).
    virtual httplib::Result Get(const char *path, const httplib::Headers &headers,
                                httplib::ContentReceiver content_receiver, httplib::Progress progress) = 0;
    virtual void set_proxy(const char *host, int port) = 0;
    virtual void set_connection_timeout(int sec, int microsec) = 0;
    virtual void set_read_timeout(int sec, int microsec) = 0;
};

// HttpClientWrapper wraps httplib::Client.
class HttpClientWrapper : public BaseClient
{
  public:
    httplib::Client client;
    HttpClientWrapper(const char *host, int port) : client(host, port)
    {
    }
    httplib::Result Get(const char *path, const httplib::Headers &headers) override
    {
        return client.Get(path, headers);
    }
    httplib::Result Get(const char *path, const httplib::Headers &headers, httplib::ContentReceiver content_receiver,
                        httplib::Progress progress) override
    {
        return client.Get(path, headers, content_receiver, progress);
    }
    void set_proxy(const char *host, int port) override
    {
        client.set_proxy(host, port);
    }
    void set_connection_timeout(int sec, int microsec) override
    {
        client.set_connection_timeout(sec, microsec);
    }
    void set_read_timeout(int sec, int microsec) override
    {
        client.set_read_timeout(sec, microsec);
    }
};

// HttpsClientWrapper wraps httplib::SSLClient.
class HttpsClientWrapper : public BaseClient
{
  public:
    httplib::SSLClient client;
    HttpsClientWrapper(const char *host, int port) : client(host, port)
    {
    }
    httplib::Result Get(const char *path, const httplib::Headers &headers) override
    {
        return client.Get(path, headers);
    }
    httplib::Result Get(const char *path, const httplib::Headers &headers, httplib::ContentReceiver content_receiver,
                        httplib::Progress progress) override
    {
        return client.Get(path, headers, content_receiver, progress);
    }
    void set_proxy(const char *host, int port) override
    {
        client.set_proxy(host, port);
    }
    void set_connection_timeout(int sec, int microsec) override
    {
        client.set_connection_timeout(sec, microsec);
    }
    void set_read_timeout(int sec, int microsec) override
    {
        client.set_read_timeout(sec, microsec);
    }
};

// Forward declaration of URL.
struct URL;

// Factory function to create a BaseClient pointer based on URL scheme.
std::unique_ptr<BaseClient> create_client(const URL &url_data);

// -------------------- Structures and Global Variables --------------------

// Task structure: one download task contains the source URL and target file.
struct Task
{
    std::string url;
    std::string output_file;
};

// URL structure for parsing.
struct URL
{
    std::string scheme;
    std::string host;
    int port;
    std::string path;
};

// Global task queue and synchronization primitives.
std::queue<Task> task_queue;
std::mutex queue_mutex;
std::condition_variable cv;
bool tasks_finished = false;

// Statistics: number of files and bytes downloaded.
std::atomic<int> total_files(0);
std::atomic<int> downloaded_files(0);
std::atomic<int> sh_files_downloaded(0);
std::atomic<size_t> global_total_bytes_downloaded(0); // in bytes

// For controlling the refresh display thread.
std::atomic<bool> download_done(false);

// Mutex for console output.
std::mutex output_mutex;

// Proxy configuration (if empty, not used).
std::string global_proxy_host = "";
int global_proxy_port = 0;

// Global format selection, default "lc"; supported formats.
std::string global_format = "lc";

// -------------------- Helper Functions --------------------

// Validate if the given format is supported.
bool is_supported_format(const std::string &format)
{
    static const std::vector<std::string> supported_formats = {"ffic", "ffir", "tp", "fast-tp", "lc", "fast-lc", "dv"};
    for (const auto &s : supported_formats)
    {
        if (s == format)
            return true;
    }
    return false;
}

// Simple URL parser: supports URLs like "http://host[:port]/path" or "https://host[:port]/path".
bool parse_url(const std::string &url, URL &result)
{
    std::regex re(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)$)");
    std::smatch match;
    if (std::regex_match(url, match, re))
    {
        result.scheme = match[1];
        result.host = match[2];
        result.port = match[3].matched ? std::stoi(match[3]) : (result.scheme == "https" ? 443 : 80);
        result.path = match[4];
        return true;
    }
    return false;
}

// Implementation of create_client() using our wrapper classes.
std::unique_ptr<BaseClient> create_client(const URL &url_data)
{
    if (url_data.scheme == "https")
    {
        return std::unique_ptr<BaseClient>(new HttpsClientWrapper(url_data.host.c_str(), url_data.port));
    }
    else
    {
        return std::unique_ptr<BaseClient>(new HttpClientWrapper(url_data.host.c_str(), url_data.port));
    }
}

// Print help message (CLI in English).
void print_help()
{
    std::cout
        << "Usage: tess_downloader [OPTIONS]\n"
           "Options:\n"
           "  -h, --help           Show this help message and exit\n"
           "  --start=SECTOR       Specify the start sector (default: 1)\n"
           "  --end=SECTOR         Specify the end sector (default: 83)\n"
           "  --threads=NUM        Specify the number of threads to use (default: hardware concurrency)\n"
           "  --output-dir=DIR     Specify the output directory for downloaded files (default: current directory)\n"
           "  --proxy=HOST:PORT    Specify proxy server (e.g., 127.0.0.1:8080)\n"
           "  --format=TYPE        Specify data format: ffic, ffir, tp, fast-tp, lc, fast-lc, dv (default: lc)\n"
        << std::endl;
}

// -------------------- Download Functions --------------------

// Download shell script files for each sector using our BaseClient.
// Shell script URL format:
// "https://archive.stsci.edu/missions/tess/download_scripts/sector/tesscurl_sector_<sector>_<format>.sh"
void download_sh_files(int start_sector, int end_sector, const std::string &output_dir, int thread_count)
{
    std::atomic<int> sector_index(start_sector);
    std::vector<std::thread> threads;
    int total = end_sector - start_sector + 1;

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]() {
            while (true)
            {
                int sector = sector_index.fetch_add(1);
                if (sector > end_sector)
                    break;
                // Construct the shell script URL with the chosen format.
                std::string sh_url =
                    "https://archive.stsci.edu/missions/tess/download_scripts/sector/tesscurl_sector_" +
                    std::to_string(sector) + "_" + global_format + ".sh";
                std::string sh_file =
                    output_dir + "/tesscurl_sector_" + std::to_string(sector) + "_" + global_format + ".sh";

                if (!fs::exists(sh_file))
                {
                    URL url_data;
                    if (!parse_url(sh_url, url_data))
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        std::cerr << "Invalid URL: " << sh_url << std::endl;
                        continue;
                    }
                    auto client = create_client(url_data);
                    if (!global_proxy_host.empty())
                    {
                        client->set_proxy(global_proxy_host.c_str(), global_proxy_port);
                    }
                    client->set_connection_timeout(5, 0);
                    client->set_read_timeout(30, 0);
                    // Use the simple Get overload (without callbacks) for shell script download.
                    auto res = client->Get(url_data.path.c_str(), httplib::Headers());
                    if (res && res->status == 200)
                    {
                        std::ofstream ofs(sh_file);
                        ofs << res->body;
                        ofs.close();
                    }
                    else
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        std::cerr << "Failed to download shell script for sector " << sector << std::endl;
                    }
                }
                sh_files_downloaded++;
            }
        });
    }
    for (auto &t : threads)
    {
        t.join();
    }
}

// Parse shell script files to initialize download tasks.
// The script file contains lines like "curl ... -o <filename> <http...>".
void download_and_parse_tasks(std::vector<Task> &tasks, int start_sector, int end_sector, const std::string &output_dir,
                              int thread_count)
{
    std::atomic<int> sector_index(start_sector);
    std::mutex tasks_mutex;
    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]() {
            while (true)
            {
                int sector = sector_index.fetch_add(1);
                if (sector > end_sector)
                    break;
                std::string sh_file =
                    output_dir + "/tesscurl_sector_" + std::to_string(sector) + "_" + global_format + ".sh";
                std::string sector_folder = output_dir + "/T" + std::to_string(sector);
                if (!fs::exists(sector_folder))
                {
                    fs::create_directory(sector_folder);
                }
                std::ifstream infile(sh_file);
                std::string line;
                std::regex regex_pattern(R"(curl.*-o\s+(\S+)\s+(http\S+))");
                while (std::getline(infile, line))
                {
                    std::smatch match;
                    if (std::regex_search(line, match, regex_pattern))
                    {
                        Task task;
                        task.url = match[2].str();
                        task.output_file = sector_folder + "/" + match[1].str();
                        {
                            std::lock_guard<std::mutex> lock(tasks_mutex);
                            tasks.push_back(task);
                        }
                    }
                }
            }
        });
    }
    for (auto &t : threads)
    {
        t.join();
    }
}

// Check for already existing files to skip already downloaded tasks.
void check_existing_files(std::vector<Task> &tasks, int thread_count)
{
    std::atomic<int> task_index(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]() {
            while (true)
            {
                int index = task_index.fetch_add(1);
                if (index >= tasks.size())
                    break;
                if (fs::exists(tasks[index].output_file))
                {
                    downloaded_files++;
                }
            }
        });
    }
    for (auto &t : threads)
    {
        t.join();
    }
}

// -------------------- Refresh Display Thread --------------------
// refresh_display() periodically updates the download speed and progress display on a single line.
// It uses a carriage return ("\r") to overwrite the same line.
void refresh_display()
{
    size_t previous_total = global_total_bytes_downloaded.load();
    while (!download_done.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        size_t current_total = global_total_bytes_downloaded.load();
        size_t diff = current_total - previous_total;
        // Calculate speed in KB/s (scale 500ms to 1s)
        double overall_speed_kbps = diff / 1024.0 * (1000.0 / 500.0);
        previous_total = current_total;
        int downloaded = downloaded_files.load();
        float progress = total_files.load() ? static_cast<float>(downloaded) / total_files.load() : 0.0f;
        const int bar_width = 50;
        int pos = static_cast<int>(progress * bar_width);

        std::ostringstream oss;
        oss << "Download speed: " << overall_speed_kbps << " KB/s, ";
        oss << "Progress: [";
        for (int i = 0; i < bar_width; ++i)
        {
            if (i < pos)
                oss << "=";
            else if (i == pos)
                oss << ">";
            else
                oss << " ";
        }
        oss << "] " << int(progress * 100) << "% (" << downloaded << "/" << total_files.load() << ")";
        std::string output_line = oss.str();

        {
            std::lock_guard<std::mutex> lock(output_mutex);
            // Use carriage return to rewrite the same line.
            std::cout << "\r" << output_line << std::flush;
        }
    }
    {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << std::endl << "All download tasks completed!" << std::endl;
    }
}

// -------------------- Worker Thread Function --------------------
// Each worker fetches tasks from the queue, downloads the file using the BaseClient with callbacks, and updates
// statistics.
void worker()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !task_queue.empty() || tasks_finished; });
            if (tasks_finished && task_queue.empty())
                break;
            task = task_queue.front();
            task_queue.pop();
        }
        URL url_data;
        if (!parse_url(task.url, url_data))
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "Invalid URL: " << task.url << std::endl;
            continue;
        }
        auto client = create_client(url_data);
        if (!global_proxy_host.empty())
        {
            client->set_proxy(global_proxy_host.c_str(), global_proxy_port);
        }
        client->set_connection_timeout(5, 0);
        client->set_read_timeout(30, 0);

        std::ofstream ofs(task.output_file, std::ios::binary);
        if (!ofs)
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "Failed to open file: " << task.output_file << std::endl;
            continue;
        }

        size_t file_downloaded = 0;
        auto content_receiver = [&](const char *data, size_t data_length) -> bool {
            ofs.write(data, data_length);
            file_downloaded += data_length;
            global_total_bytes_downloaded += data_length;
            return true;
        };
        auto progress_callback = [&](uint64_t current, uint64_t total) -> bool { return true; };

        httplib::Headers headers;
        auto res = client->Get(url_data.path.c_str(), headers, content_receiver, progress_callback);
        if (res && res->status == 200)
        {
            downloaded_files++;
        }
        else
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cerr << "\nFailed to download: " << task.url << std::endl;
        }
        ofs.close();
    }
}

// -------------------- Main Function --------------------
int main(int argc, char *argv[])
{
    // Default parameters.
    int start_sector = 1;
    int end_sector = 83;
    int thread_count = std::thread::hardware_concurrency();
    std::string output_dir = "./";

    // Parse command line arguments.
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            print_help();
            return 0;
        }
        else if (arg.find("--start=") == 0)
        {
            start_sector = std::stoi(arg.substr(8));
        }
        else if (arg.find("--end=") == 0)
        {
            end_sector = std::stoi(arg.substr(6));
        }
        else if (arg.find("--threads=") == 0)
        {
            thread_count = std::stoi(arg.substr(10));
        }
        else if (arg.find("--output-dir=") == 0)
        {
            output_dir = arg.substr(13);
            if (!fs::exists(output_dir))
            {
                std::cerr << "Output directory does not exist: " << output_dir << std::endl;
                return 1;
            }
        }
        else if (arg.find("--proxy=") == 0)
        {
            std::string proxy = arg.substr(8);
            size_t pos = proxy.find(":");
            if (pos != std::string::npos)
            {
                global_proxy_host = proxy.substr(0, pos);
                global_proxy_port = std::stoi(proxy.substr(pos + 1));
            }
            else
            {
                std::cerr << "Invalid proxy format. Use HOST:PORT" << std::endl;
                return 1;
            }
        }
        else if (arg.find("--format=") == 0)
        {
            global_format = arg.substr(9);
            if (!is_supported_format(global_format))
            {
                std::cerr << "Unsupported format: " << global_format << std::endl;
                std::cerr << "Supported formats are: ffic, ffir, tp, fast-tp, lc, fast-lc, dv" << std::endl;
                return 1;
            }
        }
    }
    if (!is_supported_format(global_format))
    {
        std::cerr << "Unsupported format: " << global_format << std::endl;
        std::cerr << "Supported formats are: ffic, ffir, tp, fast-tp, lc, fast-lc, dv" << std::endl;
        return 1;
    }

    // 1. Download shell script files for each sector.
    std::cout << "Downloading shell scripts..." << std::endl;
    download_sh_files(start_sector, end_sector, output_dir, thread_count);

    // 2. Parse shell scripts to generate download tasks.
    std::vector<Task> tasks;
    download_and_parse_tasks(tasks, start_sector, end_sector, output_dir, thread_count);
    total_files = tasks.size();

    // 3. Check for existing files to skip already downloaded tasks.
    check_existing_files(tasks, thread_count);

    std::cout << "\nTotal files to download: " << total_files << std::endl;
    std::cout << "Already existing files: " << downloaded_files.load() << std::endl;
    std::cout << "Proceed with download? (y/n): ";
    char choice;
    std::cin >> choice;
    if (choice != 'y' && choice != 'Y')
    {
        std::cout << "Download cancelled." << std::endl;
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        for (const auto &task : tasks)
        {
            if (!fs::exists(task.output_file))
            {
                task_queue.push(task);
            }
        }
    }

    // Start the refresh display thread.
    std::thread refresh_thread(refresh_display);

    // Start worker threads.
    std::vector<std::thread> workers;
    for (int i = 0; i < thread_count; ++i)
    {
        workers.emplace_back(worker);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        tasks_finished = true;
    }
    cv.notify_all();
    for (auto &t : workers)
    {
        t.join();
    }

    download_done = true;
    refresh_thread.join();

    return 0;
}
