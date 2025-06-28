#include "logger.h"

#include <fstream>
#include <iostream>
#include <mutex>

static std::ofstream g_file;
static std::mutex g_file_lock;
static std::ostream* g_out_ptr = &std::cout;

void logger_init()
{
  std::lock_guard lock(g_file_lock);
  const char* path = std::getenv("OUT_FILE");
  if (path)
  {
    // Close if already open, to handle re-init scenarios
    if (g_file.is_open())
    {
      g_file.close();
    }
    g_file.open(path, std::ios::out | std::ios::app);
    if (g_file.is_open())
    {
      g_out_ptr = &g_file;
    }
    else
    {
      g_out_ptr = &std::cerr; // Log to stderr if file opening fails
      std::cerr << "IsoFuzz WARNING: Could not open OUT_FILE=" << path << ". Logging to stderr." << std::endl;
    }
  }
  else
  {
    g_out_ptr = &std::cout;
  }
}

void logger_shutdown()
{
  std::lock_guard lock(g_file_lock);
  if (g_file.is_open())
  {
    g_file.flush();
    g_file.close();
  }
  g_out_ptr = &std::cout;
}

void logger_log_line(const std::string& line)
{
  std::lock_guard lock(g_file_lock);
  *g_out_ptr << line << std::endl;
}
