// File utilities implementation
#include "file_utils.hpp"
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <ctime>
#include <algorithm>

namespace fs = std::filesystem;

namespace FileUtils
{
    // ==================== Date and Time Utilities ====================

    std::string timeToString(time_t timestamp)
    {
        struct tm* timeinfo = localtime(&timestamp);
        char buffer[80];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
        return std::string(buffer);
    }

    time_t stringToTime(const std::string& datestr)
    {
        struct tm tm_info = {};
        strptime(datestr.c_str(), "%Y-%m-%d %H:%M:%S", &tm_info);
        return mktime(&tm_info);
    }

    std::string getCurrentDateString()
    {
        time_t now = std::time(nullptr);
        return timeToString(now);
    }

    time_t getCurrentTime()
    {
        return std::time(nullptr);
    }

    // ==================== Formatting Utilities ====================

    std::string formatCurrency(double amount)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << amount;
        std::string result = oss.str();

        // Handle negative signs
        bool is_negative = false;
        if (result[0] == '-')
        {
            is_negative = true;
            result = result.substr(1);
        }

        // Insert commas for thousands
        size_t decimal_pos = result.find('.');
        if (decimal_pos == std::string::npos)
            decimal_pos = result.length();

        int insert_pos = (int)decimal_pos - 3;
        while (insert_pos > 0)
        {
            result.insert(insert_pos, ",");
            insert_pos -= 3;
        }

        // Add sign back
        if (is_negative)
            result = "-" + result;

        return "$" + result;
    }

    std::string formatPercentage(double percentage)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << percentage << "%";
        return oss.str();
    }

    // ==================== Path Utilities ====================

    bool fileExists(const std::string& filepath)
    {
        return fs::exists(filepath) && fs::is_regular_file(filepath);
    }

    bool directoryExists(const std::string& dirpath)
    {
        return fs::exists(dirpath) && fs::is_directory(dirpath);
    }

    bool createDirectory(const std::string& dirpath)
    {
        try
        {
            if (!fs::exists(dirpath))
            {
                fs::create_directories(dirpath);
            }
            return true;
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Error creating directory: " << e.what() << std::endl;
            return false;
        }
    }

    std::string getFileName(const std::string& filepath)
    {
        return fs::path(filepath).filename().string();
    }

    std::string getDirectoryPath(const std::string& filepath)
    {
        return fs::path(filepath).parent_path().string();
    }

    // ==================== Directory Listing ====================

    std::vector<std::string> listFilesInDirectory(const std::string& dirpath)
    {
        std::vector<std::string> files;

        try
        {
            if (!fs::exists(dirpath) || !fs::is_directory(dirpath))
                return files;

            for (const auto& entry : fs::directory_iterator(dirpath))
            {
                if (entry.is_regular_file())
                {
                    files.push_back(entry.path().filename().string());
                }
            }

            std::sort(files.begin(), files.end());
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Error listing files: " << e.what() << std::endl;
        }

        return files;
    }

    std::vector<std::string> listDirectories(const std::string& dirpath)
    {
        std::vector<std::string> directories;

        try
        {
            if (!fs::exists(dirpath) || !fs::is_directory(dirpath))
                return directories;

            for (const auto& entry : fs::directory_iterator(dirpath))
            {
                if (entry.is_directory())
                {
                    directories.push_back(entry.path().filename().string());
                }
            }

            std::sort(directories.begin(), directories.end());
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Error listing directories: " << e.what() << std::endl;
        }

        return directories;
    }

    bool deleteFile(const std::string& filepath)
    {
        try
        {
            if (fs::exists(filepath))
            {
                fs::remove(filepath);
                return true;
            }
            return false;
        }
        catch (const fs::filesystem_error& e)
        {
            std::cerr << "Error deleting file: " << e.what() << std::endl;
            return false;
        }
    }
}

