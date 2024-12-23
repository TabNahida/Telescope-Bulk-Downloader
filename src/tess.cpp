#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Task structure
struct Task
{
    std::string url;
    std::string output_file;
};

// Thread pool task queue
std::queue<Task> task_queue;
std::mutex queue_mutex;
std::condition_variable cv;
bool finished = false;

// Statistics
std::atomic<int> total_files(0);
std::atomic<int> downloaded_files(0);

void show_progress_bar(int total, int downloaded)
{
    const int bar_width = 50;
    float progress = static_cast<float>(downloaded) / total;
    int pos = static_cast<int>(progress * bar_width);

    std::cout << "[";
    for (int i = 0; i < bar_width; ++i)
    {
        if (i < pos)
            std::cout << "=";
        else if (i == pos)
            std::cout << ">";
        else
            std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << "% (" << downloaded << "/" << total << ")\r";
    std::cout.flush();
}

void worker()
{
    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !task_queue.empty() || finished; });
            if (finished && task_queue.empty())
                break;
            task = task_queue.front();
            task_queue.pop();
        }

        const std::string &url = task.url;
        const std::string &output_file = task.output_file;

        if (!fs::exists(output_file))
        {
            std::string command = "curl -C - -L -o \"" + output_file + "\" \"" + url + "\" > /dev/null 2>&1";
            int ret = system(command.c_str());
            if (ret == 0)
            {
                downloaded_files++;
            }
            else
            {
                std::cerr << "Download failed: " << url << std::endl;
            }
        }
        else
        {
            downloaded_files++;
        }

        show_progress_bar(total_files, downloaded_files);
    }
}

void print_help()
{
    std::cout << "Usage: tess_downloader [OPTIONS]\n"
              << "Options:\n"
              << "  -h, --help          Show this help message and exit\n"
              << "  --start=SECTOR      Specify the start sector (default: 1)\n"
              << "  --end=SECTOR        Specify the end sector (default: 83)\n"
              << "  --threads=NUM       Specify the number of threads to use "
                 "(default: number of hardware threads)\n"
              << "  --output-dir=DIR    Specify the output directory for "
                 "downloaded files (default: current directory)\n"
              << std::endl;
}

void download_and_parse_tasks(std::vector<Task> &tasks, int start_sector, int end_sector, const std::string &output_dir,
                              int thread_count)
{
    std::atomic<int> sector_index(start_sector);
    std::mutex tasks_mutex;
    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]() {
            int sector;
            while ((sector = sector_index++) <= end_sector)
            {
                std::string sh_url = "https://archive.stsci.edu/missions/tess/"
                                     "download_scripts/sector/tesscurl_sector_" +
                                     std::to_string(sector) + "_lc.sh";
                std::string sh_file = output_dir + "/tesscurl_sector_" + std::to_string(sector) + "_lc.sh";

                if (!fs::exists(sh_file))
                {
                    std::cout << "Downloading SH file: " << sh_file << std::endl;
                    std::string command = "curl -L -o " + sh_file + " \"" + sh_url + "\"";
                    int ret = system(command.c_str());
                    if (ret != 0)
                    {
                        std::cerr << "Failed to download SH file: " << sh_url << std::endl;
                        continue;
                    }
                }

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
                        Task task = {match[2].str(), sector_folder + "/" + match[1].str()};
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

void check_existing_files(std::vector<Task> &tasks, int thread_count)
{
    std::atomic<int> task_index(0);
    std::mutex tasks_mutex;
    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]() {
            while (true)
            {
                int index = task_index++;
                {
                    std::lock_guard<std::mutex> lock(tasks_mutex);
                    if (index >= tasks.size())
                        break;
                    if (fs::exists(tasks[index].output_file))
                    {
                        downloaded_files++;
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

int main(int argc, char *argv[])
{
    int start_sector = 1;
    int end_sector = 83;
    int thread_count = std::thread::hardware_concurrency();
    std::string output_dir = "./";

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
    }

    // Parse sectors and initialize task queue
    std::vector<Task> tasks;
    download_and_parse_tasks(tasks, start_sector, end_sector, output_dir, thread_count);

    total_files = tasks.size();

    check_existing_files(tasks, thread_count);

    std::cout << "Total files to download: " << total_files << std::endl;
    std::cout << "Already existing files: " << downloaded_files << std::endl;

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

    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back(worker);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        finished = true;
    }
    cv.notify_all();

    for (auto &t : threads)
    {
        t.join();
    }

    std::cout << "\nAll download tasks completed!" << std::endl;
    return 0;
}
